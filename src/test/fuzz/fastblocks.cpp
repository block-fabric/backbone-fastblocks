// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// FastBlocks under fuzzing (doc/fastblocks.md):
//  - pos_header: extended headers and blocks read back as written, their hash covers hashSections and
//    not the signature (which commits to the genesis block), sections decode only in their one
//    encoding, slots and the chain key are consistent, the context-free block rules never crash, and
//    headers nobody could check count one anchor at most per new commitment height;
//  - pos_ticket: tickets read back, the pool of tickets stays bounded and consistent whatever peers
//    send, and the count of tickets a peer may send holds;
//  - pos_evidence: a proof is an offence exactly when two different headers of one slot are signed
//    by its leader, whichever comes first, and the pool of proofs stays bounded;
//  - pos_state: a chain of blocks of stake, acknowledgements, BMM blocks, deposits, payments and
//    withdrawals, bonds, unbonds and leaders that sign twice (and the proofs that punish them, with
//    headers signed on another network or by another branch's leader among them), headers from peers
//    (signed or not, whose sets the node may not know), built by the producer and the template from
//    fuzzed choices: the state agrees with the chain key, the totals of staking with its validators,
//    the coins of the chain are the deposits (pegbalance), taking blocks back and connecting them
//    again comes back to the same state, and no header is taken that the rules of headers refuse.
//  - pos_cold_hooks: the same chain, hooks rebuilt from it mid-chain (a node that restarted) against
//    those that saw it from the start: the same state transition for every block to come, the same
//    time limit of blocks, final height, chain key of headers, and leaders and header outcomes where
//    both can tell the leader (the warm ones may know more sets, from memory, and no less).
//  - pos_staking: the staking rules alone on a state (bonds, unbonds, peg steps, liveness, penalties)
//    with fuzzed parameters: every coin bonded is bonded, unbonding, in the reserve, owed to a
//    reporter or paid out; the totals agree with the validators; the journal takes each step back.

#include <addresstype.h>
#include <chain.h>
#include <chainparams.h>
#include <common/signmessage.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <interfaces/mining.h>
#include <kernel/chainparams.h>
#include <kernel/coinstats.h>
#include <key.h>
#include <key_io.h>
#include <node/miner.h>
#include <pos/evidence.h>
#include <pos/finality.h>
#include <pos/forkchoice.h>
#include <pos/header.h>
#include <pos/hooks.h>
#include <pos/producer.h>
#include <pos/protection.h>
#include <pos/schedule.h>
#include <pos/staking.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/solver.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/check.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <cassert>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

std::vector<CKey> ValidatorKeys()
{
    std::vector<CKey> keys;
    for (uint8_t i{1}; i <= 3; ++i) {
        std::vector<unsigned char> secret(32, 0);
        secret[0] = 0x11;
        secret[31] = i;
        CKey key;
        key.Set(secret.begin(), secret.end(), /*fCompressedIn=*/true);
        keys.push_back(key);
    }
    return keys;
}

uint256 XOnly(const CKey& key)
{
    const XOnlyPubKey xonly{key.GetPubKey()};
    return uint256{std::span<const unsigned char>{xonly.begin(), xonly.end()}};
}

/** Keys that are no genesis validators: they take part by bonding. */
std::vector<CKey> BondingKeys()
{
    std::vector<CKey> keys;
    for (uint8_t i{1}; i <= 2; ++i) {
        std::vector<unsigned char> secret(32, 0);
        secret[0] = 0x22;
        secret[31] = i;
        CKey key;
        key.Set(secret.begin(), secret.end(), /*fCompressedIn=*/true);
        keys.push_back(key);
    }
    return keys;
}

//! The x-only keys of ValidatorKeys (known before any secp256k1 context exists).
const std::vector<std::string> VALIDATOR_XONLY{
    "100f6d8cbf94afb6fc58e9c384b9b3a6516091373a83c869f4e24a9d2bb4a494",
    "45d3b9ce0f54f4d6a17edfe3f9e0993b94d6b299c1a6e5a728ff036ecd9e139f",
    "57a62b05e99914350ce87639a68d0f3dd588e98afaf6c1131235a855d41962f3",
};

/** The chain parameters of a regtest sidechain in slot 5 with the three validators (or none: validators bond). */
std::unique_ptr<const CChainParams> FastParams(bool genesis_validators = true)
{
    CChainParams::RegTestOptions options;
    options.sidechain_slot = 5;
    if (genesis_validators) {
        for (const std::string& hex : VALIDATOR_XONLY) options.pos_validators.push_back({.pubkey = uint256{std::span<const unsigned char>{ParseHex(hex)}}, .stake = 1});
    }
    return CChainParams::RegTest(options);
}

std::unique_ptr<const CChainParams> g_params;

void initialize_pos()
{
    static const auto testing_setup{MakeNoLogFileContext<>()};
    g_params = FastParams();
    // The hooks of the chain, for the process: extended blocks on.
    pos::InstallHooks(*g_params);
}

pos::FastBlocksHooks& Hooks() { return *Assert(pos::InstalledHooks()); }

/**
 * The leader of a slot with the genesis set, and the seed of its epoch when no mainchain block on
 * record is old enough to seed it (a null hash): as the chains of these targets have it (the regtest
 * seed lookahead is an hour, and their records start at the mock time).
 */
uint256 LeaderOf(int64_t slot)
{
    const Consensus::PosParams& params{Hooks().Params()};
    const auto leader{Hooks().GenesisSet().Leader(slot, pos::ValidatorSet::Seed(pos::EpochOf(slot, params), uint256{}))};
    return Hooks().GenesisSet().Validators()[*Assert(leader)].pubkey;
}

const CKey& LeaderKey(const std::vector<CKey>& keys, int64_t slot)
{
    const uint256 pubkey{LeaderOf(slot)};
    for (const CKey& key : keys) {
        if (XOnly(key) == pubkey) return key;
    }
    assert(false);
    return keys[0];
}

CScript FuzzedPayee(FuzzedDataProvider& fdp)
{
    switch (fdp.ConsumeIntegralInRange<int>(0, 3)) {
    case 0: {
        auto hash{fdp.ConsumeBytes<unsigned char>(20)};
        hash.resize(20);
        return GetScriptForDestination(WitnessV0KeyHash{uint160{hash}});
    }
    case 1: return CScript() << OP_RETURN;
    case 2: return CScript() << OP_TRUE;
    default: return ConsumeScript(fdp);
    }
}

/** A PoS header from fuzzed fields; signed by the leader of its slot, another key, or nothing. */
CBlockHeader FuzzedPosHeader(FuzzedDataProvider& fdp, const std::vector<CKey>& keys)
{
    const Consensus::PosParams& params{Hooks().Params()};
    CBlockHeader header;
    header.nVersion = fdp.ConsumeIntegral<int32_t>() | pos::POS_BLOCK_BIT;
    header.hashPrevBlock = uint256{fdp.ConsumeIntegralInRange<uint8_t>(0, 3)};
    header.hashMerkleRoot = uint256{fdp.ConsumeIntegralInRange<uint8_t>(0, 3)};
    const int64_t slot{fdp.ConsumeIntegralInRange<int64_t>(1, 1000)};
    header.nTime = fdp.ConsumeBool() ? static_cast<uint32_t>(pos::SlotTime(slot, params)) : fdp.ConsumeIntegral<uint32_t>();
    header.nBits = fdp.ConsumeIntegral<uint32_t>();
    header.nNonce = fdp.ConsumeIntegralInRange<uint32_t>(0, 3);
    header.ext.hashSections = uint256{fdp.ConsumeIntegralInRange<uint8_t>(0, 3)};
    switch (fdp.ConsumeIntegralInRange<int>(0, 2)) {
    case 0: {
        const int64_t time_slot{pos::SlotOf(header.nTime, params)};
        if (time_slot > 0) {
            assert(LeaderKey(keys, time_slot).SignSchnorr(pos::BlockSigHash(header, Hooks().Genesis()), header.ext.sig, nullptr, uint256{}));
        }
        break;
    }
    case 1: assert(keys[fdp.ConsumeIntegralInRange<size_t>(0, keys.size() - 1)].SignSchnorr(pos::BlockSigHash(header, Hooks().Genesis()), header.ext.sig, nullptr, uint256{})); break;
    default: {
        const auto bytes{fdp.ConsumeBytes<unsigned char>(64)};
        std::copy(bytes.begin(), bytes.end(), header.ext.sig.begin());
    }
    }
    return header;
}

} // namespace

