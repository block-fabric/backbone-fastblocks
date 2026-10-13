// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// FastBlocks: blocks of stake signed by the leader of their slot, anchors and their
// acknowledgements, the order of chains, the fee pot, BMM blocks as the fallback, the block
// producer and its slashing protection. A chainstate of a regtest sidechain with three validators,
// and a record of the mainchain the test writes itself.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <addresstype.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <common/signmessage.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <interfaces/chain.h>
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
#include <pos/income.h>
#include <pos/producer.h>
#include <pos/protection.h>
#include <pos/schedule.h>
#include <pos/staking.h>
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
#ifdef ENABLE_WALLET
#include <rpc/request.h>
#include <rpc/util.h>
#include <univalue.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>
#endif

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <fstream>
#include <memory>
#include <thread>
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
    std::optional<int64_t> seed_lookahead;
    explicit InstallFastBlocks(bool with_validators = true, std::optional<int64_t> lookahead = std::nullopt) : seed_lookahead{lookahead}
    {
        CChainParams::RegTestOptions options;
        options.sidechain_slot = 5;
        options.pos_seed_lookahead = lookahead;
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
    if (installed.seed_lookahead) installed.args.push_back(strprintf("-posseedlookahead=%d", *installed.seed_lookahead));
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

    explicit FastBlocksSetup(bool with_validators = true, std::optional<int64_t> lookahead = std::nullopt)
        : InstallFastBlocks{with_validators, lookahead}, TestingSetup{ChainType::REGTEST, TestOpts{.extra_args = SetupArgs(*this)}}
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
        const auto leader{Hooks().ActiveLeaderOf(slot)};
        BOOST_REQUIRE(leader.status == pos::FastBlocksHooks::LeaderStatus::OK);
        const uint256 pubkey{leader.pubkey};
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
        BOOST_REQUIRE(pos::Producer::Sign(*block, Leader(slot), Genesis()));
        return *block;
    }
    /** After a change: the sections hash, the merkle root, and the leader's signature again. */
    void Resign(CBlock& block, const CKey* signer = nullptr)
    {
        block.ext.hashSections = block.vSections.empty() ? uint256{} : pos::SectionsHash(block.vSections);
        node::RegenerateCommitments(block, Chainman());
        BOOST_REQUIRE(pos::Producer::Sign(block, signer ? *signer : Leader(pos::SlotOf(block.nTime, Pos())), Genesis()));
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

    //
    // Staking
    //

    uint256 Genesis() { return Chainman().GetConsensus().hashGenesisBlock; }
    /** The state of the active chain (the store of the chainstate, which lives as long as it). */
    const sidechain::StoreView& ActiveView()
    {
        LOCK(::cs_main);
        return Chain().SideCache();
    }
    uint160 Owner(const CKey& owner) { return uint160{owner.GetPubKey().GetID()}; }
    pos::StakeState Stake() { return WITH_LOCK(::cs_main, return pos::GetStakeState(Chain().SideState().View())); }
    std::optional<pos::ValidatorRecord> Validator(const uint256& pubkey) { return WITH_LOCK(::cs_main, return pos::GetValidator(Chain().SideState().View(), pubkey)); }
    /**
     * A bond output: `validator` signs the proof of possession for `owner`; `owner_signer`, if given,
     * signs the bond for a transaction that spends `spends` first (a bond to a validator with an owner).
     */
    CTxOut BondOut(const CKey& validator, CAmount amount, const uint160& owner, const CKey* owner_signer = nullptr, const COutPoint& spends = TxWithCoin())
    {
        std::array<unsigned char, 64> pop;
        BOOST_REQUIRE(validator.SignSchnorr(pos::BondPopHash(owner, Genesis()), pop, nullptr, uint256{}));
        std::vector<unsigned char> owner_sig;
        if (owner_signer) BOOST_REQUIRE(owner_signer->SignCompact(MessageHash(pos::BondMessage(XOnly(validator), amount, spends, Genesis())), owner_sig));
        return CTxOut{amount, pos::BondScript(XOnly(validator), owner, pop, owner_sig)};
    }
    /** An unbond output, signed by `owner`. */
    CTxOut UnbondOut(const CKey& validator, CAmount amount, uint32_t sequence, const CKey& owner)
    {
        pos::Unbond unbond;
        unbond.pubkey = XOnly(validator);
        unbond.amount = amount;
        unbond.sequence = sequence;
        BOOST_REQUIRE(owner.SignCompact(MessageHash(pos::UnbondMessage(unbond.pubkey, amount, sequence, Genesis())), unbond.signature));
        return CTxOut{0, pos::UnbondScript(unbond)};
    }
    /** A transaction with these outputs, spending the coin, its change to the key. */
    CMutableTransaction Pay(COutPoint& coin, CTxOut& spent, std::vector<CTxOut> outputs)
    {
        CAmount out{0};
        for (const CTxOut& o : outputs) out += o.nValue;
        outputs.emplace_back(spent.nValue - out - TX_FEE, script);
        const CMutableTransaction tx{Spend(coin, spent, outputs)};
        coin = COutPoint{tx.GetHash(), static_cast<uint32_t>(outputs.size() - 1)};
        spent = outputs.back();
        return tx;
    }
    /** Why the mempool turns a transaction away. */
    std::string Refused(const CMutableTransaction& tx)
    {
        const auto result{WITH_LOCK(::cs_main, return Chainman().ProcessTransaction(MakeTransactionRef(tx), /*test_accept=*/true))};
        return result.m_result_type == MempoolAcceptResult::ResultType::VALID ? std::string{} : result.m_state.GetRejectReason();
    }
    /** The staking rules on a transaction, on an overlay of the tip's state that is dropped. */
    std::string StakingRules(const CTransaction& tx)
    {
        LOCK(::cs_main);
        sidechain::StoreOverlay overlay{Chain().SideState().View()};
        std::string reason;
        if (Hooks().GetStaking().ApplyTx(overlay, tx, Tip()->nHeight + 1, Fast().anchors, reason)) return {};
        return reason;
    }
    /** The coin TxWith spends. */
    static COutPoint TxWithCoin() { return COutPoint{Txid::FromUint256(uint256{0x77}), 0}; }
    CTransaction TxWith(std::vector<CTxOut> outputs)
    {
        CMutableTransaction tx;
        tx.vin.emplace_back(TxWithCoin());
        tx.vout = std::move(outputs);
        return CTransaction{tx};
    }
    /** Two blocks of stake of slot base_slot + i on the tip, both signed by its leader. */
    std::pair<CBlock, CBlock> TwoBlocks(int i)
    {
        const CBlock first{PosBlock(i)};
        CBlock second{first};
        SetCoinbase(second, [](CMutableTransaction& tx) { tx.vout.emplace_back(0, CScript() << OP_RETURN); });
        Resign(second);
        return {first, second};
    }
    /** What AfterBlock says of a block on the tip, on a state over `base`. */
    std::string After(const CBlock& block, const sidechain::StoreView& base)
    {
        sidechain::StoreOverlay overlay{base};
        sidechain::State state{overlay};
        std::string reason;
        LOCK(::cs_main);
        if (Hooks().AfterBlock(state, block, Tip()->nHeight + 1, std::nullopt, 0, Chainman().GetConsensus().sidechain, reason)) return {};
        return reason;
    }
    const CKey& NotLeader(int64_t slot)
    {
        for (const CKey& k : keys) {
            if (XOnly(k) != XOnly(Leader(slot))) return k;
        }
        BOOST_FAIL("every key leads the slot");
        return keys[0];
    }
    /** The block of stake of slot base_slot + i carrying these proofs. */
    CBlock WithEvidence(int i, const std::vector<pos::DoubleProposal>& evidence, const std::optional<pos::AnchorAck>& ack = std::nullopt)
    {
        const int64_t slot{Slot(i)};
        std::string error;
        auto block{producer->Build(slot, ack, error, evidence)};
        BOOST_REQUIRE_MESSAGE(block, error);
        BOOST_REQUIRE(pos::Producer::Sign(*block, Leader(slot), Genesis()));
        return *block;
    }
    /** Whether a coinbase pays this output. */
    static bool Pays(const CBlock& block, const CTxOut& out)
    {
        const auto& vout{block.vtx.at(0)->vout};
        return std::find(vout.begin(), vout.end(), out) != vout.end();
    }

    /** Headers as a peer sends them: why they are turned away (nothing if they are taken). */
    //! Why the node refuses these headers from a peer (`source`), if it does.
    std::string HeaderReason(const std::vector<CBlockHeader>& headers, int64_t source = 0)
    {
        BlockValidationState state;
        if (Chainman().ProcessNewBlockHeaders(headers, /*min_pow_checked=*/true, state, nullptr, source)) return {};
        return state.GetRejectReason();
    }
    uint256 BestHeader() { return WITH_LOCK(::cs_main, return Chainman().m_best_header->GetBlockHash()); }
    CBlockIndex* Index(const uint256& hash) { return WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(hash)); }
    size_t Waiting() { return WITH_LOCK(::cs_main, return Chainman().m_bmm_waiting.size()); }
    /**
     * The header of a block of stake on `prev` at `slot`, acknowledging the commitment at `nonce` (0:
     * none), that nobody signed: what anyone can make. `like`: a header of a block of stake to take
     * the version from.
     */
    CBlockHeader UnsignedHeader(const CBlockHeader& like, const uint256& prev, int64_t slot, uint32_t nonce, uint8_t salt = 0)
    {
        CBlockHeader header{like};
        header.hashPrevBlock = prev;
        header.hashMerkleRoot = uint256{salt};
        header.nTime = static_cast<uint32_t>(pos::SlotTime(slot, Pos()));
        header.nNonce = nonce;
        header.ext.hashSections = nonce ? uint256{0xa0} : uint256{};
        header.ext.sig.fill(0);
        return header;
    }
    /** Acknowledge a ticket for the tip in a block of stake of slot base_slot + i, taken: an anchor. */
    pos::AnchorAck AckTip(int i)
    {
        const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
        Take(PosBlock(i, ack));
        return ack;
    }
};

struct NoValidatorsSetup : public FastBlocksSetup {
    NoValidatorsSetup() : FastBlocksSetup{/*with_validators=*/false} {}
};

/** The genesis validators, and the seed of an epoch taken at its start (no lookahead). */
struct SeedSetup : public FastBlocksSetup {
    SeedSetup() : FastBlocksSetup{/*with_validators=*/true, /*lookahead=*/0} {}
};

/** Wait (real time) until `done`, at most `seconds`; whether it came. */
bool WaitFor(const std::function<bool()>& done, int seconds = 20)
{
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{seconds}};
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return true;
}

} // namespace

#ifdef ENABLE_WALLET
namespace wallet {
RPCMethod getbalances();
RPCMethod gettransaction();
} // namespace wallet
#endif

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

