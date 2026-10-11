// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_FORKCHOICE_H
#define BITCOIN_POS_FORKCHOICE_H

#include <arith_uint256.h>

#include <cstdint>

namespace pos {

/**
 * The order of chains (doc/fastblocks.md, "Fork choice"), kept in nChainWork:
 *
 *   anchors · 2^96 + (2^32 − 1 − last_anchor_height) · 2^64 + height
 *
 * More anchors wins (an acknowledged ticket or a BMM block counts one); with as many, the chain
 * whose last anchor the mainchain committed to first; then the longer. Two chains with the same
 * number of anchors and the same last commitment height share that last anchor (the mainchain
 * commits to one hash per slot and block), so comparing heights compares the number of PoS blocks
 * since the block it anchors. The key grows along every chain: a block either adds an anchor or
 * adds one to the height.
 */
struct ChainKey {
    uint64_t anchors{0};
    //! The mainchain height of the commitment of the last anchor; 0 before any.
    uint32_t last_anchor_height{0};
    uint64_t height{0};

    arith_uint256 Work() const;
    static ChainKey FromWork(const arith_uint256& work);
    friend bool operator==(const ChainKey&, const ChainKey&) = default;
};

} // namespace pos

#endif // BITCOIN_POS_FORKCHOICE_H
