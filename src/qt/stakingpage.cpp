// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/stakingpage.h>

#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/itemviews.h>
#include <qt/noderpc.h>
#include <qt/optionsmodel.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>

#include <QBoxLayout>
#include <QDateTime>
#include <QEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShowEvent>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace {
using NodeRpc::Args;
using NodeRpc::Item;
using NodeRpc::Text;

constexpr int REFRESH_INTERVAL_MS{3000};
//! Narrower than this, the boxes of a row of the page go one under the other.
constexpr int NARROW_WIDTH{860};

//! An amount of a reply; one of another type is shown as it is.
QString Amount(const UniValue& value) { return value.isNum() ? QString::number(value.get_real(), 'f', 8) : Text(value); }
QString Amount(CAmount amount) { return QString::number(static_cast<double>(amount) / COIN, 'f', 8); }
//! An amount of a reply, in satoshis (0 if it is not a number).
CAmount Sats(const UniValue& value) { return value.isNum() ? static_cast<CAmount>(std::llround(value.get_real() * COIN)) : 0; }

//! Whether the text is an amount: a positive number.
bool IsAmount(const QString& text)
{
    bool ok{false};
    const double value{text.toDouble(&ok)};
    return ok && value > 0;
}

//! Whether the text is an x-only key in hex.
bool IsKey(const QString& text)
{
    static const QRegularExpression hex{QStringLiteral("^[0-9a-fA-F]{64}$")};
    return hex.match(text).hasMatch();
}

//! A key, shortened for a table: its first and last bytes.
QString ShortKey(const QString& key) { return key.size() > 20 ? key.left(10) + QStringLiteral("…") + key.right(8) : key; }

//! A time to come, as "1 h 5 min", "3 min 20 s" or "12 s".
QString Until(int64_t seconds)
{
    seconds = std::max<int64_t>(seconds, 0);
    if (seconds >= 3600) return QObject::tr("%1 h %2 min").arg(seconds / 3600).arg((seconds % 3600) / 60);
    if (seconds >= 60) return QObject::tr("%1 min %2 s").arg(seconds / 60).arg(seconds % 60);
    return QObject::tr("%1 s").arg(seconds);
}

QTableWidget* MakeTable(QWidget* parent, const QString& name, const QStringList& headers)
{
    auto* table{new ItemViews::Table(0, headers.size(), parent)};
    table->setObjectName(name);
    table->setHorizontalHeaderLabels(headers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setMinimumHeight(140);
    return table;
}

QLabel* Value(QWidget* parent, const QString& name)
{
    auto* label{new QLabel(parent)};
    label->setObjectName(name);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

/** Lays the boxes of its rows side by side, or one under the other when the page is narrow. */
class Reflow : public QObject
{
public:
    Reflow(QWidget* watched, std::vector<QBoxLayout*> rows) : QObject(watched), m_rows(std::move(rows)) { watched->installEventFilter(this); }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Resize) {
            const auto direction{static_cast<QWidget*>(watched)->width() < NARROW_WIDTH ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight};
            for (QBoxLayout* row : m_rows) {
                if (row->direction() != direction) row->setDirection(direction);
            }
        }
        return false;
    }

private:
    std::vector<QBoxLayout*> m_rows;
};
} // namespace

