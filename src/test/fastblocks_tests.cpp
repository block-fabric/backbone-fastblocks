// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// FastBlocks: blocks of stake signed by the leader of their slot, anchors and their
// acknowledgements, the order of chains, the fee pot, BMM blocks as the fallback, the block
// producer and its slashing protection. A chainstate of a regtest sidechain with three validators,
// and a record of the mainchain the test writes itself.

#include <addresstype.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <common/signmessage.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <interfaces/mining.h>
#include <kernel/chainparams.h>
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
#include <script/sign.h>
#include <streams.h>
#include <script/signingprovider.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

//! The time the tests start at (mock time): well after the regtest chain's first block.
constexpr int64_t START_TIME{1800000000};
constexpr CAmount TX_FEE{10000};

void Mock(int64_t time) { SetMockTime(std::chrono::seconds{time}); }

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

/** Installs the FastBlocks hooks before the chainstate is made, and removes them after. */
struct InstallFastBlocks {
    std::vector<CKey> keys{ValidatorKeys()};
    std::string validators;
    std::vector<std::string> args;
    std::unique_ptr<const CChainParams> params;
    explicit InstallFastBlocks(bool with_validators = true)
    {
        CChainParams::RegTestOptions options;
        options.sidechain_slot = 5;
        if (with_validators) {
            ECC_Context ecc;
            for (const CKey& key : keys) {
                validators += (validators.empty() ? "" : ",") + HexStr(XOnly(key));
                options.pos_validators.push_back({.pubkey = XOnly(key), .stake = 1});
            }
        }
        params = CChainParams::RegTest(options);
        pos::InstallHooks(*params);
    }
    ~InstallFastBlocks() { pos::UninstallHooks(); }
};

/** The verdict validation gave on the blocks it checked, by hash. */
struct BlockCatcher final : public CValidationInterface {
    std::map<uint256, BlockValidationState> states;
    void BlockChecked(const std::shared_ptr<const CBlock>& block, const BlockValidationState& state) override { states[block->GetHash()] = state; }
};

std::vector<const char*> SetupArgs(InstallFastBlocks& installed)
{
    installed.args = {"-sidechainslot=5"};
    if (!installed.validators.empty()) installed.args.push_back("-posvalidators=" + installed.validators);
    std::vector<const char*> args;
    for (const std::string& arg : installed.args) args.push_back(arg.c_str());
    return args;
}

struct FastBlocksSetup : public InstallFastBlocks, public TestingSetup {
    CKey key;
    CScript script;
    std::shared_ptr<BlockCatcher> catcher{std::make_shared<BlockCatcher>()};
    std::unique_ptr<pos::Producer> producer;
    int64_t base_slot{0};

    explicit FastBlocksSetup(bool with_validators = true)
        : InstallFastBlocks{with_validators}, TestingSetup{ChainType::REGTEST, TestOpts{.extra_args = SetupArgs(*this)}}
    {
        Hooks().Attach(m_node.chainman.get());
        Mock(START_TIME);
        base_slot = pos::SlotOf(START_TIME, Pos());
        key.MakeNewKey(/*fCompressed=*/true);
        script = GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey()});
        m_node.validation_signals->RegisterSharedValidationInterface(catcher);
        std::string error;
        auto protection{pos::SlashingProtection::Open(m_args.GetDataDirNet() / "validator_protection.dat", m_args.GetDataDirNet(), "keys.lock", error)};
        BOOST_REQUIRE_MESSAGE(protection, error);
        producer = std::make_unique<pos::Producer>(m_node, Hooks(), keys, script, std::move(protection));
        // The first block of the mainchain, which no sidechain acts on.
        Main();
    }
    ~FastBlocksSetup()
    {
        producer.reset();
        m_node.validation_signals->UnregisterSharedValidationInterface(catcher);
        SetMockTime(std::chrono::seconds{0});
    }

    pos::FastBlocksHooks& Hooks() { return *Assert(pos::InstalledHooks()); }
    const Consensus::PosParams& Pos() { return Assert(pos::InstalledHooks())->Params(); }
    ChainstateManager& Chainman() { return *Assert(m_node.chainman); }
    Chainstate& Chain() { return Chainman().ActiveChainstate(); }
    sidechain::Mainchain& Record() { return *Assert(Chainman().m_mainchain); }
    const CBlockIndex* Tip() { return WITH_LOCK(::cs_main, return Chain().m_chain.Tip()); }
    pos::FastState Fast() { return WITH_LOCK(::cs_main, return pos::GetFastState(Chain().SideState().View())); }
    pos::ChainKey Key() { return pos::ChainKey::FromWork(Tip()->nChainWork); }

    /** Append a block to the record of the mainchain, at the current time. */
    void Main(const std::function<void(sidechain::MainBlock&)>& fill = {})
    {
        sidechain::MainBlock block;
        const int height{Record().Height() + 1};
        block.hash = ArithToUint256(arith_uint256{static_cast<uint64_t>(0x20000 + height)} << 128);
        block.prev_hash = Record().TipHash();
        block.time = GetTime();
        if (fill) fill(block);
        BOOST_REQUIRE(Record().Append(block));
    }
    /** A mainchain block that commits to `hash`. */
    void Commit(const uint256& hash) { Main([&](sidechain::MainBlock& b) { b.bmm = hash; }); }

    /** Move the clock to slot base_slot + i, and return it. */
    int64_t Slot(int i)
    {
        const int64_t slot{base_slot + i};
        Mock(pos::SlotTime(slot, Pos()));
        return slot;
    }
    const CKey& Leader(int64_t slot)
    {
        const auto leader{Hooks().Validators().Leader(slot, Pos(), Chainman().GetConsensus().hashGenesisBlock)};
        BOOST_REQUIRE(leader);
        const uint256 pubkey{Hooks().Validators().Validators()[*leader].pubkey};
        for (const CKey& k : keys) {
            if (XOnly(k) == pubkey) return k;
        }
        BOOST_FAIL("no key leads the slot");
        return keys[0];
    }

    /** The PoS block of `slot` on the tip, signed by its leader (the clock is moved to the slot). */
    CBlock PosBlock(int i, const std::optional<pos::AnchorAck>& ack = std::nullopt)
    {
        const int64_t slot{Slot(i)};
        std::string error;
        auto block{producer->Build(slot, ack, error)};
        BOOST_REQUIRE_MESSAGE(block, error);
        BOOST_REQUIRE(pos::Producer::Sign(*block, Leader(slot)));
        return *block;
    }
    /** After a change: the sections hash, the merkle root, and the leader's signature again. */
    void Resign(CBlock& block, const CKey* signer = nullptr)
    {
        block.ext.hashSections = block.vSections.empty() ? uint256{} : pos::SectionsHash(block.vSections);
        node::RegenerateCommitments(block, Chainman());
        BOOST_REQUIRE(pos::Producer::Sign(block, signer ? *signer : Leader(pos::SlotOf(block.nTime, Pos()))));
    }
    /** A block with an acknowledgement put in by hand (as no producer would make it). */
    CBlock WithAck(CBlock block, const pos::AnchorAck& ack)
    {
        pos::PosSections sections;
        sections.ack = ack;
        block.vSections = sections.Encode();
        block.nNonce = static_cast<uint32_t>(ack.main_height);
        Resign(block);
        return block;
    }
    /** Replace the coinbase's outputs. */
    void SetCoinbase(CBlock& block, const std::function<void(CMutableTransaction&)>& change)
    {
        CMutableTransaction coinbase{*block.vtx[0]};
        change(coinbase);
        block.vtx[0] = MakeTransactionRef(std::move(coinbase));
    }

    /** What validation says of a block on the tip, without taking it. */
    std::string Check(const CBlock& block)
    {
        LOCK(::cs_main);
        const BlockValidationState state{TestBlockValidity(Chain(), block, /*check_pow=*/false, /*check_merkle_root=*/true)};
        return state.IsValid() ? std::string{} : state.GetRejectReason();
    }
    void Submit(const CBlock& block)
    {
        Chainman().ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/nullptr);
        m_node.validation_signals->SyncWithValidationInterfaceQueue();
    }
    /** Take a valid block: it becomes the tip. */
    void Take(const CBlock& block)
    {
        BOOST_REQUIRE_EQUAL(Check(block), "");
        Submit(block);
        BOOST_REQUIRE(Tip()->GetBlockHash() == block.GetHash());
    }
    std::string Reason(const CBlock& block)
    {
        const auto it{catcher->states.find(block.GetHash())};
        if (it == catcher->states.end() || it->second.IsValid()) return {};
        return it->second.GetRejectReason();
    }

    /** A BMM block on the tip, not committed yet. */
    CBlock BmmBlock(bool use_mempool = true, const CScript& coinbase_script = CScript() << OP_TRUE)
    {
        auto block_template{interfaces::MakeMining(m_node)->createNewBlock({.use_mempool = use_mempool, .coinbase_output_script = coinbase_script, .test_block_validity = false}, /*cooldown=*/false)};
        BOOST_REQUIRE(block_template);
        CBlock block{block_template->getBlock()};
        FinishBmm(block);
        return block;
    }
    void FinishBmm(CBlock& block)
    {
        block.hashMerkleRoot = BlockMerkleRoot(block);
        while (!CheckProofOfWork(block.GetHash(), block.nBits, Chainman().GetConsensus())) ++block.nNonce;
    }
    /** Have the mainchain commit to a BMM block and take it. */
    void MineBmm(const CBlock& block)
    {
        Commit(block.GetHash());
        Submit(block);
        BOOST_REQUIRE_MESSAGE(Tip()->GetBlockHash() == block.GetHash(), Reason(block));
    }

    /** A coin of `amount` for the key, from a deposit paid by a BMM block. */
    CTxOut Fund(CAmount amount, COutPoint& coin)
    {
        Main([&](sidechain::MainBlock& block) {
            sidechain::MainDeposit deposit;
            deposit.destination = EncodeDestination(WitnessV0KeyHash{key.GetPubKey()});
            deposit.amount = amount;
            deposit.txid = uint256{static_cast<uint8_t>(Record().Height() + 1)};
            block.deposits.push_back(deposit);
        });
        const CBlock block{BmmBlock(/*use_mempool=*/false)};
        MineBmm(block);
        const CTransaction& coinbase{*block.vtx[0]};
        BOOST_REQUIRE(coinbase.vout.size() >= 2);
        BOOST_REQUIRE(coinbase.vout[1].scriptPubKey == script);
        coin = COutPoint{coinbase.GetHash(), 1};
        return coinbase.vout[1];
    }
    CMutableTransaction Spend(const COutPoint& prevout, const CTxOut& spent, std::vector<CTxOut> outputs)
    {
        CMutableTransaction tx;
        tx.vin.emplace_back(prevout);
        tx.vout = std::move(outputs);
        FillableSigningProvider keystore;
        BOOST_REQUIRE(keystore.AddKey(key));
        std::map<COutPoint, Coin> coins{{prevout, Coin{spent, 1, /*fCoinBaseIn=*/false}}};
        std::map<int, bilingual_str> errors;
        BOOST_REQUIRE(SignTransaction(tx, &keystore, coins, SignOptions{.sighash_type = SIGHASH_ALL}, errors));
        return tx;
    }
    void Accept(const CMutableTransaction& tx)
    {
        const auto result{WITH_LOCK(::cs_main, return Chainman().ProcessTransaction(MakeTransactionRef(tx)))};
        BOOST_REQUIRE_MESSAGE(result.m_result_type == MempoolAcceptResult::ResultType::VALID, result.m_state.ToString());
    }

    /** A ticket for the block `anchored`, paying the pot to `payee`. */
    pos::AnchorTicket Ticket(const uint256& anchored, const CScript& payee)
    {
        pos::AnchorTicket ticket;
        ticket.anchored = anchored;
        ticket.payee = payee;
        return ticket;
    }
    /** Have the mainchain commit to a ticket; the acknowledgement of it. */
    pos::AnchorAck Anchor(const pos::AnchorTicket& ticket)
    {
        Commit(ticket.Hash());
        Hooks().AddTicket(ticket);
        return pos::AnchorAck{.ticket = ticket, .main_height = Record().Height()};
    }
    /** Start PoS: a first block of stake after the BMM blocks of the start. */
    void StartPos()
    {
        // A BMM block first, so that the chain has an anchor point.
        MineBmm(BmmBlock(/*use_mempool=*/false));
    }
};

