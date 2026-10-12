// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_HOOKS_H
#define BITCOIN_POS_HOOKS_H

#include <pos/evidence.h>
#include <pos/header.h>
#include <pos/schedule.h>
#include <pos/staking.h>
#include <sidechain/hooks.h>
#include <kernel/cs_main.h>
#include <sidechain/store.h>
#include <sync.h>
#include <uint256.h>

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

class CBlockIndex;
class CChainParams;
class ChainstateManager;

namespace pos {

class Producer;

//! The P2P message that relays anchor tickets.
inline constexpr const char* MSG_TICKET{"fbticket"};
//! The P2P message that asks peers for the tickets of commitments: a list of h*.
inline constexpr const char* MSG_GETTICKETS{"fbgettickets"};
//! Tickets asked for in one message, at most; and the newest mainchain blocks a node asks for the tickets of.
inline constexpr size_t MAX_TICKETS_ASKED{16};
//! Tickets a node keeps, at most: the oldest go first.
inline constexpr size_t MAX_TICKETS{1000};
//! Acknowledging blocks whose commitment a node keeps in memory, at most (others are read from disk).
inline constexpr size_t MAX_NOTED_ACKS{20000};
//! Tickets the mainchain has not committed to that a node takes from one peer per mainchain block, at most.
inline constexpr size_t MAX_PEER_TICKETS{8};
/**
 * How far ahead of the node's clock a block may be, in seconds, while blocks of stake are on (Bitcoin's
 * two hours otherwise). BMM blocks with times far ahead would push the median time past ahead, and
 * blocks of stake, whose time is that of their slot, must come after it: a bidder who wins a few BMM
 * auctions in a row could stop them for two hours.
 */
inline constexpr int64_t MAX_FUTURE_BLOCK_TIME_POS{10 * 60};

/**
 * What the state of the chain says about anchors, the fee pot and PoS blocks, after a block: one
 * entry of the sidechain store (table byte 0x55), so that the store's journal is its undo.
 */
struct FastState {
    //! Anchors on the chain: acknowledged tickets and BMM blocks.
    uint64_t anchors{0};
    //! The last anchor: the mainchain height of its commitment, and the block it anchors (P).
    int32_t last_main_height{-1};
    uint256 last_anchored;
    int32_t last_anchored_height{-1};
    //! The last block that took the peg actions (the acknowledgement, or the BMM block): the next
    //! ticket must anchor it or a block after it.
    uint256 anchor_point;
    int32_t anchor_point_height{-1};
    //! Coins put in the fee pot by PoS blocks since the chain began, and paid out of it.
    int64_t pot_accrued{0};
    int64_t pot_paid{0};
    //! The slot of the last PoS block, its time, and the slot of the first PoS block no anchor covers.
    int64_t last_pos_slot{-1};
    int64_t first_unanchored_slot{-1};

    SERIALIZE_METHODS(FastState, obj)
    {
        READWRITE(obj.anchors, obj.last_main_height, obj.last_anchored, obj.last_anchored_height, obj.anchor_point, obj.anchor_point_height,
                  obj.pot_accrued, obj.pot_paid, obj.last_pos_slot, obj.first_unanchored_slot);
    }
    friend bool operator==(const FastState&, const FastState&) = default;
};

/** The FastBlocks part of a chain's state. */
FastState GetFastState(const sidechain::StoreView& view);
/** The pot accrued as of the block at `height` on the chain of `view`, at least what was paid. */
int64_t PotAccruedAt(const sidechain::StoreView& view, int32_t height);

/**
 * FastBlocks on the template's hooks (sidechain/hooks.h): PoS blocks signed by the leader of their
 * slot, anchors, the order of chains, the fee pot, the producer and the anchorer.
 * See doc/fastblocks.md.
 */
class FastBlocksHooks : public sidechain::ChainHooks
{
public:
    explicit FastBlocksHooks(const CChainParams& params);
    ~FastBlocksHooks() override;

    /** The chainstate manager the hooks read the block index and the mainchain record from (none to forget it). */
    void Attach(ChainstateManager* chainman) { m_chainman.store(chainman); }
    ChainstateManager* Chainman() const { return m_chainman.load(); }
    /** The validators the chain starts with (PosParams::genesis_validators): the set of its first epochs. */
    const ValidatorSet& GenesisSet() const { return m_genesis_set; }
    const Staking& GetStaking() const { return m_staking; }
    const Consensus::PosParams& Params() const { return m_params; }

