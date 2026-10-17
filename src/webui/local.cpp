// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The host side of the pages of this chain of its own (src/webui/app/src/chain/local.ts). The template
// has none: a sidechain made from it adds its RPC methods and host methods here. See doc/webui.md.

#include <webui/local.h>

#include <uint256.h>

namespace webui::local {

std::map<std::string, Bridge::Thread> RpcMethods()
{
    // For example: {"listnames", Bridge::Thread::WORKER}.
    return {};
}

std::set<std::string> KeyedRpcMethods()
{
    // For example: {"listmynames"} (a method that reads with the keys).
    return {};
}

std::map<std::string, confirm::Method> ConfirmedRpcMethods()
{
    // For example: {"registername", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG,
    //                                confirm::Generic("Register a name", "Register", "The wallet registers this name.", {"owner"}, {"fee"}),
    //                                /*fee_unknown=*/true}}.
    return {};
}

void RegisterHost(Bridge& bridge, NodeFn node, guard::ConfirmFn confirm, confirm::KeysFn keys)
{
    (void)bridge;
    (void)node;
    (void)confirm;
    (void)keys;
}

std::vector<std::string> CoinbaseRoles(interfaces::Chain& chain, const uint256& block)
{
    // For example (FastBlocks): {"staked", "bmm"} for a coinbase whose first output is the producer's share
    // and whose second is the fee pot paid to a BMM bidder.
    (void)chain;
    (void)block;
    return {};
}

} // namespace webui::local
