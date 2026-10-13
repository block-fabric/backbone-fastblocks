// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_STAKINGPAGE_H
#define BITCOIN_QT_STAKINGPAGE_H

#include <consensus/amount.h>
#include <univalue.h>

#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <optional>
#include <string>

class ClientModel;
class PlatformStyle;
class WalletModel;

QT_BEGIN_NAMESPACE
class QDateTime;
class QHideEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QTableWidget;
class QTimer;
QT_END_NAMESPACE

/**
 * FastBlocks' blocks of stake, a page of the wallet:
 *  - the chain: whether blocks of stake are made, the slot, its leader, the last anchor;
 *  - this node as a validator: its keys, their bonds (bonded, unbonding, eligible), whether they are in
 *    the set, jailed or banned, their share of the set's stake, the next slot they lead, the blocks
 *    they made and the slots they missed; staking on and off;
 *  - staking income: what the wallet received as the producer's share of blocks of stake, as an
 *    anchor's payee (the pot) and as a reporter of a double signature, today and in all;
 *  - finality: how far the chain is anchored and final, and the wallet's coins that are soft,
 *    anchored or final;
 *  - the validator set of the epoch, and the newest anchors;
 *  - the wallet's bonds: bond coins to a validator key, add to a bond, unbond some or all of it.
 * It works through the node's RPC commands (getposinfo, getstakinginfo, setstaking, getvalidators,
 * getanchorinfo, getbalances, listbonds, createbond, createunbond), run off the GUI thread
 * (NodeRpc::RunAsync), and the wallet's transactions; it refreshes every few seconds while shown.
 */
class StakingPage : public QWidget
{
    Q_OBJECT

public:
    explicit StakingPage(const PlatformStyle* platform_style, QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model);
    void setWalletModel(WalletModel* wallet_model);

    /** The wallet's staking income, from its transactions. */
    struct Income {
        CAmount staked_today{0}, staked{0};
        CAmount anchor_today{0}, anchor{0};
        CAmount slashing_today{0}, slashing{0};
        int blocks_today{0}, blocks{0};
    };
    /** Add up the staking income of the wallet's transactions, today being from `midnight` on. */
    static Income WalletIncome(WalletModel& wallet_model, const QDateTime& midnight);

public Q_SLOTS:
    /** Reload everything shown from the node, off the GUI thread. */
    void refresh();

Q_SIGNALS:
    /** A refresh is shown. */
    void refreshed();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    //! The replies of one refresh (nothing for a call that failed, with its error).
    struct Snapshot {
        std::optional<UniValue> pos, staking, anchors, chain, validators, bonds, balances;
        QStringList errors;
    };
    //! Show a refresh; it may throw on a reply of an unexpected shape.
    void apply(const Snapshot& snapshot);
    void showValidator(const UniValue& staking, const UniValue* pos);
    void showIncome();
    /** Run a wallet RPC command off the GUI thread; `done` gets its result, or a box tells the error. */
    void walletCall(const std::string& method, const UniValue& params, const QString& failure, std::function<void(const UniValue&)> done);
    void setStaking(bool enabled);
    /** Bond the amount entered to the key (this node's first validator key if none), after asking. */
    void bond(bool add);
    /** Unbond the amount entered, or all of the bond, from the key, after asking. */
    void unbond(bool all);
    /** The key entered, else the bond selected, else this node's first validator key. */
    QString bondKey() const;
    /** What the wallet's bond to a key is, as last listed, if it has one. */
    std::optional<UniValue> walletBond(const QString& key) const;
    void tell(const QString& text, bool error);

    ClientModel* m_client_model{nullptr};
    QPointer<WalletModel> m_wallet_model;
    QTimer* m_timer{nullptr};
    bool m_fetching{false};
    bool m_refresh_again{false};
    //! This node's validator keys, and the wallet's bonds, as last fetched.
    QStringList m_local_keys;
    UniValue m_last_bonds{UniValue::VARR};

    QLabel* m_summary{nullptr};
    QLabel* m_staking_status{nullptr};
    QPushButton* m_staking_start{nullptr};
    QPushButton* m_staking_stop{nullptr};
    QLabel* m_keys{nullptr};
    QLabel* m_bonded{nullptr};
    QLabel* m_unbonding{nullptr};
    QLabel* m_eligible{nullptr};
    QLabel* m_in_set{nullptr};
    QLabel* m_jail{nullptr};
    QLabel* m_share{nullptr};
    QLabel* m_next_slot{nullptr};
    QLabel* m_blocks_made{nullptr};
    QLabel* m_missed{nullptr};
    QLabel* m_income_staked_today{nullptr};
    QLabel* m_income_staked{nullptr};
    QLabel* m_income_anchor_today{nullptr};
    QLabel* m_income_anchor{nullptr};
    QLabel* m_income_slashing_today{nullptr};
    QLabel* m_income_slashing{nullptr};
    QLabel* m_income_total_today{nullptr};
    QLabel* m_income_total{nullptr};
    QLabel* m_finality{nullptr};
    QLabel* m_balance_soft{nullptr};
    QLabel* m_balance_anchored{nullptr};
    QLabel* m_balance_final{nullptr};
    QTableWidget* m_validators{nullptr};
    QTableWidget* m_anchors{nullptr};
    QLineEdit* m_bond_key{nullptr};
    QLineEdit* m_bond_amount{nullptr};
    QPushButton* m_bond_button{nullptr};
    QPushButton* m_add_bond_button{nullptr};
    QPushButton* m_unbond_button{nullptr};
    QPushButton* m_unbond_all_button{nullptr};
    QTableWidget* m_bonds{nullptr};
    QLabel* m_error{nullptr};
};

#endif // BITCOIN_QT_STAKINGPAGE_H
