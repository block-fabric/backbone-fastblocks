// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/blockexplorer.h>

#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/itemviews.h>
#include <qt/noderpc.h>

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPolygonF>
#include <QPushButton>
#include <QRegularExpression>
#include <QShowEvent>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {
using NodeRpc::Args;
using NodeRpc::Item;
using NodeRpc::Text;

//! Most transactions of the mempool to list.
constexpr size_t MAX_MEMPOOL_ROWS{1000};

QTableWidget* Table(const QStringList& headers, QWidget* parent)
{
    auto* table{new ItemViews::Table(0, headers.size(), parent)};
    table->setHorizontalHeaderLabels(headers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    return table;
}

QString Time(int64_t time)
{
    return QLocale().toString(QDateTime::fromSecsSinceEpoch(time), QLocale::ShortFormat);
}

//! A key, shortened for a table: its first and last bytes.
QString ShortKey(const QString& key) { return key.size() > 20 ? key.left(8) + QStringLiteral("…") + key.right(6) : key; }

/**
 * The mark of a kind of block: a blue disc for a block of stake, an amber diamond for a merged mined
 * one. Drawn, with an outline, so that it reads on every theme (light or dark, and on style sheets
 * that colour the cells), and by its shape as well as its colour.
 */
QIcon KindIcon(const QString& kind)
{
    const bool pos{kind == QStringLiteral("pos") || kind == QStringLiteral("ack")};
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor fill{pos ? QColor(0x2f, 0x8f, 0xe0) : QColor(0xe8, 0x9a, 0x1c)};
    painter.setPen(QPen(fill.darker(160), 3));
    painter.setBrush(fill);
    if (pos) {
        painter.drawEllipse(QRectF(4, 4, 24, 24));
        // A block of stake that acknowledges an anchor: with the amber diamond of the proof of work.
        if (kind == QStringLiteral("ack")) {
            const QColor pow{0xe8, 0x9a, 0x1c};
            painter.setPen(QPen(pow.darker(160), 2));
            painter.setBrush(pow);
            painter.drawPolygon(QPolygonF{{QPointF(23, 13), QPointF(32, 22), QPointF(23, 31), QPointF(14, 22)}});
        }
    } else {
        painter.drawPolygon(QPolygonF{{QPointF(16, 2), QPointF(30, 16), QPointF(16, 30), QPointF(2, 16)}});
    }
    painter.end();
    return QIcon(pixmap);
}

QString FinalityTip()
{
    return QObject::tr("Soft: only blocks of stake are on it. BMM-confirmed: a BMM commitment on the mainchain covers it (a BMM block recorded one for it or a block after it). "
                       "Final: enough BMM commitments cover it.");
}
} // namespace

QString BlockExplorer::KindName(const QString& kind)
{
    if (kind == QStringLiteral("pos")) return tr("PoS");
    if (kind == QStringLiteral("bmm")) return tr("PoW");
    return {};
}

