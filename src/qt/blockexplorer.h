// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_BLOCKEXPLORER_H
#define BITCOIN_QT_BLOCKEXPLORER_H

#include <univalue.h>

#include <QDialog>
#include <QSet>
#include <QString>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class ClientModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;
class QTableWidget;
class QTabWidget;
QT_END_NAMESPACE

/**
 * Window to browse the blocks of the chain and the transactions waiting to be mined. On FastBlocks,
 * each block shows its kind (a block of stake, PoS, or a merged mined one, PoW), the slot and producer
 * of a block of stake, the mainchain block that committed to a merged mined one, and how final it is.
 * What it shows comes from the node's RPC commands, run off the GUI thread (NodeRpc::RunAsync).
 */
class BlockExplorer : public QDialog
{
    Q_OBJECT

public:
    explicit BlockExplorer(QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model);

    //! The columns of the list of blocks.
    enum BlockColumn { COL_HEIGHT, COL_KIND, COL_SLOT, COL_PRODUCER, COL_MAINCHAIN, COL_FINALITY, COL_TIME, COL_TXS, COL_SIZE, COL_HASH, BLOCK_COLUMNS };

    /** A block of the list, as the node gave it. */
    struct BlockRow {
        int height{0};
        QString hash;
        //! "pos", "bmm", or empty on a chain without blocks of stake.
        QString kind;
        QString slot;
        QString proposer;
        //! The mainchain height that committed to a merged mined block, if known.
        QString main_height;
        //! What getblock said of its finality, when it was read.
        QString finality;
        int64_t time{0};
        QString txs;
        QString size;
    };

    /** The words for a kind of block: "PoS", "PoW". */
    static QString KindName(const QString& kind);

public Q_SLOTS:
    void refresh();
    /** Show a block, given its height or hash, or a transaction, given its id. */
    void search(const QString& query);

protected:
    void showEvent(QShowEvent* event) override;
    //! Another theme or font: the columns fit their contents again.
    void changeEvent(QEvent* event) override;

private:
    //! What one refresh of the list of blocks fetches.
    struct Snapshot {
        std::vector<BlockRow> blocks;
        //! This node's validator keys.
        QSet<QString> local_keys;
        //! How far the chain is anchored and final, if it has anchors.
        std::optional<int> anchored_height, final_height;
    };
    using CallFn = std::function<std::optional<UniValue>(const std::string& method, const UniValue& params)>;
    /** Fetch the newest `count` blocks, reusing what `last` read of the same blocks (off the GUI thread). */
    static Snapshot Fetch(const CallFn& call, int count, std::vector<BlockRow> last);

    void refreshBlocks();
    void refreshMempool();
    void applyBlocks(Snapshot snapshot);
    QString finalityOf(const BlockRow& row) const;
    QString producerName(const QString& pubkey) const;
    /** Show a block, given its hash, off the GUI thread; `found` is told whether it was. */
    void showBlock(const QString& hash, std::function<void(bool)> found = {});
    void showTransaction(const QString& txid, const QString& block_hash, std::function<void(bool)> found = {});
    void displayBlock(const QString& hash, const UniValue& block);
    void displayTransaction(const QString& txid, const UniValue& tx);

    ClientModel* m_client_model{nullptr};
    QLineEdit* m_search;
    QSpinBox* m_count;
    QTabWidget* m_tabs;
    QTableWidget* m_blocks;
    QTableWidget* m_mempool;
    QLabel* m_details_title;
    QLabel* m_summary;
    QListWidget* m_block_txs;
    QPlainTextEdit* m_details;
    //! Hash of the block whose transactions are listed.
    QString m_shown_block;
    //! The last list of blocks fetched.
    Snapshot m_last;
    bool m_fetching{false};
    bool m_refresh_again{false};
    bool m_fetching_mempool{false};
};

#endif // BITCOIN_QT_BLOCKEXPLORER_H