BOOST_AUTO_TEST_CASE(network_parameters)
{
    // The user's decisions (design, section 13): 30-second slots; blocks of stake halt after an hour
    // without anchors; BMM blocks take transactions after 30 minutes without blocks of stake (10 on
    // the test networks), which is sooner, so that the chain goes on in BMM blocks.
    for (const auto& params : {CChainParams::Main(), CChainParams::TestNet(), CChainParams::SigNet({})}) {
        const Consensus::PosParams& pos{params->GetConsensus().pos};
        BOOST_CHECK_EQUAL(pos.slot_seconds, 30);
        BOOST_CHECK_EQUAL(pos.slot_seconds * pos.max_unanchored_slots, 60 * 60);
        BOOST_CHECK_LT(pos.stall_seconds, pos.slot_seconds * pos.max_unanchored_slots);
        BOOST_CHECK_EQUAL(pos.producer_share_bps, 5000);
        // Bitcoin's heuristics count a block a slot; version bits periods stay 2016 blocks.
        BOOST_CHECK_EQUAL(params->GetConsensus().nPowTargetSpacing, pos.slot_seconds);
        BOOST_CHECK_EQUAL(params->GetConsensus().DifficultyAdjustmentInterval(), 2016);
    }
    BOOST_CHECK_EQUAL(CChainParams::Main()->GetConsensus().pos.stall_seconds, 30 * 60);
    BOOST_CHECK_EQUAL(Consensus::PosParams{}.slot_seconds * Consensus::PosParams{}.max_unanchored_slots, 60 * 60);
    // Regtest: one-second slots, stalls of 30 slots; -posslotseconds (a soak test) sets the slot.
    CChainParams::RegTestOptions options;
    options.sidechain_slot = 5;
    BOOST_CHECK_EQUAL(CChainParams::RegTest(options)->GetConsensus().pos.slot_seconds, 1);
    BOOST_CHECK_EQUAL(CChainParams::RegTest(options)->GetConsensus().pos.stall_seconds, 30);
    options.pos_slot_seconds = 30;
    BOOST_CHECK_EQUAL(CChainParams::RegTest(options)->GetConsensus().pos.slot_seconds, 30);
    BOOST_CHECK_EQUAL(CChainParams::RegTest(options)->GetConsensus().pos.stall_seconds, 30 * 30);
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
        const auto leader{set.Leader(slot, pos::ValidatorSet::Seed(pos::EpochOf(slot, params), uint256{9}))};
        BOOST_REQUIRE(leader);
        BOOST_CHECK(leader == set.Leader(slot, pos::ValidatorSet::Seed(pos::EpochOf(slot, params), uint256{9})));
        ++led[*leader];
    }
    BOOST_CHECK_GT(led[0], 1400);
    BOOST_CHECK_GT(led[1], 100);
    BOOST_CHECK_GT(led[2], 100);
    // Too few validators: no leader.
    params.min_active_validators = 4;
    BOOST_CHECK(!pos::ValidatorSet{params}.Active());
    BOOST_CHECK(!pos::ValidatorSet{params}.Leader(1, uint256{9}));
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
        BOOST_CHECK(pos::CheckBlockSig(header, XOnly(Leader(slot)), Genesis()));
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
        // Kept to try again later only if its leader signed it: not a header anyone can make.
        CBlock unsigned_block{block};
        unsigned_block.ext.sig[0] ^= 1;
        BOOST_CHECK_EQUAL(Check(unsigned_block), "bad-pos-sig");
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
    BOOST_CHECK(!Hooks().GenesisSet().Active());
    BOOST_CHECK(!Hooks().PosActiveNow());
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
        // (The mainchain block before committed to a BMM block, the last anchor: a header that names
        // it is refused as such, and the state's rule alone says the commitment is not there.)
        pos::AnchorAck wrong{ack};
        wrong.main_height = ack.main_height - 1;
        const CBlock block{WithAck(PosBlock(3), wrong)};
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-ack-order");
        std::optional<sidechain::PegStep> step;
        std::string reason;
        BOOST_CHECK(!Hooks().GetPegStep(block, Record(), step, reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-ack-height");
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
    // Chained: the next ticket anchors the block the last anchor anchored, or a block after it -- one
    // below the acknowledgement too (a ticket made for the next mainchain block before the last
    // commitment was acknowledged: anchors need not wait for acknowledgements). Its anchor builds no
    // bundle (the peg actions of the acknowledgement came between the block it anchors and it); one
    // that anchors the acknowledgement or a block after it does.
    {
        const pos::AnchorAck below{Anchor(Ticket(anchored->pprev->GetBlockHash(), other_payee))};
        BOOST_CHECK_EQUAL(Check(WithAck(PosBlock(4), below)), "bad-pos-ack-chain");
        const pos::AnchorAck early{Anchor(Ticket(anchored->GetBlockHash(), other_payee))};
        const pos::AnchorAck chained{Anchor(Ticket(acked.GetHash(), payee))};
        {
            LOCK(::cs_main);
            BOOST_CHECK_EQUAL(*Hooks().AckPegStep(early, *anchored, *Tip()).max_withdrawal_height, -1);
            BOOST_CHECK_EQUAL(*Hooks().AckPegStep(chained, *Tip(), *Tip()).max_withdrawal_height, Tip()->nHeight);
        }
        Take(PosBlock(4, early));
        BOOST_CHECK_EQUAL(Key().anchors, before.anchors + 2);
        BOOST_CHECK(Fast().last_anchored == anchored->GetBlockHash());
        Take(PosBlock(5, chained));
        BOOST_CHECK_EQUAL(Key().anchors, before.anchors + 3);
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
    // An acknowledgement committed on record, but no block index to find what it anchors. (Extended
    // blocks on for it, as installed hooks would have them.)
    {
        SetBlockExtensions(true);
        pos::PosSections sections;
        sections.ack = pos::AnchorAck{.ticket = pos::AnchorTicket{.version = 1, .anchored = uint256{7}, .payee = CScript() << OP_TRUE}, .main_height = 1};
        CBlock acking{block};
        acking.vSections = sections.Encode();
        sidechain::Mainchain record;
        sidechain::MainBlock first;
        first.hash = uint256{1};
        BOOST_REQUIRE(record.Append(first));
        sidechain::MainBlock committing;
        committing.hash = uint256{2};
        committing.prev_hash = first.hash;
        committing.bmm = sections.ack->ticket.Hash();
        BOOST_REQUIRE(record.Append(committing));
        std::optional<sidechain::PegStep> step;
        BOOST_CHECK(!hooks.GetPegStep(acking, record, step, reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-context");
        // Sections that do not read.
        acking.vSections = {9};
        BOOST_CHECK(!hooks.GetPegStep(acking, record, step, reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-sections");
        SetBlockExtensions(false);
    }
    // Without validators, no block of stake.
    BOOST_CHECK(!hooks.GenesisSet().Active());
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
    const int64_t slot{pos::SlotOf(first.nTime, Pos())};
    const uint256 leader{XOnly(Leader(slot))};
    std::string reason;

    // Two headers of one slot, both signed by its leader: the offence.
    const pos::DoubleProposal proof{.a = first, .b = second};
    const auto offence{pos::CheckEvidence(proof, leader, Genesis(), Pos(), reason)};
    BOOST_REQUIRE_MESSAGE(offence, reason);
    BOOST_CHECK(offence->pubkey == XOnly(Leader(slot)));
    BOOST_CHECK_EQUAL(offence->slot, slot);
    // Headers of blocks of stake.
    BOOST_CHECK(!pos::CheckEvidence({.a = first, .b = BmmBlock(/*use_mempool=*/false)}, leader, Genesis(), Pos(), reason));
    BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-kind");
    // Of one slot, which starts at their time.
    {
        CBlock later{PosBlock(2)};
        BOOST_CHECK(!pos::CheckEvidence({.a = first, .b = later}, leader, Genesis(), Pos(), reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-slot");
        CBlockHeader a{first}, b{second};
        a.nTime = b.nTime = static_cast<uint32_t>(Pos().genesis_time);
        BOOST_CHECK(!pos::CheckEvidence({.a = a, .b = b}, leader, Genesis(), Pos(), reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-slot");
    }
    // Two headers, not one twice.
    BOOST_CHECK(!pos::CheckEvidence({.a = first, .b = first}, leader, Genesis(), Pos(), reason));
    BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-same");
    // Both signed by the leader of the slot.
    for (const CKey& other : keys) {
        if (XOnly(other) == XOnly(Leader(slot))) continue;
        CBlock forged{second};
        Resign(forged, &other);
        BOOST_CHECK(!pos::CheckEvidence({.a = first, .b = forged}, leader, Genesis(), Pos(), reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-sig");
    }
    // Not the leader's key: no offence of its.
    for (const CKey& other : keys) {
        if (XOnly(other) == leader) continue;
        BOOST_CHECK(!pos::CheckEvidence(proof, XOnly(other), Genesis(), Pos(), reason));
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
    for (int64_t i{0}; i <= static_cast<int64_t>(pos::MAX_SIGNED_HEADERS); ++i) BOOST_CHECK(!pool.NoteSigned(header, i, uint256{1}));
    BOOST_CHECK(!pool.NoteSigned(second, 0, uint256{1}));
    BOOST_CHECK(pool.NoteSigned(second, static_cast<int64_t>(pos::MAX_SIGNED_HEADERS), uint256{1}));
    BOOST_CHECK(!pool.NoteSigned(header, 5, uint256{1}));
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
    {
        DataStream stream{};
        stream << uint8_t{1};
        std::string misbehaving;
        BOOST_CHECK(Hooks().ProcessMessage(7, pos::MSG_TICKET, stream, misbehaving));
        BOOST_CHECK_EQUAL(misbehaving, "malformed anchor ticket");
    }
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

BOOST_FIXTURE_TEST_CASE(withheld_header, FastBlocksSetup)
{
    // A leader signs a header that claims an acknowledgement (more anchors than any chain) and keeps
    // its block: the node's best header has more work than its tip for as long as the block does not
    // come. The validators go on making blocks all the same.
    StartPos();
    Take(PosBlock(1));
    // (A commitment after the last anchor's, whose ticket the leader claims to have.)
    Commit(uint256{0xc1});
    CBlock claimed{PosBlock(2)};
    pos::PosSections sections;
    sections.ack = pos::AnchorAck{.ticket = Ticket(Tip()->GetBlockHash(), script), .main_height = Record().Height()};
    claimed.vSections = sections.Encode();
    claimed.nNonce = static_cast<uint32_t>(Record().Height());
    Resign(claimed);
    {
        BlockValidationState state;
        BOOST_REQUIRE_MESSAGE(Chainman().ProcessNewBlockHeaders({{claimed}}, /*min_pow_checked=*/true, state), state.ToString());
    }
    BOOST_CHECK(WITH_LOCK(::cs_main, return Chainman().m_best_header->GetBlockHash()) == claimed.GetHash());
    BOOST_CHECK(!WITH_LOCK(::cs_main, return pos::Producer::Behind(Chainman())));
    std::string error;
    const auto made{producer->Produce(Slot(3), error)};
    BOOST_REQUIRE_MESSAGE(made, error);
    BOOST_CHECK(Tip()->GetBlockHash() == *made);

    // A block of more work the node has, and has yet to connect: behind, no block.
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    const CBlock better{PosBlock(4, ack)};
    {
        LOCK(::cs_main);
        BlockValidationState state;
        CBlockIndex* index{nullptr};
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(Chainman().AcceptBlock(std::make_shared<const CBlock>(better), state, &index, /*fRequested=*/true, /*dbp=*/nullptr, &new_block, /*min_pow_checked=*/true),
                              state.ToString());
        BOOST_CHECK(Chain().setBlockIndexCandidates.contains(index));
        BOOST_CHECK(pos::Producer::Behind(Chainman()));
    }
    BOOST_CHECK(!producer->Produce(Slot(5), error));
    BOOST_CHECK_EQUAL(error, "the node is still taking in the best chain");
    {
        BlockValidationState state;
        BOOST_REQUIRE(Chain().ActivateBestChain(state));
    }
    BOOST_CHECK(Tip()->GetBlockHash() == better.GetHash());
    BOOST_CHECK(!WITH_LOCK(::cs_main, return pos::Producer::Behind(Chainman())));
}

BOOST_FIXTURE_TEST_CASE(ticket_flood, FastBlocksSetup)
{
    // Anyone can make tickets for a known block by the thousand, and each new one would go to every
    // peer: a node takes MAX_PEER_TICKETS a peer per mainchain block of those the mainchain did not
    // commit to; those it committed to, always.
    StartPos();
    const auto send{[&](int64_t peer, const pos::AnchorTicket& ticket) {
        DataStream stream{};
        stream << ticket;
        std::string misbehaving;
        BOOST_CHECK(Hooks().ProcessMessage(peer, pos::MSG_TICKET, stream, misbehaving));
        BOOST_CHECK_EQUAL(misbehaving, "");
        return Hooks().GetTicket(ticket.Hash()).has_value();
    }};
    const auto ticket{[&](uint32_t i) {
        return Ticket(Tip()->GetBlockHash(), GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, static_cast<unsigned char>(i))}}));
    }};
    for (uint32_t i{0}; i < pos::MAX_PEER_TICKETS; ++i) BOOST_CHECK(send(1, ticket(i)));
    BOOST_CHECK(!send(1, ticket(100)));
    // Another peer has its own count.
    BOOST_CHECK(send(2, ticket(101)));
    // A ticket the mainchain committed to is taken, past the count.
    const pos::AnchorTicket committed{ticket(102)};
    Commit(committed.Hash());
    BOOST_CHECK(send(1, committed));
    // A new mainchain block: the counts start again.
    BOOST_CHECK(send(1, ticket(100)));
}

BOOST_FIXTURE_TEST_CASE(acks_noted_only_when_signed, FastBlocksSetup)
{
    // The blocks that rest on a commitment are noted from blocks that read, before their header is
    // checked: only those signed by the leader of their slot, or a flood of blocks anyone can make
    // would push those that count out of memory.
    StartPos();
    Take(PosBlock(1));
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    CBlock forged{PosBlock(2, ack)};
    forged.ext.sig[5] ^= 1;
    BlockValidationState state;
    BOOST_CHECK(Hooks().CheckBlock(forged, state));
    auto resting{Hooks().BlocksCommittedBy(ack.ticket.Hash())};
    BOOST_CHECK(std::find(resting.begin(), resting.end(), forged.GetHash()) == resting.end());
    // The header rules turn it away.
    BOOST_CHECK_EQUAL(Check(forged), "bad-pos-sig");
    const CBlock signed_block{PosBlock(2, ack)};
    BOOST_CHECK(Hooks().CheckBlock(signed_block, state));
    resting = Hooks().BlocksCommittedBy(ack.ticket.Hash());
    BOOST_CHECK(std::find(resting.begin(), resting.end(), signed_block.GetHash()) != resting.end());
}

BOOST_FIXTURE_TEST_CASE(false_evidence_misbehaves, FastBlocksSetup)
{
    // A proof that is none costs two signature checks: the peer that sends it misbehaves (the
    // validator set is the chain's: an honest node relays only proofs that check).
    StartPos();
    const CBlock first{PosBlock(1)};
    const auto send{[&](const pos::DoubleProposal& proof) {
        DataStream stream{};
        stream << proof;
        std::string misbehaving;
        BOOST_CHECK(Hooks().ProcessMessage(3, pos::MSG_EVIDENCE, stream, misbehaving));
        return misbehaving;
    }};
    BOOST_CHECK_EQUAL(send({.a = first, .b = first}), "invalid evidence: bad-pos-evidence-same");
    CBlock second{first};
    SetCoinbase(second, [](CMutableTransaction& tx) { tx.vout.emplace_back(0, CScript() << OP_RETURN); });
    Resign(second);
    BOOST_CHECK_EQUAL(send({.a = first, .b = second}), "");
    // Known already: no fault.
    BOOST_CHECK_EQUAL(send({.a = second, .b = first}), "");
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 1U);
}

BOOST_FIXTURE_TEST_CASE(bmm_time_ahead, FastBlocksSetup)
{
    // A BMM block may be at most MAX_FUTURE_BLOCK_TIME_POS ahead of the clock while blocks of stake are
    // on (not two hours): a run of them two hours ahead would push the median time past, which blocks
    // of stake must come after, two hours ahead.
    BOOST_CHECK_EQUAL(Hooks().MaxFutureBlockTime(), pos::MAX_FUTURE_BLOCK_TIME_POS);
    StartPos();
    const auto header_reason{[&](int64_t ahead) {
        CBlock block{BmmBlock(/*use_mempool=*/false)};
        block.nTime = static_cast<uint32_t>(GetTime() + ahead);
        FinishBmm(block);
        Commit(block.GetHash());
        BlockValidationState state;
        if (Chainman().ProcessNewBlockHeaders({{block}}, /*min_pow_checked=*/true, state)) return std::string{};
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_TIME_FUTURE);
        return state.GetRejectReason();
    }};
    BOOST_CHECK_EQUAL(header_reason(pos::MAX_FUTURE_BLOCK_TIME_POS + 1), "time-too-new");
    BOOST_CHECK_EQUAL(header_reason(pos::MAX_FUTURE_BLOCK_TIME_POS), "");
}

BOOST_FIXTURE_TEST_CASE(bmm_time_ahead_without_validators, NoValidatorsSetup)
{
    // Without blocks of stake the chain is the template's: two hours.
    BOOST_CHECK_EQUAL(Hooks().MaxFutureBlockTime(), MAX_FUTURE_BLOCK_TIME);
}

BOOST_FIXTURE_TEST_CASE(moved_commitment, FastBlocksSetup)
{
    // The order of chains counts a BMM block as an anchor committed at the height its commitment has
    // on the record. A reorg of the mainchain that moves the commitment to another height (the
    // follower takes the block back and checks it again) moves it in the key too: else nodes that saw
    // it before the reorg and nodes that saw it after would order chains differently.
    StartPos();
    Take(PosBlock(1));
    const CBlock bmm{BmmBlock(/*use_mempool=*/false)};
    MineBmm(bmm);
    const int committed_at{Record().Height()};
    BOOST_CHECK_EQUAL(Key().last_anchor_height, static_cast<uint32_t>(committed_at));
    // The mainchain drops the block that committed to it, and commits to it again one block later.
    Record().Truncate(committed_at - 1);
    Main();
    Commit(bmm.GetHash());
    BOOST_REQUIRE_EQUAL(Record().Height(), committed_at + 1);
    CBlockIndex* index{WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(bmm.GetHash()))};
    BlockValidationState state;
    BOOST_REQUIRE(Chain().InvalidateBlock(state, index, /*by_record=*/true));
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Chain().ReconsiderRecordFailure(index)));
    BOOST_REQUIRE(Chain().ActivateBestChain(state));
    BOOST_REQUIRE(Tip() == index);
    BOOST_CHECK_EQUAL(Key().last_anchor_height, static_cast<uint32_t>(committed_at + 1));
    // As a node that never saw it at the old height computes it.
    BOOST_CHECK(WITH_LOCK(::cs_main, return Hooks().ChainWork(index->pprev, index->GetBlockHeader())) == index->nChainWork);
    // And blocks on it follow.
    Take(PosBlock(2));
    BOOST_CHECK_EQUAL(Key().last_anchor_height, static_cast<uint32_t>(committed_at + 1));
}

BOOST_AUTO_TEST_CASE(net_anchors)
{
    // Anchors of a chain (newest first): C acknowledged at 10 covers up to 9 (commitment 102), A at 7
    // up to 5 (101), Z at 3 up to 2 (99).
    std::vector<CBlockIndex> blocks(11);
    for (int i{0}; i < 11; ++i) blocks[i].nHeight = i;
    const std::vector<pos::ChainAnchor> anchors{{.block = &blocks[10], .anchored = &blocks[9], .main_height = 102},
                                                {.block = &blocks[7], .anchored = &blocks[5], .main_height = 101},
                                                {.block = &blocks[3], .anchored = &blocks[2], .main_height = 99}};
    // Without commitments skipped: the blocks the anchors anchor.
    BOOST_CHECK(pos::NetAnchoredHeights(anchors, {}, 3) == (std::vector<int>{9, 5, 2}));
    // The commitment at 100 skipped: a chain that forks at 6 and acknowledges it and A's ticket (whose
    // block, 5, it has) has as many anchors as this one after 6 (A and C), the last one earlier (101
    // against 102): block 8, which only C covers, is not anchored; block 5 is, by one.
    const std::vector<int> net{pos::NetAnchoredHeights(anchors, {{.main_height = 100}}, 3)};
    BOOST_CHECK(net == (std::vector<int>{5, 2, 2}));
    BOOST_CHECK(pos::FinalityAt(8, 3, net, 3).status == pos::FinalityStatus::SOFT);
    BOOST_CHECK_EQUAL(pos::FinalityAt(5, 6, net, 3).anchors, 1);
    // A commitment skipped after C counts against every block C covers.
    BOOST_CHECK(pos::NetAnchoredHeights(anchors, {{.main_height = 103}}, 3) == (std::vector<int>{5, 2, 2}));
    BOOST_CHECK(pos::NetAnchoredHeights({}, {{.main_height = 100}}, 3).empty());
    // Known to rest on a block of another branch, which leaves this chain at 8: only a fork there has
    // it (block 9, covered by C, is not anchored; block 8 is, as a fork below it cannot have it).
    BOOST_CHECK(pos::NetAnchoredHeights(anchors, {{.main_height = 103, .exact_fork = 8}}, 3) == (std::vector<int>{8, 5, 2}));
    // Known to name block 6 of this chain: forks at 6 or above have it.
    BOOST_CHECK(pos::NetAnchoredHeights(anchors, {{.main_height = 100, .min_fork = 6}}, 3) == (std::vector<int>{6, 5, 2}));
}

BOOST_FIXTURE_TEST_CASE(hidden_anchor, FastBlocksSetup)
{
    // The order of chains: with as many anchors, the last committed first wins. A bidder that wins one
    // auction with the ticket of a block it keeps to itself, on a branch of its own, can later show the
    // branch with that acknowledgement and take back a block of the chain that an anchor committed
    // after covers. Such a block is not anchored, net of the commitment skipped.
    StartPos();
    Take(PosBlock(1));
    const CBlock hidden{PosBlock(2)};
    Take(PosBlock(3));
    Take(PosBlock(4));
    const CBlockIndex* covered{Tip()};
    const pos::AnchorTicket kept{Ticket(hidden.GetHash(), script)};
    Commit(kept.Hash());
    const int kept_height{Record().Height()};
    Take(PosBlock(5, Anchor(Ticket(covered->GetBlockHash(), script))));
    const auto finality{[&](const CBlockIndex* block) { return WITH_LOCK(::cs_main, return pos::FinalityOf(Hooks(), *block, *Tip(), 3)); }};
    // An anchor covers it, but the commitment skipped before (one this node has no ticket of) may be
    // the anchor of a chain that leaves it out.
    BOOST_CHECK(finality(covered).status == pos::FinalityStatus::SOFT);
    BOOST_CHECK(WITH_LOCK(::cs_main, return pos::AnchoredHeights(Hooks(), *Tip(), 3)).empty() ||
                WITH_LOCK(::cs_main, return pos::AnchoredHeights(Hooks(), *Tip(), 3)).front() < covered->nHeight - 2);

    // And so it is: the branch, with the acknowledgement of the ticket kept, takes over.
    Submit(hidden);
    CBlockIndex* fork_child{WITH_LOCK(::cs_main, return Chain().m_chain[covered->nHeight - 1])};
    {
        BlockValidationState state;
        BOOST_REQUIRE(Chain().InvalidateBlock(state, fork_child));
        BOOST_REQUIRE(Chain().ActivateBestChain(state));
    }
    BOOST_REQUIRE(Tip()->GetBlockHash() == hidden.GetHash());
    const CBlock attack{PosBlock(6, pos::AnchorAck{.ticket = kept, .main_height = kept_height})};
    WITH_LOCK(::cs_main, Chain().ResetBlockFailureFlags(fork_child));
    Submit(attack);
    BOOST_CHECK(Tip()->GetBlockHash() == attack.GetHash());
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Chain().m_chain.Contains(*covered)));

    // On the branch that won, the commitment of the other branch's ticket is skipped in turn; but the
    // node knows where that ticket's block leaves this chain: below the attack, so it counts only
    // against blocks above that fork and below the attack, not against the attack.
    const CBlockIndex* attacked{Tip()};
    Take(PosBlock(7, Anchor(Ticket(Tip()->GetBlockHash(), script))));
    BOOST_CHECK_EQUAL(finality(attacked).anchors, 1);
    BOOST_CHECK(finality(attacked).status == pos::FinalityStatus::ANCHORED);
    // The hidden block itself, just above the fork of the other branch: that branch has one anchor
    // since the fork, this one two (the ticket kept, and the new one).
    BOOST_CHECK_EQUAL(finality(attacked->pprev).anchors, 1);
    Take(PosBlock(8, Anchor(Ticket(Tip()->GetBlockHash(), script))));
    BOOST_CHECK_EQUAL(finality(attacked).anchors, 2);
    BOOST_CHECK_EQUAL(finality(attacked->pprev).anchors, 2);
}

BOOST_FIXTURE_TEST_CASE(stale_tickets, FastBlocksSetup)
{
    // Blocks of stake stopped for want of an anchor, long enough that a ticket for the block before
    // the tip (ticket depth 1) can no longer be acknowledged: the blocks above it would have been
    // uncovered too long. Leaders do not try it (it would fail the slot), the merged miner does not
    // wait for it (it would wait for ever), and a new ticket names the tip, which is in time.
    StartPos();
    Take(PosBlock(1));
    Take(PosBlock(2));
    const CBlockIndex* stopped{Tip()};
    const pos::AnchorAck old{Anchor(Ticket(stopped->pprev->GetBlockHash(), script))};
    const int last{static_cast<int>(Pos().max_unanchored_slots)};
    // Still in time: the leader acknowledges it.
    BOOST_CHECK(WITH_LOCK(::cs_main, return Hooks().NextAck(*stopped, Fast(), base_slot + 3)) == std::optional<pos::AnchorAck>{old});
    BOOST_CHECK(WITH_LOCK(::cs_main, return Hooks().TicketTarget(*stopped, Fast(), base_slot + 3)) == stopped->pprev);
    // Too late: the block of stake above it (slot 2) would be uncovered for max_unanchored_slots.
    const int64_t late{base_slot + 2 + last};
    BOOST_CHECK_EQUAL(Check(PosBlock(2 + last, old)), "bad-pos-unanchored");
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Hooks().Acknowledgeable(old.ticket, *stopped, Fast(), late)));
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Hooks().NextAck(*stopped, Fast(), late)));
    BOOST_CHECK(WITH_LOCK(::cs_main, return Hooks().TicketTarget(*stopped, Fast(), late)) == stopped);
    std::string error;
    BOOST_CHECK(!producer->Produce(Slot(2 + last), error));
    BOOST_CHECK_EQUAL(error, "no anchor for too long: blocks of stake wait for one");
    // A ticket for the tip is in time, and starts the blocks of stake again.
    const pos::AnchorAck fresh{Anchor(Ticket(stopped->GetBlockHash(), script))};
    BOOST_CHECK(WITH_LOCK(::cs_main, return Hooks().NextAck(*stopped, Fast(), late + 1)) == std::optional<pos::AnchorAck>{fresh});
    const auto made{producer->Produce(Slot(3 + last), error)};
    BOOST_REQUIRE_MESSAGE(made, error);
    BOOST_CHECK_EQUAL(Fast().last_main_height, fresh.main_height);
    Take(PosBlock(4 + last));
}

BOOST_FIXTURE_TEST_CASE(swapped_signature, FastBlocksSetup)
{
    // The block hash does not cover the leader's signature: a node that has the header of a block of
    // stake turns away a block with that hash and another signature (another key's, or none) as a
    // mutated block, without taking the header for invalid; the block its leader signed is taken after.
    StartPos();
    Take(PosBlock(1));
    const CBlock honest{PosBlock(2)};
    const CKey& leader{Leader(pos::SlotOf(honest.nTime, Pos()))};
    const CKey* other{nullptr};
    for (const CKey& k : keys) {
        if (XOnly(k) != XOnly(leader)) other = &k;
    }
    BOOST_REQUIRE(other);
    CBlock swapped{honest};
    BOOST_REQUIRE(pos::Producer::Sign(swapped, *other, Genesis()));
    CBlock zeroed{honest};
    zeroed.ext.sig.fill(0);
    BOOST_REQUIRE(swapped.GetHash() == honest.GetHash() && zeroed.GetHash() == honest.GetHash());
    BOOST_REQUIRE(swapped.ext.sig != honest.ext.sig);

    // Without the header, the signature is checked with it: bad-pos-sig, and no index.
    Submit(swapped);
    BOOST_CHECK_EQUAL(Reason(swapped), "bad-pos-sig");
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(honest.GetHash())));

    // With the header: a mutated block.
    BlockValidationState header_state;
    BOOST_REQUIRE_MESSAGE(Chainman().ProcessNewBlockHeaders({{honest}}, /*min_pow_checked=*/true, header_state), header_state.ToString());
    const CBlockIndex* index{WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(honest.GetHash()))};
    BOOST_REQUIRE(index);
    for (const CBlock& bad : {swapped, zeroed}) {
        BlockValidationState state;
        BOOST_CHECK(!WITH_LOCK(::cs_main, return Chainman().AcceptBlock(std::make_shared<const CBlock>(bad), state, nullptr, true, nullptr, nullptr, true)));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-extension");
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_MUTATED);
        Submit(bad);
        BOOST_CHECK_EQUAL(Reason(bad), "bad-blk-extension");
        LOCK(::cs_main);
        BOOST_CHECK(!(index->nStatus & BLOCK_FAILED_VALID));
        BOOST_CHECK(!(index->nStatus & BLOCK_HAVE_DATA));
        BOOST_CHECK(index->m_ext->sig == honest.ext.sig);
    }
    BOOST_CHECK(Tip() == index->pprev);
    // The block its leader signed: taken, and stored with its signature.
    Take(honest);
    CBlock read;
    BOOST_REQUIRE(Chainman().m_blockman.ReadBlock(read, *index));
    BOOST_CHECK(read.ext == honest.ext);
    // Afterwards, another signature is still a mutated block, and changes nothing.
    BlockValidationState state;
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Chainman().AcceptBlock(std::make_shared<const CBlock>(swapped), state, nullptr, true, nullptr, nullptr, true)));
    BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_MUTATED);
    BOOST_CHECK(Tip() == index);
    Take(PosBlock(3));
}