StakingPage::StakingPage(const PlatformStyle*, QWidget* parent) : QWidget(parent)
{
    // Everything in a scroll area: the page stays readable in a small window.
    auto* outer{new QVBoxLayout(this)};
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll{new QScrollArea(this)};
    scroll->setObjectName("stakingScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* content{new QWidget(scroll)};
    scroll->setWidget(content);
    auto* layout{new QVBoxLayout(content)};

    m_summary = Value(content, "stakingSummary");
    layout->addWidget(m_summary);

    // This node as a validator | staking income, and the coins by finality.
    auto* top{new QBoxLayout(QBoxLayout::LeftToRight)};
    auto* validator_box{new QGroupBox(tr("This node as a validator"), content)};
    auto* validator_layout{new QVBoxLayout(validator_box)};
    m_staking_status = Value(validator_box, "stakingStatus");
    validator_layout->addWidget(m_staking_status);
    auto* buttons{new QHBoxLayout};
    m_staking_start = new QPushButton(tr("Start staking"), validator_box);
    m_staking_start->setObjectName("stakingStart");
    m_staking_start->setToolTip(tr("Make blocks of stake at the slots this node's validator keys lead"));
    m_staking_stop = new QPushButton(tr("Stop staking"), validator_box);
    m_staking_stop->setObjectName("stakingStop");
    m_staking_stop->setToolTip(tr("Make no blocks of stake until started again (a validator that misses its slots may be jailed)"));
    buttons->addWidget(m_staking_start);
    buttons->addWidget(m_staking_stop);
    buttons->addStretch();
    validator_layout->addLayout(buttons);
    auto* overview{new QFormLayout};
    overview->setRowWrapPolicy(QFormLayout::WrapLongRows);
    overview->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_keys = Value(validator_box, "validatorKeys");
    m_bonded = Value(validator_box, "validatorBonded");
    m_unbonding = Value(validator_box, "validatorUnbonding");
    m_eligible = Value(validator_box, "validatorEligible");
    m_in_set = Value(validator_box, "validatorInSet");
    m_jail = Value(validator_box, "validatorJail");
    m_share = Value(validator_box, "validatorShare");
    m_next_slot = Value(validator_box, "validatorNextSlot");
    m_blocks_made = Value(validator_box, "validatorBlocks");
    m_missed = Value(validator_box, "validatorMissed");
    m_payout = Value(validator_box, "validatorPayout");
    overview->addRow(tr("Keys:"), m_keys);
    overview->addRow(tr("Bonded:"), m_bonded);
    overview->addRow(tr("Unbonding:"), m_unbonding);
    overview->addRow(tr("Eligible stake:"), m_eligible);
    overview->addRow(tr("In the validator set:"), m_in_set);
    overview->addRow(tr("Jailed or banned:"), m_jail);
    overview->addRow(tr("Share of the set's stake:"), m_share);
    overview->addRow(tr("Next slot it leads:"), m_next_slot);
    overview->addRow(tr("Blocks made:"), m_blocks_made);
    overview->addRow(tr("Missed slots:"), m_missed);
    overview->addRow(tr("Fees paid to:"), m_payout);
    m_payout->setToolTip(tr("Where the producer's share of the fees of this node's blocks of stake goes: -validatorpayout, else an address of the node's wallet, else the first validator key's own address"));
    m_share->setToolTip(tr("Leaders are picked by stake: the share of the set's stake is the share of the slots to expect"));
    m_missed->setToolTip(tr("Slots this node's keys led and it made no block for, since it started. A validator that misses too many in an epoch is jailed for a while."));
    validator_layout->addLayout(overview);
    validator_layout->addStretch();
    top->addWidget(validator_box, 3);

    auto* right{new QVBoxLayout};
    auto* income_box{new QGroupBox(tr("Staking income of this wallet"), content)};
    auto* income{new QGridLayout(income_box)};
    income->addWidget(new QLabel(tr("Today"), income_box), 0, 1, Qt::AlignRight);
    income->addWidget(new QLabel(tr("In all"), income_box), 0, 2, Qt::AlignRight);
    const auto income_row{[&](int row, const QString& label, const QString& tip, QLabel*& today, QLabel*& all, const QString& name) {
        auto* title{new QLabel(label, income_box)};
        title->setToolTip(tip);
        income->addWidget(title, row, 0);
        today = Value(income_box, name + "Today");
        all = Value(income_box, name + "Total");
        today->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        all->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        income->addWidget(today, row, 1);
        income->addWidget(all, row, 2);
    }};
    income_row(1, tr("Staked"), tr("The producer's share of the fees of the blocks of stake made, paid to this wallet"), m_income_staked_today, m_income_staked, "incomeStaked");
    income_row(2, tr("BMM rewards"), tr("The fee pot (and a share of the penalties) paid to this wallet for BMM: a BMM commitment it bid for, or a merged mined block"), m_income_anchor_today, m_income_anchor, "incomeAnchor");
    income_row(3, tr("Slashing rewards"), tr("The reporter's share of the penalty of a validator that signed two blocks for a slot, for proofs this wallet's blocks carried"), m_income_slashing_today, m_income_slashing, "incomeSlashing");
    income_row(4, tr("Total"), {}, m_income_total_today, m_income_total, "incomeAll");
    income->setColumnStretch(0, 1);
    right->addWidget(income_box);

    auto* finality_box{new QGroupBox(tr("Coins of this wallet by finality"), content)};
    auto* finality_form{new QFormLayout(finality_box)};
    finality_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_finality = Value(finality_box, "finalityStatus");
    m_finality->setToolTip(tr("Soft: only blocks of stake are on it, like an unconfirmed payment with slashing behind it. "
                              "BMM-confirmed: a BMM commitment on the mainchain covers it; undoing it takes the mainchain's merged miners, not stake. "
                              "Final: enough BMM commitments cover it."));
    finality_form->addRow(m_finality);
    m_balance_soft = Value(finality_box, "balanceSoft");
    m_balance_anchored = Value(finality_box, "balanceAnchored");
    m_balance_final = Value(finality_box, "balanceFinal");
    finality_form->addRow(tr("Soft:"), m_balance_soft);
    finality_form->addRow(tr("BMM-confirmed:"), m_balance_anchored);
    finality_form->addRow(tr("Final:"), m_balance_final);
    right->addWidget(finality_box);
    right->addStretch();
    top->addLayout(right, 2);
    layout->addLayout(top);

    // The validator set | the newest anchors.
    auto* tables{new QBoxLayout(QBoxLayout::LeftToRight)};
    auto* validators_box{new QGroupBox(tr("Validator set of this epoch"), content)};
    auto* validators_layout{new QVBoxLayout(validators_box)};
    m_validators = MakeTable(validators_box, "validators", {tr("Key"), tr("Stake"), tr("Share"), tr("Slots"), tr("Status")});
    m_validators->horizontalHeaderItem(2)->setToolTip(tr("Its share of the set's stake: of the slots it is expected to lead"));
    validators_layout->addWidget(m_validators);
    tables->addWidget(validators_box, 3);
    auto* anchors_box{new QGroupBox(tr("Newest BMM blocks"), content)};
    auto* anchors_layout{new QVBoxLayout(anchors_box)};
    m_anchors = MakeTable(anchors_box, "anchors", {tr("Block"), tr("Kind"), tr("Covers block"), tr("Mainchain block")});
    anchors_layout->addWidget(m_anchors);
    tables->addWidget(anchors_box, 2);
    layout->addLayout(tables, 1);

    // The wallet's bonds.
    auto* bonds_box{new QGroupBox(tr("Bonds of this wallet"), content)};
    auto* bonds_layout{new QVBoxLayout(bonds_box)};
    auto* bond_form{new QBoxLayout(QBoxLayout::LeftToRight)};
    m_bond_key = new QLineEdit(bonds_box);
    m_bond_key->setObjectName("bondKey");
    m_bond_key->setPlaceholderText(tr("Validator key (x-only, hex): the bond selected below, or this node's, if empty"));
    m_bond_amount = new QLineEdit(bonds_box);
    m_bond_amount->setObjectName("bondAmount");
    m_bond_amount->setPlaceholderText(tr("Amount"));
    bond_form->addWidget(m_bond_key, 3);
    bond_form->addWidget(m_bond_amount, 1);
    bonds_layout->addLayout(bond_form);
    auto* bond_buttons{new QHBoxLayout};
    m_bond_button = new QPushButton(tr("Bond"), bonds_box);
    m_bond_button->setObjectName("bondButton");
    m_bond_button->setToolTip(tr("Bond coins of this wallet to the validator key: they leave the wallet, and the key takes part in the validator sets from the epoch after next"));
    m_add_bond_button = new QPushButton(tr("Add to bond"), bonds_box);
    m_add_bond_button->setObjectName("addBondButton");
    m_add_bond_button->setToolTip(tr("Add coins to this wallet's bond to the key"));
    m_unbond_button = new QPushButton(tr("Unbond"), bonds_box);
    m_unbond_button->setObjectName("unbondButton");
    m_unbond_button->setToolTip(tr("Take the amount entered back from the bond: paid to this wallet some BMM commitments later (about two days on the main network)"));
    m_unbond_all_button = new QPushButton(tr("Unbond all"), bonds_box);
    m_unbond_all_button->setObjectName("unbondAllButton");
    m_unbond_all_button->setToolTip(tr("Take all of the bond back (all that is bonded when the unbond is mined): paid to this wallet some BMM commitments later"));
    bond_buttons->addWidget(m_bond_button);
    bond_buttons->addWidget(m_add_bond_button);
    bond_buttons->addWidget(m_unbond_button);
    bond_buttons->addWidget(m_unbond_all_button);
    bond_buttons->addStretch();
    bonds_layout->addLayout(bond_buttons);
    m_bonds = MakeTable(bonds_box, "bonds", {tr("Key"), tr("Bonded"), tr("Unbonding"), tr("State")});
    m_bonds->setMinimumHeight(100);
    bonds_layout->addWidget(m_bonds);
    layout->addWidget(bonds_box);

    m_error = Value(content, "stakingError");
    layout->addWidget(m_error);

    new Reflow(content, {top, tables, bond_form});

    connect(m_staking_start, &QPushButton::clicked, this, [this] { setStaking(true); });
    connect(m_staking_stop, &QPushButton::clicked, this, [this] { setStaking(false); });
    connect(m_bond_button, &QPushButton::clicked, this, [this] { bond(/*add=*/false); });
    connect(m_add_bond_button, &QPushButton::clicked, this, [this] { bond(/*add=*/true); });
    connect(m_unbond_button, &QPushButton::clicked, this, [this] { unbond(/*all=*/false); });
    connect(m_unbond_all_button, &QPushButton::clicked, this, [this] { unbond(/*all=*/true); });

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &StakingPage::refresh);
}

