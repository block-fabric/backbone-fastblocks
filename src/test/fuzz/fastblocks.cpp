// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// FastBlocks under fuzzing (doc/fastblocks.md):
//  - pos_header: extended headers and blocks read back as written, their hash covers hashSections and
//    not the signature, sections decode only in their one encoding, slots and the chain key are
//    consistent, and the context-free block rules never crash;
//  - pos_ticket: tickets read back, the pool of tickets stays bounded and consistent whatever peers
//    send, and the count of tickets a peer may send holds;
//  - pos_evidence: a proof is an offence exactly when two different headers of one slot are signed
//    by its leader, whichever comes first, and the pool of proofs stays bounded;
//  - pos_state: a chain of blocks of stake, acknowledgements, BMM blocks, deposits, payments and
//    withdrawals, built by the producer and the template from fuzzed choices: the state agrees with
//    the chain key, the coins of the chain are the deposits (pegbalance), and taking blocks back and
//    connecting them again comes back to the same state.

#include <addresstype.h>
#include <chain.h>
#include <chainparams.h>
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

//! The x-only keys of ValidatorKeys (known before any secp256k1 context exists).
const std::vector<std::string> VALIDATOR_XONLY{
    "100f6d8cbf94afb6fc58e9c384b9b3a6516091373a83c869f4e24a9d2bb4a494",
    "45d3b9ce0f54f4d6a17edfe3f9e0993b94d6b299c1a6e5a728ff036ecd9e139f",
    "57a62b05e99914350ce87639a68d0f3dd588e98afaf6c1131235a855d41962f3",
};

/** The chain parameters of a regtest sidechain in slot 5 with the three validators. */
std::unique_ptr<const CChainParams> FastParams()
{
    CChainParams::RegTestOptions options;
    options.sidechain_slot = 5;
    for (const std::string& hex : VALIDATOR_XONLY) options.pos_validators.push_back({.pubkey = uint256{std::span<const unsigned char>{ParseHex(hex)}}, .stake = 1});
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

const CKey& LeaderKey(const std::vector<CKey>& keys, int64_t slot)
{
    const auto leader{Hooks().Validators().Leader(slot, Hooks().Params(), g_params->GetConsensus().hashGenesisBlock)};
    const uint256 pubkey{Hooks().Validators().Validators()[*Assert(leader)].pubkey};
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
            assert(LeaderKey(keys, time_slot).SignSchnorr(pos::BlockSigHash(header), header.ext.sig, nullptr, uint256{}));
        }
        break;
    }
    case 1: assert(keys[fdp.ConsumeIntegralInRange<size_t>(0, keys.size() - 1)].SignSchnorr(pos::BlockSigHash(header), header.ext.sig, nullptr, uint256{})); break;
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

    // Signatures: the leader's signature checks for the header it signed, and only for it.
    {
        static const std::vector<CKey> keys{ValidatorKeys()};
        const CBlockHeader header{FuzzedPosHeader(fdp, keys)};
        for (const CKey& key : keys) {
            if (pos::CheckBlockSig(header, XOnly(key))) {
                CBlockHeader other{header};
                other.nNonce ^= 1;
                assert(!pos::CheckBlockSig(other, XOnly(key)));
            }
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
    const uint256 genesis{g_params->GetConsensus().hashGenesisBlock};
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
            if (slot > 0) assert(LeaderKey(keys, slot).SignSchnorr(pos::BlockSigHash(b), b.ext.sig, nullptr, uint256{}));
        }
        if (fdp.ConsumeBool()) b.nVersion &= ~pos::POS_BLOCK_BIT;
        const pos::DoubleProposal proof{.a = a, .b = b};
        std::string reason;
        const auto offence{pos::CheckEvidence(proof, Hooks().Validators(), params, genesis, reason)};
        // Whichever header comes first.
        std::string swapped_reason;
        const auto swapped{pos::CheckEvidence({.a = b, .b = a}, Hooks().Validators(), params, genesis, swapped_reason)};
        assert(offence.has_value() == swapped.has_value());
        if (offence) {
            assert(*offence == *swapped);
            assert(pos::IsPosHeader(a) && pos::IsPosHeader(b) && a.nTime == b.nTime && a.GetHash() != b.GetHash());
            assert(offence->slot == pos::SlotOf(a.nTime, params) && pos::SlotTime(offence->slot, params) == a.nTime);
            assert(pos::CheckBlockSig(a, offence->pubkey) && pos::CheckBlockSig(b, offence->pubkey));
            assert(Hooks().Validators().Leader(offence->slot, params, genesis) == Hooks().Validators().Find(offence->pubkey));
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
        if (const auto found{pool.NoteSigned(fdp.ConsumeBool() ? a : b, slot)}) {
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

/** A chain of a regtest sidechain with the three validators and a producer, and the record of its mainchain the target writes. */
struct FastChain {
    std::unique_ptr<const CChainParams> params{FastParams()};
    std::unique_ptr<TestingSetup> setup;
    std::unique_ptr<pos::Producer> producer;
    std::vector<CKey> keys{ValidatorKeys()};
    CKey key;
    CScript script;
    int64_t slot{0};
    //! Coins of `key`: what the coinbases and payments of the chain paid it.
    std::map<COutPoint, CTxOut> coins;

    FastChain()
    {
        // Mock time from the start: the chain is made at a slot of its own.
        slot = pos::SlotOf(1800000000, params->GetConsensus().pos);
        SetMockTime(std::chrono::seconds{pos::SlotTime(slot, params->GetConsensus().pos)});
        pos::InstallHooks(*params);
        static const std::string validators_arg{"-posvalidators=" + VALIDATOR_XONLY[0] + "," + VALIDATOR_XONLY[1] + "," + VALIDATOR_XONLY[2]};
        setup = MakeNoLogFileContext<TestingSetup>(ChainType::REGTEST, {.extra_args = {"-sidechainslot=5", validators_arg.c_str()}});
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
        WITH_LOCK(::cs_main, Chainman().ProcessTransaction(MakeTransactionRef(tx)));
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
    // A BMM block first, so that the chain has an anchor point.
    chain.Bmm(/*use_mempool=*/false);
    LIMITED_WHILE(fdp.ConsumeBool(), 40)
    {
        switch (fdp.ConsumeIntegralInRange<int>(0, 7)) {
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
    }
    chain.CheckPegBalance();
}
