// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/stakingtests.h>

#include <addresstype.h>
#include <consensus/merkle.h>
#include <interfaces/chain.h>
#include <interfaces/mining.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <kernel/chainparams.h>
#include <key.h>
#include <key_io.h>
#include <node/context.h>
#include <pos/hooks.h>
#include <pos/producer.h>
#include <pos/staking.h>
#include <pow.h>
#include <qt/clientmodel.h>
#include <qt/noderpc.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/stakingpage.h>
#include <qt/walletmodel.h>
#include <rpc/server.h>
#include <script/descriptor.h>
#include <script/signingprovider.h>
#include <sidechain/mainchain.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <validation.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QTableWidget>
#include <QTemporaryDir>

#include <fstream>
#include <memory>
#include <string>
#include <vector>

using wallet::AddWallet;
using wallet::CreateMockableWalletDatabase;
using wallet::CWallet;
using wallet::RemoveWallet;
using wallet::WalletContext;
using wallet::WalletDescriptor;
using wallet::WALLET_FLAG_DESCRIPTORS;

namespace {

CKey ValidatorKey(uint8_t i)
{
    std::vector<unsigned char> secret(32, 0);
    secret[0] = 0x11;
    secret[31] = i;
    CKey key;
    key.Set(secret.begin(), secret.end(), /*fCompressedIn=*/true);
    return key;
}

std::string XOnlyHex(const CKey& key)
{
    const XOnlyPubKey xonly{key.GetPubKey()};
    return HexStr(std::span<const unsigned char>{xonly.begin(), xonly.end()});
}

/** The FastBlocks hooks, installed before the chainstate is made and removed after it is gone. */
struct InstalledHooks {
    std::unique_ptr<const CChainParams> params;
    std::string validators;
    InstalledHooks()
    {
        ECC_Context ecc;
        CChainParams::RegTestOptions options;
        options.sidechain_slot = 5;
        for (uint8_t i{1}; i <= 3; ++i) {
            const CKey key{ValidatorKey(i)};
            validators += (validators.empty() ? "" : ",") + XOnlyHex(key);
            const XOnlyPubKey xonly{key.GetPubKey()};
            options.pos_validators.push_back({.pubkey = uint256{std::span<const unsigned char>{xonly.begin(), xonly.end()}}, .stake = 1});
        }
        params = CChainParams::RegTest(options);
        pos::InstallHooks(*params);
    }
    ~InstalledHooks() { pos::UninstallHooks(); }
};

/** A FastBlocks node with the first of the three validator keys, and a record of the mainchain the test writes. */
struct StakingChain : InstalledHooks {
    QTemporaryDir key_dir;
    std::vector<std::string> args;
    std::vector<const char*> arg_pointers;
    std::unique_ptr<TestingSetup> test;
    CKey key;

    StakingChain()
    {
        const QString key_file{key_dir.filePath("validator.key")};
        // The secret of ValidatorKey(1).
        std::ofstream{key_file.toStdString()} << "11" << std::string(60, '0') << "01\n";
        args = {"-sidechainslot=5", "-posvalidators=" + validators, "-validatorkeyfile=" + key_file.toStdString()};
        for (const std::string& arg : args) arg_pointers.push_back(arg.c_str());
        test = std::make_unique<TestingSetup>(ChainType::REGTEST, TestOpts{.extra_args = arg_pointers});
        Hooks().Attach(test->m_node.chainman.get());
        key = GenerateRandomKey();
        // The first block of the mainchain, which no sidechain acts on.
        Main();
    }
    ~StakingChain()
    {
        Hooks().StopServices(test->m_node);
        Hooks().Attach(nullptr);
        test.reset();
    }

    pos::FastBlocksHooks& Hooks() { return *Assert(pos::InstalledHooks()); }
    sidechain::Mainchain& Record() { return *Assert(test->m_node.chainman->m_mainchain); }

    void Main(const std::function<void(sidechain::MainBlock&)>& fill = {})
    {
        sidechain::MainBlock block;
        block.hash = uint256{static_cast<uint8_t>(Record().Height() + 2)};
        block.prev_hash = Record().TipHash();
        block.time = GetTime();
        if (fill) fill(block);
        QVERIFY(Record().Append(block));
    }

    /** A merged mined block, committed to and connected. */
    CBlock Mine()
    {
        auto block_template{interfaces::MakeMining(test->m_node)->createNewBlock({.use_mempool = false, .coinbase_output_script = CScript() << OP_TRUE}, /*cooldown=*/false)};
        CBlock block{block_template->getBlock()};
        block.hashMerkleRoot = BlockMerkleRoot(block);
        while (!CheckProofOfWork(block.GetHash(), block.nBits, test->m_node.chainman->GetConsensus())) ++block.nNonce;
        Main([&](sidechain::MainBlock& b) { b.bmm = block.GetHash(); });
        test->m_node.chainman->ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/nullptr);
        return block;
    }
};

/**
 * Press a button of every message box that pops up while `action` runs.
 * @return the texts of the message boxes
 */
