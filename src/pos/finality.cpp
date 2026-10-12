// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/finality.h>

#include <chain.h>
#include <pos/forkchoice.h>
#include <pos/hooks.h>
#include <validation.h>

#include <algorithm>

namespace pos {

std::vector<ChainAnchor> RecentAnchors(const FastBlocksHooks& hooks, const CBlockIndex& tip, size_t count, int floor)
{
    AssertLockHeld(::cs_main);
    std::vector<ChainAnchor> anchors;
    // Between two anchors there are at most max_unanchored_slots blocks of stake (and BMM blocks are
    // anchors), so the walk is short.
    for (const CBlockIndex* index{&tip}; index && index->nHeight > floor && anchors.size() < count; index = index->pprev) {
        const CBlockIndex* anchored{hooks.AnchoredBy(*index)};
        if (!anchored) continue;
        anchors.push_back({.block = index, .anchored = anchored, .main_height = ChainKey::FromWork(index->nChainWork).last_anchor_height});
    }
    return anchors;
}

Finality FinalityOf(const FastBlocksHooks& hooks, const CBlockIndex& block, const CBlockIndex& tip, int final_anchors)
{
    AssertLockHeld(::cs_main);
    std::vector<int> heights;
    for (const ChainAnchor& anchor : RecentAnchors(hooks, tip, static_cast<size_t>(std::max(final_anchors, 0)), block.nHeight)) {
        heights.push_back(anchor.anchored->nHeight);
    }
    return FinalityAt(block.nHeight, tip.nHeight - block.nHeight + 1, heights, final_anchors);
}

} // namespace pos
