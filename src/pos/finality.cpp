// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/finality.h>

#include <chain.h>
#include <pos/forkchoice.h>
#include <pos/hooks.h>
#include <sidechain/mainchain.h>
#include <validation.h>

#include <algorithm>
#include <limits>
#include <set>

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

std::vector<int> NetAnchoredHeights(const std::vector<ChainAnchor>& anchors, const std::vector<SkippedCommitment>& skipped, int count)
{
    std::vector<int> heights;
    if (anchors.empty() || count <= 0) return heights;
    // The anchors a chain that leaves out the blocks above F can have, against those of this chain:
    // it shares the anchors acknowledged at or below F, and may acknowledge again those that anchor a
    // block at or below F (both have them: they cancel), and every commitment this chain skipped
    // after the last anchor it shares. What this chain has more: the anchors of blocks above F, less
    // the commitments skipped since. (Forks below the oldest acknowledgement walked are not looked at.)
    const int oldest{anchors.back().block->nHeight};
    const auto margin{[&](int fork) -> std::optional<int> {
        if (fork < oldest) return std::nullopt;
        int above{0};
        uint32_t shared_height{0};
        for (const ChainAnchor& anchor : anchors) {
            if (anchor.anchored->nHeight > fork) ++above;
            if (anchor.block->nHeight <= fork) shared_height = std::max(shared_height, anchor.main_height);
        }
        const int after{static_cast<int>(std::count_if(skipped.begin(), skipped.end(), [&](const SkippedCommitment& c) { return c.main_height > shared_height && c.UsableAt(fork); }))};
        return above - after;
    }};
    // The margin of a block at `height`: the least over the forks below it. It changes only where an
    // anchored block or an acknowledgement is, so those are the forks to try.
    const auto net{[&](int height) {
        int least{std::numeric_limits<int>::max()};
        const auto take{[&](int fork) { if (const auto m{margin(fork)}) least = std::min(least, *m); }};
        take(height - 1);
        for (const ChainAnchor& anchor : anchors) {
            if (anchor.block->nHeight - 1 < height - 1) take(anchor.block->nHeight - 1);
        }
        for (const SkippedCommitment& commitment : skipped) {
            if (commitment.exact_fork && *commitment.exact_fork < height - 1) take(*commitment.exact_fork);
        }
        // Deeper than every acknowledgement walked: as many anchors as were walked.
        return least == std::numeric_limits<int>::max() ? static_cast<int>(anchors.size()) : least;
    }};
    std::set<int> candidates;
    for (const ChainAnchor& anchor : anchors) {
        candidates.insert(anchor.anchored->nHeight);
        candidates.insert(anchor.block->nHeight - 1);
    }
    // (A fork at a height adds a candidate just above it.)
    for (const SkippedCommitment& commitment : skipped) {
        if (commitment.exact_fork) candidates.insert(*commitment.exact_fork);
        if (commitment.min_fork) candidates.insert(*commitment.min_fork);
    }
    std::vector<std::pair<int, int>> margins; // height, margin: the margin does not grow with the height
    for (const int height : candidates) margins.emplace_back(height, net(height));
    for (int k{1}; k <= count; ++k) {
        std::optional<int> highest;
        for (const auto& [height, m] : margins) {
            if (m >= k && (!highest || height > *highest)) highest = height;
        }
        if (!highest) break;
        heights.push_back(*highest);
    }
    return heights;
}

std::vector<SkippedCommitment> SkippedCommitments(const FastBlocksHooks& hooks, const CBlockIndex& tip, const std::vector<ChainAnchor>& anchors)
{
    AssertLockHeld(::cs_main);
    std::vector<SkippedCommitment> skipped;
    const ChainstateManager* chainman{hooks.Chainman()};
    if (anchors.empty() || !chainman || !chainman->m_mainchain) return skipped;
    std::set<uint32_t> used;
    for (const ChainAnchor& anchor : anchors) used.insert(anchor.main_height);
    const sidechain::Mainchain& record{*chainman->m_mainchain};
    for (int height{static_cast<int>(anchors.back().main_height) + 1}; height <= record.Height(); ++height) {
        const auto bmm{record.BmmAt(height)};
        if (!bmm || used.contains(static_cast<uint32_t>(height))) continue;
        SkippedCommitment commitment{.main_height = static_cast<uint32_t>(height)};
        // Where a chain that has it leaves this one: at the branch of the block it rests on, if the
        // node knows it (a BMM block, or the block a ticket names); anywhere otherwise.
        const CBlockIndex* rests_on{chainman->m_blockman.LookupBlockIndex(*bmm)};
        if (!rests_on) {
            if (const auto ticket{hooks.GetTicket(*bmm)}) rests_on = chainman->m_blockman.LookupBlockIndex(ticket->anchored);
        }
        if (rests_on && tip.GetAncestor(rests_on->nHeight) == rests_on) {
            commitment.min_fork = rests_on->nHeight;
        } else if (rests_on) {
            commitment.exact_fork = LastCommonAncestor(rests_on, &tip)->nHeight;
        }
        skipped.push_back(commitment);
    }
    return skipped;
}

std::vector<int> AnchoredHeights(const FastBlocksHooks& hooks, const CBlockIndex& tip, int final_anchors)
{
    AssertLockHeld(::cs_main);
    if (final_anchors <= 0) return {};
    const std::vector<ChainAnchor> anchors{RecentAnchors(hooks, tip, AnchorsWalked(final_anchors))};
    return NetAnchoredHeights(anchors, SkippedCommitments(hooks, tip, anchors), final_anchors);
}

Finality FinalityOf(const FastBlocksHooks& hooks, const CBlockIndex& block, const CBlockIndex& tip, int final_anchors)
{
    AssertLockHeld(::cs_main);
    return FinalityAt(block.nHeight, tip.nHeight - block.nHeight + 1, AnchoredHeights(hooks, tip, final_anchors), final_anchors);
}

} // namespace pos
