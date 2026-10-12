// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/hooks.h>

#include <arith_uint256.h>
#include <chain.h>
#include <consensus/validation.h>
#include <kernel/chainparams.h>
#include <logging.h>
#include <net_processing.h>
#include <node/context.h>
#include <pos/forkchoice.h>
#include <pos/producer.h>
#include <sidechain/follower.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <streams.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>

#include <array>
#include <ios>

namespace pos {

namespace {
//! FastBlocks' tables of the sidechain store (0x50 to 0x5f are FastBlocks'; see sidechain/state.h).
const sidechain::Cell<FastState> FAST_STATE{0x55, FastState{}};
//! The pot accrued as of a block, by height: written by the PoS blocks that add to it, erased once paid.
const sidechain::Table<uint32_t, int64_t> POT_ACCRUED{0x56};

FastBlocksHooks* g_installed{nullptr};

//! Validator sets of anchors a node keeps in memory, at most.
constexpr size_t MAX_KNOWN_SETS{4096};

bool Fail(std::string& reject_reason, const char* reason)
{
    reject_reason = reason;
    return false;
}

/** The ancestor of `index` at `height`, if `index` is that high. */
const CBlockIndex* AncestorAt(const CBlockIndex* index, int height)
{
    if (!index || height < 0 || height > index->nHeight) return nullptr;
    return index->GetAncestor(height);
}

/** Whether `block` is `ancestor` or descends from it. */
bool Descends(const CBlockIndex* block, const CBlockIndex* ancestor)
{
    return block && ancestor && AncestorAt(block, ancestor->nHeight) == ancestor;
}
} // namespace

FastState GetFastState(const sidechain::StoreView& view) { return FAST_STATE.Get(view); }

int64_t PotAccruedAt(const sidechain::StoreView& view, int32_t height)
{
    int64_t accrued{FAST_STATE.Get(view).pot_paid};
    if (height < 0) return accrued;
    POT_ACCRUED.ForEach(view, [&](const uint32_t& at, const int64_t& value) {
        if (at > static_cast<uint32_t>(height)) return false;
        accrued = value;
        return true;
    });
    return accrued;
}

FastBlocksHooks::FastBlocksHooks(const CChainParams& params)
    : m_chain_params{params},
      m_params{params.GetConsensus().pos},
      m_genesis_set{params.GetConsensus().pos},
      m_staking{params.GetConsensus().pos, params.GetConsensus().hashGenesisBlock},
      m_pos_bits{PosBits(params.GetConsensus().powLimit)}
{
}

FastBlocksHooks::~FastBlocksHooks() = default;

sidechain::BlockKind FastBlocksHooks::Kind(const CBlockHeader& header) const
{
    return IsPosHeader(header) ? sidechain::BlockKind::PLUGIN : sidechain::BlockKind::BMM;
}

std::optional<int64_t> FastBlocksHooks::LastPosSlot(const CBlockIndex* index, const Consensus::PosParams& params)
{
    for (; index; index = index->pprev) {
        if (index->m_ext) return SlotOf(index->nTime, params);
    }
    return std::nullopt;
}

bool FastBlocksHooks::CheckHeader(const CBlockHeader& header, const CBlockIndex& prev, const ChainstateManager& chainman, BlockValidationState& state) const
{
    AssertLockHeld(::cs_main);
    const auto invalid{[&](const char* reason, const char* debug) { return state.Invalid(BlockValidationResult::BLOCK_INVALID_HEADER, reason, debug); }};
    if (header.nBits != m_pos_bits) return invalid("bad-pos-bits", "a block of stake has the chain's proof-of-work limit as nBits");
    const int64_t time{header.nTime};
    if (time <= m_params.genesis_time || (time - m_params.genesis_time) % m_params.slot_seconds != 0) {
        return invalid("bad-pos-time", "a block of stake has the time its slot starts");
    }
    const int64_t slot{SlotOf(time, m_params)};
    // The signature before anything that reads more of the chain, or keeps the header to try again
    // later: only a leader makes the node look further. The leader is that of the validator set of the
    // slot's epoch, which an anchor before the epoch before it made, and of the epoch's seed, which the
    // record of the mainchain gives.
    const SlotLeader leader{LeaderOf(prev, slot)};
    switch (leader.status) {
    case LeaderStatus::INACTIVE:
        return invalid("bad-pos-inactive", "no blocks of stake in this epoch: too few validators");
    case LeaderStatus::SEED_UNKNOWN:
        return state.Invalid(BlockValidationResult::BLOCK_TIME_FUTURE, "pos-seed-unknown", "the mainchain block that seeds the epoch is not on record yet");
    case LeaderStatus::SET_UNKNOWN:
        // The set of an anchor of a branch this node has not connected: the block is checked when it
        // is (AfterBlock), as that anchor is then.
        break;
    case LeaderStatus::OK:
        if (!CheckBlockSig(header, leader.pubkey)) return invalid("bad-pos-sig", "a block of stake is signed by the leader of its slot");
        break;
    }
    if (header.Time() > NodeClock::now() + std::chrono::seconds{m_params.max_future_seconds}) {
        return state.Invalid(BlockValidationResult::BLOCK_TIME_FUTURE, "pos-time-future", "the slot of the block of stake has not started");
    }
    // A second header the leader signed for this slot is the proof that it signed twice, whatever
    // else is wrong with it.
    if (leader.status == LeaderStatus::OK) {
        if (const auto proof{m_evidence.NoteSigned(header, slot)}) {
            std::string reason;
            if (const auto offence{CheckEvidence(*proof, leader.pubkey, m_params, reason)}) {
                if (m_evidence.Add(*offence, *proof)) LogWarning("FastBlocks: validator %s signed two blocks for slot %d", HexStr(offence->pubkey), offence->slot);
            }
        }
    }
    if (const auto last{LastPosSlot(&prev, m_params)}; last && slot <= *last) {
        return invalid("bad-pos-slot", "a block of stake comes in a later slot than the one before it");
    }
    // No more than max_unanchored_slots after the first block of stake no anchor covers, as the
    // state checks (AfterBlock), as far as the headers tell: this bounds the headers a node takes
    // in without anchors. Without an acknowledgement, that block is the first block of stake after
    // the last BMM block, or at or before the last acknowledgement (which covers at most up to its
    // parent). (An acknowledgement may cover much: the state checks it.)
    if (header.nNonce == 0) {
        int64_t first{slot};
        int64_t walked{0};
        for (const CBlockIndex* index{&prev}; index && index->nHeight > 0 && index->m_ext; index = index->pprev) {
            first = SlotOf(index->nTime, m_params);
            // (Slots grow block by block: so many blocks back, the bound is passed.)
            if (index->nNonce != 0 || ++walked > m_params.max_unanchored_slots) break;
        }
        if (slot - first >= m_params.max_unanchored_slots) {
            return invalid("bad-pos-unanchored", "a block of stake comes within max_unanchored_slots of the first one no anchor covers");
        }
    }
    // An acknowledgement names a mainchain block with a commitment: not known yet, the block may be
    // tried again later (it is checked in full with the block).
    if (header.nNonce != 0 && (!chainman.m_mainchain || !chainman.m_mainchain->BmmAt(static_cast<int>(header.nNonce)))) {
        return state.Invalid(BlockValidationResult::BLOCK_TIME_FUTURE, "pos-ack-unknown", "no commitment on record at the height the block acknowledges");
    }
    return true;
}

bool FastBlocksHooks::CheckBlock(const CBlock& block, BlockValidationState& state) const
{
    const auto invalid{[&](const char* reason, const char* debug) { return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, reason, debug); }};
    size_t pots{0};
    for (const CTxOut& out : block.vtx.at(0)->vout) pots += IsPotScript(out.scriptPubKey);
    // Proofs in the coinbase of a BMM block (a block of stake has them in its sections): their form (the
    // chain checks the rest: AfterBlock).
    size_t coinbase_proofs{0};
    for (const CTxOut& out : block.vtx.at(0)->vout) {
        if (!IsEvidenceScript(out.scriptPubKey)) continue;
        if (IsPosHeader(block)) return invalid("bad-pos-evidence-not-bmm", "a block of stake carries proofs in its sections, not its coinbase");
        if (++coinbase_proofs > MAX_BMM_EVIDENCE) return invalid("bad-pos-evidence-count", "too many proofs in the coinbase of a BMM block");
        const auto proof{ParseEvidenceScript(out.scriptPubKey)};
        if (!proof) return invalid("bad-pos-evidence-output", "a proof in the coinbase of a BMM block does not read");
        std::string reason;
        if (!CheckEvidenceForm(*proof, m_params, reason)) return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, reason, "a proof that a leader signed twice does not have the form of one");
    }
    if (!IsPosHeader(block)) {
        if (pots) return invalid("bad-pos-pot-not-pos", "only a block of stake puts coins in the fee pot");
        return true;
    }
    if (pots > 1) return invalid("bad-pos-pot-multiple", "one fee pot output at most");
    PosSections sections;
    if (!PosSections::Decode(block.vSections, sections)) return invalid("bad-pos-sections", "the sections of a block of stake do not read");
    // The form of its proofs (the chain checks the rest: AfterBlock).
    for (const DoubleProposal& proof : sections.evidence) {
        std::string reason;
        if (!CheckEvidenceForm(proof, m_params, reason)) return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, reason, "a proof that a leader signed twice does not have the form of one");
    }
    if (!sections.ack) {
        if (block.nNonce != 0) return invalid("bad-pos-ack-nonce", "a block with no acknowledgement has nNonce 0");
        return true;
    }
    const AnchorAck& ack{*sections.ack};
    if (ack.main_height <= 0 || static_cast<uint32_t>(ack.main_height) != block.nNonce) {
        return invalid("bad-pos-ack-nonce", "nNonce is the mainchain height of the acknowledged commitment");
    }
    if (ack.ticket.version != 1) return invalid("bad-pos-ticket-version", "unknown ticket version");
    if (!IsValidPayee(ack.ticket.payee)) return invalid("bad-pos-ticket-payee", "a ticket pays the pot to a standard script of 80 bytes or less");
    // Noted (to find the blocks that rest on a commitment the mainchain drops) only if the leader of
    // its slot signed it: anyone can make a block that reads, and enough of them would push the
    // acknowledgements that count out of memory. (The header rules turn the others away. A block whose
    // leader this node cannot tell yet is noted when it is connected: AfterBlock.)
    const int64_t time{block.nTime};
    if (time > m_params.genesis_time && (time - m_params.genesis_time) % m_params.slot_seconds == 0 && Chainman()) {
        LOCK(::cs_main);
        if (const CBlockIndex* prev{Chainman()->m_blockman.LookupBlockIndex(block.hashPrevBlock)}) {
            const SlotLeader leader{LeaderOf(*prev, SlotOf(time, m_params))};
            if (leader.status == LeaderStatus::OK && CheckBlockSig(block, leader.pubkey)) NoteAck(block.GetHash(), ack);
        }
    }
    return true;
}

