// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/producer.h>

#include <addresstype.h>
#include <chain.h>
#include <common/args.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <kernel/chainparams.h>
#include <key_io.h>
#include <logging.h>
#include <node/context.h>
#include <node/miner.h>
#include <pos/hooks.h>
#include <pubkey.h>
#include <random.h>
#include <tinyformat.h>
#include <txmempool.h>
#include <util/fs.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/thread.h>
#include <util/time.h>
#include <validation.h>

#include <fstream>

namespace pos {

namespace {
uint256 XOnly(const CKey& key)
{
    const XOnlyPubKey xonly{key.GetPubKey()};
    return uint256{std::span<const unsigned char>{xonly.begin(), xonly.end()}};
}
} // namespace

int64_t Producer::ProducerShare(int64_t fees, int64_t bps)
{
    // fees * bps would overflow for large fees: by parts.
    return fees / 10000 * bps + fees % 10000 * bps / 10000;
}

bool Producer::Behind(ChainstateManager& chainman)
{
    AssertLockHeld(::cs_main);
    // (Not initial block download as such: a chain stalled for a day, restarted, would never start again.)
    const Chainstate& chainstate{chainman.ActiveChainstate()};
    const CBlockIndex* tip{chainstate.m_chain.Tip()};
    if (!tip) return true;
    // A block of more work that the node has, with all below it (a candidate for the tip): it is
    // connecting it. Not merely a header of more work.
    for (auto it{chainstate.setBlockIndexCandidates.rbegin()}; it != chainstate.setBlockIndexCandidates.rend(); ++it) {
        const CBlockIndex* candidate{*it};
        if (candidate->nChainWork <= tip->nChainWork) break;
        if (!(candidate->nStatus & BLOCK_FAILED_VALID)) return true;
    }
    return false;
}

std::unique_ptr<Producer> Producer::FromArgs(node::NodeContext& node, const FastBlocksHooks& hooks, std::string& error)
{
    const ArgsManager& args{*node.args};
    if (!args.IsArgSet("-validatorkeyfile")) return nullptr;
    const fs::path key_path{args.GetPathArg("-validatorkeyfile")};
    std::ifstream file{key_path.std_path()};
    if (!file.is_open()) {
        error = strprintf("cannot read the validator key file %s", fs::PathToString(key_path));
        return nullptr;
    }
    std::vector<CKey> keys;
    std::string line;
    while (std::getline(file, line)) {
        line = util::TrimString(line);
        if (line.empty() || line[0] == '#') continue;
        const auto secret{TryParseHex<uint8_t>(line)};
        CKey key;
        if (secret && secret->size() == 32) key.Set(secret->begin(), secret->end(), /*fCompressedIn=*/true);
        if (!key.IsValid()) {
            error = strprintf("the validator key file %s has a line that is not a 32-byte secret key in hex", fs::PathToString(key_path));
            return nullptr;
        }
        keys.push_back(key);
    }
    if (keys.empty()) {
        error = strprintf("the validator key file %s has no key", fs::PathToString(key_path));
        return nullptr;
    }
    for (const CKey& key : keys) {
        if (!hooks.Validators().Find(XOnly(key))) {
            LogWarning("FastBlocks: validator key %s is not in the validator set: it makes no blocks", HexStr(XOnly(key)));
        }
    }
    // The producer's share of fees goes to -validatorpayout, or else to the first key (P2WPKH).
    CScript payout{GetScriptForDestination(WitnessV0KeyHash{keys[0].GetPubKey()})};
    if (args.IsArgSet("-validatorpayout")) {
        const CTxDestination dest{DecodeDestination(args.GetArg("-validatorpayout", ""))};
        if (!IsValidDestination(dest)) {
            error = "-validatorpayout is not an address of this chain";
            return nullptr;
        }
        payout = GetScriptForDestination(dest);
    }
    auto protection{SlashingProtection::Open(args.GetDataDirNet() / "validator_protection.dat", key_path.parent_path(),
                                             fs::PathFromString(fs::PathToString(key_path.filename()) + ".lock"), error)};
    if (!protection) return nullptr;
    LogInfo("FastBlocks: block producer with %u validator key(s)", keys.size());
    return std::make_unique<Producer>(node, hooks, std::move(keys), std::move(payout), std::move(protection));
}

Producer::Producer(node::NodeContext& node, const FastBlocksHooks& hooks, std::vector<CKey> keys, CScript payout, std::unique_ptr<SlashingProtection> protection)
    : m_node{node}, m_hooks{hooks}, m_keys{std::move(keys)}, m_payout{std::move(payout)}, m_protection{std::move(protection)}
{
}

Producer::~Producer() { Stop(); }

void Producer::Start()
{
    LOCK(m_mutex);
    if (m_thread.joinable()) return;
    m_stop = false;
    m_thread = std::thread{[this] { util::TraceThread("fbproducer", [this] { Run(); }); }};
}

void Producer::Stop()
{
    std::thread thread;
    {
        LOCK(m_mutex);
        m_stop = true;
        thread = std::move(m_thread);
    }
    m_wake.notify_all();
    if (thread.joinable()) thread.join();
}

Producer::Status Producer::GetStatus() const
{
    LOCK(m_mutex);
    return m_status;
}

int64_t Producer::LastSignedSlot() const
{
    int64_t last{-1};
    for (const CKey& key : m_keys) {
        if (const auto signed_slot{m_protection->LastSigned(XOnly(key))}) last = std::max(last, *signed_slot);
    }
    return last;
}

std::vector<uint256> Producer::Keys() const
{
    std::vector<uint256> keys;
    for (const CKey& key : m_keys) keys.push_back(XOnly(key));
    return keys;
}

void Producer::Run()
{
    const Consensus::PosParams& params{m_hooks.Params()};
    int64_t last_tried{-1};
    while (!m_stop) {
        const int64_t now{TicksSinceEpoch<std::chrono::seconds>(NodeClock::now())};
        const int64_t slot{SlotOf(now, params)};
        if (m_enabled && slot > last_tried && now >= SlotTime(slot, params) && now > params.genesis_time) {
            last_tried = slot;
            std::string error;
            const auto made{Produce(slot, error)};
            if (!made && !error.empty()) LogDebug(BCLog::VALIDATION, "FastBlocks: no block for slot %d: %s\n", slot, error);
        }
        WAIT_LOCK(m_mutex, lock);
        m_wake.wait_for(lock, std::chrono::milliseconds{100}, [this] { return m_stop.load(); });
    }
}

std::optional<CBlock> Producer::Build(int64_t slot, const std::optional<AnchorAck>& ack, std::string& error) const
{
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::PosParams& params{m_hooks.Params()};
    node::BlockCreateOptions options;
    options.coinbase_output_script = m_payout;
    options.test_block_validity = false;
    node::SidechainAssembly assembly;
    PosSections sections;
    if (ack) {
        const CBlockIndex* anchored{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(ack->ticket.anchored))};
        if (!anchored) {
            error = "the block the ticket anchors is unknown";
            return std::nullopt;
        }
        sections.ack = ack;
        assembly.peg = node::SidechainAssembly::Peg::STEP;
        // The bundle is P's (design 4.3.4): its withdrawals, its hash as the nonce, its height as the lock time.
        assembly.step = sidechain::PegStep{.main_height = ack->main_height - 1, .nonce = anchored->GetBlockHash(), .max_withdrawal_height = anchored->nHeight, .peg_height = anchored->nHeight};
        assembly.sections = sections.Encode();
    } else {
        assembly.peg = node::SidechainAssembly::Peg::NONE;
    }
    std::unique_ptr<node::CBlockTemplate> block_template;
    try {
        block_template = node::BlockAssembler{chainman.ActiveChainstate(), m_node.mempool.get(), options, assembly}.CreateNewBlock();
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }
    CBlock block{block_template->block};

