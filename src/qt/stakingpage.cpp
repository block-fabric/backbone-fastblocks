// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/stakingpage.h>

#include <qt/clientmodel.h>
#include <qt/itemviews.h>
#include <qt/noderpc.h>
#include <qt/walletmodel.h>

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
using NodeRpc::Args;
using NodeRpc::Item;
using NodeRpc::Text;

constexpr int REFRESH_INTERVAL_MS{3000};

//! An amount of a reply; one of another type is shown as it is.
QString Amount(const UniValue& value) { return value.isNum() ? QString::number(value.get_real(), 'f', 8) : Text(value); }

//! A key, shortened for a table: its first and last bytes.
QString ShortKey(const QString& key) { return key.size() > 20 ? key.left(10) + QStringLiteral("…") + key.right(8) : key; }

QTableWidget* MakeTable(QWidget* parent, const QString& name, const QStringList& headers)
{
    auto* table{new ItemViews::Table(0, headers.size(), parent)};
    table->setObjectName(name);
    table->setHorizontalHeaderLabels(headers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    return table;
}
} // namespace

StakingPage::StakingPage(const PlatformStyle*, QWidget* parent) : QWidget(parent)
{
    auto* layout{new QVBoxLayout(this)};

    m_summary = new QLabel(this);
    m_summary->setObjectName("stakingSummary");
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_summary);

    auto* top{new QHBoxLayout};
    auto* validator_box{new QGroupBox(tr("This node as a validator"), this)};
    auto* validator_layout{new QVBoxLayout(validator_box)};
    m_staking_status = new QLabel(validator_box);
    m_staking_status->setObjectName("stakingStatus");
    m_staking_status->setWordWrap(true);
    m_staking_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
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
    validator_layout->addStretch();
    top->addWidget(validator_box, 1);

    auto* finality_box{new QGroupBox(tr("Finality"), this)};
    auto* finality_form{new QFormLayout(finality_box)};
    m_finality = new QLabel(finality_box);
    m_finality->setObjectName("finalityStatus");
    m_finality->setWordWrap(true);
    m_finality->setToolTip(tr("Soft: only blocks of stake are on it, like an unconfirmed payment with slashing behind it. "
                              "Anchored: the mainchain committed to a block at or after it; undoing it takes the mainchain's merged miners, not stake. "
                              "Final: enough anchors cover it."));
    finality_form->addRow(m_finality);
    m_balance_soft = new QLabel(finality_box);
    m_balance_soft->setObjectName("balanceSoft");
    m_balance_anchored = new QLabel(finality_box);
    m_balance_anchored->setObjectName("balanceAnchored");
    m_balance_final = new QLabel(finality_box);
    m_balance_final->setObjectName("balanceFinal");
    finality_form->addRow(tr("Soft:"), m_balance_soft);
    finality_form->addRow(tr("Anchored:"), m_balance_anchored);
    finality_form->addRow(tr("Final:"), m_balance_final);
    top->addWidget(finality_box, 1);
    layout->addLayout(top);

    auto* tables{new QHBoxLayout};
    auto* validators_box{new QGroupBox(tr("Validators of this epoch"), this)};
    auto* validators_layout{new QVBoxLayout(validators_box)};
    m_validators = MakeTable(validators_box, "validators", {tr("Key"), tr("Stake"), tr("Slots"), tr("This node")});
    validators_layout->addWidget(m_validators);
    tables->addWidget(validators_box, 1);
    auto* anchors_box{new QGroupBox(tr("Newest anchors"), this)};
    auto* anchors_layout{new QVBoxLayout(anchors_box)};
    m_anchors = MakeTable(anchors_box, "anchors", {tr("Block"), tr("Kind"), tr("Anchors block"), tr("Mainchain block")});
    anchors_layout->addWidget(m_anchors);
    tables->addWidget(anchors_box, 1);
    layout->addLayout(tables, 1);

    m_error = new QLabel(this);
    m_error->setObjectName("stakingError");
    m_error->setWordWrap(true);
    layout->addWidget(m_error);

    connect(m_staking_start, &QPushButton::clicked, this, [this] { setStaking(true); });
    connect(m_staking_stop, &QPushButton::clicked, this, [this] { setStaking(false); });

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

std::optional<UniValue> StakingPage::call(const std::string& method, const UniValue& params, bool wallet)
{
    QString error;
    std::optional<QString> wallet_name;
    if (wallet) {
        if (!m_wallet_model) return std::nullopt;
        wallet_name = m_wallet_model->getWalletName();
    }
    auto result{NodeRpc::Call(m_client_model, method, params, error, wallet_name)};
    if (!result) m_error->setText(tr("%1: %2").arg(QString::fromStdString(method), error));
    return result;
}

void StakingPage::setStaking(bool enabled)
{
    if (!m_client_model) return;
    m_error->clear();
    if (call("setstaking", Args({enabled}))) refresh();
}

void StakingPage::refresh()
{
    if (!m_client_model) return;
    // A reply of an unexpected shape makes the UniValue getters throw: not out of this slot.
    try {
        refreshPage();
    } catch (const std::exception& e) {
        m_error->setText(tr("Unexpected reply from the node: %1").arg(QString::fromStdString(e.what())));
    }
}

