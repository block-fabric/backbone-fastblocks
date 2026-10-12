// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <consensus/amount.h>
#include <core_io.h>
#include <kernel/chainparams.h>
#include <node/context.h>
#include <pos/evidence.h>
#include <pos/finality.h>
#include <pos/forkchoice.h>
#include <pos/hooks.h>
#include <pos/producer.h>
#include <rpc/pos.h>
#include <streams.h>
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

pos::FastBlocksHooks& EnsureHooks()
{
    pos::FastBlocksHooks* hooks{pos::InstalledHooks()};
    if (!hooks) throw JSONRPCError(RPC_MISC_ERROR, "This chain is not a sidechain: it has no blocks of stake");
    return *hooks;
}

int64_t NowSlot(const Consensus::PosParams& params)
{
    return pos::SlotOf(TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()), params);
}

uint256 ParsePubkey(const UniValue& value)
{
    const std::vector<unsigned char> bytes{ParseHexV(value, "pubkey")};
    if (bytes.size() != 32) throw JSONRPCError(RPC_INVALID_PARAMETER, "pubkey must be an x-only key: 32 bytes in hex");
    return uint256{std::span<const unsigned char>{bytes}};
}

/** The slots of `epoch` each validator leads. */
std::vector<int64_t> SlotsLed(const pos::FastBlocksHooks& hooks, int64_t epoch, const uint256& genesis)
{
    const Consensus::PosParams& params{hooks.Params()};
    std::vector<int64_t> counts(hooks.Validators().Validators().size(), 0);
    for (int64_t slot{epoch * params.slots_per_epoch}; slot < (epoch + 1) * params.slots_per_epoch; ++slot) {
        if (const auto leader{hooks.Validators().Leader(slot, params, genesis)}) ++counts[*leader];
    }
    return counts;
}

/** The first slot from `from` on, within two epochs, that one of `keys` leads. */
std::optional<int64_t> NextSlotLed(const pos::FastBlocksHooks& hooks, const std::vector<uint256>& keys, int64_t from, const uint256& genesis)
{
    const Consensus::PosParams& params{hooks.Params()};
    for (int64_t slot{from}; slot < from + 2 * params.slots_per_epoch; ++slot) {
        const auto leader{hooks.Validators().Leader(slot, params, genesis)};
        if (!leader) return std::nullopt;
        if (std::find(keys.begin(), keys.end(), hooks.Validators().Validators()[*leader].pubkey) != keys.end()) return slot;
    }
    return std::nullopt;
}

std::vector<uint256> LocalKeys(const pos::FastBlocksHooks& hooks)
{
    const pos::Producer* producer{hooks.GetProducer()};
    return producer ? producer->Keys() : std::vector<uint256>{};
}

UniValue ProofToJSON(const pos::Offence& offence, const pos::DoubleProposal& proof)
{
    UniValue entry(UniValue::VOBJ);
    entry.pushKV("pubkey", HexStr(offence.pubkey));
    entry.pushKV("slot", offence.slot);
    UniValue hashes(UniValue::VARR);
    hashes.push_back(proof.a.GetHash().GetHex());
    hashes.push_back(proof.b.GetHash().GetHex());
    entry.pushKV("hashes", std::move(hashes));
    DataStream s{};
    s << proof;
    entry.pushKV("hex", HexStr(s));
    return entry;
}

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
            {RPCResult::Type::NUM, "evidence", "Proofs this node knows that a leader signed two blocks for one slot"},
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
    result.pushKV("evidence", hooks->Evidence().Size());
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


