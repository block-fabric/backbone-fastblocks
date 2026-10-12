// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_HOOKS_H
#define BITCOIN_POS_HOOKS_H

#include <pos/evidence.h>
#include <pos/header.h>
#include <pos/schedule.h>
#include <sidechain/hooks.h>
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
    const ValidatorSet& Validators() const { return m_validators; }
    const Consensus::PosParams& Params() const { return m_params; }

    bool BlockExtensions() const override { return true; }
    sidechain::BlockKind Kind(const CBlockHeader& header) const override;
    bool CheckHeader(const CBlockHeader& header, const CBlockIndex& prev, const ChainstateManager& chainman, BlockValidationState& state) const override;
    bool CheckBlock(const CBlock& block, BlockValidationState& state) const override;
    bool IsWaitReason(const std::string& reject_reason) const override;
    std::optional<int64_t> WaitingReady(const CBlockHeader& header, const sidechain::Mainchain& record) const override;
    uint256 SectionsHash(std::span<const unsigned char> sections) const override;

    bool CustomChainOrder() const override { return true; }
    arith_uint256 ChainWork(const CBlockIndex* prev, const CBlockHeader& header) const override;

    std::optional<uint256> CommitmentOf(const CBlockIndex& index) const override;
    std::vector<uint256> BlocksCommittedBy(const uint256& commitment) const override;

    bool GetPegStep(const CBlock& block, const sidechain::Mainchain& mainchain, std::optional<sidechain::PegStep>& step, std::string& reject_reason) const override;
    bool RefundsOnlyInPegSteps() const override { return true; }
    bool BeforePegStep(sidechain::State& state, const CBlock& block, int height, const sidechain::PegStep& step, const Consensus::SidechainParams& params,
                       std::vector<CTxOut>& owed_tx, std::string& reject_reason) const override;
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
     * The acknowledgement a PoS block on top of `tip` can carry: of the anchor committed first among
     * those it may acknowledge, if any. Called with cs_main held.
     */
    std::optional<AnchorAck> NextAck(const CBlockIndex& tip, const FastState& fast) const;
    /** The block a ticket made now would anchor: the block before the tip, or the tip if that one is not after the last anchor. */
    static const CBlockIndex* TicketTarget(const CBlockIndex& tip, const FastState& fast);
    /** Whether a BMM block built now would be in stall mode (and may take transactions). */
    bool Stalled(const FastState& fast, int64_t main_time) const;
    const Producer* GetProducer() const { return m_producer.get(); }
    Producer* GetProducer() { return m_producer.get(); }
    /**
     * The block an anchor on the chain anchors (P): for an acknowledgement, the block its ticket
     * names; for a BMM block, its parent. Nothing for a block that is no anchor. Called with cs_main held.
     */
    const CBlockIndex* AnchoredBy(const CBlockIndex& index) const;
    /** Why the block producer the options ask for could not start; empty if it did, or none was asked for. */
    std::string ProducerError() const { return m_producer_error; }

private:
    /** The acknowledgement in a PoS block, read from its sections, noted by commitment. */
    void NoteAck(const uint256& block_hash, const AnchorAck& ack) const EXCLUSIVE_LOCKS_REQUIRED(!m_acks_mutex);
    /** The acknowledgement a PoS block carries, from memory or else from disk. */
    std::optional<AnchorAck> ReadAck(const CBlockIndex& index) const EXCLUSIVE_LOCKS_REQUIRED(!m_acks_mutex);

    const CChainParams& m_chain_params;
    const Consensus::PosParams& m_params;
    const ValidatorSet m_validators;
    const uint32_t m_pos_bits;
    std::atomic<ChainstateManager*> m_chainman{nullptr};

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