void StakingPage::refreshPage()
{
    m_error->clear();
    const auto pos{call("getposinfo", Args({}))};
    if (!pos) {
        m_summary->setText(tr("This chain has no blocks of stake."));
        return;
    }
    const auto& info{*pos};
    QString summary;
    if (info["active"].get_bool()) {
        summary = tr("Blocks of stake come every %1 s. Slot %2, epoch %3, led by %4.")
                      .arg(Text(info["slotseconds"]), Text(info["slot"]), Text(info["epoch"]), ShortKey(Text(info.find_value("leader"))));
    } else {
        summary = tr("Blocks of stake are off on this chain (too few validators): its blocks are merged mined only.");
    }
    summary += QStringLiteral(" ") + tr("%1 anchors; the last one committed at mainchain block %2.").arg(Text(info["anchors"]), Text(info["lastanchormainheight"]));
    if (info["stalled"].get_bool() && info["active"].get_bool()) summary += QStringLiteral(" ") + tr("No blocks of stake lately: merged mined blocks carry the transactions.");
    m_summary->setText(summary);

    if (const auto staking{call("getstakinginfo", Args({}))}) {
        const auto& s{*staking};
        QString status;
        if (!s["validator"].get_bool()) {
            status = tr("This node has no validator keys. Start it with -validatorkeyfile to make blocks of stake.");
            if (!Text(s["producererror"]).isEmpty()) status += QStringLiteral(" ") + Text(s["producererror"]);
        } else {
            QStringList keys;
            for (const UniValue& key : s["keys"].getValues()) {
                keys << ShortKey(Text(key["pubkey"])) + (key["inset"].get_bool() ? QString{} : tr(" (not in the set)"));
            }
            status = tr("Staking is %1. Keys: %2. Stake %3 of %4.")
                         .arg(s["enabled"].get_bool() ? tr("on") : tr("off"), keys.join(QStringLiteral(", ")), Text(s["stake"]), Text(s["totalstake"]));
            status += QStringLiteral("\n") + tr("Made %1 block(s) since the node started, %2 of them acknowledgements; missed %3 slot(s).")
                                                  .arg(Text(s["produced"]), Text(s["acknowledgements"]), Text(s["failed"]));
            if (!s.find_value("nextslot").isNull()) {
                status += QStringLiteral("\n") + tr("Next slot it leads: %1, in %2 slot(s).")
                                                      .arg(Text(s["nextslot"]), QString::number(s["nextslot"].getInt<int64_t>() - s["slot"].getInt<int64_t>()));
            }
            if (!Text(s["error"]).isEmpty()) status += QStringLiteral("\n") + tr("Last problem: %1").arg(Text(s["error"]));
        }
        m_staking_status->setText(status);
        m_staking_start->setEnabled(s["validator"].get_bool() && !s["enabled"].get_bool());
        m_staking_stop->setEnabled(s["validator"].get_bool() && s["enabled"].get_bool());
    }

    if (const auto anchors{call("getanchorinfo", Args({10}))}) {
        const auto& a{*anchors};
        const auto chain{call("getblockchaininfo", Args({}))};
        const QString height{chain ? Text((*chain)["blocks"]) : QStringLiteral("?")};
        m_finality->setText(tr("Best block %1. Anchored up to block %2, final up to block %3 (%4 anchor(s) make a block final).")
                                .arg(height, Text(a["anchoredheight"]), Text(a["finalheight"]), Text(a["finalanchors"])));
        const UniValue& recent{a["recent"]};
        m_anchors->setRowCount(recent.size());
        for (size_t i{0}; i < recent.size(); ++i) {
            const UniValue& anchor{recent[i]};
            const int row{static_cast<int>(i)};
            m_anchors->setItem(row, 0, Item(Text(anchor["height"])));
            m_anchors->setItem(row, 1, Item(Text(anchor["kind"]) == QStringLiteral("bmm") ? tr("merged mined block") : tr("ticket")));
            m_anchors->setItem(row, 2, Item(Text(anchor["anchoredheight"])));
            m_anchors->setItem(row, 3, Item(Text(anchor["mainheight"])));
            m_anchors->item(row, 0)->setToolTip(Text(anchor["block"]));
            m_anchors->item(row, 2)->setToolTip(Text(anchor["anchored"]));
        }
    }

    if (const auto validators{call("getvalidators", Args({}))}) {
        const UniValue& list{(*validators)["validators"]};
        m_validators->setRowCount(list.size());
        for (size_t i{0}; i < list.size(); ++i) {
            const UniValue& validator{list[i]};
            const int row{static_cast<int>(i)};
            m_validators->setItem(row, 0, Item(ShortKey(Text(validator["pubkey"]))));
            m_validators->item(row, 0)->setToolTip(Text(validator["pubkey"]));
            m_validators->setItem(row, 1, Item(Text(validator["stake"])));
            m_validators->setItem(row, 2, Item(Text(validator["slots"])));
            m_validators->setItem(row, 3, Item(validator["local"].get_bool() ? tr("yes") : QString{}));
        }
    }

    // The wallet's coins by finality.
    if (m_wallet_model) {
        if (const auto balances{call("getbalances", Args({}), /*wallet=*/true)}) {
            const UniValue& finality{(*balances)["mine"].find_value("finality")};
            if (finality.isObject()) {
                m_balance_soft->setText(Amount(finality["soft"]));
                m_balance_anchored->setText(Amount(finality["anchored"]));
                m_balance_final->setText(Amount(finality["final"]));
            }
        }
    }
}