RPCMethod getvalidators()
{
    return RPCMethod{
        "getvalidators",
        "Returns the validator set of an epoch of FastBlocks' blocks of stake, and how many slots each validator leads in it.",
        {
            {"epoch", RPCArg::Type::NUM, RPCArg::DefaultHint{"the current epoch"}, "The epoch"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "epoch", "The epoch"},
            {RPCResult::Type::NUM, "firstslot", "Its first slot"},
            {RPCResult::Type::BOOL, "active", "Whether blocks of stake can be made (enough validators)"},
            {RPCResult::Type::NUM, "totalstake", "The stake of the set"},
            {RPCResult::Type::ARR, "validators", "In the order leaders are picked from (by key)",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR_HEX, "pubkey", "x-only key"},
                    {RPCResult::Type::NUM, "stake", "Weight in leader selection"},
                    {RPCResult::Type::NUM, "slots", "Slots of the epoch it leads"},
                    {RPCResult::Type::BOOL, "local", "Whether this node holds its key"},
                }},
            }},
        }},
        RPCExamples{HelpExampleCli("getvalidators", "") + HelpExampleCli("getvalidators", "100")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const pos::FastBlocksHooks& hooks{EnsureHooks()};
    const Consensus::PosParams& params{hooks.Params()};
    const int64_t epoch{request.params[0].isNull() ? pos::EpochOf(NowSlot(params), params) : request.params[0].getInt<int64_t>()};
    if (epoch < 0 || epoch > std::numeric_limits<uint32_t>::max()) throw JSONRPCError(RPC_INVALID_PARAMETER, "epoch out of range");
    const uint256 genesis{chainman.GetConsensus().hashGenesisBlock};
    const auto slots{SlotsLed(hooks, epoch, genesis)};
    const auto local{LocalKeys(hooks)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("epoch", epoch);
    result.pushKV("firstslot", epoch * params.slots_per_epoch);
    result.pushKV("active", hooks.Validators().Active());
    result.pushKV("totalstake", hooks.Validators().TotalStake());
    UniValue validators(UniValue::VARR);
    for (size_t i{0}; i < hooks.Validators().Validators().size(); ++i) {
        const auto& validator{hooks.Validators().Validators()[i]};
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("pubkey", HexStr(validator.pubkey));
        entry.pushKV("stake", validator.stake);
        entry.pushKV("slots", slots[i]);
        entry.pushKV("local", std::find(local.begin(), local.end(), validator.pubkey) != local.end());
        validators.push_back(std::move(entry));
    }
    result.pushKV("validators", std::move(validators));
    return result;
},
    };
}

