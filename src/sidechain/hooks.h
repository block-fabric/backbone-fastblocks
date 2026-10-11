// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SIDECHAIN_HOOKS_H
#define BITCOIN_SIDECHAIN_HOOKS_H

#include <arith_uint256.h>
#include <consensus/amount.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

class BlockValidationState;
class CBlockIndex;
class ChainstateManager;
class DataStream;
namespace Consensus {
struct Params;
struct SidechainParams;
} // namespace Consensus
namespace node {
struct NodeContext;
} // namespace node

/**
 * Extension points for sidechains whose blocks are not all merged mined: blocks of kinds of their
 * own, a chain order of their own, peg actions in some blocks only, a block producer of their own
 * and messages of their own.
 *
 * Everything here is off unless a sidechain installs hooks of its own (SetHooks) at startup. The
 * defaults are the template's behaviour exactly: every block is merged mined (BlockKind::BMM), takes
 * the peg actions, and the chain with the most work wins. A sidechain that wants more derives from
 * ChainHooks and overrides what it needs.
 */
namespace sidechain {

class Follower;
class Mainchain;
class State;

/** The kinds of blocks a chain can have. */
enum class BlockKind : uint8_t {
    //! A block the mainchain commits to (blind merged mining), with Bitcoin's header: the template's.
    BMM = 0,
    //! A block of a kind of the sidechain's own: its header is checked by ChainHooks::CheckHeader,
    //! and it has no proof of work.
    PLUGIN = 1,
};

/**
 * Where a block takes the peg actions (deposits, bundle outcomes, a new bundle, refunds, payouts):
 * as if it followed the mainchain up to `main_height`, with `nonce` in a bundle it starts.
 */
struct PegStep {
    //! The mainchain block up to which the block acts on what the mainchain did.
    int32_t main_height{0};
    //! The nonce of a bundle the block starts (PendingBundle::nonce): in the template, the block's parent.
    uint256 nonce;
    //! If set, a bundle the block starts takes only withdrawals made at this height or below.
    std::optional<int32_t> max_withdrawal_height;
    friend bool operator==(const PegStep&, const PegStep&) = default;
};

/**
 * The hooks, with the template's behaviour. Each is called where its comment says; the caller holds
 * cs_main wherever the code it replaces did.
 */
class ChainHooks
{
public:
    virtual ~ChainHooks();

    //
    // Blocks and headers
    //

    /** Whether this chain has extended blocks (BLOCK_EXTENSION_BIT): read once, when the hooks are installed. */
    virtual bool BlockExtensions() const { return false; }
    /** The kind of a block, from its header. */
    virtual BlockKind Kind(const CBlockHeader& header) const;
    /**
     * Check the header of a block of a kind of the sidechain's own, in place of the proof of work and
     * merged mining rules (ContextualCheckBlockHeader). A header that may become valid once the node
     * knows more is BLOCK_TIME_FUTURE, with a reason that IsWaitReason accepts. Called with cs_main held.
     */
    virtual bool CheckHeader(const CBlockHeader& header, const CBlockIndex& prev, const ChainstateManager& chainman, BlockValidationState& state) const;
    /** Context free checks of a whole block, after Bitcoin's (CheckBlock). */
    virtual bool CheckBlock(const CBlock& block, BlockValidationState& state) const;
    /** What an extended header's hashSections must be for these sections (not empty). */
    virtual uint256 SectionsHash(std::span<const unsigned char> sections) const;

    //
    // Chain order
    //

    /**
     * Whether ChainWork is the chain order of the sidechain's own rather than proof of work. If so,
     * header synchronisation does not ask for a minimum of work (headers presync), which would mean nothing.
     */
    virtual bool CustomChainOrder() const { return false; }
    /**
     * The nChainWork of a block with this header on top of `prev` (none for the genesis block): the
     * key by which the chain with the most of it is the best one. It must grow along every chain.
     * Used only if CustomChainOrder; otherwise, and by default, the work of the parent plus that of the header.
     */
    virtual arith_uint256 ChainWork(const CBlockIndex* prev, const CBlockHeader& header) const;

    //
    // Headers that wait for the node to know more (the record of the mainchain, say)
    //