BlockExplorer::BlockExplorer(QWidget* parent) : QDialog(parent, GUIUtil::dialog_flags | Qt::WindowMinMaxButtonsHint)
{
    setWindowTitle(tr("Block Explorer"));
    auto* layout{new QVBoxLayout(this)};

    auto* top{new QHBoxLayout};
    m_search = new QLineEdit(this);
    m_search->setObjectName("explorerSearch");
    m_search->setPlaceholderText(tr("Block height, block hash or transaction id"));
    auto* search_button{new QPushButton(tr("Search"), this)};
    m_count = new QSpinBox(this);
    m_count->setRange(10, 1000);
    m_count->setValue(50);
    m_count->setPrefix(tr("Last "));
    m_count->setSuffix(tr(" blocks"));
    auto* refresh_button{new QPushButton(tr("Refresh"), this)};
    top->addWidget(m_search, 1);
    top->addWidget(search_button);
    top->addWidget(m_count);
    top->addWidget(refresh_button);
    layout->addLayout(top);

    auto* splitter{new QSplitter(Qt::Vertical, this)};
    m_tabs = new QTabWidget(splitter);
    m_blocks = Table({tr("Height"), tr("Kind"), tr("Slot"), tr("Producer"), tr("Mainchain block"), tr("Finality"), tr("Time"), tr("Transactions"), tr("Size"), tr("Hash")}, m_tabs);
    m_blocks->setObjectName("explorerBlocks");
    m_blocks->horizontalHeaderItem(COL_KIND)->setToolTip(tr("PoS: a block of stake, signed by the validator that leads its slot. PoW: a merged mined block, which the mainchain committed to."));
    m_blocks->horizontalHeaderItem(COL_SLOT)->setToolTip(tr("The slot of a block of stake"));
    m_blocks->horizontalHeaderItem(COL_PRODUCER)->setToolTip(tr("The validator key that signed a block of stake (\"this node\" for one of this node's keys)"));
    m_blocks->horizontalHeaderItem(COL_MAINCHAIN)->setToolTip(tr("The height of the mainchain block that committed to a merged mined block"));
    m_blocks->horizontalHeaderItem(COL_FINALITY)->setToolTip(FinalityTip());
    m_blocks->setIconSize(QSize(12, 12));
    m_tabs->addTab(m_blocks, tr("Blocks"));
    m_mempool = Table({tr("Received"), tr("Fee"), tr("Virtual size"), tr("Transaction id")}, m_tabs);
    m_mempool->setObjectName("explorerMempool");
    m_tabs->addTab(m_mempool, tr("Waiting transactions"));
    splitter->addWidget(m_tabs);

    auto* lower{new QWidget(splitter)};
    auto* lower_layout{new QVBoxLayout(lower)};
    lower_layout->setContentsMargins(0, 0, 0, 0);
    m_details_title = new QLabel(tr("Double-click a block or a transaction to see its details."), lower);
    m_details_title->setObjectName("explorerTitle");
    m_details_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_details_title->setWordWrap(true);
    lower_layout->addWidget(m_details_title);
    m_summary = new QLabel(lower);
    m_summary->setObjectName("explorerSummary");
    m_summary->setTextFormat(Qt::RichText);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_summary->setWordWrap(true);
    m_summary->hide();
    lower_layout->addWidget(m_summary);
    auto* details_splitter{new QSplitter(Qt::Horizontal, lower)};
    m_block_txs = new ItemViews::List(details_splitter);
    m_block_txs->setObjectName("explorerBlockTxs");
    m_block_txs->setToolTip(tr("Transactions of the block. Click one to see it."));
    m_details = new QPlainTextEdit(details_splitter);
    m_details->setObjectName("explorerDetails");
    m_details->setReadOnly(true);
    m_details->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_details->setFont(GUIUtil::fixedPitchFont());
    details_splitter->addWidget(m_block_txs);
    details_splitter->addWidget(m_details);
    details_splitter->setStretchFactor(1, 2);
    lower_layout->addWidget(details_splitter, 1);
    splitter->addWidget(lower);
    layout->addWidget(splitter, 1);

    connect(search_button, &QPushButton::clicked, this, [this] { search(m_search->text()); });
    connect(m_search, &QLineEdit::returnPressed, this, [this] { search(m_search->text()); });
    connect(refresh_button, &QPushButton::clicked, this, &BlockExplorer::refresh);
    connect(m_count, qOverload<int>(&QSpinBox::valueChanged), this, [this] { refreshBlocks(); });
    connect(m_blocks, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (auto* item{m_blocks->item(row, COL_HASH)}) showBlock(item->text());
    });
    connect(m_mempool, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        m_block_txs->clear();
        m_shown_block.clear();
        if (auto* item{m_mempool->item(row, 3)}) showTransaction(item->text(), {});
    });
    connect(m_block_txs, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) { showTransaction(item->text(), m_shown_block); });

    resize(1100, 700);
    GUIUtil::handleCloseWindowShortcut(this);
}

void BlockExplorer::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
    if (client_model) {
        connect(client_model, &ClientModel::numBlocksChanged, this, [this] { if (isVisible()) refreshBlocks(); });
    }
}