FUZZ_TARGET(pos_header, .init = initialize_pos)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    const Consensus::PosParams& params{Hooks().Params()};

    // A header: an extended one is read with its extension; it reads back as it was written, and its
    // hash does not cover the signature.
    {
        DataStream stream{fdp.ConsumeBytes<std::byte>(fdp.ConsumeIntegralInRange<size_t>(0, 200))};
        try {
            CBlockHeader header;
            stream >> header;
            DataStream again{};
            again << header;
            assert(again.size() == (header.IsExtended() ? 176U : 80U));
            CBlockHeader read;
            again >> read;
            assert(read.GetHash() == header.GetHash() && read.ext == (header.IsExtended() ? header.ext : read.ext));
            if (header.IsExtended()) {
                CBlockHeader other{header};
                other.ext.sig[0] ^= 1;
                assert(other.GetHash() == header.GetHash());
                other.ext.hashSections = ArithToUint256(UintToArith256(header.ext.hashSections) + 1);
                assert(other.GetHash() != header.GetHash());
                assert(!header.IsNull());
            }
        } catch (const std::ios_base::failure&) {
        }
    }

    // A block: read and written back alike; the context-free rules never crash, and the sections
    // are checked against the header.
    {
        DataStream stream{fdp.ConsumeBytes<std::byte>(fdp.ConsumeIntegralInRange<size_t>(0, 600))};
        try {
            CBlock block;
            stream >> TX_WITH_WITNESS(block);
            DataStream again{};
            again << TX_WITH_WITNESS(block);
            CBlock read;
            again >> TX_WITH_WITNESS(read);
            assert(read.GetHash() == block.GetHash() && read.vSections == (block.IsExtended() ? block.vSections : std::vector<unsigned char>{}));
            if (block.IsExtended()) {
                const bool matches{block.ext.hashSections == (block.vSections.empty() ? uint256{} : pos::SectionsHash(block.vSections))};
                assert(sidechain::CheckSections(block) == matches);
            }
            BlockValidationState state;
            if (!block.vtx.empty() && block.vtx[0] && Hooks().CheckBlock(block, state)) {
                // A valid block of stake has sections that read; nNonce is its acknowledged height.
                if (pos::IsPosHeader(block)) {
                    pos::PosSections sections;
                    assert(pos::PosSections::Decode(block.vSections, sections));
                    assert(block.nNonce == (sections.ack ? static_cast<uint32_t>(sections.ack->main_height) : 0U));
                }
            }
        } catch (const std::ios_base::failure&) {
        }
    }

    // Sections: what decodes encodes back to the same bytes (one encoding of each).
    {
        const auto bytes{fdp.ConsumeBytes<unsigned char>(fdp.ConsumeIntegralInRange<size_t>(0, 200))};
        pos::PosSections sections;
        if (pos::PosSections::Decode(bytes, sections)) assert(sections.Encode() == bytes);
        assert(pos::PosSections::Decode(sections.Encode(), sections));
    }

    // Slots: each starts at its time, and times in it fall in it.
    {
        const int64_t slot{fdp.ConsumeIntegralInRange<int64_t>(1, int64_t{1} << 30)};
        assert(pos::SlotOf(pos::SlotTime(slot, params), params) == slot);
        assert(pos::SlotOf(pos::SlotTime(slot, params) + params.slot_seconds - 1, params) == slot);
        assert(pos::EpochOf(slot, params) == slot / params.slots_per_epoch);
    }

    // The chain key: read back; ordered by anchors, then the earlier last commitment, then the height.
    {
        const pos::ChainKey a{.anchors = fdp.ConsumeIntegral<uint32_t>(), .last_anchor_height = fdp.ConsumeIntegral<uint32_t>(), .height = fdp.ConsumeIntegral<uint64_t>()};
        const pos::ChainKey b{.anchors = fdp.ConsumeIntegral<uint32_t>(), .last_anchor_height = fdp.ConsumeIntegral<uint32_t>(), .height = fdp.ConsumeIntegral<uint64_t>()};
        assert(pos::ChainKey::FromWork(a.Work()) == a);
        const auto order{[](const pos::ChainKey& k) { return std::make_tuple(k.anchors, ~k.last_anchor_height, k.height); }};
        assert((a.Work() < b.Work()) == (order(a) < order(b)));
    }

    // Signatures: the leader's signature checks for the header it signed, and only for it, and only
    // on this chain (another genesis block, another network).
    {
        static const std::vector<CKey> keys{ValidatorKeys()};
        const CBlockHeader header{FuzzedPosHeader(fdp, keys)};
        for (const CKey& key : keys) {
            if (pos::CheckBlockSig(header, XOnly(key), Hooks().Genesis())) {
                CBlockHeader other{header};
                other.nNonce ^= 1;
                assert(!pos::CheckBlockSig(other, XOnly(key), Hooks().Genesis()));
                assert(!pos::CheckBlockSig(header, XOnly(key), ArithToUint256(UintToArith256(Hooks().Genesis()) + 1)));
            }
        }
    }

    // Headers whose signature nobody checked yet (their epoch's set unknown: anything may be in them)
    // under the header rule of acknowledgements: along a chain of them, each adds one anchor at most to
    // the order of chains, and only for a commitment height after every one before it -- a branch
    // counts no more anchors than the commitments it names, in order.
    {
        std::vector<std::unique_ptr<CBlockIndex>> chain;
        CBlockHeader genesis;
        chain.push_back(std::make_unique<CBlockIndex>(genesis));
        chain.back()->nChainWork = Hooks().ChainWork(nullptr, genesis);
        std::set<uint32_t> named;
        uint64_t bmm{0};
        LIMITED_WHILE(fdp.ConsumeBool(), 100)
        {
            const CBlockIndex& prev{*chain.back()};
            CBlockHeader header;
            header.hashPrevBlock = ArithToUint256(arith_uint256{chain.size()});
            const bool pos_block{fdp.ConsumeIntegralInRange<int>(0, 7) != 0};
            header.nVersion = pos_block ? pos::POS_BLOCK_BIT : 0;
            header.nNonce = fdp.ConsumeBool() ? 0 : fdp.ConsumeIntegralInRange<uint32_t>(0, 60);
            // (The rule of headers of blocks of stake: a BMM block's nNonce is its proof of work's.)
            if (pos_block && !pos::FastBlocksHooks::AckInOrder(prev, header)) {
                // Refused: an acknowledgement at or below the last anchor's commitment.
                assert(header.nNonce != 0 && header.nNonce <= pos::ChainKey::FromWork(prev.nChainWork).last_anchor_height);
                continue;
            }
            auto index{std::make_unique<CBlockIndex>(header)};
            index->pprev = chain.back().get();
            index->nHeight = prev.nHeight + 1;
            index->nChainWork = Hooks().ChainWork(&prev, header);
            const pos::ChainKey before{pos::ChainKey::FromWork(prev.nChainWork)};
            const pos::ChainKey after{pos::ChainKey::FromWork(index->nChainWork)};
            assert(index->nChainWork > prev.nChainWork);
            assert(after.anchors <= before.anchors + 1);
            if (pos_block && header.nNonce != 0) {
                assert(after.anchors == before.anchors + 1 && after.last_anchor_height == header.nNonce && header.nNonce > before.last_anchor_height);
                assert(named.insert(header.nNonce).second);
            } else if (pos_block) {
                assert(after.anchors == before.anchors);
            } else {
                ++bmm;
            }
            assert(after.anchors == named.size() + bmm);
            chain.push_back(std::move(index));
        }
    }
}

FUZZ_TARGET(pos_ticket, .init = initialize_pos)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    // Hooks of their own, not attached to a chainstate: the pool of tickets alone.
    pos::FastBlocksHooks hooks{*g_params};
    std::set<uint256> added;
    std::map<int64_t, size_t> taken;
    LIMITED_WHILE(fdp.ConsumeBool(), 3000)
    {
        switch (fdp.ConsumeIntegralInRange<int>(0, 3)) {
        case 0: {
            // A ticket from bytes reads back, with the same hash.
            DataStream stream{fdp.ConsumeBytes<std::byte>(fdp.ConsumeIntegralInRange<size_t>(0, 120))};
            try {
                pos::AnchorTicket ticket;
                stream >> ticket;
                DataStream again{};
                again << ticket;
                pos::AnchorTicket read;
                again >> read;
                assert(read == ticket && read.Hash() == ticket.Hash());
                pos::AnchorAck ack{.ticket = ticket, .main_height = fdp.ConsumeIntegral<int32_t>()};
                DataStream ack_stream{};
                ack_stream << ack;
                pos::AnchorAck ack_read;
                ack_stream >> ack_read;
                assert(ack_read == ack);
                // A valid payee: standard, not data, 80 bytes at most.
                if (pos::IsValidPayee(ticket.payee)) {
                    std::vector<std::vector<unsigned char>> solutions;
                    const TxoutType type{Solver(ticket.payee, solutions)};
                    assert(ticket.payee.size() <= pos::MAX_PAYEE_SIZE && type != TxoutType::NONSTANDARD && type != TxoutType::NULL_DATA);
                }
            } catch (const std::ios_base::failure&) {
            }
            break;
        }
        case 1: {
            pos::AnchorTicket ticket;
            ticket.anchored = uint256{fdp.ConsumeIntegral<uint8_t>()};
            ticket.payee = FuzzedPayee(fdp);
            const bool own{fdp.ConsumeBool()};
            // Not new: added before (it may have been pushed out since, and be new again).
            if (!hooks.AddTicket(ticket, own)) assert(added.contains(ticket.Hash()));
            added.insert(ticket.Hash());
            break;
        }
        case 2: {
            // At most MAX_PEER_TICKETS a peer (per mainchain block: none here, so for good).
            const int64_t peer{fdp.ConsumeIntegralInRange<int64_t>(0, 4)};
            const bool allowed{hooks.TicketAllowed(peer)};
            assert(allowed == (taken[peer] < pos::MAX_PEER_TICKETS));
            if (allowed) ++taken[peer];
            break;
        }
        default: {
            // Messages from peers never crash; only malformed ones or too long lists misbehave.
            const std::string type{fdp.PickValueInArray({pos::MSG_TICKET, pos::MSG_GETTICKETS, pos::MSG_EVIDENCE, "other"})};
            DataStream stream{fdp.ConsumeBytes<std::byte>(fdp.ConsumeIntegralInRange<size_t>(0, 400))};
            std::string misbehaving;
            const bool handled{hooks.ProcessMessage(fdp.ConsumeIntegralInRange<int64_t>(0, 4), type, stream, misbehaving)};
            assert(handled == (type != "other"));
        }
        }
        // Bounded, and consistent with itself.
        const auto tickets{hooks.Tickets()};
        assert(tickets.size() <= 2 * pos::MAX_TICKETS);
        std::set<uint256> hashes;
        for (const pos::AnchorTicket& ticket : tickets) {
            assert(hashes.insert(ticket.Hash()).second);
            assert(hooks.GetTicket(ticket.Hash()) == ticket);
        }
    }
}

