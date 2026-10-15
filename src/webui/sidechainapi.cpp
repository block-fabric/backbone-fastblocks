// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/sidechainapi.h>

#include <webui/bridge.h>

#include <chainparams.h>
#include <drivechain/sidechain.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/context.h>
#include <sidechain/follower.h>
#include <sidechain/mainclient.h>
#include <tinyformat.h>
#include <util/fs.h>
#include <util/moneystr.h>
#include <util/string.h>

namespace webui {
namespace {
const UniValue& Param(const UniValue& params, const std::string& key)
{
    return params.isObject() ? params.find_value(key) : NullUniValue;
}

std::string ParamStr(const UniValue& params, const std::string& key)
{
    const UniValue& value{Param(params, key)};
    if (!value.isStr()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: a string is needed", key));
    return value.get_str();
}

const std::vector<std::string> METHODS{"peg.connection", "peg.call", "peg.deposit"};
} // namespace

const std::set<std::string>& SidechainApi::MainchainMethods()
{
    // What the pages read of the mainchain: nothing here changes it, nor spends.
    static const std::set<std::string> methods{
        "getbestblockhash", "getblockchaininfo", "getblockcount", "getdrivechaininfo", "getsidechain",
        "getsidechainevents", "listactivesidechains", "listsidechaindeposits", "listwithdrawalbundles",
        // Its wallet: what it holds, and an address of it for a withdrawal to come to.
        "getbalances", "getnewaddress", "getwalletinfo"};
    return methods;
}

const std::set<std::string>& SidechainApi::MainchainWalletMethods()
{
    static const std::set<std::string> methods{"getbalances", "getnewaddress", "getwalletinfo"};
    return methods;
}

SidechainApi::SidechainApi(NodeFn node) : m_node(std::move(node)) {}

void SidechainApi::Register(Bridge& bridge)
{
    for (const std::string& method : METHODS) {
        // Calls to the mainchain node go over the network, and may wait for it: apart from the quick calls.
        bridge.RegisterHost(method, [this, method](const UniValue& params) { return Call(method, params); },
                            method == "peg.connection" ? Bridge::Thread::WORKER : Bridge::Thread::LONG);
    }
}

UniValue SidechainApi::Call(const std::string& method, const UniValue& params)
{
    // What the page gives is checked first; then the node, its follower and its connection.
    const auto follower{[&]() -> const sidechain::Follower& {
        node::NodeContext* context{m_node().context()};
        if (!context || !context->follower) throw HostError("This chain is not running as a sidechain.");
        return *context->follower;
    }};
    const uint32_t slot{Params().GetConsensus().sidechain.slot};
    const auto call_main{[&](const std::string& m, const UniValue& p, bool wallet) {
        const sidechain::Follower& f{follower()};
        try {
            return f.Client().Call(m, p, wallet);
        } catch (const sidechain::MainClientError& e) {
            throw HostError(e.what());
        }
    }};

    if (method == "peg.connection") {
        const sidechain::Follower& f{follower()};
        const auto status{f.GetStatus()};
        const auto& options{f.Client().GetOptions()};
        UniValue out{UniValue::VOBJ};
        out.pushKV("connected", status.connected);
        if (!status.connected) out.pushKV("error", status.error);
        out.pushKV("node", strprintf("%s:%u", options.host, options.port));
        out.pushKV("slot", static_cast<int64_t>(slot));
        out.pushKV("walletSet", options.wallet_set);
        out.pushKV("wallet", options.wallet);
        out.pushKV("auth", options.credentials.empty() ? "cookie" : "password");
        if (options.credentials.empty()) out.pushKV("cookieFile", fs::PathToString(options.cookie_file));
        return out;
    }
    if (method == "peg.call") {
        const std::string name{ParamStr(params, "method")};
        if (!MainchainMethods().contains(name)) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s is not a method the pages call on the mainchain node", name));
        const UniValue& args{Param(params, "params")};
        if (!args.isNull() && !args.isArray()) throw HostError(BRIDGE_INVALID_REQUEST, "params: an array is needed");
        return call_main(name, args.isNull() ? UniValue{UniValue::VARR} : args, MainchainWalletMethods().contains(name));
    }
    if (method == "peg.deposit") {
        // A deposit address of this sidechain: its slot, and an address of this chain to credit.
        const std::string destination{util::TrimString(ParamStr(params, "destination"))};
        drivechain::DepositAddress deposit_address;
        if (drivechain::ParseDepositAddress(destination, deposit_address) != drivechain::DepositAddressKind::VALID) {
            throw HostError(BRIDGE_INVALID_REQUEST, "This is not a deposit address: get one with \"New deposit address\".");
        }
        if (deposit_address.slot != slot) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("This deposit address is for the sidechain in slot %u, not this one (slot %u).", deposit_address.slot, slot));
        if (!IsValidDestinationString(deposit_address.address)) throw HostError(BRIDGE_INVALID_REQUEST, "The deposit address does not name an address of this chain.");
        const std::string amount_text{util::TrimString(ParamStr(params, "amount"))};
        const auto amount{ParseMoney(amount_text)};
        if (!amount || *amount <= 0) throw HostError(BRIDGE_INVALID_REQUEST, "The amount is a positive number of coins, like 1.5");
        UniValue args{UniValue::VARR};
        args.push_back(static_cast<int64_t>(slot));
        args.push_back(destination);
        args.push_back(FormatMoney(*amount));
        return call_main("createsidechaindeposit", args, /*wallet=*/true);
    }
    throw HostError(BRIDGE_METHOD_NOT_FOUND, "No method " + method);
}

} // namespace webui