RPCMethod getvalidator()
{
    return RPCMethod{
        "getvalidator",
        "Returns what this node knows of a validator of FastBlocks: whether it is in the set, its stake, and the next slot it leads.",
        {
            {"pubkey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The validator's x-only key"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "pubkey", "x-only key"},
            {RPCResult::Type::BOOL, "inset", "Whether it is in the validator set"},
            {RPCResult::Type::NUM, "stake", /*optional=*/true, "Its weight in leader selection"},
            {RPCResult::Type::NUM, "index", /*optional=*/true, "Its position in the set"},
            {RPCResult::Type::NUM, "slots", /*optional=*/true, "Slots of the current epoch it leads"},
            {RPCResult::Type::NUM, "nextslot", /*optional=*/true, "The next slot it leads (within two epochs)"},
            {RPCResult::Type::BOOL, "local", "Whether this node holds its key"},
            {RPCResult::Type::NUM, "offences", "Proofs this node knows that it signed two blocks for one slot"},
        }},
        RPCExamples{HelpExampleCli("getvalidator", "\"<pubkey>\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const pos::FastBlocksHooks& hooks{EnsureHooks()};
    const Consensus::PosParams& params{hooks.Params()};
    const uint256 pubkey{ParsePubkey(request.params[0])};
    const uint256 genesis{chainman.GetConsensus().hashGenesisBlock};
    const int64_t slot{NowSlot(params)};
    const auto local{LocalKeys(hooks)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("pubkey", HexStr(pubkey));
    const auto index{hooks.Validators().Find(pubkey)};
    result.pushKV("inset", index.has_value());
    if (index) {
        result.pushKV("stake", hooks.Validators().Validators()[*index].stake);
        result.pushKV("index", static_cast<uint64_t>(*index));
        result.pushKV("slots", SlotsLed(hooks, pos::EpochOf(slot, params), genesis)[*index]);
        if (const auto next{NextSlotLed(hooks, {pubkey}, slot + 1, genesis)}) result.pushKV("nextslot", *next);
    }
    result.pushKV("local", std::find(local.begin(), local.end(), pubkey) != local.end());
    int64_t offences{0};
    for (const auto& [offence, _] : hooks.Evidence().All()) offences += offence.pubkey == pubkey;
    result.pushKV("offences", offences);
    return result;
},
    };
}

RPCMethod getleaderschedule()
{
    return RPCMethod{
        "getleaderschedule",
        "Returns the leader of each slot of an epoch of FastBlocks' blocks of stake.",
        {
            {"epoch", RPCArg::Type::NUM, RPCArg::DefaultHint{"the current epoch"}, "The epoch"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "epoch", "The epoch"},
            {RPCResult::Type::ARR, "slots", "Its slots, in order",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::NUM, "slot", "The slot"},
                    {RPCResult::Type::NUM_TIME, "time", "When it starts, " + UNIX_EPOCH_TIME},
                    {RPCResult::Type::STR_HEX, "leader", /*optional=*/true, "x-only key of its leader (none if blocks of stake cannot be made)"},
                }},
            }},
        }},
        RPCExamples{HelpExampleCli("getleaderschedule", "") + HelpExampleCli("getleaderschedule", "100")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const pos::FastBlocksHooks& hooks{EnsureHooks()};
    const Consensus::PosParams& params{hooks.Params()};
    const int64_t epoch{request.params[0].isNull() ? pos::EpochOf(NowSlot(params), params) : request.params[0].getInt<int64_t>()};
    if (epoch < 0 || epoch > std::numeric_limits<uint32_t>::max()) throw JSONRPCError(RPC_INVALID_PARAMETER, "epoch out of range");
    const uint256 genesis{chainman.GetConsensus().hashGenesisBlock};
    UniValue slots(UniValue::VARR);
    for (int64_t slot{epoch * params.slots_per_epoch}; slot < (epoch + 1) * params.slots_per_epoch; ++slot) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("slot", slot);
        entry.pushKV("time", pos::SlotTime(slot, params));
        if (const auto leader{hooks.Validators().Leader(slot, params, genesis)}) entry.pushKV("leader", HexStr(hooks.Validators().Validators()[*leader].pubkey));
        slots.push_back(std::move(entry));
    }
    UniValue result(UniValue::VOBJ);
    result.pushKV("epoch", epoch);
    result.pushKV("slots", std::move(slots));
    return result;
},
    };
}

