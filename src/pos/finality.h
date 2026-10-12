// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_FINALITY_H
#define BITCOIN_POS_FINALITY_H

#include <kernel/cs_main.h>
#include <sync.h>

#include <cstddef>
#include <cstdint>
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
 *  - anchored: an anchor covers it (it is at or below the block an anchor anchors): undoing it
 *    takes the mainchain's merged miners, not stake;
 *  - final: final_anchors anchors cover it.
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