BOOST_FIXTURE_TEST_CASE(bundle_fixed_by_anchored_block, FastBlocksSetup)
{
    // Design 4.3.4: the bundle is the anchored block P's -- its withdrawals, its hash as the nonce,
    // its height as the lock time -- whatever the height of the block Q that acknowledges the
    // anchor: two branches that acknowledge the same one build the same bundle.
    COutPoint coin;
    const CTxOut spent{Fund(COIN, coin)};
    StartPos();
    const CMutableTransaction withdrawal{Spend(coin, spent, {CTxOut{COIN / 2, sidechain::WithdrawalScript(1000, key.GetPubKey().GetID(), script)},
                                                             CTxOut{COIN / 2 - TX_FEE, script}})};
    Accept(withdrawal);
    Take(PosBlock(1));
    Take(PosBlock(2));
    const CBlockIndex* anchored{Tip()};
    const pos::AnchorAck ack{Anchor(Ticket(anchored->GetBlockHash(), script))};
    const auto bundle_of{[&](const CBlock& block) {
        std::optional<uint256> found;
        for (const CTxOut& out : block.vtx[0]->vout) {
            if (const auto hash{sidechain::ParseBundleCommitScript(out.scriptPubKey)}) found = hash;
        }
        return found;
    }};
    // Branch 1: Q right on P.
    const CBlock q1{PosBlock(3, ack)};
    const auto bundle1{bundle_of(q1)};
    BOOST_REQUIRE(bundle1);
    Take(q1);
    {
        LOCK(::cs_main);
        const auto pending{Chain().SideState().Bundle()};
        BOOST_REQUIRE(pending);
        BOOST_CHECK(pending->hash == *bundle1);
        BOOST_CHECK_EQUAL(pending->height, anchored->nHeight);
        BOOST_CHECK(pending->nonce == anchored->GetBlockHash());
        BOOST_CHECK_EQUAL(Assert(Chain().SideState().BundleTx())->nLockTime, static_cast<uint32_t>(anchored->nHeight));
    }
    // The step the chain's rules give Q: P's height for the peg actions.
    {
        std::optional<sidechain::PegStep> step;
        std::string reason;
        BOOST_REQUIRE_MESSAGE(Hooks().GetPegStep(q1, Record(), step, reason), reason);
        BOOST_REQUIRE(step);
        BOOST_CHECK(step->peg_height == std::optional<int32_t>{anchored->nHeight});
        BOOST_CHECK(step->max_withdrawal_height == std::optional<int32_t>{anchored->nHeight});
    }
    // Branch 2: a block of stake between P and Q.
    CBlockIndex* q1_index{WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(q1.GetHash()))};
    {
        BlockValidationState state;
        BOOST_REQUIRE(Chain().InvalidateBlock(state, q1_index));
    }
    BOOST_REQUIRE(Tip() == anchored);
    Take(PosBlock(4));
    const CBlock q2{PosBlock(5, ack)};
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return Chain().m_chain.Height()) + 1, anchored->nHeight + 2);
    BOOST_CHECK(bundle_of(q2) == bundle1);
    // A Q that commits to the bundle built at its own height (as before) is refused.
    {
        const int q2_height{anchored->nHeight + 2};
        const auto own{WITH_LOCK(::cs_main, return Chain().SideState().NextBundle(q2_height, anchored->GetBlockHash(), Chainman().GetConsensus().sidechain, nullptr, anchored->nHeight))};
        BOOST_REQUIRE(own);
        BOOST_CHECK_EQUAL(own->nLockTime, static_cast<uint32_t>(q2_height));
        BOOST_REQUIRE(own->GetHash().ToUint256() != *bundle1);
        CBlock own_height{q2};
        SetCoinbase(own_height, [&](CMutableTransaction& coinbase) {
            for (CTxOut& out : coinbase.vout) {
                if (sidechain::ParseBundleCommitScript(out.scriptPubKey)) out.scriptPubKey = sidechain::BundleCommitScript(own->GetHash().ToUint256());
            }
        });
        Resign(own_height);
        BOOST_CHECK_EQUAL(Check(own_height), "bad-sc-bundle-hash");
    }
    Take(q2);
    BOOST_CHECK(WITH_LOCK(::cs_main, return Chain().SideState().Bundle()->hash) == *bundle1);
}

BOOST_AUTO_TEST_CASE(staking_parameters)
{
    // The user's decisions (design, section 13): a minimum stake of 1 CHN (0.01 on the test networks,
    // 0.0001 on regtest), 4 to 100 validators (3 to 32; 1 to 8), slashing of 10 % with 1/8 to the
    // reporter, about two days to unbond. The main and test networks start with no validators: merged
    // mined only until enough stake is bonded.
    const auto main{CChainParams::Main()};
    const Consensus::PosParams& m{main->GetConsensus().pos};
    BOOST_CHECK_EQUAL(m.min_stake, COIN);
    BOOST_CHECK_EQUAL(m.min_active_validators, 4U);
    BOOST_CHECK_EQUAL(m.max_validators, 100U);
    BOOST_CHECK_EQUAL(m.slots_per_epoch * m.slot_seconds, 60 * 60);
    // Two days of anchors, one a mainchain block (a minute).
    BOOST_CHECK_EQUAL(m.unbonding_anchors, 2 * 24 * 60);
    for (const auto& params : {CChainParams::Main(), CChainParams::TestNet(), CChainParams::SigNet({})}) {
        const Consensus::PosParams& pos{params->GetConsensus().pos};
        BOOST_CHECK(pos.genesis_validators.empty());
        BOOST_CHECK(!pos::ValidatorSet{pos}.Active());
        BOOST_CHECK_EQUAL(pos.slash_bps, 1000);
        BOOST_CHECK_EQUAL(pos.reporter_share_divisor, 8);
        // A proof lands while the stake is still bonded or unbonding.
        BOOST_CHECK_GE(pos.unbonding_anchors, 2 * pos.evidence_window_anchors);
        // The stake is fixed an epoch before the seed exists (the seed block is mined some minutes after
        // its median time, which is at most an epoch before the epoch starts).
        BOOST_CHECK_LE(pos.seed_lookahead_seconds, pos.slots_per_epoch * pos.slot_seconds);
    }
    for (const auto& params : {CChainParams::TestNet(), CChainParams::SigNet({})}) {
        const Consensus::PosParams& pos{params->GetConsensus().pos};
        BOOST_CHECK_EQUAL(pos.min_stake, COIN / 100);
        BOOST_CHECK_EQUAL(pos.min_active_validators, 3U);
        BOOST_CHECK_EQUAL(pos.max_validators, 32U);
        BOOST_CHECK_EQUAL(pos.slots_per_epoch, 20);
    }
    CChainParams::RegTestOptions options;
    options.sidechain_slot = 5;
    const Consensus::PosParams& reg{CChainParams::RegTest(options)->GetConsensus().pos};
    BOOST_CHECK_EQUAL(reg.min_stake, COIN / 10000);
    BOOST_CHECK_EQUAL(reg.min_active_validators, 1U);
    BOOST_CHECK_EQUAL(reg.max_validators, 8U);
    BOOST_CHECK_EQUAL(reg.slots_per_epoch, 20);
    BOOST_CHECK_EQUAL(reg.unbonding_anchors, 20);
}

BOOST_AUTO_TEST_CASE(staking_scripts)
{
    ECC_Context ecc;
    CKey validator, owner;
    validator.MakeNewKey(/*fCompressed=*/true);
    owner.MakeNewKey(/*fCompressed=*/true);
    const uint160 owner_hash{owner.GetPubKey().GetID()};
    const uint256 genesis{1};
    std::array<unsigned char, 64> pop;
    BOOST_REQUIRE(validator.SignSchnorr(pos::BondPopHash(owner_hash, genesis), pop, nullptr, uint256{}));
    // A bond reads back as written; the proof of possession is for one owner, on one chain.
    const CTxOut bond_out{5000, pos::BondScript(XOnly(validator), owner_hash, pop)};
    const auto bond{pos::ParseBond(bond_out)};
    BOOST_REQUIRE(bond);
    BOOST_CHECK(bond->pubkey == XOnly(validator) && bond->owner == owner_hash && bond->pop == pop && bond->amount == 5000);
    BOOST_CHECK(pos::IsBondScript(bond_out.scriptPubKey) && !pos::IsUnbondScript(bond_out.scriptPubKey));
    BOOST_CHECK(pos::CheckBondPop(XOnly(validator), owner_hash, pop, genesis));
    BOOST_CHECK(!pos::CheckBondPop(XOnly(validator), uint160{}, pop, genesis));
    BOOST_CHECK(!pos::CheckBondPop(XOnly(validator), owner_hash, pop, uint256{2}));
    BOOST_CHECK(!pos::CheckBondPop(XOnly(owner), owner_hash, pop, genesis));
    // A script with the tag claims to be a bond, read or not.
    const CScript truncated{CScript() << OP_RETURN << std::vector<unsigned char>{0xDB, 'S', 'T', 'K', 1, 2, 3}};
    BOOST_CHECK(pos::IsBondScript(truncated));
    BOOST_CHECK(!pos::ParseBond(CTxOut{1, truncated}));
    // An unbond reads back; an amount above MAX_MONEY does not read.
    pos::Unbond unbond;
    unbond.pubkey = XOnly(validator);
    unbond.amount = 4000;
    unbond.sequence = 7;
    BOOST_REQUIRE(owner.SignCompact(MessageHash(pos::UnbondMessage(unbond.pubkey, unbond.amount, unbond.sequence, genesis)), unbond.signature));
    const auto read{pos::ParseUnbond(CTxOut{0, pos::UnbondScript(unbond)})};
    BOOST_REQUIRE(read);
    BOOST_CHECK(read->pubkey == unbond.pubkey && read->amount == 4000 && read->sequence == 7 && read->signature == unbond.signature);
    BOOST_CHECK(pos::IsUnbondScript(pos::UnbondScript(unbond)));
    pos::Unbond huge{unbond};
    huge.amount = MAX_MONEY + 1;
    BOOST_CHECK(!pos::ParseUnbond(CTxOut{0, pos::UnbondScript(huge)}));
    // The message names the validator, the amount, the sequence and the chain.
    BOOST_CHECK(pos::UnbondMessage(unbond.pubkey, 4000, 7, genesis) != pos::UnbondMessage(unbond.pubkey, 4000, 8, genesis));
    BOOST_CHECK(pos::UnbondMessage(unbond.pubkey, 4000, 7, genesis) != pos::UnbondMessage(unbond.pubkey, 4000, 7, uint256{2}));
    // A bond with the owner's signature reads back; one with a signature of another size does not.
    const std::vector<unsigned char> owner_sig(CPubKey::COMPACT_SIGNATURE_SIZE, 3);
    const auto signed_bond{pos::ParseBond(CTxOut{5000, pos::BondScript(XOnly(validator), owner_hash, pop, owner_sig)})};
    BOOST_REQUIRE(signed_bond);
    BOOST_CHECK(signed_bond->owner_sig == owner_sig && signed_bond->pop == pop && bond->owner_sig.empty());
    BOOST_CHECK(!pos::ParseBond(CTxOut{5000, pos::BondScript(XOnly(validator), owner_hash, pop, std::vector<unsigned char>(64, 3))}));
    // The owner's message names the validator, the amount, the coin spent and the chain.
    const COutPoint coin{Txid::FromUint256(uint256{5}), 1};
    const std::string bond_message{pos::BondMessage(unbond.pubkey, 4000, coin, genesis)};
    BOOST_CHECK(bond_message != pos::BondMessage(unbond.pubkey, 4001, coin, genesis));
    BOOST_CHECK(bond_message != pos::BondMessage(unbond.pubkey, 4000, COutPoint{coin.hash, 2}, genesis));
    BOOST_CHECK(bond_message != pos::BondMessage(unbond.pubkey, 4000, coin, uint256{2}));
    BOOST_CHECK(bond_message != pos::BondMessage(XOnly(owner), 4000, coin, genesis));
    // An unbond of all says so, and reads back.
    BOOST_CHECK(pos::UnbondMessage(unbond.pubkey, pos::UNBOND_ALL, 7, genesis).find(" all ") != std::string::npos);
    pos::Unbond all{unbond};
    all.amount = pos::UNBOND_ALL;
    BOOST_CHECK_EQUAL(pos::ParseUnbond(CTxOut{0, pos::UnbondScript(all)})->amount, pos::UNBOND_ALL);
    // The penalty: 10 %, in integers, without overflow.
    Consensus::PosParams params;
    const pos::Staking staking{params, genesis};
    BOOST_CHECK_EQUAL(staking.Penalty(100000), 10000);
    BOOST_CHECK_EQUAL(staking.Penalty(9), 0);
    BOOST_CHECK_EQUAL(staking.Penalty(MAX_MONEY), MAX_MONEY / 10);
}

BOOST_AUTO_TEST_CASE(leader_seed)
{
    // The seed of an epoch: the hash of the highest mainchain block whose median time is at least
    // seed_lookahead_seconds before the epoch starts; known once a later block is on record.
    Consensus::PosParams params;
    params.genesis_time = 1000;
    params.slot_seconds = 30;
    params.slots_per_epoch = 120;
    params.seed_lookahead_seconds = 1200;
    sidechain::Mainchain record;
    BOOST_CHECK(!pos::SeedHeight(1, params, record));
    const auto append{[&](int height, uint8_t salt = 0) {
        sidechain::MainBlock block;
        block.hash = uint256{static_cast<uint8_t>(height + salt)};
        block.prev_hash = record.TipHash();
        block.time = 1000 + 60 * height;
        BOOST_REQUIRE(record.Append(block));
    }};
    // Epoch 1 starts at 1000 + 3600; its seed is taken at 3400: block 40.
    BOOST_CHECK_EQUAL(pos::SeedTime(1, params), 3400);
    for (int height{0}; height <= 40; ++height) append(height);
    BOOST_CHECK(!pos::SeedHeight(1, params, record));
    BOOST_CHECK(!pos::EpochSeed(1, params, record));
    append(41);
    BOOST_CHECK(pos::SeedHeight(1, params, record) == std::optional<int>{40});
    BOOST_CHECK(pos::EpochSeed(1, params, record) == pos::ValidatorSet::Seed(1, uint256{40}));
    // Before the first block on record: a null hash.
    BOOST_CHECK(pos::SeedHeight(0, params, record) == std::optional<int>{-1});
    BOOST_CHECK(pos::EpochSeed(0, params, record) == pos::ValidatorSet::Seed(0, uint256{}));
    // A mainchain reorg that replaces the block moves the seed.
    record.Truncate(39);
    append(40, 100);
    append(41, 100);
    BOOST_CHECK(pos::EpochSeed(1, params, record) == pos::ValidatorSet::Seed(1, uint256{140}));
    BOOST_CHECK(pos::ValidatorSet::Seed(1, uint256{40}) != pos::ValidatorSet::Seed(2, uint256{40}));
}

BOOST_FIXTURE_TEST_CASE(bonds, FastBlocksSetup)
{
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    CKey validator, stranger;
    validator.MakeNewKey(/*fCompressed=*/true);
    stranger.MakeNewKey(/*fCompressed=*/true);
    const uint256 pubkey{XOnly(validator)};
    const CAmount min{Pos().min_stake};
    // The mempool (and blocks) refuse a bond whose proof of possession another key signed.
    {
        const auto signed_by_stranger{*pos::ParseBond(BondOut(stranger, 2 * min, Owner(key)))};
        COutPoint c{coin};
        CTxOut s{spent};
        BOOST_CHECK_EQUAL(Refused(Pay(c, s, {CTxOut{2 * min, pos::BondScript(pubkey, Owner(key), signed_by_stranger.pop)}})), "bad-pos-bond-pop");
    }
    // A bond: the coins leave circulation into the state, the key's owner is the bond's.
    Accept(Pay(coin, spent, {BondOut(validator, 2 * min, Owner(key), &key, coin)}));
    const CBlock block{PosBlock(1)};
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 2U);
    Take(block);
    auto record{Validator(pubkey)};
    BOOST_REQUIRE(record);
    BOOST_CHECK(record->owner == Owner(key));
    BOOST_CHECK_EQUAL(record->bonded, 2 * min);
    BOOST_CHECK_EQUAL(record->first_bond_height, Tip()->nHeight);
    BOOST_CHECK(record->Eligible(Pos()));
    BOOST_CHECK_EQUAL(Stake().bonded, 2 * min);
    // More, by the same owner, who signs them (the amount and the coin the transaction spends first).
    Accept(Pay(coin, spent, {BondOut(validator, min, Owner(key), &key, coin)}));
    Take(PosBlock(2));
    BOOST_CHECK_EQUAL(Validator(pubkey)->bonded, 3 * min);
    BOOST_CHECK_EQUAL(Stake().bonded, 3 * min);
    // The rules.
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, min, Owner(stranger))})), "bad-pos-bond-owner");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, min, uint160{})})), "bad-pos-bond-owner");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, 0, Owner(key))})), "bad-pos-bond-amount");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, MAX_MONEY + 1, Owner(key))})), "bad-pos-bond-amount");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({CTxOut{min, CScript() << OP_RETURN << std::vector<unsigned char>{0xDB, 'S', 'T', 'K', 1}}})), "bad-pos-bond");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, min, Owner(key), &key)})), "");
    // Without the owner's signature, or with another's, or with one for another coin or amount: refused.
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, min, Owner(key))})), "bad-pos-bond-owner-sig");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, min, Owner(key), &stranger)})), "bad-pos-bond-owner-sig");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, min, Owner(key), &key, coin)})), "bad-pos-bond-owner-sig");
    {
        CTxOut other_amount{BondOut(validator, min, Owner(key), &key)};
        other_amount.nValue = min + 1;
        BOOST_CHECK_EQUAL(StakingRules(TxWith({other_amount})), "bad-pos-bond-owner-sig");
    }
    // A smaller bond is taken, but the key takes part only with min_stake.
    Accept(Pay(coin, spent, {BondOut(stranger, min - 1, Owner(key), &key, coin)}));
    Take(PosBlock(3));
    BOOST_CHECK_EQUAL(Validator(XOnly(stranger))->bonded, min - 1);
    BOOST_CHECK(!Validator(XOnly(stranger))->Eligible(Pos()));
    // The sets made at anchors: the next anchor's set has the bonded key, with its stake as weight
    // (and the genesis validators); used from the epoch after the next.
    MineBmm(BmmBlock(/*use_mempool=*/false));
    const int64_t epoch{pos::EpochOf(pos::SlotOf(Tip()->nTime, Pos()), Pos()) + 2};
    const auto set{WITH_LOCK(::cs_main, return Hooks().SetFor(*Tip(), epoch))};
    BOOST_REQUIRE(set);
    const auto index{set->Find(pubkey)};
    BOOST_REQUIRE(index);
    BOOST_CHECK_EQUAL(set->Validators()[*index].stake, 3 * min);
    BOOST_CHECK(!set->Find(XOnly(stranger)));
    BOOST_CHECK_EQUAL(set->Validators().size(), 4U);
    // The set of the epoch before is still the genesis set.
    const auto before{WITH_LOCK(::cs_main, return Hooks().SetFor(*Tip(), epoch - 1))};
    BOOST_REQUIRE(before);
    BOOST_CHECK(!before->Find(pubkey));
}

BOOST_FIXTURE_TEST_CASE(unbonds, FastBlocksSetup)
{
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    CKey validator, stranger;
    validator.MakeNewKey(/*fCompressed=*/true);
    stranger.MakeNewKey(/*fCompressed=*/true);
    const uint256 pubkey{XOnly(validator)};
    const CAmount min{Pos().min_stake};
    Accept(Pay(coin, spent, {BondOut(validator, 3 * min, Owner(key), &key, coin)}));
    Take(PosBlock(1));
    // The rules.
    BOOST_CHECK_EQUAL(StakingRules(TxWith({CTxOut{0, CScript() << OP_RETURN << std::vector<unsigned char>{0xDC, 'U', 'N', 'B', 1}}})), "bad-pos-unbond");
    CTxOut valued{UnbondOut(validator, min, 0, key)};
    valued.nValue = 1;
    BOOST_CHECK_EQUAL(StakingRules(TxWith({valued})), "bad-pos-unbond-value");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(stranger, min, 0, key)})), "bad-pos-unbond-unknown");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(validator, min, 1, key)})), "bad-pos-unbond-sequence");
    // (UNBOND_ALL, 0: all that is bonded.)
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(validator, pos::UNBOND_ALL, 0, key)})), "");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(validator, 3 * min + 1, 0, key)})), "bad-pos-unbond-amount");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(validator, 3 * min - min / 2, 0, key)})), "bad-pos-unbond-remainder");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(validator, min, 0, stranger)})), "bad-pos-unbond-sig");
    {
        COutPoint c{coin};
        CTxOut s{spent};
        BOOST_CHECK_EQUAL(Refused(Pay(c, s, {UnbondOut(validator, min, 0, stranger)})), "bad-pos-unbond-sig");
    }
    // An unbond: off the stake at once, paid unbonding_anchors anchors later to the owner's key.
    const uint64_t anchors{Fast().anchors};
    Accept(Pay(coin, spent, {UnbondOut(validator, min, 0, key)}));
    Take(PosBlock(2));
    auto record{*Validator(pubkey)};
    BOOST_CHECK_EQUAL(record.bonded, 2 * min);
    BOOST_CHECK_EQUAL(record.unbond_seq, 1U);
    BOOST_REQUIRE_EQUAL(record.unbonding.size(), 1U);
    BOOST_CHECK(record.unbonding[0] == (pos::UnbondingEntry{.release_anchor = anchors + Pos().unbonding_anchors, .sequence = 0, .amount = min}));
    BOOST_CHECK_EQUAL(Stake().bonded, 2 * min);
    BOOST_CHECK_EQUAL(Stake().unbonding, min);
    // Not twice.
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(validator, min, 0, key)})), "bad-pos-unbond-sequence");
    // All that is left: the key no longer takes part.
    Accept(Pay(coin, spent, {UnbondOut(validator, 2 * min, 1, key)}));
    Take(PosBlock(3));
    BOOST_CHECK(!Validator(pubkey)->Eligible(Pos()));
    // Nothing bonded: nothing to unbond, all of it or not.
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(validator, pos::UNBOND_ALL, 2, key)})), "bad-pos-unbond-amount");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(validator, 1, 2, key)})), "bad-pos-unbond-amount");
    BOOST_CHECK_EQUAL(Stake().unbonding, 3 * min);
    // Paid at the anchor it is due, through the payouts of the peg actions: not before.
    const CTxOut first{min, script}, second{2 * min, script};
    while (Fast().anchors + 1 < anchors + Pos().unbonding_anchors) {
        const CBlock bmm{BmmBlock(/*use_mempool=*/false)};
        BOOST_CHECK(!Pays(bmm, first) && !Pays(bmm, second));
        MineBmm(bmm);
    }
    const CBlock paying{BmmBlock(/*use_mempool=*/false)};
    BOOST_CHECK(Pays(paying, first));
    BOOST_CHECK(Pays(paying, second));
    MineBmm(paying);
    BOOST_CHECK_EQUAL(Stake().unbonding, 0);
    BOOST_CHECK_EQUAL(Stake().bonded, 0);
    BOOST_CHECK(Validator(pubkey)->unbonding.empty());
}