RPCMethod getanchorinfo()
{
    return RPCMethod{
        "getanchorinfo",
        "Returns FastBlocks' anchors: the newest ones on the active chain, the blocks they make anchored and final, the fee pot, and the tickets this node knows.",
        {
            {"count", RPCArg::Type::NUM, RPCArg::Default{10}, "How many of the newest anchors and tickets to list (at most 100)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "anchors", "Anchors on the active chain"},
            {RPCResult::Type::NUM, "finalanchors", "Anchors that make a block final"},
            {RPCResult::Type::STR_HEX, "anchoredtip", /*optional=*/true, "The highest block an anchor covers"},
            {RPCResult::Type::NUM, "anchoredheight", "Its height, -1 if none"},
            {RPCResult::Type::STR_HEX, "finaltip", /*optional=*/true, "The highest final block"},
            {RPCResult::Type::NUM, "finalheight", "Its height, -1 if none"},
            {RPCResult::Type::ARR, "recent", "The newest anchors, newest first",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR_HEX, "block", "The acknowledgement or the BMM block"},
                    {RPCResult::Type::NUM, "height", "Its height"},
                    {RPCResult::Type::STR, "kind", "\"ticket\" (an acknowledgement) or \"bmm\""},
                    {RPCResult::Type::STR_HEX, "anchored", "The block it anchors"},
                    {RPCResult::Type::NUM, "anchoredheight", "Its height"},
                    {RPCResult::Type::NUM, "mainheight", "Mainchain height of the commitment"},
                }},
            }},
            {RPCResult::Type::OBJ, "pot", "The fee pot",
            {
                {RPCResult::Type::STR_AMOUNT, "accrued", "Put in the pot since the chain began"},
                {RPCResult::Type::STR_AMOUNT, "paid", "Paid out of it"},
                {RPCResult::Type::STR_AMOUNT, "due", "What an anchor of the tip would pay"},
            }},
            {RPCResult::Type::ARR, "tickets", "The newest tickets this node knows, newest first",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR_HEX, "hash", "h*, what the mainchain commits to"},
                    {RPCResult::Type::STR_HEX, "anchored", "The block it anchors"},
                    {RPCResult::Type::STR_HEX, "payee", "The script the pot is paid to"},
                    {RPCResult::Type::NUM, "committedheight", /*optional=*/true, "Mainchain height of the commitment to it, if any"},
                }},
            }},
        }},
        RPCExamples{HelpExampleCli("getanchorinfo", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const pos::FastBlocksHooks& hooks{EnsureHooks()};
    const int count{request.params[0].isNull() ? 10 : request.params[0].getInt<int>()};
    if (count < 0 || count > 100) throw JSONRPCError(RPC_INVALID_PARAMETER, "count must be from 0 to 100");
    const int final_anchors{hooks.Params().final_anchors};
    UniValue result(UniValue::VOBJ);
    UniValue recent(UniValue::VARR);
    UniValue pot(UniValue::VOBJ);
    {
        LOCK(::cs_main);
        const CBlockIndex& tip{*chainman.ActiveChain().Tip()};
        const pos::FastState fast{pos::GetFastState(chainman.ActiveChainstate().SideState().View())};
        result.pushKV("anchors", fast.anchors);
        result.pushKV("finalanchors", final_anchors);
        const auto anchors{pos::RecentAnchors(hooks, tip, std::max<size_t>(count, final_anchors))};
        const auto heights{pos::AnchoredHeights(hooks, tip, final_anchors)};
        const CBlockIndex* anchored{heights.empty() ? nullptr : tip.GetAncestor(heights.front())};
        const CBlockIndex* final_block{static_cast<int>(heights.size()) >= final_anchors && final_anchors > 0 ? tip.GetAncestor(heights[final_anchors - 1]) : nullptr};
        if (anchored) result.pushKV("anchoredtip", anchored->GetBlockHash().GetHex());
        result.pushKV("anchoredheight", anchored ? anchored->nHeight : -1);
        if (final_block) result.pushKV("finaltip", final_block->GetBlockHash().GetHex());
        result.pushKV("finalheight", final_block ? final_block->nHeight : -1);
        for (size_t i{0}; i < anchors.size() && i < static_cast<size_t>(count); ++i) {
            const pos::ChainAnchor& anchor{anchors[i]};
            UniValue entry(UniValue::VOBJ);
            entry.pushKV("block", anchor.block->GetBlockHash().GetHex());
            entry.pushKV("height", anchor.block->nHeight);
            entry.pushKV("kind", anchor.block->m_ext ? "ticket" : "bmm");
            entry.pushKV("anchored", anchor.anchored->GetBlockHash().GetHex());
            entry.pushKV("anchoredheight", anchor.anchored->nHeight);
            entry.pushKV("mainheight", static_cast<uint64_t>(anchor.main_height));
            recent.push_back(std::move(entry));
        }
        pot.pushKV("accrued", ValueFromAmount(fast.pot_accrued));
        pot.pushKV("paid", ValueFromAmount(fast.pot_paid));
        pot.pushKV("due", ValueFromAmount(pos::PotAccruedAt(chainman.ActiveChainstate().SideState().View(), tip.nHeight) - fast.pot_paid));
    }
    result.pushKV("recent", std::move(recent));
    result.pushKV("pot", std::move(pot));
    UniValue tickets(UniValue::VARR);
    const auto known{hooks.Tickets()};
    for (auto it{known.rbegin()}; it != known.rend() && static_cast<int>(tickets.size()) < count; ++it) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("hash", it->Hash().GetHex());
        entry.pushKV("anchored", it->anchored.GetHex());
        entry.pushKV("payee", HexStr(it->payee));
        if (chainman.m_mainchain) {
            if (const auto committed{chainman.m_mainchain->CommittedHeight(it->Hash())}) entry.pushKV("committedheight", *committed);
        }
        tickets.push_back(std::move(entry));
    }
    result.pushKV("tickets", std::move(tickets));
    return result;
},
    };
}