void BlockExplorer::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    refresh();
}

void BlockExplorer::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        // After the style has laid the cells out anew.
        QTimer::singleShot(0, this, [this] {
            m_blocks->resizeColumnsToContents();
            m_mempool->resizeColumnsToContents();
        });
    }
}

void BlockExplorer::refresh()
{
    refreshBlocks();
    refreshMempool();
}

BlockExplorer::Snapshot BlockExplorer::Fetch(const CallFn& call, int count, std::vector<BlockRow> last)
{
    Snapshot snapshot;
    // The UniValue getters throw on a field of another type or a missing one: what is not as
    // expected is left out (the keys, the anchors) or left as it was (the blocks).
    try {
        if (const auto staking{call("getstakinginfo", Args({}))}; staking && staking->isObject()) {
            for (const UniValue& key : staking->find_value("keys").getValues()) snapshot.local_keys.insert(Text(key["pubkey"]));
        }
    } catch (const std::exception&) {
    }
    try {
        if (const auto anchors{call("getanchorinfo", Args({1}))}; anchors && anchors->isObject()) {
            snapshot.anchored_height = (*anchors)["anchoredheight"].getInt<int>();
            snapshot.final_height = (*anchors)["finalheight"].getInt<int>();
        }
    } catch (const std::exception&) {
        snapshot.anchored_height.reset();
        snapshot.final_height.reset();
    }
    try {
        const auto best{call("getbestblockhash", Args({}))};
        QString next{best && best->isStr() ? Text(*best) : QString{}};
        while (static_cast<int>(snapshot.blocks.size()) < count && !next.isEmpty()) {
            // A block read last time: so were the ones before it, in order.
            const auto known{std::find_if(last.begin(), last.end(), [&](const BlockRow& row) { return row.hash == next; })};
            if (known != last.end()) {
                for (auto it{known}; it != last.end() && static_cast<int>(snapshot.blocks.size()) < count; ++it) snapshot.blocks.push_back(*it);
                // Fewer than asked for: the rest are read below.
                if (static_cast<int>(snapshot.blocks.size()) >= count || snapshot.blocks.back().height == 0) break;
                const auto header{call("getblockheader", Args({snapshot.blocks.back().hash.toStdString()}))};
                if (!header || !header->isObject()) break;
                const UniValue& previous{header->find_value("previousblockhash")};
                next = previous.isStr() ? Text(previous) : QString{};
                last.clear();
                continue;
            }
            const auto block{call("getblock", Args({next.toStdString(), 1}))};
            if (!block || !block->isObject()) break;
            const UniValue& b{*block};
            BlockRow row;
            row.hash = next;
            row.height = b["height"].getInt<int>();
            row.time = b["time"].getInt<int64_t>();
            row.txs = Text(b["nTx"]);
            row.size = Text(b["size"]);
            if (b.find_value("kind").isStr()) row.kind = Text(b.find_value("kind"));
            if (b.find_value("slot").isNum()) row.slot = Text(b.find_value("slot"));
            if (b.find_value("proposer").isStr()) row.proposer = Text(b.find_value("proposer"));
            if (b.find_value("mainheight").isNum()) row.main_height = Text(b.find_value("mainheight"));
            if (const UniValue& ack{b.find_value("ack")}; ack.isObject()) {
                row.ack = true;
                if (ack.find_value("mainheight").isNum()) row.main_height = Text(ack.find_value("mainheight"));
            }
            const UniValue& finality{b.find_value("finality")};
            if (finality.isObject() && finality.find_value("status").isStr()) row.finality = Text(finality.find_value("status"));
            snapshot.blocks.push_back(std::move(row));
            const UniValue& previous{b.find_value("previousblockhash")};
            next = previous.isStr() ? Text(previous) : QString{};
        }
    } catch (const std::exception&) {
        snapshot.blocks = std::move(last);
    }
    return snapshot;
}