uint256 FastBlocksHooks::SectionsHash(std::span<const unsigned char> sections) const { return pos::SectionsHash(sections); }

sidechain::OtherExtension FastBlocksHooks::CheckOtherExtension(const CBlockHeader& header, const CBlockIndex& index) const
{
    AssertLockHeld(::cs_main);
    // (The block hash covers hashSections: only the signature can differ.)
    if (!IsPosHeader(header) || !index.m_ext || !index.pprev || header.ext.hashSections != index.m_ext->hashSections) return sidechain::OtherExtension::INVALID;
    const SlotLeader leader{LeaderOf(*index.pprev, SlotOf(header.nTime, m_params))};
    switch (leader.status) {
    case LeaderStatus::SET_UNKNOWN:
    case LeaderStatus::SEED_UNKNOWN:
        return sidechain::OtherExtension::UNKNOWN;
    case LeaderStatus::INACTIVE:
        return sidechain::OtherExtension::INVALID;
    case LeaderStatus::OK:
        break;
    }
    if (!CheckBlockSig(header, leader.pubkey)) return sidechain::OtherExtension::INVALID;
    // A valid signature, and the node's own (taken before it knew the leader) is not known to be: the
    // node cannot tell which block is the one to keep. No fault of the peer's.
    if (!CheckBlockSig(index.GetBlockHeader(), leader.pubkey)) return sidechain::OtherExtension::UNKNOWN;
    return sidechain::OtherExtension::EQUIVALENT;
}

int64_t FastBlocksHooks::MaxFutureBlockTime() const
{
    // While the network has blocks of stake: from the start (genesis validators), or once the node saw an
    // active set made from bonds.
    return m_genesis_set.Active() || m_staking_seen.load() ? MAX_FUTURE_BLOCK_TIME_POS : ChainHooks::MaxFutureBlockTime();
}

bool FastBlocksHooks::IsWaitReason(const std::string& reject_reason) const
{
    // A BMM block the mainchain has not committed to yet; a block of stake whose slot has not started
    // by this node's clock, or that acknowledges a commitment not on this node's record yet.
    // Or whose epoch's seed is not on record yet.
    return reject_reason == "bmm-unknown" || reject_reason == "pos-time-future" || reject_reason == "pos-ack-unknown" || reject_reason == "pos-seed-unknown";
}

std::optional<int64_t> FastBlocksHooks::WaitingReady(const CBlockHeader& header, const sidechain::Mainchain& record) const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman) return ChainHooks::WaitingReady(header, record);
    AssertLockHeld(::cs_main);
    // Ready when it is, and so is every waiting header between it and a block the node knows: then
    // its height orders it after its parent (blocks of both kinds wait, so commitment heights would not).
    const auto ready{[&](const CBlockHeader& waiting) {
        if (!IsPosHeader(waiting)) return record.CommittedHeight(waiting.GetHash()).has_value();
        if (waiting.Time() > NodeClock::now() + std::chrono::seconds{m_params.max_future_seconds}) return false;
        if (!EpochSeed(EpochOf(SlotOf(waiting.nTime, m_params), m_params), m_params, record)) return false;
        return waiting.nNonce == 0 || record.BmmAt(static_cast<int>(waiting.nNonce)).has_value();
    }};
    const CBlockHeader* current{&header};
    for (int64_t depth{1}; depth <= static_cast<int64_t>(chainman->m_bmm_waiting.size()) + 1; ++depth) {
        if (!ready(*current)) return std::nullopt;
        if (const CBlockIndex* prev{chainman->m_blockman.LookupBlockIndex(current->hashPrevBlock)}) return prev->nHeight + depth;
        const auto parent{chainman->m_bmm_waiting.find(current->hashPrevBlock)};
        if (parent == chainman->m_bmm_waiting.end()) return std::nullopt;
        current = &parent->second.header;
    }
    return std::nullopt;
}

arith_uint256 FastBlocksHooks::ChainWork(const CBlockIndex* prev, const CBlockHeader& header) const
{
    if (!prev) return ChainKey{}.Work();
    ChainKey key{ChainKey::FromWork(prev->nChainWork)};
    key.height = static_cast<uint64_t>(prev->nHeight) + 1;
    if (IsPosHeader(header)) {
        // An acknowledgement is an anchor; the mainchain height of its commitment is in the header.
        if (header.nNonce != 0) {
            ++key.anchors;
            key.last_anchor_height = header.nNonce;
        }
    } else {
        // A BMM block is an anchor, committed where the record says (the worst place if it does not).
        ++key.anchors;
        key.last_anchor_height = 0xFFFFFFFFu;
        if (const ChainstateManager* chainman{Chainman()}; chainman && chainman->m_mainchain) {
            if (const auto height{chainman->m_mainchain->BmmHeight(header.GetHash())}) key.last_anchor_height = static_cast<uint32_t>(*height);
        }
    }
    return key.Work();
}

void FastBlocksHooks::NoteAck(const uint256& block_hash, const AnchorAck& ack) const
{
    const uint256 commitment{ack.ticket.Hash()};
    LOCK(m_acks_mutex);
    if (!m_ack_of.emplace(block_hash, commitment).second) return;
    m_anchored_by.emplace(block_hash, ack.ticket.anchored);
    m_acks_by_commitment[commitment].insert(block_hash);
    m_ack_order.emplace_back(block_hash, commitment);
    // Bounded: the oldest go first (a commitment so old is not dropped by the mainchain any more; and
    // CommitmentOf reads a block from disk when it has to).
    while (m_ack_order.size() > MAX_NOTED_ACKS) {
        const auto& [old_block, old_commitment] = m_ack_order.front();
        m_ack_of.erase(old_block);
        m_anchored_by.erase(old_block);
        if (auto found{m_acks_by_commitment.find(old_commitment)}; found != m_acks_by_commitment.end()) {
            found->second.erase(old_block);
            if (found->second.empty()) m_acks_by_commitment.erase(found);
        }
        m_ack_order.pop_front();
    }
}