struct NoValidatorsSetup : public FastBlocksSetup {
    NoValidatorsSetup() : FastBlocksSetup{/*with_validators=*/false} {}
};

} // namespace

BOOST_AUTO_TEST_SUITE(fastblocks_tests)

BOOST_AUTO_TEST_CASE(formats)
{
    // Sections: nothing is no bytes; an acknowledgement reads back; anything else does not read.
    pos::PosSections sections;
    BOOST_CHECK(sections.Encode().empty());
    pos::PosSections read;
    BOOST_CHECK(pos::PosSections::Decode({}, read) && !read.ack);
    sections.ack = pos::AnchorAck{.ticket = pos::AnchorTicket{.version = 1, .anchored = uint256{7}, .payee = CScript() << OP_TRUE}, .main_height = 42};
    const auto bytes{sections.Encode()};
    BOOST_CHECK(pos::PosSections::Decode(bytes, read));
    BOOST_CHECK(read == sections);
    auto longer{bytes};
    longer.push_back(0);
    BOOST_CHECK(!pos::PosSections::Decode(longer, read));
    auto evidence{bytes};
    evidence.back() = 1;
    BOOST_CHECK(!pos::PosSections::Decode(evidence, read));
    BOOST_CHECK(!pos::PosSections::Decode(std::vector<unsigned char>{0}, read));
    BOOST_CHECK(!pos::PosSections::Decode(std::vector<unsigned char>{1, 2}, read));

    // A ticket's hash is a tagged hash of the ticket, and changes with each part of it.
    const pos::AnchorTicket ticket{sections.ack->ticket};
    pos::AnchorTicket other{ticket};
    other.payee = CScript() << OP_2;
    BOOST_CHECK(ticket.Hash() != other.Hash());
    other = ticket;
    other.anchored = uint256{8};
    BOOST_CHECK(ticket.Hash() != other.Hash());

    // Payees: standard scripts of 80 bytes or less, no data.
    BOOST_CHECK(pos::IsValidPayee(GetScriptForDestination(WitnessV0KeyHash{uint160{}})));
    BOOST_CHECK(!pos::IsValidPayee(CScript() << OP_RETURN));
    BOOST_CHECK(!pos::IsValidPayee(CScript() << OP_TRUE << OP_DROP));
    BOOST_CHECK(!pos::IsValidPayee(CScript() << std::vector<unsigned char>(79, 1) << OP_CHECKSIG));

    // The producer's share, in integers, without overflow.
    BOOST_CHECK_EQUAL(pos::Producer::ProducerShare(10001, 5000), 5000);
    BOOST_CHECK_EQUAL(pos::Producer::ProducerShare(MAX_MONEY, 5000), MAX_MONEY / 2);
    BOOST_CHECK_EQUAL(pos::Producer::ProducerShare(MAX_MONEY, 10000), MAX_MONEY);
    BOOST_CHECK_EQUAL(pos::Producer::ProducerShare(3, 3333), 0);
}