void BlockExplorer::refreshBlocks()
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
    NodeRpc::RunAsync(this, [this, client_model, count = m_count->value(), last = m_last.blocks]() mutable -> std::function<void()> {
        Snapshot snapshot{Fetch([client_model](const std::string& method, const UniValue& params) {
            QString error;
            return NodeRpc::Call(client_model, method, params, error);
        }, count, std::move(last))};
        return [this, snapshot = std::move(snapshot)]() mutable {
            m_fetching = false;
            applyBlocks(std::move(snapshot));
            if (std::exchange(m_refresh_again, false)) refreshBlocks();
        };
    });
}

QString BlockExplorer::finalityOf(const BlockRow& row) const
{
    // As of the last refresh: blocks on the active chain, up to the anchored and final tips.
    if (!row.kind.isEmpty() && m_last.anchored_height && m_last.final_height) {
        if (row.height <= *m_last.final_height) return tr("final");
        if (row.height <= *m_last.anchored_height) return tr("BMM-confirmed");
        return tr("soft");
    }
    if (row.finality == QStringLiteral("final")) return tr("final");
    if (row.finality == QStringLiteral("anchored")) return tr("BMM-confirmed");
    if (row.finality == QStringLiteral("soft")) return tr("soft");
    return row.finality;
}

QString BlockExplorer::producerName(const QString& pubkey) const
{
    if (pubkey.isEmpty()) return {};
    return m_last.local_keys.contains(pubkey) ? tr("this node") : ShortKey(pubkey);
}

void BlockExplorer::applyBlocks(Snapshot snapshot)
{
    m_last = std::move(snapshot);
    const int rows{static_cast<int>(m_last.blocks.size())};
    m_blocks->setRowCount(rows);
    const QIcon pos_icon{KindIcon(QStringLiteral("pos"))}, pow_icon{KindIcon(QStringLiteral("bmm"))}, ack_icon{KindIcon(QStringLiteral("ack"))};
    for (int row{0}; row < rows; ++row) {
        const BlockRow& block{m_last.blocks[row]};
        m_blocks->setItem(row, COL_HEIGHT, Item(QString::number(block.height)));
        auto* kind{Item(KindName(block.kind))};
        if (block.kind == QStringLiteral("pos") && block.ack) {
            kind->setText(tr("BMM"));
            kind->setIcon(ack_icon);
            kind->setToolTip(tr("A BMM block: it records the BMM commitment mainchain block %1 made for this chain, which ties it to the mainchain; deposits and withdrawals happen in it").arg(block.main_height));
        } else if (block.kind == QStringLiteral("pos")) {
            kind->setIcon(pos_icon);
            kind->setToolTip(tr("A block of stake"));
        } else if (block.kind == QStringLiteral("bmm")) {
            kind->setIcon(pow_icon);
            kind->setToolTip(tr("A merged mined block (proof of work): made only while no blocks of stake come"));
        }
        m_blocks->setItem(row, COL_KIND, kind);
        m_blocks->setItem(row, COL_SLOT, Item(block.slot));
        auto* producer{Item(producerName(block.proposer))};
        producer->setToolTip(block.proposer);
        m_blocks->setItem(row, COL_PRODUCER, producer);
        auto* main{Item(block.main_height)};
        if (block.ack) main->setToolTip(tr("The mainchain block whose BMM commitment this BMM block records"));
        m_blocks->setItem(row, COL_MAINCHAIN, main);
        m_blocks->setItem(row, COL_FINALITY, Item(finalityOf(block)));
        m_blocks->setItem(row, COL_TIME, Item(Time(block.time)));
        m_blocks->setItem(row, COL_TXS, Item(block.txs));
        m_blocks->setItem(row, COL_SIZE, Item(block.size));
        m_blocks->setItem(row, COL_HASH, Item(block.hash));
    }
    // The columns a chain without blocks of stake has nothing for.
    const bool fastblocks{std::any_of(m_last.blocks.begin(), m_last.blocks.end(), [](const BlockRow& row) { return !row.kind.isEmpty(); })};
    for (const int column : {COL_KIND, COL_SLOT, COL_PRODUCER, COL_MAINCHAIN, COL_FINALITY}) m_blocks->setColumnHidden(column, !fastblocks);
    m_blocks->resizeColumnsToContents();
}