void StakingPage::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
    if (!client_model) m_timer->stop();
}

void StakingPage::setWalletModel(WalletModel* wallet_model) { m_wallet_model = wallet_model; }

void StakingPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refresh();
    m_timer->start(REFRESH_INTERVAL_MS);
}

void StakingPage::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    m_timer->stop();
}

StakingPage::Income StakingPage::WalletIncome(WalletModel& wallet_model, const QDateTime& midnight)
{
    Income income;
    TransactionTableModel* transactions{wallet_model.getTransactionTableModel()};
    if (!transactions) return income;
    for (int row{0}; row < transactions->rowCount({}); ++row) {
        const QModelIndex index{transactions->index(row, 0)};
        const int type{index.data(TransactionTableModel::TypeRole).toInt()};
        if (type != TransactionRecord::Staked && type != TransactionRecord::AnchorReward && type != TransactionRecord::SlashingReward) continue;
        const CAmount amount{index.data(TransactionTableModel::AmountRole).toLongLong()};
        const bool today{index.data(TransactionTableModel::DateRole).toDateTime() >= midnight};
        switch (type) {
        case TransactionRecord::Staked:
            income.staked += amount;
            ++income.blocks;
            if (today) {
                income.staked_today += amount;
                ++income.blocks_today;
            }
            break;
        case TransactionRecord::AnchorReward:
            income.anchor += amount;
            if (today) income.anchor_today += amount;
            break;
        case TransactionRecord::SlashingReward:
            income.slashing += amount;
            if (today) income.slashing_today += amount;
            break;
        }
    }
    return income;
}