std::optional<AnchorAck> FastBlocksHooks::ReadAck(const CBlockIndex& index) const
{
    ChainstateManager* chainman{Chainman()};
    if (!index.m_ext || index.nNonce == 0 || !chainman) return std::nullopt;
    CBlock block;
    const bool have{WITH_LOCK(::cs_main, return (index.nStatus & BLOCK_HAVE_DATA) && chainman->m_blockman.ReadBlock(block, index))};
    PosSections sections;
    if (!have || !PosSections::Decode(block.vSections, sections) || !sections.ack) return std::nullopt;
    NoteAck(index.GetBlockHash(), *sections.ack);
    return sections.ack;
}

const CBlockIndex* FastBlocksHooks::AnchoredBy(const CBlockIndex& index) const
{
    AssertLockHeld(::cs_main);
    if (!index.m_ext) return index.nHeight > 0 ? index.pprev : nullptr;
    if (index.nNonce == 0) return nullptr;
    ChainstateManager* chainman{Chainman()};
    if (!chainman) return nullptr;
    std::optional<uint256> anchored;
    {
        LOCK(m_acks_mutex);
        if (const auto found{m_anchored_by.find(index.GetBlockHash())}; found != m_anchored_by.end()) anchored = found->second;
    }
    if (!anchored) {
        const auto ack{ReadAck(index)};
        if (!ack) return nullptr;
        anchored = ack->ticket.anchored;
    }
    // On the chain of the acknowledgement, below it (a valid one names such a block).
    const CBlockIndex* target{chainman->m_blockman.LookupBlockIndex(*anchored)};
    if (!target || target->nHeight >= index.nHeight || index.GetAncestor(target->nHeight) != target) return nullptr;
    return target;
}

std::optional<uint256> FastBlocksHooks::CommitmentOf(const CBlockIndex& index) const
{
    // A BMM block needs a commitment to itself; a PoS block one to the ticket it acknowledges, if any.
    if (!index.m_ext) return index.GetBlockHash();
    if (index.nNonce == 0) return std::nullopt;
    {
        LOCK(m_acks_mutex);
        if (const auto found{m_ack_of.find(index.GetBlockHash())}; found != m_ack_of.end()) return found->second;
    }
    ChainstateManager* chainman{Chainman()};
    if (chainman) {
        if (const auto ack{ReadAck(index)}) return ack->ticket.Hash();
        // A header without its block: what the record has at the height it names.
        if (chainman->m_mainchain) {
            if (const auto bmm{chainman->m_mainchain->BmmAt(static_cast<int>(index.nNonce))}) return *bmm;
        }
    }
    // Nothing on record: a commitment nobody made.
    return index.GetBlockHash();
}

std::vector<uint256> FastBlocksHooks::BlocksCommittedBy(const uint256& commitment) const
{
    std::vector<uint256> blocks{commitment};
    LOCK(m_acks_mutex);
    if (const auto found{m_acks_by_commitment.find(commitment)}; found != m_acks_by_commitment.end()) {
        blocks.insert(blocks.end(), found->second.begin(), found->second.end());
    }
    return blocks;
}

bool FastBlocksHooks::GetPegStep(const CBlock& block, const sidechain::Mainchain& mainchain, std::optional<sidechain::PegStep>& step, std::string& reject_reason) const
{
    if (!IsPosHeader(block)) return ChainHooks::GetPegStep(block, mainchain, step, reject_reason);
    PosSections sections;
    if (!PosSections::Decode(block.vSections, sections)) return Fail(reject_reason, "bad-pos-sections");
    step.reset();
    // A PoS block without an acknowledgement takes no peg actions.
    if (!sections.ack) return true;
    const AnchorAck& ack{*sections.ack};
    // The mainchain committed to the ticket, first, at the height the acknowledgement says (on record:
    // what block assembly assumes is for the block's own hash).
    const auto committed{mainchain.CommittedHeight(ack.ticket.Hash())};
    if (!committed) return Fail(reject_reason, "bmm-unknown");
    if (*committed != ack.main_height) return Fail(reject_reason, "bad-pos-ack-height");
    ChainstateManager* chainman{Chainman()};
    if (!chainman) return Fail(reject_reason, "bad-pos-context");
    const CBlockIndex* anchored{WITH_LOCK(::cs_main, return chainman->m_blockman.LookupBlockIndex(ack.ticket.anchored))};
    if (!anchored) return Fail(reject_reason, "bad-pos-ack-anchored");
    // As if the block followed the mainchain up to the block before the commitment, with the bundle
    // built from the block anchored: its withdrawals, its hash as the nonce, its height as the lock
    // time and as the height a bundle failure counts from (design 4.3.4: every block that
    // acknowledges the same anchor builds the same bundle, whatever its own height).
    step = sidechain::PegStep{.main_height = ack.main_height - 1, .nonce = anchored->GetBlockHash(), .max_withdrawal_height = anchored->nHeight, .peg_height = anchored->nHeight};
    return true;
}

bool FastBlocksHooks::BeforePegStep(sidechain::State& state, const CBlock& block, int height, const sidechain::PegStep& step, const Consensus::SidechainParams&,
                                    std::vector<CTxOut>& owed_tx, std::string& reject_reason) const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman) return Fail(reject_reason, "bad-pos-context");
    FastState fast{FAST_STATE.Get(state.View())};
    CScript payee;
    uint256 anchored;
    int32_t anchored_height;
    int32_t main_height;
    // (Block assembly gives the block without the PoS header bit, but with its sections: only PoS blocks have any.)
    if (IsPosHeader(block) || !block.vSections.empty()) {
        PosSections sections;
        if (!PosSections::Decode(block.vSections, sections) || !sections.ack) return Fail(reject_reason, "bad-pos-sections");
        const AnchorAck& ack{*sections.ack};
        // Anchors are acknowledged in the order the mainchain committed to them.
        if (ack.main_height <= fast.last_main_height) return Fail(reject_reason, "bad-pos-ack-order");
        LOCK(::cs_main);
        const CBlockIndex* prev{chainman->m_blockman.LookupBlockIndex(block.hashPrevBlock)};
        const CBlockIndex* target{chainman->m_blockman.LookupBlockIndex(ack.ticket.anchored)};
        // The block anchored is on this chain, at or below the parent.
        if (!target || !Descends(prev, target)) return Fail(reject_reason, "bad-pos-ack-anchored");
        // ... and it is, or comes after, the block that took the last peg actions: every refund and
        // every acknowledgement before is at or below it, so the bundle depends on it alone.
        if (fast.anchor_point_height >= 0) {
            const CBlockIndex* point{AncestorAt(target, fast.anchor_point_height)};
            if (!point || point->GetBlockHash() != fast.anchor_point) return Fail(reject_reason, "bad-pos-ack-chain");
        }
        payee = ack.ticket.payee;
        anchored = target->GetBlockHash();
        anchored_height = target->nHeight;
        main_height = ack.main_height;
    } else {
        // A BMM block anchors its parent, and its coinbase's first output takes the pot.
        payee = block.vtx.at(0)->vout.empty() ? CScript{} : block.vtx[0]->vout[0].scriptPubKey;
        anchored = block.hashPrevBlock;
        anchored_height = height - 1;
        main_height = step.main_height + 1;
    }

    // The pot accrued up to the block anchored, less what was paid, goes to the payee.
    sidechain::StoreOverlay& out{state.Writable()};
    const int64_t accrued{PotAccruedAt(state.View(), anchored_height)};
    // (At least what was paid: PotAccruedAt starts there, and the pot only grows.)
    const int64_t pay{accrued - fast.pot_paid};
    std::vector<uint32_t> paid_entries;
    POT_ACCRUED.ForEach(state.View(), [&](const uint32_t& at, const int64_t&) {
        if (at > static_cast<uint32_t>(anchored_height)) return false;
        paid_entries.push_back(at);
        return true;
    });
    for (const uint32_t at : paid_entries) POT_ACCRUED.Erase(out, at);
    fast.pot_paid = accrued;

    ++fast.anchors;
    fast.last_main_height = main_height;
    fast.last_anchored = anchored;
    fast.last_anchored_height = anchored_height;
    fast.anchor_point = block.GetHash();
    fast.anchor_point_height = height;
    FAST_STATE.Put(out, fast);

    // Staking: unbonded coins and rewards for proofs paid, a share of the penalties with the pot,
    // validators that missed their slots jailed, and the validator set of this anchor made. (A BMM
    // block's time may be ahead of the clock, by two hours at most: its epoch at the earliest, then.)
    const int64_t slot{SlotOf(block.nTime, m_params)};
    const bool pos_block{IsPosHeader(block) || !block.vSections.empty()};
    const int64_t earliest{pos_block ? slot : std::max<int64_t>(0, slot - MAX_FUTURE_BLOCK_TIME / m_params.slot_seconds)};
    // The anchor's slot, for the epochs whose set it makes (SetFor): not before the median time of the
    // mainchain block that committed to it (AnchorSlot), which whoever makes the block cannot set back.
    int64_t anchor_slot{slot};
    if (const auto main{chainman->m_mainchain ? chainman->m_mainchain->GetBlock(main_height) : std::nullopt}) anchor_slot = std::max(anchor_slot, SlotOf(main->time, m_params));
    std::vector<CTxOut> staking_owed;
    const auto staked{m_staking.PegStep(out, block.GetHash(), fast.anchors, anchor_slot, EpochOf(earliest, m_params), staking_owed)};
    if (staked.set_active) m_staking_seen = true;
    const int64_t paid_out{pay + staked.release};
    if (paid_out > 0) owed_tx.emplace_back(paid_out, payee);
    owed_tx.insert(owed_tx.end(), staking_owed.begin(), staking_owed.end());
    return true;
}

