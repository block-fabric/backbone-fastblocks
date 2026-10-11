// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/schedule.h>

#include <arith_uint256.h>
#include <hash.h>
#include <pos/header.h>

#include <algorithm>

namespace pos {

ValidatorSet::ValidatorSet(const Consensus::PosParams& params)
{
    std::vector<Consensus::PosValidator> all{params.genesis_validators};
    // One entry per key: a key given twice counts once, with the stake given first.
    std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.pubkey < b.pubkey; });
    all.erase(std::unique(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.pubkey == b.pubkey; }), all.end());
    // The largest stakes take part, ties to the lower key.
    std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.stake > b.stake; });
    if (all.size() > params.max_validators) all.resize(params.max_validators);
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.pubkey < b.pubkey; });
    for (const auto& validator : all) {
        if (validator.stake <= 0) continue;
        m_validators.push_back(validator);
        m_total += validator.stake;
    }
    m_active = !m_validators.empty() && m_validators.size() >= params.min_active_validators;
}

std::optional<size_t> ValidatorSet::Find(const uint256& pubkey) const
{
    for (size_t i{0}; i < m_validators.size(); ++i) {
        if (m_validators[i].pubkey == pubkey) return i;
    }
    return std::nullopt;
}

uint256 ValidatorSet::Seed(int64_t epoch, const uint256& genesis_hash)
{
    return (TaggedHash("hpos/seed") << epoch << genesis_hash).GetSHA256();
}

std::optional<size_t> ValidatorSet::Leader(int64_t slot, const Consensus::PosParams& params, const uint256& genesis_hash) const
{
    if (!m_active || m_total <= 0) return std::nullopt;
    const uint256 seed{Seed(EpochOf(slot, params), genesis_hash)};
    const arith_uint256 draw{UintToArith256((TaggedHash("hpos/leader") << seed << slot).GetSHA256())};
    // draw mod total, in integers.
    const arith_uint256 total{static_cast<uint64_t>(m_total)};
    const uint64_t r{(draw - (draw / total) * total).GetLow64()};
    uint64_t cumulative{0};
    for (size_t i{0}; i < m_validators.size(); ++i) {
        cumulative += static_cast<uint64_t>(m_validators[i].stake);
        if (cumulative > r) return i;
    }
    return m_validators.size() - 1; // not reached: r < total
}

} // namespace pos
