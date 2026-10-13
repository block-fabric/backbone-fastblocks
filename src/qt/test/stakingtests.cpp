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
#include <qt/blockexplorer.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/noderpc.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/stakingpage.h>
#include <qt/theme.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>
#include <qt/transactionview.h>
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
#include <QSignalSpy>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
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

    /** A merged mined block, committed to and connected; its first output (the anchor's payee) pays `payee`. */
    CBlock Mine(const CScript& payee = CScript() << OP_TRUE)
    {
        auto block_template{interfaces::MakeMining(test->m_node)->createNewBlock({.use_mempool = false, .coinbase_output_script = payee}, /*cooldown=*/false)};
        CBlock block{block_template->getBlock()};
        block.hashMerkleRoot = BlockMerkleRoot(block);
        while (!CheckProofOfWork(block.GetHash(), block.nBits, test->m_node.chainman->GetConsensus())) ++block.nNonce;
        Main([&](sidechain::MainBlock& b) { b.bmm = block.GetHash(); });
        test->m_node.chainman->ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/nullptr);
        return block;
    }
};

/**
 * Press a button of every message box that pops up while `action` runs, and after it until there were
 * `wait_for` of them.
 * @return the texts of the message boxes
 */
QStringList WithMessageBoxes(QMessageBox::StandardButton button, const std::function<void()>& action, int wait_for = 0)
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
    // The boxes that tell the outcome of a call off the GUI thread come later.
    for (int waited{0}; texts.size() < wait_for && waited < 30000; waited += 20) QTest::qWait(20);
    timer.stop();
    return texts;
}