FUZZ_TARGET(pos_evidence, .init = initialize_pos)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    static const std::vector<CKey> keys{ValidatorKeys()};
    const Consensus::PosParams& params{Hooks().Params()};
    pos::EvidencePool pool;
    LIMITED_WHILE(fdp.ConsumeBool(), 200)
    {
        CBlockHeader a{FuzzedPosHeader(fdp, keys)};
        CBlockHeader b{fdp.ConsumeBool() ? a : FuzzedPosHeader(fdp, keys)};
        if (fdp.ConsumeBool()) {
            // Two blocks of one slot, by its leader (the second differs in the merkle root).
            b = a;
            b.hashMerkleRoot = ArithToUint256(UintToArith256(a.hashMerkleRoot) + 1);
            const int64_t slot{pos::SlotOf(b.nTime, params)};
            if (slot > 0) assert(LeaderKey(keys, slot).SignSchnorr(pos::BlockSigHash(b, Hooks().Genesis()), b.ext.sig, nullptr, uint256{}));
        }
        if (fdp.ConsumeBool()) b.nVersion &= ~pos::POS_BLOCK_BIT;
        const pos::DoubleProposal proof{.a = a, .b = b};
        std::string reason;
        // Against the leader of the first header's slot (a slot of 0 has none: a null key).
        const int64_t proof_slot{pos::SlotOf(a.nTime, params)};
        const uint256 leader{proof_slot > 0 ? LeaderOf(proof_slot) : uint256{}};
        const auto offence{pos::CheckEvidence(proof, leader, Hooks().Genesis(), params, reason)};
        // Whichever header comes first.
        std::string swapped_reason;
        const auto swapped{pos::CheckEvidence({.a = b, .b = a}, leader, Hooks().Genesis(), params, swapped_reason)};
        assert(offence.has_value() == swapped.has_value());
        if (offence) {
            assert(*offence == *swapped);
            assert(pos::IsPosHeader(a) && pos::IsPosHeader(b) && a.nTime == b.nTime && a.GetHash() != b.GetHash());
            assert(offence->slot == pos::SlotOf(a.nTime, params) && pos::SlotTime(offence->slot, params) == a.nTime);
            assert(pos::CheckBlockSig(a, offence->pubkey, Hooks().Genesis()) && pos::CheckBlockSig(b, offence->pubkey, Hooks().Genesis()));
            assert(LeaderOf(offence->slot) == offence->pubkey);
            pos::DoubleProposal normal{proof};
            normal.Normalize();
            pos::DoubleProposal twice{normal};
            twice.Normalize();
            assert(twice.a.GetHash() == normal.a.GetHash() && normal.a.GetHash() < normal.b.GetHash());
            DataStream stream{};
            stream << normal;
            pos::DoubleProposal read;
            stream >> read;
            assert(read.a.GetHash() == normal.a.GetHash() && read.b.ext == normal.b.ext);
            const bool is_new{pool.Add(*offence, proof)};
            assert(!is_new || pool.Size() >= pos::MAX_EVIDENCE || pool.Get(*offence));
        } else {
            assert(!reason.empty() && reason == swapped_reason);
        }
        // The headers a node notes: a second one for a slot is a proof of two different headers.
        const int64_t slot{fdp.ConsumeIntegralInRange<int64_t>(0, 5000)};
        if (const auto found{pool.NoteSigned(fdp.ConsumeBool() ? a : b, slot, proof_slot > 0 ? leader : uint256{})}) {
            assert(found->a.GetHash() != found->b.GetHash() && found->a.GetHash() < found->b.GetHash());
        }
        if (fdp.ConsumeBool()) {
            const auto unrelayed{pool.TakeUnrelayed()};
            assert(unrelayed.size() <= pos::MAX_EVIDENCE);
            assert(pool.TakeUnrelayed().empty());
        }
        assert(pool.Size() <= pos::MAX_EVIDENCE && pool.All().size() == pool.Size());
    }
}

namespace {

/**
 * A chain of a regtest sidechain with the three validators (or none: then validators bond, and blocks
 * of stake start once a set made at an anchor has one) and a producer, and the record of its
 * mainchain the target writes.
 */
struct FastChain {
    std::unique_ptr<const CChainParams> params;
    std::unique_ptr<TestingSetup> setup;
    std::unique_ptr<pos::Producer> producer;
    std::vector<CKey> keys{[] {
        auto all{ValidatorKeys()};
        for (const CKey& k : BondingKeys()) all.push_back(k);
        return all;
    }()};
    CKey key;
    CScript script;
    int64_t slot{0};
    //! Coins of `key`: what the coinbases and payments of the chain paid it.
    std::map<COutPoint, CTxOut> coins;