BOOST_AUTO_TEST_CASE(chain_key)
{
    // The order of chains: anchors first, then the earlier last commitment, then the height.
    const pos::ChainKey key{.anchors = 5, .last_anchor_height = 1000, .height = 77};
    BOOST_CHECK(pos::ChainKey::FromWork(key.Work()) == key);
    const pos::ChainKey more_anchors{.anchors = 6, .last_anchor_height = 0xFFFFFFFF, .height = 1};
    const pos::ChainKey earlier{.anchors = 5, .last_anchor_height = 999, .height = 1};
    const pos::ChainKey longer{.anchors = 5, .last_anchor_height = 1000, .height = 78};
    BOOST_CHECK(more_anchors.Work() > key.Work());
    BOOST_CHECK(earlier.Work() > key.Work());
    BOOST_CHECK(longer.Work() > key.Work());
    BOOST_CHECK(more_anchors.Work() > earlier.Work());
    BOOST_CHECK(earlier.Work() > longer.Work());
}

BOOST_AUTO_TEST_CASE(leader_schedule)
{
    // Leaders are picked by stake, the same way by everyone; a larger stake leads more slots.
    Consensus::PosParams params;
    params.slots_per_epoch = 20;
    params.min_active_validators = 2;
    params.max_validators = 3;
    params.genesis_validators = {{.pubkey = uint256{3}, .stake = 1}, {.pubkey = uint256{1}, .stake = 8}, {.pubkey = uint256{2}, .stake = 1},
                                 {.pubkey = uint256{4}, .stake = 1}, {.pubkey = uint256{1}, .stake = 99}};
    const pos::ValidatorSet set{params};
    // A key given twice counts once (its first stake); the lowest stake, highest key, is left out.
    BOOST_REQUIRE_EQUAL(set.Validators().size(), 3U);
    BOOST_CHECK(set.Validators()[0].pubkey == uint256{1});
    BOOST_CHECK_EQUAL(set.Validators()[0].stake, 8);
    BOOST_CHECK(!set.Find(uint256{4}));
    BOOST_CHECK_EQUAL(set.TotalStake(), 10);
    BOOST_CHECK(set.Active());
    std::vector<int> led(3, 0);
    for (int64_t slot{0}; slot < 2000; ++slot) {
        const auto leader{set.Leader(slot, params, uint256{9})};
        BOOST_REQUIRE(leader);
        BOOST_CHECK(leader == set.Leader(slot, params, uint256{9}));
        ++led[*leader];
    }
    BOOST_CHECK_GT(led[0], 1400);
    BOOST_CHECK_GT(led[1], 100);
    BOOST_CHECK_GT(led[2], 100);
    // Too few validators: no leader.
    params.min_active_validators = 4;
    BOOST_CHECK(!pos::ValidatorSet{params}.Active());
    BOOST_CHECK(!pos::ValidatorSet{params}.Leader(1, params, uint256{9}));
}

BOOST_AUTO_TEST_CASE(slashing_protection)
{
    const fs::path dir{fs::temp_directory_path() / fs::PathFromString(strprintf("fastblocks_protection_%d", static_cast<uint64_t>(GetRandHash().GetUint64(0))))};
    fs::create_directories(dir);
    const fs::path file{dir / "protection.dat"};
    std::string error;
    {
        auto protection{pos::SlashingProtection::Open(file, dir, "keys.lock", error)};
        BOOST_REQUIRE_MESSAGE(protection, error);
        // A second one with the same keys is refused while the first is open.
        BOOST_CHECK(!pos::SlashingProtection::Open(file, dir, "keys.lock", error));
        BOOST_CHECK(error.find("another validator") != std::string::npos);
        BOOST_CHECK(!protection->LastSigned(uint256{1}));
        BOOST_CHECK(protection->MaySign(uint256{1}, 10, error));
        // Never at or below a slot signed.
        BOOST_CHECK(!protection->MaySign(uint256{1}, 10, error));
        BOOST_CHECK(!protection->MaySign(uint256{1}, 9, error));
        BOOST_CHECK(protection->MaySign(uint256{2}, 5, error));
        BOOST_CHECK(protection->MaySign(uint256{1}, 11, error));
    }
    {
        // What was signed is on disk: a restarted node remembers it.
        auto protection{pos::SlashingProtection::Open(file, dir, "keys.lock", error)};
        BOOST_REQUIRE_MESSAGE(protection, error);
        BOOST_CHECK(protection->LastSigned(uint256{1}) == std::optional<int64_t>{11});
        BOOST_CHECK(protection->LastSigned(uint256{2}) == std::optional<int64_t>{5});
        BOOST_CHECK(!protection->MaySign(uint256{1}, 11, error));
    }
    {
        // A file that does not read stops the producer rather than forgetting what was signed.
        std::ofstream{file.std_path()} << "something else\n";
        BOOST_CHECK(!pos::SlashingProtection::Open(file, dir, "keys.lock", error));
        std::ofstream{file.std_path()} << "fastblocks-slashing-protection 1\nnot a line\n";
        BOOST_CHECK(!pos::SlashingProtection::Open(file, dir, "keys.lock", error));
    }
    fs::remove_all(dir);
}

BOOST_FIXTURE_TEST_CASE(blocks_of_stake, FastBlocksSetup)
{
    StartPos();
    const int height{Tip()->nHeight};
    // The producer makes the block of each slot, signed by its leader; slots may be skipped.
    for (const int i : {1, 2, 4, 5}) {
        std::string error;
        const int64_t slot{Slot(i)};
        const auto made{producer->Produce(slot, error)};
        BOOST_REQUIRE_MESSAGE(made, error);
        BOOST_CHECK(Tip()->GetBlockHash() == *made);
        const CBlockHeader header{Tip()->GetBlockHeader()};
        BOOST_CHECK(pos::IsPosHeader(header));
        BOOST_CHECK_EQUAL(header.nTime, pos::SlotTime(slot, Pos()));
        BOOST_CHECK(pos::CheckBlockSig(header, XOnly(Leader(slot))));
        // A slot is never signed twice.
        BOOST_CHECK(!producer->Produce(slot, error));
    }
    BOOST_CHECK_EQUAL(Tip()->nHeight, height + 4);
    // No anchor yet: the height grows, the anchors do not.
    BOOST_CHECK_EQUAL(Key().anchors, 1U);
    BOOST_CHECK_EQUAL(Key().height, static_cast<uint64_t>(height + 4));
    BOOST_CHECK_EQUAL(Fast().last_pos_slot, base_slot + 5);
    BOOST_CHECK_EQUAL(Fast().first_unanchored_slot, base_slot + 1);
    BOOST_CHECK_EQUAL(producer->GetStatus().produced, 4U);
}