RPCMethod submitevidence()
{
    return RPCMethod{
        "submitevidence",
        "Gives the node a proof that a leader of FastBlocks signed two blocks for one slot (two extended headers, serialized): it keeps it and relays it.",
        {
            {"hex", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The proof: the two headers, serialized, in hex"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "pubkey", "The validator that signed twice"},
            {RPCResult::Type::NUM, "slot", "The slot"},
            {RPCResult::Type::BOOL, "new", "Whether the node did not know the proof yet"},
        }},
        RPCExamples{HelpExampleCli("submitevidence", "\"<hex>\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const pos::FastBlocksHooks& hooks{EnsureHooks()};
    const std::vector<unsigned char> bytes{ParseHexV(request.params[0], "hex")};
    pos::DoubleProposal proof;
    try {
        SpanReader reader{bytes};
        reader >> proof;
        if (!reader.empty()) throw std::ios_base::failure("extra bytes");
    } catch (const std::ios_base::failure&) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "The proof does not read: two extended headers are expected");
    }
    const size_t before{hooks.Evidence().Size()};
    std::string reason;
    const auto offence{hooks.AddEvidence(proof, reason)};
    if (!offence) throw JSONRPCError(RPC_VERIFY_REJECTED, reason);
    UniValue result(UniValue::VOBJ);
    result.pushKV("pubkey", HexStr(offence->pubkey));
    result.pushKV("slot", offence->slot);
    result.pushKV("new", hooks.Evidence().Size() > before);
    return result;
},
    };
}

RPCMethod listevidence()
{
    return RPCMethod{
        "listevidence",
        "Lists the proofs this node knows that a leader of FastBlocks signed two blocks for one slot (found by watching headers, or received).",
        {},
        RPCResult{RPCResult::Type::ARR, "", "",
        {
            {RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "pubkey", "The validator that signed twice"},
                {RPCResult::Type::NUM, "slot", "The slot"},
                {RPCResult::Type::ARR, "hashes", "The two blocks", {{RPCResult::Type::STR_HEX, "", ""}}},
                {RPCResult::Type::STR_HEX, "hex", "The proof, for submitevidence"},
            }},
        }},
        RPCExamples{HelpExampleCli("listevidence", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const pos::FastBlocksHooks& hooks{EnsureHooks()};
    UniValue result(UniValue::VARR);
    for (const auto& [offence, proof] : hooks.Evidence().All()) result.push_back(ProofToJSON(offence, proof));
    return result;
},
    };
}

RPCMethod setstaking()
{
    return RPCMethod{
        "setstaking",
        "Turns the making of blocks of stake by this node's validator keys (-validatorkeyfile) on or off. It is on when the node starts.",
        {
            {"enabled", RPCArg::Type::BOOL, RPCArg::Optional::NO, "Whether to make blocks at the slots the keys lead"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::BOOL, "enabled", "Whether the node makes blocks of stake now"},
        }},
        RPCExamples{HelpExampleCli("setstaking", "false")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    pos::FastBlocksHooks& hooks{EnsureHooks()};
    pos::Producer* producer{hooks.GetProducer()};
    if (!producer) throw JSONRPCError(RPC_MISC_ERROR, "This node has no validator keys (-validatorkeyfile)");
    producer->SetEnabled(request.params[0].get_bool());
    LogInfo("FastBlocks: making blocks of stake turned %s", producer->Enabled() ? "on" : "off");
    UniValue result(UniValue::VOBJ);
    result.pushKV("enabled", producer->Enabled());
    return result;
},
    };
}