    explicit FastChain(bool genesis_validators = true) : params{FastParams(genesis_validators)}
    {
        // Mock time from the start: the chain is made at a slot of its own.
        slot = pos::SlotOf(1800000000, params->GetConsensus().pos);
        SetMockTime(std::chrono::seconds{pos::SlotTime(slot, params->GetConsensus().pos)});
        pos::InstallHooks(*params);
        static const std::string validators_arg{"-posvalidators=" + VALIDATOR_XONLY[0] + "," + VALIDATOR_XONLY[1] + "," + VALIDATOR_XONLY[2]};
        std::vector<const char*> args{"-sidechainslot=5"};
        if (genesis_validators) args.push_back(validators_arg.c_str());
        setup = MakeNoLogFileContext<TestingSetup>(ChainType::REGTEST, {.extra_args = args});
        Hooks().Attach(setup->m_node.chainman.get());
        std::vector<unsigned char> secret(32, 0x42);
        key.Set(secret.begin(), secret.end(), /*fCompressedIn=*/true);
        script = GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey()});
        std::string error;
        auto protection{pos::SlashingProtection::Open(setup->m_args.GetDataDirNet() / "validator_protection.dat", setup->m_args.GetDataDirNet(), "keys.lock", error)};
        producer = std::make_unique<pos::Producer>(setup->m_node, Hooks(), keys, script, std::move(Assert(protection)));
        Main();
    }
    ~FastChain()
    {
        producer.reset();
        Hooks().Attach(nullptr);
        setup.reset();
        pos::UninstallHooks();
        SetMockTime(std::chrono::seconds{0});
    }

    ChainstateManager& Chainman() { return *Assert(setup->m_node.chainman); }
    Chainstate& Chain() { return Chainman().ActiveChainstate(); }
    sidechain::Mainchain& Record() { return *Assert(Chainman().m_mainchain); }
    const CBlockIndex* Tip() { return WITH_LOCK(::cs_main, return Chain().m_chain.Tip()); }

    void Main(const std::function<void(sidechain::MainBlock&)>& fill = {})
    {
        sidechain::MainBlock block;
        const int height{Record().Height() + 1};
        block.hash = ArithToUint256(arith_uint256{static_cast<uint64_t>(0x30000 + height)} << 128);
        block.prev_hash = Record().TipHash();
        block.time = GetTime();
        if (fill) fill(block);
        assert(Record().Append(block));
    }
    void Commit(const uint256& hash) { Main([&](sidechain::MainBlock& b) { b.bmm = hash; }); }

    /** Note the coins a newly connected block paid `key`, and those it spent. */
    void NoteCoins(const CBlock& block)
    {
        for (const CTransactionRef& tx : block.vtx) {
            for (const CTxIn& in : tx->vin) coins.erase(in.prevout);
            for (uint32_t n{0}; n < tx->vout.size(); ++n) {
                if (tx->vout[n].scriptPubKey == script && tx->vout[n].nValue > 0) coins.emplace(COutPoint{tx->GetHash(), n}, tx->vout[n]);
            }
        }
    }

    void Submit(const CBlock& block)
    {
        const auto shared{std::make_shared<const CBlock>(block)};
        Chainman().ProcessNewBlock(shared, /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/nullptr);
        setup->m_node.validation_signals->SyncWithValidationInterfaceQueue();
        if (Tip()->GetBlockHash() == block.GetHash()) NoteCoins(block);
    }

    void Bmm(bool use_mempool)
    {
        std::unique_ptr<interfaces::BlockTemplate> block_template;
        try {
            block_template = interfaces::MakeMining(setup->m_node)->createNewBlock({.use_mempool = use_mempool, .coinbase_output_script = script, .test_block_validity = false}, /*cooldown=*/false);
        } catch (const std::exception&) {
        }
        if (!block_template) return;
        CBlock block{block_template->getBlock()};
        block.hashMerkleRoot = BlockMerkleRoot(block);
        while (!CheckProofOfWork(block.GetHash(), block.nBits, Chainman().GetConsensus())) ++block.nNonce;
        Commit(block.GetHash());
        Submit(block);
    }

    void Produce(int skip)
    {
        slot += skip;
        SetMockTime(std::chrono::seconds{pos::SlotTime(slot, Hooks().Params())});
        std::string error;
        if (const auto made{producer->Produce(slot, error)}) {
            CBlock block;
            const CBlockIndex* index{WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(*made))};
            assert(Chainman().m_blockman.ReadBlock(block, *index));
            NoteCoins(block);
        }
    }

    void Pay(FuzzedDataProvider& fdp)
    {
        if (coins.empty()) return;
        auto it{coins.begin()};
        std::advance(it, fdp.ConsumeIntegralInRange<size_t>(0, coins.size() - 1));
        const auto [outpoint, spent]{*it};
        const CAmount fee{fdp.ConsumeIntegralInRange<CAmount>(1000, 100000)};
        if (spent.nValue <= fee + 1000) return;
        CMutableTransaction tx;
        tx.vin.emplace_back(outpoint);
        CAmount left{spent.nValue - fee};
        if (fdp.ConsumeBool()) {
            // A withdrawal of part of it.
            const CAmount amount{fdp.ConsumeIntegralInRange<CAmount>(1, left)};
            tx.vout.emplace_back(amount, sidechain::WithdrawalScript(fdp.ConsumeIntegralInRange<CAmount>(0, amount - 1), key.GetPubKey().GetID(), script));
            left -= amount;
        }
        if (left > 0) tx.vout.emplace_back(left, script);
        if (tx.vout.empty()) return;
        FillableSigningProvider keystore;
        keystore.AddKey(key);
        std::map<COutPoint, Coin> spent_coins{{outpoint, Coin{spent, 1, /*fCoinBaseIn=*/false}}};
        std::map<int, bilingual_str> errors;
        if (!SignTransaction(tx, &keystore, spent_coins, SignOptions{.sighash_type = SIGHASH_ALL}, errors)) return;
        (void)WITH_LOCK(::cs_main, return Chainman().ProcessTransaction(MakeTransactionRef(tx)));
    }

    /** A transaction spending a coin of `key`, with these outputs (taking their value from it) and the change; nothing if no coin pays for it. */
    std::optional<CMutableTransaction> Spend(FuzzedDataProvider& fdp, std::vector<CTxOut> outputs, const std::function<void(CMutableTransaction&)>& before_signing = {})
    {
        if (coins.empty()) return std::nullopt;
        auto it{coins.begin()};
        std::advance(it, fdp.ConsumeIntegralInRange<size_t>(0, coins.size() - 1));
        const auto [outpoint, spent]{*it};
        const CAmount fee{10000};
        CAmount left{spent.nValue - fee};
        for (const CTxOut& o : outputs) left -= o.nValue;
        if (left < 0) return std::nullopt;
        CMutableTransaction tx;
        tx.vin.emplace_back(outpoint);
        tx.vout = std::move(outputs);
        if (left > 0) tx.vout.emplace_back(left, script);
        if (before_signing) before_signing(tx);
        FillableSigningProvider keystore;
        keystore.AddKey(key);
        std::map<COutPoint, Coin> spent_coins{{outpoint, Coin{spent, 1, /*fCoinBaseIn=*/false}}};
        std::map<int, bilingual_str> errors;
        if (!SignTransaction(tx, &keystore, spent_coins, SignOptions{.sighash_type = SIGHASH_ALL}, errors)) return std::nullopt;
        return tx;
    }

    uint256 Genesis() { return Chainman().GetConsensus().hashGenesisBlock; }

    /** A bond of a fuzzed amount to one of the keys, owned by `key` (or, rarely, signed by the wrong key). */
    void Bond(FuzzedDataProvider& fdp)
    {
        const CKey& validator{keys[fdp.ConsumeIntegralInRange<size_t>(0, keys.size() - 1)]};
        const uint160 owner{key.GetPubKey().GetID()};
        std::array<unsigned char, 64> pop;
        const CKey& signer{fdp.ConsumeIntegralInRange<int>(0, 15) == 0 ? keys[0] : validator};
        if (!signer.SignSchnorr(pos::BondPopHash(owner, Genesis()), pop, nullptr, uint256{})) return;
        const CAmount amount{fdp.ConsumeIntegralInRange<CAmount>(1, 2 * Hooks().Params().min_stake + 1000)};
        // The owner's signature, which a bond to a validator with an owner needs (mostly there).
        const bool owner_signs{fdp.ConsumeIntegralInRange<int>(0, 7) != 0};
        const auto sign{[&](CMutableTransaction& tx) {
            std::vector<unsigned char> sig;
            if (owner_signs && !key.SignCompact(MessageHash(pos::BondMessage(XOnly(validator), amount, tx.vin[0].prevout, Genesis())), sig)) return;
            tx.vout[0].scriptPubKey = pos::BondScript(XOnly(validator), owner, pop, sig);
        }};
        if (const auto tx{Spend(fdp, {CTxOut{amount, pos::BondScript(XOnly(validator), owner, pop)}}, sign)}) {
            (void)WITH_LOCK(::cs_main, return Chainman().ProcessTransaction(MakeTransactionRef(*tx)));
        }
    }

    /** An unbond of part or all of what a key has bonded (or a fuzzed amount, or sequence). */
    void Unbond(FuzzedDataProvider& fdp)
    {
        const CKey& validator{keys[fdp.ConsumeIntegralInRange<size_t>(0, keys.size() - 1)]};
        const auto record{WITH_LOCK(::cs_main, return pos::GetValidator(Chain().SideCache(), XOnly(validator)))};
        if (!record || record->bonded <= 0) return;
        pos::Unbond unbond;
        unbond.pubkey = XOnly(validator);
        switch (fdp.ConsumeIntegralInRange<int>(0, 3)) {
        case 0: unbond.amount = record->bonded; break;
        case 1: unbond.amount = std::max<CAmount>(1, record->bonded - Hooks().Params().min_stake); break;
        default: unbond.amount = fdp.ConsumeIntegralInRange<CAmount>(1, record->bonded + 1);
        }
        unbond.sequence = record->unbond_seq + (fdp.ConsumeIntegralInRange<int>(0, 15) == 0);
        if (!key.SignCompact(MessageHash(pos::UnbondMessage(unbond.pubkey, unbond.amount, unbond.sequence, Genesis())), unbond.signature)) return;
        if (const auto tx{Spend(fdp, {CTxOut{0, pos::UnbondScript(unbond)}})}) {
            (void)WITH_LOCK(::cs_main, return Chainman().ProcessTransaction(MakeTransactionRef(*tx)));
        }
    }

    /**
     * The leader of a later slot signs two blocks for it: one is taken (or not), and the proof is
     * known, for the producer to put in a block.
     */
    void SignTwice(FuzzedDataProvider& fdp, int skip)
    {
        slot += skip;
        SetMockTime(std::chrono::seconds{pos::SlotTime(slot, Hooks().Params())});
        const auto leader{Hooks().ActiveLeaderOf(slot)};
        if (leader.status != pos::FastBlocksHooks::LeaderStatus::OK) return;
        const CKey* signer{nullptr};
        for (const CKey& k : keys) {
            if (XOnly(k) == leader.pubkey) signer = &k;
        }
        if (!signer) return;
        std::string error;
        auto first{producer->Build(slot, std::nullopt, error)};
        if (!first) return;
        CBlock second{*first};
        CMutableTransaction coinbase{*second.vtx[0]};
        coinbase.vout.emplace_back(0, CScript() << OP_RETURN << fdp.ConsumeIntegral<uint8_t>());
        second.vtx[0] = MakeTransactionRef(std::move(coinbase));
        node::RegenerateCommitments(second, Chainman());
        // The second header: the leader's, or one signed on another network (another genesis block,
        // whose slots line up), or by another key (the leader of the slot on another branch, say).
        const int kind{fdp.ConsumeIntegralInRange<int>(0, 3)};
        const CKey* second_signer{signer};
        if (kind == 2) {
            for (const CKey& k : keys) {
                if (XOnly(k) != leader.pubkey) second_signer = &k;
            }
        }
        const uint256 second_genesis{kind == 1 ? ArithToUint256(UintToArith256(Genesis()) + 1) : Genesis()};
        if (!pos::Producer::Sign(*first, *signer, Genesis()) || !pos::Producer::Sign(second, *second_signer, second_genesis)) return;
        if (fdp.ConsumeBool()) Submit(*first);
        std::string reason;
        const auto offence{Hooks().AddEvidence({.a = *first, .b = second}, reason)};
        // A proof only of two headers the offender signed for this chain.
        if (offence) {
            assert(pos::CheckBlockSig(*first, offence->pubkey, Genesis()) && pos::CheckBlockSig(second, offence->pubkey, Genesis()));
        }
        if (kind == 1 || (kind == 2 && second_signer != signer)) assert(!offence);
    }

    /**
     * A header from a peer, on a block the node has (of the active chain or not), of a slot that has
     * started, that claims a commitment on record or not, signed by the slot's leader, another key or
     * nobody; now and then after the node forgot the sets it read (as on a restart). What is taken
     * keeps the rules: a header whose leader the node knows is taken only with its signature; one whose
     * leader it cannot tell, only on a branch that leaves the active chain at or above its final
     * block, from a set the state did not prune; either counts one anchor at most, for a commitment
     * after the last one below it.
     */
    void PeerHeader(FuzzedDataProvider& fdp)
    {
        if (fdp.ConsumeIntegralInRange<int>(0, 3) == 0) Hooks().ForgetKnownSets();
        const CBlockIndex* prev;
        {
            LOCK(::cs_main);
            std::vector<const CBlockIndex*> known;
            for (const auto& [_, index] : Chainman().m_blockman.m_block_index) known.push_back(&index);
            std::sort(known.begin(), known.end(), [](const CBlockIndex* a, const CBlockIndex* b) { return std::make_pair(a->nHeight, a->GetBlockHash()) < std::make_pair(b->nHeight, b->GetBlockHash()); });
            prev = known[known.size() - 1 - fdp.ConsumeIntegralInRange<size_t>(0, std::min<size_t>(known.size() - 1, 40))];
        }
        const Consensus::PosParams& params{Hooks().Params()};
        CBlockHeader header;
        header.nVersion = 0x20000000 | pos::POS_BLOCK_BIT;
        header.hashPrevBlock = prev->GetBlockHash();
        header.hashMerkleRoot = ArithToUint256(arith_uint256{fdp.ConsumeIntegral<uint64_t>()});
        const int64_t at{slot - fdp.ConsumeIntegralInRange<int64_t>(0, 3)};
        header.nTime = static_cast<uint32_t>(pos::SlotTime(at, params));
        header.nBits = pos::PosBits(Chainman().GetConsensus().powLimit);
        switch (fdp.ConsumeIntegralInRange<int>(0, 2)) {
        case 0: header.nNonce = 0; break;
        case 1: {
            const int height{fdp.ConsumeIntegralInRange<int>(1, std::max(1, Record().Height()))};
            header.nNonce = Record().BmmAt(height) ? static_cast<uint32_t>(height) : 0;
            break;
        }
        default: header.nNonce = fdp.ConsumeIntegralInRange<uint32_t>(0, Record().Height() + 2);
        }
        header.ext.hashSections = header.nNonce ? uint256{1} : uint256{};
        const auto leader{WITH_LOCK(::cs_main, return Hooks().LeaderOf(*prev, at))};
        switch (fdp.ConsumeIntegralInRange<int>(0, 2)) {
        case 0:
            for (const CKey& k : keys) {
                if (leader.status == pos::FastBlocksHooks::LeaderStatus::OK && XOnly(k) == leader.pubkey) assert(k.SignSchnorr(pos::BlockSigHash(header, Genesis()), header.ext.sig, nullptr, uint256{}));
            }
            break;
        case 1: assert(keys[fdp.ConsumeIntegralInRange<size_t>(0, keys.size() - 1)].SignSchnorr(pos::BlockSigHash(header, Genesis()), header.ext.sig, nullptr, uint256{})); break;
        default: break;
        }
        const bool had{WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(header.GetHash()) != nullptr)};
        BlockValidationState state;
        Chainman().ProcessNewBlockHeaders({{header}}, /*min_pow_checked=*/true, state);
        LOCK(::cs_main);
        const CBlockIndex* index{Chainman().m_blockman.LookupBlockIndex(header.GetHash())};
        if (!index || had) return;
        const pos::ChainKey before{pos::ChainKey::FromWork(prev->nChainWork)};
        const pos::ChainKey after{pos::ChainKey::FromWork(index->nChainWork)};
        assert(after.anchors <= before.anchors + 1);
        if (header.nNonce != 0) assert(header.nNonce > before.last_anchor_height && after.last_anchor_height == header.nNonce);
        assert(leader.status == pos::FastBlocksHooks::LeaderStatus::OK || leader.status == pos::FastBlocksHooks::LeaderStatus::SET_UNKNOWN);
        if (leader.status == pos::FastBlocksHooks::LeaderStatus::OK) assert(pos::CheckBlockSig(header, leader.pubkey, Genesis()));
        if (leader.status == pos::FastBlocksHooks::LeaderStatus::SET_UNKNOWN) {
            const CChain& active{Chain().m_chain};
            assert(!leader.set_anchor || !active.Contains(*leader.set_anchor));
            const CBlockIndex* fork{active.FindFork(*prev)};
            assert(!fork || fork == active.Tip() || fork->nHeight >= Hooks().FinalHeight());
        }
    }

    /** The totals of staking agree with its validators and its queue of unbonds; none is negative. */
    void CheckStake()
    {
        LOCK(::cs_main);
        const sidechain::StoreView& view{Chain().SideCache()};
        const pos::StakeState stake{pos::GetStakeState(view)};
        assert(stake.bonded >= 0 && stake.unbonding >= 0 && stake.pot_reserve >= 0 && stake.rewards_owed >= 0 && stake.slashed >= 0);
        CAmount bonded{0}, unbonding{0}, queued{0};
        pos::ForEachValidator(view, [&](const uint256&, const pos::ValidatorRecord& record) {
            assert(record.bonded >= 0);
            for (const pos::UnbondingEntry& entry : record.unbonding) assert(entry.amount > 0);
            assert(!record.tombstoned || record.bonded == 0);
            bonded += record.bonded;
            unbonding += record.Unbonding();
            return true;
        });
        pos::ForEachUnbonding(view, [&](uint64_t, const uint256&, CAmount amount) {
            assert(amount > 0);
            queued += amount;
            return true;
        });
        assert(bonded == stake.bonded);
        assert(unbonding == stake.unbonding && queued == stake.unbonding);
    }

    /** The coins of the chain against the deposits the state acted on (no fee is left unclaimed here). */
    void CheckPegBalance()
    {
        LOCK(::cs_main);
        Chain().ForceFlushStateToDisk(/*wipe_cache=*/false);
        const auto stats{kernel::ComputeUTXOStats(kernel::CoinStatsHashType::NONE, Chain().CoinsDB(), Chainman().m_blockman)};
        assert(stats && stats->total_amount);
        const sidechain::State side{Chain().SideState()};
        CAmount total{*stats->total_amount};
        side.ForEachWithdrawal([&](const sidechain::Withdrawal& w) {
            total += w.Burned();
            return true;
        });
        for (const CTxOut& out : side.Queue()) total += out.nValue;
        for (const CTxOut& out : side.TxQueue()) total += out.nValue;
        const pos::FastState fast{pos::GetFastState(Chain().SideState().View())};
        assert(0 <= fast.pot_paid && fast.pot_paid <= fast.pot_accrued);
        total += fast.pot_accrued - fast.pot_paid;
        // Staking: coins bonded, unbonding, penalties in the reserve, rewards owed.
        const pos::StakeState stake{pos::GetStakeState(Chain().SideState().View())};
        total += stake.bonded + stake.unbonding + stake.pot_reserve + stake.rewards_owed;
        CAmount deposited{0};
        for (int h{0}; h <= side.MainHeight(); ++h) {
            const auto block{Record().GetBlock(h)};
            for (const sidechain::MainDeposit& deposit : Assert(block)->deposits) deposited += deposit.amount;
        }
        assert(total == deposited);
    }

    /** The state agrees with the chain key. */
    void CheckKey()
    {
        LOCK(::cs_main);
        const CBlockIndex* tip{Chain().m_chain.Tip()};
        const pos::FastState fast{pos::GetFastState(Chain().SideState().View())};
        const pos::ChainKey chain_key{pos::ChainKey::FromWork(tip->nChainWork)};
        assert(chain_key.anchors == fast.anchors && chain_key.height == static_cast<uint64_t>(tip->nHeight));
        if (fast.anchors > 0) assert(chain_key.last_anchor_height == static_cast<uint32_t>(fast.last_main_height));
        if (tip->pprev) assert(tip->nChainWork > tip->pprev->nChainWork);
    }

    uint256 StateHash()
    {
        LOCK(::cs_main);
        return sidechain::StoreHash(Chain().SideCache(), {}, {});
    }
};

} // namespace