void StakingPage::tell(const QString& text, bool error)
{
    if (error) {
        m_error->setText(text);
        QMessageBox::warning(this, tr("Staking"), text);
    } else {
        QMessageBox::information(this, tr("Staking"), text);
    }
}

void StakingPage::walletCall(const std::string& method, const UniValue& params, const QString& failure, std::function<void(const UniValue&)> done)
{
    if (!m_client_model || !m_wallet_model) return;
    NodeRpc::CallAsync(this, m_client_model, method, params, [this, failure, done = std::move(done)](std::optional<UniValue> result, const QString& error) {
        if (!result) {
            tell(tr("%1: %2").arg(failure, error), /*error=*/true);
            return;
        }
        const QPointer<StakingPage> self{this};
        done(*result);
        if (self) refresh();
    }, m_wallet_model->getWalletName());
}

void StakingPage::setStaking(bool enabled)
{
    if (!m_client_model) return;
    m_error->clear();
    NodeRpc::CallAsync(this, m_client_model, "setstaking", Args({enabled}), [this](std::optional<UniValue> result, const QString& error) {
        if (!result) m_error->setText(tr("setstaking: %1").arg(error));
        refresh();
    });
}

QString StakingPage::bondKey() const
{
    const QString entered{m_bond_key->text().trimmed()};
    if (!entered.isEmpty()) return entered.toLower();
    if (const auto selected{m_bonds->selectionModel()->selectedRows()}; !selected.isEmpty()) {
        if (auto* item{m_bonds->item(selected.first().row(), 0)}) return item->toolTip();
    }
    return m_local_keys.isEmpty() ? QString{} : m_local_keys.first();
}

std::optional<UniValue> StakingPage::walletBond(const QString& key) const
{
    for (const UniValue& bond : m_last_bonds.getValues()) {
        if (bond.isObject() && bond.find_value("validator").isStr() && Text(bond.find_value("validator")) == key) return bond;
    }
    return std::nullopt;
}

