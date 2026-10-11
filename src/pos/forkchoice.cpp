// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/forkchoice.h>

namespace pos {

arith_uint256 ChainKey::Work() const
{
    arith_uint256 work{anchors};
    work <<= 32;
    work += arith_uint256{static_cast<uint64_t>(0xFFFFFFFFu - last_anchor_height)};
    work <<= 64;
    work += arith_uint256{height};
    return work;
}

ChainKey ChainKey::FromWork(const arith_uint256& work)
{
    ChainKey key;
    key.height = work.GetLow64();
    key.last_anchor_height = 0xFFFFFFFFu - static_cast<uint32_t>((work >> 64).GetLow64() & 0xFFFFFFFFu);
    key.anchors = (work >> 96).GetLow64();
    return key;
}

} // namespace pos