    // The fee split: the producer keeps its share, the rest goes to the pot.
    CMutableTransaction coinbase{*block.vtx.at(0)};
    const CAmount fees{coinbase.vout.at(0).nValue};
    const CAmount share{ProducerShare(fees, params.producer_share_bps)};
    coinbase.vout[0].nValue = share;
    if (fees - share > 0) coinbase.vout.emplace_back(fees - share, PotScript());
    block.vtx[0] = MakeTransactionRef(std::move(coinbase));
    node::RegenerateCommitments(block, chainman);

    block.nVersion |= POS_BLOCK_BIT;
    block.nTime = static_cast<uint32_t>(SlotTime(slot, params));
    block.nBits = PosBits(chainman.GetConsensus().powLimit);
    block.nNonce = ack ? static_cast<uint32_t>(ack->main_height) : 0;
    block.vSections = sections.Encode();
    block.ext.hashSections = block.vSections.empty() ? uint256{} : SectionsHash(block.vSections);
    block.ext.sig = {};
    return block;
}

bool Producer::Sign(CBlock& block, const CKey& key)
{
    return key.SignSchnorr(BlockSigHash(block), block.ext.sig, /*merkle_root=*/nullptr, GetRandHash());
}

std::optional<uint256> Producer::Produce(int64_t slot, std::string& error)
{
    LOCK(m_produce_mutex);
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::PosParams& params{m_hooks.Params()};
    const auto leader{m_hooks.Validators().Leader(slot, params, chainman.GetConsensus().hashGenesisBlock)};
    if (!leader) {
        error = "no blocks of stake on this chain";
        return std::nullopt;
    }
    const uint256 leader_key{m_hooks.Validators().Validators()[*leader].pubkey};
    const CKey* key{nullptr};
    for (const CKey& candidate : m_keys) {
        if (XOnly(candidate) == leader_key) key = &candidate;
    }
    if (!key) return std::nullopt;
    const auto fail{[&](std::string why) -> std::optional<uint256> {
        error = std::move(why);
        LOCK(m_mutex);
        ++m_status.failed;
        m_status.error = error;
        return std::nullopt;
    }};

    std::optional<AnchorAck> ack;
    uint256 tip_hash;
    {
        LOCK(::cs_main);
        const CBlockIndex* tip{chainman.ActiveChain().Tip()};
        // Not while this node is behind: its block would fork the chain. Behind means a better block
        // it has and has yet to connect, not a better header: a leader can sign a header that claims
        // an acknowledgement (more anchors than any chain) and never send its block.
        if (Behind(chainman)) return fail("the node is still taking in the best chain");
        if (const auto last{FastBlocksHooks::LastPosSlot(tip, params)}; last && *last >= slot) return fail("the chain has a block of this slot or a later one");
        if (SlotTime(slot, params) <= tip->GetMedianTimePast()) return fail("the slot is not after the median time of the chain");
        const FastState fast{GetFastState(chainman.ActiveChainstate().SideState().View())};
        ack = m_hooks.NextAck(*tip, fast, slot);
        if (!ack && fast.first_unanchored_slot >= 0 && slot - fast.first_unanchored_slot >= params.max_unanchored_slots) {
            return fail("no anchor for too long: blocks of stake wait for one");
        }
        tip_hash = tip->GetBlockHash();
    }
    std::string build_error;
    auto block{Build(slot, ack, build_error)};
    // An acknowledgement the block cannot carry: the block without it.
    if (!block && ack) {
        LogInfo("FastBlocks: slot %d without the acknowledgement of %s: %s", slot, ack->ticket.Hash().ToString(), build_error);
        ack.reset();
        block = Build(slot, ack, build_error);
    }
    if (!block) return fail(build_error);
    if (block->hashPrevBlock != tip_hash) return fail("the tip moved while the block was built");

    // The slot is noted on disk before the signature exists.
    std::string protection_error;
    if (!m_protection->MaySign(leader_key, slot, protection_error)) return fail("slashing protection: " + protection_error);
    if (!Sign(*block, *key)) return fail("signing failed");

    const auto check{[&]() -> std::optional<std::string> {
        LOCK(::cs_main);
        if (chainman.ActiveChain().Tip()->GetBlockHash() != tip_hash) return "the tip moved while the block was built";
        const BlockValidationState state{TestBlockValidity(chainman.ActiveChainstate(), *block, /*check_pow=*/false, /*check_merkle_root=*/true)};
        if (!state.IsValid()) return "the block is not valid: " + state.ToString();
        return std::nullopt;
    }};
    auto invalid{check()};
    // An acknowledgement that makes the block invalid (what the state checks against the record, say):
    // the block without it, rather than no block. (Signed again: only one of the two leaves the node.)
    if (invalid && ack) {
        LogInfo("FastBlocks: slot %d without the acknowledgement of %s: %s", slot, ack->ticket.Hash().ToString(), *invalid);
        ack.reset();
        block = Build(slot, ack, build_error);
        if (!block) return fail(build_error);
        if (block->hashPrevBlock != tip_hash) return fail("the tip moved while the block was built");
        if (!Sign(*block, *key)) return fail("signing failed");
        invalid = check();
    }
    if (invalid) return fail(*invalid);
    const auto shared{std::make_shared<const CBlock>(*block)};
    bool new_block{false};
    if (!chainman.ProcessNewBlock(shared, /*force_processing=*/true, /*min_pow_checked=*/true, &new_block)) return fail("the node refused the block");
    LogInfo("FastBlocks: block %s of slot %d%s", shared->GetHash().ToString(), slot, ack ? strprintf(", acknowledging the anchor committed at %d", ack->main_height) : "");
    LOCK(m_mutex);
    ++m_status.produced;
    if (ack) ++m_status.acknowledgements;
    m_status.last_slot = slot;
    m_status.error.clear();
    return shared->GetHash();
}

} // namespace pos