BOOST_FIXTURE_TEST_CASE(header_rules, FastBlocksSetup)
{
    StartPos();
    Take(PosBlock(1));
    // The proof-of-work limit as nBits.
    {
        CBlock block{PosBlock(2)};
        block.nBits = 0x1d00ffff;
        Resign(block);
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-bits");
    }
    // The time its slot starts, after the first block. (Every second starts a slot on regtest; and a time
    // before the first block's fails Bitcoin's rule first, so the hook is asked directly.)
    {
        CBlock block{PosBlock(2)};
        block.nTime = static_cast<uint32_t>(Pos().genesis_time);
        Resign(block);
        LOCK(::cs_main);
        BlockValidationState state;
        BOOST_CHECK(!Hooks().CheckHeader(block, *Chain().m_chain.Tip(), Chainman(), state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-pos-time");
    }
    // A later slot than the last block of stake.
    {
        CBlock block{PosBlock(2)};
        block.nTime = pos::SlotTime(base_slot + 1, Pos());
        Resign(block);
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-slot");
    }
    // A slot that has started (give or take max_future_seconds).
    {
        CBlock block{PosBlock(2)};
        Mock(block.nTime - Pos().max_future_seconds - 1);
        BOOST_CHECK_EQUAL(Check(block), "pos-time-future");
        Mock(block.nTime - Pos().max_future_seconds);
        BOOST_CHECK_EQUAL(Check(block), "");
    }
    // Signed by the leader of the slot, and no one else.
    {
        CBlock block{PosBlock(2)};
        const CKey& leader{Leader(pos::SlotOf(block.nTime, Pos()))};
        for (const CKey& other : keys) {
            if (XOnly(other) == XOnly(leader)) continue;
            Resign(block, &other);
            BOOST_CHECK_EQUAL(Check(block), "bad-pos-sig");
        }
        block.ext.sig[0] ^= 1;
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-sig");
    }
    // An acknowledgement of a mainchain height with no commitment on record: not yet.
    {
        CBlock block{PosBlock(2)};
        pos::PosSections sections;
        sections.ack = pos::AnchorAck{.ticket = Ticket(Tip()->GetBlockHash(), script), .main_height = Record().Height() + 5};
        block.vSections = sections.Encode();
        block.nNonce = static_cast<uint32_t>(sections.ack->main_height);
        Resign(block);
        BOOST_CHECK_EQUAL(Check(block), "pos-ack-unknown");
    }
}

BOOST_FIXTURE_TEST_CASE(no_validators, NoValidatorsSetup)
{
    // Without validators the chain is the template's: merged mined blocks only, which carry transactions.
    BOOST_CHECK(!Hooks().Validators().Active());
    COutPoint coin;
    const CTxOut spent{Fund(COIN, coin)};
    Accept(Spend(coin, spent, {CTxOut{COIN - TX_FEE, script}}));
    const CBlock block{BmmBlock()};
    BOOST_CHECK_EQUAL(block.vtx.size(), 2U);
    MineBmm(block);
    BOOST_CHECK_EQUAL(Fast().anchors, 2U);
    // A block of stake is not valid.
    CBlock pos_block{block};
    pos_block.hashPrevBlock = Tip()->GetBlockHash();
    pos_block.nVersion |= pos::POS_BLOCK_BIT;
    pos_block.nTime = pos::SlotTime(pos::SlotOf(GetTime(), Pos()) + 1, Pos());
    pos_block.nBits = pos::PosBits(Chainman().GetConsensus().powLimit);
    pos_block.nNonce = 0;
    pos_block.vtx.resize(1);
    pos_block.hashMerkleRoot = BlockMerkleRoot(pos_block);
    Mock(pos_block.nTime);
    BOOST_CHECK_EQUAL(Check(pos_block), "bad-pos-inactive");
}

BOOST_FIXTURE_TEST_CASE(block_rules, FastBlocksSetup)
{
    StartPos();
    // Only a block of stake puts coins in the pot.
    {
        CBlock block{BmmBlock(/*use_mempool=*/false)};
        SetCoinbase(block, [](CMutableTransaction& tx) { tx.vout.emplace_back(0, pos::PotScript()); });
        FinishBmm(block);
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-pot-not-pos");
    }
    // One pot output at most.
    {
        CBlock block{PosBlock(1)};
        SetCoinbase(block, [](CMutableTransaction& tx) {
            tx.vout.insert(tx.vout.begin() + 1, CTxOut{0, pos::PotScript()});
            tx.vout.insert(tx.vout.begin() + 1, CTxOut{0, pos::PotScript()});
        });
        Resign(block);
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-pot-multiple");
    }
    // Sections that read.
    {
        CBlock block{PosBlock(1)};
        block.vSections = {7, 7};
        Resign(block);
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-sections");
    }
    // nNonce is the height of the acknowledged commitment, 0 without one.
    {
        CBlock block{PosBlock(1)};
        block.nNonce = 1;
        Resign(block);
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-ack-nonce");
    }
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    {
        CBlock block{PosBlock(1, ack)};
        block.nNonce += 1;
        Resign(block);
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-ack-nonce");
    }
    // Tickets of version 1, with a payee.
    const auto with_ticket{[&](const std::function<void(pos::AnchorTicket&)>& change) {
        CBlock block{PosBlock(1, ack)};
        pos::PosSections sections;
        BOOST_REQUIRE(pos::PosSections::Decode(block.vSections, sections));
        change(sections.ack->ticket);
        block.vSections = sections.Encode();
        Resign(block);
        return block;
    }};
    BOOST_CHECK_EQUAL(Check(with_ticket([](pos::AnchorTicket& t) { t.version = 2; })), "bad-pos-ticket-version");
    BOOST_CHECK_EQUAL(Check(with_ticket([](pos::AnchorTicket& t) { t.payee = CScript() << OP_RETURN; })), "bad-pos-ticket-payee");
    // The block itself is valid.
    BOOST_CHECK_EQUAL(Check(PosBlock(1, ack)), "");
}

BOOST_FIXTURE_TEST_CASE(fee_split, FastBlocksSetup)
{
    COutPoint coin;
    const CTxOut spent{Fund(COIN, coin)};
    StartPos();
    const CMutableTransaction tx{Spend(coin, spent, {CTxOut{COIN - TX_FEE, script}})};
    Accept(tx);
    const CBlock block{PosBlock(1)};
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 2U);
    // Half to the producer, half to the pot.
    const CTransaction& coinbase{*block.vtx[0]};
    BOOST_CHECK_EQUAL(coinbase.vout[0].nValue, TX_FEE / 2);
    const auto pot{std::find_if(coinbase.vout.begin(), coinbase.vout.end(), [](const CTxOut& out) { return pos::IsPotScript(out.scriptPubKey); })};
    BOOST_REQUIRE(pot != coinbase.vout.end());
    BOOST_CHECK_EQUAL(pot->nValue, TX_FEE / 2);
    // The producer may not keep more, by output 0 or another output.
    {
        CBlock greedy{block};
        SetCoinbase(greedy, [](CMutableTransaction& cb) { cb.vout[0].nValue += 1; for (auto& out : cb.vout) if (pos::IsPotScript(out.scriptPubKey)) out.nValue -= 1; });
        Resign(greedy);
        BOOST_CHECK_EQUAL(Check(greedy), "bad-pos-producer-share");
        CBlock extra{block};
        SetCoinbase(extra, [&](CMutableTransaction& cb) {
            for (auto& out : cb.vout) if (pos::IsPotScript(out.scriptPubKey)) out.nValue -= 1;
            cb.vout.insert(cb.vout.begin() + 1, CTxOut{1, script});
        });
        Resign(extra);
        BOOST_CHECK_EQUAL(Check(extra), "bad-pos-producer-share");
    }
    // Less is fine: what is left unclaimed is lost.
    {
        CBlock modest{block};
        SetCoinbase(modest, [](CMutableTransaction& cb) { cb.vout[0].nValue -= 1; });
        Resign(modest);
        BOOST_CHECK_EQUAL(Check(modest), "");
    }
    Take(block);
    BOOST_CHECK_EQUAL(Fast().pot_accrued, TX_FEE / 2);
    BOOST_CHECK_EQUAL(Fast().pot_paid, 0);
    BOOST_CHECK_EQUAL(pos::PotAccruedAt(WITH_LOCK(::cs_main, return Chain().SideState().View()), Tip()->nHeight), TX_FEE / 2);
}

BOOST_FIXTURE_TEST_CASE(acknowledgements, FastBlocksSetup)
{
    COutPoint coin;
    const CTxOut spent{Fund(COIN, coin)};
    StartPos();
    const CMutableTransaction tx{Spend(coin, spent, {CTxOut{COIN - TX_FEE, script}})};
    Accept(tx);
    Take(PosBlock(1));
    Take(PosBlock(2));
    const pos::ChainKey before{Key()};
    const CBlockIndex* anchored{Tip()->pprev};
    const CScript payee{GetScriptForDestination(WitnessV0KeyHash{*uint160::FromHex("0909090909090909090909090909090909090909")})};
    const CScript other_payee{GetScriptForDestination(WitnessV0KeyHash{*uint160::FromHex("0808080808080808080808080808080808080808")})};
    const pos::AnchorAck ack{Anchor(Ticket(anchored->GetBlockHash(), payee))};

    // The mainchain committed to the ticket, at the height said.
    {
        // (The mainchain block before committed to a BMM block.)
        pos::AnchorAck wrong{ack};
        wrong.main_height = ack.main_height - 1;
        BOOST_CHECK_EQUAL(Check(WithAck(PosBlock(3), wrong)), "bad-pos-ack-height");
    }
    {
        const pos::AnchorAck unknown{.ticket = Ticket(anchored->GetBlockHash(), script), .main_height = Record().Height()};
        BOOST_CHECK_EQUAL(Check(WithAck(PosBlock(3), unknown)), "bmm-unknown");
    }
    // The block anchored is on the chain, at or below the parent.
    {
        const pos::AnchorAck nowhere{Anchor(Ticket(uint256{0x55}, payee))};
        BOOST_CHECK_EQUAL(Check(WithAck(PosBlock(3), nowhere)), "bad-pos-ack-anchored");
    }

    // The acknowledgement: the pot accrued up to the block anchored goes to the payee, the anchors
    // count one more, and the bundle and deposits are as of the commitment.
    // (A block of the same slot, not acknowledging, which stays on a branch of its own.)
    const CBlock sibling{PosBlock(3)};
    Take(sibling);
    CBlockIndex* sibling_index{WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(sibling.GetHash()))};
    {
        BlockValidationState state;
        Chain().InvalidateBlock(state, sibling_index);
    }
    const CBlock acked{PosBlock(3, ack)};
    WITH_LOCK(::cs_main, Chain().ResetBlockFailureFlags(sibling_index));
    BOOST_CHECK_EQUAL(acked.nNonce, static_cast<uint32_t>(ack.main_height));
    const CTransaction& coinbase{*acked.vtx[0]};
    BOOST_REQUIRE_GE(coinbase.vout.size(), 2U);
    BOOST_CHECK(coinbase.vout[1] == CTxOut(TX_FEE / 2, payee));
    Take(acked);
    BOOST_CHECK_EQUAL(Key().anchors, before.anchors + 1);
    BOOST_CHECK_EQUAL(Key().last_anchor_height, static_cast<uint32_t>(ack.main_height));
    const pos::FastState fast{Fast()};
    BOOST_CHECK_EQUAL(fast.pot_paid, TX_FEE / 2);
    BOOST_CHECK_EQUAL(fast.last_main_height, ack.main_height);
    BOOST_CHECK(fast.last_anchored == anchored->GetBlockHash());
    BOOST_CHECK(fast.anchor_point == acked.GetHash());
    // The blocks after the one anchored are not covered yet: the first of them starts the count.
    BOOST_CHECK_EQUAL(fast.first_unanchored_slot, base_slot + 2);
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return Chain().SideState().MainHeight()), ack.main_height - 1);
    // The block rests on the commitment to the ticket.
    BOOST_CHECK(Hooks().CommitmentOf(*Tip()) == ack.ticket.Hash());
    const auto resting{Hooks().BlocksCommittedBy(ack.ticket.Hash())};
    BOOST_CHECK(std::find(resting.begin(), resting.end(), acked.GetHash()) != resting.end());

    // In the order of the mainchain: not the same commitment again, nor an older one.
    BOOST_CHECK_EQUAL(Check(WithAck(PosBlock(4), ack)), "bad-pos-ack-order");
    // The block anchored is an ancestor: not a block of another branch.
    {
        const pos::AnchorAck elsewhere{Anchor(Ticket(sibling.GetHash(), payee))};
        BOOST_CHECK_EQUAL(Check(WithAck(PosBlock(4), elsewhere)), "bad-pos-ack-anchored");
    }
    // Chained: the next ticket anchors the acknowledgement, or a block after it.
    {
        const pos::AnchorAck below{Anchor(Ticket(anchored->GetBlockHash(), other_payee))};
        BOOST_CHECK_EQUAL(Check(WithAck(PosBlock(4), below)), "bad-pos-ack-chain");
        const pos::AnchorAck chained{Anchor(Ticket(acked.GetHash(), payee))};
        Take(PosBlock(4, chained));
        BOOST_CHECK_EQUAL(Key().anchors, before.anchors + 2);
        // Nothing accrued since: nothing paid.
        BOOST_CHECK_EQUAL(Tip()->nTx, 1U);
        BOOST_CHECK_EQUAL(Fast().pot_paid, TX_FEE / 2);
    }
}