bool FastBlocksHooks::ApplyTx(sidechain::State& state, const CTransaction& tx, int height, bool, std::vector<CTxOut>&, std::string& reject_reason) const
{
    // Bonds and unbonds: an unbond is paid unbonding_anchors after the anchors of the chain so far.
    return m_staking.ApplyTx(state.Writable(), tx, height, FAST_STATE.Get(state.View()).anchors, reject_reason);
}

bool FastBlocksHooks::Stalled(const FastState& fast, int64_t main_time, bool pos_active) const
{
    if (!pos_active) return true;
    const int64_t last{fast.last_pos_slot >= 0 ? SlotTime(fast.last_pos_slot, m_params) : m_params.genesis_time};
    return main_time - last >= m_params.stall_seconds;
}

bool FastBlocksHooks::AfterBlock(sidechain::State& state, const CBlock& block, int height, const std::optional<sidechain::PegStep>& step, CAmount minted,
                                 const Consensus::SidechainParams&, std::string& reject_reason) const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman) return Fail(reject_reason, "bad-pos-context");
    sidechain::StoreOverlay& out{state.Writable()};
    FastState fast{FAST_STATE.Get(state.View())};
    const CTransaction& coinbase{*block.vtx.at(0)};
    LOCK(::cs_main);
    const CBlockIndex* prev{chainman->m_blockman.LookupBlockIndex(block.hashPrevBlock)};
    if (!prev) return Fail(reject_reason, "bad-pos-context");
    const int64_t slot{SlotOf(block.nTime, m_params)};
    if (IsPosHeader(block)) {
        // Signed by the leader of its slot, as this chain and the record say now: the header may have
        // come before the node knew the set of its epoch (CheckHeader), and the record may have moved
        // the seed since.
        const SlotLeader leader{LeaderOf(*prev, slot, &state.View())};
        switch (leader.status) {
        case LeaderStatus::INACTIVE: return Fail(reject_reason, "bad-pos-inactive");
        case LeaderStatus::SEED_UNKNOWN: return Fail(reject_reason, "pos-seed-unknown");
        case LeaderStatus::SET_UNKNOWN: return Fail(reject_reason, "bad-pos-set-unknown");
        case LeaderStatus::OK: break;
        }
        if (!CheckBlockSig(block, leader.pubkey)) return Fail(reject_reason, "bad-pos-sig");
        // A banned leader leads no more: from the block after the one that punished it, its slots
        // stay empty (the sets made before its ban still name it; the state of the chain below the
        // block says it is banned, so every node agrees). Its slots are not given to others: who
        // carries a proof would otherwise choose the leaders of the slots to come.
        if (const auto record{GetValidator(state.View(), leader.pubkey)}; record && record->tombstoned) return Fail(reject_reason, "bad-pos-leader-banned");

        // The fee split: what the producer keeps (all of the coinbase but the payouts and the pot) is at
        // most producer_share_bps of it and the pot together. Fees it leaves unclaimed are lost.
        CAmount total{0}, pot{0};
        for (const CTxOut& txout : coinbase.vout) {
            total += txout.nValue;
            if (IsPotScript(txout.scriptPubKey)) pot += txout.nValue;
        }
        // (Not negative: the payouts and the pot are outputs of the coinbase, which has no negative ones.)
        const CAmount take{total - minted - pot};
        arith_uint256 kept{static_cast<uint64_t>(take)}, share{static_cast<uint64_t>(take + pot)};
        kept *= arith_uint256{10000};
        share *= arith_uint256{static_cast<uint64_t>(m_params.producer_share_bps)};
        if (kept > share) return Fail(reject_reason, "bad-pos-producer-share");
        if (pot > 0) {
            fast.pot_accrued += pot;
            POT_ACCRUED.Put(out, static_cast<uint32_t>(height), fast.pot_accrued);
        }

        // PoS blocks stop when no anchor covers them for too long: the first one no anchor covers
        // starts the count. An acknowledgement covers what is at or below the block it anchors.
        const int64_t before_step_unanchored{fast.first_unanchored_slot};
        if (step) {
            int64_t first{slot};
            for (const CBlockIndex* index{prev}; index && index->nHeight > fast.last_anchored_height; index = index->pprev) {
                if (index->m_ext) first = SlotOf(index->nTime, m_params);
            }
            fast.first_unanchored_slot = first;
        } else if (fast.first_unanchored_slot < 0) {
            fast.first_unanchored_slot = slot;
        }
        if (slot - fast.first_unanchored_slot >= m_params.max_unanchored_slots) return Fail(reject_reason, "bad-pos-unanchored");

        // Liveness: the block's leader made it; the leaders of the slots since the last block of stake
        // (an epoch of them at most) made none -- of the slots at which a block could be made: not
        // those max_unanchored_slots or more after the first block of stake no anchor covered (blocks
        // of stake had stopped for want of anchors), nor those up to a BMM block the block follows
        // (which may end such a stop, or a stall, and whose slot is the earliest the leaders could
        // build on it). Leaders are not jailed for slots nobody could fill.
        int64_t charged_from{std::max(fast.last_pos_slot + 1, slot - m_params.slots_per_epoch)};
        int64_t charged_until{slot};
        if (prev->nHeight > 0 && !prev->m_ext) {
            charged_from = std::max(charged_from, SlotOf(prev->nTime, m_params) + 1);
        } else if (before_step_unanchored >= 0) {
            charged_until = std::min(charged_until, before_step_unanchored + m_params.max_unanchored_slots);
        }
        std::vector<std::pair<int64_t, uint256>> missed;
        std::map<int64_t, SlotLeader> epochs;
        for (int64_t at{charged_from}; at < charged_until; ++at) {
            const int64_t epoch{EpochOf(at, m_params)};
            auto found{epochs.find(epoch)};
            if (found == epochs.end()) found = epochs.emplace(epoch, LeaderOf(*prev, epoch * m_params.slots_per_epoch, &state.View())).first;
            const SlotLeader& schedule{found->second};
            if (schedule.status != LeaderStatus::OK) continue;
            if (const auto index{schedule.set->Leader(at, schedule.seed)}) missed.emplace_back(at, schedule.set->Validators()[*index].pubkey);
        }
        m_staking.NoteLiveness(out, slot, leader.pubkey, missed);
        fast.last_pos_slot = slot;

        // Proofs that leaders signed twice: punished, and the block paid for them.
        PosSections sections;
        if (!PosSections::Decode(block.vSections, sections)) return Fail(reject_reason, "bad-pos-sections");
        for (const DoubleProposal& proof : sections.evidence) {
            const auto offence{CheckChainEvidence(proof, *prev, slot, state.View(), fast.anchors, reject_reason)};
            if (!offence) return false;
            // The offender carrying the proof against itself is paid nothing for it.
            const std::optional<CScript> reporter{offence->pubkey == leader.pubkey ? std::nullopt : std::optional<CScript>{coinbase.vout.at(0).scriptPubKey}};
            if (!m_staking.Punish(out, *offence, reporter, fast.anchors, reject_reason)) return false;
            LogDebug(BCLog::VALIDATION, "FastBlocks: validator %s punished for signing two blocks at slot %d\n", HexStr(offence->pubkey), offence->slot);
        }
        if (sections.ack) NoteAck(block.GetHash(), *sections.ack);
    } else {
        // A BMM block carries transactions only when the PoS blocks stalled: no active validator set
        // for its epoch, or, as of the mainchain block before its commitment, none for stall_seconds.
        if (block.vtx.size() > 1) {
            const auto set{SetFor(*prev, EpochOf(slot, m_params), &state.View())};
            const bool pos_active{!set || set->Active()};
            // (A BMM block's commitment is on record, so is the block before.)
            const auto main{step && chainman->m_mainchain ? chainman->m_mainchain->GetBlock(step->main_height) : std::nullopt};
            if (pos_active && (!main || !Stalled(fast, main->time, pos_active))) return Fail(reject_reason, "bad-pos-bmm-not-stalled");
        }
        // Proofs in its coinbase, as in a block of stake: punished, and its payee (the coinbase's first
        // output, which the pot is paid to) paid for them at the next peg step.
        for (const CTxOut& txout : coinbase.vout) {
            if (!IsEvidenceScript(txout.scriptPubKey)) continue;
            const auto proof{ParseEvidenceScript(txout.scriptPubKey)};
            if (!proof) return Fail(reject_reason, "bad-pos-evidence-output");
            const auto offence{CheckChainEvidence(*proof, *prev, slot, state.View(), fast.anchors, reject_reason)};
            if (!offence) return false;
            if (!m_staking.Punish(out, *offence, coinbase.vout.at(0).scriptPubKey, fast.anchors, reject_reason)) return false;
            LogDebug(BCLog::VALIDATION, "FastBlocks: validator %s punished for signing two blocks at slot %d (proof in a BMM block)\n", HexStr(offence->pubkey), offence->slot);
        }
        // It anchors everything below it.
        fast.first_unanchored_slot = -1;
    }
    FAST_STATE.Put(out, fast);
    return true;
}

