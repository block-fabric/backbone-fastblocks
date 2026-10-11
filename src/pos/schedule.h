// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_SCHEDULE_H
#define BITCOIN_POS_SCHEDULE_H

#include <pos/params.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace pos {

/**
 * The validators that take part, in the order leaders are picked from: by key (bytes in order).
 * Phase 2 of the design: the fixed set of the chain's parameters. At most max_validators, the
 * highest stakes first (ties to the lower key).
 */
class ValidatorSet
{
public:
    explicit ValidatorSet(const Consensus::PosParams& params);

    const std::vector<Consensus::PosValidator>& Validators() const { return m_validators; }
    int64_t TotalStake() const { return m_total; }
    /** Whether PoS blocks can be made: enough validators. */
    bool Active() const { return m_active; }
    /** The position of a key in the set, if it is in it. */
    std::optional<size_t> Find(const uint256& pubkey) const;

    /**
     * The seed of an epoch. Phase 2: from the epoch and the chain's first block only, so the
     * schedule is known in advance. (Phase 3 takes it from a mainchain block hash.)
     */
    static uint256 Seed(int64_t epoch, const uint256& genesis_hash);
    /** The leader of a slot: picked by stake, with the epoch's seed. Nothing if the set is not active. */
    std::optional<size_t> Leader(int64_t slot, const Consensus::PosParams& params, const uint256& genesis_hash) const;

private:
    std::vector<Consensus::PosValidator> m_validators;
    int64_t m_total{0};
    bool m_active{false};
};

} // namespace pos

#endif // BITCOIN_POS_SCHEDULE_H
