// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/rpcpolicy.h>

#include <map>
#include <set>

namespace webui {
namespace {
using enum Bridge::Thread;

// Keep in order; LONG for the methods that may run for long (they never hold up the quick ones).
const std::map<std::string, Bridge::Thread>& Methods()
{
    static const std::map<std::string, Bridge::Thread> methods{
        // The chain and the node.
        {"getbestblockhash", WORKER},
        {"getblock", WORKER},
        {"getblockchaininfo", WORKER},
        {"getblockcount", WORKER},
        {"getblockhash", WORKER},
        {"getblockheader", WORKER},
        {"getmempoolentry", WORKER},
        {"getmempoolinfo", WORKER},
        {"getmininginfo", WORKER},
        {"getnetworkinfo", WORKER},
        {"getpeerinfo", WORKER},
        {"getrawmempool", WORKER},
        {"getrawtransaction", WORKER},
        {"help", WORKER},
        {"listbanned", WORKER},
        {"uptime", WORKER},
        // Mining with the processor (the Mine page).
        {"getgenerate", WORKER},
        {"setgenerate", WORKER},
        // Drivechains (the Sidechains page).
        {"acksidechain", WORKER},
        {"createsidechainproposal", WORKER},
        {"getdrivechaininfo", WORKER},
        {"getsidechainevents", WORKER},
        {"listactivesidechains", WORKER},
        {"listsidechaindeposits", WORKER},
        {"listsidechainproposals", WORKER},
        {"listwithdrawalbundles", WORKER},
        {"removesidechainproposal", WORKER},
        {"setdefaultwithdrawalvote", WORKER},
        {"setwithdrawalvote", WORKER},
        // Addresses, scripts and PSBTs (multisig lounge, proof of funds, crypto tools).
        {"createpsbt", WORKER},
        {"decodepsbt", WORKER},
        {"deriveaddresses", WORKER},
        {"finalizepsbt", WORKER},
        {"getdescriptorinfo", WORKER},
        {"scantxoutset", LONG},
        {"sendrawtransaction", WORKER},
        {"utxoupdatepsbt", WORKER},
        {"validateaddress", WORKER},
        {"verifymessage", WORKER},
        // Wallets: what the pages read, and the unlock "for a while" the user asks for.
        {"getaddressinfo", WORKER},
        {"getbalances", WORKER},
        {"getnewaddress", WORKER},
        {"getwalletinfo", WORKER},
        {"listunspent", WORKER},
        {"listwallets", WORKER},
        {"walletlock", WORKER},
        {"walletpassphrase", WORKER},
        // What needs the keys: run as is by a wallet that needs no unlocking (a locked one answers -13,
        // and the page asks for the passphrase and goes through wallet.withKeys).
        {"createsidechaindeposit", WORKER},
        {"send", WORKER},
        {"signmessage", WORKER},
        {"walletprocesspsbt", WORKER},
    };
    return methods;
}
} // namespace

std::optional<Bridge::Thread> PageRpcPolicy(const std::string& method)
{
    const auto it{Methods().find(method)};
    if (it == Methods().end()) return std::nullopt;
    return it->second;
}

std::vector<std::string> PageRpcMethods()
{
    std::vector<std::string> out;
    for (const auto& [name, thread] : Methods()) out.push_back(name);
    return out;
}

bool KeyedWalletRpc(const std::string& method)
{
    static const std::set<std::string> methods{"createsidechaindeposit", "send", "signmessage", "walletprocesspsbt"};
    return methods.contains(method);
}

} // namespace webui
