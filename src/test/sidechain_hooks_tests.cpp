// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <blockencodings.h>
#include <chain.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <hash.h>
#include <interfaces/mining.h>
#include <node/miner.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <sidechain/hooks.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <txmempool.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

// The generic extension points of the template (sidechain/hooks.h): what they do by default, which
// must be the template's behaviour exactly, and what they do when a sidechain switches them on.

namespace {

/** Hooks of a sidechain with extended blocks of its own kind, which take no peg actions. */
struct TestHooks : sidechain::ChainHooks {
    bool BlockExtensions() const override { return true; }
    sidechain::BlockKind Kind(const CBlockHeader& header) const override { return header.IsExtended() ? sidechain::BlockKind::PLUGIN : sidechain::BlockKind::BMM; }
    bool CustomChainOrder() const override { return true; }
    arith_uint256 ChainWork(const CBlockIndex* prev, const CBlockHeader&) const override { return (prev ? prev->nChainWork : arith_uint256{0}) + 7; }
    bool RefundsOnlyInPegSteps() const override { return true; }
    bool GetPegStep(const CBlock& block, const sidechain::Mainchain& mainchain, std::optional<sidechain::PegStep>& step, std::string& reject_reason) const override
    {
        if (!block.IsExtended()) return ChainHooks::GetPegStep(block, mainchain, step, reject_reason);
        step.reset();
        return true;
    }
};

/** Installs hooks for the life of the object; the template's again after. */
struct InstallHooks {
    explicit InstallHooks(std::unique_ptr<sidechain::ChainHooks> hooks) { sidechain::SetHooks(std::move(hooks)); }
    ~InstallHooks() { sidechain::SetHooks(nullptr); }
};

CBlockHeader Header(bool extension_bit)
{
    CBlockHeader header;
    header.nVersion = 0x20000000 | (extension_bit ? BLOCK_EXTENSION_BIT : 0);
    header.hashPrevBlock = uint256{1};
    header.hashMerkleRoot = uint256{2};
    header.nTime = 1700000000;
    header.nBits = 0x207fffff;
    header.nNonce = 3;
    header.ext.hashSections = uint256{4};
    header.ext.sig.fill(5);
    return header;
}

/** SHA256d of the 80 bytes of Bitcoin's header, and of what follows them in the hash. */
uint256 HashOf(const CBlockHeader& h, const std::optional<uint256>& sections = std::nullopt)
{
    HashWriter hasher{};
    hasher << h.nVersion << h.hashPrevBlock << h.hashMerkleRoot << h.nTime << h.nBits << h.nNonce;
    if (sections) hasher << *sections;
    return hasher.GetHash();
}

CBlock CoinbaseBlock(bool extension_bit, std::vector<CTxOut> extra_outputs = {})
{
    CBlock block{Header(extension_bit)};
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << 2 << OP_0;
    coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
    coinbase.vout.insert(coinbase.vout.end(), extra_outputs.begin(), extra_outputs.end());
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    block.hashMerkleRoot = BlockMerkleRoot(block);
    return block;
}

void AppendMain(sidechain::Mainchain& mainchain, int64_t time, std::optional<uint256> bmm = std::nullopt)
{
    sidechain::MainBlock block;
    block.hash = uint256{static_cast<uint8_t>(0x80 + mainchain.Height() + 1)};
    block.prev_hash = mainchain.TipHash();
    block.time = time;
    block.bmm = bmm;
    BOOST_REQUIRE(mainchain.Append(block));
}

/** A transaction with these outputs, spending some coin. */
CTransaction Tx(std::vector<CTxOut> outputs, uint8_t salt = 0x77)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{salt}), 1});
    tx.vout = std::move(outputs);
    return CTransaction{tx};
}

CTxOut WithdrawalOut(CAmount amount)
{
    const uint160 keyhash{*uint160::FromHex("0101010101010101010101010101010101010101")};
    return CTxOut{amount, sidechain::WithdrawalScript(1000, keyhash, GetScriptForDestination(WitnessV0KeyHash{keyhash}))};
}

/**
 * Hooks with peg actions of their own: a block that takes the peg actions owes 7 satoshis to the
 * script of its coinbase's first output, and every block notes what its coinbase paid out, under a
 * table byte of its own.
 */
const sidechain::Table<uint32_t, int64_t> MINTED_TABLE{0xF0};