std::shared_ptr<const ValidatorSet> FastBlocksHooks::AnchorSetOf(const uint256& anchor, const sidechain::StoreView& view, uint64_t* set_anchors, bool from_state) const
{
    // From memory only for a header (the active chain's state is no state of its branch): the rules
    // of a block read the state of its chain, so that what a node holds in memory -- a set the state
    // pruned since, or one an earlier record made (CheckSeeds) -- does not change the outcome.
    if (!from_state) {
        LOCK(m_sets_mutex);
        if (const auto found{m_known_sets.find(anchor)}; found != m_known_sets.end()) {
            if (set_anchors) *set_anchors = found->second.anchors;
            return found->second.set;
        }
    }
    const auto entry{GetAnchorSet(view, anchor)};
    if (!entry) return nullptr;
    const auto validators{GetSetVersion(view, entry->version)};
    if (!validators) return nullptr;
    auto set{std::make_shared<const ValidatorSet>(*validators, m_params)};
    if (set_anchors) *set_anchors = entry->anchors;
    // Kept in memory, for the headers of branches that are not active (bounded: the oldest go first).
    LOCK(m_sets_mutex);
    if (const auto [found, inserted]{m_known_sets.insert_or_assign(anchor, KnownSet{.set = set, .anchors = entry->anchors})}; inserted) {
        m_known_order.push_back(anchor);
        while (m_known_order.size() > MAX_KNOWN_SETS) {
            m_known_sets.erase(m_known_order.front());
            m_known_order.pop_front();
        }
    }
    return set;
}

int64_t FastBlocksHooks::AnchorSlot(const CBlockIndex& anchor) const
{
    int64_t time{anchor.nTime};
    if (const ChainstateManager* chainman{Chainman()}; chainman && chainman->m_mainchain) {
        const sidechain::Mainchain& record{*chainman->m_mainchain};
        const auto height{anchor.m_ext ? std::optional<int>{static_cast<int>(anchor.nNonce)} : record.BmmHeight(anchor.GetBlockHash())};
        if (const auto main{height ? record.GetBlock(*height) : std::nullopt}) time = std::max<int64_t>(time, main->time);
    }
    return SlotOf(time, m_params);
}

std::shared_ptr<const ValidatorSet> FastBlocksHooks::SetFor(const CBlockIndex& prev, int64_t epoch, const sidechain::StoreView* view, uint64_t* set_anchors) const
{
    AssertLockHeld(::cs_main);
    // The newest anchor at or below prev whose slot is before the epoch before: the stake was fixed an
    // epoch before the seed of the epoch exists.
    const int64_t boundary{(epoch - 1) * m_params.slots_per_epoch};
    for (const CBlockIndex* index{&prev}; index; index = index->pprev) {
        if (index->nHeight == 0) {
            if (set_anchors) *set_anchors = 0;
            return std::shared_ptr<const ValidatorSet>{std::shared_ptr<const ValidatorSet>{}, &m_genesis_set};
        }
        const bool anchor{!index->m_ext || index->nNonce != 0};
        if (!anchor || SlotOf(index->nTime, m_params) >= boundary || AnchorSlot(*index) >= boundary) continue;
        if (view) return AnchorSetOf(index->GetBlockHash(), *view, set_anchors, /*from_state=*/true);
        ChainstateManager* chainman{Chainman()};
        if (!chainman) return nullptr;
        return AnchorSetOf(index->GetBlockHash(), chainman->ActiveChainstate().SideCache(), set_anchors, /*from_state=*/false);
    }
    return nullptr;
}

FastBlocksHooks::SlotLeader FastBlocksHooks::LeaderOf(const CBlockIndex& prev, int64_t slot, const sidechain::StoreView* view) const
{
    AssertLockHeld(::cs_main);
    SlotLeader leader;
    const int64_t epoch{EpochOf(slot, m_params)};
    leader.set = SetFor(prev, epoch, view, &leader.set_anchors);
    if (!leader.set) {
        leader.status = LeaderStatus::SET_UNKNOWN;
        return leader;
    }
    if (!leader.set->Active()) {
        leader.status = LeaderStatus::INACTIVE;
        return leader;
    }
    const ChainstateManager* chainman{Chainman()};
    const auto seed{chainman && chainman->m_mainchain ? EpochSeed(epoch, m_params, *chainman->m_mainchain) : std::nullopt};
    if (!seed) {
        leader.status = LeaderStatus::SEED_UNKNOWN;
        return leader;
    }
    leader.seed = *seed;
    const auto index{leader.set->Leader(slot, *seed)};
    if (!index) {
        leader.status = LeaderStatus::INACTIVE;
        return leader;
    }
    leader.pubkey = leader.set->Validators()[*index].pubkey;
    leader.status = LeaderStatus::OK;
    return leader;
}

FastBlocksHooks::SlotLeader FastBlocksHooks::ActiveLeaderOf(int64_t slot) const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman) return {};
    LOCK(::cs_main);
    const CBlockIndex* tip{chainman->ActiveChain().Tip()};
    if (!tip) return {};
    return LeaderOf(*tip, slot);
}

bool FastBlocksHooks::PosActiveNow() const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman) return m_genesis_set.Active();
    const int64_t slot{SlotOf(TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()), m_params)};
    LOCK(::cs_main);
    const CBlockIndex* tip{chainman->ActiveChain().Tip()};
    if (!tip) return m_genesis_set.Active();
    const auto set{SetFor(*tip, EpochOf(slot, m_params))};
    return set && set->Active();
}

std::optional<Offence> FastBlocksHooks::CheckChainEvidence(const DoubleProposal& proof, const CBlockIndex& prev, int64_t slot, const sidechain::StoreView& view,
                                                           uint64_t anchors, std::string& reason) const
{
    AssertLockHeld(::cs_main);
    const auto fail{[&](const char* why) -> std::optional<Offence> {
        reason = why;
        return std::nullopt;
    }};
    if (!CheckEvidenceForm(proof, m_params, reason)) return std::nullopt;
    const int64_t offence_slot{SlotOf(proof.a.nTime, m_params)};
    if (offence_slot > slot) return fail("bad-pos-evidence-late");
    // The leader of the slot on this chain.
    const SlotLeader leader{LeaderOf(prev, offence_slot, &view)};
    switch (leader.status) {
    case LeaderStatus::SEED_UNKNOWN: return fail("pos-seed-unknown");
    case LeaderStatus::SET_UNKNOWN: return fail("bad-pos-evidence-old");
    case LeaderStatus::INACTIVE: return fail("bad-pos-evidence-sig");
    case LeaderStatus::OK: break;
    }
    const auto offence{CheckEvidence(proof, leader.pubkey, m_params, reason)};
    if (!offence) return std::nullopt;
    // Within the window: while the stake it signed with is still bonded or unbonding.
    if (anchors > leader.set_anchors + static_cast<uint64_t>(m_params.evidence_window_anchors)) return fail("bad-pos-evidence-old");
    if (OffencePunished(view, offence->pubkey, offence->slot)) return fail("bad-pos-evidence-known");
    // Slashed once: no proof against a banned validator takes anything more.
    if (const auto record{GetValidator(view, offence->pubkey)}; record && record->tombstoned) return fail("bad-pos-evidence-banned");
    return offence;
}