void StakingPage::bond(bool add)
{
    if (!m_client_model || !m_wallet_model) return;
    m_error->clear();
    const QString key{bondKey()};
    const QString amount{m_bond_amount->text().trimmed()};
    if (key.isEmpty()) return tell(tr("Enter a validator key, select a bond, or run this node with -validatorkeyfile."), /*error=*/true);
    if (!IsKey(key)) return tell(tr("A validator key is an x-only key: 64 hexadecimal digits."), /*error=*/true);
    if (!IsAmount(amount)) return tell(tr("Enter the amount to bond as a number, like 1.5."), /*error=*/true);
    const auto existing{walletBond(key)};
    if (add && !existing) return tell(tr("This wallet has no bond to the validator key %1: bond to it first.").arg(ShortKey(key)), /*error=*/true);
    // The page, or the wallet, may be gone after a message box: its event loop runs everything else.
    const QPointer<StakingPage> self{this};
    QString question;
    if (existing) {
        question = tr("Add %1 to this wallet's bond of %2 to the validator key %3? The coins leave this wallet now; unbonding them takes some BMM commitments.")
                       .arg(amount, Amount(existing->find_value("bonded")), ShortKey(key));
    } else {
        question = tr("Bond %1 to the validator key %2? The coins leave this wallet now; unbonding them takes some BMM commitments.").arg(amount, ShortKey(key));
    }
    if (QMessageBox::question(this, tr("Staking"), question) != QMessageBox::Yes) return;
    if (!self || !m_wallet_model) return;
    walletCall("createbond", Args({key.toStdString(), amount.toStdString()}), add ? tr("Could not add to the bond") : tr("Could not bond"), [this](const UniValue& result) {
        m_bond_amount->clear();
        QString text{tr("The bond is in transaction %1. The key has %2 bonded once it is mined.").arg(Text(result.find_value("txid")), Amount(result.find_value("bonded")))};
        if (result.find_value("eligible").isBool() && !result.find_value("eligible").get_bool()) text += QStringLiteral(" ") + tr("That is less than the minimum stake: the key takes part once it has that much bonded.");
        tell(text, /*error=*/false);
    });
}

void StakingPage::unbond(bool all)
{
    if (!m_client_model || !m_wallet_model) return;
    m_error->clear();
    const QString key{bondKey()};
    const QString amount{m_bond_amount->text().trimmed()};
    if (key.isEmpty()) return tell(tr("Enter a validator key, select a bond, or run this node with -validatorkeyfile."), /*error=*/true);
    if (!IsKey(key)) return tell(tr("A validator key is an x-only key: 64 hexadecimal digits."), /*error=*/true);
    const auto existing{walletBond(key)};
    const CAmount bonded{existing ? Sats(existing->find_value("bonded")) : 0};
    if (!existing || bonded <= 0) return tell(tr("This wallet has nothing bonded to the validator key %1.").arg(ShortKey(key)), /*error=*/true);
    if (!all) {
        if (!IsAmount(amount)) return tell(tr("Enter the amount to unbond as a number, like 1.5, or use Unbond all."), /*error=*/true);
        if (static_cast<CAmount>(std::llround(amount.toDouble() * COIN)) > bonded) {
            return tell(tr("The bond has %1 bonded: unbond at most that much.").arg(Amount(bonded)), /*error=*/true);
        }
    }
    const QPointer<StakingPage> self{this};
    const QString what{all ? tr("all of the bond (%1 now)").arg(Amount(bonded)) : amount};
    if (QMessageBox::question(this, tr("Staking"), tr("Unbond %1 from the validator key %2? The coins are paid back to this wallet some BMM commitments later.").arg(what, ShortKey(key))) != QMessageBox::Yes) return;
    if (!self || !m_wallet_model) return;
    UniValue params{Args({key.toStdString()})};
    params.push_back(all ? std::string{"all"} : amount.toStdString());
    walletCall("createunbond", params, tr("Could not unbond"), [this](const UniValue& result) {
        m_bond_amount->clear();
        tell(tr("The unbond of %1 is in transaction %2. It is paid to %3 at BMM commitment %4 or so.")
                 .arg(Amount(result.find_value("amount")), Text(result.find_value("txid")), Text(result.find_value("owner")), Text(result.find_value("releaseanchor"))),
             /*error=*/false);
    });
}

void StakingPage::refresh()
{
    if (!m_client_model) return;
    // Off the GUI thread (NodeRpc::RunAsync), one refresh at a time.
    if (m_fetching) {
        m_refresh_again = true;
        return;
    }
    m_fetching = true;
    // The client model outlives the thread of RunAsync (NodeRpc::Stop comes first at shutdown).
    ClientModel* const client_model{m_client_model};
    std::optional<QString> wallet;
    if (m_wallet_model) wallet = m_wallet_model->getWalletName();
    const int64_t midnight{QDateTime(QDate::currentDate(), QTime(0, 0)).toSecsSinceEpoch()};
    NodeRpc::RunAsync(this, [this, client_model, wallet, midnight]() -> std::function<void()> {
        Snapshot snapshot;
        const auto call{[&](const std::string& method, const UniValue& params, bool in_wallet = false) {
            QString error;
            auto result{NodeRpc::Call(client_model, method, params, error, in_wallet ? wallet : std::nullopt)};
            if (!result) snapshot.errors << tr("%1: %2").arg(QString::fromStdString(method), error);
            return result;
        }};
        snapshot.pos = call("getposinfo", Args({}));
        if (snapshot.pos) {
            snapshot.staking = call("getstakinginfo", Args({midnight}));
            snapshot.anchors = call("getanchorinfo", Args({10}));
            snapshot.chain = call("getblockchaininfo", Args({}));
            snapshot.validators = call("getvalidators", Args({}));
            if (wallet) {
                snapshot.bonds = call("listbonds", Args({}), /*in_wallet=*/true);
                snapshot.balances = call("getbalances", Args({}), /*in_wallet=*/true);
            }
        }
        // On the GUI thread, if this page is still there.
        return [this, snapshot = std::move(snapshot)] {
            m_fetching = false;
            // A reply of an unexpected shape makes the UniValue getters throw: not out of this slot.
            try {
                apply(snapshot);
            } catch (const std::exception& e) {
                m_error->setText(tr("Unexpected reply from the node: %1").arg(QString::fromStdString(e.what())));
            }
            Q_EMIT refreshed();
            if (std::exchange(m_refresh_again, false)) refresh();
        };
    });
}