FUZZ_TARGET(pos_state)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    FastChain chain;
    // A BMM block first, so that the chain has an anchor point; it pays a deposit to the key, for
    // payments, bonds and unbonds.
    chain.Main([&](sidechain::MainBlock& block) {
        sidechain::MainDeposit deposit;
        deposit.destination = EncodeDestination(WitnessV0KeyHash{chain.key.GetPubKey()});
        deposit.amount = 10 * COIN;
        deposit.txid = uint256{0xde};
        block.deposits.push_back(deposit);
    });
    chain.Bmm(/*use_mempool=*/false);
    LIMITED_WHILE(fdp.ConsumeBool(), 60)
    {
        switch (fdp.ConsumeIntegralInRange<int>(0, 11)) {
        case 11:
            chain.PeerHeader(fdp);
            break;
        case 8:
            chain.Bond(fdp);
            break;
        case 9:
            chain.Unbond(fdp);
            break;
        case 10:
            chain.SignTwice(fdp, fdp.ConsumeIntegralInRange<int>(1, 4));
            break;
        case 0:
        case 1:
            // A block of stake, at a later slot (with the acknowledgement of a committed ticket, if one is due).
            chain.Produce(fdp.ConsumeIntegralInRange<int>(1, 4));
            break;
        case 2: {
            // A ticket for a recent block of the chain, committed to.
            pos::AnchorTicket ticket;
            const CBlockIndex* tip{chain.Tip()};
            const CBlockIndex* target{WITH_LOCK(::cs_main, return tip->GetAncestor(std::max(0, tip->nHeight - fdp.ConsumeIntegralInRange<int>(0, 3))))};
            ticket.anchored = target->GetBlockHash();
            ticket.payee = fdp.ConsumeBool() ? chain.script : FuzzedPayee(fdp);
            chain.Commit(ticket.Hash());
            if (fdp.ConsumeBool()) Hooks().AddTicket(ticket);
            break;
        }
        case 3:
            // A deposit to the key.
            chain.Main([&](sidechain::MainBlock& block) {
                sidechain::MainDeposit deposit;
                deposit.destination = EncodeDestination(WitnessV0KeyHash{chain.key.GetPubKey()});
                deposit.amount = fdp.ConsumeIntegralInRange<CAmount>(1, 10 * COIN);
                deposit.txid = ArithToUint256(arith_uint256{static_cast<uint64_t>(chain.Record().Height() + 1)});
                block.deposits.push_back(deposit);
            });
            break;
        case 4:
            chain.Bmm(fdp.ConsumeBool());
            break;
        case 5:
            chain.Pay(fdp);
            break;
        case 6:
            chain.Main();
            break;
        default: {
            // Take back the tip and connect it again: the same state.
            const uint256 before{chain.StateHash()};
            CBlockIndex* tip{WITH_LOCK(::cs_main, return chain.Chain().m_chain.Tip())};
            if (tip->nHeight == 0) break;
            BlockValidationState state;
            chain.Chain().InvalidateBlock(state, tip);
            {
                // (As reconsiderblock does.)
                LOCK(::cs_main);
                chain.Chain().ResetBlockFailureFlags(tip);
                chain.Chainman().RecalculateBestHeader();
            }
            chain.Chain().ActivateBestChain(state);
            if (chain.Tip() == tip) assert(chain.StateHash() == before);
        }
        }
        chain.CheckKey();
        chain.CheckStake();
    }
    chain.CheckPegBalance();
}