void FastBlocksHooks::CheckSeeds() const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman || !chainman->m_mainchain) return;
    const sidechain::Mainchain& record{*chainman->m_mainchain};
    const uint256 main_tip{record.TipHash()};
    {
        LOCK(m_sets_mutex);
        if (m_seeds_checked_for == main_tip) return;
        m_seeds_checked_for = main_tip;
    }
    // Whether the record agrees with the leader of a block of stake (the set of its epoch is that of
    // the chain below it, which the record does not change).
    const auto agrees{[&](const CBlockIndex& index) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        const SlotLeader leader{LeaderOf(*index.pprev, SlotOf(index.nTime, m_params))};
        if (leader.status == LeaderStatus::SET_UNKNOWN) return true;
        return leader.status == LeaderStatus::OK && CheckBlockSig(index.GetBlockHeader(), leader.pubkey);
    }};
    CBlockIndex* lowest{nullptr};
    CBlockIndex* moved{nullptr};
    std::vector<CBlockIndex*> again;
    {
        LOCK(::cs_main);
        // The newest blocks of the active chain: a mainchain reorg reaches the seeds of the last epochs
        // only. Their blocks of stake, and the epochs whose seeds they read: their own, and that of the
        // slots they skipped (the leaders they charged for them, which the sets that anchors make
        // after depend on).
        std::vector<CBlockIndex*> blocks;
        std::map<int64_t, std::optional<uint256>> seeds;
        CBlockIndex* index{chainman->ActiveChain().Tip()};
        for (int64_t i{0}; index && index->nHeight > 0 && i < 2 * m_params.slots_per_epoch + m_params.max_unanchored_slots; ++i, index = index->pprev) {
            if (!index->m_ext) continue;
            blocks.push_back(index);
            const int64_t slot{SlotOf(index->nTime, m_params)};
            for (int64_t epoch{EpochOf(std::max<int64_t>(0, slot - m_params.slots_per_epoch), m_params)}; epoch <= EpochOf(slot, m_params); ++epoch) {
                if (!seeds.contains(epoch)) seeds.emplace(epoch, EpochSeed(epoch, m_params, record));
            }
        }
        for (CBlockIndex* block : blocks) {
            if (!agrees(*block)) lowest = block;
        }
        // An epoch whose seed the record moved since the last check: the blocks of stake from its start
        // on are connected again, even if their own leaders did not change, so that their state is the
        // one the record gives now.
        std::optional<int64_t> changed;
        std::vector<CBlockIndex*> failed;
        {
            LOCK(m_sets_mutex);
            for (const auto& [epoch, seed] : seeds) {
                const auto before{m_epoch_seeds.find(epoch)};
                // (A seed not known at the last check, known now, is no change: no block read it before.)
                if (before != m_epoch_seeds.end() && before->second && before->second != seed && (!changed || epoch < *changed)) changed = epoch;
            }
            m_epoch_seeds = seeds;
            for (auto it{m_seed_failed.begin()}; it != m_seed_failed.end();) {
                CBlockIndex* block{chainman->m_blockman.LookupBlockIndex(*it)};
                if (!block || !(block->nStatus & BLOCK_FAILED_VALID)) {
                    it = m_seed_failed.erase(it);
                } else {
                    failed.push_back(block);
                    ++it;
                }
            }
            // The sets anchors made may change with the state: read again.
            if (changed || lowest) {
                m_known_sets.clear();
                m_known_order.clear();
            }
        }
        if (changed) {
            for (CBlockIndex* block : blocks) {
                if (SlotOf(block->nTime, m_params) >= *changed * m_params.slots_per_epoch) moved = block;
            }
        }
        // (Not with m_sets_mutex held: the leaders read the sets.)
        for (CBlockIndex* block : failed) {
            if (block->pprev && agrees(*block)) again.push_back(block);
        }
        if (!again.empty()) {
            LOCK(m_sets_mutex);
            for (CBlockIndex* block : again) m_seed_failed.erase(block->GetBlockHash());
        }
    }
    if (lowest) {
        LogInfo("FastBlocks: the record moved the seed of the epoch of block %s: it and the blocks after it are taken back", lowest->GetBlockHash().ToString());
        WITH_LOCK(m_sets_mutex, m_seed_failed.insert(lowest->GetBlockHash()));
    }
    // From the lower of the two: a block whose leader the record moved stays failed until the record
    // agrees again (when connected again, it fails too: m_seed_failed keeps it); one below it whose
    // seed moved only is checked again at once.
    const bool recheck{moved && (!lowest || moved->nHeight < lowest->nHeight)};
    if (CBlockIndex* start{recheck ? moved : lowest}) {
        if (recheck) LogInfo("FastBlocks: the record moved the seed of the epoch of block %s: it and the blocks after it are checked again", moved->GetBlockHash().ToString());
        BlockValidationState state;
        chainman->ActiveChainstate().InvalidateBlock(state, start, /*by_record=*/true);
        if (recheck) WITH_LOCK(::cs_main, chainman->ActiveChainstate().ReconsiderRecordFailure(start));
    }
    if (!again.empty()) {
        LOCK(::cs_main);
        for (CBlockIndex* block : again) {
            if (chainman->ActiveChainstate().ReconsiderRecordFailure(block)) LogInfo("FastBlocks: the record gives the seed of block %s back", block->GetBlockHash().ToString());
        }
    }
    if (lowest || recheck || !again.empty()) {
        BlockValidationState state;
        chainman->ActiveChainstate().ActivateBestChain(state);
    }
}

const CBlockIndex* FastBlocksHooks::TicketTarget(const CBlockIndex& tip, const FastState& fast, int64_t slot) const
{
    AssertLockHeld(::cs_main);
    // The block just before the newest (ticket depth 1), unless that is before the last anchor point.
    const CBlockIndex* target{tip.pprev ? tip.pprev : &tip};
    if (target->nHeight < fast.anchor_point_height) return &tip;
    // A ticket for it that could not be acknowledged any more (blocks of stake stopped): the tip, which
    // no block will follow before an anchor covers it.
    if (!InTime(tip, *target, slot + 1)) return &tip;
    return target;
}

bool FastBlocksHooks::InTime(const CBlockIndex& tip, const CBlockIndex& target, int64_t slot) const
{
    int64_t first{slot};
    for (const CBlockIndex* index{&tip}; index && index->nHeight > target.nHeight; index = index->pprev) {
        if (index->m_ext) first = SlotOf(index->nTime, m_params);
    }
    return slot - first < m_params.max_unanchored_slots;
}

bool FastBlocksHooks::Acknowledgeable(const AnchorTicket& ticket, const CBlockIndex& tip, const FastState& fast, int64_t slot) const
{
    AssertLockHeld(::cs_main);
    ChainstateManager* chainman{Chainman()};
    if (!chainman || ticket.version != 1 || !IsValidPayee(ticket.payee)) return false;
    // On this chain (at or below the tip: the parent of the block that would carry it).
    const CBlockIndex* target{chainman->m_blockman.LookupBlockIndex(ticket.anchored)};
    if (!target || !Descends(&tip, target)) return false;
    // At or after the block that took the last peg actions (the state checks it: bad-pos-ack-chain).
    if (fast.anchor_point_height >= 0) {
        const CBlockIndex* point{AncestorAt(target, fast.anchor_point_height)};
        if (!point || point->GetBlockHash() != fast.anchor_point) return false;
    }
    // Not so old that the blocks of stake above it would have been uncovered too long (the state
    // checks it: bad-pos-unanchored).
    return InTime(tip, *target, slot);
}

std::optional<AnchorAck> FastBlocksHooks::NextAck(const CBlockIndex& tip, const FastState& fast, int64_t slot, const std::set<uint256>* skip) const
{
    AssertLockHeld(::cs_main);
    ChainstateManager* chainman{Chainman()};
    if (!chainman || !chainman->m_mainchain) return std::nullopt;
    const sidechain::Mainchain& record{*chainman->m_mainchain};
    std::optional<AnchorAck> best;
    for (const AnchorTicket& ticket : Tickets()) {
        if (skip && skip->contains(ticket.Hash())) continue;
        const auto committed{record.CommittedHeight(ticket.Hash())};
        if (!committed || *committed <= fast.last_main_height) continue;
        if (best && best->main_height <= *committed) continue;
        if (!Acknowledgeable(ticket, tip, fast, slot)) continue;
        // A block that acknowledged it failed when the record lost the commitment, and the follower is
        // about to give it back now that the record has it again: acknowledging it here too would only
        // fork the chain (both branches with the same anchor; the longer wins).
        const auto acked{BlocksCommittedBy(ticket.Hash())};
        if (std::any_of(acked.begin() + 1, acked.end(), [&](const uint256& hash) {
                const CBlockIndex* index{chainman->m_blockman.LookupBlockIndex(hash)};
                return index && (index->nStatus & BLOCK_FAILED_VALID) && record.GetFailure(hash) == sidechain::Mainchain::Failure::RECORD;
            })) {
            continue;
        }
        best = AnchorAck{.ticket = ticket, .main_height = *committed};
    }
    return best;
}

