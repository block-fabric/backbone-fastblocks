// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_STAKINGPAGE_H
#define BITCOIN_QT_STAKINGPAGE_H

#include <univalue.h>

#include <QPointer>
#include <QWidget>

#include <optional>
#include <string>

class ClientModel;
class PlatformStyle;
class WalletModel;

QT_BEGIN_NAMESPACE
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
 *  - this node as a validator: its keys, what it made, its next slot; staking on and off;
 *  - finality: how far the chain is anchored and final, and the wallet's coins that are soft,
 *    anchored or final;
 *  - the validators of the epoch, and the newest anchors;
 *  - the wallet's bonds: bond coins to a validator key, unbond them.
 * It works through the node's RPC commands (getposinfo, getstakinginfo, setstaking, getvalidators,
 * getanchorinfo, getbalances, listbonds, createbond, createunbond), and refreshes every few seconds
 * while it is shown.
 */
class StakingPage : public QWidget
{
    Q_OBJECT

public:
    explicit StakingPage(const PlatformStyle* platform_style, QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model);
    void setWalletModel(WalletModel* wallet_model);

public Q_SLOTS:
    /** Reload everything shown from the node. */
    void refresh();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    //! What refresh does; it may throw on a reply of an unexpected shape.
    void refreshPage();
    /** Run an RPC command; nothing, with the error noted on the page, if it fails. */
    std::optional<UniValue> call(const std::string& method, const UniValue& params, bool wallet = false);
    void setStaking(bool enabled);
    /** Bond the amount entered to the key entered (this node's first validator key if none), after asking. */
    void bond();
    /** Unbond the amount entered (all of it if none) from the key entered, after asking. */
    void unbond();
    /** The key entered, or this node's first validator key. */
    QString bondKey();

    ClientModel* m_client_model{nullptr};
    QPointer<WalletModel> m_wallet_model;
    QTimer* m_timer{nullptr};

    QLabel* m_summary{nullptr};
    QLabel* m_staking_status{nullptr};
    QPushButton* m_staking_start{nullptr};
    QPushButton* m_staking_stop{nullptr};
    QLabel* m_finality{nullptr};
    QLabel* m_balance_soft{nullptr};
    QLabel* m_balance_anchored{nullptr};
    QLabel* m_balance_final{nullptr};
    QTableWidget* m_validators{nullptr};
    QTableWidget* m_anchors{nullptr};
    QLineEdit* m_bond_key{nullptr};
    QLineEdit* m_bond_amount{nullptr};
    QPushButton* m_bond_button{nullptr};
    QPushButton* m_unbond_button{nullptr};
    QTableWidget* m_bonds{nullptr};
    QLabel* m_error{nullptr};
};

#endif // BITCOIN_QT_STAKINGPAGE_H