BOOST_FIXTURE_TEST_CASE(staking_starts_pos, NoValidatorsSetup)
{
    // The main and test networks start so: no validators, merged mined blocks only (with
    // transactions), until enough stake is bonded. Then blocks of stake from the epoch after next.
    BOOST_CHECK(!Hooks().PosActiveNow());
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    const CKey& validator{keys[0]};
    Accept(Pay(coin, spent, {BondOut(validator, Pos().min_stake, Owner(key), &key, coin)}));
    const CBlock bonding{BmmBlock()};
    BOOST_REQUIRE_EQUAL(bonding.vtx.size(), 2U);
    MineBmm(bonding);
    // The anchor after it makes a set with it.
    MineBmm(BmmBlock(/*use_mempool=*/false));
    const int64_t epoch{pos::EpochOf(pos::SlotOf(Tip()->nTime, Pos()), Pos()) + 2};
    std::string error;
    {
        const int64_t slot{(epoch - 1) * Pos().slots_per_epoch};
        Mock(pos::SlotTime(slot, Pos()));
        BOOST_CHECK(!Hooks().PosActiveNow());
        BOOST_CHECK(!producer->Produce(slot, error));
        BOOST_CHECK_EQUAL(error, "no blocks of stake in this epoch: too few validators");
    }
    const int64_t slot{epoch * Pos().slots_per_epoch};
    Mock(pos::SlotTime(slot, Pos()));
    BOOST_CHECK(Hooks().PosActiveNow());
    const auto made{producer->Produce(slot, error)};
    BOOST_REQUIRE_MESSAGE(made, error);
    BOOST_CHECK(Tip()->GetBlockHash() == *made);
    BOOST_CHECK(pos::CheckBlockSig(Tip()->GetBlockHeader(), XOnly(validator), Genesis()));
    // Off again once the stake leaves: the set an anchor makes then is empty.
    Accept(Pay(coin, spent, {UnbondOut(validator, Pos().min_stake, 0, key)}));
    Mock(pos::SlotTime(slot + 1, Pos()));
    BOOST_REQUIRE_MESSAGE(producer->Produce(slot + 1, error), error);
    BOOST_CHECK_EQUAL(Validator(XOnly(validator))->bonded, 0);
    MineBmm(BmmBlock(/*use_mempool=*/false));
    const int64_t off{pos::EpochOf(pos::SlotOf(Tip()->nTime, Pos()), Pos()) + 2};
    Mock(pos::SlotTime(off * Pos().slots_per_epoch, Pos()));
    BOOST_CHECK(!Hooks().PosActiveNow());
    BOOST_CHECK(!producer->Produce(off * Pos().slots_per_epoch, error));
    BOOST_CHECK_EQUAL(error, "no blocks of stake in this epoch: too few validators");
}

BOOST_AUTO_TEST_CASE(jailing)
{
    // A validator that made less than half the blocks it led in an epoch (and led at least ten) is
    // left out of the sets made in the next epochs, jail_epochs more; it keeps its coins.
    ECC_Context ecc;
    std::vector<CKey> validators(3);
    for (CKey& k : validators) k.MakeNewKey(/*fCompressed=*/true);
    Consensus::PosParams params;
    params.slots_per_epoch = 20;
    params.min_active_validators = 1;
    params.max_validators = 8;
    params.min_stake = 1000;
    const uint256 genesis{1};
    CKey owner_key;
    owner_key.MakeNewKey(/*fCompressed=*/true);
    const uint160 owner{owner_key.GetPubKey().GetID()};
    const pos::Staking staking{params, genesis};
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay out{empty};
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{1}), 0});
    for (const CKey& k : validators) {
        std::array<unsigned char, 64> pop;
        BOOST_REQUIRE(k.SignSchnorr(pos::BondPopHash(owner, genesis), pop, nullptr, uint256{}));
        std::vector<unsigned char> owner_sig;
        BOOST_REQUIRE(owner_key.SignCompact(MessageHash(pos::BondMessage(XOnly(k), 5000, tx.vin[0].prevout, genesis)), owner_sig));
        tx.vout.emplace_back(5000, pos::BondScript(XOnly(k), owner, pop, owner_sig));
    }
    std::string reason;
    BOOST_REQUIRE(staking.ApplyTx(out, CTransaction{tx}, 1, 0, reason));
    const uint256 a{XOnly(validators[0])}, b{XOnly(validators[1])}, c{XOnly(validators[2])};
    // Epoch 0: A made its ten; B led twelve and made none; C led four and made none (too few to tell).
    for (int64_t slot{0}; slot < 10; ++slot) staking.NoteLiveness(out, slot, a, {});
    std::vector<std::pair<int64_t, uint256>> missed;
    for (int64_t slot{10}; slot < 18; ++slot) missed.emplace_back(slot, b);
    for (int64_t slot{0}; slot < 4; ++slot) missed.emplace_back(slot, b);
    for (int64_t slot{0}; slot < 4; ++slot) missed.emplace_back(slot, c);
    staking.NoteLiveness(out, 19, a, missed);
    BOOST_CHECK(pos::GetLiveness(out, 0, a) == (pos::Liveness{.assigned = 11, .produced = 11}));
    BOOST_CHECK(pos::GetLiveness(out, 0, b) == (pos::Liveness{.assigned = 12, .produced = 0}));
    // The first anchor of epoch 1 jails B; its set leaves B out.
    std::vector<CTxOut> owed;
    const auto made{staking.PegStep(out, uint256{0xa1}, 1, 20, 1, owed)};
    BOOST_CHECK(made.set_active);
    BOOST_CHECK(owed.empty());
    BOOST_CHECK_EQUAL(pos::GetValidator(out, b)->jailed_until_epoch, 3);
    BOOST_CHECK_EQUAL(pos::GetValidator(out, a)->jailed_until_epoch, 0);
    BOOST_CHECK_EQUAL(pos::GetValidator(out, c)->jailed_until_epoch, 0);
    BOOST_CHECK(!pos::GetLiveness(out, 0, b));
    const auto set{pos::GetSetVersion(out, pos::GetAnchorSet(out, uint256{0xa1})->version)};
    BOOST_REQUIRE(set);
    BOOST_CHECK_EQUAL(set->size(), 2U);
    BOOST_CHECK(!pos::ValidatorSet(*set, params).Find(b));
    // No coins lost.
    BOOST_CHECK_EQUAL(pos::GetValidator(out, b)->bonded, 5000);
    BOOST_CHECK_EQUAL(pos::GetStakeState(out).bonded, 15000);
    // Out of the sets made in epoch 2 too; back in those of epoch 3.
    BOOST_CHECK(!staking.MakeSet(out, 2).Find(b));
    BOOST_CHECK(staking.MakeSet(out, 3).Find(b));
    staking.PegStep(out, uint256{0xa2}, 2, 60, 3, owed);
    BOOST_CHECK(pos::ValidatorSet(*pos::GetSetVersion(out, pos::GetAnchorSet(out, uint256{0xa2})->version), params).Find(b));
}

BOOST_FIXTURE_TEST_CASE(liveness_noted, FastBlocksSetup)
{
    // Each block of stake: its leader made one; the leaders of the slots since the last one (an epoch
    // of them at most) made none.
    StartPos();
    Take(PosBlock(1));
    Take(PosBlock(4));
    const auto epoch{pos::EpochOf(base_slot, Pos())};
    uint32_t assigned{0}, produced{0};
    LOCK(::cs_main);
    for (int64_t e{epoch - 1}; e <= epoch + 1; ++e) {
        for (const CKey& k : keys) {
            const auto liveness{pos::GetLiveness(Chain().SideState().View(), e, XOnly(k))};
            if (!liveness) continue;
            assigned += liveness->assigned;
            produced += liveness->produced;
        }
    }
    // The first block of stake: itself (the slots up to the BMM block it follows are nobody's to
    // fill); the second: two slots and itself.
    BOOST_CHECK_EQUAL(assigned, static_cast<uint32_t>(1 + 2 + 1));
    BOOST_CHECK_EQUAL(produced, 2U);
}

BOOST_FIXTURE_TEST_CASE(slashing, FastBlocksSetup)
{
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    const CAmount bond{100000};
    // Every validator bonds.
    Accept(Pay(coin, spent, {BondOut(keys[0], bond, Owner(key), &key, coin), BondOut(keys[1], bond, Owner(key), &key, coin), BondOut(keys[2], bond, Owner(key), &key, coin)}));
    const CBlock bonding{PosBlock(1)};
    Take(bonding);
    BOOST_CHECK_EQUAL(Stake().bonded, 3 * bond);
    // The leader of slot 2 signs two blocks for it; the first is taken.
    const auto [first, second]{TwoBlocks(2)};
    const pos::DoubleProposal proof{.a = first, .b = second};
    const uint256 offender{XOnly(Leader(base_slot + 2))};
    const CKey& other{NotLeader(base_slot + 2)};
    Take(first);

    // Proofs the chain refuses (asked of the rules directly: blocks of one slot given to validation
    // would be proofs that its leader signed them all). One of a slot after the block's.
    const sidechain::StoreView& view{ActiveView()};
    {
        const auto [x, y]{TwoBlocks(4)};
        BOOST_CHECK_EQUAL(After(WithEvidence(3, {{.a = x, .b = y}}), view), "bad-pos-evidence-late");
    }
    // Not signed by the slot's leader.
    {
        CBlock a{first}, b{second};
        Resign(a, &other);
        Resign(b, &other);
        BOOST_CHECK_EQUAL(After(WithEvidence(3, {{.a = a, .b = b}}), view), "bad-pos-evidence-sig");
    }
    // Not of the form of one (context free, CheckBlock): two blocks of stake of one slot, different.
    const auto form{[&](const CBlock& block) {
        BlockValidationState state;
        return Hooks().CheckBlock(block, state) ? std::string{} : state.GetRejectReason();
    }};
    BOOST_CHECK_EQUAL(form(WithEvidence(3, {{.a = first, .b = first}})), "bad-pos-evidence-same");
    BOOST_CHECK_EQUAL(form(WithEvidence(3, {{.a = first, .b = bonding}})), "bad-pos-evidence-slot");
    BOOST_CHECK_EQUAL(form(WithEvidence(3, {{.a = first, .b = BmmBlock(/*use_mempool=*/false)}})), "bad-pos-evidence-kind");
    // At most MAX_SECTION_EVIDENCE in a block.
    BOOST_CHECK_EQUAL(form(WithEvidence(3, std::vector<pos::DoubleProposal>(pos::MAX_SECTION_EVIDENCE + 1, proof))), "bad-pos-sections");
    BOOST_CHECK_EQUAL(form(WithEvidence(3, std::vector<pos::DoubleProposal>(pos::MAX_SECTION_EVIDENCE, proof))), "");
    // Two in a block for one offence.
    BOOST_CHECK_EQUAL(After(WithEvidence(3, {proof, proof}), view), "bad-pos-evidence-known");
    BOOST_CHECK_EQUAL(After(WithEvidence(3, {proof}), view), "");

    // The proof, in the block of slot 3 (as the producer puts the proofs it knows in): the leader loses
    // 10 % of its stake and is banned, the rest is unbonded; the block is owed 1/8 of the penalty.
    std::string reason;
    BOOST_REQUIRE_MESSAGE(Hooks().AddEvidence(proof, reason), reason);
    const int64_t slot3{Slot(3)};
    std::string error;
    // (The producer's key for slot 3 is one of the fixture's.)
    BOOST_REQUIRE_MESSAGE(producer->Produce(slot3, error), error);
    CBlock carrying;
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Chainman().m_blockman.ReadBlock(carrying, *Tip())));
    pos::PosSections sections;
    BOOST_REQUIRE(pos::PosSections::Decode(carrying.vSections, sections));
    BOOST_REQUIRE_EQUAL(sections.evidence.size(), 1U);
    const uint256 carrying_hash{carrying.GetHash()};
    const CAmount penalty{bond / 10};
    const auto record{Validator(offender)};
    BOOST_REQUIRE(record);
    BOOST_CHECK(record->tombstoned);
    BOOST_CHECK_EQUAL(record->bonded, 0);
    BOOST_CHECK_EQUAL(record->Unbonding(), bond - penalty);
    BOOST_CHECK(!record->Eligible(Pos()));
    BOOST_CHECK_EQUAL(Stake().bonded, 2 * bond);
    BOOST_CHECK_EQUAL(Stake().unbonding, bond - penalty);
    BOOST_CHECK_EQUAL(Stake().rewards_owed, penalty / 8);
    BOOST_CHECK_EQUAL(Stake().pot_reserve, penalty - penalty / 8);
    BOOST_CHECK_EQUAL(Stake().slashed, penalty);
    // Every coin still accounted for.
    BOOST_CHECK_EQUAL(Stake().bonded + Stake().unbonding + Stake().pot_reserve + Stake().rewards_owed, 3 * bond);
    // Not twice; and no more bonds of its key.
    BOOST_CHECK_EQUAL(After(WithEvidence(4, {proof}), view), "bad-pos-evidence-known");
    for (const CKey& k : keys) {
        if (XOnly(k) == offender) BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(k, bond, Owner(key))})), "bad-pos-bond-tombstoned");
    }
    // The producer does not put it in again.
    BOOST_REQUIRE_MESSAGE(producer->Produce(Slot(4), error), error);
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Chainman().m_blockman.ReadBlock(carrying, *Tip())));
    BOOST_REQUIRE(pos::PosSections::Decode(carrying.vSections, sections));
    BOOST_CHECK(sections.evidence.empty());

    // At the next peg step: the reward, to the script of the first output of the block that carried
    // the proof; a share of the reserve, with the pot, to the anchor's payee.
    const CScript reporter{GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey()})};
    const CBlock bmm{BmmBlock(/*use_mempool=*/false)};
    BOOST_CHECK(Pays(bmm, CTxOut{penalty / 8, reporter}));
    const CAmount due{Fast().pot_accrued - Fast().pot_paid};
    const CAmount reserve{penalty - penalty / 8};
    const CAmount release{reserve / Pos().pot_reserve_anchors + (reserve % Pos().pot_reserve_anchors != 0)};
    BOOST_CHECK(Pays(bmm, CTxOut{due + release, CScript() << OP_TRUE}));
    MineBmm(bmm);
    BOOST_CHECK_EQUAL(Stake().rewards_owed, 0);
    // What a wallet shows of them (not consensus): the reward to the reporter, the pot to the payee;
    // the first output of the block of stake that carried the proof, its producer's share.
    {
        using Role = interfaces::Chain::CoinbaseRole;
        const auto chain{interfaces::MakeChain(m_node)};
        const auto roles{chain->getCoinbaseRoles(bmm.GetHash())};
        const auto& vout{bmm.vtx[0]->vout};
        BOOST_REQUIRE_EQUAL(roles.size(), vout.size());
        size_t rewards{0}, pots{0};
        for (size_t i{0}; i < vout.size(); ++i) {
            const Role expected{i > 0 && vout[i] == CTxOut{penalty / 8, reporter} ? Role::REPORTER :
                                i > 0 && vout[i] == CTxOut{due + release, CScript() << OP_TRUE} ? Role::ANCHOR : Role::OTHER};
            BOOST_CHECK(roles[i] == expected);
            rewards += roles[i] == Role::REPORTER;
            pots += roles[i] == Role::ANCHOR;
        }
        BOOST_CHECK_EQUAL(rewards, 1U);
        BOOST_CHECK_EQUAL(pots, 1U);
        const auto carried{chain->getCoinbaseRoles(carrying_hash)};
        BOOST_REQUIRE(!carried.empty());
        BOOST_CHECK(carried[0] == Role::STAKED);
        BOOST_CHECK(chain->getCoinbaseRoles(uint256::ONE).empty());
    }
    BOOST_CHECK_EQUAL(Stake().pot_reserve, reserve - release);
    // The sets made from then on leave the offender out.
    const auto set{WITH_LOCK(::cs_main, return Hooks().SetFor(*Tip(), pos::EpochOf(pos::SlotOf(Tip()->nTime, Pos()), Pos()) + 2))};
    BOOST_REQUIRE(set);
    BOOST_CHECK(!set->Find(offender));
    BOOST_CHECK_EQUAL(set->Validators().size(), 2U);

    // A proof of a slot whose set was made more than evidence_window_anchors anchors before: too old.
    const auto [late_a, late_b]{TwoBlocks(5)};
    Take(late_a);
    for (int64_t i{0}; i <= Pos().evidence_window_anchors; ++i) MineBmm(BmmBlock(/*use_mempool=*/false));
    BOOST_CHECK_EQUAL(Check(WithEvidence(60, {{.a = late_a, .b = late_b}})), "bad-pos-evidence-old");
}

BOOST_AUTO_TEST_CASE(coinbase_roles)
{
    // A merged mined block that pays, after its first output (the payee's): a deposit to the payee,
    // the pot to the payee, an unbond, a reward to the payee and one to another reporter, a refund.
    const CScript payee{CScript() << OP_TRUE};
    const CScript other{CScript() << OP_2};
    const CScript owner{CScript() << OP_3};
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vout = {CTxOut{0, payee}, CTxOut{5, payee}, CTxOut{7, payee}, CTxOut{3, owner}, CTxOut{1, payee}, CTxOut{2, other}, CTxOut{4, owner},
                     CTxOut{0, CScript() << OP_RETURN}};
    CBlock bmm;
    bmm.vtx.push_back(MakeTransactionRef(coinbase));
    BOOST_CHECK(pos::IsAnchorBlock(bmm));
    BOOST_CHECK(pos::AnchorPayee(bmm) == payee);
    using Role = pos::CoinbaseRole;
    BOOST_CHECK(pos::CoinbaseRoles(bmm, {payee, other}) == (std::vector<Role>{Role::OTHER, Role::OTHER, Role::ANCHOR, Role::OTHER, Role::REPORTER, Role::REPORTER, Role::OTHER, Role::OTHER}));
    // No rewards owed: the last payout to the payee is the pot.
    BOOST_CHECK(pos::CoinbaseRoles(bmm, {}) == (std::vector<Role>{Role::OTHER, Role::OTHER, Role::OTHER, Role::OTHER, Role::ANCHOR, Role::OTHER, Role::OTHER, Role::OTHER}));
    // Proofs in its coinbase: one reward each, to its first output's script.
    CMutableTransaction with_proof{coinbase};
    CBlockHeader a, b;
    a.nTime = 1;
    b.nTime = 2;
    with_proof.vout.emplace_back(0, pos::EvidenceScript({.a = a, .b = b}));
    CBlock carrying;
    carrying.vtx.push_back(MakeTransactionRef(with_proof));
    BOOST_CHECK(pos::ReporterScripts(carrying, std::nullopt, uint256{}) == std::vector<CScript>{payee});
    BOOST_CHECK(pos::ReporterScripts(bmm, std::nullopt, uint256{}).empty());
}