bool FastBlocksHooks::AddTicket(const AnchorTicket& ticket, bool own) const
{
    const uint256 hash{ticket.Hash()};
    const ChainstateManager* chainman{Chainman()};
    const sidechain::Mainchain* record{chainman ? chainman->m_mainchain.get() : nullptr};
    LOCK(m_tickets_mutex);
    if (own) m_own_tickets.insert(hash);
    if (!m_tickets.emplace(hash, ticket).second) return false;
    m_ticket_order.push_back(hash);
    // Bounded: the oldest go first, but not this node's own nor those the mainchain committed to, so
    // that a peer sending tickets by the thousand cannot push out the ones that count. (Tickets
    // committed to long ago go too, with the oldest, once there are enough of them.)
    for (size_t i{0}; m_ticket_order.size() > MAX_TICKETS && i < m_ticket_order.size();) {
        const uint256 candidate{m_ticket_order[i]};
        const bool keep{m_ticket_order.size() <= 2 * MAX_TICKETS && (m_own_tickets.contains(candidate) || (record && record->CommittedHeight(candidate)))};
        if (keep) {
            ++i;
            continue;
        }
        m_tickets.erase(candidate);
        m_own_tickets.erase(candidate);
        m_ticket_order.erase(m_ticket_order.begin() + i);
    }
    return true;
}

bool FastBlocksHooks::TicketAllowed(int64_t peer) const
{
    const ChainstateManager* chainman{Chainman()};
    const uint256 main_tip{chainman && chainman->m_mainchain ? chainman->m_mainchain->TipHash() : uint256{}};
    LOCK(m_tickets_mutex);
    // Counted per mainchain block: all counts start again with a new one.
    if (m_ticket_counts_for != main_tip) {
        m_ticket_counts.clear();
        m_ticket_counts_for = main_tip;
    }
    size_t& count{m_ticket_counts[peer]};
    if (count >= MAX_PEER_TICKETS) return false;
    ++count;
    return true;
}

std::optional<AnchorTicket> FastBlocksHooks::GetTicket(const uint256& hash) const
{
    LOCK(m_tickets_mutex);
    const auto found{m_tickets.find(hash)};
    if (found == m_tickets.end()) return std::nullopt;
    return found->second;
}

std::vector<AnchorTicket> FastBlocksHooks::Tickets() const
{
    LOCK(m_tickets_mutex);
    std::vector<AnchorTicket> tickets;
    for (const uint256& hash : m_ticket_order) tickets.push_back(m_tickets.at(hash));
    return tickets;
}

void FastBlocksHooks::RelayTicket(const AnchorTicket& ticket, std::optional<int64_t> except) const
{
    if (!m_node || !m_node->peerman) return;
    DataStream s{};
    s << ticket;
    m_node->peerman->BroadcastSidechainMessage(MSG_TICKET, {UCharCast(s.data()), UCharCast(s.data()) + s.size()}, except);
}

bool FastBlocksHooks::ProcessMessage(int64_t peer, const std::string& msg_type, DataStream& recv, std::string& misbehaving) const
{
    ChainstateManager* chainman{Chainman()};
    if (msg_type == MSG_TICKET) {
        AnchorTicket ticket;
        try {
            recv >> ticket;
        } catch (const std::ios_base::failure&) {
            misbehaving = "malformed anchor ticket";
            return true;
        }
        if (ticket.version != 1 || !IsValidPayee(ticket.payee)) return true;
        // Only tickets for a block this node knows, or that the mainchain committed to (a node that is
        // still taking in the chain needs those of the newest anchors): others cannot be acknowledged
        // here anyway, and anyone can make them.
        if (!chainman) return true;
        const bool committed{chainman->m_mainchain && chainman->m_mainchain->CommittedHeight(ticket.Hash())};
        if (!committed) {
            if (!WITH_LOCK(::cs_main, return chainman->m_blockman.LookupBlockIndex(ticket.anchored) != nullptr)) return true;
            // Anyone can make tickets for a block, by the thousand, and each new one goes to every
            // peer: a few per peer and mainchain block (bidders make one each), the rest dropped.
            // Those the mainchain committed to cost a commitment each, and are always taken.
            if (!TicketAllowed(peer)) {
                LogDebug(BCLog::NET, "FastBlocks: anchor ticket from peer=%d dropped: too many for this mainchain block\n", peer);
                return true;
            }
        }
        if (AddTicket(ticket)) RelayTicket(ticket, peer);
        return true;
    }
    if (msg_type == MSG_GETTICKETS) {
        std::vector<uint256> wanted;
        try {
            // (Not more than asked for at most: a longer list is not read.)
            const uint64_t count{ReadCompactSize(recv, /*range_check=*/false)};
            if (count > MAX_TICKETS_ASKED) {
                misbehaving = "too many anchor tickets asked for";
                return true;
            }
            wanted.resize(count);
            for (uint256& hash : wanted) recv >> hash;
        } catch (const std::ios_base::failure&) {
            misbehaving = "malformed request for anchor tickets";
            return true;
        }
        if (!m_node || !m_node->peerman) return true;
        for (const uint256& hash : wanted) {
            const auto ticket{GetTicket(hash)};
            if (!ticket) continue;
            DataStream s{};
            s << *ticket;
            m_node->peerman->SendSidechainMessage(peer, MSG_TICKET, {UCharCast(s.data()), UCharCast(s.data()) + s.size()});
        }
        return true;
    }
    if (msg_type == MSG_EVIDENCE) {
        DoubleProposal proof;
        try {
            recv >> proof;
        } catch (const std::ios_base::failure&) {
            misbehaving = "malformed evidence";
            return true;
        }
        // Relayed only if it is a proof (one this node does not know yet). One that is not (two
        // signature checks for nothing) is the peer's fault: the validator set is the chain's.
        std::string reason;
        if (!AddEvidence(proof, reason, peer) && reason != "pos-evidence-unknown") misbehaving = "invalid evidence: " + reason;
        return true;
    }
    return false;
}

std::optional<Offence> FastBlocksHooks::AddEvidence(const DoubleProposal& proof, std::string& reason, std::optional<int64_t> except) const
{
    if (!CheckEvidenceForm(proof, m_params, reason)) return std::nullopt;
    // The leader of the slot: as the chain below either header says, or the active chain.
    std::optional<Offence> offence;
    bool unknown{false};
    if (ChainstateManager* chainman{Chainman()}) {
        LOCK(::cs_main);
        const int64_t slot{SlotOf(proof.a.nTime, m_params)};
        const std::array<const CBlockIndex*, 3> prevs{chainman->m_blockman.LookupBlockIndex(proof.a.hashPrevBlock), chainman->m_blockman.LookupBlockIndex(proof.b.hashPrevBlock),
                                                      chainman->ActiveChain().Tip()};
        for (const CBlockIndex* prev : prevs) {
            if (!prev) continue;
            const SlotLeader leader{LeaderOf(*prev, slot)};
            if (leader.status == LeaderStatus::SET_UNKNOWN || leader.status == LeaderStatus::SEED_UNKNOWN) unknown = true;
            if (leader.status != LeaderStatus::OK) continue;
            offence = CheckEvidence(proof, leader.pubkey, m_params, reason);
            if (offence) break;
        }
    }
    if (!offence) {
        // Not the peer's fault if this node cannot tell the leader yet.
        reason = unknown ? "pos-evidence-unknown" : "bad-pos-evidence-sig";
        return std::nullopt;
    }
    if (!m_evidence.Add(*offence, proof)) return offence;
    LogWarning("FastBlocks: validator %s signed two blocks for slot %d", HexStr(offence->pubkey), offence->slot);
    m_evidence.MarkRelayed(*offence);
    if (m_node && m_node->peerman) {
        DoubleProposal normal{proof};
        normal.Normalize();
        DataStream s{};
        s << normal;
        m_node->peerman->BroadcastSidechainMessage(MSG_EVIDENCE, {UCharCast(s.data()), UCharCast(s.data()) + s.size()}, except);
    }
    return offence;
}

