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
    if (const auto last{LastPosSlot(&prev, m_params)}; last && slot <= *last) {
        return invalid("bad-pos-slot", "a block of stake comes in a later slot than the one before it");
    }
    if (header.Time() > NodeClock::now() + std::chrono::seconds{m_params.max_future_seconds}) {
        return state.Invalid(BlockValidationResult::BLOCK_TIME_FUTURE, "pos-time-future", "the slot of the block of stake has not started");
    }
    const auto leader{m_validators.Leader(slot, m_params, m_chain_params.GetConsensus().hashGenesisBlock)};
    if (!leader || !CheckBlockSig(header, m_validators.Validators()[*leader].pubkey)) {
        return invalid("bad-pos-sig", "a block of stake is signed by the leader of its slot");
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
    NoteAck(block.GetHash(), ack);
    return true;
}

uint256 FastBlocksHooks::SectionsHash(std::span<const unsigned char> sections) const { return pos::SectionsHash(sections); }

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
    m_ack_of[block_hash] = commitment;
    m_acks_by_commitment[commitment].insert(block_hash);
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
        CBlock block;
        const bool have{WITH_LOCK(::cs_main, return (index.nStatus & BLOCK_HAVE_DATA) && chainman->m_blockman.ReadBlock(block, index))};
        PosSections sections;
        if (have && PosSections::Decode(block.vSections, sections) && sections.ack) {
            NoteAck(index.GetBlockHash(), *sections.ack);
            return sections.ack->ticket.Hash();
        }
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

const CBlockIndex* FastBlocksHooks::TicketTarget(const CBlockIndex& tip, const FastState& fast)
{
    // The block just before the newest (ticket depth 1), unless that is before the last anchor point.
    const CBlockIndex* target{tip.pprev ? tip.pprev : &tip};
    if (target->nHeight < fast.anchor_point_height) target = &tip;
    return target;
}

std::optional<AnchorAck> FastBlocksHooks::NextAck(const CBlockIndex& tip, const FastState& fast) const
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
        if (ticket.version != 1 || !IsValidPayee(ticket.payee)) continue;
        const CBlockIndex* target{chainman->m_blockman.LookupBlockIndex(ticket.anchored)};
        if (!target || !Descends(&tip, target)) continue;
        if (fast.anchor_point_height >= 0) {
            const CBlockIndex* point{AncestorAt(target, fast.anchor_point_height)};
            if (!point || point->GetBlockHash() != fast.anchor_point) continue;
        }
        best = AnchorAck{.ticket = ticket, .main_height = *committed};
    }
    return best;
}

bool FastBlocksHooks::AddTicket(const AnchorTicket& ticket) const
{
    const uint256 hash{ticket.Hash()};
    LOCK(m_tickets_mutex);
    if (!m_tickets.emplace(hash, ticket).second) return false;
    m_ticket_order.push_back(hash);
    while (m_ticket_order.size() > MAX_TICKETS) {
        m_tickets.erase(m_ticket_order.front());
        m_ticket_order.pop_front();
    }
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
    if (msg_type != MSG_TICKET) return false;
    AnchorTicket ticket;
    try {
        recv >> ticket;
    } catch (const std::ios_base::failure&) {
        misbehaving = "malformed anchor ticket";
        return true;
    }
    if (ticket.version != 1 || !IsValidPayee(ticket.payee)) return true;
    // Only tickets for a block this node knows: others cannot be acknowledged here anyway.
    ChainstateManager* chainman{Chainman()};
    if (!chainman || !WITH_LOCK(::cs_main, return chainman->m_blockman.LookupBlockIndex(ticket.anchored) != nullptr)) return true;
    if (AddTicket(ticket)) RelayTicket(ticket, peer);
    return true;
}

bool FastBlocksHooks::FollowerRound(sidechain::Follower& follower) const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman || !chainman->m_mainchain) return false;
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
    // acknowledgement or a block after it.
    if (const auto last{record.BmmAt(main_height)}; last && fast.last_main_height < main_height && GetTicket(*last)) return true;

    AnchorTicket ticket;
    ticket.anchored = WITH_LOCK(::cs_main, return TicketTarget(*tip, fast)->GetBlockHash());
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
    AddTicket(ticket);
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