BOOST_FIXTURE_TEST_CASE(leader_checked_when_connected, FastBlocksSetup)
{
    StartPos();
    // A block whose epoch's set this state does not have (the header was taken before the node knew
    // it): refused when connected. (The set of the anchor of an epoch two on.) Even once this node has
    // that set in memory: the rules of a block read its chain's state alone, so that a set the node
    // read before (one the state pruned since, or an earlier record made) does not decide.
    {
        const int64_t slot{Slot(2 * static_cast<int>(Pos().slots_per_epoch) + 5)};
        std::string error;
        const auto block{producer->Build(slot, std::nullopt, error)};
        BOOST_REQUIRE_MESSAGE(block, error);
        sidechain::EmptyStore empty;
        BOOST_CHECK_EQUAL(After(*block, empty), "bad-pos-set-unknown");
        BOOST_REQUIRE(WITH_LOCK(::cs_main, return Hooks().SetFor(*Tip(), pos::EpochOf(slot, Pos()))));
        BOOST_CHECK_EQUAL(After(*block, empty), "bad-pos-set-unknown");
    }
    // Signed by another key than the leader's: refused when connected, whatever the header check said.
    CBlock block{PosBlock(1)};
    const sidechain::StoreView& view{ActiveView()};
    BOOST_CHECK_EQUAL(After(block, view), "");
    Resign(block, &NotLeader(pos::SlotOf(block.nTime, Pos())));
    BOOST_CHECK_EQUAL(After(block, view), "bad-pos-sig");
}

BOOST_FIXTURE_TEST_CASE(inactive_when_connected, NoValidatorsSetup)
{
    // No validators: a block of stake is refused when connected, too.
    StartPos();
    const int64_t slot{Slot(1)};
    std::string error;
    const auto block{producer->Build(slot, std::nullopt, error)};
    BOOST_REQUIRE_MESSAGE(block, error);
    const sidechain::StoreView& view{ActiveView()};
    BOOST_CHECK_EQUAL(After(*block, view), "bad-pos-inactive");
}

BOOST_FIXTURE_TEST_CASE(seed_from_the_mainchain, SeedSetup)
{
    // The seed of an epoch is the hash of the highest mainchain block whose median time is at or
    // before the epoch's start (no lookahead here): a block of stake of an epoch whose seed the record
    // does not have yet waits.
    StartPos();
    BOOST_CHECK(Hooks().IsWaitReason("pos-seed-unknown"));
    // The first slot of the next epoch.
    const int64_t epoch{pos::EpochOf(base_slot, Pos()) + 1};
    const int i{static_cast<int>(epoch * Pos().slots_per_epoch - base_slot)};
    const int64_t slot{Slot(i)};
    // Its leader will be that of the seed of the block on record now.
    const uint256 seed{pos::ValidatorSet::Seed(epoch, Record().TipHash())};
    const auto leader{Hooks().GenesisSet().Leader(slot, seed)};
    BOOST_REQUIRE(leader);
    std::string error;
    BOOST_CHECK(!producer->Produce(slot, error));
    BOOST_CHECK_EQUAL(error, "the mainchain block that seeds the epoch is not on record yet");
    auto block{producer->Build(slot, std::nullopt, error)};
    BOOST_REQUIRE_MESSAGE(block, error);
    for (const CKey& k : keys) {
        if (XOnly(k) == Hooks().GenesisSet().Validators()[*leader].pubkey) BOOST_REQUIRE(pos::Producer::Sign(*block, k, Genesis()));
    }
    BOOST_CHECK_EQUAL(Check(*block), "pos-seed-unknown");
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Hooks().WaitingReady(*block, Record())));
    const sidechain::StoreView& view{ActiveView()};
    BOOST_CHECK_EQUAL(After(*block, view), "pos-seed-unknown");
    // A mainchain block after the epoch's start: the seed is known, the block taken.
    Mock(GetTime() + 1);
    Main();
    BOOST_CHECK(WITH_LOCK(::cs_main, return Hooks().WaitingReady(*block, Record())));
    Take(*block);
    // A mainchain reorg that replaces the block that seeds the epoch moves its leaders: the blocks of
    // stake of the epoch whose leader changed are taken back, and given back when the record is.
    std::vector<uint256> blocks{block->GetHash()};
    for (int n{1}; n <= 6; ++n) {
        Take(PosBlock(i + n));
        blocks.push_back(Tip()->GetBlockHash());
    }
    const uint256 tip{Tip()->GetBlockHash()};
    Hooks().CheckSeeds();
    BOOST_CHECK(Tip()->GetBlockHash() == tip);
    const int seed_height{Record().Height() - 1};
    const auto replaced{Record().Truncate(seed_height - 1)};
    for (sidechain::MainBlock main : replaced) {
        main.prev_hash = Record().TipHash();
        main.hash = ArithToUint256(UintToArith256(main.hash) + 1);
        BOOST_REQUIRE(Record().Append(main));
    }
    Hooks().CheckSeeds();
    // (Whether a block's leader changed is up to the new seed: with seven blocks, some did.)
    const auto changed{[&](const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        const CBlockIndex* index{Chainman().m_blockman.LookupBlockIndex(hash)};
        const auto now{Hooks().LeaderOf(*index->pprev, pos::SlotOf(index->nTime, Pos()))};
        return now.status != pos::FastBlocksHooks::LeaderStatus::OK || !pos::CheckBlockSig(index->GetBlockHeader(), now.pubkey, Genesis());
    }};
    {
        LOCK(::cs_main);
        const auto first_changed{std::find_if(blocks.begin(), blocks.end(), changed)};
        if (first_changed != blocks.end()) {
            BOOST_CHECK(Chainman().m_blockman.LookupBlockIndex(*first_changed)->nStatus & BLOCK_FAILED_VALID);
            BOOST_CHECK(!Chain().m_chain.Contains(*Chainman().m_blockman.LookupBlockIndex(*first_changed)));
        } else {
            BOOST_CHECK(Tip()->GetBlockHash() == tip);
        }
    }
    // The record as it was: the blocks are given back.
    Record().Truncate(seed_height - 1);
    for (const sidechain::MainBlock& main : replaced) BOOST_REQUIRE(Record().Append(main));
    Hooks().CheckSeeds();
    BOOST_CHECK(Tip()->GetBlockHash() == tip);
}

BOOST_FIXTURE_TEST_CASE(record_failures_pardoned, FastBlocksSetup)
{
    // A block that failed against the record (its anchor's commitment lost, say), and the blocks on
    // it: a peer that sends their headers is not punished (its record may be ahead), unlike for a
    // block the operator marked invalid.
    BOOST_CHECK(Hooks().PardonRecordFailures());
    StartPos();
    const CBlock first{PosBlock(1)};
    Take(first);
    const CBlock second{PosBlock(2)};
    Take(second);
    CBlockIndex* failed{WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(first.GetHash()))};
    BlockValidationState invalidated;
    BOOST_REQUIRE(Chain().InvalidateBlock(invalidated, failed, /*by_record=*/true));
    const auto result{[&](const CBlockHeader& header) {
        BlockValidationState state;
        Chainman().ProcessNewBlockHeaders({{header}}, /*min_pow_checked=*/true, state);
        return std::make_pair(state.GetResult(), state.GetRejectReason());
    }};
    BOOST_CHECK(result(second) == std::make_pair(BlockValidationResult::BLOCK_TIME_FUTURE, std::string{"record-failed"}));
    BOOST_CHECK(result(first) == std::make_pair(BlockValidationResult::BLOCK_TIME_FUTURE, std::string{"record-failed"}));
    BOOST_REQUIRE(Chain().InvalidateBlock(invalidated, failed, /*by_record=*/false));
    BOOST_CHECK(result(second).first == BlockValidationResult::BLOCK_CACHED_INVALID);
}

BOOST_FIXTURE_TEST_CASE(block_failing_against_the_record_pardoned, FastBlocksSetup)
{
    // A block that fails against the record when it is connected (here: its acknowledgement names
    // the height of another commitment, as after a mainchain reorg moved it) is no fault of the peer
    // that sent it, whose record may say otherwise: not BLOCK_CONSENSUS, which a peer is punished
    // for. (After a mainchain reorg, the nodes that connected such a block disconnected the
    // validator that had relayed it: the soak test.) A block that breaks a rule whatever the record
    // says still is.
    StartPos();
    Take(PosBlock(1));
    Take(PosBlock(2));
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->pprev->GetBlockHash(), script))};
    Commit(uint256{7});
    pos::AnchorAck moved{ack};
    moved.main_height = Record().Height();
    const CBlock block{WithAck(PosBlock(3), moved)};
    const auto result{[&](const CBlock& b) {
        LOCK(::cs_main);
        const BlockValidationState state{TestBlockValidity(Chain(), b, /*check_pow=*/false, /*check_merkle_root=*/true)};
        return std::make_pair(state.GetResult(), state.GetRejectReason());
    }};
    BOOST_CHECK(result(block) == std::make_pair(BlockValidationResult::BLOCK_TIME_FUTURE, std::string{"bad-pos-ack-height"}));
    // Its producer keeping more than its share: whatever the record says.
    CBlock greedy{PosBlock(3)};
    CMutableTransaction coinbase{*greedy.vtx[0]};
    coinbase.vout[0].nValue += 1;
    greedy.vtx[0] = MakeTransactionRef(std::move(coinbase));
    Resign(greedy, nullptr);
    BOOST_CHECK(result(greedy).first == BlockValidationResult::BLOCK_CONSENSUS);
}

BOOST_FIXTURE_TEST_CASE(no_second_ack_while_one_waits, FastBlocksSetup)
{
    // The acknowledgement of a commitment the record lost and has again waits for the follower to
    // give its block back: a leader does not acknowledge the commitment again on another branch.
    // One the operator marked invalid, it may.
    StartPos();
    Take(PosBlock(1));
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    const CBlockIndex* parent{Tip()};
    const CBlock acking{PosBlock(2, ack)};
    Take(acking);
    CBlockIndex* index{WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(acking.GetHash()))};
    const auto next_ack{[&](int i) {
        const int64_t slot{Slot(i)};
        LOCK(::cs_main);
        return Hooks().NextAck(*Tip(), pos::GetFastState(Chain().SideState().View()), slot);
    }};
    BlockValidationState state;
    BOOST_REQUIRE(Chain().InvalidateBlock(state, index, /*by_record=*/true));
    BOOST_REQUIRE(Tip() == parent);
    BOOST_CHECK(!next_ack(3));
    BOOST_REQUIRE(Chain().InvalidateBlock(state, index, /*by_record=*/false));
    const auto again{next_ack(3)};
    BOOST_REQUIRE(again);
    BOOST_CHECK(*again == ack);
}

BOOST_FIXTURE_TEST_CASE(one_signature_per_slot, FastBlocksSetup)
{
    // A block the node finds invalid once signed (here: one with an acknowledgement, refused by a
    // check of the test's) is not made again without what made it invalid: a second block of the slot
    // would be a second signature, which the node, having checked the first header, would take for
    // proof against its own key, relay, and have the chain punish. The slot is lost; the
    // acknowledgement is left out of the blocks to come.
    StartPos();
    Take(PosBlock(1));
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    producer->SetExtraCheckForTesting([](const CBlock& block) -> std::optional<std::string> {
        if (!block.vSections.empty()) return "refused by the test";
        return std::nullopt;
    });
    const uint256 tip{Tip()->GetBlockHash()};
    std::string error;
    BOOST_CHECK(!producer->Produce(Slot(2), error));
    BOOST_CHECK(error.find("refused by the test") != std::string::npos);
    BOOST_CHECK(Tip()->GetBlockHash() == tip);
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 0U);
    BOOST_CHECK(Hooks().Evidence().TakeUnrelayed().empty());
    BOOST_CHECK(producer->RefusedTickets() == std::set<uint256>{ack.ticket.Hash()});
    // The next slot: a block without the acknowledgement; still no proof against the producer.
    const auto made{producer->Produce(Slot(3), error)};
    BOOST_REQUIRE_MESSAGE(made, error);
    BOOST_CHECK(Tip()->GetBlockHash() == *made);
    BOOST_CHECK_EQUAL(Tip()->nNonce, 0U);
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 0U);
    // Proofs that made a block invalid are left out too.
    producer->SetExtraCheckForTesting({});
    const auto [first, second]{TwoBlocks(4)};
    Take(first);
    std::string reason;
    const auto offence{Hooks().AddEvidence({.a = first, .b = second}, reason)};
    BOOST_REQUIRE_MESSAGE(offence, reason);
    producer->SetExtraCheckForTesting([](const CBlock& block) -> std::optional<std::string> {
        pos::PosSections sections;
        if (pos::PosSections::Decode(block.vSections, sections) && !sections.evidence.empty()) return "refused by the test";
        return std::nullopt;
    });
    BOOST_CHECK(!producer->Produce(Slot(5), error));
    BOOST_CHECK(producer->RefusedOffences() == std::set<pos::Offence>{*offence});
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 1U);
    const auto next{producer->Produce(Slot(6), error)};
    BOOST_REQUIRE_MESSAGE(next, error);
    CBlock block;
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Chainman().m_blockman.ReadBlock(block, *Tip())));
    BOOST_CHECK(block.vSections.empty());
}

BOOST_FIXTURE_TEST_CASE(no_jail_for_slots_nobody_could_fill, FastBlocksSetup)
{
    // The leaders of slots at which no block of stake could be made are not charged for them: slots
    // max_unanchored_slots or more after the first block no anchor covered (blocks of stake stopped),
    // and slots up to a BMM block the block follows (which ends such a stop, or a stall).
    const auto total{[&]() {
        uint32_t assigned{0};
        LOCK(::cs_main);
        for (int64_t e{pos::EpochOf(base_slot, Pos()) - 1}; e <= pos::EpochOf(base_slot, Pos()) + 20; ++e) {
            for (const CKey& k : keys) {
                if (const auto liveness{pos::GetLiveness(Chain().SideState().View(), e, XOnly(k))}) assigned += liveness->assigned;
            }
        }
        return assigned;
    }};
    StartPos();
    Take(PosBlock(1));
    BOOST_CHECK_EQUAL(total(), 1U);
    // Blocks of stake stop at slot 1 + max_unanchored_slots; an acknowledgement 10 slots later: an
    // epoch of slots back, but only those before the stop are charged.
    const int stop{1 + static_cast<int>(Pos().max_unanchored_slots)};
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    const CBlock late{PosBlock(stop + 10, ack)};
    BOOST_REQUIRE_EQUAL(Check(late), "");
    Take(late);
    // (The anchor's peg actions took the liveness of the epochs before its own into account, and
    // dropped it, before the block noted its own.) Ten slots and the block, not an epoch of slots.
    BOOST_CHECK_EQUAL(total(), 10U + 1);
    // A BMM block, then a block of stake: the slots up to the BMM block are not charged.
    const int bmm_at{stop + 40};
    Slot(bmm_at);
    MineBmm(BmmBlock(/*use_mempool=*/false));
    const uint32_t after_bmm{total()};
    Take(PosBlock(bmm_at + 5));
    BOOST_CHECK_EQUAL(total(), after_bmm + 4 + 1);
}

BOOST_FIXTURE_TEST_CASE(seed_moved_state_connected_again, SeedSetup)
{
    // A mainchain reorg that moves the seed of an epoch changes more than the leaders of its blocks:
    // the leaders charged for the slots the blocks skipped (and the sets anchors make after). The
    // blocks of stake of the epoch are connected again, also those whose own leader did not change, so
    // that the state is the one the record gives now, as a node that takes the chain in afresh has it.
    StartPos();
    const int64_t spe{Pos().slots_per_epoch};
    const int64_t e1{pos::EpochOf(base_slot, Pos()) + 1};
    const int64_t e2{e1 + 1};
    const auto at{[&](int64_t slot) { return static_cast<int>(slot - base_slot); }};
    // Epoch e1 (the genesis set): its seed is the block of the BMM block's commitment.
    Slot(at(e1 * spe) + 1);
    Main();
    Take(PosBlock(at(e1 * spe) + 2));
    Take(PosBlock(at(e1 * spe) + 3));
    // Epoch e2 (the set of the BMM block, an anchor): its seed is the block just added.
    Slot(at(e2 * spe) + 1);
    Main();
    for (int k : {7, 9, 11, 13, 15, 17}) Take(PosBlock(at(e2 * spe) + k));
    const uint256 tip{Tip()->GetBlockHash()};
    using Snapshot = std::map<std::pair<int64_t, uint256>, pos::Liveness>;
    const auto liveness{[&]() {
        Snapshot snapshot;
        LOCK(::cs_main);
        for (int64_t e{e1 - 1}; e <= e2; ++e) {
            for (const CKey& k : keys) {
                if (const auto l{pos::GetLiveness(Chain().SideState().View(), e, XOnly(k))}) snapshot[{e, XOnly(k)}] = *l;
            }
        }
        return snapshot;
    }};
    const Snapshot original{liveness()};
    Hooks().CheckSeeds();
    BOOST_CHECK(Tip()->GetBlockHash() == tip);
    // The reorg: the block that seeds e2 replaced (that of e1 stays); by one with which the first block
    // of e2 keeps its leader, and a slot it skipped does not (it charged another leader for it).
    const int seed_height{Record().Height() - 1};
    const CBlockIndex* first{WITH_LOCK(::cs_main, return Chain().m_chain[Tip()->nHeight - 5])};
    BOOST_REQUIRE_EQUAL(pos::SlotOf(first->nTime, Pos()), e2 * spe + 7);
    const auto leaders{[&]() {
        std::vector<uint256> keys_of;
        LOCK(::cs_main);
        for (int64_t slot{e2 * spe}; slot <= e2 * spe + 7; ++slot) keys_of.push_back(Hooks().LeaderOf(*first->pprev, slot).pubkey);
        return keys_of;
    }};
    const std::vector<uint256> leaders_before{leaders()};
    std::vector<sidechain::MainBlock> replaced;
    bool found{false};
    for (int salt{1}; salt < 100 && !found; ++salt) {
        replaced = Record().Truncate(seed_height - 1);
        for (sidechain::MainBlock main : replaced) {
            main.prev_hash = Record().TipHash();
            main.hash = ArithToUint256(UintToArith256(main.hash) + salt);
            BOOST_REQUIRE(Record().Append(main));
        }
        const std::vector<uint256> leaders_after{leaders()};
        found = leaders_after.back() == leaders_before.back() && !std::equal(leaders_after.begin(), leaders_after.end() - 1, leaders_before.begin());
        if (!found) {
            Record().Truncate(seed_height - 1);
            for (const sidechain::MainBlock& main : replaced) BOOST_REQUIRE(Record().Append(main));
        }
    }
    BOOST_REQUIRE(found);
    Hooks().CheckSeeds();
    const CBlockIndex* now{Tip()};
    const Snapshot checked{liveness()};
    // As the chain is connected afresh under the record as it is now.
    CBlockIndex* first_e2{nullptr};
    {
        LOCK(::cs_main);
        for (CBlockIndex* index{Chain().m_chain.Tip()}; index && index->nHeight > 0; index = index->pprev) {
            if (index->m_ext && pos::SlotOf(index->nTime, Pos()) >= e2 * spe) first_e2 = index;
        }
    }
    BOOST_REQUIRE(first_e2 == first);
    {
        BlockValidationState state;
        BOOST_REQUIRE(Chain().InvalidateBlock(state, first_e2, /*by_record=*/true));
        WITH_LOCK(::cs_main, Chain().ReconsiderRecordFailure(first_e2));
        BOOST_REQUIRE(Chain().ActivateBestChain(state));
        BOOST_CHECK(Tip() == now);
        BOOST_CHECK(liveness() == checked);
    }
    BOOST_CHECK(checked != original);
    // The record as it was: the blocks are given back (the leaders of blocks taken back read the set of
    // an anchor), and the state is the first one again.
    Record().Truncate(seed_height - 1);
    for (const sidechain::MainBlock& main : replaced) BOOST_REQUIRE(Record().Append(main));
    Hooks().CheckSeeds();
    BOOST_CHECK(Tip()->GetBlockHash() == tip);
    BOOST_CHECK(liveness() == original);
}

BOOST_FIXTURE_TEST_CASE(anchor_slot_from_the_commitment, FastBlocksSetup)
{
    // A BMM block whose time is set back (as far as the median time of the chain allows: after a
    // stall, to before an epoch whose seed is known already) does not make the set of the epochs
    // after its time, but of those after the median time of the mainchain block that committed to it.
    // Otherwise whoever makes it could bond, see the seed, and then fix the set (stake grinding).
    StartPos();
    Take(PosBlock(1));
    const int64_t spe{Pos().slots_per_epoch};
    const int64_t later{Slot(static_cast<int>(3 * spe))};
    CBlock bmm{BmmBlock(/*use_mempool=*/false)};
    bmm.nTime = WITH_LOCK(::cs_main, return Tip()->GetMedianTimePast()) + 1;
    FinishBmm(bmm);
    MineBmm(bmm);
    const auto entry{pos::GetAnchorSet(ActiveView(), bmm.GetHash())};
    BOOST_REQUIRE(entry);
    BOOST_CHECK_EQUAL(entry->slot, later);
    BOOST_CHECK_EQUAL(entry->anchors, 2U);
    // The epoch whose set an anchor of the block's time would make: the earlier anchor's set.
    const int64_t epoch{pos::EpochOf(pos::SlotOf(bmm.nTime, Pos()), Pos()) + 2};
    BOOST_REQUIRE(pos::EpochOf(later, Pos()) >= epoch - 1);
    uint64_t set_anchors{0};
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Hooks().SetFor(*Tip(), epoch, nullptr, &set_anchors)));
    BOOST_CHECK_EQUAL(set_anchors, 1U);
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Hooks().SetFor(*Tip(), epoch, &ActiveView(), &set_anchors)));
    BOOST_CHECK_EQUAL(set_anchors, 1U);
    // Its own: from the epoch after the next of its commitment.
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Hooks().SetFor(*Tip(), pos::EpochOf(later, Pos()) + 2, nullptr, &set_anchors)));
    BOOST_CHECK_EQUAL(set_anchors, 2U);
}