void FastBlocksHooks::RelayNewEvidence() const
{
    const auto proofs{m_evidence.TakeUnrelayed()};
    if (proofs.empty() || !m_node || !m_node->peerman) return;
    for (const DoubleProposal& proof : proofs) {
        DataStream s{};
        s << proof;
        m_node->peerman->BroadcastSidechainMessage(MSG_EVIDENCE, {UCharCast(s.data()), UCharCast(s.data()) + s.size()});
    }
}

std::vector<uint256> FastBlocksHooks::AskMissingTickets() const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman || !chainman->m_mainchain) return {};
    const sidechain::Mainchain& record{*chainman->m_mainchain};
    const int height{record.Height()};
    const uint256 main_tip{record.TipHash()};
    {
        LOCK(m_anchor_mutex);
        if (m_tickets_asked_for == main_tip) return {};
    }
    std::vector<uint256> missing;
    {
        LOCK(::cs_main);
        const FastState fast{FAST_STATE.Get(chainman->ActiveChainstate().SideState().View())};
        const int from{std::max(fast.last_main_height + 1, height - static_cast<int>(MAX_TICKETS_ASKED) + 1)};
        for (int h{std::max(from, 1)}; h <= height; ++h) {
            const auto bmm{record.BmmAt(h)};
            // A commitment to a block (one known, or waiting for it), or of a ticket known, is no ticket to ask for.
            if (!bmm || chainman->m_blockman.LookupBlockIndex(*bmm) || chainman->m_bmm_waiting.contains(*bmm)) continue;
            if (GetTicket(*bmm)) continue;
            missing.push_back(*bmm);
        }
    }
    {
        LOCK(m_anchor_mutex);
        m_tickets_asked_for = main_tip;
    }
    if (missing.empty() || !m_node || !m_node->peerman) return missing;
    DataStream s{};
    s << missing;
    m_node->peerman->BroadcastSidechainMessage(MSG_GETTICKETS, {UCharCast(s.data()), UCharCast(s.data()) + s.size()});
    LogDebug(BCLog::NET, "FastBlocks: asked the peers for %u anchor ticket(s)\n", missing.size());
    return missing;
}

std::vector<CTxOut> FastBlocksHooks::MergeMinedOutputs(const sidechain::StoreView& view, const CBlockIndex& prev, int64_t time) const
{
    AssertLockHeld(::cs_main);
    // The proofs this node knows that the chain has not punished, one per validator (a validator is
    // slashed once), a few at most.
    std::vector<CTxOut> outputs;
    std::set<uint256> offenders;
    const int64_t slot{SlotOf(time, m_params)};
    const uint64_t anchors{FAST_STATE.Get(view).anchors};
    for (const auto& [offence, proof] : m_evidence.All()) {
        if (outputs.size() >= MAX_MINED_EVIDENCE) break;
        if (offenders.contains(offence.pubkey)) continue;
        std::string reason;
        if (!CheckChainEvidence(proof, prev, slot, view, anchors, reason)) continue;
        offenders.insert(offence.pubkey);
        outputs.emplace_back(0, EvidenceScript(proof));
    }
    return outputs;
}

bool FastBlocksHooks::FollowerRound(sidechain::Follower& follower) const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman || !chainman->m_mainchain) return false;
    // Proofs of leaders that signed twice, found by watching headers; and the tickets of the newest
    // commitments, for a node that does not have them. Blocks of stake whose epoch's seed the record
    // moved are taken back.
    RelayNewEvidence();
    AskMissingTickets();
    CheckSeeds();
    const sidechain::Mainchain& record{*chainman->m_mainchain};
    const sidechain::Follower::Mining mining{follower.GetMining()};
    const int main_height{record.Height()};
    const uint256 main_tip{record.TipHash()};
    const auto main_block{record.GetBlock(main_height)};
    if (!main_block) return false;

    FastState fast;
    const CBlockIndex* tip;
    {
        LOCK(::cs_main);
        tip = chainman->ActiveChain().Tip();
        fast = FAST_STATE.Get(chainman->ActiveChainstate().SideState().View());
    }
    // No PoS blocks for a while (or none at all on this chain, or no active validator set): merged mined
    // blocks, as the template's.
    if (Stalled(fast, main_block->time, PosActiveNow())) return false;
    // Otherwise anchors: a ticket per mainchain block, for the merged miner of this node.
    if (!mining.enabled) return true;
    {
        LOCK(m_anchor_mutex);
        if (m_ticket_requested_for == main_tip) return true;
    }
    // A ticket of the last mainchain block is acknowledged first: the next one must anchor that
    // acknowledgement or a block after it. Until then this node's ticket goes to its peers again, once
    // per mainchain block, in case the leaders lost it. (Not a ticket that cannot be acknowledged any
    // more, as one made before blocks of stake stopped for want of anchors: waiting for it would
    // wait for ever.)
    const int64_t slot{SlotOf(TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()), m_params)};
    if (const auto last{record.BmmAt(main_height)}; last && fast.last_main_height < main_height) {
        const auto ticket{GetTicket(*last)};
        if (ticket && WITH_LOCK(::cs_main, return Acknowledgeable(*ticket, *tip, fast, slot + 1))) {
            LOCK(m_anchor_mutex);
            if (m_ticket_relayed_for != main_tip && WITH_LOCK(m_tickets_mutex, return m_own_tickets.contains(*last))) {
                m_ticket_relayed_for = main_tip;
                RelayTicket(*ticket);
            }
            return true;
        }
    }

    AnchorTicket ticket;
    ticket.anchored = WITH_LOCK(::cs_main, return TicketTarget(*tip, fast, slot)->GetBlockHash());
    ticket.payee = mining.coinbase_script;
    if (!IsValidPayee(ticket.payee)) return true;
    // Bid what the pot will pay, but the operator's share, or what the operator said (for tests).
    const int64_t expected{WITH_LOCK(::cs_main, return PotAccruedAt(chainman->ActiveChainstate().SideState().View(),
                                                                    chainman->m_blockman.LookupBlockIndex(ticket.anchored)->nHeight)) - fast.pot_paid};
    CAmount offer{mining.always ? mining.amount : sidechain::Follower::Offer(expected)};
    if (!mining.always && offer < sidechain::Follower::MIN_OFFER) return true;
    std::string error;
    const auto request{follower.RequestCommitment(ticket.Hash(), main_tip, offer, error)};
    {
        LOCK(m_anchor_mutex);
        m_ticket_requested_for = main_tip;
    }
    if (!request) {
        LogDebug(BCLog::NET, "FastBlocks: no anchor ticket for mainchain block %s: %s\n", main_tip.ToString(), error);
        return true;
    }
    AddTicket(ticket, /*own=*/true);
    RelayTicket(ticket);
    LogInfo("FastBlocks: anchor ticket %s for block %s, %d offered", ticket.Hash().ToString(), ticket.anchored.ToString(), offer);
    return true;
}

void FastBlocksHooks::StartServices(node::NodeContext& node) const
{
    m_node = &node;
    std::string error;
    m_producer = Producer::FromArgs(node, *this, error);
    m_producer_error = error;
    if (!error.empty()) {
        LogError("FastBlocks: no block producer: %s", error);
        return;
    }
    if (m_producer) m_producer->Start();
    // The acknowledgements of the recent blocks of the active chain, so that a commitment the mainchain
    // drops takes the blocks that rest on it with it.
    if (ChainstateManager* chainman{Chainman()}) {
        std::vector<const CBlockIndex*> recent;
        {
            LOCK(::cs_main);
            const CBlockIndex* index{chainman->ActiveChain().Tip()};
            for (int i{0}; index && i < 2 * static_cast<int>(m_params.max_unanchored_slots) + 1000; ++i, index = index->pprev) {
                if (index->m_ext && index->nNonce != 0) recent.push_back(index);
            }
        }
        for (const CBlockIndex* index : recent) CommitmentOf(*index);
    }
}

void FastBlocksHooks::StopServices(node::NodeContext&) const
{
    if (m_producer) m_producer->Stop();
    m_producer.reset();
    m_node = nullptr;
}

FastBlocksHooks* InstalledHooks() { return g_installed; }

void UninstallHooks()
{
    g_installed = nullptr;
    sidechain::SetHooks(nullptr);
}

void InstallHooks(const CChainParams& params)
{
    if (!params.GetConsensus().sidechain.enabled) {
        if (g_installed) UninstallHooks();
        return;
    }
    auto hooks{std::make_unique<FastBlocksHooks>(params)};
    g_installed = hooks.get();
    sidechain::SetHooks(std::move(hooks));
}

} // namespace pos