QStringList WithMessageBoxes(QMessageBox::StandardButton button, const std::function<void()>& action)
{
    QStringList texts;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [button, &texts] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* box{qobject_cast<QMessageBox*>(widget)};
            if (!box || !box->isVisible()) continue;
            texts << box->text();
            // An information box only has OK.
            QAbstractButton* choice{box->button(button)};
            if (!choice) choice = box->button(QMessageBox::Ok);
            if (choice) choice->click();
        }
    });
    timer.start(20);
    action();
    timer.stop();
    return texts;
}

void SaveScreenshot(QWidget& widget, const QString& name)
{
    const QString dir{qEnvironmentVariable("CHAINS_GUI_SCREENSHOT_DIR")};
    if (dir.isEmpty()) return;
    QDir().mkpath(dir);
    widget.grab().save(QDir(dir).filePath(name + ".png"));
}

} // namespace

void StakingTests::stakingTests()
{
    // AppTests shut a node down before, which stopped the calls off the GUI thread.
    NodeRpc::Restart();
    StakingChain chain;
    node::NodeContext& context{chain.test->m_node};

    // 10 coins for the wallet's key, from a deposit in a merged mined block; a second one anchors it.
    chain.Main([&](sidechain::MainBlock& b) {
        sidechain::MainDeposit deposit;
        deposit.destination = EncodeDestination(WitnessV0KeyHash{chain.key.GetPubKey()});
        deposit.amount = 10 * COIN;
        b.deposits.push_back(deposit);
    });
    const CBlock funding{chain.Mine()};
    QVERIFY(funding.vtx[0]->vout.size() >= 2);
    QCOMPARE(funding.vtx[0]->vout[1].nValue, 10 * COIN);
    chain.Mine();
    QCOMPARE(WITH_LOCK(::cs_main, return context.chainman->ActiveChain().Height()), 2);

    auto wallet_loader = interfaces::MakeWalletLoader(*context.chain, *Assert(context.args));
    context.wallet_loader = wallet_loader.get();
    m_node.setContext(&context);
    wallet_loader->registerRpcs();
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(context.chain.get(), "", CreateMockableWalletDatabase());
    {
        LOCK(wallet->cs_wallet);
        wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        // What the wallet makes goes to the node's mempool (the bonds below).
        wallet->SetBroadcastTransactions(true);
        wallet->SetupDescriptorScriptPubKeyMans();
        FlatSigningProvider provider;
        std::string error;
        auto descs = Parse("combo(" + EncodeSecret(chain.key) + ")", provider, error, /*require_checksum=*/false);
        QVERIFY(descs.size() == 1);
        WalletDescriptor w_desc(std::move(descs.at(0)), 0, 0, 1, 1);
        QVERIFY(wallet->AddWalletDescriptor(w_desc, provider, "", false));
        const CBlockIndex* tip{WITH_LOCK(::cs_main, return context.chainman->ActiveChain().Tip())};
        wallet->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
    }
    {
        wallet::WalletRescanReserver reserver(*wallet);
        reserver.reserve();
        const auto result{wallet->ScanForWalletTransactions(context.chainman->GetParams().GetConsensus().hashGenesisBlock, /*start_height=*/0, /*max_height=*/{}, reserver, /*save_progress=*/false)};
        QCOMPARE(result.status, CWallet::ScanResult::SUCCESS);
    }
    WalletContext& wallet_context = *m_node.walletLoader().context();
    AddWallet(wallet_context, wallet);
    struct Cleanup {
        std::function<void()> fn;
        ~Cleanup() { fn(); }
    } cleanup{[&] {
        NodeRpc::Stop();
        RemoveWallet(wallet_context, wallet, /*load_on_start=*/std::nullopt);
    }};

    // The block producer of the validator key, as the node starts it.
    chain.Hooks().StartServices(context);
    QVERIFY2(chain.Hooks().ProducerError().empty(), chain.Hooks().ProducerError().c_str());
    QVERIFY(chain.Hooks().GetProducer());

    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    OptionsModel options_model(m_node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    ClientModel client_model(m_node, &options_model);
    WalletModel wallet_model(interfaces::MakeWallet(wallet_context, wallet), client_model, platform_style.get());

    StakingPage page(platform_style.get());
    page.resize(1000, 700);
    page.setClientModel(&client_model);
    page.setWalletModel(&wallet_model);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));

    auto* summary{page.findChild<QLabel*>("stakingSummary")};
    auto* status{page.findChild<QLabel*>("stakingStatus")};
    auto* finality{page.findChild<QLabel*>("finalityStatus")};
    auto* start{page.findChild<QPushButton*>("stakingStart")};
    auto* stop{page.findChild<QPushButton*>("stakingStop")};
    auto* validators{page.findChild<QTableWidget*>("validators")};
    auto* anchors{page.findChild<QTableWidget*>("anchors")};
    QVERIFY(summary && status && finality && start && stop && validators && anchors);

    // The chain, the validator, the validators of the epoch.
    QVERIFY2(summary->text().contains("Blocks of stake come every 1 s"), qPrintable(summary->text()));
    QVERIFY2(status->text().contains("Staking is on"), qPrintable(status->text()));
    QVERIFY(status->text().contains(QString::fromStdString(XOnlyHex(ValidatorKey(1)).substr(0, 10))));
    QVERIFY(status->text().contains("Stake 1 of 3"));
    QVERIFY(!start->isEnabled());
    QVERIFY(stop->isEnabled());
    QCOMPARE(validators->rowCount(), 3);
    int local{0};
    for (int row{0}; row < 3; ++row) local += validators->item(row, 3)->text() == "yes";
    QCOMPARE(local, 1);

    // Finality: the first merged mined block is anchored by the second, so the deposit is final
    // (one anchor makes a block final on regtest).
    QVERIFY2(finality->text().contains("final up to block 1"), qPrintable(finality->text()));
    QCOMPARE(page.findChild<QLabel*>("balanceFinal")->text(), QString{"10.00000000"});
    QCOMPARE(page.findChild<QLabel*>("balanceSoft")->text(), QString{"0.00000000"});
    QCOMPARE(page.findChild<QLabel*>("balanceAnchored")->text(), QString{"0.00000000"});
    QVERIFY(anchors->rowCount() >= 2);
    QCOMPARE(anchors->item(0, 1)->text(), QString{"merged mined block"});

    // Staking off, from the page: the producer stops; and on again.
    stop->click();
    QVERIFY(!chain.Hooks().GetProducer()->Enabled());
    QVERIFY2(status->text().contains("Staking is off"), qPrintable(status->text()));
    QVERIFY(start->isEnabled());
    QVERIFY(!stop->isEnabled());
    start->click();
    QVERIFY(chain.Hooks().GetProducer()->Enabled());
    QVERIFY(status->text().contains("Staking is on"));

    // The validator makes blocks of stake at its slots; the page shows them as it refreshes.
    QTRY_VERIFY_WITH_TIMEOUT(chain.Hooks().GetProducer()->GetStatus().produced > 0, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(!status->text().contains("Made 0 block"), 10000);
    QVERIFY(!page.findChild<QLabel*>("stakingError")->text().contains("getstakinginfo"));
    SaveScreenshot(page, "staking-page");

    // Bonds: the wallet bonds a coin to this node's validator key (the node signs the proof of
    // possession); it is in the list of the wallet's bonds once a block of stake has it. Asked first.
    auto* bond_key{page.findChild<QLineEdit*>("bondKey")};
    auto* bond_amount{page.findChild<QLineEdit*>("bondAmount")};
    auto* bond_button{page.findChild<QPushButton*>("bondButton")};
    auto* unbond_button{page.findChild<QPushButton*>("unbondButton")};
    auto* bonds{page.findChild<QTableWidget*>("bonds")};
    QVERIFY(bond_key && bond_amount && bond_button && unbond_button && bonds);
    QCOMPARE(bonds->rowCount(), 0);
    bond_amount->setText("1");
    // Said no: nothing happens.
    QStringList texts{WithMessageBoxes(QMessageBox::No, [&] { bond_button->click(); })};
    QCOMPARE(texts.size(), 1);
    QVERIFY2(texts[0].startsWith("Bond 1 to the validator key"), qPrintable(texts[0]));
    QCOMPARE(bond_amount->text(), QString{"1"});
    texts = WithMessageBoxes(QMessageBox::Yes, [&] { bond_button->click(); });
    QCOMPARE(texts.size(), 2);
    QVERIFY2(texts[1].contains("The bond is in transaction"), qPrintable(texts.join(" / ")));
    QVERIFY2(page.findChild<QLabel*>("stakingError")->text().isEmpty(), qPrintable(page.findChild<QLabel*>("stakingError")->text()));
    QTRY_VERIFY_WITH_TIMEOUT(bonds->rowCount() == 1 && bonds->item(0, 1)->text() == "1.00000000", 30000);
    QCOMPARE(WITH_LOCK(::cs_main, return pos::GetStakeState(context.chainman->ActiveChainstate().SideState().View()).bonded), COIN);
    QCOMPARE(bonds->item(0, 0)->toolTip(), QString::fromStdString(XOnlyHex(ValidatorKey(1))));
    // Unbond all of it (no amount): unbonding, paid some anchors later.
    QVERIFY(bond_amount->text().isEmpty());
    texts = WithMessageBoxes(QMessageBox::Yes, [&] { unbond_button->click(); });
    QCOMPARE(texts.size(), 2);
    QVERIFY2(texts[0].contains("all of the bond"), qPrintable(texts[0]));
    QVERIFY2(texts[1].contains("The unbond of 1.00000000"), qPrintable(texts.join(" / ")));
    QTRY_VERIFY_WITH_TIMEOUT(bonds->item(0, 1)->text() == "0.00000000" && bonds->item(0, 2)->text() == "1.00000000", 30000);
    SaveScreenshot(page, "staking-page-bonds");

    // Without a wallet, the rest of the page still works.
    page.setWalletModel(nullptr);
    page.refresh();
    QVERIFY(page.findChild<QLabel*>("stakingError")->text().isEmpty());
    QVERIFY(!bond_button->isEnabled());
    QCOMPARE(bonds->rowCount(), 0);
    page.hide();
    chain.Hooks().StopServices(context);
}
