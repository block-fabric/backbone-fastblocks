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

#include <ios>

namespace pos {

namespace {
//! FastBlocks' tables of the sidechain store (0x50 to 0x5f are FastBlocks'; see sidechain/state.h).
const sidechain::Cell<FastState> FAST_STATE{0x55, FastState{}};
//! The pot accrued as of a block, by height: written by the PoS blocks that add to it, erased once paid.
const sidechain::Table<uint32_t, int64_t> POT_ACCRUED{0x56};

FastBlocksHooks* g_installed{nullptr};

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
      m_validators{params.GetConsensus().pos},
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
    const auto invalid{[&](const char* reason, const char* debug) { return state.Invalid(BlockValidationResult::BLOCK_INVALID_HEADER, reason, debug); }};
    if (!m_validators.Active()) return invalid("bad-pos-inactive", "no blocks of stake on this chain: too few validators");
    if (header.nBits != m_pos_bits) return invalid("bad-pos-bits", "a block of stake has the chain's proof-of-work limit as nBits");
    const int64_t time{header.nTime};
    if (time <= m_params.genesis_time || (time - m_params.genesis_time) % m_params.slot_seconds != 0) {
        return invalid("bad-pos-time", "a block of stake has the time its slot starts");
    }
    const int64_t slot{SlotOf(time, m_params)};
    if (header.Time() > NodeClock::now() + std::chrono::seconds{m_params.max_future_seconds}) {
        return state.Invalid(BlockValidationResult::BLOCK_TIME_FUTURE, "pos-time-future", "the slot of the block of stake has not started");
    }
    // The signature before anything that reads the chain: only a leader makes the node look further.
    const auto leader{m_validators.Leader(slot, m_params, m_chain_params.GetConsensus().hashGenesisBlock)};
    if (!leader || !CheckBlockSig(header, m_validators.Validators()[*leader].pubkey)) {
        return invalid("bad-pos-sig", "a block of stake is signed by the leader of its slot");
    }
    // A second header the leader signed for this slot is the proof that it signed twice, whatever
    // else is wrong with it.
    if (const auto proof{m_evidence.NoteSigned(header, slot)}) {
        std::string reason;
        if (const auto offence{CheckEvidence(*proof, m_validators, m_params, m_chain_params.GetConsensus().hashGenesisBlock, reason)}) {
            if (m_evidence.Add(*offence, *proof)) LogWarning("FastBlocks: validator %s signed two blocks for slot %d", HexStr(offence->pubkey), offence->slot);
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
    if (!IsPosHeader(block)) {
        if (pots) return invalid("bad-pos-pot-not-pos", "only a block of stake puts coins in the fee pot");
        return true;
    }
    if (pots > 1) return invalid("bad-pos-pot-multiple", "one fee pot output at most");
    PosSections sections;
    if (!PosSections::Decode(block.vSections, sections)) return invalid("bad-pos-sections", "the sections of a block of stake do not read");
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
    // acknowledgements that count out of memory. (The header rules turn the others away.)
    const int64_t time{block.nTime};
    if (time > m_params.genesis_time && (time - m_params.genesis_time) % m_params.slot_seconds == 0) {
        const auto leader{m_validators.Leader(SlotOf(time, m_params), m_params, m_chain_params.GetConsensus().hashGenesisBlock)};
        if (leader && CheckBlockSig(block, m_validators.Validators()[*leader].pubkey)) NoteAck(block.GetHash(), ack);
    }
    return true;
}

uint256 FastBlocksHooks::SectionsHash(std::span<const unsigned char> sections) const { return pos::SectionsHash(sections); }

int64_t FastBlocksHooks::MaxFutureBlockTime() const { return m_validators.Active() ? MAX_FUTURE_BLOCK_TIME_POS : ChainHooks::MaxFutureBlockTime(); }

bool FastBlocksHooks::IsWaitReason(const std::string& reject_reason) const
{
    // A BMM block the mainchain has not committed to yet; a block of stake whose slot has not started
    // by this node's clock, or that acknowledges a commitment not on this node's record yet.
    return reject_reason == "bmm-unknown" || reject_reason == "pos-time-future" || reject_reason == "pos-ack-unknown";
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
    // built from the block anchored: its withdrawals, its hash as the nonce.
    step = sidechain::PegStep{.main_height = ack.main_height - 1, .nonce = anchored->GetBlockHash(), .max_withdrawal_height = anchored->nHeight};
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
    if (pay > 0) owed_tx.emplace_back(pay, payee);

    ++fast.anchors;
    fast.last_main_height = main_height;
    fast.last_anchored = anchored;
    fast.last_anchored_height = anchored_height;
    fast.anchor_point = block.GetHash();
    fast.anchor_point_height = height;
    FAST_STATE.Put(out, fast);
    return true;
}

bool FastBlocksHooks::Stalled(const FastState& fast, int64_t main_time) const
{
    if (!m_validators.Active()) return true;
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
    if (IsPosHeader(block)) {
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
        const int64_t slot{SlotOf(block.nTime, m_params)};
        if (step) {
            int64_t first{slot};
            LOCK(::cs_main);
            for (const CBlockIndex* index{chainman->m_blockman.LookupBlockIndex(block.hashPrevBlock)}; index && index->nHeight > fast.last_anchored_height; index = index->pprev) {
                if (index->m_ext) first = SlotOf(index->nTime, m_params);
            }
            fast.first_unanchored_slot = first;
        } else if (fast.first_unanchored_slot < 0) {
            fast.first_unanchored_slot = slot;
        }
        if (slot - fast.first_unanchored_slot >= m_params.max_unanchored_slots) return Fail(reject_reason, "bad-pos-unanchored");
        fast.last_pos_slot = slot;
    } else {
        // A BMM block carries transactions only when the PoS blocks stalled: as of the mainchain block
        // before its commitment, none for stall_seconds.
        if (block.vtx.size() > 1 && m_validators.Active()) {
            // (A BMM block's commitment is on record, so is the block before.)
            const auto main{step && chainman->m_mainchain ? chainman->m_mainchain->GetBlock(step->main_height) : std::nullopt};
            if (!main || !Stalled(fast, main->time)) return Fail(reject_reason, "bad-pos-bmm-not-stalled");
        }
        // It anchors everything below it.
        fast.first_unanchored_slot = -1;
    }
    FAST_STATE.Put(out, fast);
    return true;
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

std::optional<AnchorAck> FastBlocksHooks::NextAck(const CBlockIndex& tip, const FastState& fast, int64_t slot) const
{
    AssertLockHeld(::cs_main);
    ChainstateManager* chainman{Chainman()};
    if (!chainman || !chainman->m_mainchain) return std::nullopt;
    const sidechain::Mainchain& record{*chainman->m_mainchain};
    std::optional<AnchorAck> best;
    for (const AnchorTicket& ticket : Tickets()) {
        const auto committed{record.CommittedHeight(ticket.Hash())};
        if (!committed || *committed <= fast.last_main_height) continue;
        if (best && best->main_height <= *committed) continue;
        if (!Acknowledgeable(ticket, tip, fast, slot)) continue;
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
        if (!AddEvidence(proof, reason, peer)) misbehaving = "invalid evidence: " + reason;
        return true;
    }
    return false;
}

std::optional<Offence> FastBlocksHooks::AddEvidence(const DoubleProposal& proof, std::string& reason, std::optional<int64_t> except) const
{
    const auto offence{CheckEvidence(proof, m_validators, m_params, m_chain_params.GetConsensus().hashGenesisBlock, reason)};
    if (!offence) return std::nullopt;
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

bool FastBlocksHooks::FollowerRound(sidechain::Follower& follower) const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman || !chainman->m_mainchain) return false;
    // Proofs of leaders that signed twice, found by watching headers; and the tickets of the newest
    // commitments, for a node that does not have them.
    RelayNewEvidence();
    AskMissingTickets();
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
    // No PoS blocks for a while (or none at all on this chain): merged mined blocks, as the template's.
    if (Stalled(fast, main_block->time)) return false;
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