    /** Whether a header turned away for `reject_reason` may become valid later, and should wait. */
    virtual bool IsWaitReason(const std::string& reject_reason) const;
    /**
     * Whether a waiting header can be tried again, given the record: if so, a key that orders the
     * headers to try (lowest first, so that each comes after its parent). By default the height of
     * the mainchain block that committed to it.
     */
    virtual std::optional<int64_t> WaitingReady(const CBlockHeader& header, const Mainchain& record) const;

    //
    // Mainchain commitments (the Follower)
    //

    /**
     * The hash the mainchain has to commit to for the block to be valid, if it needs one; by
     * default the block's own. A block that needs none is valid whatever commitments come and go.
     */
    virtual std::optional<uint256> CommitmentOf(const CBlockIndex& index) const;
    /** The blocks whose validity rests on a commitment to `commitment`; by default the block of that hash. */
    virtual std::vector<uint256> BlocksCommittedBy(const uint256& commitment) const;

    //
    // Peg actions (sidechain/state.cpp)
    //

    /**
     * Whether `block` takes the peg actions, and where. False, with the reason, if it cannot be
     * known or the block is invalid. By default every block does, as committed: up to the mainchain
     * block before the one that committed to it, with its parent as the bundle nonce ("bmm-unknown"
     * if no mainchain block on record committed to it).
     */
    virtual bool GetPegStep(const CBlock& block, const Mainchain& mainchain, std::optional<PegStep>& step, std::string& reject_reason) const;
    /** Whether refund requests are valid only in blocks that take the peg actions. */
    virtual bool RefundsOnlyInPegSteps() const { return false; }
    /**
     * Peg actions of the sidechain's own, first in a block that takes them: what it adds to
     * `owed_tx` is paid by the coinbase as refunds are. Block assembly calls it too, with the block
     * as far as it is known then: its parent, version, time and sections (SidechainAssembly), and a
     * coinbase whose only output pays the coinbase script, with no value; no other transactions.
     */
    virtual bool BeforePegStep(State& state, const CBlock& block, int height, const PegStep& step, const Consensus::SidechainParams& params,
                               std::vector<CTxOut>& owed_tx, std::string& reject_reason) const;
    /**
     * Last in every block, after the payouts: what the sidechain's own rules note of the block.
     * `minted` is what the coinbase pays out (its outputs 1 to n: deposits, refunds and the like).
     * Not called by block assembly.
     */
    virtual bool AfterBlock(State& state, const CBlock& block, int height, const std::optional<PegStep>& step, CAmount minted,
                            const Consensus::SidechainParams& params, std::string& reject_reason) const;

    //
    // Block production and services
    //

    /**
     * Called by the Follower each round, once the record is up to date, in place of its own merged
     * mining: true if the sidechain's own producer did what it does, false to let the Follower mine.
     */
    virtual bool FollowerRound(Follower& follower) const;
    /** Start the services of the sidechain's own (a block producer, say), after the Follower started. */
    virtual void StartServices(node::NodeContext& node) const;
    /** Stop them, before the Follower stops. */
    virtual void StopServices(node::NodeContext& node) const;

    //
    // P2P
    //

    /**
     * A message of a type Bitcoin does not know, from `peer`: true if it was handled. Set
     * `misbehaving` to say why the peer should be punished.
     */
    virtual bool ProcessMessage(int64_t peer, const std::string& msg_type, DataStream& recv, std::string& misbehaving) const;
};

/** The hooks this chain runs with: the template's unless a sidechain installed its own. */
const ChainHooks& Hooks();
/**
 * Install hooks, or the template's again with nullptr. At startup, before any block is read and
 * before other threads run: it also turns extended blocks on or off (SetBlockExtensions).
 * @return the hooks that were installed before
 */
std::unique_ptr<ChainHooks> SetHooks(std::unique_ptr<ChainHooks> hooks);

/** Whether a header has the proof of work it needs: none for a block of a kind of the sidechain's own. */
bool CheckHeaderWork(const CBlockHeader& header, const Consensus::Params& params);
/** Whether an extended block's sections are those its header commits to (always so for others). */
bool CheckSections(const CBlock& block);

} // namespace sidechain

#endif // BITCOIN_SIDECHAIN_HOOKS_H
