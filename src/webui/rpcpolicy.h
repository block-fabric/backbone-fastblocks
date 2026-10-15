// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_RPCPOLICY_H
#define BITCOIN_WEBUI_RPCPOLICY_H

#include <webui/bridge.h>

#include <optional>
#include <string>
#include <vector>

namespace webui {

/**
 * The RPC methods the pages call through the bridge (kind "rpc"), and where they run. The page
 * calls no other: whatever it would need beyond them is a host method with rules of its own, and a
 * command typed by the user goes through the console (console.run), which runs any command.
 *
 * src/webui/app/src/test/rpcpolicy.test.ts checks that every method the pages call is listed here.
 */
std::optional<Bridge::Thread> PageRpcPolicy(const std::string& method);

/** The methods of PageRpcPolicy, in order (the tests). */
std::vector<std::string> PageRpcMethods();

/**
 * The wallet RPC methods that wallet.withKeys runs with the keys of an encrypted wallet unlocked for
 * the call alone (deposits, stamps, signing a PSBT of the multisig lounge, proofs of funds).
 */
bool KeyedWalletRpc(const std::string& method);

} // namespace webui

#endif // BITCOIN_WEBUI_RPCPOLICY_H