RPCMethod getstakinginfo()
{
    return RPCMethod{
        "getstakinginfo",
        "Returns what this node does as a validator of FastBlocks: its keys, whether it makes blocks of stake, what it made, and its next slot.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::BOOL, "validator", "Whether the node has validator keys (-validatorkeyfile)"},
            {RPCResult::Type::BOOL, "enabled", "Whether it makes blocks of stake (setstaking)"},
            {RPCResult::Type::STR, "producererror", "Why the block producer could not start, if it could not"},
            {RPCResult::Type::ARR, "keys", "Its validator keys",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR_HEX, "pubkey", "x-only key"},
                    {RPCResult::Type::BOOL, "inset", "Whether it is in the validator set"},
                    {RPCResult::Type::NUM, "stake", "Its stake (0 if not in the set)"},
                }},
            }},
            {RPCResult::Type::NUM, "stake", "The stake of its keys in the set"},
            {RPCResult::Type::NUM, "totalstake", "The stake of the whole set"},
            {RPCResult::Type::NUM, "produced", "Blocks made since the node started"},
            {RPCResult::Type::NUM, "acknowledgements", "Of which acknowledgements"},
            {RPCResult::Type::NUM, "failed", "Slots its keys led and it made no block for"},
            {RPCResult::Type::NUM, "lastslot", "Slot of the last block made, -1 if none"},
            {RPCResult::Type::NUM, "lastsignedslot", "The highest slot its keys signed, by the slashing protection, -1 if none"},
            {RPCResult::Type::STR, "error", "Why the last block could not be made, if it could not"},
            {RPCResult::Type::NUM, "slot", "The current slot"},
            {RPCResult::Type::NUM, "nextslot", /*optional=*/true, "The next slot one of its keys leads (within two epochs)"},
            {RPCResult::Type::NUM_TIME, "nexttime", /*optional=*/true, "When it starts, " + UNIX_EPOCH_TIME},
        }},
        RPCExamples{HelpExampleCli("getstakinginfo", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    pos::FastBlocksHooks& hooks{EnsureHooks()};
    const Consensus::PosParams& params{hooks.Params()};
    const pos::Producer* producer{hooks.GetProducer()};
    const int64_t slot{NowSlot(params)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("validator", producer != nullptr);
    result.pushKV("enabled", producer && producer->Enabled());
    result.pushKV("producererror", hooks.ProducerError());
    const auto keys{LocalKeys(hooks)};
    UniValue entries(UniValue::VARR);
    int64_t stake{0};
    for (const uint256& key : keys) {
        const auto index{hooks.Validators().Find(key)};
        const int64_t key_stake{index ? hooks.Validators().Validators()[*index].stake : 0};
        stake += key_stake;
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("pubkey", HexStr(key));
        entry.pushKV("inset", index.has_value());
        entry.pushKV("stake", key_stake);
        entries.push_back(std::move(entry));
    }
    result.pushKV("keys", std::move(entries));
    result.pushKV("stake", stake);
    result.pushKV("totalstake", hooks.Validators().TotalStake());
    const pos::Producer::Status status{producer ? producer->GetStatus() : pos::Producer::Status{}};
    result.pushKV("produced", status.produced);
    result.pushKV("acknowledgements", status.acknowledgements);
    result.pushKV("failed", status.failed);
    result.pushKV("lastslot", status.last_slot);
    result.pushKV("lastsignedslot", producer ? producer->LastSignedSlot() : -1);
    result.pushKV("error", status.error);
    result.pushKV("slot", slot);
    if (const auto next{NextSlotLed(hooks, keys, slot + 1, chainman.GetConsensus().hashGenesisBlock)}) {
        result.pushKV("nextslot", *next);
        result.pushKV("nexttime", pos::SlotTime(*next, params));
    }
    return result;
},
    };
}

} // namespace