BOOST_FIXTURE_TEST_CASE(refunds_wait_for_acknowledgements, FastBlocksSetup)
{
    COutPoint coin;
    const CTxOut spent{Fund(COIN, coin)};
    StartPos();
    // A withdrawal in a block of stake.
    const CMutableTransaction withdrawal{Spend(coin, spent, {CTxOut{COIN / 2, sidechain::WithdrawalScript(1000, key.GetPubKey().GetID(), script)},
                                                             CTxOut{COIN / 2 - TX_FEE, script}})};
    Accept(withdrawal);
    Take(PosBlock(1));
    // A refund request for it waits for a block that takes the peg actions.
    sidechain::RefundRequest request;
    request.withdrawal = COutPoint{withdrawal.GetHash(), 0};
    BOOST_REQUIRE(key.SignCompact(MessageHash(sidechain::RefundMessage(request.withdrawal)), request.signature));
    const CTxOut& change{withdrawal.vout[1]};
    const CMutableTransaction refund{Spend(COutPoint{withdrawal.GetHash(), 1}, change, {CTxOut{0, sidechain::RefundScript(request)}, CTxOut{change.nValue - TX_FEE, script}})};
    Accept(refund);
    CBlock plain{PosBlock(2)};
    BOOST_CHECK_EQUAL(plain.vtx.size(), 1U);
    plain.vtx.push_back(MakeTransactionRef(refund));
    Resign(plain);
    BOOST_CHECK_EQUAL(Check(plain), "bad-sc-refund-not-peg");
    // An acknowledgement takes it.
    Take(PosBlock(2));
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    const CBlock acked{PosBlock(3, ack)};
    BOOST_CHECK_EQUAL(acked.vtx.size(), 2U);
    Take(acked);
}

BOOST_FIXTURE_TEST_CASE(unanchored_blocks_stop, FastBlocksSetup)
{
    StartPos();
    Take(PosBlock(1));
    // max_unanchored_slots after the first block no anchor covers, blocks of stake stop...
    const int last{static_cast<int>(Pos().max_unanchored_slots)};
    {
        CBlock block{PosBlock(last + 1)};
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-unanchored");
    }
    Take(PosBlock(last));
    // ... until an anchor covers them.
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    Take(PosBlock(last + 1, ack));
    BOOST_CHECK_EQUAL(Fast().first_unanchored_slot, base_slot + last + 1);
    Take(PosBlock(last + 2));
}

BOOST_FIXTURE_TEST_CASE(bmm_fallback, FastBlocksSetup)
{
    COutPoint coin;
    const CTxOut spent{Fund(COIN, coin)};
    StartPos();
    Accept(Spend(coin, spent, {CTxOut{COIN - TX_FEE, script}}));
    // A block of stake puts half its fees in the pot.
    Take(PosBlock(1));
    BOOST_CHECK_EQUAL(Fast().pot_accrued, TX_FEE / 2);

    // A BMM block without transactions is valid while blocks of stake come.
    CBlock empty{BmmBlock(/*use_mempool=*/false, script)};
    BOOST_CHECK_EQUAL(empty.vtx.size(), 1U);
    // It anchors its parent, and its first output takes the pot.
    BOOST_REQUIRE_GE(empty.vtx[0]->vout.size(), 2U);
    BOOST_CHECK(empty.vtx[0]->vout[1] == CTxOut(TX_FEE / 2, script));
    const pos::ChainKey before{Key()};
    MineBmm(empty);
    BOOST_CHECK_EQUAL(Key().anchors, before.anchors + 1);
    BOOST_CHECK_EQUAL(Fast().pot_paid, TX_FEE / 2);
    BOOST_CHECK_EQUAL(Fast().first_unanchored_slot, -1);
}