struct OwedHooks : sidechain::ChainHooks {
    mutable std::vector<CBlock> seen;
    bool BeforePegStep(sidechain::State&, const CBlock& block, int, const sidechain::PegStep&, const Consensus::SidechainParams&,
                       std::vector<CTxOut>& owed_tx, std::string&) const override
    {
        seen.push_back(block);
        owed_tx.emplace_back(7, block.vtx.at(0)->vout.at(0).scriptPubKey);
        return true;
    }
    bool AfterBlock(sidechain::State& state, const CBlock&, int height, const std::optional<sidechain::PegStep>&, CAmount minted,
                    const Consensus::SidechainParams&, std::string&) const override
    {
        MINTED_TABLE.Put(state.Writable(), static_cast<uint32_t>(height), minted);
        return true;
    }
};

/** Hooks that let blocks be at most a minute ahead of the clock. */
struct NearTimeHooks : sidechain::ChainHooks {
    int64_t MaxFutureBlockTime() const override { return 60; }
};

/**
 * Hooks with a chain order of their own that reads something that can change, as a record of the
 * mainchain does: each block adds 1 and `bonus` to the key of its parent.
 */
struct BonusHooks : sidechain::ChainHooks {
    mutable int64_t bonus{0};
    bool CustomChainOrder() const override { return true; }
    arith_uint256 ChainWork(const CBlockIndex* prev, const CBlockHeader&) const override
    {
        return (prev ? prev->nChainWork : arith_uint256{0}) + arith_uint256{static_cast<uint64_t>(1 + bonus)};
    }
};

/** A chain that is a sidechain in slot 3, with a record of its mainchain the test writes. */
struct SidechainSetup : public TestingSetup {
    SidechainSetup() : TestingSetup{ChainType::REGTEST, {.extra_args = {"-sidechainslot=3"}}} {}
    sidechain::Mainchain& Record() { return *Assert(m_node.chainman->m_mainchain); }

