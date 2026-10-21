// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The host side of the pages of FastBlocks (src/webui/app/src/chain/local.ts): the RPC methods of its blocks of
// stake the pages read (src/rpc/pos.cpp, src/wallet/rpc/sidechain.cpp), and what they do that must not happen on
// the page's word alone: bonding and unbonding coins of the wallet, turning the making of blocks of stake off,
// signing a proof of possession with a validator key. Each of these asks the desktop first (Window::Confirm,
// outside the page), with the key and the amount in full, and only then calls the node.

#include <webui/local.h>

#include <addresstype.h>
#include <chainparams.h>
#include <common/args.h>
#include <consensus/amount.h>
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <node/context.h>
#include <pos/hooks.h>
#include <rpc/util.h>
#include <tinyformat.h>
#include <uint256.h>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <webui/chainconfig.h>
#include <webui/walletapi.h>

#include <memory>
#include <optional>
#include <string>

namespace webui::local {

std::map<std::string, Bridge::Thread> RpcMethods()
{
    return {
        // What the node says of the blocks of stake: read only.
        {"getposinfo", Bridge::Thread::WORKER},
        {"getstakinginfo", Bridge::Thread::WORKER},
        {"getvalidators", Bridge::Thread::WORKER},
        {"getvalidator", Bridge::Thread::WORKER},
        {"getleaderschedule", Bridge::Thread::WORKER},
        {"getanchorinfo", Bridge::Thread::WORKER},
        {"listevidence", Bridge::Thread::WORKER},
        // A proof that a leader signed two blocks for a slot, given by the user: the node checks it, keeps it and
        // relays it (it moves no coins; a wrong one is refused).
        {"submitevidence", Bridge::Thread::WORKER},
        // The bonds of the wallet (and, with all, of every validator): read only.
        {"listbonds", Bridge::Thread::WORKER},
        // Not createbond, createunbond, setstaking or signbondpop: fastblocks.bond, fastblocks.unbond,
        // fastblocks.staking and fastblocks.pop ask the desktop first (below).
    };
}

std::set<std::string> KeyedRpcMethods()
{
    // None: the bonds go through the host methods below, which unlock the wallet for their call alone.
    return {};
}

namespace {
const UniValue& Param(const UniValue& params, const std::string& key)
{
    return params.isObject() ? params.find_value(key) : NullUniValue;
}

std::string Text(const UniValue& params, const std::string& key, size_t max = 1024)
{
    const UniValue& value{Param(params, key)};
    if (!value.isStr() || value.get_str().size() > max) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: a text", key));
    return value.get_str();
}

/** A validator key as the RPCs take it: an x-only key, 64 hex digits (lower case). */
std::string ValidatorKey(const UniValue& params)
{
    std::string key{Text(params, "pubkey", 64)};
    if (key.size() != 64 || !IsHex(key)) throw HostError(BRIDGE_INVALID_REQUEST, "A validator key is an x-only key: 64 hexadecimal digits.");
    for (char& c : key) c = ToLower(c);
    return key;
}

/** An amount of coins the user typed, as the RPCs take it ("1.5"): positive, at most 8 decimals. */
CAmount Coins(const std::string& text)
{
    const auto amount{ParseMoney(text)};
    if (!amount || *amount <= 0 || !MoneyRange(*amount)) throw HostError(BRIDGE_INVALID_REQUEST, "Enter the amount as a positive number, like 1.5.");
    return *amount;
}

std::string Shown(CAmount amount)
{
    return strprintf("%s %s", FormatMoney(amount), std::string{chain::TICKER});
}

/** The key, cut in groups of eight for a dialog: read whole, not taken for another by its ends. */
std::string Grouped(const std::string& key)
{
    std::string out;
    for (size_t i{0}; i < key.size(); i += 8) {
        if (i) out += i == 32 ? "\n" : " ";
        out += key.substr(i, 8);
    }
    return out;
}

/** A wallet that is loaded, by its name. */
std::unique_ptr<interfaces::Wallet> WalletNamed(interfaces::Node& node, const std::string& name)
{
    for (auto& wallet : node.walletLoader().getWallets()) {
        if (wallet->getWalletName() == name) return std::move(wallet);
    }
    throw HostError(-18, "This wallet is not loaded");
}

/** The wallet's bond to a validator key (listbonds), if it has one. */
std::optional<UniValue> WalletBond(interfaces::Node& node, const std::string& wallet, const std::string& key)
{
    const UniValue bonds{node.executeRpc("listbonds", UniValue{UniValue::VARR}, WalletUri(wallet))};
    for (const UniValue& bond : bonds.getValues()) {
        if (bond.isObject() && bond.find_value("validator").isStr() && bond.find_value("validator").get_str() == key) return bond;
    }
    return std::nullopt;
}

CAmount AmountOf(const UniValue& value)
{
    return value.isNum() ? AmountFromValue(value) : 0;
}

/** How many anchors an unbond waits (getposinfo staking.unbondinganchors). */
int UnbondingAnchors(interfaces::Node& node)
{
    const UniValue info{node.executeRpc("getposinfo", UniValue{UniValue::VARR}, "/")};
    const UniValue& staking{info.find_value("staking")};
    const UniValue& anchors{staking.isObject() ? staking.find_value("unbondinganchors") : NullUniValue};
    return anchors.isNum() ? anchors.getInt<int>() : 0;
}

//! About how long `anchors` BMM commitments take: one a mainchain block, ten minutes on the eCash networks, a minute on Chains'.
std::string AnchorsTime(int anchors)
{
    const int64_t seconds{int64_t{anchors} * (IsEcashChain(Params().GetChainType()) ? 600 : 60)};
    if (seconds >= 2 * 86400) return strprintf("about %d days", (seconds + 43200) / 86400);
    if (seconds >= 2 * 3600) return strprintf("about %d hours", (seconds + 1800) / 3600);
    return strprintf("about %d minutes", (seconds + 30) / 60);
}

void Ask(const guard::ConfirmFn& confirm, const Window::ConfirmDialog& dialog, const std::string& declined)
{
    if (!confirm || !confirm(dialog)) throw HostError(BRIDGE_CANCELLED, declined);
}

Window::ConfirmDialog BondConfirmation(const std::string& wallet, const std::string& key, CAmount amount, const std::optional<UniValue>& existing, int unbonding_anchors, bool other_node)
{
    Window::ConfirmDialog dialog;
    dialog.kind = "bond";
    // The facts, decoded by the host (audit 7 #1): the key whole, then the amount.
    dialog.rows = confirm::WalletRows(wallet);
    dialog.rows.push_back(confirm::Destination("Validator key", key));
    if (existing) dialog.rows.push_back(confirm::Amount("Bonded now", AmountOf(existing->find_value("bonded"))));
    dialog.rows.push_back(confirm::Amount(existing ? "Added" : "Amount", amount));
    dialog.title = existing ? "Add to a bond" : "Bond coins";
    const std::string what{existing ? strprintf("Add %s to this wallet's bond of %s to the validator key", Shown(amount), Shown(AmountOf(existing->find_value("bonded")))) :
                                      strprintf("Bond %s of this wallet to the validator key", Shown(amount))};
    dialog.message = strprintf("Wallet: %s\n\n%s\n\n%s\n\n"
                               "The coins leave this wallet now. The key takes part in the validator sets from the epoch after next, while its "
                               "bond is at least the minimum stake. Taking them back (unbonding) pays them %d BMM commitments later (%s).%s",
                               guard::OneLine(wallet, 64), what, Grouped(key), unbonding_anchors, AnchorsTime(unbonding_anchors),
                               other_node ? "\n\nThe key is not one of this node's: its own node signed the proof of possession given." : "");
    dialog.accept = existing ? "Add to the bond" : "Bond";
    return dialog;
}

Window::ConfirmDialog UnbondConfirmation(const std::string& wallet, const std::string& key, std::optional<CAmount> amount, const UniValue& bond, int unbonding_anchors)
{
    Window::ConfirmDialog dialog;
    dialog.title = "Unbond";
    dialog.kind = "unbond";
    const CAmount bonded{AmountOf(bond.find_value("bonded"))};
    dialog.rows = confirm::WalletRows(wallet);
    dialog.rows.push_back(confirm::Destination("Validator key", key));
    if (bond.find_value("owner").isStr()) dialog.rows.push_back(confirm::Destination("Paid back to", bond.find_value("owner").get_str()));
    dialog.rows.push_back(confirm::Amount("Bonded now", bonded));
    if (amount) dialog.rows.push_back(confirm::Amount("Unbonded", *amount));
    else dialog.rows.push_back(confirm::Text("Unbonded", "all of it"));
    const std::string what{amount ? strprintf("Unbond %s of the %s bonded to the validator key", Shown(*amount), Shown(bonded)) :
                                    strprintf("Unbond all of the bond to the validator key (%s now, and whatever is added before the unbond is mined)", Shown(bonded))};
    const UniValue& owner{bond.find_value("owner")};
    dialog.message = strprintf("Wallet: %s\n\n%s\n\n%s\n\n"
                               "The coins leave the validator's stake at once. They are paid back to the bond's owner, %s, %d BMM commitments "
                               "later (%s); until then a proof that the validator signed two blocks for one "
                               "slot can still take a share of them.",
                               guard::OneLine(wallet, 64), what, Grouped(key), owner.isStr() ? guard::OneLine(owner.get_str(), 90) : std::string{"this wallet"}, unbonding_anchors, AnchorsTime(unbonding_anchors));
    dialog.accept = "Unbond";
    // The stake leaves the validator at once and earns nothing until it is paid back (user decision: danger).
    dialog.level = dialog::Level::DANGER;
    return dialog;
}

Window::ConfirmDialog StopStakingConfirmation()
{
    Window::ConfirmDialog dialog;
    dialog.title = "Stop staking";
    dialog.message = "This node stops making blocks of stake at the slots its validator keys lead, until you start it again (or the app "
                     "restarts).\n\nA validator that misses its slots is jailed for a while: left out of the validator sets of the next "
                     "epochs. No coins are lost, but it earns nothing meanwhile.";
    dialog.accept = "Stop staking";
    // A validator that misses its slots is jailed (user decision: danger).
    dialog.level = dialog::Level::DANGER;
    return dialog;
}

Window::ConfirmDialog PopConfirmation(const std::string& owner, const std::string& key)
{
    Window::ConfirmDialog dialog;
    dialog.title = "Sign a proof of possession";
    dialog.message = strprintf("Sign, with this node's validator key\n\n%s\n\nthe proof a bond owned by the address\n\n%s\n\nneeds. "
                               "With it, the wallet that holds that address can bond its coins to this key, and alone take them back. "
                               "Sign it only for an address you trust, such as one of your own wallets on another node.",
                               key.empty() ? std::string{"(its first key)"} : Grouped(key), guard::OneLine(owner, 90));
    dialog.accept = "Sign";
    return dialog;
}
} // namespace

std::map<std::string, confirm::Method> ConfirmedRpcMethods()
{
    // None: bonds are made through fastblocks.bond and fastblocks.unbond, which the host confirms (below).
    return {};
}

void RegisterHost(Bridge& bridge, NodeFn node, guard::ConfirmFn confirm, confirm::KeysFn keys)
{
    // fastblocks.bond {wallet, pubkey, amount, pop?, owner?}: createbond, after the desktop's yes, the passphrase of a
    // locked wallet typed in the same dialog of the host (audit 7 #1). A new bond, or more coins to the wallet's bond
    // (only its owner may add); with pop and owner, to the key of another node, whose proof of possession that node
    // signed (fastblocks.pop there).
    bridge.RegisterHost("fastblocks.bond", [node, keys](const UniValue& params) -> UniValue {
        const std::string wallet_name{Text(params, "wallet")};
        const std::string key{ValidatorKey(params)};
        const std::string amount_text{Text(params, "amount", 32)};
        const CAmount amount{Coins(amount_text)};
        const UniValue& pop{Param(params, "pop")};
        const UniValue& owner{Param(params, "owner")};
        if (!pop.isNull() && (!pop.isStr() || pop.get_str().size() != 128 || !IsHex(pop.get_str()))) throw HostError(BRIDGE_INVALID_REQUEST, "A proof of possession is a signature: 128 hexadecimal digits.");
        if (!owner.isNull() && (!owner.isStr() || owner.get_str().size() > 90)) throw HostError(BRIDGE_INVALID_REQUEST, "owner: an address");
        interfaces::Node& n{node()};
        auto wallet{WalletNamed(n, wallet_name)};
        if (wallet->privateKeysDisabled()) throw HostError(-4, "This wallet has no private keys: it cannot bond.");
        if (!keys) throw HostError(-18, "Wallets are disabled in this node (-disablewallet).");
        const auto existing{WalletBond(n, wallet_name, key)};
        UniValue args{UniValue::VARR};
        args.push_back(key);
        args.push_back(FormatMoney(amount));
        if (!pop.isNull()) {
            args.push_back(pop.get_str());
            if (!owner.isNull()) args.push_back(owner.get_str());
        }
        return keys(wallet_name, BondConfirmation(wallet_name, key, amount, existing, UnbondingAnchors(n), !pop.isNull()), /*always=*/true, /*fee_unknown=*/true,
                    [&] { return n.executeRpc("createbond", args, WalletUri(wallet_name)); });
    }, Bridge::Thread::LONG);

    // fastblocks.unbond {wallet, pubkey, amount ("all" or coins)}: createunbond, after the desktop's yes (the passphrase
    // in the same dialog).
    bridge.RegisterHost("fastblocks.unbond", [node, keys](const UniValue& params) -> UniValue {
        const std::string wallet_name{Text(params, "wallet")};
        const std::string key{ValidatorKey(params)};
        const std::string amount_text{Text(params, "amount", 32)};
        const std::optional<CAmount> amount{amount_text == "all" ? std::nullopt : std::optional{Coins(amount_text)}};
        interfaces::Node& n{node()};
        auto wallet{WalletNamed(n, wallet_name)};
        if (wallet->privateKeysDisabled()) throw HostError(-4, "This wallet has no private keys: it cannot unbond.");
        if (!keys) throw HostError(-18, "Wallets are disabled in this node (-disablewallet).");
        const auto bond{WalletBond(n, wallet_name, key)};
        if (!bond || AmountOf(bond->find_value("bonded")) <= 0) throw HostError(-8, "This wallet has nothing bonded to this validator key.");
        UniValue args{UniValue::VARR};
        args.push_back(key);
        args.push_back(amount ? FormatMoney(*amount) : std::string{"all"});
        return keys(wallet_name, UnbondConfirmation(wallet_name, key, amount, *bond, UnbondingAnchors(n)), /*always=*/true, /*fee_unknown=*/true,
                    [&] { return n.executeRpc("createunbond", args, WalletUri(wallet_name)); });
    }, Bridge::Thread::LONG);

    // fastblocks.staking {enabled}: setstaking; turning it off asks the desktop first (the validator may be jailed).
    bridge.RegisterHost("fastblocks.staking", [node, confirm](const UniValue& params) -> UniValue {
        const UniValue& enabled{Param(params, "enabled")};
        if (!enabled.isBool()) throw HostError(BRIDGE_INVALID_REQUEST, "enabled: true or false");
        if (!enabled.get_bool()) Ask(confirm, StopStakingConfirmation(), "Staking goes on: stopping it was declined.");
        UniValue args{UniValue::VARR};
        args.push_back(enabled.get_bool());
        return node().executeRpc("setstaking", args, "/");
    });

    // fastblocks.pop {owner, pubkey?}: signbondpop, after the desktop's yes.
    bridge.RegisterHost("fastblocks.pop", [node, confirm](const UniValue& params) -> UniValue {
        const std::string owner{Text(params, "owner", 90)};
        const std::string key{Param(params, "pubkey").isNull() ? std::string{} : ValidatorKey(params)};
        Ask(confirm, PopConfirmation(owner, key), "No proof was signed: it was declined.");
        UniValue args{UniValue::VARR};
        args.push_back(owner);
        if (!key.empty()) args.push_back(key);
        return node().executeRpc("signbondpop", args, "/");
    });

    // fastblocks.anchoring {}: what this node bids, as a validator, for a BMM commitment to its ticket in every
    // mainchain block (-validatoranchorbid), and where the pot it wins goes. Information only: no RPC says it.
    bridge.RegisterHost("fastblocks.anchoring", [node](const UniValue&) -> UniValue {
        interfaces::Node& n{node()};
        UniValue out{UniValue::VOBJ};
        const pos::FastBlocksHooks* hooks{pos::InstalledHooks()};
        const auto anchoring{hooks ? hooks->ValidatorAnchoring() : std::nullopt};
        out.pushKV("on", anchoring.has_value());
        if (anchoring) {
            out.pushKV("bid", FormatMoney(anchoring->second));
            CTxDestination dest;
            out.pushKV("payee", ExtractDestination(anchoring->first, dest) ? EncodeDestination(dest) : HexStr(anchoring->first));
        }
        node::NodeContext* context{n.context()};
        const ArgsManager* args{context ? context->args : nullptr};
        out.pushKV("validator", hooks && hooks->GetProducer() != nullptr);
        if (args && args->IsArgSet("-validatoranchorbid")) out.pushKV("option", guard::OneLine(args->GetArg("-validatoranchorbid", ""), 32));
        out.pushKV("defaultbid", FormatMoney(pos::DEFAULT_VALIDATOR_ANCHOR_BID));
        return out;
    });
}

std::vector<std::string> CoinbaseRoles(interfaces::Chain& chain, const uint256& block)
{
    // pos::CoinbaseRole by the words of the page (chain/local.ts COINBASE_ROLES): the producer's share of a block
    // of stake, the pot paid to a BMM bidder, a reporter's reward for a proof.
    std::vector<std::string> roles;
    for (const auto role : chain.getCoinbaseRoles(block)) {
        switch (role) {
        case interfaces::Chain::CoinbaseRole::STAKED: roles.emplace_back("staked"); break;
        case interfaces::Chain::CoinbaseRole::ANCHOR: roles.emplace_back("bmm"); break;
        case interfaces::Chain::CoinbaseRole::REPORTER: roles.emplace_back("slashing"); break;
        case interfaces::Chain::CoinbaseRole::VALIDATOR: roles.emplace_back("validator"); break;
        case interfaces::Chain::CoinbaseRole::OTHER: roles.emplace_back(); break;
        }
    }
    return roles;
}

} // namespace webui::local
