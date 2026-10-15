// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/bridge.h>
#include <webui/chainconfig.h>
#include <webui/rpcpolicy.h>
#include <webui/sidechainapi.h>

#include <chainparams.h>
#include <drivechain/sidechain.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <string>

namespace {
//! A node that is never reached: what the page gives is refused before the node or the mainchain is asked.
interfaces::Node& NoNode()
{
    throw webui::HostError("no node in this test");
}

int CodeOf(const std::function<void()>& fn)
{
    try {
        fn();
    } catch (const webui::HostError& e) {
        return e.code;
    }
    return 0;
}

UniValue Obj(std::initializer_list<std::pair<std::string, UniValue>> fields)
{
    UniValue out{UniValue::VOBJ};
    for (const auto& [k, v] : fields) out.pushKV(k, v);
    return out;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(webui_sidechainapi_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(mainchain_methods_read_only)
{
    // What the pages may ask of the mainchain node: reading, and its wallet's balances and addresses. Never a
    // method that spends (the deposit has peg.deposit, which checks what it pays to), nor one that votes.
    const auto& methods{webui::SidechainApi::MainchainMethods()};
    for (const char* m : {"getdrivechaininfo", "listwithdrawalbundles", "getsidechainevents", "listsidechaindeposits", "getbalances"}) BOOST_CHECK(methods.contains(m));
    for (const char* m : {"createsidechaindeposit", "sendtoaddress", "send", "createbmmrequest", "setwithdrawalvote", "acksidechain", "stop", "walletpassphrase", "dumpprivkey"}) {
        BOOST_CHECK_MESSAGE(!methods.contains(m), m);
    }
    for (const auto& m : webui::SidechainApi::MainchainWalletMethods()) BOOST_CHECK(methods.contains(m));
}

BOOST_AUTO_TEST_CASE(page_input_refused_before_the_node)
{
    webui::SidechainApi api{NoNode};
    // A method not on the list.
    BOOST_CHECK_EQUAL(CodeOf([&] { api.Call("peg.call", Obj({{"method", "sendtoaddress"}, {"params", UniValue{UniValue::VARR}}})); }), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(CodeOf([&] { api.Call("peg.call", Obj({{"method", "getblockcount"}, {"params", "x"}})); }), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(CodeOf([&] { api.Call("peg.nothing", UniValue{UniValue::VOBJ}); }), webui::BRIDGE_METHOD_NOT_FOUND);

    // The deposit helper pays only to a deposit address of this sidechain, with a valid address of this chain in it.
    const uint32_t slot{Params().GetConsensus().sidechain.slot};
    const std::string own{EncodeDestination(WitnessV0KeyHash{uint160{}})};
    const std::string good{drivechain::FormatDepositAddress({slot, own})};
    const std::string other_slot{drivechain::FormatDepositAddress({slot + 1, own})};
    const std::string not_an_address{drivechain::FormatDepositAddress({slot, "hello"})};
    std::string typo{good};
    typo.back() = typo.back() == 'a' ? 'b' : 'a';
    for (const std::string& bad : {std::string{"hello"}, typo, other_slot, not_an_address}) {
        BOOST_CHECK_MESSAGE(CodeOf([&] { api.Call("peg.deposit", Obj({{"destination", bad}, {"amount", "1"}})); }) == webui::BRIDGE_INVALID_REQUEST, bad);
    }
    for (const char* amount : {"0", "-1", "abc", ""}) {
        BOOST_CHECK_MESSAGE(CodeOf([&] { api.Call("peg.deposit", Obj({{"destination", good}, {"amount", amount}})); }) == webui::BRIDGE_INVALID_REQUEST, amount);
    }
    // A good request goes on to the node (here: none).
    BOOST_CHECK_EQUAL(CodeOf([&] { api.Call("peg.deposit", Obj({{"destination", good}, {"amount", "1.5"}})); }), webui::BRIDGE_HOST_ERROR);
}

BOOST_AUTO_TEST_CASE(page_rpc_policy_of_the_chain)
{
#if WEBUI_SIDECHAIN
    // A sidechain: its peg and merged mining; none of the drivechain methods of the mainchain, no solo mining.
    for (const char* m : {"getmainchaininfo", "getbmminfo", "setbmm", "requestbmmblock", "listwithdrawals", "getwithdrawalbundle", "getdepositaddress", "createwithdrawal", "refundwithdrawal"}) {
        BOOST_CHECK_MESSAGE(webui::PageRpcPolicy(m).has_value(), m);
    }
    for (const char* m : {"acksidechain", "createsidechainproposal", "setwithdrawalvote", "createsidechaindeposit", "setgenerate", "getgenerate"}) {
        BOOST_CHECK_MESSAGE(!webui::PageRpcPolicy(m).has_value(), m);
    }
    BOOST_CHECK(webui::KeyedWalletRpc("createwithdrawal"));
    BOOST_CHECK(webui::KeyedWalletRpc("refundwithdrawal"));
    BOOST_CHECK(!webui::KeyedWalletRpc("createsidechaindeposit"));
    // Mining one block waits for the mainchain node: apart from the quick calls.
    BOOST_CHECK(webui::PageRpcPolicy("requestbmmblock") == webui::Bridge::Thread::LONG);
#else
    BOOST_CHECK(webui::PageRpcPolicy("acksidechain").has_value());
    BOOST_CHECK(!webui::PageRpcPolicy("setbmm").has_value());
#endif
}

BOOST_AUTO_TEST_SUITE_END()