namespace {

/**
 * FastBlocks hooks rebuilt mid-chain (a node that restarted: nothing in memory but what it reads from
 * the chain's state) against those that saw the chain from its start: the rules of blocks (all of a
 * block's state transition), the time limit of blocks, the final height, the chain key of a header and
 * the leaders a header is checked against must be the same. Only what the warm hooks know from memory
 * (the sets of anchors they read before, which the state may have pruned) may make them tell a leader
 * the cold ones cannot.
 */
struct ColdHooks {
    FastChain& chain;
    std::unique_ptr<sidechain::ChainHooks> owner;
    pos::FastBlocksHooks* cold{nullptr};

    explicit ColdHooks(FastChain& c) : chain{c} {}
    ~ColdHooks() { Reset(); }

    void Reset()
    {
        if (cold) cold->Attach(nullptr);
        cold = nullptr;
        owner.reset();
    }
    void Rebuild()
    {
        Reset();
        auto hooks{std::make_unique<pos::FastBlocksHooks>(*chain.params)};
        hooks->Attach(&chain.Chainman());
        for (const pos::AnchorTicket& ticket : Hooks().Tickets()) hooks->AddTicket(ticket);
        cold = hooks.get();
        owner = std::move(hooks);
    }

    /** The state transition of `block` on the tip, with the installed hooks or the cold ones: its outcome and the state after. */
    std::pair<std::string, uint256> Connect(const CBlock& block, bool use_cold)
    {
        // (The template reads the hooks through sidechain::Hooks(): the cold ones are put in its place for the call.)
        std::unique_ptr<sidechain::ChainHooks> warm;
        if (use_cold) warm = sidechain::SetHooks(std::move(owner));
        std::pair<std::string, uint256> result;
        {
            LOCK(::cs_main);
            const sidechain::StoreView& base{chain.Chain().SideCache()};
            sidechain::StoreOverlay overlay{base};
            sidechain::State state{overlay};
            CAmount minted{0};
            std::string reason;
            const bool ok{state.ConnectBlock(block, chain.Tip()->nHeight + 1, chain.Chainman().GetConsensus().sidechain, chain.Record(), minted, reason)};
            result = {ok ? "" : reason, ok ? sidechain::StoreHash(overlay) : uint256{}};
        }
        if (use_cold) owner = sidechain::SetHooks(std::move(warm));
        return result;
    }

    void CompareBlock(const CBlock& block)
    {
        assert(Connect(block, /*use_cold=*/false) == Connect(block, /*use_cold=*/true));
    }

    /** A block of stake the producer would make for a slot to come (with an acknowledgement if one is due), signed by its leader. */
    std::optional<CBlock> PosCandidate(int64_t slot)
    {
        std::optional<pos::AnchorAck> ack;
        pos::FastBlocksHooks::SlotLeader leader;
        {
            LOCK(::cs_main);
            const CBlockIndex* tip{chain.Tip()};
            ack = Hooks().NextAck(*tip, pos::GetFastState(chain.Chain().SideCache()), slot);
            leader = Hooks().LeaderOf(*tip, slot);
        }
        if (leader.status != pos::FastBlocksHooks::LeaderStatus::OK) return std::nullopt;
        std::string error;
        auto block{chain.producer->Build(slot, ack, error)};
        if (!block && ack) block = chain.producer->Build(slot, std::nullopt, error);
        if (!block) return std::nullopt;
        for (const CKey& k : chain.keys) {
            if (XOnly(k) == leader.pubkey && pos::Producer::Sign(*block, k, chain.Genesis())) return block;
        }
        return std::nullopt;
    }