void StakingPage::apply(const Snapshot& snapshot)
{
    m_error->setText(snapshot.errors.join(QStringLiteral("\n")));
    if (!snapshot.pos) {
        m_summary->setText(tr("This chain has no blocks of stake."));
        return;
    }
    const UniValue& info{*snapshot.pos};
    QString summary;
    if (info["active"].get_bool()) {
        summary = tr("Blocks of stake come every %1 s. Slot %2, epoch %3, led by %4.")
                      .arg(Text(info["slotseconds"]), Text(info["slot"]), Text(info["epoch"]), ShortKey(Text(info.find_value("leader"))));
    } else {
        summary = tr("Blocks of stake are off in this epoch (too few validators with stake bonded): the chain's blocks are merged mined only.");
    }
    summary += QStringLiteral(" ") + tr("%1 BMM commitments; the last one at mainchain block %2.").arg(Text(info["anchors"]), Text(info["lastanchormainheight"]));
    if (info["stalled"].get_bool() && info["active"].get_bool()) summary += QStringLiteral(" ") + tr("No blocks of stake lately: merged mined blocks carry the transactions.");
    m_summary->setText(summary);

    if (snapshot.staking) showValidator(*snapshot.staking, &info);
    showIncome();

    if (snapshot.anchors) {
        const UniValue& a{*snapshot.anchors};
        const QString height{snapshot.chain ? Text((*snapshot.chain)["blocks"]) : QStringLiteral("?")};
        m_finality->setText(tr("Best block %1. BMM-confirmed up to block %2, final up to block %3 (%4 BMM commitment(s) make a block final).")
                                .arg(height, Text(a["anchoredheight"]), Text(a["finalheight"]), Text(a["finalanchors"])));
        const UniValue& recent{a["recent"]};
        m_anchors->setRowCount(recent.size());
        for (size_t i{0}; i < recent.size(); ++i) {
            const UniValue& anchor{recent[i]};
            const int row{static_cast<int>(i)};
            m_anchors->setItem(row, 0, Item(Text(anchor["height"])));
            m_anchors->setItem(row, 1, Item(Text(anchor["kind"]) == QStringLiteral("bmm") ? tr("merged mined block (PoW)") : tr("BMM block")));
            m_anchors->setItem(row, 2, Item(Text(anchor["anchoredheight"])));
            m_anchors->setItem(row, 3, Item(Text(anchor["mainheight"])));
            m_anchors->item(row, 0)->setToolTip(Text(anchor["block"]));
            m_anchors->item(row, 2)->setToolTip(Text(anchor["anchored"]));
        }
    }

    if (snapshot.validators) {
        const UniValue& set{*snapshot.validators};
        const UniValue& list{set["validators"]};
        const double total{static_cast<double>(set["totalstake"].getInt<int64_t>())};
        m_validators->setRowCount(list.size());
        for (size_t i{0}; i < list.size(); ++i) {
            const UniValue& validator{list[i]};
            const int row{static_cast<int>(i)};
            const int64_t stake{validator["stake"].getInt<int64_t>()};
            m_validators->setItem(row, 0, Item(ShortKey(Text(validator["pubkey"]))));
            m_validators->item(row, 0)->setToolTip(Text(validator["pubkey"]));
            m_validators->setItem(row, 1, Item(Amount(stake)));
            m_validators->setItem(row, 2, Item(total > 0 ? QString::number(100.0 * stake / total, 'f', 1) + QStringLiteral(" %") : QString{}));
            m_validators->setItem(row, 3, Item(Text(validator["slots"])));
            QStringList status;
            if (validator["local"].get_bool()) status << tr("this node");
            if (validator.find_value("banned").isBool() && validator.find_value("banned").get_bool()) {
                status << tr("banned: signed two blocks for a slot");
            } else if (validator.find_value("jaileduntilepoch").isNum() && validator.find_value("jaileduntilepoch").getInt<int64_t>() > info["epoch"].getInt<int64_t>()) {
                status << tr("jailed until epoch %1").arg(Text(validator.find_value("jaileduntilepoch")));
            } else {
                status << tr("in the set");
            }
            m_validators->setItem(row, 4, Item(status.join(QStringLiteral(", "))));
        }
    }

    // The wallet's bonds.
    for (QPushButton* button : {m_bond_button, m_add_bond_button, m_unbond_button, m_unbond_all_button}) button->setEnabled(m_wallet_model);
    if (!m_wallet_model) {
        m_bonds->setRowCount(0);
        m_last_bonds = UniValue{UniValue::VARR};
    } else if (snapshot.bonds && snapshot.bonds->isArray()) {
        const UniValue& bonds{*snapshot.bonds};
        m_last_bonds = bonds;
        // Keep the bond selected, by its key.
        QString selected;
        if (const auto rows{m_bonds->selectionModel()->selectedRows()}; !rows.isEmpty() && m_bonds->item(rows.first().row(), 0)) selected = m_bonds->item(rows.first().row(), 0)->toolTip();
        m_bonds->setRowCount(bonds.size());
        for (size_t i{0}; i < bonds.size(); ++i) {
            const UniValue& bond{bonds[i]};
            const int row{static_cast<int>(i)};
            m_bonds->setItem(row, 0, Item(ShortKey(Text(bond["validator"]))));
            m_bonds->item(row, 0)->setToolTip(Text(bond["validator"]));
            m_bonds->setItem(row, 1, Item(Amount(bond["bonded"])));
            QStringList unbonds;
            for (const UniValue& unbond : bond["unbonds"].getValues()) unbonds << tr("%1 at BMM commitment %2").arg(Amount(unbond["amount"]), Text(unbond["releaseanchor"]));
            m_bonds->setItem(row, 2, Item(Amount(bond["unbonding"])));
            m_bonds->item(row, 2)->setToolTip(unbonds.join(QStringLiteral("\n")));
            QString state;
            if (bond["banned"].get_bool()) {
                state = tr("banned: signed two blocks for a slot");
            } else if (bond["inset"].get_bool()) {
                state = tr("in the validator set");
            } else if (bond["eligible"].get_bool()) {
                state = tr("joins the set at a coming epoch");
            } else {
                state = tr("not taking part (too little bonded)");
            }
            if (bond["jaileduntilepoch"].getInt<int64_t>() > info["epoch"].getInt<int64_t>()) state += QStringLiteral(", ") + tr("jailed until epoch %1").arg(Text(bond["jaileduntilepoch"]));
            m_bonds->setItem(row, 3, Item(state));
            if (Text(bond["validator"]) == selected) m_bonds->selectRow(row);
        }
    }

    // The wallet's coins by finality.
    if (m_wallet_model && snapshot.balances) {
        const UniValue& finality{(*snapshot.balances)["mine"].find_value("finality")};
        if (finality.isObject()) {
            m_balance_soft->setText(Amount(finality["soft"]));
            m_balance_anchored->setText(Amount(finality["anchored"]));
            m_balance_final->setText(Amount(finality["final"]));
        }
    }
}