void BlockExplorer::refreshMempool()
{
    if (!m_client_model || m_fetching_mempool) return;
    m_fetching_mempool = true;
    NodeRpc::CallAsync(this, m_client_model, "getrawmempool", Args({true}), [this](std::optional<UniValue> mempool, const QString&) {
        m_fetching_mempool = false;
        if (!mempool || !mempool->isObject()) return;
        try {
            const auto& txids{mempool->getKeys()};
            const size_t rows{std::min(txids.size(), MAX_MEMPOOL_ROWS)};
            m_mempool->setRowCount(rows);
            for (size_t row{0}; row < rows; ++row) {
                const UniValue& entry{(*mempool)[txids[row]]};
                m_mempool->setItem(row, 0, Item(Time(entry["time"].getInt<int64_t>())));
                m_mempool->setItem(row, 1, Item(Text(entry["fees"]["base"])));
                m_mempool->setItem(row, 2, Item(Text(entry["vsize"])));
                m_mempool->setItem(row, 3, Item(QString::fromStdString(txids[row])));
            }
            m_mempool->resizeColumnsToContents();
            m_tabs->setTabText(1, tr("Waiting transactions (%1)").arg(txids.size()));
        } catch (const std::exception&) {
        }
    });
}

void BlockExplorer::showBlock(const QString& hash, std::function<void(bool)> found)
{
    NodeRpc::CallAsync(this, m_client_model, "getblock", Args({hash.toStdString(), 1}), [this, hash, found = std::move(found)](std::optional<UniValue> block, const QString&) {
        const bool ok{block && block->isObject()};
        if (ok) displayBlock(hash, *block);
        if (found) found(ok);
    });
}

void BlockExplorer::displayBlock(const QString& hash, const UniValue& block)
{
    m_shown_block = hash;
    m_block_txs->clear();
    UniValue rest(UniValue::VOBJ);
    for (const auto& key : block.getKeys()) {
        if (key == "tx") {
            for (const UniValue& txid : block[key].getValues()) m_block_txs->addItem(Text(txid));
        } else {
            rest.pushKV(key, block[key]);
        }
    }
    m_details_title->setText(tr("Block %1: %2").arg(Text(block.find_value("height")), hash));

    // A summary of what FastBlocks says of it, above the whole of it.
    QStringList lines;
    const QString kind{block.find_value("kind").isStr() ? Text(block.find_value("kind")) : QString{}};
    const auto escape{[](const QString& text) { return text.toHtmlEscaped(); }};
    if (kind == QStringLiteral("pos")) {
        const QString proposer{block.find_value("proposer").isStr() ? Text(block.find_value("proposer")) : QString{}};
        QString who{proposer.isEmpty() ? tr("unknown") : producerName(proposer)};
        if (!proposer.isEmpty() && who == tr("this node")) who = tr("this node, %1").arg(ShortKey(proposer));
        lines << tr("<b>Kind:</b> %1").arg(escape(tr("PoS (slot %1, producer %2)").arg(Text(block.find_value("slot")), who)));
        const UniValue& ack{block.find_value("ack")};
        if (ack.isObject()) {
            lines << tr("<b>BMM block:</b> %1").arg(escape(tr("yes: it records the BMM commitment of mainchain block %1, which covers block %2. Deposits and withdrawals happen here, and the fee pot goes to the BMM bidder")
                                                            .arg(Text(ack.find_value("mainheight")), ShortKey(Text(ack.find_value("anchored"))))));
        } else {
            lines << tr("<b>BMM block:</b> %1").arg(escape(tr("no (it records no BMM commitment)")));
        }
    } else if (kind == QStringLiteral("bmm")) {
        const UniValue& main{block.find_value("mainheight")};
        lines << tr("<b>Kind:</b> %1").arg(escape(main.isNum() ? tr("PoW (merge-mined, committed at mainchain block %1)").arg(Text(main)) : tr("PoW (merge-mined)")));
        lines << tr("<b>BMM block:</b> %1").arg(escape(tr("a merged mined block, made only while no blocks of stake come: its own BMM commitment covers its parent. Deposits and withdrawals happen here, and the fee pot goes to its first output")));
    }
    if (!kind.isEmpty()) {
        const UniValue& finality{block.find_value("finality")};
        if (finality.isObject()) {
            const QString status{Text(finality.find_value("status"))};
            const QString word{status == QStringLiteral("final") ? tr("final") : status == QStringLiteral("anchored") ? tr("BMM-confirmed") : tr("soft")};
            lines << tr("<b>Finality:</b> %1").arg(escape(tr("%1 (%2 BMM commitment(s) cover it)").arg(word, Text(finality.find_value("anchors")))));
        } else {
            lines << tr("<b>Finality:</b> %1").arg(escape(tr("not on the active chain")));
        }
        const UniValue& evidence{block.find_value("evidence")};
        if (evidence.isArray() && !evidence.empty()) lines << tr("<b>Proofs:</b> %1").arg(escape(tr("%n proof(s) that a leader signed two blocks for a slot", "", evidence.size())));
    }
    m_summary->setText(lines.join(QStringLiteral("<br>")));
    m_summary->setVisible(!lines.isEmpty());
    m_details->setPlainText(QString::fromStdString(rest.write(2)));
}