    /** A block on the tip, with this time (the clock's if none), its proof of work done. */
    CBlock Build(std::optional<int64_t> time = std::nullopt)
    {
        auto block_template{interfaces::MakeMining(m_node)->createNewBlock({.use_mempool = false, .coinbase_output_script = CScript() << OP_TRUE, .test_block_validity = false}, /*cooldown=*/false)};
        BOOST_REQUIRE(block_template);
        CBlock block{block_template->getBlock()};
        if (time) block.nTime = static_cast<uint32_t>(*time);
        block.hashMerkleRoot = BlockMerkleRoot(block);
        while (!CheckProofOfWork(block.GetHash(), block.nBits, m_node.chainman->GetConsensus())) ++block.nNonce;
        return block;
    }
    /** Have the next mainchain block commit to `hash`. */
    void Commit(const uint256& hash)
    {
        sidechain::MainBlock main;
        main.hash = uint256{static_cast<uint8_t>(0x40 + Record().Height() + 1)};
        main.prev_hash = Record().TipHash();
        main.bmm = hash;
        BOOST_REQUIRE(Record().Append(main));
    }
    /** Commit to a block and give it to the node. */
    void Take(const CBlock& block)
    {
        Commit(block.GetHash());
        BOOST_REQUIRE(m_node.chainman->ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/nullptr));
        BOOST_REQUIRE(WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash()) == block.GetHash());
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(sidechain_hooks_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(template_defaults)
{
    // Without hooks of a sidechain's own, every hook does what the template does.
    const sidechain::ChainHooks& hooks{sidechain::Hooks()};
    BOOST_CHECK(!hooks.BlockExtensions());
    BOOST_CHECK(!BlockExtensionsEnabled());
    BOOST_CHECK(!hooks.CustomChainOrder());
    BOOST_CHECK(!hooks.RefundsOnlyInPegSteps());

    // Every block is merged mined, the extension bit included: it means nothing.
    const CBlockHeader plain{Header(false)}, flagged{Header(true)};
    BOOST_CHECK(hooks.Kind(plain) == sidechain::BlockKind::BMM);
    BOOST_CHECK(hooks.Kind(flagged) == sidechain::BlockKind::BMM);

    // Blocks may be two hours ahead of the clock, as Bitcoin's.
    BOOST_CHECK_EQUAL(hooks.MaxFutureBlockTime(), MAX_FUTURE_BLOCK_TIME);

    // Headers wait only for a commitment.
    BOOST_CHECK(hooks.IsWaitReason("bmm-unknown"));
    BOOST_CHECK(!hooks.IsWaitReason("bmm-prev-unknown"));
    BOOST_CHECK(!hooks.IsWaitReason("bad-pos-sig"));

    // The chain with the most work: that of the parent plus the header's.
    CBlockIndex prev{plain};
    prev.nChainWork = 5;
    BOOST_CHECK(hooks.ChainWork(&prev, plain) == arith_uint256{5} + GetBlockProof(plain));
    BOOST_CHECK(hooks.ChainWork(nullptr, plain) == GetBlockProof(plain));

    // A block needs a commitment to itself, and only it rests on that commitment.
    const uint256 hash{plain.GetHash()};
    CBlockIndex index{plain};
    index.phashBlock = &hash;
    BOOST_CHECK(hooks.CommitmentOf(index) == hash);
    BOOST_CHECK(hooks.BlocksCommittedBy(uint256{9}) == std::vector<uint256>{uint256{9}});

    // No kinds of blocks of its own, no rules of its own, no messages of its own.
    BlockValidationState state;
    BOOST_CHECK(!hooks.CheckHeader(plain, prev, *m_node.chainman, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-block-kind");
    BlockValidationState block_state;
    BOOST_CHECK(hooks.CheckBlock(CoinbaseBlock(false), block_state));
    BOOST_CHECK(block_state.IsValid());
    DataStream recv{};
    std::string misbehaving;
    BOOST_CHECK(!hooks.ProcessMessage(1, "ticket", recv, misbehaving));
    BOOST_CHECK(misbehaving.empty());

    // The sections hash is a tagged hash, of the bytes as they are.
    const std::vector<unsigned char> sections{1, 2, 3};
    BOOST_CHECK(hooks.SectionsHash(sections) == (TaggedHash("sidechain/sections") << std::span<const unsigned char>{sections}).GetSHA256());
    BOOST_CHECK(hooks.SectionsHash(sections) != hooks.SectionsHash(std::vector<unsigned char>{1, 2}));

    // Peg actions in every block, as committed: up to the mainchain block before the commitment,
    // with the parent as the bundle nonce, and no limit on the withdrawals a bundle takes.
    const CBlock block{CoinbaseBlock(false)};
    sidechain::Mainchain mainchain;
    AppendMain(mainchain, 100);
    std::optional<sidechain::PegStep> step;
    std::string reason;
    BOOST_CHECK(!hooks.GetPegStep(block, mainchain, step, reason));
    BOOST_CHECK_EQUAL(reason, "bmm-unknown");
    BOOST_CHECK(!hooks.WaitingReady(block, mainchain));
    AppendMain(mainchain, 101, block.GetHash());
    BOOST_CHECK(hooks.GetPegStep(block, mainchain, step, reason));
    BOOST_REQUIRE(step);
    BOOST_CHECK(*step == (sidechain::PegStep{.main_height = 0, .nonce = block.hashPrevBlock, .max_withdrawal_height = std::nullopt}));
    BOOST_CHECK(hooks.WaitingReady(block, mainchain) == std::optional<int64_t>{1});

    // The sidechain's own peg actions and notes: none.
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State side{store};
    std::vector<CTxOut> owed_tx;
    BOOST_CHECK(hooks.BeforePegStep(side, block, 1, *step, Params().GetConsensus().sidechain, owed_tx, reason));
    BOOST_CHECK(owed_tx.empty());
    BOOST_CHECK(hooks.AfterBlock(side, block, 1, step, /*minted=*/0, Params().GetConsensus().sidechain, reason));
    BOOST_CHECK(store.Changes().empty());

    // Proof of work for every header.
    CBlockHeader mined{plain};
    while (!CheckProofOfWork(mined.GetHash(), mined.nBits, Params().GetConsensus())) ++mined.nNonce;
    BOOST_CHECK(sidechain::CheckHeaderWork(mined, Params().GetConsensus()));
    CBlockHeader unmined{plain};
    unmined.nBits = 0x1d00ffff;
    BOOST_CHECK(!sidechain::CheckHeaderWork(unmined, Params().GetConsensus()));
}

BOOST_AUTO_TEST_CASE(extensions_off)
{
    // Off, the extension bit changes nothing: Bitcoin's 80 bytes, Bitcoin's hash, no sections.
    const CBlockHeader header{Header(true)};
    BOOST_CHECK(!header.IsExtended());
    DataStream ss{};
    ss << header;
    BOOST_CHECK_EQUAL(ss.size(), 80U);
    BOOST_CHECK(header.GetHash() == HashOf(header));
    CBlockHeader no_work{header};
    no_work.nBits = 0;
    BOOST_CHECK(no_work.IsNull());

    CBlock block{CoinbaseBlock(true)};
    CBlock without{block};
    block.vSections = {1, 2, 3};
    BOOST_CHECK_EQUAL(GetSerializeSize(TX_WITH_WITNESS(block)), GetSerializeSize(TX_WITH_WITNESS(without)));
    BOOST_CHECK(sidechain::CheckSections(block));

    // The block index stores what it always did.
    CBlockIndex index{header};
    BOOST_CHECK(!index.m_ext);
    DataStream disk{};
    disk << CDiskBlockIndex{&index};
    CDiskBlockIndex read;
    disk >> read;
    BOOST_CHECK(disk.empty());
    BOOST_CHECK(!read.m_ext);
}

BOOST_AUTO_TEST_CASE(extensions_on)
{
    InstallHooks installed{std::make_unique<TestHooks>()};
    BOOST_CHECK(BlockExtensionsEnabled());

    // An extended header: the 80 bytes and the extension; the hash covers hashSections, not the signature.
    CBlockHeader header{Header(true)};
    BOOST_CHECK(header.IsExtended());
    DataStream ss{};
    ss << header;
    BOOST_CHECK_EQUAL(ss.size(), 80U + 32U + 64U);
    CBlockHeader read;
    ss >> read;
    BOOST_CHECK(read.ext == header.ext);
    BOOST_CHECK(read.GetHash() == header.GetHash());
    BOOST_CHECK(header.GetHash() == HashOf(header, header.ext.hashSections));
    CBlockHeader resigned{header};
    resigned.ext.sig.fill(6);
    BOOST_CHECK(resigned.GetHash() == header.GetHash());
    CBlockHeader other_sections{header};
    other_sections.ext.hashSections = uint256{7};
    BOOST_CHECK(other_sections.GetHash() != header.GetHash());
    // Without proof of work, a header all the same.
    CBlockHeader no_work{header};
    no_work.nBits = 0;
    BOOST_CHECK(!no_work.IsNull());
    BOOST_CHECK(sidechain::CheckHeaderWork(no_work, Params().GetConsensus()));

    // A header without the bit is Bitcoin's, as before.
    const CBlockHeader plain{Header(false)};
    DataStream plain_ss{};
    plain_ss << plain;
    BOOST_CHECK_EQUAL(plain_ss.size(), 80U);
    BOOST_CHECK(plain.GetHash() == HashOf(plain));

    // The sections of an extended block go after its transactions, and the header commits to them.
    CBlock block{CoinbaseBlock(true)};
    block.vSections = {1, 2, 3};
    block.ext.hashSections = sidechain::Hooks().SectionsHash(block.vSections);
    BOOST_CHECK(sidechain::CheckSections(block));
    DataStream block_ss{};
    block_ss << TX_WITH_WITNESS(block);
    CBlock block_read;
    block_ss >> TX_WITH_WITNESS(block_read);
    BOOST_CHECK(block_read.vSections == block.vSections);
    BOOST_CHECK(block_read.GetHash() == block.GetHash());
    // No sections: a null hash.
    CBlock empty{CoinbaseBlock(true)};
    empty.ext.hashSections.SetNull();
    BOOST_CHECK(sidechain::CheckSections(empty));
    empty.ext.hashSections = uint256{1};
    BOOST_CHECK(!sidechain::CheckSections(empty));

    // Sections other than those committed to: a mutated block, never cached as invalid.
    CBlock mutated{block};
    mutated.vSections = {1, 2, 4};
    BOOST_CHECK(IsBlockMutated(mutated, /*check_witness_root=*/false));
    BOOST_CHECK(!IsBlockMutated(block, /*check_witness_root=*/false));
    BlockValidationState state;
    BOOST_CHECK(!CheckBlock(mutated, state, Params().GetConsensus(), /*fCheckPOW=*/true, /*fCheckMerkleRoot=*/true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-sections-hash");
    BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_MUTATED);
    BlockValidationState ok_state;
    BOOST_CHECK_MESSAGE(CheckBlock(block, ok_state, Params().GetConsensus(), /*fCheckPOW=*/true, /*fCheckMerkleRoot=*/true), ok_state.ToString());

    // The block index keeps the extension, and gives the header back as it was.
    CBlockIndex index{block};
    BOOST_REQUIRE(index.m_ext);
    BOOST_CHECK(*index.m_ext == block.ext);
    BOOST_CHECK(index.GetBlockHeader().ext == block.ext);
    DataStream disk{};
    disk << CDiskBlockIndex{&index};
    CDiskBlockIndex disk_read;
    disk >> disk_read;
    BOOST_CHECK(disk.empty());
    BOOST_REQUIRE(disk_read.m_ext);
    BOOST_CHECK(*disk_read.m_ext == block.ext);
    CBlockHeader unlinked{static_cast<const CBlockHeader&>(block)};
    unlinked.hashPrevBlock.SetNull();
    BOOST_CHECK(disk_read.ConstructBlockHash() == unlinked.GetHash());

    // Compact blocks carry the sections whole.
    const CBlockHeaderAndShortTxIDs compact{block, /*nonce=*/42};
    DataStream compact_ss{};
    compact_ss << compact;
    CBlockHeaderAndShortTxIDs compact_read;
    compact_ss >> compact_read;
    BOOST_CHECK(compact_read.sections == block.vSections);
    PartiallyDownloadedBlock partial{m_node.mempool.get()};
    BOOST_REQUIRE(partial.InitData(compact_read, {}) == READ_STATUS_OK);
    CBlock filled;
    BOOST_REQUIRE(partial.FillBlock(filled, {}, /*segwit_active=*/true) == READ_STATUS_OK);
    BOOST_CHECK(filled.vSections == block.vSections);
    BOOST_CHECK(filled.GetHash() == block.GetHash());

    // The chain order of the sidechain's.
    BOOST_CHECK(sidechain::Hooks().CustomChainOrder());
    BOOST_CHECK(sidechain::Hooks().ChainWork(nullptr, header) == arith_uint256{7});
}

BOOST_AUTO_TEST_CASE(hooks_reinstalled)
{
    // Installing hooks gives back those before; nullptr puts the template's back, extended blocks off.
    BOOST_CHECK(!sidechain::SetHooks(std::make_unique<TestHooks>()));
    BOOST_CHECK(BlockExtensionsEnabled());
    const auto before{sidechain::SetHooks(nullptr)};
    BOOST_CHECK(before);
    BOOST_CHECK(!BlockExtensionsEnabled());
    BOOST_CHECK(!sidechain::Hooks().CustomChainOrder());
}

BOOST_AUTO_TEST_CASE(mainchain_time_height)
{
    sidechain::Mainchain mainchain;
    BOOST_CHECK(!mainchain.TimeHeight(1000));
    BOOST_CHECK(!mainchain.HashAt(0));
    AppendMain(mainchain, 100);
    AppendMain(mainchain, 100);
    AppendMain(mainchain, 150);
    AppendMain(mainchain, 200);
    // The highest block with a median time at or below the time.
    BOOST_CHECK(!mainchain.TimeHeight(99));
    BOOST_CHECK(mainchain.TimeHeight(100) == std::optional<int>{1});
    BOOST_CHECK(mainchain.TimeHeight(149) == std::optional<int>{1});
    BOOST_CHECK(mainchain.TimeHeight(150) == std::optional<int>{2});
    BOOST_CHECK(mainchain.TimeHeight(199) == std::optional<int>{2});
    BOOST_CHECK(mainchain.TimeHeight(1000) == std::optional<int>{3});
    BOOST_CHECK(mainchain.HashAt(2) == mainchain.GetBlock(2)->hash);
    BOOST_CHECK(mainchain.HashAt(3) == mainchain.TipHash());
    BOOST_CHECK(!mainchain.HashAt(4));
    BOOST_CHECK(!mainchain.HashAt(-1));
    mainchain.Truncate(1);
    BOOST_CHECK(mainchain.TimeHeight(1000) == std::optional<int>{1});
}

BOOST_AUTO_TEST_CASE(bundle_withdrawal_height)
{
    // A bundle may be limited to withdrawals made at or below a height; by default it takes any.
    Consensus::SidechainParams params;
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    std::vector<CTxOut> payouts;
    std::string reason;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(Tx({WithdrawalOut(COIN)}, 1), 1, params, payouts, reason), reason);
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(Tx({WithdrawalOut(2 * COIN)}, 2), 5, params, payouts, reason), reason);
    std::vector<COutPoint> all, low, none;
    BOOST_REQUIRE(state.NextBundle(10, uint256{0xa}, params, &all));
    BOOST_CHECK_EQUAL(all.size(), 2U);
    BOOST_REQUIRE(state.NextBundle(10, uint256{0xa}, params, &low, /*max_withdrawal_height=*/3));
    BOOST_REQUIRE_EQUAL(low.size(), 1U);
    BOOST_CHECK_EQUAL(state.GetWithdrawal(low[0])->height, 1);
    BOOST_CHECK(!state.NextBundle(10, uint256{0xa}, params, &none, /*max_withdrawal_height=*/0));
    // Started with the limit, the bundle has the withdrawals of the limit.
    const auto bundle{state.NextBundle(10, uint256{0xa}, params, nullptr, 3)};
    BOOST_CHECK(!state.StartBundle(bundle->GetHash().ToUint256(), 10, uint256{0xa}, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-sc-bundle-hash");
    BOOST_REQUIRE_MESSAGE(state.StartBundle(bundle->GetHash().ToUint256(), 10, uint256{0xa}, params, reason, 3), reason);
    BOOST_CHECK(state.Bundle()->withdrawals == low);
}

BOOST_AUTO_TEST_CASE(payouts_without_peg_step)
{
    // A block without the peg actions pays nothing: what is owed waits, in order, for one that has them.
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    const std::vector<CTxOut> owed{{1, CScript() << OP_TRUE}}, owed_tx{{2, CScript() << OP_TRUE}};
    BOOST_CHECK(state.TakePayouts(owed, owed_tx, 0).empty());
    BOOST_CHECK(state.Queue() == owed);
    BOOST_CHECK(state.TxQueue() == owed_tx);
    const auto paid{state.TakePayouts({}, {})};
    BOOST_REQUIRE_EQUAL(paid.size(), 2U);
    BOOST_CHECK(paid[0] == owed[0]);
    BOOST_CHECK(paid[1] == owed_tx[0]);
    BOOST_CHECK(state.Queue().empty() && state.TxQueue().empty());
}

BOOST_AUTO_TEST_CASE(block_without_peg_step)
{
    InstallHooks installed{std::make_unique<TestHooks>()};
    const Consensus::SidechainParams& params{Params().GetConsensus().sidechain};
    sidechain::Mainchain mainchain;
    AppendMain(mainchain, 100);
    sidechain::EmptyStore empty;
    CAmount minted{-1};
    std::string reason;

    // A block of the sidechain's own kind that takes no peg actions: it acts on nothing the
    // mainchain did, and needs no commitment.
    {
        sidechain::StoreOverlay store{empty, /*journal=*/true};
        sidechain::State state{store};
        BOOST_CHECK_MESSAGE(state.ConnectBlock(CoinbaseBlock(true), 1, params, mainchain, minted, reason), reason);
        BOOST_CHECK_EQUAL(minted, 0);
        BOOST_CHECK_EQUAL(state.MainHeight(), -1);
    }
    // A block of the template's kind still needs its commitment.
    {
        sidechain::StoreOverlay store{empty, /*journal=*/true};
        sidechain::State state{store};
        BOOST_CHECK(!state.ConnectBlock(CoinbaseBlock(false), 1, params, mainchain, minted, reason));
        BOOST_CHECK_EQUAL(reason, "bmm-unknown");
    }
    // It cannot start a bundle.
    {
        sidechain::StoreOverlay store{empty, /*journal=*/true};
        sidechain::State state{store};
        BOOST_CHECK(!state.ConnectBlock(CoinbaseBlock(true, {CTxOut{0, sidechain::BundleCommitScript(uint256{1})}}), 1, params, mainchain, minted, reason));
        BOOST_CHECK_EQUAL(reason, "bad-sc-bundle-not-peg");
    }
    // Nor take a refund request, when refunds wait for the peg actions.
    sidechain::RefundRequest request;
    request.withdrawal = COutPoint{Txid::FromUint256(uint256{3}), 0};
    request.signature.assign(65, 1);
    const CTransaction refund{Tx({CTxOut{0, sidechain::RefundScript(request)}})};
    {
        sidechain::StoreOverlay store{empty, /*journal=*/true};
        sidechain::State state{store};
        CBlock block{CoinbaseBlock(true)};
        block.vtx.push_back(MakeTransactionRef(refund));
        BOOST_CHECK(!state.ConnectBlock(block, 1, params, mainchain, minted, reason));
        BOOST_CHECK_EQUAL(reason, "bad-sc-refund-not-peg");
        std::vector<CTxOut> payouts;
        BOOST_CHECK(!state.ApplyTx(refund, 1, params, payouts, reason, /*main_pending=*/false, /*peg_step=*/false));
        BOOST_CHECK_EQUAL(reason, "bad-sc-refund-not-peg");
        // With the peg actions it is checked as any refund (and this one refunds nothing).
        BOOST_CHECK(!state.ApplyTx(refund, 1, params, payouts, reason, /*main_pending=*/false, /*peg_step=*/true));
        BOOST_CHECK_EQUAL(reason, "bad-sc-refund-unknown");
    }
    // Withdrawals are made in it all the same.
    {
        sidechain::StoreOverlay store{empty, /*journal=*/true};
        sidechain::State state{store};
        CBlock block{CoinbaseBlock(true)};
        block.vtx.push_back(MakeTransactionRef(Tx({WithdrawalOut(COIN)})));
        BOOST_CHECK_MESSAGE(state.ConnectBlock(block, 1, params, mainchain, minted, reason), reason);
        size_t withdrawals{0};
        state.ForEachWithdrawal([&](const sidechain::Withdrawal&) { ++withdrawals; return true; });
        BOOST_CHECK_EQUAL(withdrawals, 1U);
    }
}

BOOST_AUTO_TEST_CASE(template_refunds_anywhere)
{
    // In the template a refund request is checked as before whatever the block: the switch is off.
    Consensus::SidechainParams params;
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    sidechain::RefundRequest request;
    request.withdrawal = COutPoint{Txid::FromUint256(uint256{3}), 0};
    request.signature.assign(65, 1);
    std::vector<CTxOut> payouts;
    std::string reason;
    BOOST_CHECK(!state.ApplyTx(Tx({CTxOut{0, sidechain::RefundScript(request)}}), 1, params, payouts, reason, /*main_pending=*/false, /*peg_step=*/false));
    BOOST_CHECK_EQUAL(reason, "bad-sc-refund-unknown");
}

BOOST_AUTO_TEST_CASE(own_peg_actions)
{
    // What a sidechain's own peg actions owe is paid by the coinbase, as refunds are; the notes it
    // takes of a block (AfterBlock) see what the coinbase paid out, and go in the state.
    InstallHooks installed{std::make_unique<OwedHooks>()};
    const Consensus::SidechainParams& params{Params().GetConsensus().sidechain};
    sidechain::Mainchain mainchain;
    AppendMain(mainchain, 100);
    sidechain::EmptyStore empty;
    CAmount minted{-1};
    std::string reason;
    CBlock block{CoinbaseBlock(false)};
    AppendMain(mainchain, 101, block.GetHash());
    {
        // The coinbase does not pay it.
        sidechain::StoreOverlay store{empty, /*journal=*/true};
        sidechain::State state{store};
        BOOST_CHECK(!state.ConnectBlock(block, 1, params, mainchain, minted, reason));
        BOOST_CHECK_EQUAL(reason, "bad-sc-payouts-missing");
    }
    CBlock paying{CoinbaseBlock(false, {CTxOut{7, CScript() << OP_TRUE}})};
    AppendMain(mainchain, 102, paying.GetHash());
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    BOOST_CHECK_MESSAGE(state.ConnectBlock(paying, 1, params, mainchain, minted, reason), reason);
    BOOST_CHECK_EQUAL(minted, 7);
    BOOST_CHECK(MINTED_TABLE.Get(state.View(), 1) == std::optional<int64_t>{7});
    // Taken back with the block's undo data.
    store.Revert(store.TakeUndo());
    BOOST_CHECK(!MINTED_TABLE.Get(store, 1));
}

BOOST_FIXTURE_TEST_CASE(assembly_takes_own_peg_actions, SidechainSetup)
{
    // Block assembly calls the sidechain's own peg actions too, with the block as far as it is known
    // (parent, sections, a coinbase paying to the coinbase script), and its coinbase pays what they owe.
    auto owned{std::make_unique<OwedHooks>()};
    const OwedHooks& hooks{*owned};
    InstallHooks installed{std::move(owned)};
    sidechain::MainBlock main;
    main.hash = uint256{0x51};
    BOOST_REQUIRE(Record().Append(main));
    const CScript script{CScript() << OP_2};
    node::BlockCreateOptions options;
    options.coinbase_output_script = script;
    options.test_block_validity = false;
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
    const uint256 tip{WITH_LOCK(::cs_main, return chainstate.m_chain.Tip()->GetBlockHash())};

    // The template's block: the peg actions as of the next mainchain block.
    {
        const auto block_template{node::BlockAssembler{chainstate, m_node.mempool.get(), options}.CreateNewBlock()};
        BOOST_REQUIRE_EQUAL(hooks.seen.size(), 1U);
        BOOST_CHECK(hooks.seen[0].hashPrevBlock == tip);
        BOOST_CHECK(hooks.seen[0].vtx.at(0)->vout.at(0) == CTxOut(0, script));
        BOOST_CHECK(hooks.seen[0].vSections.empty());
        const CTransaction& coinbase{*block_template->block.vtx.at(0)};
        BOOST_REQUIRE_GE(coinbase.vout.size(), 2U);
        BOOST_CHECK(coinbase.vout[1] == CTxOut(7, script));
    }
    // A block with the peg actions of a given step, and sections.
    node::SidechainAssembly assembly;
    assembly.peg = node::SidechainAssembly::Peg::STEP;
    assembly.step = sidechain::PegStep{.main_height = 0, .nonce = tip, .max_withdrawal_height = std::nullopt};
    assembly.sections = {1, 2, 3};
    {
        const auto block_template{node::BlockAssembler{chainstate, m_node.mempool.get(), options, assembly}.CreateNewBlock()};
        BOOST_REQUIRE_EQUAL(hooks.seen.size(), 2U);
        BOOST_CHECK(hooks.seen[1].vSections == assembly.sections);
        BOOST_CHECK(block_template->block.vtx.at(0)->vout.at(1) == CTxOut(7, script));
    }
    // A block without the peg actions: none of the sidechain's own either, and nothing paid.
    assembly.peg = node::SidechainAssembly::Peg::NONE;
    {
        const auto block_template{node::BlockAssembler{chainstate, m_node.mempool.get(), options, assembly}.CreateNewBlock()};
        BOOST_CHECK_EQUAL(hooks.seen.size(), 2U);
        for (const CTxOut& out : block_template->block.vtx.at(0)->vout) BOOST_CHECK(out != CTxOut(7, script));
    }
}

BOOST_FIXTURE_TEST_CASE(future_block_time, SidechainSetup)
{
    // A header an hour and a half ahead of the clock: within Bitcoin's two hours, the template takes it.
    sidechain::MainBlock first;
    first.hash = uint256{0x3f};
    BOOST_REQUIRE(Record().Append(first));
    const int64_t ahead{GetTime() + 90 * 60};
    {
        const CBlock block{Build(ahead)};
        Commit(block.GetHash());
        BlockValidationState state;
        BOOST_CHECK_MESSAGE(m_node.chainman->ProcessNewBlockHeaders({{block}}, /*min_pow_checked=*/true, state), state.ToString());
    }
    // A sidechain that allows a minute turns it away, as a header that may come later; a block
    // half a minute ahead it takes.
    InstallHooks installed{std::make_unique<NearTimeHooks>()};
    const CBlock block{Build(GetTime() + 90)};
    Commit(block.GetHash());
    BlockValidationState state;
    BOOST_CHECK(!m_node.chainman->ProcessNewBlockHeaders({{block}}, /*min_pow_checked=*/true, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "time-too-new");
    BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_TIME_FUTURE);
    const CBlock near{Build(GetTime() + 30)};
    Commit(near.GetHash());
    BlockValidationState near_state;
    BOOST_CHECK_MESSAGE(m_node.chainman->ProcessNewBlockHeaders({{near}}, /*min_pow_checked=*/true, near_state), near_state.ToString());
}

BOOST_FIXTURE_TEST_CASE(chain_work_recomputed, SidechainSetup)
{
    // A chain order that reads the record is computed anew for a block, and all above it, when the
    // record takes back its failure (its commitment came back, perhaps at another height).
    auto owned{std::make_unique<BonusHooks>()};
    const BonusHooks& hooks{*owned};
    InstallHooks installed{std::move(owned)};
    sidechain::MainBlock first;
    first.hash = uint256{0x3f};
    BOOST_REQUIRE(Record().Append(first));
    ChainstateManager& chainman{*m_node.chainman};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    const arith_uint256 base{WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()->nChainWork)};
    const CBlock a{Build()};
    Take(a);
    const CBlock b{Build()};
    Take(b);
    CBlockIndex* index_a{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(a.GetHash()))};
    CBlockIndex* index_b{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(b.GetHash()))};
    BOOST_CHECK(index_a->nChainWork == base + 1);
    BOOST_CHECK(index_b->nChainWork == base + 2);

    // The record drops the commitment to a, and it comes back: what the key reads changed meanwhile.
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, index_a, /*by_record=*/true));
    hooks.bonus = 10;
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainstate.ReconsiderRecordFailure(index_a)));
    BOOST_CHECK(index_a->nChainWork == base + 11);
    BOOST_CHECK(index_b->nChainWork == base + 22);
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.m_best_header) == index_b);
    BOOST_REQUIRE(chainstate.ActivateBestChain(state));
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()) == index_b);

    // With Bitcoin's order the work is the parent's and the header's, whatever was before.
    sidechain::SetHooks(nullptr);
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, index_a, /*by_record=*/true));
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainstate.ReconsiderRecordFailure(index_a)));
    BOOST_CHECK(index_a->nChainWork == base + 11);
}

BOOST_AUTO_TEST_SUITE_END()
