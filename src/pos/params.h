// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_PARAMS_H
#define BITCOIN_POS_PARAMS_H

#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <vector>

namespace Consensus {

/** A validator of the fixed set a network starts with: its BIP340 x-only key, and its stake (weight). */
struct PosValidator {
    uint256 pubkey;
    int64_t stake{1};
    SERIALIZE_METHODS(PosValidator, obj) { READWRITE(obj.pubkey, obj.stake); }
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
    int64_t max_unanchored_slots{120};
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
    /**
     * Not consensus: the headers whose signature the node cannot check yet (their epoch's set made by
     * an anchor it has not connected) that a peer may hold until their blocks are connected, and the
     * bytes of their blocks the node stores (apart from the block files) for it. Each connected peer
     * has the whole budget, whatever the others hold. See doc/fastblocks.md, "Headers and blocks the
     * node cannot check yet".
     */
    int64_t unchecked_headers_per_peer{10000};
    int64_t unchecked_block_bytes_per_peer{int64_t{128} << 20};
    /**
     * The validators the chain starts with, a bootstrap (regtest's -posvalidators): in the set until
     * jailed or banned, with their stake as weight on top of what they bond. None on the main and test
     * networks, which start merged mined only, until enough stake is bonded (doc/fastblocks.md, "Staking").
     */
    std::vector<PosValidator> genesis_validators;

    //
    // Staking (doc/fastblocks.md, "Staking"). Amounts in satoshis.
    //

    //! A validator takes part only with at least this much bonded (a genesis validator whatever it bonds).
    int64_t min_stake{100'000'000};
    //! Unbonded coins are paid this many anchors after the unbond (about two days on the main network).
    int64_t unbonding_anchors{2880};
    /**
     * A proof that a validator signed twice is valid in a block whose chain has at most this many
     * anchors more than the one that fixed the validator set of the offence's epoch.
     */
    int64_t evidence_window_anchors{1440};
    /**
     * The leader seed of an epoch is the hash of the highest mainchain block whose median time is at
     * least this long before the epoch starts.
     */
    int64_t seed_lookahead_seconds{1200};
    //! Signing two blocks for one slot costs this share of the stake (bonded and unbonding), in basis points.
    int64_t slash_bps{1000};
    //! The block that carries the proof is paid 1/reporter_share_divisor of the penalty; the rest goes to the pot's reserve.
    int64_t reporter_share_divisor{8};
    //! A validator that made less than this share of the blocks it led in an epoch (basis points) ...
    int64_t liveness_min_bps{5000};
    //! ... having led at least this many slots ...
    int64_t liveness_min_assigned{10};
    //! ... is left out of the validator sets for this many epochs more (jailed; no coins are lost).
    int64_t jail_epochs{1};
    //! Each anchor pays 1/pot_reserve_anchors of the pot's reserve (penalties) with the pot.
    int64_t pot_reserve_anchors{1440};
};

} // namespace Consensus

#endif // BITCOIN_POS_PARAMS_H