void BlockExplorer::showTransaction(const QString& txid, const QString& block_hash, std::function<void(bool)> found)
{
    UniValue params{Args({txid.toStdString(), true})};
    if (!block_hash.isEmpty()) params.push_back(block_hash.toStdString());
    NodeRpc::CallAsync(this, m_client_model, "getrawtransaction", params, [this, txid, found = std::move(found)](std::optional<UniValue> tx, const QString&) {
        const bool ok{tx && tx->isObject()};
        if (ok) displayTransaction(txid, *tx);
        if (found) found(ok);
    });
}

void BlockExplorer::displayTransaction(const QString& txid, const UniValue& tx)
{
    // Everything but the serialized transaction, which is long and says nothing new.
    UniValue shown(UniValue::VOBJ);
    for (const auto& key : tx.getKeys()) {
        if (key != "hex") shown.pushKV(key, tx[key]);
    }
    m_details_title->setText(tr("Transaction %1").arg(txid));
    m_summary->clear();
    m_summary->hide();
    m_details->setPlainText(QString::fromStdString(shown.write(2)));
}

void BlockExplorer::search(const QString& input)
{
    const QString query{input.trimmed()};
    if (query.isEmpty()) return;
    static const QRegularExpression number{QStringLiteral("^[0-9]{1,9}$")};
    static const QRegularExpression hash{QStringLiteral("^[0-9a-fA-F]{64}$")};
    const QPointer<BlockExplorer> self{this};
    const auto tell{[self](const QString& error) {
        if (self) QMessageBox::information(self, self->windowTitle(), error);
    }};
    if (number.match(query).hasMatch()) {
        NodeRpc::CallAsync(this, m_client_model, "getblockhash", Args({query.toInt()}), [this, query, tell](std::optional<UniValue> block_hash, const QString& error) {
            if (!block_hash || !block_hash->isStr()) return tell(error.isEmpty() ? tr("No block at height %1.").arg(query) : error);
            showBlock(Text(*block_hash), [tell, query](bool found) {
                if (!found) tell(QObject::tr("No block at height %1.").arg(query));
            });
        });
    } else if (hash.match(query).hasMatch()) {
        const QString id{query.toLower()};
        showBlock(id, [this, id, tell](bool found) {
            if (found) return;
            m_block_txs->clear();
            m_shown_block.clear();
            showTransaction(id, {}, [tell](bool found_tx) {
                if (!found_tx) {
                    tell(QObject::tr("No block has this hash, and no transaction with this id is waiting to be mined. "
                                     "Finding a mined transaction by its id alone requires the node to run with -txindex."));
                }
            });
        });
    } else {
        tell(tr("Enter a block height, a block hash or a transaction id."));
    }
}