std::vector<RPCResult> PosBlockFields()
{
    return {
        {RPCResult::Type::STR, "kind", /*optional=*/true, "FastBlocks: \"pos\" for a block of stake, \"bmm\" for a merged mined block"},
        {RPCResult::Type::NUM, "slot", /*optional=*/true, "FastBlocks: the slot of a block of stake"},
        {RPCResult::Type::STR_HEX, "proposer", /*optional=*/true, "FastBlocks: the x-only key of the leader that signed a block of stake"},
        {RPCResult::Type::OBJ, "ack", /*optional=*/true, "FastBlocks: the anchor a block of stake acknowledges",
        {
            {RPCResult::Type::STR_HEX, "ticket", "h*, what the mainchain committed to"},
            {RPCResult::Type::STR_HEX, "anchored", "The block anchored"},
            {RPCResult::Type::STR_HEX, "payee", "The script the pot is paid to"},
            {RPCResult::Type::NUM, "mainheight", "Mainchain height of the commitment"},
        }},
        {RPCResult::Type::OBJ, "finality", /*optional=*/true, "FastBlocks: how final the block is (on the active chain only)",
        {
            {RPCResult::Type::STR, "status", "\"soft\", \"anchored\" or \"final\""},
            {RPCResult::Type::NUM, "anchors", "Anchors that cover it (counted up to finalanchors)"},
        }},
    };
}

void PushPosBlockInfo(const ChainstateManager& chainman, const CBlock& block, const CBlockIndex& index, UniValue& result)
{
    const pos::FastBlocksHooks* hooks{pos::InstalledHooks()};
    if (!hooks) return;
    const Consensus::PosParams& params{hooks->Params()};
    const bool is_pos{pos::IsPosHeader(block)};
    result.pushKV("kind", is_pos ? "pos" : "bmm");
    if (is_pos) {
        const int64_t slot{pos::SlotOf(block.nTime, params)};
        result.pushKV("slot", slot);
        if (const auto leader{hooks->Validators().Leader(slot, params, chainman.GetConsensus().hashGenesisBlock)}) {
            result.pushKV("proposer", HexStr(hooks->Validators().Validators()[*leader].pubkey));
        }
        pos::PosSections sections;
        if (pos::PosSections::Decode(block.vSections, sections) && sections.ack) {
            UniValue ack(UniValue::VOBJ);
            ack.pushKV("ticket", sections.ack->ticket.Hash().GetHex());
            ack.pushKV("anchored", sections.ack->ticket.anchored.GetHex());
            ack.pushKV("payee", HexStr(sections.ack->ticket.payee));
            ack.pushKV("mainheight", sections.ack->main_height);
            result.pushKV("ack", std::move(ack));
        }
    }
    LOCK(::cs_main);
    const CChain& chain{chainman.ActiveChain()};
    if (!chain.Contains(index)) return;
    const pos::Finality finality{pos::FinalityOf(*hooks, index, *chain.Tip(), params.final_anchors)};
    UniValue entry(UniValue::VOBJ);
    entry.pushKV("status", pos::FinalityName(finality.status));
    entry.pushKV("anchors", finality.anchors);
    result.pushKV("finality", std::move(entry));
}

void PushPosChainInfo(const ChainstateManager& chainman, UniValue& result)
{
    const pos::FastBlocksHooks* hooks{pos::InstalledHooks()};
    if (!hooks) return;
    LOCK(::cs_main);
    const int final_anchors{hooks->Params().final_anchors};
    const CBlockIndex& tip{*chainman.ActiveChain().Tip()};
    const auto heights{pos::AnchoredHeights(*hooks, tip, std::max(final_anchors, 1))};
    if (!heights.empty()) result.pushKV("anchored_tip", tip.GetAncestor(heights.front())->GetBlockHash().GetHex());
    if (final_anchors > 0 && static_cast<int>(heights.size()) >= final_anchors) result.pushKV("final_tip", tip.GetAncestor(heights[final_anchors - 1])->GetBlockHash().GetHex());
}


void RegisterPosRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"sidechain", &getposinfo},
        {"sidechain", &getvalidators},
        {"sidechain", &getvalidator},
        {"sidechain", &getleaderschedule},
        {"sidechain", &getanchorinfo},
        {"sidechain", &submitevidence},
        {"sidechain", &listevidence},
        {"sidechain", &setstaking},
        {"sidechain", &getstakinginfo},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
