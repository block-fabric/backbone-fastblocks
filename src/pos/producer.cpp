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
#include <pos/staking.h>
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
//! Tickets (and offences) the producer leaves out of its blocks, at most.
constexpr size_t MAX_REFUSED{1000};

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
    // (A key leads slots once it is in the validator set of an epoch: from the start, or bonded.)
    for (const CKey& key : keys) LogInfo("FastBlocks: validator key %s", HexStr(XOnly(key)));
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

void Producer::SetExtraCheckForTesting(std::function<std::optional<std::string>(const CBlock&)> check)
{
    LOCK(m_mutex);
    m_extra_check = std::move(check);
}

std::set<uint256> Producer::RefusedTickets() const
{
    LOCK(m_mutex);
    return m_refused_tickets;
}

std::set<Offence> Producer::RefusedOffences() const
{
    LOCK(m_mutex);
    return m_refused_offences;
}

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

std::optional<std::array<unsigned char, 64>> Producer::SignBondPop(const uint256& pubkey, const uint160& owner) const
{
    for (const CKey& key : m_keys) {
        if (XOnly(key) != pubkey) continue;
        std::array<unsigned char, 64> pop;
        if (!key.SignSchnorr(BondPopHash(owner, m_node.chainman->GetConsensus().hashGenesisBlock), pop, /*merkle_root=*/nullptr, GetRandHash())) return std::nullopt;
        return pop;
    }
    return std::nullopt;
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

std::optional<CBlock> Producer::Build(int64_t slot, const std::optional<AnchorAck>& ack, std::string& error, const std::vector<DoubleProposal>& evidence) const
{
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::PosParams& params{m_hooks.Params()};
    node::BlockCreateOptions options;
    options.coinbase_output_script = m_payout;
    options.test_block_validity = false;
    node::SidechainAssembly assembly;
    PosSections sections;
    sections.evidence = evidence;
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

bool Producer::Sign(CBlock& block, const CKey& key, const uint256& genesis)
{
    // Deterministic: BIP340 with the auxiliary randomness all zero, so that the nonce depends on the
    // key and the message alone (BIP340's own derivation). A block signed twice gets the same
    // signature, and no peer holds a copy with another one. (The auxiliary randomness only guards the
    // nonce against side channels on the signing machine; a validator key signs nothing else.)
    return key.SignSchnorr(BlockSigHash(block, genesis), block.ext.sig, /*merkle_root=*/nullptr, uint256::ZERO);
}

std::optional<uint256> Producer::Produce(int64_t slot, std::string& error)
{
    LOCK(m_produce_mutex);
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::PosParams& params{m_hooks.Params()};
    const auto fail{[&](std::string why) -> std::optional<uint256> {
        error = std::move(why);
        LOCK(m_mutex);
        ++m_status.failed;
        m_status.error = error;
        return std::nullopt;
    }};

    std::optional<AnchorAck> ack;
    std::vector<DoubleProposal> evidence;
    std::vector<Offence> offences;
    uint256 tip_hash;
    uint256 leader_key;
    const CKey* key{nullptr};
    {
        LOCK(::cs_main);
        const CBlockIndex* tip{chainman.ActiveChain().Tip()};
        // The leader of the slot on the tip: the set of its epoch, which an anchor made, and its seed.
        const FastBlocksHooks::SlotLeader leader{m_hooks.LeaderOf(*tip, slot)};
        switch (leader.status) {
        case FastBlocksHooks::LeaderStatus::INACTIVE: error = "no blocks of stake in this epoch: too few validators"; return std::nullopt;
        case FastBlocksHooks::LeaderStatus::SEED_UNKNOWN: error = "the mainchain block that seeds the epoch is not on record yet"; return std::nullopt;
        case FastBlocksHooks::LeaderStatus::SET_UNKNOWN: error = "the validator set of the epoch is not known"; return std::nullopt;
        case FastBlocksHooks::LeaderStatus::OK: break;
        }
        leader_key = leader.pubkey;
        for (const CKey& candidate : m_keys) {
            if (XOnly(candidate) == leader_key) key = &candidate;
        }
        if (!key) return std::nullopt;
        // Not while this node is behind: its block would fork the chain. Behind means a better block
        // it has and has yet to connect, not a better header: a leader can sign a header that claims
        // an acknowledgement (more anchors than any chain) and never send its block.
        if (Behind(chainman)) return fail("the node is still taking in the best chain");
        if (const auto last{FastBlocksHooks::LastPosSlot(tip, params)}; last && *last >= slot) return fail("the chain has a block of this slot or a later one");
        if (SlotTime(slot, params) <= tip->GetMedianTimePast()) return fail("the slot is not after the median time of the chain");
        const sidechain::StoreView& view{chainman.ActiveChainstate().SideCache()};
        // A banned leader leads no more (its block would be invalid: bad-pos-leader-banned).
        if (const auto record{GetValidator(view, leader_key)}; record && record->tombstoned) return fail("this validator is banned: it signed two blocks for a slot");
        const FastState fast{GetFastState(view)};
        const auto [refused_tickets, refused_offences]{WITH_LOCK(m_mutex, return std::make_pair(m_refused_tickets, m_refused_offences))};
        ack = m_hooks.NextAck(*tip, fast, slot, &refused_tickets);
        if (!ack && fast.first_unanchored_slot >= 0 && slot - fast.first_unanchored_slot >= params.max_unanchored_slots) {
            return fail("no anchor for too long: blocks of stake wait for one");
        }
        // The proofs this node knows that the chain has not punished yet, one per validator (a
        // validator is slashed once: a second proof against it in the block would be invalid).
        const uint64_t anchors{fast.anchors + (ack ? 1 : 0)};
        std::set<uint256> offenders;
        for (const auto& [offence, proof] : m_hooks.Evidence().All()) {
            if (evidence.size() >= MAX_SECTION_EVIDENCE) break;
            if (refused_offences.contains(offence) || offenders.contains(offence.pubkey)) continue;
            std::string reason;
            if (m_hooks.CheckChainEvidence(proof, *tip, slot, view, anchors, reason)) {
                evidence.push_back(proof);
                offences.push_back(offence);
                offenders.insert(offence.pubkey);
            }
        }
        tip_hash = tip->GetBlockHash();
    }
    std::string build_error;
    auto block{Build(slot, ack, build_error, evidence)};
    // An acknowledgement the block cannot carry: the block without it.
    if (!block && ack) {
        LogInfo("FastBlocks: slot %d without the acknowledgement of %s: %s", slot, ack->ticket.Hash().ToString(), build_error);
        ack.reset();
        block = Build(slot, ack, build_error, evidence);
    }
    if (!block) return fail(build_error);
    if (block->hashPrevBlock != tip_hash) return fail("the tip moved while the block was built");

    // The slot is noted on disk before the signature exists.
    std::string protection_error;
    if (!m_protection->MaySign(leader_key, slot, protection_error)) return fail("slashing protection: " + protection_error);
    if (!Sign(*block, *key, chainman.GetConsensus().hashGenesisBlock)) return fail("signing failed");

    // The block is not made again without what made it invalid: the node checked the signed header
    // (and noted it, to find leaders that sign twice), and a second block of the slot would be a
    // second signature, which this node would take for proof against its own key. The slot is lost;
    // the acknowledgement and the proofs it carried are left out of the blocks to come.
    std::optional<std::string> invalid;
    {
        LOCK(::cs_main);
        if (chainman.ActiveChain().Tip()->GetBlockHash() != tip_hash) return fail("the tip moved while the block was built");
        const BlockValidationState state{TestBlockValidity(chainman.ActiveChainstate(), *block, /*check_pow=*/false, /*check_merkle_root=*/true)};
        if (!state.IsValid()) invalid = state.ToString();
    }
    if (const auto extra{WITH_LOCK(m_mutex, return m_extra_check)}; !invalid && extra) invalid = extra(*block);
    if (invalid) {
        if (ack || !evidence.empty()) {
            LogInfo("FastBlocks: slot %d lost; its acknowledgement and proofs are left out of the next blocks: %s", slot, *invalid);
            LOCK(m_mutex);
            if (m_refused_tickets.size() >= MAX_REFUSED) m_refused_tickets.clear();
            if (m_refused_offences.size() >= MAX_REFUSED) m_refused_offences.clear();
            if (ack) m_refused_tickets.insert(ack->ticket.Hash());
            m_refused_offences.insert(offences.begin(), offences.end());
        }
        return fail("the block is not valid: " + *invalid);
    }
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