    /** What the chain says of the leader of a slot. */
    enum class LeaderStatus { OK, INACTIVE, SET_UNKNOWN, SEED_UNKNOWN };
    struct SlotLeader {
        LeaderStatus status{LeaderStatus::SET_UNKNOWN};
        //! The leader's key (OK only).
        uint256 pubkey;
        //! The validator set of the slot's epoch, and the anchors of the chain at the anchor that made it.
        std::shared_ptr<const ValidatorSet> set;
        uint64_t set_anchors{0};
        uint256 seed;
    };
    /**
     * The validator set of `epoch` for a block on top of `prev`: the one the newest anchor at or below
     * `prev` whose slot is before the epoch before `epoch` made (none: the genesis set). nullptr if this
     * node does not know it: the anchor is on a branch whose blocks it has not connected. `view`: the
     * state to read the sets of anchors from, the active chain's if none. Called with cs_main held.
     */
    std::shared_ptr<const ValidatorSet> SetFor(const CBlockIndex& prev, int64_t epoch, const sidechain::StoreView* view = nullptr, uint64_t* set_anchors = nullptr) const
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_sets_mutex);
    /** The leader of `slot` for a block on top of `prev`: its epoch's set, and seed (from the record). Called with cs_main held. */
    SlotLeader LeaderOf(const CBlockIndex& prev, int64_t slot, const sidechain::StoreView* view = nullptr) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_sets_mutex);
    /** The leader of `slot` on the active chain's tip. */
    SlotLeader ActiveLeaderOf(int64_t slot) const EXCLUSIVE_LOCKS_REQUIRED(!m_sets_mutex);
    /** Whether blocks of stake can be made now: the active chain has an active set for the current epoch. */
    bool PosActiveNow() const EXCLUSIVE_LOCKS_REQUIRED(!m_sets_mutex);
    /**
     * Check a proof that a block on top of `prev`, of `slot`, carries: the leader of the proof's slot
     * on this chain signed both headers, within the window, and was not punished for it before.
     * Reasons: those of CheckEvidence; bad-pos-evidence-late (a slot after the block's),
     * bad-pos-evidence-old (more than evidence_window_anchors anchors since the set of its epoch was
     * made, or a set the state no longer has), bad-pos-evidence-known, pos-seed-unknown. Called with
     * cs_main held. `anchors`: those of the chain with the block.
     */
    std::optional<Offence> CheckChainEvidence(const DoubleProposal& proof, const CBlockIndex& prev, int64_t slot, const sidechain::StoreView& view, uint64_t anchors,
                                              std::string& reason) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_sets_mutex);

    bool BlockExtensions() const override { return true; }
    sidechain::BlockKind Kind(const CBlockHeader& header) const override;
    bool CheckHeader(const CBlockHeader& header, const CBlockIndex& prev, const ChainstateManager& chainman, BlockValidationState& state) const override;
    bool CheckBlock(const CBlock& block, BlockValidationState& state) const override;
    bool IsWaitReason(const std::string& reject_reason) const override;
    std::optional<int64_t> WaitingReady(const CBlockHeader& header, const sidechain::Mainchain& record) const override;
    uint256 SectionsHash(std::span<const unsigned char> sections) const override;
    int64_t MaxFutureBlockTime() const override;

    bool CustomChainOrder() const override { return true; }
    arith_uint256 ChainWork(const CBlockIndex* prev, const CBlockHeader& header) const override;

    std::optional<uint256> CommitmentOf(const CBlockIndex& index) const override;
    std::vector<uint256> BlocksCommittedBy(const uint256& commitment) const override;

    bool GetPegStep(const CBlock& block, const sidechain::Mainchain& mainchain, std::optional<sidechain::PegStep>& step, std::string& reject_reason) const override;
    bool RefundsOnlyInPegSteps() const override { return true; }
    bool BeforePegStep(sidechain::State& state, const CBlock& block, int height, const sidechain::PegStep& step, const Consensus::SidechainParams& params,
                       std::vector<CTxOut>& owed_tx, std::string& reject_reason) const override;
    bool ApplyTx(sidechain::State& state, const CTransaction& tx, int height, bool peg_step, std::vector<CTxOut>& payouts, std::string& reject_reason) const override;
    bool AfterBlock(sidechain::State& state, const CBlock& block, int height, const std::optional<sidechain::PegStep>& step, CAmount minted,
                    const Consensus::SidechainParams& params, std::string& reject_reason) const override;

    bool FollowerRound(sidechain::Follower& follower) const override;
    void StartServices(node::NodeContext& node) const override;
    void StopServices(node::NodeContext& node) const override;
    bool ProcessMessage(int64_t peer, const std::string& msg_type, DataStream& recv, std::string& misbehaving) const override;

    //
    // For the producer, the anchorer and RPC.
    //

    /** The slot of the last PoS block at or below `index`, if any. */
    static std::optional<int64_t> LastPosSlot(const CBlockIndex* index, const Consensus::PosParams& params);
    /** Note a ticket (`own`: this node's merged miner made it). @return whether it was new */
    bool AddTicket(const AnchorTicket& ticket, bool own = false) const EXCLUSIVE_LOCKS_REQUIRED(!m_tickets_mutex);
    std::optional<AnchorTicket> GetTicket(const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(!m_tickets_mutex);
    /** Whether a ticket the mainchain has not committed to may be taken from `peer` (and count it): MAX_PEER_TICKETS per mainchain block. */
    bool TicketAllowed(int64_t peer) const EXCLUSIVE_LOCKS_REQUIRED(!m_tickets_mutex);
    std::vector<AnchorTicket> Tickets() const EXCLUSIVE_LOCKS_REQUIRED(!m_tickets_mutex);
    /** Relay a ticket to the peers (but `except`). */
    void RelayTicket(const AnchorTicket& ticket, std::optional<int64_t> except = std::nullopt) const;
    /**
     * Ask the peers for the tickets of the commitments on record after the last acknowledged one
     * that this node has no ticket nor block of (the newest anchors, for a node that just started or
     * synced): once per mainchain block. @return the commitments asked for
     */
    std::vector<uint256> AskMissingTickets() const EXCLUSIVE_LOCKS_REQUIRED(!m_tickets_mutex, !m_anchor_mutex);

    /** The proofs of leaders that signed twice this node knows, and the headers it saw signed. */
    EvidencePool& Evidence() const { return m_evidence; }
    /**
     * Check a proof that a leader signed two blocks for one slot, keep it and relay it (but to `except`).
     * @return the offence; nothing, with `reason` set, if the proof is not one
     */
    std::optional<Offence> AddEvidence(const DoubleProposal& proof, std::string& reason, std::optional<int64_t> except = std::nullopt) const;
    /** Relay the proofs found since the last call (by watching headers). */
    void RelayNewEvidence() const;
    /**
     * The acknowledgement a PoS block of `slot` on top of `tip` can carry: of the anchor committed first
     * among those it may acknowledge (Acknowledgeable), if any. Called with cs_main held.
     */
    std::optional<AnchorAck> NextAck(const CBlockIndex& tip, const FastState& fast, int64_t slot) const;
    /**
     * Whether a PoS block of `slot` on top of `tip` may acknowledge a ticket, as far as the chain tells
     * (not the record): a ticket of version 1 with a payee, for a block on the chain at or after the
     * last anchor point, and not so old that the blocks of stake above it would be uncovered for
     * max_unanchored_slots. Called with cs_main held.
     */
    bool Acknowledgeable(const AnchorTicket& ticket, const CBlockIndex& tip, const FastState& fast, int64_t slot) const;
    /**
     * The block a ticket made at `slot` would anchor: the block before the tip (ticket depth 1), or the
     * tip if that one is not after the last anchor point, or if a ticket for it could not be
     * acknowledged in the next slot (blocks of stake stopped for want of an anchor: nothing is built on
     * the tip until one comes). Called with cs_main held.
     */
    const CBlockIndex* TicketTarget(const CBlockIndex& tip, const FastState& fast, int64_t slot) const;
    /**
     * Whether a BMM block built now would be in stall mode (and may take transactions): no active
     * validator set (`pos_active` false), or no block of stake for stall_seconds by `main_time`.
     */
    bool Stalled(const FastState& fast, int64_t main_time, bool pos_active) const;
    const Producer* GetProducer() const { return m_producer.get(); }
    Producer* GetProducer() { return m_producer.get(); }
    /**
     * The block an anchor on the chain anchors (P): for an acknowledgement, the block its ticket
     * names; for a BMM block, its parent. Nothing for a block that is no anchor. Called with cs_main held.
     */
    const CBlockIndex* AnchoredBy(const CBlockIndex& index) const;
    /**
     * Blocks of stake on the active chain whose leader the record no longer agrees with (a mainchain
     * reorg moved the seed of their epoch): taken back, as blocks that lost their commitment are; and
     * those taken back so whose leader it agrees with again, given back. Once per mainchain tip, in the
     * follower's round.
     */
    void CheckSeeds() const EXCLUSIVE_LOCKS_REQUIRED(!m_sets_mutex);
    /** Why the block producer the options ask for could not start; empty if it did, or none was asked for. */
    std::string ProducerError() const { return m_producer_error; }

private:
    /**
     * Whether a PoS block of `slot` on `tip` that acknowledges an anchor of `target` is in time: the
     * blocks of stake above `target` not uncovered for max_unanchored_slots by then.
     */
    bool InTime(const CBlockIndex& tip, const CBlockIndex& target, int64_t slot) const;
    /** The acknowledgement in a PoS block, read from its sections, noted by commitment. */
    void NoteAck(const uint256& block_hash, const AnchorAck& ack) const EXCLUSIVE_LOCKS_REQUIRED(!m_acks_mutex);
    /** The acknowledgement a PoS block carries, from memory or else from disk. */
    std::optional<AnchorAck> ReadAck(const CBlockIndex& index) const EXCLUSIVE_LOCKS_REQUIRED(!m_acks_mutex);

    /** The set an anchor made, from memory or the state `view` (noted in memory then). */
    std::shared_ptr<const ValidatorSet> AnchorSetOf(const uint256& anchor, const sidechain::StoreView& view, uint64_t* set_anchors) const EXCLUSIVE_LOCKS_REQUIRED(!m_sets_mutex);

    const CChainParams& m_chain_params;
    const Consensus::PosParams& m_params;
    const ValidatorSet m_genesis_set;
    const Staking m_staking;
    const uint32_t m_pos_bits;
    std::atomic<ChainstateManager*> m_chainman{nullptr};
    //! Whether this node has seen an active validator set made from the state (blocks of stake on the network).
    mutable std::atomic<bool> m_staking_seen{false};

    mutable Mutex m_sets_mutex;
    struct KnownSet {
        std::shared_ptr<const ValidatorSet> set;
        uint64_t anchors{0};
    };
    //! The sets anchors made, by anchor, as this node read them from a state: of branches no longer active too.
    mutable std::map<uint256, KnownSet> m_known_sets GUARDED_BY(m_sets_mutex);
    mutable std::deque<uint256> m_known_order GUARDED_BY(m_sets_mutex);
    //! Blocks taken back because the record moved the seed of their epoch.
    mutable std::set<uint256> m_seed_failed GUARDED_BY(m_sets_mutex);
    //! The mainchain tip for which the seeds of the active chain were last checked.
    mutable uint256 m_seeds_checked_for GUARDED_BY(m_sets_mutex);

    mutable Mutex m_acks_mutex;
    //! Commitment (h*) of each acknowledging block seen, and the blocks of each commitment.
    mutable std::map<uint256, uint256> m_ack_of GUARDED_BY(m_acks_mutex);
    //! The block each acknowledging block seen anchors (P).
    mutable std::map<uint256, uint256> m_anchored_by GUARDED_BY(m_acks_mutex);
    mutable std::map<uint256, std::set<uint256>> m_acks_by_commitment GUARDED_BY(m_acks_mutex);
    mutable std::deque<std::pair<uint256, uint256>> m_ack_order GUARDED_BY(m_acks_mutex);

    mutable EvidencePool m_evidence;

    mutable Mutex m_tickets_mutex;
    mutable std::map<uint256, AnchorTicket> m_tickets GUARDED_BY(m_tickets_mutex);
    mutable std::deque<uint256> m_ticket_order GUARDED_BY(m_tickets_mutex);
    mutable std::set<uint256> m_own_tickets GUARDED_BY(m_tickets_mutex);
    //! Tickets taken from each peer that the mainchain had not committed to, since the mainchain block m_ticket_counts_for.
    mutable std::map<int64_t, size_t> m_ticket_counts GUARDED_BY(m_tickets_mutex);
    mutable uint256 m_ticket_counts_for GUARDED_BY(m_tickets_mutex);

    mutable node::NodeContext* m_node{nullptr};
    mutable std::unique_ptr<Producer> m_producer;
    mutable std::string m_producer_error;
    //! The mainchain block for which a ticket was last asked for.
    mutable Mutex m_anchor_mutex;
    mutable uint256 m_ticket_requested_for GUARDED_BY(m_anchor_mutex);
    //! The mainchain block for which this node's committed ticket was last relayed again.
    mutable uint256 m_ticket_relayed_for GUARDED_BY(m_anchor_mutex);
    //! The mainchain block for which missing tickets were last asked for.
    mutable uint256 m_tickets_asked_for GUARDED_BY(m_anchor_mutex);
};

/** The FastBlocks hooks installed, if they are. */
FastBlocksHooks* InstalledHooks();
/** Install the FastBlocks hooks for a chain that is a sidechain (none otherwise). */
void InstallHooks(const CChainParams& params);
/** Put the template's hooks back. */
void UninstallHooks();

} // namespace pos

#endif // BITCOIN_POS_HOOKS_H
