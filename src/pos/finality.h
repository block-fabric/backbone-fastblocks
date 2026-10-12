// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_FINALITY_H
#define BITCOIN_POS_FINALITY_H

#include <kernel/cs_main.h>
#include <sync.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class CBlockIndex;

namespace pos {

class FastBlocksHooks;

/** An anchor on a chain: the block that took the peg actions for it, and the block it anchors. */
struct ChainAnchor {
    //! The acknowledgement, or the BMM block.
    const CBlockIndex* block{nullptr};
    //! The block anchored (P): what the ticket names, or the BMM block's parent.
    const CBlockIndex* anchored{nullptr};
    //! The mainchain height of the commitment.
    uint32_t main_height{0};
};

/**
 * How final a block is (doc/fastblocks.md, "Finality"). Not consensus: what wallets show.
 *  - soft: no anchor covers it yet; only blocks of stake (or a BMM block's own commitment) are on it;
 *  - anchored: an anchor covers it (it is at or below the block an anchor anchors), net of the
 *    commitments the chain skipped (AnchoredHeights): undoing it takes the mainchain's merged miners,
 *    not stake;
 *  - final: final_anchors anchors cover it, net.
 */
enum class FinalityStatus : uint8_t { SOFT, ANCHORED, FINAL };

struct Finality {
    FinalityStatus status{FinalityStatus::SOFT};
    //! Blocks from it to the tip, itself included (its confirmations).
    int depth{0};
    //! Anchors that cover it, counted up to final_anchors.
    int anchors{0};
};

/** "soft", "anchored" or "final". (Inline, with FinalityAt: wallets use them, which do not link the node's code.) */
inline std::string FinalityName(FinalityStatus status)
{
    switch (status) {
    case FinalityStatus::SOFT: return "soft";
    case FinalityStatus::ANCHORED: return "anchored";
    case FinalityStatus::FINAL: return "final";
    }
    return "soft";
}

/**
 * The newest anchors of the chain that ends at `tip`, newest first: at most `count`, and only those
 * above `floor` (an anchor at or below a block cannot cover it). The blocks they anchor come
 * lower and lower. Called with cs_main held.
 */
std::vector<ChainAnchor> RecentAnchors(const FastBlocksHooks& hooks, const CBlockIndex& tip, size_t count, int floor = 0) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

/**
 * The heights of the blocks the anchors of the chain cover, for FinalityAt, net of the commitments the
 * chain skipped. By the order of chains, a chain that leaves a block out wins with as many anchors and
 * an earlier last commitment: every commitment on record this chain did not acknowledge (a ticket of
 * another branch, or one whose ticket nobody showed) may be such a chain's anchor. So a block is
 * covered by k anchors net when every chain that leaves it out (forks below it) can have k fewer than
 * this one at most. Element k-1 is the highest block covered by k (newest first, at most `count`).
 * A skipped commitment counts against the forks that could have it: those at its block's place, for a
 * ticket or BMM block the node knows; every fork, for one it does not (its ticket was kept hidden).
 */
struct SkippedCommitment {
    //! The mainchain height of the commitment.
    uint32_t main_height{0};
    //! Where a chain that has it as an anchor leaves this one, as far as the node knows: any fork
    //! (a commitment whose ticket or block it does not know); at or above a height (the ticket names
    //! a block of this chain there); or exactly at a height (a block of another branch).
    std::optional<int> min_fork;
    std::optional<int> exact_fork;
    /** Whether a chain that leaves this one above `fork` can have it. */
    bool UsableAt(int fork) const { return exact_fork ? fork == *exact_fork : (!min_fork || fork >= *min_fork); }
};
std::vector<int> NetAnchoredHeights(const std::vector<ChainAnchor>& anchors, const std::vector<SkippedCommitment>& skipped, int count);
/** The anchors AnchoredHeights looks at: forks deeper than these are not looked at. */
inline size_t AnchorsWalked(int final_anchors) { return static_cast<size_t>(final_anchors) * 4 + 8; }
/**
 * The commitments on the node's record, after the oldest of `anchors` (those of the chain that ends
 * at `tip`, newest first), that the chain does not use. Called with cs_main held.
 */
std::vector<SkippedCommitment> SkippedCommitments(const FastBlocksHooks& hooks, const CBlockIndex& tip, const std::vector<ChainAnchor>& anchors) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
/** The same for the chain that ends at `tip`, with the commitments on its node's record (AnchorsWalked anchors). Called with cs_main held. */
std::vector<int> AnchoredHeights(const FastBlocksHooks& hooks, const CBlockIndex& tip, int final_anchors) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

/** How final `block` is, on the chain that ends at `tip` (which it must be on). Called with cs_main held. */
Finality FinalityOf(const FastBlocksHooks& hooks, const CBlockIndex& block, const CBlockIndex& tip, int final_anchors) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

/**
 * The same, from the heights of the blocks the newest anchors anchor (newest first, as many as
 * there are up to final_anchors): for wallets, which know a transaction's height and depth.
 */
inline Finality FinalityAt(int height, int depth, const std::vector<int>& anchored_heights, int final_anchors)
{
    Finality finality;
    finality.depth = depth;
    for (const int anchored : anchored_heights) {
        if (anchored < height || finality.anchors >= final_anchors) break;
        ++finality.anchors;
    }
    if (finality.anchors == 0) {
        finality.status = FinalityStatus::SOFT;
    } else if (finality.anchors >= final_anchors) {
        finality.status = FinalityStatus::FINAL;
    } else {
        finality.status = FinalityStatus::ANCHORED;
    }
    return finality;
}

} // namespace pos

#endif // BITCOIN_POS_FINALITY_H