BOOST_AUTO_TEST_CASE(penalty_from_unbonding)
{
    // A penalty larger than what is still bonded takes the rest from what the validator is unbonding,
    // the latest unbond first (an unbond taken to nothing leaves the queue); the coins stay accounted for.
    ECC_Context ecc;
    CKey validator, owner;
    validator.MakeNewKey(/*fCompressed=*/true);
    owner.MakeNewKey(/*fCompressed=*/true);
    Consensus::PosParams params;
    params.min_stake = 100;
    params.unbonding_anchors = 10;
    const uint256 genesis{1};
    const pos::Staking staking{params, genesis};
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay out{empty};
    const uint160 owner_hash{owner.GetPubKey().GetID()};
    const uint256 pubkey{XOnly(validator)};
    std::array<unsigned char, 64> pop;
    BOOST_REQUIRE(validator.SignSchnorr(pos::BondPopHash(owner_hash, genesis), pop, nullptr, uint256{}));
    std::string reason;
    CMutableTransaction bond;
    bond.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{1}), 0});
    std::vector<unsigned char> owner_sig;
    BOOST_REQUIRE(owner.SignCompact(MessageHash(pos::BondMessage(pubkey, 10000, bond.vin[0].prevout, genesis)), owner_sig));
    bond.vout.emplace_back(10000, pos::BondScript(pubkey, owner_hash, pop, owner_sig));
    BOOST_REQUIRE(staking.ApplyTx(out, CTransaction{bond}, 1, 0, reason));
    const auto unbond{[&](CAmount amount, uint32_t sequence, uint64_t anchors) {
        pos::Unbond u;
        u.pubkey = pubkey;
        u.amount = amount;
        u.sequence = sequence;
        BOOST_REQUIRE(owner.SignCompact(MessageHash(pos::UnbondMessage(pubkey, amount, sequence, genesis)), u.signature));
        CMutableTransaction tx;
        tx.vout.emplace_back(0, pos::UnbondScript(u));
        BOOST_REQUIRE_MESSAGE(staking.ApplyTx(out, CTransaction{tx}, 2, anchors, reason), reason);
    }};
    unbond(9400, 0, 0);
    unbond(500, 1, 1);
    BOOST_CHECK_EQUAL(pos::GetValidator(out, pubkey)->bonded, 100);
    // 10 % of 10000: 100 from the bond, 500 from the latest unbond (gone), 400 from the first.
    BOOST_REQUIRE(staking.Punish(out, pos::Offence{.pubkey = pubkey, .slot = 7}, CScript() << OP_TRUE, 2, reason));
    const auto record{*pos::GetValidator(out, pubkey)};
    BOOST_CHECK(record.tombstoned);
    BOOST_CHECK_EQUAL(record.bonded, 0);
    BOOST_REQUIRE_EQUAL(record.unbonding.size(), 1U);
    BOOST_CHECK(record.unbonding[0] == (pos::UnbondingEntry{.release_anchor = 10, .sequence = 0, .amount = 9000}));
    std::vector<std::pair<uint64_t, CAmount>> queue;
    pos::ForEachUnbonding(out, [&](uint64_t release, const uint256& key, CAmount amount) {
        BOOST_CHECK(key == pubkey);
        queue.emplace_back(release, amount);
        return true;
    });
    BOOST_CHECK(queue == (std::vector<std::pair<uint64_t, CAmount>>{{10, 9000}}));
    const pos::StakeState stake{pos::GetStakeState(out)};
    BOOST_CHECK_EQUAL(stake.bonded, 0);
    BOOST_CHECK_EQUAL(stake.unbonding, 9000);
    BOOST_CHECK_EQUAL(stake.slashed, 1000);
    BOOST_CHECK_EQUAL(stake.rewards_owed + stake.pot_reserve, 1000);
}

BOOST_FIXTURE_TEST_CASE(deterministic_signatures, FastBlocksSetup)
{
    // The producer signs a block the same way every time (BIP340, all-zero auxiliary randomness). A
    // block with another valid signature of its leader (made with other randomness) is the same block:
    // a node that has the header with one takes the block with the other, keeps its own, and the chain
    // goes on (no mutated block, no peer punished, no split). Another key's signature stays a mutated block.
    StartPos();
    Take(PosBlock(1));
    const CBlock honest{PosBlock(2)};
    const int64_t slot{pos::SlotOf(honest.nTime, Pos())};
    const CKey& leader{Leader(slot)};
    CBlock again{honest};
    again.ext.sig.fill(0);
    BOOST_REQUIRE(pos::Producer::Sign(again, leader, Genesis()));
    BOOST_CHECK(again.ext.sig == honest.ext.sig);
    std::array<unsigned char, 64> zero_aux;
    BOOST_REQUIRE(leader.SignSchnorr(pos::BlockSigHash(honest, Genesis()), zero_aux, nullptr, uint256::ZERO));
    BOOST_CHECK(zero_aux == honest.ext.sig);

    CBlock other{honest};
    BOOST_REQUIRE(leader.SignSchnorr(pos::BlockSigHash(honest, Genesis()), other.ext.sig, nullptr, GetRandHash()));
    BOOST_REQUIRE(other.ext.sig != honest.ext.sig && other.GetHash() == honest.GetHash());
    BOOST_REQUIRE(pos::CheckBlockSig(other, XOnly(leader), Genesis()));
    CBlock swapped{honest};
    BOOST_REQUIRE(pos::Producer::Sign(swapped, NotLeader(slot), Genesis()));

    // The node has the header with the other signature.
    BlockValidationState header_state;
    BOOST_REQUIRE_MESSAGE(Chainman().ProcessNewBlockHeaders({{other}}, /*min_pow_checked=*/true, header_state), header_state.ToString());
    const CBlockIndex* index{WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(honest.GetHash()))};
    BOOST_REQUIRE(index && index->m_ext->sig == other.ext.sig);
    BOOST_CHECK(WITH_LOCK(::cs_main, return Hooks().CheckOtherExtension(honest, *index)) == sidechain::OtherExtension::EQUIVALENT);
    BOOST_CHECK(WITH_LOCK(::cs_main, return Hooks().CheckOtherExtension(swapped, *index)) == sidechain::OtherExtension::INVALID);
    // Another key's: a mutated block, as before.
    {
        BlockValidationState state;
        BOOST_CHECK(!WITH_LOCK(::cs_main, return Chainman().AcceptBlock(std::make_shared<const CBlock>(swapped), state, nullptr, true, nullptr, nullptr, true)));
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_MUTATED);
    }
    // The producer's: taken, stored with the signature of the header the node had.
    {
        BlockValidationState state;
        bool new_block{false};
        BOOST_CHECK(WITH_LOCK(::cs_main, return Chainman().AcceptBlock(std::make_shared<const CBlock>(honest), state, nullptr, true, nullptr, &new_block, true)));
        BOOST_CHECK(state.IsValid());
        BOOST_CHECK(new_block);
    }
    Submit(honest);
    BOOST_CHECK(Tip() == index);
    BOOST_CHECK_EQUAL(Reason(honest), "");
    CBlock read;
    BOOST_REQUIRE(Chainman().m_blockman.ReadBlock(read, *index));
    BOOST_CHECK(read.ext.sig == other.ext.sig);
    // Either again: a duplicate, no failure.
    for (const CBlock& block : {honest, other}) {
        BlockValidationState state;
        BOOST_CHECK(WITH_LOCK(::cs_main, return Chainman().AcceptBlock(std::make_shared<const CBlock>(block), state, nullptr, true, nullptr, nullptr, true)));
        BOOST_CHECK(state.IsValid());
    }
    BOOST_CHECK(WITH_LOCK(::cs_main, return !(index->nStatus & BLOCK_FAILED_VALID)));
    // No proof that the leader signed twice: one block, one hash.
    BOOST_CHECK(Hooks().Evidence().All().empty());
    Take(PosBlock(3));
}

BOOST_FIXTURE_TEST_CASE(bond_griefing, FastBlocksSetup)
{
    // Anyone could add to a validator's bond (the proof of possession is public): a satoshi added
    // made the owner's signed unbond of all of it leave a remainder under the minimum stake. Now only
    // the owner adds bonds (it signs them), and an unbond of all takes all there is when it is applied.
    // (An unbond signed before the owner's own top-up no longer counts: unbond_replay_after_topup.)
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    CKey validator, attacker;
    validator.MakeNewKey(/*fCompressed=*/true);
    attacker.MakeNewKey(/*fCompressed=*/true);
    const uint256 pubkey{XOnly(validator)};
    const CAmount min{Pos().min_stake};
    Accept(Pay(coin, spent, {BondOut(validator, 2 * min, Owner(key), &key, coin)}));
    Take(PosBlock(1));
    // The owner signs an unbond of all of it.
    const CTxOut unbond_all{UnbondOut(validator, pos::UNBOND_ALL, 0, key)};
    // The attacker's top-up of a satoshi, with the public proof of possession: refused, without the
    // owner's signature, with its own, or with the owner's of another bond.
    const pos::Bond public_bond{*pos::ParseBond(BondOut(validator, 2 * min, Owner(key)))};
    {
        COutPoint c{coin};
        CTxOut s{spent};
        BOOST_CHECK_EQUAL(Refused(Pay(c, s, {CTxOut{1, pos::BondScript(pubkey, Owner(key), public_bond.pop)}})), "bad-pos-bond-owner-sig");
    }
    {
        COutPoint c{coin};
        CTxOut s{spent};
        BOOST_CHECK_EQUAL(Refused(Pay(c, s, {BondOut(validator, 1, Owner(key), &attacker, coin)})), "bad-pos-bond-owner-sig");
    }
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, 1, Owner(key), &attacker)})), "bad-pos-bond-owner-sig");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, 1, Owner(attacker))})), "bad-pos-bond-owner");
    // Nor in a block.
    {
        CBlock block{PosBlock(2)};
        COutPoint c{coin};
        CTxOut s{spent};
        block.vtx.push_back(MakeTransactionRef(Pay(c, s, {CTxOut{1, pos::BondScript(pubkey, Owner(key), public_bond.pop)}})));
        Resign(block);
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-bond-owner-sig");
    }
    BOOST_CHECK_EQUAL(Validator(pubkey)->bonded, 2 * min);
    // The unbond of all the owner signed takes all there is.
    BOOST_CHECK_EQUAL(StakingRules(TxWith({unbond_all})), "");
    // The owner's own top-up, signed, is taken (and the unbond signed before it is void); a new unbond
    // of all takes all of it.
    Accept(Pay(coin, spent, {BondOut(validator, min + 1, Owner(key), &key, coin)}));
    Take(PosBlock(2));
    BOOST_CHECK_EQUAL(Validator(pubkey)->bonded, 3 * min + 1);
    BOOST_CHECK_EQUAL(StakingRules(TxWith({unbond_all})), "bad-pos-unbond-sequence");
    Accept(Pay(coin, spent, {UnbondOut(validator, pos::UNBOND_ALL, 1, key)}));
    Take(PosBlock(3));
    const auto record{*Validator(pubkey)};
    BOOST_CHECK_EQUAL(record.bonded, 0);
    BOOST_CHECK_EQUAL(record.Unbonding(), 3 * min + 1);
    BOOST_CHECK_EQUAL(record.unbond_seq, 2U);
    BOOST_CHECK(!record.Eligible(Pos()));
    BOOST_CHECK_EQUAL(Stake().bonded, 0);
    BOOST_CHECK_EQUAL(Stake().unbonding, 3 * min + 1);
}

BOOST_FIXTURE_TEST_CASE(slashed_once_and_out_of_the_schedule, FastBlocksSetup)
{
    // A validator is slashed once: a proof of another of its offences, once it is banned, takes
    // nothing more (bad-pos-evidence-banned), nor does a second proof in the block that bans it. And
    // from the block after the one that bans it, it leads no more: its blocks are invalid
    // (bad-pos-leader-banned), its producer makes none, and the others go on.
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    const CAmount bond{100000};
    Accept(Pay(coin, spent, {BondOut(keys[0], bond, Owner(key), &key, coin), BondOut(keys[1], bond, Owner(key), &key, coin), BondOut(keys[2], bond, Owner(key), &key, coin)}));
    Take(PosBlock(1));
    const auto [a1, b1]{TwoBlocks(2)};
    const pos::DoubleProposal first_offence{.a = a1, .b = b1};
    const uint256 offender{XOnly(Leader(base_slot + 2))};
    Take(a1);
    const auto leads{[&](int i) { return XOnly(Leader(base_slot + i)) == offender; }};
    int k{3};
    while (leads(k)) ++k;
    int j{k + 1};
    while (!leads(j)) ++j;
    int m{j + 1};
    while (leads(m)) ++m;
    // Another offence, at slot j (on the slot-2 block); its first block is valid as yet.
    const auto [a2, b2]{TwoBlocks(j)};
    const pos::DoubleProposal second_offence{.a = a2, .b = b2};
    BOOST_CHECK_EQUAL(Check(a2), "");
    const sidechain::StoreView& before{ActiveView()};
    BOOST_CHECK_EQUAL(After(WithEvidence(m, {second_offence}), before), "");
    BOOST_CHECK_EQUAL(After(WithEvidence(m, {first_offence, second_offence}), before), "bad-pos-evidence-banned");

    // The first proof, in the block of slot k: banned, 10 % gone.
    Take(WithEvidence(k, {first_offence}));
    const auto banned{*Validator(offender)};
    BOOST_REQUIRE(banned.tombstoned);
    const pos::StakeState stake{Stake()};
    BOOST_CHECK_EQUAL(stake.slashed, bond / 10);
    // The second proof takes nothing more. (Asked of the rules directly: blocks of one slot given to
    // validation would be proofs that its leader signed them all.)
    BOOST_CHECK_EQUAL(After(WithEvidence(m, {second_offence}), ActiveView()), "bad-pos-evidence-banned");
    // The offender's block of slot j, on the block that banned it: invalid; its producer makes none.
    {
        const int64_t slot_j{Slot(j)};
        std::string error;
        auto block{producer->Build(slot_j, std::nullopt, error)};
        BOOST_REQUIRE_MESSAGE(block, error);
        BOOST_REQUIRE(pos::Producer::Sign(*block, Leader(slot_j), Genesis()));
        BOOST_CHECK_EQUAL(Check(*block), "bad-pos-leader-banned");
        BOOST_CHECK(!producer->Produce(slot_j, error));
        BOOST_CHECK(error.find("banned") != std::string::npos);
    }
    // The producer leaves the second proof out; the others' blocks go on. Nothing more is taken.
    std::string reason;
    BOOST_REQUIRE_MESSAGE(Hooks().AddEvidence(second_offence, reason), reason);
    std::string error;
    BOOST_REQUIRE_MESSAGE(producer->Produce(Slot(m), error), error);
    CBlock made;
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Chainman().m_blockman.ReadBlock(made, *Tip())));
    pos::PosSections sections;
    BOOST_REQUIRE(pos::PosSections::Decode(made.vSections, sections));
    BOOST_CHECK(sections.evidence.empty());
    BOOST_CHECK(Stake() == stake);
    BOOST_CHECK(*Validator(offender) == banned);
    // A merged mined block leaves it out too.
    const CBlock bmm{BmmBlock(/*use_mempool=*/false)};
    for (const CTxOut& out : bmm.vtx[0]->vout) BOOST_CHECK(!pos::IsEvidenceScript(out.scriptPubKey));
}

BOOST_FIXTURE_TEST_CASE(self_reported, FastBlocksSetup)
{
    // The offender carrying the proof against itself: the penalty all the same, no reward, its
    // share to the reserve.
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    const CAmount bond{100000};
    Accept(Pay(coin, spent, {BondOut(keys[0], bond, Owner(key), &key, coin), BondOut(keys[1], bond, Owner(key), &key, coin), BondOut(keys[2], bond, Owner(key), &key, coin)}));
    Take(PosBlock(1));
    const auto [first, second]{TwoBlocks(2)};
    const pos::DoubleProposal proof{.a = first, .b = second};
    const uint256 offender{XOnly(Leader(base_slot + 2))};
    Take(first);
    int k{3};
    while (XOnly(Leader(base_slot + k)) != offender) ++k;
    const CBlock carrying{WithEvidence(k, {proof})};
    Take(carrying);
    const CAmount penalty{bond / 10};
    BOOST_CHECK(Validator(offender)->tombstoned);
    BOOST_CHECK_EQUAL(Stake().slashed, penalty);
    BOOST_CHECK_EQUAL(Stake().rewards_owed, 0);
    BOOST_CHECK_EQUAL(Stake().pot_reserve, penalty);
    BOOST_CHECK_EQUAL(Stake().bonded + Stake().unbonding + Stake().pot_reserve + Stake().rewards_owed, 3 * bond);
    // The next peg step pays no reward to the block that carried it.
    const CBlock bmm{BmmBlock(/*use_mempool=*/false)};
    BOOST_CHECK(!Pays(bmm, CTxOut{penalty / 8, carrying.vtx[0]->vout[0].scriptPubKey}));
    MineBmm(bmm);
    BOOST_CHECK_EQUAL(Stake().rewards_owed, 0);
}

BOOST_FIXTURE_TEST_CASE(evidence_in_bmm_blocks, FastBlocksSetup)
{
    // A merged mined block carries proofs in its coinbase (OP_RETURN 0xDD'EVD' | the proof), under the
    // rules of a block of stake's; the reward goes to its payee (its coinbase's first output) at the
    // next peg step. The node's merged miner puts the proofs it knows in.
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    const CAmount bond{100000};
    Accept(Pay(coin, spent, {BondOut(keys[0], bond, Owner(key), &key, coin), BondOut(keys[1], bond, Owner(key), &key, coin), BondOut(keys[2], bond, Owner(key), &key, coin)}));
    Take(PosBlock(1));
    const auto [first, second]{TwoBlocks(2)};
    pos::DoubleProposal proof{.a = first, .b = second};
    proof.Normalize();
    const uint256 offender{XOnly(Leader(base_slot + 2))};
    Take(first);

    // The form, context free (CheckBlock).
    const auto form{[&](const CBlock& block) {
        BlockValidationState state;
        return Hooks().CheckBlock(block, state) ? std::string{} : state.GetRejectReason();
    }};
    const auto with_outputs{[&](CBlock block, const std::vector<CTxOut>& outputs) {
        SetCoinbase(block, [&](CMutableTransaction& tx) { tx.vout.insert(tx.vout.end(), outputs.begin(), outputs.end()); });
        return block;
    }};
    const CBlock plain{BmmBlock(/*use_mempool=*/false)};
    BOOST_CHECK_EQUAL(form(with_outputs(plain, {CTxOut{0, pos::EvidenceScript(proof)}})), "");
    BOOST_CHECK_EQUAL(form(with_outputs(plain, {CTxOut{0, CScript() << OP_RETURN << std::vector<unsigned char>{0xDD, 'E', 'V', 'D', 1, 2}}})), "bad-pos-evidence-output");
    BOOST_CHECK_EQUAL(form(with_outputs(plain, {CTxOut{0, pos::EvidenceScript({.a = first, .b = first})}})), "bad-pos-evidence-same");
    BOOST_CHECK_EQUAL(form(with_outputs(plain, std::vector<CTxOut>(pos::MAX_BMM_EVIDENCE, CTxOut{0, pos::EvidenceScript(proof)}))), "");
    BOOST_CHECK_EQUAL(form(with_outputs(plain, std::vector<CTxOut>(pos::MAX_BMM_EVIDENCE + 1, CTxOut{0, pos::EvidenceScript(proof)}))), "bad-pos-evidence-count");
    BOOST_CHECK_EQUAL(form(with_outputs(PosBlock(3), {CTxOut{0, pos::EvidenceScript(proof)}})), "bad-pos-evidence-not-bmm");
    BOOST_CHECK(pos::ParseEvidenceScript(pos::EvidenceScript(proof)) == std::optional<pos::DoubleProposal>{proof});

    // Two for one offence, in a BMM block: the second is known.
    {
        CBlock twice{with_outputs(plain, {CTxOut{0, pos::EvidenceScript(proof)}, CTxOut{0, pos::EvidenceScript(proof)}})};
        FinishBmm(twice);
        Commit(twice.GetHash());
        BOOST_CHECK_EQUAL(Check(twice), "bad-pos-evidence-known");
    }

    // The merged miner puts the proof the node knows in its block.
    std::string reason;
    BOOST_REQUIRE_MESSAGE(Hooks().AddEvidence(proof, reason), reason);
    CKey payee_key;
    payee_key.MakeNewKey(/*fCompressed=*/true);
    const CScript payee{GetScriptForDestination(WitnessV0KeyHash{payee_key.GetPubKey()})};
    const CBlock carrying{BmmBlock(/*use_mempool=*/false, payee)};
    std::vector<pos::DoubleProposal> carried;
    for (const CTxOut& out : carrying.vtx[0]->vout) {
        if (const auto read{pos::ParseEvidenceScript(out.scriptPubKey)}) carried.push_back(*read);
    }
    BOOST_REQUIRE_EQUAL(carried.size(), 1U);
    BOOST_CHECK(carried[0] == proof);
    MineBmm(carrying);
    const CAmount penalty{bond / 10};
    BOOST_CHECK(Validator(offender)->tombstoned);
    BOOST_CHECK_EQUAL(Stake().slashed, penalty);
    BOOST_CHECK_EQUAL(Stake().rewards_owed, penalty / 8);
    BOOST_CHECK_EQUAL(Stake().pot_reserve, penalty - penalty / 8);
    // Not again: the next one carries nothing, and one that does is refused.
    const CBlock next{BmmBlock(/*use_mempool=*/false)};
    for (const CTxOut& out : next.vtx[0]->vout) BOOST_CHECK(!pos::IsEvidenceScript(out.scriptPubKey));
    {
        CBlock again{with_outputs(next, {CTxOut{0, pos::EvidenceScript(proof)}})};
        FinishBmm(again);
        Commit(again.GetHash());
        BOOST_CHECK_EQUAL(Check(again), "bad-pos-evidence-known");
    }
    // The reward, to the BMM block's payee, at the next peg step.
    BOOST_CHECK(Pays(next, CTxOut{penalty / 8, payee}));
    MineBmm(next);
    BOOST_CHECK_EQUAL(Stake().rewards_owed, 0);
}