BOOST_FIXTURE_TEST_CASE(bmm_blocks_carry_transactions_when_stalled, FastBlocksSetup)
{
    COutPoint coin;
    const CTxOut spent{Fund(COIN, coin)};
    StartPos();
    Take(PosBlock(1));
    Accept(Spend(coin, spent, {CTxOut{COIN - TX_FEE, script}}));
    // Blocks of stake came recently: no transactions in a BMM block.
    {
        const CBlock block{BmmBlock()};
        BOOST_REQUIRE_EQUAL(block.vtx.size(), 2U);
        Commit(block.GetHash());
        Submit(block);
        BOOST_CHECK_EQUAL(Reason(block), "bad-pos-bmm-not-stalled");
    }
    // stall_seconds after the last of them, as the mainchain block before the commitment says, it may.
    Mock(pos::SlotTime(base_slot + 1, Pos()) + Pos().stall_seconds);
    Main();
    const CBlock block{BmmBlock()};
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 2U);
    MineBmm(block);
}

BOOST_FIXTURE_TEST_CASE(fork_choice, FastBlocksSetup)
{
    StartPos();
    Take(PosBlock(1));
    const CBlockIndex* fork{Tip()};
    // Two tickets for the block at the fork, committed one after the other.
    const pos::AnchorAck first{Anchor(Ticket(fork->GetBlockHash(), script))};
    const pos::AnchorAck second{Anchor(Ticket(fork->GetBlockHash(), GetScriptForDestination(WitnessV0KeyHash{*uint160::FromHex("0707070707070707070707070707070707070707")})))};
    // Branches B and C on the fork, each acknowledging one of them (C skips the first: allowed).
    const CBlock b{PosBlock(4, first)};
    const CBlock c{PosBlock(5, second)};
    // Branch A: two blocks of stake and no anchor.
    Take(PosBlock(2));
    const CBlock a2{PosBlock(3)};
    Take(a2);
    Mock(pos::SlotTime(base_slot + 5, Pos()));
    // More anchors beat more blocks.
    Submit(c);
    BOOST_CHECK(Tip()->GetBlockHash() == c.GetHash());
    BOOST_CHECK_GT(Key().anchors, pos::ChainKey::FromWork(WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(a2.GetHash())->nChainWork)).anchors);
    // As many anchors: the one the mainchain committed to first wins, whatever came first.
    Submit(b);
    BOOST_CHECK(Tip()->GetBlockHash() == b.GetHash());
    BOOST_CHECK_EQUAL(Key().last_anchor_height, static_cast<uint32_t>(first.main_height));
    // And B grows by blocks of stake.
    Take(PosBlock(6));
    BOOST_CHECK(Tip()->pprev->GetBlockHash() == b.GetHash());
}

BOOST_FIXTURE_TEST_CASE(ticket_pool, FastBlocksSetup)
{
    // A peer that sends tickets by the thousand pushes out the oldest, but not this node's own nor
    // those the mainchain committed to.
    const pos::AnchorTicket own{Ticket(Tip()->GetBlockHash(), script)};
    BOOST_CHECK(Hooks().AddTicket(own, /*own=*/true));
    BOOST_CHECK(!Hooks().AddTicket(own));
    const pos::AnchorTicket committed{Ticket(Tip()->GetBlockHash(), GetScriptForDestination(WitnessV0KeyHash{*uint160::FromHex("0606060606060606060606060606060606060606")}))};
    Commit(committed.Hash());
    BOOST_CHECK(Hooks().AddTicket(committed));
    const pos::AnchorTicket first_junk{Ticket(uint256{1}, script)};
    for (uint32_t i{1}; i <= pos::MAX_TICKETS + 100; ++i) {
        pos::AnchorTicket junk{Ticket(ArithToUint256(arith_uint256{i}), script)};
        BOOST_CHECK(Hooks().AddTicket(junk));
    }
    BOOST_CHECK(Hooks().GetTicket(own.Hash()));
    BOOST_CHECK(Hooks().GetTicket(committed.Hash()));
    BOOST_CHECK(!Hooks().GetTicket(first_junk.Hash()));
    BOOST_CHECK_EQUAL(Hooks().Tickets().size(), pos::MAX_TICKETS);
}

BOOST_AUTO_TEST_CASE(hooks_without_context)
{
    // Hooks not attached to a chainstate refuse what they cannot check.
    CChainParams::RegTestOptions options;
    options.sidechain_slot = 5;
    const auto params{CChainParams::RegTest(options)};
    pos::FastBlocksHooks hooks{*params};
    CBlock block;
    block.nVersion = pos::POS_BLOCK_BIT;
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
    block.vtx.push_back(MakeTransactionRef(coinbase));
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty};
    sidechain::State state{store};
    std::vector<CTxOut> owed;
    std::string reason;
    BOOST_CHECK(!hooks.BeforePegStep(state, block, 1, sidechain::PegStep{}, params->GetConsensus().sidechain, owed, reason));
    BOOST_CHECK_EQUAL(reason, "bad-pos-context");
    BOOST_CHECK(!hooks.AfterBlock(state, block, 1, std::nullopt, 0, params->GetConsensus().sidechain, reason));
    BOOST_CHECK_EQUAL(reason, "bad-pos-context");
    // Without validators, no block of stake.
    BOOST_CHECK(!hooks.Validators().Active());
}

BOOST_FIXTURE_TEST_CASE(finality, FastBlocksSetup)
{
    // From the heights the newest anchors anchor: soft, anchored (k), final.
    BOOST_CHECK(pos::FinalityAt(10, 1, {}, 3).status == pos::FinalityStatus::SOFT);
    BOOST_CHECK(pos::FinalityAt(10, 1, {9, 5}, 3).status == pos::FinalityStatus::SOFT);
    const pos::Finality anchored{pos::FinalityAt(10, 4, {12, 10, 5}, 3)};
    BOOST_CHECK(anchored.status == pos::FinalityStatus::ANCHORED);
    BOOST_CHECK_EQUAL(anchored.anchors, 2);
    BOOST_CHECK_EQUAL(anchored.depth, 4);
    BOOST_CHECK(pos::FinalityAt(10, 9, {14, 12, 10}, 3).status == pos::FinalityStatus::FINAL);
    BOOST_CHECK(pos::FinalityAt(10, 9, {14}, 1).status == pos::FinalityStatus::FINAL);
    BOOST_CHECK_EQUAL(pos::FinalityName(pos::FinalityStatus::ANCHORED), "anchored");

    // On a chain: a BMM block anchors its parent; an acknowledgement the block its ticket names.
    StartPos();
    const CBlockIndex* bmm{Tip()};
    Take(PosBlock(1));
    const CBlockIndex* p1{Tip()};
    Take(PosBlock(2));
    const CBlockIndex* p2{Tip()};
    Take(PosBlock(3));
    const CBlockIndex* p3{Tip()};
    const auto finality{[&](const CBlockIndex* block, int final_anchors) { return WITH_LOCK(::cs_main, return pos::FinalityOf(Hooks(), *block, *Tip(), final_anchors)); }};
    BOOST_CHECK(finality(p1, 3).status == pos::FinalityStatus::SOFT);
    BOOST_CHECK_EQUAL(finality(p1, 3).depth, 3);
    BOOST_CHECK(finality(bmm->pprev, 3).status == pos::FinalityStatus::ANCHORED);

    const pos::AnchorAck first{Anchor(Ticket(p2->GetBlockHash(), script))};
    Take(PosBlock(4, first));
    const CBlockIndex* q1{Tip()};
    BOOST_CHECK(finality(p1, 3).status == pos::FinalityStatus::ANCHORED);
    BOOST_CHECK_EQUAL(finality(p2, 3).anchors, 1);
    BOOST_CHECK(finality(p3, 3).status == pos::FinalityStatus::SOFT);
    // With one anchor to be final (regtest's final_anchors), anchored is final.
    BOOST_CHECK(finality(p2, 1).status == pos::FinalityStatus::FINAL);
    BOOST_CHECK_EQUAL(Pos().final_anchors, 1);

    const pos::AnchorAck second{Anchor(Ticket(q1->GetBlockHash(), script))};
    Take(PosBlock(5, second));
    BOOST_CHECK_EQUAL(finality(p1, 3).anchors, 2);
    BOOST_CHECK(finality(p3, 3).status == pos::FinalityStatus::ANCHORED);
    MineBmm(BmmBlock(/*use_mempool=*/false));
    BOOST_CHECK(finality(p1, 3).status == pos::FinalityStatus::FINAL);
    BOOST_CHECK(finality(Tip(), 3).status == pos::FinalityStatus::SOFT);

    // The newest anchors, newest first, with what they anchor and the height of their commitment.
    const auto anchors{WITH_LOCK(::cs_main, return pos::RecentAnchors(Hooks(), *Tip(), 10))};
    BOOST_REQUIRE_EQUAL(anchors.size(), 4U);
    BOOST_CHECK(anchors[0].block == Tip() && anchors[0].anchored == Tip()->pprev);
    BOOST_CHECK_EQUAL(anchors[0].main_height, static_cast<uint32_t>(Record().Height()));
    BOOST_CHECK(anchors[1].anchored == q1);
    BOOST_CHECK_EQUAL(anchors[1].main_height, static_cast<uint32_t>(second.main_height));
    BOOST_CHECK(anchors[2].block == q1 && anchors[2].anchored == p2);
    BOOST_CHECK(anchors[3].block == bmm);
    // At most as many as asked for; none at or below the floor.
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return pos::RecentAnchors(Hooks(), *Tip(), 2)).size(), 2U);
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return pos::RecentAnchors(Hooks(), *Tip(), 10, q1->nHeight)).size(), 2U);
    // A block that is no anchor anchors nothing; the acknowledgement is read from disk once forgotten.
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Hooks().AnchoredBy(*p3)));
    BOOST_CHECK(WITH_LOCK(::cs_main, return Hooks().AnchoredBy(*q1)) == p2);
}