    void Compare(FuzzedDataProvider& fdp)
    {
        assert(Hooks().MaxFutureBlockTime() == cold->MaxFutureBlockTime());
        const Consensus::PosParams& params{Hooks().Params()};
        {
            LOCK(::cs_main);
            // (Finality is what a node knows: the cold hooks have the tickets the warm ones have, as a
            // node that restarted asks its peers for them.)
            assert(Hooks().FinalHeight() == cold->FinalHeight());
            // Leaders of slots now and to come, on the tip and on blocks below it or off the active chain.
            std::vector<const CBlockIndex*> known;
            for (const auto& [_, index] : chain.Chainman().m_blockman.m_block_index) known.push_back(&index);
            std::sort(known.begin(), known.end(), [](const CBlockIndex* a, const CBlockIndex* b) { return std::make_pair(a->nHeight, a->GetBlockHash()) < std::make_pair(b->nHeight, b->GetBlockHash()); });
            for (int n{0}; n < 3; ++n) {
                const CBlockIndex* prev{known[known.size() - 1 - fdp.ConsumeIntegralInRange<size_t>(0, std::min<size_t>(known.size() - 1, 40))]};
                const int64_t at{chain.slot + fdp.ConsumeIntegralInRange<int64_t>(-3, 3 * params.slots_per_epoch)};
                if (at <= 0) continue;
                const auto warm_leader{Hooks().LeaderOf(*prev, at)};
                const auto cold_leader{cold->LeaderOf(*prev, at)};
                // (Either may know a set the other does not, from memory: the warm hooks forget too, now and then.)
                if (warm_leader.status != pos::FastBlocksHooks::LeaderStatus::SET_UNKNOWN && cold_leader.status != pos::FastBlocksHooks::LeaderStatus::SET_UNKNOWN) {
                    assert(warm_leader.status == cold_leader.status);
                    assert(warm_leader.pubkey == cold_leader.pubkey && warm_leader.seed == cold_leader.seed);
                }
                // A header on it, signed by the leader or another key or nobody, claiming a commitment or not:
                // the same outcome where both tell its leader, and the same chain key.
                CBlockHeader header;
                header.nVersion = 0x20000000 | pos::POS_BLOCK_BIT;
                header.hashPrevBlock = prev->GetBlockHash();
                header.hashMerkleRoot = ArithToUint256(arith_uint256{fdp.ConsumeIntegral<uint64_t>()});
                header.nTime = static_cast<uint32_t>(pos::SlotTime(at, params));
                header.nBits = pos::PosBits(chain.Chainman().GetConsensus().powLimit);
                header.nNonce = fdp.ConsumeBool() ? 0 : fdp.ConsumeIntegralInRange<uint32_t>(0, chain.Record().Height() + 1);
                header.ext.hashSections = header.nNonce ? uint256{1} : uint256{};
                const CKey& signer{chain.keys[fdp.ConsumeIntegralInRange<size_t>(0, chain.keys.size() - 1)]};
                for (const CKey& k : chain.keys) {
                    if (warm_leader.status == pos::FastBlocksHooks::LeaderStatus::OK && XOnly(k) == warm_leader.pubkey && fdp.ConsumeBool()) {
                        assert(k.SignSchnorr(pos::BlockSigHash(header, chain.Genesis()), header.ext.sig, nullptr, uint256{}));
                    }
                }
                if (fdp.ConsumeIntegralInRange<int>(0, 3) == 0) assert(signer.SignSchnorr(pos::BlockSigHash(header, chain.Genesis()), header.ext.sig, nullptr, uint256{}));
                assert(Hooks().ChainWork(prev, header) == cold->ChainWork(prev, header));
                if (warm_leader.status == pos::FastBlocksHooks::LeaderStatus::OK && cold_leader.status == pos::FastBlocksHooks::LeaderStatus::OK) {
                    BlockValidationState warm_state, cold_state;
                    const bool warm_ok{Hooks().CheckHeader(header, *prev, chain.Chainman(), warm_state)};
                    const bool cold_ok{cold->CheckHeader(header, *prev, chain.Chainman(), cold_state)};
                    assert(warm_ok == cold_ok && warm_state.GetRejectReason() == cold_state.GetRejectReason());
                }
            }
        }
        // Blocks to come: a block of stake, a BMM block (with or without the mempool).
        if (const auto block{PosCandidate(chain.slot + fdp.ConsumeIntegralInRange<int64_t>(1, 3))}) CompareBlock(*block);
        if (fdp.ConsumeBool()) {
            std::unique_ptr<interfaces::BlockTemplate> block_template;
            try {
                block_template = interfaces::MakeMining(chain.setup->m_node)->createNewBlock({.use_mempool = fdp.ConsumeBool(), .coinbase_output_script = chain.script, .test_block_validity = false}, /*cooldown=*/false);
            } catch (const std::exception&) {
            }
            if (block_template) {
                CBlock block{block_template->getBlock()};
                block.hashMerkleRoot = BlockMerkleRoot(block);
                CompareBlock(block);
            }
        }
    }
};

} // namespace

FUZZ_TARGET(pos_cold_hooks)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    // With genesis validators, or without (validators bond, and blocks of stake start once a set made
    // at an anchor has one: the network's time limit of blocks comes from the state then).
    FastChain chain{/*genesis_validators=*/fdp.ConsumeBool()};
    ColdHooks hooks{chain};
    chain.Main([&](sidechain::MainBlock& block) {
        sidechain::MainDeposit deposit;
        deposit.destination = EncodeDestination(WitnessV0KeyHash{chain.key.GetPubKey()});
        deposit.amount = 10 * COIN;
        deposit.txid = uint256{0xde};
        block.deposits.push_back(deposit);
    });
    chain.Bmm(/*use_mempool=*/false);
    hooks.Rebuild();
    LIMITED_WHILE(fdp.remaining_bytes() > 0, 60)
    {
        switch (fdp.ConsumeIntegralInRange<int>(0, 11)) {
        case 0:
            // A restart: the hooks rebuilt from the chain as it is now (the installed ones forget what
            // they read too, now and then).
            hooks.Rebuild();
            if (fdp.ConsumeBool()) Hooks().ForgetKnownSets();
            break;
        case 1:
        case 2:
            chain.Produce(fdp.ConsumeIntegralInRange<int>(1, 4));
            break;
        case 3: {
            pos::AnchorTicket ticket;
            const CBlockIndex* tip{chain.Tip()};
            const CBlockIndex* target{WITH_LOCK(::cs_main, return tip->GetAncestor(std::max(0, tip->nHeight - fdp.ConsumeIntegralInRange<int>(0, 3))))};
            ticket.anchored = target->GetBlockHash();
            ticket.payee = fdp.ConsumeBool() ? chain.script : FuzzedPayee(fdp);
            chain.Commit(ticket.Hash());
            if (fdp.ConsumeBool()) {
                Hooks().AddTicket(ticket);
                if (hooks.cold) hooks.cold->AddTicket(ticket);
            }
            break;
        }
        case 4:
            chain.Main([&](sidechain::MainBlock& block) {
                sidechain::MainDeposit deposit;
                deposit.destination = EncodeDestination(WitnessV0KeyHash{chain.key.GetPubKey()});
                deposit.amount = fdp.ConsumeIntegralInRange<CAmount>(1, 10 * COIN);
                deposit.txid = ArithToUint256(arith_uint256{static_cast<uint64_t>(chain.Record().Height() + 1)});
                block.deposits.push_back(deposit);
            });
            break;
        case 5:
            chain.Bmm(fdp.ConsumeBool());
            break;
        case 6:
            chain.Pay(fdp);
            break;
        case 7:
            chain.Bond(fdp);
            break;
        case 8:
            chain.Unbond(fdp);
            break;
        case 9:
            chain.SignTwice(fdp, fdp.ConsumeIntegralInRange<int>(1, 4));
            break;
        case 10:
            chain.Main();
            break;
        default: {
            // A block taken back and connected again.
            CBlockIndex* tip{WITH_LOCK(::cs_main, return chain.Chain().m_chain.Tip())};
            if (tip->nHeight == 0) break;
            BlockValidationState state;
            chain.Chain().InvalidateBlock(state, tip);
            if (hooks.cold) hooks.Compare(fdp);
            {
                LOCK(::cs_main);
                chain.Chain().ResetBlockFailureFlags(tip);
                chain.Chainman().RecalculateBestHeader();
            }
            chain.Chain().ActivateBestChain(state);
        }
        }
        if (hooks.cold) hooks.Compare(fdp);
    }
}

namespace {
void initialize_staking()
{
    static const auto testing_setup{MakeNoLogFileContext<>()};
}
} // namespace

