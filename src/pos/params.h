// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_PARAMS_H
#define BITCOIN_POS_PARAMS_H

#include <uint256.h>

#include <cstdint>
#include <vector>

namespace Consensus {

/** A validator of the fixed set a network starts with: its BIP340 x-only key, and its stake (weight). */
struct PosValidator {
    uint256 pubkey;
    int64_t stake{1};
    friend bool operator==(const PosValidator&, const PosValidator&) = default;
};

/**
 * The parameters of FastBlocks' blocks of stake (PoS blocks), next to the merged mined blocks of the
 * sidechain template (BMM blocks). See doc/fastblocks.md. Integers only.
 */
struct PosParams {
    //! Time of slot 0 (the chain's first block); slot s starts at genesis_time + s * slot_seconds.
    int64_t genesis_time{0};
    //! One PoS block per slot at most.
    int64_t slot_seconds{30};
    //! Slots per epoch (the leader seed changes every epoch).
    int64_t slots_per_epoch{120};
    //! PoS blocks are valid only with at least this many validators.
    uint32_t min_active_validators{4};
    //! At most this many validators take part; the rest are ignored (highest stake first, then lower key).
    uint32_t max_validators{100};
    /**
     * A PoS block is valid only within this many slots of the first PoS block that no anchor covers
     * yet: without anchors from the mainchain, PoS blocks stop after so long.
     */
    int64_t max_unanchored_slots{1200};
    /**
     * A BMM block may carry transactions only when the mainchain block before the one that
     * committed to it is at least this much later (median time) than the last PoS block.
     */
    int64_t stall_seconds{1800};
    //! The producer of a PoS block may keep at most this share of its fees, in basis points; the rest goes to the pot.
    int64_t producer_share_bps{5000};
    //! A PoS block may be this far ahead of the node's clock, at most.
    int64_t max_future_seconds{2};
    //! Not consensus: wallets call a block final once this many anchors cover it.
    int final_anchors{6};
    //! The fixed validator set the chain starts with (phase 2 of the design: no bonding yet).
    std::vector<PosValidator> genesis_validators;

    /** Whether PoS blocks can be valid on this network. */
    bool Active() const { return !genesis_validators.empty() && genesis_validators.size() >= min_active_validators; }
};

} // namespace Consensus

#endif // BITCOIN_POS_PARAMS_H