BOOST_FIXTURE_TEST_CASE(unanchored_headers, FastBlocksSetup)
{
    StartPos();
    const CBlockIndex* start{Tip()};
    Take(PosBlock(1));
    const int last{static_cast<int>(Pos().max_unanchored_slots)};
    const auto header_reason{[&](const CBlock& block) {
        LOCK(::cs_main);
        BlockValidationState state;
        const CBlockIndex* prev{Chainman().m_blockman.LookupBlockIndex(block.hashPrevBlock)};
        if (Hooks().CheckHeader(block, *prev, Chainman(), state)) return std::string{};
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_INVALID_HEADER);
        return state.GetRejectReason();
    }};
    // Without an acknowledgement, the header alone tells: max_unanchored_slots after the first block of
    // stake after the last BMM block, no more.
    BOOST_CHECK_EQUAL(header_reason(PosBlock(last + 1)), "bad-pos-unanchored");
    BOOST_CHECK_EQUAL(header_reason(PosBlock(last)), "");
    // An acknowledgement's header passes: what it covers is in its block. One that covers too little
    // (the block of stake after the BMM block stays uncovered) fails in the state.
    const CBlock too_little{PosBlock(last + 1, Anchor(Ticket(start->GetBlockHash(), script)))};
    BOOST_CHECK_EQUAL(header_reason(too_little), "");
    BOOST_CHECK_EQUAL(Check(too_little), "bad-pos-unanchored");
    const CBlock covering{PosBlock(last + 1, Anchor(Ticket(Tip()->GetBlockHash(), script)))};
    Take(covering);
    // After an acknowledgement, the count starts at most at it.
    BOOST_CHECK_EQUAL(header_reason(PosBlock(last + 1 + last)), "bad-pos-unanchored");
    BOOST_CHECK_EQUAL(header_reason(PosBlock(last + last)), "");
}