void StakingPage::showValidator(const UniValue& s, const UniValue* pos)
{
    const bool validator{s["validator"].get_bool()};
    m_staking_start->setEnabled(validator && !s["enabled"].get_bool());
    m_staking_stop->setEnabled(validator && s["enabled"].get_bool());
    m_local_keys.clear();
    if (!validator) {
        QString status{tr("This node has no validator keys. Start it with -validatorkeyfile to make blocks of stake.")};
        if (!Text(s["producererror"]).isEmpty()) status += QStringLiteral(" ") + Text(s["producererror"]);
        m_staking_status->setText(status);
        for (QLabel* label : {m_keys, m_bonded, m_unbonding, m_eligible, m_in_set, m_jail, m_share, m_next_slot, m_blocks_made, m_missed, m_payout}) label->setText(tr("–"));
        return;
    }
    const int64_t epoch{pos ? (*pos)["epoch"].getInt<int64_t>() : 0};
    QStringList keys, key_tips, jail;
    CAmount bonded{0}, unbonding{0}, eligible{0};
    int in_set{0};
    const UniValue& entries{s["keys"]};
    for (const UniValue& key : entries.getValues()) {
        const QString pubkey{Text(key["pubkey"])};
        m_local_keys << pubkey;
        keys << ShortKey(pubkey);
        key_tips << pubkey;
        in_set += key["inset"].get_bool();
        const UniValue& bond{key.find_value("bond")};
        if (!bond.isObject()) continue;
        bonded += Sats(bond["bonded"]);
        unbonding += Sats(bond["unbonding"]);
        if (bond["eligible"].get_bool()) eligible += Sats(bond["bonded"]) + Sats(bond["genesisstake"]);
        if (bond["banned"].get_bool()) {
            jail << tr("%1 banned (signed two blocks for a slot)").arg(ShortKey(pubkey));
        } else if (bond["jaileduntilepoch"].getInt<int64_t>() > epoch) {
            jail << tr("%1 jailed until epoch %2").arg(ShortKey(pubkey), Text(bond["jaileduntilepoch"]));
        }
    }
    QString status{tr("Staking is %1.").arg(s["enabled"].get_bool() ? tr("on") : tr("off"))};
    if (!Text(s["error"]).isEmpty()) status += QStringLiteral(" ") + tr("Last problem: %1").arg(Text(s["error"]));
    m_staking_status->setText(status);
    m_keys->setText(keys.join(QStringLiteral(", ")));
    m_keys->setToolTip(key_tips.join(QStringLiteral("\n")));
    m_bonded->setText(Amount(bonded));
    m_unbonding->setText(Amount(unbonding));
    QString eligible_text{Amount(eligible)};
    if (eligible == 0 && pos) eligible_text += QStringLiteral(" ") + tr("(a key takes part with at least %1 bonded)").arg(Amount((*pos)["staking"].find_value("minstake")));
    m_eligible->setText(eligible_text);
    const int count{static_cast<int>(entries.size())};
    m_in_set->setText(in_set == 0 ? tr("no") : in_set == count ? tr("yes") : tr("%1 of %2 keys").arg(in_set).arg(count));
    m_jail->setText(jail.isEmpty() ? tr("no") : jail.join(QStringLiteral("; ")));
    const int64_t stake{s["stake"].getInt<int64_t>()}, total{s["totalstake"].getInt<int64_t>()};
    if (total > 0) {
        const double share{100.0 * static_cast<double>(stake) / static_cast<double>(total)};
        m_share->setText(tr("%1 % (stake %2 of %3): about %1 % of the slots").arg(QString::number(share, 'f', 1), Text(s["stake"]), Text(s["totalstake"])));
    } else {
        m_share->setText(tr("no validator set known for this epoch"));
    }
    if (!s.find_value("nextslot").isNull()) {
        const int64_t until{s["nexttime"].getInt<int64_t>() - QDateTime::currentSecsSinceEpoch()};
        m_next_slot->setText(tr("slot %1, in %2 (%3 slot(s) from now)")
                                 .arg(Text(s["nextslot"]), Until(until), QString::number(s["nextslot"].getInt<int64_t>() - s["slot"].getInt<int64_t>())));
    } else {
        m_next_slot->setText(tr("none in the next two epochs"));
    }
    QString made{tr("%1 today, %2 in all on the active chain").arg(Text(s.find_value("blocksmadesince").isNull() ? UniValue{0} : s.find_value("blocksmadesince")), Text(s["blocksmade"]))};
    made += QStringLiteral("; ") + tr("%1 since the node started, %2 of them acknowledgements").arg(Text(s["produced"]), Text(s["acknowledgements"]));
    m_blocks_made->setText(made);
    m_missed->setText(tr("%1 since the node started").arg(Text(s["failed"])));
    m_payout->setText(Text(s.find_value("payout")));
}