/** The rows of the wallet's transaction list of a type. */
QList<QModelIndex> RowsOfType(TransactionTableModel& model, TransactionRecord::Type type)
{
    QList<QModelIndex> rows;
    for (int row{0}; row < model.rowCount({}); ++row) {
        const QModelIndex index{model.index(row, TransactionTableModel::Type)};
        if (index.data(TransactionTableModel::TypeRole).toInt() == type) rows << index;
    }
    return rows;
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
        // And the address the producer pays its share of blocks of stake to (the validator key's own,
        // without -validatorpayout): the wallet sees what this node's blocks of stake earn.
        FlatSigningProvider validator_provider;
        auto validator_descs = Parse("combo(" + EncodeSecret(ValidatorKey(1)) + ")", validator_provider, error, /*require_checksum=*/false);
        QVERIFY(validator_descs.size() == 1);
        WalletDescriptor validator_desc(std::move(validator_descs.at(0)), 0, 0, 1, 1);
        QVERIFY(wallet->AddWalletDescriptor(validator_desc, validator_provider, "", false));
        const CBlockIndex* tip{WITH_LOCK(::cs_main, return context.chainman->ActiveChain().Tip())};
        wallet->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
    }
    {
        wallet::WalletRescanReserver reserver(*wallet);
        reserver.reserve();
        const auto result{wallet->ScanForWalletTransactions(context.chainman->GetParams().GetConsensus().hashGenesisBlock, /*start_height=*/0, /*max_height=*/{}, reserver, /*save_progress=*/false)};
        QCOMPARE(result.status, CWallet::ScanResult::SUCCESS);
    }
    // The wallet follows the chain from here on: the blocks of stake that pay it, the anchors.
    wallet->m_chain_notifications_handler = context.chain->handleNotifications(wallet);
    WalletContext& wallet_context = *m_node.walletLoader().context();
    AddWallet(wallet_context, wallet);
    struct Cleanup {
        std::function<void()> fn;
        ~Cleanup() { fn(); }
    } cleanup{[&] {
        NodeRpc::Stop();
        wallet->m_chain_notifications_handler.reset();
        context.validation_signals->SyncWithValidationInterfaceQueue();
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
    page.resize(1100, 900);
    page.setClientModel(&client_model);
    page.setWalletModel(&wallet_model);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));
    // Fetched off the GUI thread.
    QSignalSpy refreshed(&page, &StakingPage::refreshed);
    const auto wait_refresh{[&] {
        refreshed.clear();
        page.refresh();
        QVERIFY(refreshed.wait(30000));
        // One that was asked for while another ran comes after it.
        if (refreshed.count() == 1) refreshed.wait(200);
    }};
    wait_refresh();

    const auto label{[&](const char* name) {
        auto* found{page.findChild<QLabel*>(name)};
        return found ? found->text() : QStringLiteral("(no %1)").arg(name);
    }};
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
    const QString key1{QString::fromStdString(XOnlyHex(ValidatorKey(1)))};
    QVERIFY2(label("validatorKeys").contains(key1.left(10)), qPrintable(label("validatorKeys")));
    QCOMPARE(label("validatorInSet"), QString{"yes"});
    QCOMPARE(label("validatorJail"), QString{"no"});
    // A genesis validator: in the set with its genesis stake, nothing bonded.
    QCOMPARE(label("validatorBonded"), QString{"0.00000000"});
    QVERIFY2(label("validatorShare").startsWith("33.3 % (stake 1 of 3)"), qPrintable(label("validatorShare")));
    QVERIFY2(label("validatorNextSlot").startsWith("slot "), qPrintable(label("validatorNextSlot")));
    QVERIFY2(label("validatorBlocks").contains("in all on the active chain"), qPrintable(label("validatorBlocks")));
    QVERIFY2(label("validatorMissed").contains("since the node started"), qPrintable(label("validatorMissed")));
    QVERIFY(!start->isEnabled());
    QVERIFY(stop->isEnabled());
    QCOMPARE(validators->rowCount(), 3);
    int local{0};
    for (int row{0}; row < 3; ++row) {
        QVERIFY(validators->item(row, 2)->text() == "33.3 %");
        local += validators->item(row, 4)->text() == "this node, in the set";
    }
    QCOMPARE(local, 1);

    // Finality: the first merged mined block is anchored by the second, so the deposit is final
    // (one anchor makes a block final on regtest).
    QVERIFY2(finality->text().contains("final up to block 1"), qPrintable(finality->text()));
    QCOMPARE(label("balanceFinal"), QString{"10.00000000"});
    QCOMPARE(label("balanceSoft"), QString{"0.00000000"});
    QCOMPARE(label("balanceAnchored"), QString{"0.00000000"});
    QVERIFY(anchors->rowCount() >= 2);
    QCOMPARE(anchors->item(0, 1)->text(), QString{"merged mined block"});

    // Staking off, from the page: the producer stops; and on again.
    stop->click();
    QTRY_VERIFY_WITH_TIMEOUT(!chain.Hooks().GetProducer()->Enabled(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains("Staking is off"), 30000);
    QVERIFY(start->isEnabled());
    QVERIFY(!stop->isEnabled());
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(chain.Hooks().GetProducer()->Enabled(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains("Staking is on"), 30000);

    // The validator makes blocks of stake at its slots; the page shows them as it refreshes.
    QTRY_VERIFY_WITH_TIMEOUT(chain.Hooks().GetProducer()->GetStatus().produced > 0, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(!label("validatorBlocks").startsWith("0 today, 0 in all"), 30000);
    QVERIFY(!label("stakingError").contains("getstakinginfo"));

    // Bonds: the wallet bonds a coin to this node's validator key (the node signs the proof of
    // possession); it is in the list of the wallet's bonds once a block of stake has it. Asked first.
    auto* bond_key{page.findChild<QLineEdit*>("bondKey")};
    auto* bond_amount{page.findChild<QLineEdit*>("bondAmount")};
    auto* bond_button{page.findChild<QPushButton*>("bondButton")};
    auto* add_bond_button{page.findChild<QPushButton*>("addBondButton")};
    auto* unbond_button{page.findChild<QPushButton*>("unbondButton")};
    auto* unbond_all_button{page.findChild<QPushButton*>("unbondAllButton")};
    auto* bonds{page.findChild<QTableWidget*>("bonds")};
    QVERIFY(bond_key && bond_amount && bond_button && add_bond_button && unbond_button && unbond_all_button && bonds);
    QCOMPARE(bonds->rowCount(), 0);
    // Clear errors, before anything is sent: no amount; adding to a bond the wallet does not have;
    // unbonding from it; a key that is not one.
    QStringList texts{WithMessageBoxes(QMessageBox::Ok, [&] { bond_button->click(); })};
    QCOMPARE(texts.size(), 1);
    QVERIFY2(texts[0].contains("Enter the amount to bond"), qPrintable(texts[0]));
    bond_amount->setText("1");
    texts = WithMessageBoxes(QMessageBox::Ok, [&] { add_bond_button->click(); });
    QCOMPARE(texts.size(), 1);
    QVERIFY2(texts[0].contains("has no bond to the validator key"), qPrintable(texts[0]));
    texts = WithMessageBoxes(QMessageBox::Ok, [&] { unbond_all_button->click(); });
    QCOMPARE(texts.size(), 1);
    QVERIFY2(texts[0].contains("has nothing bonded to the validator key"), qPrintable(texts[0]));
    bond_key->setText("not a key");
    texts = WithMessageBoxes(QMessageBox::Ok, [&] { bond_button->click(); });
    QCOMPARE(texts.size(), 1);
    QVERIFY2(texts[0].contains("64 hexadecimal digits"), qPrintable(texts[0]));
    bond_key->clear();
    // Said no: nothing happens.
    texts = WithMessageBoxes(QMessageBox::No, [&] { bond_button->click(); });
    QCOMPARE(texts.size(), 1);
    QVERIFY2(texts[0].startsWith("Bond 1 to the validator key"), qPrintable(texts[0]));
    QCOMPARE(bond_amount->text(), QString{"1"});
    texts = WithMessageBoxes(QMessageBox::Yes, [&] { bond_button->click(); }, /*wait_for=*/2);
    QCOMPARE(texts.size(), 2);
    QVERIFY2(texts[1].contains("The bond is in transaction"), qPrintable(texts.join(" / ")));
    QTRY_VERIFY_WITH_TIMEOUT(bonds->rowCount() == 1 && bonds->item(0, 1)->text() == "1.00000000", 30000);
    QCOMPARE(WITH_LOCK(::cs_main, return pos::GetStakeState(context.chainman->ActiveChainstate().SideState().View()).bonded), COIN);
    QCOMPARE(bonds->item(0, 0)->toolTip(), key1);
    QTRY_COMPARE_WITH_TIMEOUT(label("validatorBonded"), QString{"1.00000000"}, 30000);
    // Add to it.
    bond_amount->setText("0.5");
    texts = WithMessageBoxes(QMessageBox::Yes, [&] { add_bond_button->click(); }, /*wait_for=*/2);
    QCOMPARE(texts.size(), 2);
    QVERIFY2(texts[0].startsWith("Add 0.5 to this wallet's bond of 1.00000000"), qPrintable(texts[0]));
    QVERIFY2(texts[1].contains("1.50000000 bonded once it is mined"), qPrintable(texts.join(" / ")));
    QTRY_VERIFY_WITH_TIMEOUT(bonds->item(0, 1)->text() == "1.50000000", 30000);
    // Unbond more than there is: refused before it is sent. Then all of it: unbonding, paid some anchors later.
    bond_amount->setText("2");
    texts = WithMessageBoxes(QMessageBox::Ok, [&] { unbond_button->click(); });
    QCOMPARE(texts.size(), 1);
    QVERIFY2(texts[0].contains("unbond at most that much"), qPrintable(texts[0]));
    bond_amount->clear();
    texts = WithMessageBoxes(QMessageBox::Ok, [&] { unbond_button->click(); });
    QCOMPARE(texts.size(), 1);
    QVERIFY2(texts[0].contains("or use Unbond all"), qPrintable(texts[0]));
    texts = WithMessageBoxes(QMessageBox::Yes, [&] { unbond_all_button->click(); }, /*wait_for=*/2);
    QCOMPARE(texts.size(), 2);
    QVERIFY2(texts[0].contains("all of the bond (1.50000000 now)"), qPrintable(texts[0]));
    QVERIFY2(texts[1].contains("The unbond of 1.50000000"), qPrintable(texts.join(" / ")));
    QTRY_VERIFY_WITH_TIMEOUT(bonds->item(0, 1)->text() == "0.00000000" && bonds->item(0, 2)->text() == "1.50000000", 30000);
    QTRY_COMPARE_WITH_TIMEOUT(label("validatorUnbonding"), QString{"1.50000000"}, 30000);

    // Staking income. The bonds paid fees: the blocks of stake that took them paid their producer --
    // this node, to the validator key's address, which the wallet has -- its share, and put the rest
    // in the pot. A merged mined block paying the wallet pays it the pot as the anchor's payee.
    TransactionTableModel& transactions{*wallet_model.getTransactionTableModel()};
    const auto types{[&] {
        QStringList list;
        for (int row{0}; row < transactions.rowCount({}); ++row) {
            const QModelIndex index{transactions.index(row, TransactionTableModel::Type)};
            list << QStringLiteral("%1:%2").arg(index.data(Qt::DisplayRole).toString()).arg(index.data(TransactionTableModel::AmountRole).toLongLong());
        }
        return list.join(QStringLiteral(", "));
    }};
    QTRY_VERIFY2_WITH_TIMEOUT(!RowsOfType(transactions, TransactionRecord::Staked).isEmpty(), qPrintable(types()), 30000);
    const CScript payee{GetScriptForDestination(WitnessV0KeyHash{chain.key.GetPubKey()})};
    const CBlock anchor{chain.Mine(payee)};
    QCOMPARE(WITH_LOCK(::cs_main, return context.chainman->ActiveChain().Tip()->GetBlockHash()), anchor.GetHash());
    QTRY_VERIFY_WITH_TIMEOUT(!RowsOfType(transactions, TransactionRecord::AnchorReward).isEmpty(), 30000);
    const QModelIndex pot{RowsOfType(transactions, TransactionRecord::AnchorReward).first()};
    QCOMPARE(pot.data(Qt::DisplayRole).toString(), QString{"BMM reward"});
    QVERIFY(pot.data(TransactionTableModel::AmountRole).toLongLong() > 0);
    QCOMPARE(RowsOfType(transactions, TransactionRecord::Staked).first().data(Qt::DisplayRole).toString(), QString{"Staked"});
    CAmount staked{0};
    for (const QModelIndex& row : RowsOfType(transactions, TransactionRecord::Staked)) staked += row.data(TransactionTableModel::AmountRole).toLongLong();
    QVERIFY(staked > 0);
    // The merged mined block's first output (the payee's, nothing in it) is still mined.
    QVERIFY(!RowsOfType(transactions, TransactionRecord::Generated).isEmpty());
    QCOMPARE(RowsOfType(transactions, TransactionRecord::SlashingReward).size(), 0);
    // The list's filter has the kinds of income.
    {
        TransactionView view(platform_style.get());
        view.setModel(&wallet_model);
        QStringList kinds;
        for (QComboBox* combo : view.findChildren<QComboBox*>()) {
            for (int i{0}; i < combo->count(); ++i) kinds << combo->itemText(i);
        }
        QVERIFY2(kinds.contains("Staked") && kinds.contains("BMM reward") && kinds.contains("Slashing reward") && kinds.contains("Mined"), qPrintable(kinds.join(", ")));
    }
    // The page adds them up.
    wait_refresh();
    const StakingPage::Income income{StakingPage::WalletIncome(wallet_model, QDateTime(QDate::currentDate(), QTime(0, 0)))};
    QCOMPARE(income.staked, staked);
    QVERIFY(income.anchor > 0);
    QCOMPARE(income.slashing, CAmount{0});
    QVERIFY(income.blocks > 0);
    QVERIFY2(label("incomeStakedTotal").startsWith(BitcoinUnits::format(BitcoinUnit::BTC, staked)), qPrintable(label("incomeStakedTotal")));
    QVERIFY2(label("incomeAnchorTotal").startsWith(BitcoinUnits::format(BitcoinUnit::BTC, income.anchor)), qPrintable(label("incomeAnchorTotal")));
    QVERIFY2(label("incomeSlashingTotal").startsWith("0.00000000"), qPrintable(label("incomeSlashingTotal")));
    QVERIFY2(label("incomeAllTotal").startsWith(BitcoinUnits::format(BitcoinUnit::BTC, staked + income.anchor)), qPrintable(label("incomeAllTotal")));
    // Through the wallet's RPC too.
    {
        QString rpc_error;
        const auto listed{NodeRpc::Call(&client_model, "listtransactions", NodeRpc::Args({"*", 1000}), rpc_error, QString{})};
        QVERIFY2(listed, qPrintable(rpc_error));
        int staked_entries{0}, anchor_entries{0};
        for (const UniValue& entry : listed->getValues()) {
            const UniValue& kind{entry.find_value("income")};
            staked_entries += kind.isStr() && kind.get_str() == "staked";
            anchor_entries += kind.isStr() && kind.get_str() == "anchor";
        }
        QVERIFY(staked_entries > 0);
        QCOMPARE(anchor_entries, 1);
    }
    SaveScreenshot(page, "staking-page");
    // In a small window, the boxes go one under the other, in a scroll area.
    page.resize(560, 600);
    QTest::qWait(50);
    SaveScreenshot(page, "staking-page-narrow");
    page.resize(1100, 900);

    // The block explorer: the kind of each block (PoS, PoW), the slot and producer of a block of
    // stake, the mainchain block that committed to a merged mined one, its finality.
    {
        BlockExplorer explorer;
        explorer.setClientModel(&client_model);
        explorer.resize(1200, 700);
        explorer.show();
        QVERIFY(QTest::qWaitForWindowExposed(&explorer));
        auto* blocks{explorer.findChild<QTableWidget*>("explorerBlocks")};
        const int anchor_height{WITH_LOCK(::cs_main, return context.chainman->m_blockman.LookupBlockIndex(anchor.GetHash())->nHeight)};
        const int tip_height{WITH_LOCK(::cs_main, return context.chainman->ActiveChain().Height())};
        QTRY_VERIFY_WITH_TIMEOUT(blocks->rowCount() >= 10 && blocks->item(0, BlockExplorer::COL_HEIGHT)->text().toInt() >= tip_height, 30000);
        QCOMPARE(blocks->horizontalHeaderItem(BlockExplorer::COL_KIND)->text(), QString{"Kind"});
        QVERIFY(!blocks->isColumnHidden(BlockExplorer::COL_KIND));
        int pos_rows{0}, pow_rows{0}, ack_rows{0};
        for (int row{0}; row < blocks->rowCount(); ++row) {
            const int height{blocks->item(row, BlockExplorer::COL_HEIGHT)->text().toInt()};
            const QString kind{blocks->item(row, BlockExplorer::COL_KIND)->text()};
            const QString hash{blocks->item(row, BlockExplorer::COL_HASH)->text()};
            const CBlockIndex* index{WITH_LOCK(::cs_main, return context.chainman->m_blockman.LookupBlockIndex(uint256::FromHex(hash.toStdString()).value()))};
            QVERIFY(index && index->nHeight == height);
            QVERIFY(!blocks->item(row, BlockExplorer::COL_KIND)->icon().isNull());
            if (index->m_ext) {
                ++pos_rows;
                QCOMPARE(blocks->item(row, BlockExplorer::COL_SLOT)->text().toLongLong(), pos::SlotOf(index->nTime, chain.Hooks().Params()));
                // Only this node's key makes blocks here.
                QCOMPARE(blocks->item(row, BlockExplorer::COL_PRODUCER)->text(), QString{"this node"});
                QCOMPARE(blocks->item(row, BlockExplorer::COL_PRODUCER)->toolTip(), key1);
                // A block of stake that acknowledges an anchor shows it, with the mainchain block that committed to it.
                if (index->nNonce != 0) {
                    ++ack_rows;
                    QCOMPARE(kind, QString{"BMM"});
                    QCOMPARE(blocks->item(row, BlockExplorer::COL_MAINCHAIN)->text().toUInt(), index->nNonce);
                } else {
                    QCOMPARE(kind, QString{"PoS"});
                    QVERIFY(blocks->item(row, BlockExplorer::COL_MAINCHAIN)->text().isEmpty());
                }
            } else {
                ++pow_rows;
                QCOMPARE(kind, QString{"PoW"});
                QVERIFY(blocks->item(row, BlockExplorer::COL_SLOT)->text().isEmpty());
                QVERIFY(blocks->item(row, BlockExplorer::COL_PRODUCER)->text().isEmpty());
                if (height > 0) QVERIFY2(!blocks->item(row, BlockExplorer::COL_MAINCHAIN)->text().isEmpty(), qPrintable(QString::number(height)));
            }
            const QString block_finality{blocks->item(row, BlockExplorer::COL_FINALITY)->text()};
            QVERIFY2(block_finality == "soft" || block_finality == "BMM-confirmed" || block_finality == "final", qPrintable(block_finality));
            // Everything up to the anchor's parent is final (one anchor makes a block final here).
            if (height < anchor_height) QCOMPARE(block_finality, QString{"final"});
        }
        QVERIFY(pos_rows > 0);
        QVERIFY(pow_rows > 0);
        // The summary of a merged mined block, and of a block of stake.
        auto* summary_label{explorer.findChild<QLabel*>("explorerSummary")};
        explorer.search(QString::fromStdString(anchor.GetHash().GetHex()));
        QTRY_VERIFY_WITH_TIMEOUT(summary_label->text().contains("PoW (merge-mined, committed at mainchain block"), 30000);
        QVERIFY(summary_label->isVisible());
        QVERIFY2(summary_label->text().contains("<b>Finality:</b>"), qPrintable(summary_label->text()));
        QVERIFY(summary_label->text().contains("<b>Anchor:</b> anchors its parent"));
        SaveScreenshot(explorer, "explorer-fastblocks-pow");
        const CBlockIndex* stake_block{WITH_LOCK(::cs_main, {
            const CBlockIndex* index{context.chainman->ActiveChain().Tip()};
            while (index && !index->m_ext) index = index->pprev;
            return index;
        })};
        QVERIFY(stake_block);
        explorer.search(QString::number(stake_block->nHeight));
        QTRY_VERIFY_WITH_TIMEOUT(summary_label->text().contains(QString("PoS (slot %1, producer this node, ").arg(pos::SlotOf(stake_block->nTime, chain.Hooks().Params()))), 30000);
        QVERIFY2(summary_label->text().contains("<b>Finality:</b>"), qPrintable(summary_label->text()));
        SaveScreenshot(explorer, "explorer-fastblocks");
        // The marks of the kinds read on the dark themes too.
        for (const char* theme : {"dark", "matrix", "qdarkstyle", "ocean"}) {
            Theme::Apply(theme);
            QTest::qWait(20);
            SaveScreenshot(explorer, QStringLiteral("explorer-fastblocks-%1").arg(theme));
            SaveScreenshot(page, QStringLiteral("staking-page-%1").arg(theme));
        }
        Theme::Apply("system");
        explorer.hide();
    }

    // Without a wallet, the rest of the page still works.
    page.setWalletModel(nullptr);
    wait_refresh();
    QVERIFY(label("stakingError").isEmpty());
    QVERIFY(!bond_button->isEnabled());
    QCOMPARE(bonds->rowCount(), 0);
    QCOMPARE(label("incomeStakedTotal"), QString{"–"});
    page.hide();
    chain.Hooks().StopServices(context);
}