BOOST_FIXTURE_TEST_CASE(evidence, FastBlocksSetup)
{
    StartPos();
    const CBlock first{PosBlock(1)};
    CBlock second{first};
    SetCoinbase(second, [](CMutableTransaction& tx) { tx.vout.emplace_back(0, CScript() << OP_RETURN); });
    Resign(second);
    BOOST_REQUIRE(first.GetHash() != second.GetHash());
    const uint256 genesis{Chainman().GetConsensus().hashGenesisBlock};
    const int64_t slot{pos::SlotOf(first.nTime, Pos())};
    std::string reason;

    // Two headers of one slot, both signed by its leader: the offence.
    const pos::DoubleProposal proof{.a = first, .b = second};
    const auto offence{pos::CheckEvidence(proof, Hooks().Validators(), Pos(), genesis, reason)};
    BOOST_REQUIRE_MESSAGE(offence, reason);
    BOOST_CHECK(offence->pubkey == XOnly(Leader(slot)));
    BOOST_CHECK_EQUAL(offence->slot, slot);
    // Headers of blocks of stake.
    BOOST_CHECK(!pos::CheckEvidence({.a = first, .b = BmmBlock(/*use_mempool=*/false)}, Hooks().Validators(), Pos(), genesis, reason));
    BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-kind");
    // Of one slot, which starts at their time.
    {
        CBlock later{PosBlock(2)};
        BOOST_CHECK(!pos::CheckEvidence({.a = first, .b = later}, Hooks().Validators(), Pos(), genesis, reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-slot");
        CBlockHeader a{first}, b{second};
        a.nTime = b.nTime = static_cast<uint32_t>(Pos().genesis_time);
        BOOST_CHECK(!pos::CheckEvidence({.a = a, .b = b}, Hooks().Validators(), Pos(), genesis, reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-slot");
    }
    // Two headers, not one twice.
    BOOST_CHECK(!pos::CheckEvidence({.a = first, .b = first}, Hooks().Validators(), Pos(), genesis, reason));
    BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-same");
    // Both signed by the leader of the slot.
    for (const CKey& other : keys) {
        if (XOnly(other) == XOnly(Leader(slot))) continue;
        CBlock forged{second};
        Resign(forged, &other);
        BOOST_CHECK(!pos::CheckEvidence({.a = first, .b = forged}, Hooks().Validators(), Pos(), genesis, reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-sig");
    }
    // Without validators, no leader: no offence.
    {
        Consensus::PosParams none{Pos()};
        none.genesis_validators.clear();
        BOOST_CHECK(!pos::CheckEvidence(proof, pos::ValidatorSet{none}, none, genesis, reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-sig");
    }

    // One proof per offence, whichever header comes first; it reads back as written.
    pos::DoubleProposal swapped{.a = second, .b = first};
    BOOST_CHECK(Hooks().AddEvidence(swapped, reason));
    BOOST_CHECK(Hooks().AddEvidence(proof, reason));
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 1U);
    DataStream stream{};
    stream << *Hooks().Evidence().Get(*offence);
    pos::DoubleProposal read;
    stream >> read;
    BOOST_CHECK(read.a.GetHash() == std::min(first.GetHash(), second.GetHash()));
    BOOST_CHECK(read.b.ext.sig == (read.b.GetHash() == first.GetHash() ? first.ext.sig : second.ext.sig));
    // Relayed when added (none to relay later); not if it is no proof.
    BOOST_CHECK(Hooks().Evidence().TakeUnrelayed().empty());
    BOOST_CHECK(!Hooks().AddEvidence({.a = first, .b = first}, reason));

    // A node finds proofs itself, in the headers it is given: two of slot 2 on the first.
    Take(first);
    const CBlock child{PosBlock(2)};
    CBlock rival{child};
    SetCoinbase(rival, [](CMutableTransaction& tx) { tx.vout.emplace_back(0, CScript() << OP_RETURN); });
    Resign(rival);
    {
        BlockValidationState state;
        const std::vector<CBlockHeader> headers{child, rival};
        BOOST_CHECK(Chainman().ProcessNewBlockHeaders(headers, /*min_pow_checked=*/true, state));
    }
    const pos::Offence found{.pubkey = XOnly(Leader(slot + 1)), .slot = slot + 1};
    BOOST_CHECK(Hooks().Evidence().Get(found));
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 2U);
    // ... and relays them (in the follower's round).
    const auto unrelayed{Hooks().Evidence().TakeUnrelayed()};
    BOOST_REQUIRE_EQUAL(unrelayed.size(), 1U);
    BOOST_CHECK(std::min(child.GetHash(), rival.GetHash()) == unrelayed[0].a.GetHash());

    // Bounded: the proofs of the oldest slots go first, and so do the headers noted.
    for (int64_t i{0}; i < static_cast<int64_t>(pos::MAX_EVIDENCE) + 5; ++i) Hooks().Evidence().Add({.pubkey = uint256{2}, .slot = slot + 1000 + i}, proof);
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), pos::MAX_EVIDENCE);
    BOOST_CHECK(!Hooks().Evidence().Get(*offence));
    BOOST_CHECK(!Hooks().Evidence().Get(found));
    BOOST_CHECK(!Hooks().Evidence().Get({.pubkey = uint256{2}, .slot = slot + 1000 + 4}));
    BOOST_CHECK(Hooks().Evidence().Get({.pubkey = uint256{2}, .slot = slot + 1000 + 5}));
    pos::EvidencePool pool;
    CBlockHeader header{first};
    for (int64_t i{0}; i <= static_cast<int64_t>(pos::MAX_SIGNED_HEADERS); ++i) BOOST_CHECK(!pool.NoteSigned(header, i));
    BOOST_CHECK(!pool.NoteSigned(second, 0));
    BOOST_CHECK(pool.NoteSigned(second, static_cast<int64_t>(pos::MAX_SIGNED_HEADERS)));
    BOOST_CHECK(!pool.NoteSigned(header, 5));
}

BOOST_FIXTURE_TEST_CASE(waiting_headers, FastBlocksSetup)
{
    // Headers that wait: for a commitment, for their slot, for an acknowledged commitment.
    BOOST_CHECK(Hooks().IsWaitReason("bmm-unknown"));
    BOOST_CHECK(Hooks().IsWaitReason("pos-time-future"));
    BOOST_CHECK(Hooks().IsWaitReason("pos-ack-unknown"));
    BOOST_CHECK(!Hooks().IsWaitReason("bad-pos-sig"));
    StartPos();
    const int height{Tip()->nHeight};
    const auto ready{[&](const CBlockHeader& header) { return WITH_LOCK(::cs_main, return Hooks().WaitingReady(header, Record())); }};
    const auto wait{[&](const CBlockHeader& header) { WITH_LOCK(::cs_main, Chainman().AddBmmWaiting(header, /*peer=*/-1)); }};

    // A BMM block: once committed to, ordered by its height (as blocks of stake are).
    const CBlock bmm{BmmBlock(/*use_mempool=*/false)};
    BOOST_CHECK(!ready(bmm));

    // A block of stake that acknowledges a commitment not on record yet, and one on top of it.
    const pos::AnchorTicket ticket{Ticket(Tip()->GetBlockHash(), script)};
    const CBlock acking{WithAck(PosBlock(1), pos::AnchorAck{.ticket = ticket, .main_height = Record().Height() + 1})};
    BOOST_CHECK_EQUAL(Check(acking), "pos-ack-unknown");
    CBlock child{PosBlock(2)};
    child.hashPrevBlock = acking.GetHash();
    Resign(child);
    BOOST_CHECK(!ready(acking));
    // (Validation keeps the header that waits; the one on it, the peer that sends both does.)
    wait(acking);
    wait(child);
    BOOST_CHECK(!ready(child)); // its parent waits
    Commit(ticket.Hash());
    BOOST_CHECK(ready(acking) == std::optional<int64_t>{height + 1});
    BOOST_CHECK(ready(child) == std::optional<int64_t>{height + 2});
    Commit(bmm.GetHash());
    BOOST_CHECK(ready(bmm) == std::optional<int64_t>{height + 1});

    // A block of stake whose slot has not started: not before it does.
    const CBlock future{PosBlock(5)};
    Mock(future.nTime - Pos().max_future_seconds - 1);
    BOOST_CHECK(!ready(future));
    Mock(future.nTime);
    BOOST_CHECK(ready(future) == std::optional<int64_t>{height + 1});

    // Taken in, in that order, by the follower's round: the chain moves to the acknowledgement.
    {
        BlockValidationState state;
        const std::vector<CBlockHeader> headers{acking, child};
        BOOST_CHECK(Chainman().ProcessNewBlockHeaders(headers, /*min_pow_checked=*/true, state));
    }
    BOOST_CHECK(WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(child.GetHash())) != nullptr);
}

BOOST_FIXTURE_TEST_CASE(ticket_requests, FastBlocksSetup)
{
    StartPos();
    Take(PosBlock(1));
    // A commitment on record of a ticket this node does not have: asked for, once per mainchain block.
    const pos::AnchorTicket ticket{Ticket(Tip()->GetBlockHash(), script)};
    Commit(ticket.Hash());
    BOOST_CHECK(Hooks().AskMissingTickets() == std::vector<uint256>{ticket.Hash()});
    BOOST_CHECK(Hooks().AskMissingTickets().empty());
    // Known now: not asked for again. (Nor are commitments to blocks.)
    BOOST_CHECK(Hooks().AddTicket(ticket));
    Main();
    BOOST_CHECK(Hooks().AskMissingTickets().empty());

    // From a peer: a ticket of a block this node does not know only if the mainchain committed to it.
    const auto message{[&](const std::string& type, const auto& payload) {
        DataStream stream{};
        stream << payload;
        std::string misbehaving;
        BOOST_CHECK(Hooks().ProcessMessage(/*peer=*/7, type, stream, misbehaving));
        return misbehaving;
    }};
    const pos::AnchorTicket unknown_block{Ticket(uint256{0x77}, script)};
    BOOST_CHECK_EQUAL(message(pos::MSG_TICKET, unknown_block), "");
    BOOST_CHECK(!Hooks().GetTicket(unknown_block.Hash()));
    const pos::AnchorTicket committed{Ticket(uint256{0x78}, script)};
    Commit(committed.Hash());
    BOOST_CHECK_EQUAL(message(pos::MSG_TICKET, committed), "");
    BOOST_CHECK(Hooks().GetTicket(committed.Hash()));
    // Requests: a bounded list.
    BOOST_CHECK_EQUAL(message(pos::MSG_GETTICKETS, std::vector<uint256>(pos::MAX_TICKETS_ASKED, uint256{1})), "");
    BOOST_CHECK_EQUAL(message(pos::MSG_GETTICKETS, std::vector<uint256>(pos::MAX_TICKETS_ASKED + 1, uint256{1})), "too many anchor tickets asked for");
    BOOST_CHECK_EQUAL(message(pos::MSG_GETTICKETS, std::vector<unsigned char>{5}), "malformed request for anchor tickets");
    BOOST_CHECK_EQUAL(message(pos::MSG_EVIDENCE, std::vector<unsigned char>{1, 2}), "malformed evidence");
    // Other messages are not FastBlocks'.
    DataStream empty{};
    std::string misbehaving;
    BOOST_CHECK(!Hooks().ProcessMessage(7, "other", empty, misbehaving));
}

BOOST_FIXTURE_TEST_CASE(staking_switch, FastBlocksSetup)
{
    StartPos();
    BOOST_CHECK(producer->Enabled());
    BOOST_CHECK_EQUAL(producer->LastSignedSlot(), -1);
    producer->SetEnabled(false);
    BOOST_CHECK(!producer->Enabled());
    // (The switch stops the producer's thread; a block asked for directly is still made.)
    std::string error;
    const int64_t slot{Slot(1)};
    BOOST_CHECK(producer->Produce(slot, error));
    BOOST_CHECK_EQUAL(producer->LastSignedSlot(), slot);
    producer->SetEnabled(true);
    BOOST_CHECK(producer->Enabled());
}

BOOST_AUTO_TEST_SUITE_END()