void StakingPage::showIncome()
{
    if (!m_wallet_model) {
        for (QLabel* label : {m_income_staked_today, m_income_staked, m_income_anchor_today, m_income_anchor, m_income_slashing_today, m_income_slashing, m_income_total_today, m_income_total}) label->setText(tr("–"));
        return;
    }
    const Income income{WalletIncome(*m_wallet_model, QDateTime(QDate::currentDate(), QTime(0, 0)))};
    const BitcoinUnit unit{m_wallet_model->getOptionsModel() ? m_wallet_model->getOptionsModel()->getDisplayUnit() : BitcoinUnit::BTC};
    const auto show{[&](QLabel* label, CAmount amount) { label->setText(BitcoinUnits::formatWithUnit(unit, amount)); }};
    show(m_income_staked_today, income.staked_today);
    show(m_income_staked, income.staked);
    show(m_income_anchor_today, income.anchor_today);
    show(m_income_anchor, income.anchor);
    show(m_income_slashing_today, income.slashing_today);
    show(m_income_slashing, income.slashing);
    show(m_income_total_today, income.staked_today + income.anchor_today + income.slashing_today);
    show(m_income_total, income.staked + income.anchor + income.slashing);
    m_income_staked->setToolTip(tr("From %n block(s) of stake whose producer's share this wallet received (%1 today)", "", income.blocks).arg(income.blocks_today));
}