BOOST_FIXTURE_TEST_CASE(forged_ack_headers, FastBlocksSetup)
{
    // An acknowledgement counts as an anchor in the order of chains from its header on. Its height
    // comes after the last anchor of the chain below it (the state's bad-pos-ack-order, as far as
    // the headers tell): otherwise one commitment on record could count again and again.
    StartPos();
    const int bmm_height{Record().Height()};
    Take(PosBlock(1));
    const pos::AnchorAck ack{AckTip(2)};
    BOOST_REQUIRE_EQUAL(Key().last_anchor_height, static_cast<uint32_t>(ack.main_height));
    const uint256 best{BestHeader()};
    for (const int height : {ack.main_height, bmm_height}) {
        // Signed by the leader of its slot, as it may be: the leader too counts only new commitments.
        const CBlock claimed{WithAck(PosBlock(3), pos::AnchorAck{.ticket = ack.ticket, .main_height = height})};
        BOOST_CHECK_EQUAL(HeaderReason({claimed}), "bad-pos-ack-order");
        BOOST_CHECK(BestHeader() == best);
    }

    // A branch from a block whose epoch's set the state pruned (anchors since, over epochs), on a
    // node that does not have it in memory either (it restarted): its headers cannot be checked, and
    // are refused, unsigned ones that claim old commitments with them.
    const CBlockHeader like{Tip()->GetBlockHeader()};
    const uint256 fork{Tip()->GetBlockHash()};
    std::vector<int> heights;
    for (int n{1}; n <= 16; ++n) heights.push_back(AckTip(3 + 6 * n).main_height);
    Hooks().ForgetKnownSets();
    BOOST_REQUIRE(!WITH_LOCK(::cs_main, return Hooks().SetFor(*Index(fork), pos::EpochOf(pos::SlotOf(GetTime(), Pos()), Pos()))));
    const uint256 tip_best{BestHeader()};
    uint256 prev{fork};
    std::vector<CBlockHeader> branch;
    for (size_t n{0}; n < 4; ++n) {
        branch.push_back(UnsignedHeader(like, prev, pos::SlotOf(GetTime(), Pos()) - 3 + static_cast<int64_t>(n), static_cast<uint32_t>(heights[n]), static_cast<uint8_t>(n)));
        prev = branch.back().GetHash();
    }
    BOOST_CHECK_EQUAL(HeaderReason(branch), "bad-pos-fork-too-deep");
    BOOST_CHECK(!Index(branch[0].GetHash()));
    BOOST_CHECK(BestHeader() == tip_best);
}

BOOST_FIXTURE_TEST_CASE(unsigned_ack_header_unknown_set, FastBlocksSetup)
{
    // A header whose epoch's set is that of an anchor on a branch this node never connected: its
    // signature cannot be checked until its block is connected. Taken if the branch leaves the active
    // chain above its final block (a node that catches up takes in such headers), not below: a header
    // nobody checked does not take back a final block.
    StartPos();
    Take(PosBlock(1));
    const CBlockHeader like{Tip()->GetBlockHeader()};
    const uint256 fork{Tip()->GetBlockHash()};
    // A BMM block on it, committed to, that this node has the header of only.
    const CBlock other{BmmBlock(/*use_mempool=*/false)};
    Commit(other.GetHash());
    BOOST_REQUIRE_EQUAL(HeaderReason({other}), "");
    // The active chain goes on with anchors of its own, over epochs.
    std::vector<int> heights;
    for (int n{1}; n <= 4; ++n) heights.push_back(AckTip(1 + 12 * n).main_height);
    const int64_t slot{pos::SlotOf(GetTime(), Pos())};
    BOOST_REQUIRE_GE(pos::EpochOf(slot, Pos()), pos::EpochOf(pos::SlotOf(other.nTime, Pos()), Pos()) + 2);
    BOOST_REQUIRE(!WITH_LOCK(::cs_main, return Hooks().SetFor(*Index(other.GetHash()), pos::EpochOf(slot, Pos()))));
    const uint256 best{BestHeader()};
    // Below the final block: refused, unsigned or not (nobody can tell).
    const CBlockHeader claimed{UnsignedHeader(like, other.GetHash(), slot, static_cast<uint32_t>(heights.back()))};
    BOOST_CHECK_EQUAL(HeaderReason({claimed}), "bad-pos-fork-too-deep");
    BOOST_CHECK(!Index(claimed.GetHash()));
    BOOST_CHECK(BestHeader() == best);
    (void)fork;
}

BOOST_FIXTURE_TEST_CASE(future_unsigned_header_not_waiting, FastBlocksSetup)
{
    // A header of a slot to come waits only if its leader signed it: one whose leader the node cannot
    // tell (the seed of its epoch, or the set, not known) is turned away, not kept (anyone can make
    // them, and those kept push out the headers that wait for good reasons).
    StartPos();
    Take(PosBlock(1));
    const CBlockHeader like{Tip()->GetBlockHeader()};
    const size_t waiting{Waiting()};
    // A day ahead: the seed of its epoch is not on record.
    const int64_t now{pos::SlotOf(GetTime(), Pos())};
    const CBlockHeader far{UnsignedHeader(like, Tip()->GetBlockHash(), now + 24 * 60 * 60, 0)};
    BOOST_CHECK(!Hooks().IsWaitReason(HeaderReason({far})));
    BOOST_CHECK_EQUAL(Waiting(), waiting);
    // A minute ahead, on a branch whose set the node does not know.
    const CBlock other{BmmBlock(/*use_mempool=*/false)};
    Commit(other.GetHash());
    BOOST_REQUIRE_EQUAL(HeaderReason({other}), "");
    Mock(pos::SlotTime(now + 2 * Pos().slots_per_epoch, Pos()));
    Main();
    const int64_t later{pos::SlotOf(GetTime(), Pos()) + 60};
    BOOST_REQUIRE(!WITH_LOCK(::cs_main, return Hooks().SetFor(*Index(other.GetHash()), pos::EpochOf(later, Pos()))));
    const CBlockHeader soon{UnsignedHeader(like, other.GetHash(), later, 0)};
    BOOST_CHECK(!Hooks().IsWaitReason(HeaderReason({soon})));
    BOOST_CHECK_EQUAL(Waiting(), waiting);
    // Its leader's, a few slots ahead: waits.
    const CBlock signed_block{PosBlock(static_cast<int>(pos::SlotOf(GetTime(), Pos()) - base_slot) + 5)};
    Mock(signed_block.nTime - 5);
    BOOST_CHECK_EQUAL(HeaderReason({signed_block}), "pos-time-future");
    BOOST_CHECK_EQUAL(Waiting(), waiting + 1);
}

BOOST_FIXTURE_TEST_CASE(unchecked_siblings_not_taken, FastBlocksSetup)
{
    // Headers whose leader the node cannot tell yet (a branch with an anchor it never connected, as for
    // a node that catches up), above the final block: a peer makes the node take at most its budget of
    // them (unchecked_headers_per_peer) until they are checked. Siblings by the thousand, which anyone
    // can make, are not taken; nor a message padded with headers that claim whatever the record
    // allows; and what such headers claim stops nobody else's.
    // (A budget of 3 for the test. The params are the hooks', made for this test.)
    auto& params{const_cast<Consensus::PosParams&>(Pos())};
    const int64_t budget_before{params.unchecked_headers_per_peer};
    params.unchecked_headers_per_peer = 3;
    StartPos();
    Take(PosBlock(1));
    const CBlockHeader like{Tip()->GetBlockHeader()};
    const CBlock other{BmmBlock(/*use_mempool=*/false)};
    Commit(other.GetHash());
    BOOST_REQUIRE_EQUAL(HeaderReason({other}), "");
    BOOST_REQUIRE(BestHeader() == other.GetHash());
    // Commitments on record that headers nobody checks can claim to acknowledge.
    std::vector<uint32_t> commitments;
    for (uint8_t i{1}; i <= 4; ++i) {
        Commit(uint256{static_cast<uint8_t>(0xc0 + i)});
        commitments.push_back(static_cast<uint32_t>(Record().Height()));
    }
    Mock(pos::SlotTime(pos::SlotOf(GetTime(), Pos()) + 2 * Pos().slots_per_epoch, Pos()));
    Main();
    const int64_t slot{pos::SlotOf(GetTime(), Pos())};
    BOOST_REQUIRE(!WITH_LOCK(::cs_main, return Hooks().SetFor(*Index(other.GetHash()), pos::EpochOf(slot - 10, Pos()))));

    // An attacker first: a branch that claims every commitment on record, to inflate the best header.
    std::vector<CBlockHeader> inflated;
    uint256 prev{other.GetHash()};
    for (size_t i{0}; i < 3; ++i) {
        inflated.push_back(UnsignedHeader(like, prev, slot - 10 + static_cast<int64_t>(i), commitments[i], 1));
        prev = inflated.back().GetHash();
    }
    BOOST_CHECK_EQUAL(HeaderReason(inflated, /*source=*/1), "");
    BOOST_CHECK(BestHeader() == inflated.back().GetHash());
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return Hooks().UncheckedHeaders(1)), 3U);
    // Its budget is spent: siblings of the newest header are not taken, the peer not punished.
    for (uint8_t salt{2}; salt < 12; ++salt) {
        const CBlockHeader sibling{UnsignedHeader(like, inflated[1].GetHash(), slot - 8, 0, salt)};
        BlockValidationState state;
        BOOST_CHECK(!Chainman().ProcessNewBlockHeaders({{sibling}}, /*min_pow_checked=*/true, state, nullptr, /*source=*/1));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "pos-unchecked-budget");
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_HEADER_LOW_WORK);
        BOOST_CHECK(!Index(sibling.GetHash()));
    }
    // A message padded with headers that claim more (the last commitment too): taken up to the
    // budget of its peer, not further, whatever it claims.
    std::vector<CBlockHeader> padded;
    prev = other.GetHash();
    for (size_t i{0}; i < 5; ++i) {
        padded.push_back(UnsignedHeader(like, prev, slot - 10 + static_cast<int64_t>(i), i < commitments.size() ? commitments[i] : 0, 2));
        prev = padded.back().GetHash();
    }
    BOOST_CHECK_EQUAL(HeaderReason(padded, /*source=*/2), "pos-unchecked-budget");
    for (size_t i{0}; i < padded.size(); ++i) BOOST_CHECK_EQUAL(Index(padded[i].GetHash()) != nullptr, i < 3);

    // An honest peer, whose branch claims less than the inflated best header (fewer anchors): its
    // headers are taken all the same (a node catching up needs them).
    std::vector<CBlockHeader> honest;
    prev = other.GetHash();
    for (size_t i{0}; i < 3; ++i) {
        honest.push_back(UnsignedHeader(like, prev, slot - 10 + static_cast<int64_t>(i), 0, 3));
        prev = honest.back().GetHash();
    }
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Index(honest.back().GetHash()) == nullptr && Chainman().m_best_header->nChainWork > Hooks().ChainWork(Index(other.GetHash()), honest[0])));
    BOOST_CHECK_EQUAL(HeaderReason(honest, /*source=*/3), "");
    for (const CBlockHeader& header : honest) BOOST_CHECK(Index(header.GetHash()));
    // Not from the node itself (its operator, its disk).
    const CBlockHeader own{UnsignedHeader(like, inflated[1].GetHash(), slot - 8, 0, 40)};
    BOOST_CHECK_EQUAL(HeaderReason({own}, /*source=*/-1), "");
    // A peer gone: what was counted for it goes.
    WITH_LOCK(::cs_main, Hooks().SourceGone(1));
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return Hooks().UncheckedHeaders(1)), 0U);
    params.unchecked_headers_per_peer = budget_before;
}

BOOST_FIXTURE_TEST_CASE(unchecked_headers_checked_give_room, FastBlocksSetup)
{
    // The headers nobody could check that a peer made the node take give their place back once
    // checked (their blocks connected): an honest peer's budget is not used up as the node catches up.
    auto& params{const_cast<Consensus::PosParams&>(Pos())};
    const int64_t budget_before{params.unchecked_headers_per_peer};
    params.unchecked_headers_per_peer = 1;
    StartPos();
    Take(PosBlock(1));
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    const CBlock anchor{PosBlock(2, ack)};
    Take(anchor);
    const int64_t epoch{pos::EpochOf(pos::SlotOf(anchor.nTime, Pos()), Pos())};
    const int last{static_cast<int>((epoch + 2) * Pos().slots_per_epoch - base_slot)};
    const CBlock honest{PosBlock(last)};
    // The node as it was before it connected the anchor: the anchor taken back, its set forgotten.
    CBlockIndex* anchor_index{Index(anchor.GetHash())};
    {
        BlockValidationState state;
        BOOST_REQUIRE(Chain().InvalidateBlock(state, anchor_index));
        LOCK(::cs_main);
        Chain().ResetBlockFailureFlags(anchor_index);
        Chainman().RecalculateBestHeader();
    }
    Hooks().ForgetKnownSets();
    // The header, from peer 5: taken unchecked, its budget spent.
    BOOST_REQUIRE_EQUAL(HeaderReason({honest}, /*source=*/5), "");
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return Hooks().UncheckedHeaders(5)), 1U);
    CBlockHeader sibling{honest};
    sibling.hashMerkleRoot = uint256{0x77};
    sibling.ext.sig.fill(0);
    BOOST_CHECK_EQUAL(HeaderReason({sibling}, /*source=*/5), "pos-unchecked-budget");
    // Its block connected (checked): room again.
    {
        BlockValidationState state;
        BOOST_REQUIRE(Chain().ActivateBestChain(state));
    }
    Submit(honest);
    BOOST_REQUIRE(Tip() == Index(honest.GetHash()));
    {
        LOCK(::cs_main);
        Hooks().BeginHeaders({}, 5);
        BOOST_CHECK_EQUAL(Hooks().UncheckedHeaders(5), 0U);
        Hooks().EndHeaders();
    }
    params.unchecked_headers_per_peer = budget_before;
}

BOOST_FIXTURE_TEST_CASE(record_independent_failures, FastBlocksSetup)
{
    // A block that breaks a rule of FastBlocks whatever the record says (its producer's share, here)
    // did not fail against the record: not noted so, not tried again when the record changes, and the
    // peers that send headers on it are not pardoned. One whose leader the record decides is.
    BOOST_CHECK(Hooks().RecordIndependent("bad-pos-producer-share"));
    BOOST_CHECK(sidechain::RecordIndependent("bad-pos-producer-share"));
    BOOST_CHECK(sidechain::RecordIndependent("bad-pos-bond-owner-sig"));
    BOOST_CHECK(sidechain::RecordIndependent("bad-pos-evidence-same"));
    BOOST_CHECK(!sidechain::RecordIndependent("bad-pos-sig"));
    BOOST_CHECK(!sidechain::RecordIndependent("bad-pos-ack-order"));
    BOOST_CHECK(!sidechain::RecordIndependent("pos-seed-unknown"));
    COutPoint coin;
    const CTxOut spent{Fund(COIN, coin)};
    StartPos();
    Accept(Spend(coin, spent, {CTxOut{COIN - TX_FEE, script}}));
    CBlock greedy{PosBlock(1)};
    SetCoinbase(greedy, [](CMutableTransaction& cb) {
        cb.vout[0].nValue += 1;
        for (auto& out : cb.vout) {
            if (pos::IsPotScript(out.scriptPubKey)) out.nValue -= 1;
        }
    });
    Resign(greedy);
    const CBlockIndex* tip{Tip()};
    Submit(greedy);
    BOOST_CHECK_EQUAL(Reason(greedy), "bad-pos-producer-share");
    BOOST_CHECK(Tip() == tip);
    CBlockIndex* index{Index(greedy.GetHash())};
    BOOST_REQUIRE(index);
    BOOST_CHECK(WITH_LOCK(::cs_main, return index->nStatus & BLOCK_FAILED_VALID));
    BOOST_CHECK(Record().GetFailure(greedy.GetHash()) != sidechain::Mainchain::Failure::RECORD);
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Chain().ReconsiderRecordFailure(index)));
    // A header on it: its sender is punished.
    CBlock child{greedy};
    child.hashPrevBlock = greedy.GetHash();
    child.nTime += 1;
    BlockValidationState state;
    BOOST_CHECK(!Chainman().ProcessNewBlockHeaders({{child}}, /*min_pow_checked=*/true, state));
    BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_INVALID_PREV);
}

BOOST_FIXTURE_TEST_CASE(forged_signature_replaced, FastBlocksSetup)
{
    // A node that takes a header before it can check its signature (the set of its epoch is that of
    // an anchor it has not connected yet: it is catching up) may be given the header with a forged
    // signature first (the block hash does not cover it). Once it can check it, the block its leader
    // signed replaces it -- whether the node has the forged block too, which failed (bad-pos-sig), or not.
    StartPos();
    Take(PosBlock(1));
    int last{1};
    for (const bool forged_body : {false, true}) {
        const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
        const CBlock anchor{PosBlock(last + 1, ack)};
        Take(anchor);
        const int64_t epoch{pos::EpochOf(pos::SlotOf(anchor.nTime, Pos()), Pos())};
        last = static_cast<int>((epoch + 2) * Pos().slots_per_epoch - base_slot);
        const CBlock honest{PosBlock(last)};
        CBlock forged{honest};
        forged.ext.sig.fill(0);
        // The node as it was before it connected the anchor: the anchor taken back, its set forgotten.
        CBlockIndex* anchor_index{Index(anchor.GetHash())};
        {
            BlockValidationState state;
            BOOST_REQUIRE(Chain().InvalidateBlock(state, anchor_index));
            LOCK(::cs_main);
            Chain().ResetBlockFailureFlags(anchor_index);
            Chainman().RecalculateBestHeader();
        }
        Hooks().ForgetKnownSets();
        BOOST_REQUIRE(Tip() == anchor_index->pprev);
        // The forged header: taken, its signature not checked yet.
        BOOST_REQUIRE_EQUAL(HeaderReason({forged}), "");
        const CBlockIndex* index{Index(honest.GetHash())};
        BOOST_REQUIRE(index);
        if (forged_body) {
            // ... and the forged block: it fails once the anchor is connected.
            Submit(forged);
            BOOST_CHECK_EQUAL(Reason(forged), "bad-pos-sig");
            BOOST_CHECK(Tip() == anchor_index);
            // The honest header, from a peer: it replaces the forged one, and the block (the same
            // transactions) is connected with it.
            BOOST_CHECK_EQUAL(HeaderReason({honest}), "");
            BOOST_CHECK(WITH_LOCK(::cs_main, return !(index->nStatus & BLOCK_FAILED_VALID)));
            BlockValidationState state;
            BOOST_REQUIRE(Chain().ActivateBestChain(state));
        } else {
            BlockValidationState state;
            BOOST_REQUIRE(Chain().ActivateBestChain(state));
            BOOST_REQUIRE(Tip() == anchor_index);
            // The honest block: it replaces the forged header.
            Submit(honest);
        }
        BOOST_CHECK(Tip() == index);
        BOOST_CHECK(WITH_LOCK(::cs_main, return index->m_ext->sig == honest.ext.sig));
        CBlock read;
        BOOST_REQUIRE(Chainman().m_blockman.ReadBlock(read, *index));
        BOOST_CHECK(read.ext == honest.ext);
        // A forged one again changes nothing: a mutated block.
        BlockValidationState state;
        BOOST_CHECK(!WITH_LOCK(::cs_main, return Chainman().AcceptBlock(std::make_shared<const CBlock>(forged), state, nullptr, true, nullptr, nullptr, true)));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-extension");
        BOOST_CHECK(Tip() == index);
        ++last;
    }
    Take(PosBlock(last));
}

