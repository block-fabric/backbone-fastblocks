// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <consensus/amount.h>
#include <core_io.h>
#include <kernel/chainparams.h>
#include <node/context.h>
#include <pos/forkchoice.h>
#include <pos/hooks.h>
#include <pos/producer.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <sidechain/state.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>

using node::NodeContext;

namespace {

RPCMethod getposinfo()
{
    return RPCMethod{
        "getposinfo",
        "Returns the state of FastBlocks' blocks of stake: the validators, the slot and its leader, anchors, the fee pot, and this node's block producer.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::BOOL, "active", "Whether blocks of stake can be made on this chain (enough validators)"},
            {RPCResult::Type::NUM, "slotseconds", "Length of a slot"},
            {RPCResult::Type::NUM, "slot", "The current slot, by this node's clock"},
            {RPCResult::Type::NUM, "epoch", "The current epoch"},
            {RPCResult::Type::STR_HEX, "leader", /*optional=*/true, "The x-only key of the leader of the current slot"},
            {RPCResult::Type::ARR, "validators", "The validator set",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR_HEX, "pubkey", "x-only key"},
                    {RPCResult::Type::NUM, "stake", "Weight in leader selection"},
                }},
            }},
            {RPCResult::Type::NUM, "anchors", "Anchors on the active chain (acknowledged tickets and BMM blocks)"},
            {RPCResult::Type::NUM, "lastanchormainheight", "Mainchain height of the commitment of the last anchor, -1 if none"},
            {RPCResult::Type::STR_HEX, "lastanchored", /*optional=*/true, "The block the last anchor anchors"},
            {RPCResult::Type::NUM, "lastanchoredheight", "Its height, -1 if none"},
            {RPCResult::Type::STR_HEX, "anchorpoint", /*optional=*/true, "The last block that took the peg actions: the next ticket anchors it or a block after it"},
            {RPCResult::Type::NUM, "lastposslot", "Slot of the last block of stake on the active chain, -1 if none"},
            {RPCResult::Type::NUM, "firstunanchoredslot", "Slot of the first block of stake no anchor covers, -1 if none"},
            {RPCResult::Type::BOOL, "stalled", "Whether a BMM block built now may carry transactions (blocks of stake stalled)"},
            {RPCResult::Type::OBJ, "pot", "The fee pot",
            {
                {RPCResult::Type::STR_AMOUNT, "accrued", "Put in the pot since the chain began"},
                {RPCResult::Type::STR_AMOUNT, "paid", "Paid out of it"},
            }},
            {RPCResult::Type::STR_HEX, "chainkey", "The order of the active chain (nChainWork): anchors, last commitment height, height"},
            {RPCResult::Type::NUM, "tickets", "Anchor tickets this node knows"},
            {RPCResult::Type::OBJ, "producer", /*optional=*/true, "This node's block producer, if it has validator keys",
            {
                {RPCResult::Type::ARR, "keys", "x-only keys", {{RPCResult::Type::STR_HEX, "", ""}}},
                {RPCResult::Type::NUM, "produced", "Blocks made"},
                {RPCResult::Type::NUM, "acknowledgements", "Of which acknowledgements"},
                {RPCResult::Type::NUM, "failed", "Slots this node led and made no block for"},
                {RPCResult::Type::NUM, "lastslot", "Slot of the last block made, -1 if none"},
                {RPCResult::Type::STR, "error", "Why the last block could not be made, if it could not"},
            }},
        }},
        RPCExamples{HelpExampleCli("getposinfo", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node{EnsureAnyNodeContext(request.context)};
    ChainstateManager& chainman{EnsureChainman(node)};
    const pos::FastBlocksHooks* hooks{pos::InstalledHooks()};
    if (!hooks) throw JSONRPCError(RPC_MISC_ERROR, "This chain is not a sidechain: it has no blocks of stake");
    const Consensus::PosParams& params{hooks->Params()};
    const int64_t now{TicksSinceEpoch<std::chrono::seconds>(NodeClock::now())};
    const int64_t slot{pos::SlotOf(now, params)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("active", hooks->Validators().Active());
    result.pushKV("slotseconds", params.slot_seconds);
    result.pushKV("slot", slot);
    result.pushKV("epoch", pos::EpochOf(slot, params));
    if (const auto leader{hooks->Validators().Leader(slot, params, chainman.GetConsensus().hashGenesisBlock)}) {
        result.pushKV("leader", HexStr(hooks->Validators().Validators()[*leader].pubkey));
    }
    UniValue validators(UniValue::VARR);
    for (const auto& validator : hooks->Validators().Validators()) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("pubkey", HexStr(validator.pubkey));
        entry.pushKV("stake", validator.stake);
        validators.push_back(std::move(entry));
    }
    result.pushKV("validators", std::move(validators));
    pos::FastState fast;
    arith_uint256 work;
    {
        LOCK(::cs_main);
        fast = pos::GetFastState(chainman.ActiveChainstate().SideState().View());
        work = chainman.ActiveChain().Tip()->nChainWork;
    }
    result.pushKV("anchors", fast.anchors);
    result.pushKV("lastanchormainheight", fast.last_main_height);
    if (fast.last_anchored_height >= 0) result.pushKV("lastanchored", fast.last_anchored.GetHex());
    result.pushKV("lastanchoredheight", fast.last_anchored_height);
    if (fast.anchor_point_height >= 0) result.pushKV("anchorpoint", fast.anchor_point.GetHex());
    result.pushKV("lastposslot", fast.last_pos_slot);
    result.pushKV("firstunanchoredslot", fast.first_unanchored_slot);
    const auto main_tip{chainman.m_mainchain ? chainman.m_mainchain->GetBlock(chainman.m_mainchain->Height()) : std::nullopt};
    result.pushKV("stalled", hooks->Stalled(fast, main_tip ? main_tip->time : now));
    UniValue pot(UniValue::VOBJ);
    pot.pushKV("accrued", ValueFromAmount(fast.pot_accrued));
    pot.pushKV("paid", ValueFromAmount(fast.pot_paid));
    result.pushKV("pot", std::move(pot));
    result.pushKV("chainkey", work.GetHex());
    result.pushKV("tickets", hooks->Tickets().size());
    if (const pos::Producer* producer{hooks->GetProducer()}) {
        const auto status{producer->GetStatus()};
        UniValue entry(UniValue::VOBJ);
        UniValue keys(UniValue::VARR);
        for (const uint256& key : producer->Keys()) keys.push_back(HexStr(key));
        entry.pushKV("keys", std::move(keys));
        entry.pushKV("produced", status.produced);
        entry.pushKV("acknowledgements", status.acknowledgements);
        entry.pushKV("failed", status.failed);
        entry.pushKV("lastslot", status.last_slot);
        entry.pushKV("error", status.error);
        result.pushKV("producer", std::move(entry));
    }
    return result;
},
    };
}

} // namespace

void RegisterPosRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"sidechain", &getposinfo},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