FUZZ_TARGET(pos_staking, .init = initialize_staking)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    // Small parameters, fuzzed within what the networks could set.
    Consensus::PosParams params;
    params.slots_per_epoch = fdp.ConsumeIntegralInRange<int64_t>(1, 8);
    params.min_active_validators = fdp.ConsumeIntegralInRange<uint32_t>(0, 3);
    params.max_validators = fdp.ConsumeIntegralInRange<uint32_t>(0, 4);
    params.min_stake = fdp.ConsumeIntegralInRange<int64_t>(1, 1000);
    params.unbonding_anchors = fdp.ConsumeIntegralInRange<int64_t>(1, 8);
    params.evidence_window_anchors = fdp.ConsumeIntegralInRange<int64_t>(0, params.unbonding_anchors - 1);
    params.slash_bps = fdp.ConsumeIntegralInRange<int64_t>(0, 10000);
    params.reporter_share_divisor = fdp.ConsumeIntegralInRange<int64_t>(0, 16);
    params.liveness_min_bps = fdp.ConsumeIntegralInRange<int64_t>(0, 10000);
    params.liveness_min_assigned = fdp.ConsumeIntegralInRange<int64_t>(0, 5);
    params.jail_epochs = fdp.ConsumeIntegralInRange<int64_t>(0, 3);
    params.pot_reserve_anchors = fdp.ConsumeIntegralInRange<int64_t>(0, 5);
    static const std::vector<CKey> keys{[] {
        auto all{ValidatorKeys()};
        for (const CKey& k : BondingKeys()) all.push_back(k);
        return all;
    }()};
    // Owners: two keys.
    static const std::vector<CKey> owners{[] {
        std::vector<CKey> all;
        for (uint8_t i{1}; i <= 2; ++i) {
            std::vector<unsigned char> secret(32, 0x33);
            secret[31] = i;
            CKey key;
            key.Set(secret.begin(), secret.end(), /*fCompressedIn=*/true);
            all.push_back(key);
        }
        return all;
    }()};
    const int genesis_count{fdp.ConsumeIntegralInRange<int>(0, 2)};
    for (int i{0}; i < genesis_count; ++i) params.genesis_validators.push_back({.pubkey = XOnly(keys[i]), .stake = fdp.ConsumeIntegralInRange<int64_t>(1, 100)});
    const uint256 genesis{1};
    const pos::Staking staking{params, genesis};

    sidechain::EmptyStore empty;
    sidechain::StoreOverlay state{empty, /*journal=*/true};
    // Coins that came into staking (bonds) and that went out (payouts, the reserve's shares).
    CAmount in{0}, out{0};
    uint64_t anchors{0};
    int64_t slot{0};
    const auto check{[&]() {
        const pos::StakeState stake{pos::GetStakeState(state)};
        assert(stake.bonded >= 0 && stake.unbonding >= 0 && stake.pot_reserve >= 0 && stake.rewards_owed >= 0 && stake.slashed >= 0);
        assert(in == out + stake.bonded + stake.unbonding + stake.pot_reserve + stake.rewards_owed);
        CAmount bonded{0}, unbonding{0}, queued{0};
        pos::ForEachValidator(state, [&](const uint256&, const pos::ValidatorRecord& record) {
            assert(record.bonded >= 0);
            assert(!record.tombstoned || (record.bonded == 0 && !record.Eligible(params)));
            for (const pos::UnbondingEntry& entry : record.unbonding) assert(entry.amount > 0);
            bonded += record.bonded;
            unbonding += record.Unbonding();
            return true;
        });
        pos::ForEachUnbonding(state, [&](uint64_t release, const uint256&, CAmount amount) {
            assert(amount > 0 && release > anchors);
            queued += amount;
            return true;
        });
        assert(bonded == stake.bonded && unbonding == stake.unbonding && queued == stake.unbonding);
        // A set: at most max_validators, none banned or jailed, the largest weights.
        const int64_t epoch{pos::EpochOf(slot, params)};
        const pos::ValidatorSet set{staking.MakeSet(state, epoch)};
        assert(set.Validators().size() <= params.max_validators);
        for (const Consensus::PosValidator& validator : set.Validators()) {
            const auto record{pos::GetValidator(state, validator.pubkey)};
            assert(record && record->Eligible(params) && record->jailed_until_epoch <= epoch && record->Weight() == validator.stake);
        }
    }};
    LIMITED_WHILE(fdp.ConsumeBool(), 200)
    {
        const uint256 before{sidechain::StoreHash(state)};
        const CAmount in_before{in}, out_before{out};
        const uint64_t anchors_before{anchors};
        // Each step on an overlay of its own, kept if it keeps the rules (as the state does).
        sidechain::StoreOverlay step{static_cast<const sidechain::StoreView&>(state)};
        bool kept{true};
        switch (fdp.ConsumeIntegralInRange<int>(0, 4)) {
        case 0: {
            // A transaction of bonds and unbonds.
            CMutableTransaction tx;
            // A coin it spends: what the owner's signature of a bond names.
            tx.vin.emplace_back(COutPoint{Txid::FromUint256(ArithToUint256(arith_uint256{fdp.ConsumeIntegral<uint64_t>()})), fdp.ConsumeIntegral<uint32_t>()});
            CAmount bonding{0};
            LIMITED_WHILE(fdp.ConsumeBool(), 3)
            {
                const CKey& validator{keys[fdp.ConsumeIntegralInRange<size_t>(0, keys.size() - 1)]};
                const CKey& owner{owners[fdp.ConsumeIntegralInRange<size_t>(0, owners.size() - 1)]};
                if (fdp.ConsumeBool()) {
                    const uint160 owner_hash{owner.GetPubKey().GetID()};
                    std::array<unsigned char, 64> pop;
                    const CKey& signer{fdp.ConsumeIntegralInRange<int>(0, 15) == 0 ? keys[0] : validator};
                    assert(signer.SignSchnorr(pos::BondPopHash(owner_hash, genesis), pop, nullptr, uint256{}));
                    const CAmount amount{fdp.ConsumeBool() ? fdp.ConsumeIntegralInRange<CAmount>(-1, 3 * params.min_stake) : fdp.ConsumeIntegral<CAmount>()};
                    // The owner's signature (a bond to a validator with an owner needs it), or none.
                    std::vector<unsigned char> owner_sig;
                    if (fdp.ConsumeBool()) assert(owner.SignCompact(MessageHash(pos::BondMessage(XOnly(validator), amount, tx.vin[0].prevout, genesis)), owner_sig));
                    tx.vout.emplace_back(amount, pos::BondScript(XOnly(validator), owner_hash, pop, owner_sig));
                    bonding += amount > 0 && MoneyRange(amount) ? amount : 0;
                } else {
                    const auto record{pos::GetValidator(step, XOnly(validator))};
                    pos::Unbond unbond;
                    unbond.pubkey = XOnly(validator);
                    // (0 is UNBOND_ALL: all that is bonded then.)
                    unbond.amount = fdp.ConsumeBool() && record ? record->bonded : fdp.ConsumeIntegralInRange<CAmount>(0, 3 * params.min_stake);
                    unbond.sequence = (record ? record->unbond_seq : 0) + fdp.ConsumeIntegralInRange<uint32_t>(0, 1);
                    assert(owner.SignCompact(MessageHash(pos::UnbondMessage(unbond.pubkey, unbond.amount, unbond.sequence, genesis)), unbond.signature));
                    tx.vout.emplace_back(fdp.ConsumeIntegralInRange<int>(0, 15) == 0 ? 1 : 0, pos::UnbondScript(unbond));
                }
            }
            std::string reason;
            kept = staking.ApplyTx(step, CTransaction{tx}, fdp.ConsumeIntegralInRange<int>(0, 1000), anchors, reason);
            assert(kept || !reason.empty());
            // (A bond counted, if the transaction is: one that does not read or count fails it.)
            if (kept) in += bonding;
            break;
        }
        case 1: {
            // An anchor's peg step.
            ++anchors;
            slot += fdp.ConsumeIntegralInRange<int64_t>(0, 3 * params.slots_per_epoch);
            const int64_t epoch{std::max<int64_t>(0, pos::EpochOf(slot, params) - fdp.ConsumeIntegralInRange<int64_t>(0, 1))};
            std::vector<CTxOut> owed;
            const auto result{staking.PegStep(step, ArithToUint256(arith_uint256{anchors}), anchors, slot, epoch, owed)};
            assert(result.release >= 0);
            out += result.release;
            for (const CTxOut& o : owed) {
                assert(o.nValue >= 0);
                out += o.nValue;
            }
            const auto entry{pos::GetAnchorSet(step, ArithToUint256(arith_uint256{anchors}))};
            assert(entry && entry->anchors == anchors && pos::GetSetVersion(step, entry->version));
            break;
        }
        case 2: {
            // A block of stake, and the slots it skipped.
            slot += fdp.ConsumeIntegralInRange<int64_t>(1, 4);
            std::vector<std::pair<int64_t, uint256>> missed;
            LIMITED_WHILE(fdp.ConsumeBool(), 4) missed.emplace_back(slot - fdp.ConsumeIntegralInRange<int64_t>(1, 4), XOnly(keys[fdp.ConsumeIntegralInRange<size_t>(0, keys.size() - 1)]));
            staking.NoteLiveness(step, slot, XOnly(keys[fdp.ConsumeIntegralInRange<size_t>(0, keys.size() - 1)]), missed);
            break;
        }
        case 3: {
            // A proof that a validator signed twice for a slot.
            const pos::Offence offence{.pubkey = XOnly(keys[fdp.ConsumeIntegralInRange<size_t>(0, keys.size() - 1)]), .slot = fdp.ConsumeIntegralInRange<int64_t>(0, slot + 1)};
            const auto record{pos::GetValidator(step, offence.pubkey)};
            const CAmount stake{record ? record->bonded + record->Unbonding() : 0};
            const bool known{pos::OffencePunished(step, offence.pubkey, offence.slot)};
            // Slashed once: a banned validator loses nothing more.
            const bool banned{record && record->tombstoned};
            // The offender carrying the proof itself: no reporter, the reward to the reserve.
            const bool self_reported{fdp.ConsumeBool()};
            const auto stake_before{pos::GetStakeState(step)};
            std::string reason;
            kept = staking.Punish(step, offence, self_reported ? std::nullopt : std::optional<CScript>{CScript() << OP_TRUE}, anchors, reason);
            assert(kept == (!known && !banned));
            if (kept && self_reported) assert(pos::GetStakeState(step).rewards_owed == stake_before.rewards_owed);
            if (kept) {
                const auto after{pos::GetValidator(step, offence.pubkey)};
                assert(after && after->tombstoned && after->bonded == 0);
                // slash_bps of the stake, the rest kept (unbonding).
                assert(after->Unbonding() == stake - staking.Penalty(stake));
                assert(pos::OffencePunished(step, offence.pubkey, offence.slot));
            }
            break;
        }
        default:
            // A set made at some epoch (check, below).
            slot += fdp.ConsumeIntegralInRange<int64_t>(0, params.slots_per_epoch);
            break;
        }
        if (!kept) {
            in = in_before;
            out = out_before;
            anchors = anchors_before;
            continue;
        }
        step.MergeInto(state);
        check();
        // Now and then, the step taken back by the journal.
        if (fdp.ConsumeIntegralInRange<int>(0, 7) == 0) {
            state.Revert(state.TakeUndo());
            // (The revert's own changes are no step to take back.)
            (void)state.TakeUndo();
            assert(sidechain::StoreHash(state) == before);
            in = in_before;
            out = out_before;
            anchors = anchors_before;
        } else {
            (void)state.TakeUndo();
        }
    }
    check();
}