BOOST_FIXTURE_TEST_CASE(evidence_other_network, FastBlocksSetup)
{
    // A header signed on another network (another genesis block; the slots of the networks line up)
    // is no signature here: paired with one of this network, no proof that the leader signed twice.
    StartPos();
    const CBlock local{PosBlock(1)};
    const int64_t slot{pos::SlotOf(local.nTime, Pos())};
    const CKey& leader{Leader(slot)};
    const uint256 elsewhere{CChainParams::TestNet()->GetConsensus().hashGenesisBlock};
    BOOST_REQUIRE(elsewhere != Genesis());
    CBlock foreign{local};
    SetCoinbase(foreign, [](CMutableTransaction& tx) { tx.vout.emplace_back(0, CScript() << OP_RETURN); });
    node::RegenerateCommitments(foreign, Chainman());
    BOOST_REQUIRE(pos::Producer::Sign(foreign, leader, elsewhere));
    BOOST_REQUIRE(foreign.GetHash() != local.GetHash() && foreign.nTime == local.nTime);
    BOOST_CHECK(pos::BlockSigHash(local, elsewhere) != pos::BlockSigHash(local, Genesis()));
    const pos::DoubleProposal proof{.a = local, .b = foreign};
    const uint64_t anchors{Fast().anchors};
    const sidechain::StoreView& view{ActiveView()};
    std::string reason;
    {
        LOCK(::cs_main);
        BOOST_CHECK(!Hooks().CheckChainEvidence(proof, *Tip(), slot, view, anchors, reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-sig");
    }
    BOOST_CHECK(!Hooks().AddEvidence(proof, reason));
    BOOST_CHECK(Hooks().Evidence().All().empty());
    // Signed for this network: a proof.
    CBlock twice{foreign};
    BOOST_REQUIRE(pos::Producer::Sign(twice, leader, Genesis()));
    LOCK(::cs_main);
    BOOST_CHECK(Hooks().CheckChainEvidence({.a = local, .b = twice}, *Tip(), slot, view, anchors, reason));
}

BOOST_AUTO_TEST_CASE(note_signed_per_leader)
{
    // The headers noted to find proofs, by slot and signer: a header of another leader of the slot
    // (another branch's, or one an earlier record gave) does not hide the two of this one.
    CBlockHeader a, b1, b2;
    a.nVersion = b1.nVersion = b2.nVersion = pos::POS_BLOCK_BIT;
    a.nTime = b1.nTime = b2.nTime = 1000;
    a.nNonce = 1;
    b1.nNonce = 2;
    b2.nNonce = 3;
    const uint256 leader_a{1}, leader_b{2};
    pos::EvidencePool pool;
    BOOST_CHECK(!pool.NoteSigned(a, 7, leader_a));
    BOOST_CHECK(!pool.NoteSigned(b1, 7, leader_b));
    const auto proof{pool.NoteSigned(b2, 7, leader_b)};
    BOOST_REQUIRE(proof);
    BOOST_CHECK((proof->a.GetHash() == b1.GetHash() && proof->b.GetHash() == b2.GetHash()) || (proof->a.GetHash() == b2.GetHash() && proof->b.GetHash() == b1.GetHash()));
    // The same header again is no proof.
    BOOST_CHECK(!pool.NoteSigned(b1, 7, leader_b));
    BOOST_CHECK(!pool.NoteSigned(a, 7, leader_a));
}

BOOST_FIXTURE_TEST_CASE(evidence_replay_checked_first, FastBlocksSetup)
{
    // A proof the node keeps already costs no signature check: one sent again is found in the pool
    // first, and its peer is not blamed (whatever the node can tell of the leader by then: here, with
    // no chainstate to tell it).
    StartPos();
    const auto [first, second]{TwoBlocks(1)};
    std::string reason;
    const auto offence{Hooks().AddEvidence({.a = first, .b = second}, reason)};
    BOOST_REQUIRE_MESSAGE(offence, reason);
    Hooks().Attach(nullptr);
    const auto again{Hooks().AddEvidence({.a = second, .b = first}, reason)};
    DataStream stream{};
    stream << pos::DoubleProposal{.a = first, .b = second};
    std::string misbehaving;
    BOOST_CHECK(Hooks().ProcessMessage(3, pos::MSG_EVIDENCE, stream, misbehaving));
    Hooks().Attach(&Chainman());
    BOOST_CHECK(again && *again == *offence);
    BOOST_CHECK_EQUAL(misbehaving, "");
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 1U);
}

BOOST_FIXTURE_TEST_CASE(unbond_replay_after_topup, FastBlocksSetup)
{
    // The owner signs an unbond of all, which is not mined (its transaction dropped from the
    // mempool, say), then adds to the bond and goes on validating. The unbond it signed is public: a
    // top-up moves the validator's unbond sequence on, so that nobody can mine it later.
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    CKey validator;
    validator.MakeNewKey(/*fCompressed=*/true);
    const uint256 pubkey{XOnly(validator)};
    const CAmount min{Pos().min_stake};
    Accept(Pay(coin, spent, {BondOut(validator, 2 * min, Owner(key), &key, coin)}));
    Take(PosBlock(1));
    const uint32_t sequence{Validator(pubkey)->unbond_seq};
    const CTxOut unbond_all{UnbondOut(validator, pos::UNBOND_ALL, sequence, key)};
    BOOST_CHECK_EQUAL(StakingRules(TxWith({unbond_all})), "");
    Accept(Pay(coin, spent, {BondOut(validator, min, Owner(key), &key, coin)}));
    Take(PosBlock(2));
    BOOST_CHECK_EQUAL(Validator(pubkey)->bonded, 3 * min);
    // A stranger's transaction with it: refused, in the mempool and in blocks.
    BOOST_CHECK_EQUAL(StakingRules(TxWith({unbond_all})), "bad-pos-unbond-sequence");
    {
        COutPoint c{coin};
        CTxOut s{spent};
        BOOST_CHECK_EQUAL(Refused(Pay(c, s, {unbond_all})), "bad-pos-unbond-sequence");
    }
    // The owner's new one: taken.
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(validator, pos::UNBOND_ALL, Validator(pubkey)->unbond_seq, key)})), "");
}

BOOST_FIXTURE_TEST_CASE(first_bond_signed, FastBlocksSetup)
{
    // The proof of possession of a first bond is public as soon as its transaction is: a stranger
    // could copy it into a bond of a satoshi of its own, mined first, and the owner's bond after it
    // would need a signature it does not carry. The owner signs first bonds too.
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    CKey validator, stranger;
    validator.MakeNewKey(/*fCompressed=*/true);
    stranger.MakeNewKey(/*fCompressed=*/true);
    const uint256 pubkey{XOnly(validator)};
    const CAmount min{Pos().min_stake};
    const CTxOut pending{BondOut(validator, 2 * min, Owner(key), &key, coin)};
    const pos::Bond copied{*pos::ParseBond(pending)};
    BOOST_CHECK_EQUAL(StakingRules(TxWith({CTxOut{1, pos::BondScript(pubkey, Owner(key), copied.pop)}})), "bad-pos-bond-owner-sig");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, 1, Owner(key), &stranger)})), "bad-pos-bond-owner-sig");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({CTxOut{1, pos::BondScript(pubkey, Owner(key), copied.pop, copied.owner_sig)}})), "bad-pos-bond-owner-sig");
    Accept(Pay(coin, spent, {pending}));
    Take(PosBlock(1));
    BOOST_CHECK_EQUAL(Validator(pubkey)->bonded, 2 * min);
    BOOST_CHECK(Validator(pubkey)->owner == Owner(key));
}

BOOST_FIXTURE_TEST_CASE(staking_seen_from_the_state, NoValidatorsSetup)
{
    // Blocks may be at most 10 minutes ahead of the clock while the network has blocks of stake, and
    // two hours before (the template's). That is the state's: a node that starts on a chain whose
    // newest anchor made an active set knows it before the next anchor; a reorg back below it, not.
    BOOST_CHECK_EQUAL(Hooks().MaxFutureBlockTime(), MAX_FUTURE_BLOCK_TIME);
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    Accept(Pay(coin, spent, {BondOut(keys[0], Pos().min_stake, Owner(key), &key, coin)}));
    MineBmm(BmmBlock());
    const CBlock anchor{BmmBlock(/*use_mempool=*/false)};
    MineBmm(anchor);
    BOOST_CHECK_EQUAL(Hooks().MaxFutureBlockTime(), pos::MAX_FUTURE_BLOCK_TIME_POS);
    // A node that starts now (hooks of their own over the same chain).
    pos::FastBlocksHooks restarted{*params};
    restarted.Attach(&Chainman());
    BOOST_CHECK_EQUAL(restarted.MaxFutureBlockTime(), pos::MAX_FUTURE_BLOCK_TIME_POS);
    // Back below the anchor that made the set.
    BlockValidationState state;
    BOOST_REQUIRE(Chain().InvalidateBlock(state, Index(anchor.GetHash())));
    BOOST_CHECK_EQUAL(Hooks().MaxFutureBlockTime(), MAX_FUTURE_BLOCK_TIME);
    BOOST_CHECK_EQUAL(restarted.MaxFutureBlockTime(), MAX_FUTURE_BLOCK_TIME);
    restarted.Attach(nullptr);
}

BOOST_FIXTURE_TEST_CASE(producer_retries_before_signing, SeedSetup)
{
    // A slot whose block cannot be made at first, for a reason that comes before the slashing
    // protection notes it (nothing signed), is tried again while the slot lasts: here the mainchain
    // block that seeds the epoch comes just after the slot starts. (So too a block that arrives as the
    // slot starts: the node still taking it in, or the tip moving while the block was built.)
    StartPos();
    const int64_t epoch{pos::EpochOf(base_slot, Pos()) + 1};
    const int i{static_cast<int>(epoch * Pos().slots_per_epoch - base_slot)};
    const int64_t slot{Slot(i)};
    const uint256 before{Tip()->GetBlockHash()};
    producer->Start();
    BOOST_REQUIRE(WaitFor([&] { return producer->GetStatus().tries >= 1; }));
    BOOST_CHECK(Tip()->GetBlockHash() == before);
    // The seed's mainchain block, while the clock is still in the slot.
    Main([&](sidechain::MainBlock& block) { block.time = GetTime() + 1; });
    BOOST_CHECK_MESSAGE(WaitFor([&] { return Tip()->GetBlockHash() != before; }), "no block for the slot after the seed came");
    producer->Stop();
    BOOST_CHECK_EQUAL(pos::SlotOf(Tip()->nTime, Pos()), slot);
    BOOST_CHECK_EQUAL(producer->GetStatus().produced, 1U);
    // Not a failure: the slot was made.
    BOOST_CHECK_EQUAL(producer->GetStatus().failed, 0U);
    // A failure after the slot is noted is not tried again (a second block would be a second signature).
    std::string error;
    pos::Producer::Retry retry{pos::Producer::Retry::COUNTED};
    BOOST_CHECK(!producer->Produce(slot, error, &retry));
    BOOST_CHECK(retry != pos::Producer::Retry::NO); // (the chain has a block of the slot: before the signature)
    producer->SetExtraCheckForTesting([](const CBlock&) -> std::optional<std::string> { return "refused by the test"; });
    BOOST_CHECK(!producer->Produce(Slot(i + 1), error, &retry));
    BOOST_CHECK_EQUAL(error, "the block is not valid: refused by the test");
    BOOST_CHECK(retry == pos::Producer::Retry::NO);
}

BOOST_FIXTURE_TEST_CASE(protection_next_to_the_key_file, FastBlocksSetup)
{
    // The slashing protection lives next to the validator key file, not in the data directory: wiping
    // the data directory to sync again keeps the key, and must keep the slots it signed. The file an
    // older version kept in the data directory counts too.
    ArgsManager& args{*m_node.args};
    const fs::path dir{args.GetDataDirBase() / "validator_keys"};
    fs::create_directories(dir);
    const fs::path key_file{dir / "validator.key"};
    {
        std::ofstream out{key_file.std_path()};
        out << HexStr(keys[2]) << "\n";
    }
    const fs::path legacy{args.GetDataDirNet() / "validator_protection.dat"};
    {
        std::ofstream out{legacy.std_path()};
        out << "fastblocks-slashing-protection 1\n" << HexStr(XOnly(keys[2])) << " 100\n";
    }
    args.ForceSetArg("-validatorkeyfile", fs::PathToString(key_file));
    std::string error;
    {
        const auto made{pos::Producer::FromArgs(m_node, Hooks(), error)};
        BOOST_REQUIRE_MESSAGE(made, error);
        BOOST_CHECK_EQUAL(made->LastSignedSlot(), 100);
        // Without a wallet and -validatorpayout: the key's own address.
        BOOST_CHECK(made->Payout() == GetScriptForDestination(WitnessV0KeyHash{keys[2].GetPubKey()}));
    }
    BOOST_CHECK(fs::exists(dir / "validator.key.protection"));
    // The data directory wiped: the slot is still known.
    fs::remove(legacy);
    {
        const auto made{pos::Producer::FromArgs(m_node, Hooks(), error)};
        BOOST_REQUIRE_MESSAGE(made, error);
        BOOST_CHECK_EQUAL(made->LastSignedSlot(), 100);
    }
    // A higher slot in an old file still counts (the highest of each key).
    {
        std::ofstream out{legacy.std_path()};
        out << "fastblocks-slashing-protection 1\n" << HexStr(XOnly(keys[2])) << " 150\n";
    }
    {
        const auto made{pos::Producer::FromArgs(m_node, Hooks(), error)};
        BOOST_REQUIRE_MESSAGE(made, error);
        BOOST_CHECK_EQUAL(made->LastSignedSlot(), 150);
    }
    fs::remove(legacy);
    {
        const auto made{pos::Producer::FromArgs(m_node, Hooks(), error)};
        BOOST_REQUIRE_MESSAGE(made, error);
        BOOST_CHECK_EQUAL(made->LastSignedSlot(), 150);
    }
}

BOOST_FIXTURE_TEST_CASE(mempool_drops_stale_staking_txs, FastBlocksSetup)
{
    // A bond or an unbond is checked against the tip when it enters the mempool. A block can then make
    // it one no block will take: it must leave the mempool, or it holds its inputs (and the wallet that
    // made it) until the mempool expires it, two weeks later.
    StartPos();
    COutPoint a, b, c, d;
    CTxOut spent_a{Fund(COIN, a)}, spent_b{Fund(COIN, b)}, spent_c{Fund(COIN, c)}, spent_d{Fund(COIN, d)};
    CKey validator, newcomer, other_owner;
    validator.MakeNewKey(/*fCompressed=*/true);
    newcomer.MakeNewKey(/*fCompressed=*/true);
    other_owner.MakeNewKey(/*fCompressed=*/true);
    const CAmount min{Pos().min_stake};
    Accept(Pay(a, spent_a, {BondOut(validator, 3 * min, Owner(key), &key, a)}));
    // (After the blocks of the deposits.)
    Take(PosBlock(10));
    BOOST_REQUIRE_EQUAL(m_node.mempool->size(), 0U);
    // Two unbonds the owner signed with the same sequence (the second, say, after the first seemed
    // lost), each valid on the tip: a block takes one, the other can never be mined.
    const uint32_t sequence{Validator(XOnly(validator))->unbond_seq};
    const CMutableTransaction first{Pay(a, spent_a, {UnbondOut(validator, min, sequence, key)})};
    const CMutableTransaction second{Pay(b, spent_b, {UnbondOut(validator, 2 * min, sequence, key)})};
    Accept(first);
    Accept(second);
    // Two first bonds of a key naming two owners (a wallet that bonded twice in a block): once one
    // is mined, the other names the wrong owner.
    const CMutableTransaction bond_x{Pay(c, spent_c, {BondOut(newcomer, min, Owner(key), &key, c)})};
    const CMutableTransaction bond_y{Pay(d, spent_d, {BondOut(newcomer, min, Owner(other_owner), &other_owner, d)})};
    Accept(bond_x);
    Accept(bond_y);
    BOOST_REQUIRE_EQUAL(m_node.mempool->size(), 4U);
    const CBlock block{PosBlock(11)};
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 3U);
    Take(block);
    // One of each pair was mined; the other left the mempool with the block.
    BOOST_CHECK_EQUAL(m_node.mempool->size(), 0U);
    BOOST_CHECK_EQUAL(Validator(XOnly(validator))->unbond_seq, sequence + 1);
    BOOST_CHECK(Validator(XOnly(newcomer))->owner == Owner(key) || Validator(XOnly(newcomer))->owner == Owner(other_owner));
    // The inputs of those not mined are free again.
    for (const CMutableTransaction* tx : {&first, &second, &bond_x, &bond_y}) {
        const bool mined{std::any_of(block.vtx.begin(), block.vtx.end(), [&](const CTransactionRef& in) { return in->GetHash() == tx->GetHash(); })};
        if (mined) continue;
        BOOST_CHECK(!WITH_LOCK(m_node.mempool->cs, return m_node.mempool->GetConflictTx(tx->vin.at(0).prevout)));
    }
}

#ifdef ENABLE_WALLET
BOOST_FIXTURE_TEST_CASE(wallet_finality_on_its_own_chain, FastBlocksSetup)
{
    // The finality the wallet shows is that of its own chain, the one that ends at its last block: in
    // a reorg the node's tip moves first, and the anchors of the new branch must not make a payment
    // the reorg took out look anchored or final. (The wallet here never hears of the reorg, as one
    // that has not caught up with the validation queue.)
    StartPos();
    Take(PosBlock(1));
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    CKey wallet_key;
    wallet_key.MakeNewKey(/*fCompressed=*/true);
    const CMutableTransaction payment{Pay(coin, spent, {CTxOut{COIN / 2, GetScriptForDestination(WitnessV0KeyHash{wallet_key.GetPubKey()})}})};
    Accept(payment);
    const CBlockIndex* fork{Tip()};
    const CBlock paying{PosBlock(2)};
    Take(paying);
    const int height{Tip()->nHeight};
    std::shared_ptr<wallet::CWallet> wallet{wallet::CreateSyncedWallet(*m_node.chain, Chain().m_chain, wallet_key)};
    wallet::WalletContext context;
    context.chain = m_node.chain.get();
    context.args = m_node.args;
    BOOST_REQUIRE(wallet::AddWallet(context, wallet));
    const auto call{[&](RPCMethod (*method)(), UniValue params) {
        JSONRPCRequest request;
        request.context = &context;
        request.params = std::move(params);
        return method().HandleRequest(request);
    }};
    // On its chain: soft (no anchor covers the block yet).
    BOOST_CHECK_EQUAL(call(wallet::getbalances, UniValue{UniValue::VARR})["mine"]["finality"]["soft"].getValStr(), "0.50000000");
    // Another branch from below the payment's block, without it, with an anchor that covers its height.
    BlockValidationState state;
    BOOST_REQUIRE(Chain().InvalidateBlock(state, Index(paying.GetHash())));
    BOOST_REQUIRE(Tip() == fork);
    WITH_LOCK(m_node.mempool->cs, m_node.mempool->removeRecursive(CTransaction{payment}, MemPoolRemovalReason::CONFLICT));
    Take(PosBlock(3));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, height);
    AckTip(4);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return pos::AnchoredHeights(Hooks(), *Tip(), Pos().final_anchors)).at(0), height);
    // The wallet still has the payment at that height, in the block of the other branch.
    BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->GetLastBlockHash()) == paying.GetHash());
    const UniValue finality{call(wallet::getbalances, UniValue{UniValue::VARR})["mine"]["finality"]};
    BOOST_CHECK_EQUAL(finality["soft"].getValStr(), "0.50000000");
    BOOST_CHECK_EQUAL(finality["anchored"].getValStr(), "0.00000000");
    BOOST_CHECK_EQUAL(finality["final"].getValStr(), "0.00000000");
    UniValue params{UniValue::VARR};
    params.push_back(payment.GetHash().GetHex());
    BOOST_CHECK_EQUAL(call(wallet::gettransaction, params)["finality"].get_str(), "soft");
    BOOST_REQUIRE(wallet::RemoveWallet(context, wallet, /*load_on_start=*/std::nullopt));
}
#endif // ENABLE_WALLET

BOOST_AUTO_TEST_SUITE_END()
