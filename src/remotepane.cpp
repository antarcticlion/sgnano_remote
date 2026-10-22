#include "remotepane.h"

#include "seriallink.h"
#include "transfermime.h"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QItemSelectionModel>
#include <QInputDialog>
#include <QKeySequence>
#include <QMenu>
#include <QMessageBox>
#include <QShortcut>
#include <QMimeData>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace {

enum RemoteRole {
    IsDirRole = Qt::UserRole,
    NameRole,
    SizeRole,
    MtimeRole,
    AttrRole
};

class RemoteSortProxyModel final : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void sort(int column, Qt::SortOrder order) override
    {
        m_descending = order == Qt::DescendingOrder;
        QSortFilterProxyModel::sort(column, Qt::AscendingOrder);
    }

protected:
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override
    {
        const bool leftDir = left.data(IsDirRole).toBool();
        const bool rightDir = right.data(IsDirRole).toBool();
        if (leftDir != rightDir)
            return leftDir;

        const int cmp = compare(left, right);
        return m_descending ? cmp > 0 : cmp < 0;
    }

private:
    static int compare(const QModelIndex &left, const QModelIndex &right)
    {
        if (left.column() == 1) {
            const quint64 l = left.data(SizeRole).toULongLong();
            const quint64 r = right.data(SizeRole).toULongLong();
            if (l != r)
                return l < r ? -1 : 1;
        } else if (left.column() == 2) {
            const quint64 l = left.data(MtimeRole).toULongLong();
            const quint64 r = right.data(MtimeRole).toULongLong();
            if (l != r)
                return l < r ? -1 : 1;
        } else if (left.column() == 3) {
            const uint l = left.data(AttrRole).toUInt();
            const uint r = right.data(AttrRole).toUInt();
            if (l != r)
                return l < r ? -1 : 1;
        }

        const QString lhs = left.data(NameRole).toString();
        const QString rhs = right.data(NameRole).toString();
        return QString::localeAwareCompare(lhs, rhs);
    }

    bool m_descending = false;
};

QStringList scanSerialPorts()
{
    QStringList result;

    const QDir dev(QStringLiteral("/dev"));
    const QStringList patterns = { QStringLiteral("ttyACM*"), QStringLiteral("ttyUSB*") };
    for (const QString &pattern : patterns) {
        const QFileInfoList entries = dev.entryInfoList(QStringList() << pattern,
                                                        QDir::System | QDir::Files,
                                                        QDir::Name);
        for (const QFileInfo &entry : entries) {
            const QString path = entry.absoluteFilePath();
            if (!result.contains(path))
                result.append(path);
        }
    }

    const QDir byId(QStringLiteral("/dev/serial/by-id"));
    const QFileInfoList links = byId.entryInfoList(QDir::Files | QDir::System | QDir::NoDotAndDotDot,
                                                  QDir::Name);
    for (const QFileInfo &link : links) {
        const QString path = link.absoluteFilePath();
        if (!result.contains(path))
            result.append(path);
    }

    return result;
}

QString driveTypeText(int type)
{
    switch (type) {
    case 1: return QStringLiteral("FAT");
    case 2: return QStringLiteral("FAT (FD)");
    case 3: return QStringLiteral("exFAT");
    case 4: return QStringLiteral("NTFS");
    case 5: return QStringLiteral("ISO9660");
    case 6: return QStringLiteral("その他");
    default: return QStringLiteral("不明");
    }
}

QString attributeText(uint attr)
{
    QStringList parts;
    if (attr & 0x01)
        parts.append(QStringLiteral("R"));
    if (attr & 0x02)
        parts.append(QStringLiteral("H"));
    if (attr & 0x04)
        parts.append(QStringLiteral("S"));
    if (attr & 0x20)
        parts.append(QStringLiteral("A"));
    return parts.isEmpty() ? QStringLiteral("—") : parts.join(QLatin1Char(','));
}

QString joinRemotePath(const QString &base, const QString &name)
{
    if (base.isEmpty())
        return name;
    if (base.endsWith(QLatin1Char('/')))
        return base + name;
    return base + QLatin1Char('/') + name;
}

class RemoteTreeView final : public QTreeView
{
public:
    explicit RemoteTreeView(QWidget *parent = nullptr)
        : QTreeView(parent)
    {
    }

    void setDropHandler(std::function<void(const QStringList &)> handler)
    {
        m_dropHandler = std::move(handler);
    }

protected:
    void startDrag(Qt::DropActions supportedActions) override
    {
        Q_UNUSED(supportedActions);
        if (!selectionModel())
            return;

        QStringList paths;
        const QString base = property("remoteBasePath").toString();
        for (const QModelIndex &index : selectionModel()->selectedRows(0)) {
            if (index.data(IsDirRole).toBool())
                continue;
            const QString name = index.data(NameRole).toString();
            if (!name.isEmpty())
                paths.append(joinRemotePath(base, name));
        }
        paths.removeDuplicates();
        if (paths.isEmpty())
            return;

        auto *mime = new QMimeData;
        const QByteArray data = paths.join(QLatin1Char('\n')).toUtf8();
        mime->setData(remotePathsMimeType(), data);
        mime->setText(QString::fromUtf8(data));

        auto *drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->exec(Qt::CopyAction);
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (hasLocalFiles(event->mimeData())) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        event->ignore();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (hasLocalFiles(event->mimeData())) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        event->ignore();
    }

    void dropEvent(QDropEvent *event) override
    {
        const QStringList files = localFiles(event->mimeData());
        if (files.isEmpty()) {
            event->ignore();
            return;
        }
        event->setDropAction(Qt::CopyAction);
        event->accept();
        if (m_dropHandler)
            m_dropHandler(files);
    }

private:
    static QStringList localFiles(const QMimeData *mime)
    {
        QStringList files;
        if (!mime || !mime->hasUrls())
            return files;
        for (const QUrl &url : mime->urls()) {
            if (!url.isLocalFile())
                continue;
            const QFileInfo info(url.toLocalFile());
            if (info.isFile())
                files.append(info.absoluteFilePath());
        }
        files.removeDuplicates();
        return files;
    }

    static bool hasLocalFiles(const QMimeData *mime)
    {
        return !localFiles(mime).isEmpty();
    }

    std::function<void(const QStringList &)> m_dropHandler;
};

}

RemotePane::RemotePane(QWidget *parent)
    : QWidget(parent)
    , m_link(new SerialLink(this))
    , m_protocol(new RemoteProtocol(this))
{
    buildUi();

    connect(m_link, &SerialLink::opened, this, &RemotePane::onOpened);
    connect(m_link, &SerialLink::closed, this, &RemotePane::onClosed);
    connect(m_link, &SerialLink::dataReceived, this, &RemotePane::onDataReceived);
    connect(m_link, &SerialLink::errorOccurred, this, &RemotePane::onError);

    connect(m_protocol, &RemoteProtocol::transmit, this, [this](const QByteArray &data) {
        if (m_link->isOpen())
            m_link->write(data);
    });
    connect(m_protocol, &RemoteProtocol::synchronized, this, &RemotePane::onProtocolSynchronized);
    connect(m_protocol, &RemoteProtocol::drivesReceived, this, &RemotePane::onDrivesReceived);
    connect(m_protocol, &RemoteProtocol::cwdReceived, this, &RemotePane::onCwdReceived);
    connect(m_protocol, &RemoteProtocol::listStarted, this, &RemotePane::onListStarted);
    connect(m_protocol, &RemoteProtocol::entryReceived, this, &RemotePane::onEntryReceived);
    connect(m_protocol, &RemoteProtocol::listFinished, this, &RemotePane::onListFinished);
    connect(m_protocol, &RemoteProtocol::statReceived, this, &RemotePane::onStatReceived);
    connect(m_protocol, &RemoteProtocol::operationSucceeded, this, &RemotePane::onOperationSucceeded);
    connect(m_protocol, &RemoteProtocol::requestFailed, this, &RemotePane::onRequestFailed);
    connect(m_protocol, &RemoteProtocol::requestTimedOut, this, &RemotePane::onRequestTimedOut);
    connect(m_protocol, &RemoteProtocol::consoleLine, this, &RemotePane::onConsoleLine);
    connect(m_protocol, &RemoteProtocol::busyChanged, this, &RemotePane::onBusyChanged);
    connect(m_protocol, &RemoteProtocol::logMessage, this, &RemotePane::logMessage);

    refreshPorts();
    loadSettings();
    setConnectedUi(false);
}

RemotePane::~RemotePane()
{
    saveSettings();
    disconnect(m_link, nullptr, this, nullptr);
    disconnect(m_protocol, nullptr, this, nullptr);
    m_link->close();
}

void RemotePane::buildUi()
{
    auto *title = new QLabel(tr("リモート"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);

    auto *deviceLabel = new QLabel(tr("sgnano"), this);
    deviceLabel->setForegroundRole(QPalette::PlaceholderText);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->addWidget(title);
    header->addWidget(deviceLabel);
    header->addStretch(1);

    m_portCombo = new QComboBox(this);
    m_portCombo->setEditable(true);
    m_portCombo->setInsertPolicy(QComboBox::NoInsert);
    m_portCombo->setMinimumWidth(190);
    m_portCombo->setToolTip(tr("デバッグプローブの UART ブリッジ（既定 /dev/ttyACM0）"));

    m_scanButton = new QToolButton(this);
    m_scanButton->setText(tr("再検索"));
    m_scanButton->setToolTip(tr("シリアルポートを探し直す"));

    m_baudCombo = new QComboBox(this);
    m_baudCombo->addItems({ QStringLiteral("921600"),
                            QStringLiteral("460800"),
                            QStringLiteral("230400"),
                            QStringLiteral("115200"),
                            QStringLiteral("57600"),
                            QStringLiteral("38400"),
                            QStringLiteral("19200"),
                            QStringLiteral("9600") });
    m_baudCombo->setToolTip(tr("sgnano のコンソールは 921600 baud"));

    m_connectButton = new QPushButton(tr("接続"), this);
    m_connectButton->setDefault(false);

    auto *connectRow = new QHBoxLayout;
    connectRow->setContentsMargins(0, 0, 0, 0);
    connectRow->addWidget(m_portCombo, 1);
    connectRow->addWidget(m_scanButton);
    connectRow->addWidget(m_baudCombo);
    connectRow->addWidget(m_connectButton);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_driveCombo = new QComboBox(this);
    m_driveCombo->setMinimumContentsLength(12);
    m_driveCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_driveCombo->setToolTip(tr("一覧を表示するドライブ"));

    m_remotePath = new QLineEdit(this);
    m_remotePath->setClearButtonEnabled(true);
    m_remotePath->setPlaceholderText(tr("リモートのパス（A:/…）"));
    m_remotePath->setToolTip(tr("パスを入力して Enter で移動"));

    m_counters = new QLabel(this);
    m_counters->setForegroundRole(QPalette::PlaceholderText);

    auto *remoteRow = new QHBoxLayout;
    remoteRow->setContentsMargins(0, 0, 0, 0);
    remoteRow->addWidget(m_driveCombo);
    remoteRow->addWidget(m_remotePath, 1);
    remoteRow->addWidget(m_counters);

    m_upButton = new QPushButton(tr("上へ"), this);
    m_upButton->setToolTip(tr("親フォルダへ移動"));
    m_upButton->setEnabled(false);

    m_refreshButton = new QPushButton(tr("更新"), this);
    m_refreshButton->setToolTip(tr("現在のフォルダを読み直す"));
    m_refreshButton->setEnabled(false);

    m_uploadButton = new QPushButton(tr("アップロード"), this);
    m_uploadButton->setEnabled(false);
    m_uploadButton->setToolTip(tr("選択したローカルファイルを、右ペインで表示しているフォルダへ送ります（ZMODEM）"));

    m_downloadButton = new QPushButton(tr("ダウンロード"), this);
    m_downloadButton->setEnabled(false);
    m_downloadButton->setToolTip(tr("右ペインで選択したファイルをローカルの現在のフォルダへ受け取ります（ZMODEM）"));

    m_mkdirButton = new QPushButton(tr("新しいフォルダ"), this);
    m_mkdirButton->setEnabled(false);
    m_mkdirButton->setToolTip(tr("表示中のフォルダに新しいフォルダを作ります"));

    m_renameButton = new QPushButton(tr("名前の変更"), this);
    m_renameButton->setEnabled(false);
    m_renameButton->setToolTip(tr("選択した項目の名前を変えます（F2）"));

    m_deleteButton = new QPushButton(tr("削除"), this);
    m_deleteButton->setEnabled(false);
    m_deleteButton->setToolTip(tr("選択した項目を消します（Delete）。フォルダは空のときだけ消せます"));

    auto *actionRow = new QHBoxLayout;
    actionRow->setContentsMargins(0, 0, 0, 0);
    actionRow->addWidget(m_upButton);
    actionRow->addWidget(m_refreshButton);
    actionRow->addSpacing(12);
    actionRow->addWidget(m_mkdirButton);
    actionRow->addWidget(m_renameButton);
    actionRow->addWidget(m_deleteButton);
    actionRow->addStretch(1);
    actionRow->addWidget(m_uploadButton);
    actionRow->addWidget(m_downloadButton);

    m_model = new QStandardItemModel(this);
    m_model->setHorizontalHeaderLabels({ tr("名前"), tr("大きさ"), tr("更新日時"), tr("属性") });

    m_proxy = new RemoteSortProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setDynamicSortFilter(true);
    m_proxy->sort(0, Qt::AscendingOrder);

    auto *tree = new RemoteTreeView(this);
    tree->setDropHandler([this](const QStringList &files) {
        emit localFilesDropped(files);
    });
    m_tree = tree;
    m_tree->setModel(m_proxy);
    m_tree->setRootIsDecorated(false);
    m_tree->setItemsExpandable(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setDragEnabled(true);
    m_tree->setAcceptDrops(true);
    m_tree->setDragDropMode(QAbstractItemView::DragDrop);
    m_tree->setDefaultDropAction(Qt::CopyAction);
    m_tree->setDropIndicatorShown(false);
    m_tree->setToolTip(tr("ローカルのファイルをここへドロップすると、表示中のフォルダへアップロードします"));
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(0, Qt::AscendingOrder);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    m_hint = new QLabel(tr("リモートの一覧はまだ取得していません。"), this);
    m_hint->setAlignment(Qt::AlignCenter);
    m_hint->setWordWrap(true);
    m_hint->setForegroundRole(QPalette::PlaceholderText);

    m_treePage = new QWidget(this);
    auto *treeLayout = new QVBoxLayout(m_treePage);
    treeLayout->setContentsMargins(0, 0, 0, 0);
    treeLayout->setSpacing(6);
    treeLayout->addWidget(m_tree, 1);
    treeLayout->addWidget(m_hint);

    m_emptyPage = new QWidget(this);
    auto *emptyLayout = new QVBoxLayout(m_emptyPage);
    auto *emptyText = new QLabel(tr("sgnano に接続すると、ここにリモートのファイルが表示されます。\n\n"
                                    "シリアルポートを選んで「接続」を押してください。"),
                                 m_emptyPage);
    emptyText->setAlignment(Qt::AlignCenter);
    emptyText->setWordWrap(true);
    emptyText->setForegroundRole(QPalette::PlaceholderText);
    emptyLayout->addStretch(1);
    emptyLayout->addWidget(emptyText);
    emptyLayout->addStretch(1);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_emptyPage);
    m_stack->addWidget(m_treePage);

    auto *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);
    layout->addLayout(header);
    layout->addLayout(connectRow);
    layout->addWidget(m_statusLabel);
    layout->addWidget(line);
    layout->addLayout(remoteRow);
    layout->addLayout(actionRow);
    layout->addWidget(m_stack, 1);

    connect(m_scanButton, &QToolButton::clicked, this, &RemotePane::refreshPorts);
    connect(m_connectButton, &QPushButton::clicked, this, &RemotePane::toggleConnection);
    connect(m_refreshButton, &QPushButton::clicked, this, &RemotePane::refreshRemote);
    connect(m_upButton, &QPushButton::clicked, this, &RemotePane::goUpRemote);
    connect(m_uploadButton, &QPushButton::clicked, this, &RemotePane::uploadRequested);
    connect(m_downloadButton, &QPushButton::clicked, this, &RemotePane::downloadRequested);
    connect(m_driveCombo, &QComboBox::activated, this, &RemotePane::onDriveActivated);
    connect(m_remotePath, &QLineEdit::returnPressed, this, &RemotePane::navigateRemotePath);
    connect(m_tree, &QTreeView::activated, this, &RemotePane::openRemoteIndex);
    connect(m_mkdirButton, &QPushButton::clicked, this, &RemotePane::createFolder);
    connect(m_renameButton, &QPushButton::clicked, this, &RemotePane::renameSelected);
    connect(m_deleteButton, &QPushButton::clicked, this, &RemotePane::deleteSelected);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QWidget::customContextMenuRequested, this, &RemotePane::showContextMenu);
    auto *renameKey = new QShortcut(QKeySequence(Qt::Key_F2), m_tree, nullptr, nullptr, Qt::WidgetShortcut);
    connect(renameKey, &QShortcut::activated, this, &RemotePane::renameSelected);
    auto *deleteKey = new QShortcut(QKeySequence::Delete, m_tree, nullptr, nullptr, Qt::WidgetShortcut);
    connect(deleteKey, &QShortcut::activated, this, &RemotePane::deleteSelected);
    connect(m_tree->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &RemotePane::updateTransferButtons);
    connect(m_link, &SerialLink::closed, this, &RemotePane::updateCounters);
}

void RemotePane::refreshPorts()
{
    if (m_link->isOpen())
        return;

    const QString current = m_portCombo->currentText().trimmed();
    const QStringList ports = scanSerialPorts();

    m_portCombo->blockSignals(true);
    m_portCombo->clear();
    m_portCombo->addItems(ports);
    if (!current.isEmpty())
        m_portCombo->setCurrentText(current);
    else if (m_portCombo->count() == 0)
        m_portCombo->setCurrentText(QStringLiteral("/dev/ttyACM0"));
    m_portCombo->blockSignals(false);

    if (m_portCombo->currentText().isEmpty())
        m_portCombo->setCurrentText(QStringLiteral("/dev/ttyACM0"));

    emit logMessage(tr("シリアルポート: %1").arg(
        ports.isEmpty() ? tr("見つかりません（既定 /dev/ttyACM0）") : ports.join(QStringLiteral(", "))));
}

QString RemotePane::currentPort() const
{
    return m_link->isOpen() ? m_link->portName() : m_portCombo->currentText().trimmed();
}

int RemotePane::currentBaud() const
{
    bool ok = false;
    const int baud = m_baudCombo->currentText().toInt(&ok);
    return ok ? baud : 921600;
}

void RemotePane::disconnectRemote()
{
    if (m_link->isOpen())
        m_link->close();
}

void RemotePane::reconnectRemote()
{
    if (!m_link->isOpen())
        toggleConnection();
}

void RemotePane::toggleConnection()
{
    if (m_link->isOpen()) {
        m_link->close();
        return;
    }

    const QString port = m_portCombo->currentText().trimmed();
    bool ok = false;
    const int baud = m_baudCombo->currentText().toInt(&ok);
    if (port.isEmpty() || !ok) {
        emit logMessage(tr("接続先またはボーレートが正しくありません"));
        return;
    }

    QString error;
    emit logMessage(tr("接続しています: %1 @ %2").arg(port).arg(baud));
    if (!m_link->open(port, baud, &error)) {
        emit logMessage(error);
        setConnectedUi(false);
    }
}

void RemotePane::onOpened(const QString &port, int baud)
{
    m_rxBytes = 0;
    m_entries.clear();
    m_model->removeRows(0, m_model->rowCount());
    m_remotePathText.clear();
    m_updatingDrives = true;
    m_driveCombo->clear();
    m_updatingDrives = false;

    updateCounters();
    m_statusLabel->setText(tr("接続済み: %1 @ %2 baud").arg(port).arg(baud));
    m_remotePath->clear();
    m_remotePath->setPlaceholderText(tr("同期中…"));
    m_hint->setText(tr("sgnano と同期しています…"));
    setConnectedUi(true);
    emit logMessage(tr("シリアルを開きました: %1 @ %2").arg(port).arg(baud));
    emit connectionStateChanged(true, QStringLiteral("%1 @ %2").arg(port).arg(baud));

    ++m_openGeneration;
    m_cancelingStray = false;
    const quint64 generation = m_openGeneration;
    emit logMessage(tr("ZMODEM の残留状態をクリアしています…"));
    m_link->write(QByteArray(10, '\x18'));
    QTimer::singleShot(300, this, [this, generation] {
        if (generation == m_openGeneration && m_link->isOpen())
            m_link->write(QByteArray(10, '\x18'));
    });
    QTimer::singleShot(700, this, [this, generation] {
        if (generation != m_openGeneration || !m_link->isOpen() || m_cancelingStray)
            return;
        m_protocol->reset();
        m_protocol->sync();
    });
}

void RemotePane::onClosed()
{
    ++m_openGeneration;
    m_cancelingStray = false;
    m_protocol->reset();
    m_entries.clear();
    m_model->removeRows(0, m_model->rowCount());
    m_remotePathText.clear();
    m_updatingDrives = true;
    m_driveCombo->clear();
    m_updatingDrives = false;

    m_statusLabel->setText(tr("未接続"));
    m_remotePath->clear();
    m_remotePath->setPlaceholderText(tr("未接続"));
    m_hint->setText(tr("リモートの一覧はまだ取得していません。"));
    m_pendingRefreshPath.clear();
    setConnectedUi(false);
    emit logMessage(tr("シリアルを閉じました"));
    emit connectionStateChanged(false, tr("未接続"));
}

void RemotePane::onDataReceived(const QByteArray &data)
{
    m_rxBytes += quint64(data.size());
    updateCounters();
    m_protocol->feed(data);
}

void RemotePane::onError(const QString &message)
{
    emit logMessage(tr("シリアル: %1").arg(message));
}

void RemotePane::onProtocolSynchronized()
{
    emit logMessage(tr("sgnano と同期しました"));
    m_protocol->requestDrives();
    if (!m_pendingRefreshPath.isEmpty()) {
        const QString path = m_pendingRefreshPath;
        m_pendingRefreshPath.clear();
        requestList(path);
    } else {
        m_protocol->requestCwd();
    }
}

void RemotePane::onDrivesReceived(const QList<RemoteDrive> &drives)
{
    m_updatingDrives = true;
    m_driveCombo->clear();
    for (const RemoteDrive &drive : drives) {
        QString label = QStringLiteral("%1:  %2").arg(drive.letter, driveTypeText(drive.type));
        if (!drive.ready)
            label += tr("（未挿入）");
        m_driveCombo->addItem(label, drive.letter);
        emit logMessage(tr("ドライブ %1: %2 (%3)")
                            .arg(drive.letter,
                                 driveTypeText(drive.type),
                                 drive.ready ? tr("準備完了") : tr("未挿入")));
    }
    m_updatingDrives = false;
    syncDriveCombo();
}

void RemotePane::onCwdReceived(const QString &path)
{
    applyRemotePath(path);
    requestList(path);
}

void RemotePane::onListStarted(const QString &path)
{
    m_entries.clear();
    m_model->removeRows(0, m_model->rowCount());
    applyRemotePath(path);
    m_hint->setText(tr("%1 を読み込んでいます…").arg(path));
}

void RemotePane::onEntryReceived(const RemoteEntry &entry)
{
    m_entries.append(entry);
}

void RemotePane::onListFinished(int count, bool truncated)
{
    populateTree();
    if (count == 0)
        m_hint->setText(tr("このフォルダは空です"));
    else if (truncated)
        m_hint->setText(tr("%1 項目（上限まで表示）").arg(QLocale().toString(count)));
    else
        m_hint->setText(tr("%1 項目").arg(QLocale().toString(count)));
    emit logMessage(tr("リモート一覧: %1 項目%2")
                        .arg(QLocale().toString(count),
                             truncated ? tr("（省略あり）") : QString()));
}

void RemotePane::onStatReceived(const RemoteStat &stat)
{
    emit logMessage(tr("STAT: %1%2 (%3)")
                        .arg(stat.name,
                             stat.isDir ? tr(" [フォルダ]") : QString(),
                             QLocale().formattedDataSize(qint64(stat.size), 1)));
}

void RemotePane::onRequestFailed(const QString &command, int code, const QString &message)
{
    emit logMessage(tr("%1 に失敗しました: %2 (code %3)").arg(command, message).arg(code));

    if (command.startsWith(QStringLiteral("LIST"))) {
        m_entries.clear();
        m_model->removeRows(0, m_model->rowCount());
        m_hint->setText(tr("一覧を取得できません: %1").arg(message));
        syncDriveCombo();
    } else if (command.startsWith(QStringLiteral("DEL")) || command.startsWith(QStringLiteral("REN"))
               || command.startsWith(QStringLiteral("MKDIR"))) {
        m_opErrors.append(tr("%1: %2").arg(command.section(QLatin1Char(' '), 1).replace(QLatin1Char('\t'), QStringLiteral(" → ")), message));
        if (m_opsPending > 0 && --m_opsPending == 0)
            finishOperations();
    } else if (command.startsWith(QStringLiteral("CWD"))) {
        m_hint->setText(tr("現在のフォルダを取得できません: %1").arg(message));
    } else if (command.startsWith(QStringLiteral("DRIVES"))) {
        m_hint->setText(tr("ドライブ一覧を取得できません: %1").arg(message));
    }
}

void RemotePane::onRequestTimedOut(const QString &command)
{
    if (m_opsPending > 0) {      // 時間切れは待ち行列を捨てるので、残りの操作もまとめて終わりにする
        m_opErrors.append(tr("%1: 応答がありません").arg(command));
        m_opsPending = 0;
        finishOperations();
    }
    emit logMessage(tr("%1 の応答がタイムアウトしました").arg(command));
    if (command.startsWith(QStringLiteral("LIST"))) {
        m_entries.clear();
        m_model->removeRows(0, m_model->rowCount());
        m_hint->setText(tr("一覧を取得できません（タイムアウト）。「更新」で再同期してください。"));
        syncDriveCombo();
    } else {
        m_hint->setText(tr("sgnano から応答がありません。「更新」で再同期してください。"));
    }
    m_upButton->setEnabled(false);
    m_refreshButton->setEnabled(m_link->isOpen());
}

void RemotePane::onConsoleLine(const QString &line)
{
    emit logMessage(tr("sgnano: %1").arg(line));
    const bool zmodemStarted = line.contains(QStringLiteral("[zmodem] send "))
        || line.contains(QStringLiteral("[zmodem] receive "));
    if (zmodemStarted && m_link->isOpen() && !m_cancelingStray) {
        m_cancelingStray = true;
        const quint64 generation = m_openGeneration;
        emit logMessage(tr("予期しない ZMODEM を検出したため取り消します"));
        m_link->write(QByteArray(10, '\x18'));
        QTimer::singleShot(900, this, [this, generation] {
            if (generation != m_openGeneration || !m_link->isOpen()) {
                m_cancelingStray = false;
                return;
            }
            m_protocol->reset();
            m_protocol->sync();
            m_cancelingStray = false;
        });
    }
}

void RemotePane::onBusyChanged(bool busy)
{
    Q_UNUSED(busy);
    updateTransferButtons();
}

void RemotePane::refreshRemote()
{
    if (!m_link->isOpen())
        return;
    if (!m_protocol->isSynchronized()) {
        emit logMessage(tr("sgnano と再同期しています"));
        m_protocol->sync();
        return;
    }
    if (m_remotePathText.isEmpty())
        m_protocol->requestCwd();
    else
        requestList(m_remotePathText);
}

void RemotePane::refreshRemoteAt(const QString &path)
{
    m_pendingRefreshPath = path;
    if (!m_link->isOpen())
        return;
    if (!m_protocol->isSynchronized()) {
        m_protocol->sync();
        return;
    }
    m_pendingRefreshPath.clear();
    requestList(path);
}

void RemotePane::goUpRemote()
{
    if (m_remotePathText.isEmpty()) {
        refreshRemote();
        return;
    }

    QString path = m_remotePathText;
    if (path.endsWith(QLatin1Char('/')))
        path += QStringLiteral("..");
    else
        path += QStringLiteral("/..");
    requestList(path);
}

void RemotePane::navigateRemotePath()
{
    if (!m_link->isOpen())
        return;

    const QString path = m_remotePath->text().trimmed();
    if (path.isEmpty()) {
        m_protocol->requestCwd();
        return;
    }
    requestList(path);
}

void RemotePane::loadSettings()
{
    QSettings settings;
    const QString port = settings.value(QStringLiteral("serial/port")).toString();
    if (!port.isEmpty())
        m_portCombo->setCurrentText(port);
    const QString baud = settings.value(QStringLiteral("serial/baud")).toString();
    if (!baud.isEmpty()) {
        const int index = m_baudCombo->findText(baud);
        if (index >= 0)
            m_baudCombo->setCurrentIndex(index);
    }
}

void RemotePane::saveSettings()
{
    QSettings settings;
    settings.setValue(QStringLiteral("serial/port"), m_portCombo->currentText().trimmed());
    settings.setValue(QStringLiteral("serial/baud"), m_baudCombo->currentText());
}

void RemotePane::openRemoteIndex(const QModelIndex &index)
{
    if (!index.data(IsDirRole).toBool())
        return;

    const QString name = index.data(NameRole).toString();
    requestList(joinRemotePath(m_remotePathText, name));
}

void RemotePane::onDriveActivated(int index)
{
    if (m_updatingDrives || index < 0)
        return;

    const QString letter = m_driveCombo->itemData(index).toString();
    if (letter.isEmpty())
        return;
    requestList(letter + QStringLiteral(":/"));
}

void RemotePane::requestList(const QString &path)
{
    if (!m_link->isOpen())
        return;
    m_hint->setText(tr("%1 を読み込んでいます…").arg(path.isEmpty() ? tr("現在のフォルダ") : path));
    m_protocol->requestList(path);
}

void RemotePane::populateTree()
{
    m_model->removeRows(0, m_model->rowCount());

    const QIcon folderIcon = QApplication::style()->standardIcon(QStyle::SP_DirIcon);
    const QIcon fileIcon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);

    for (const RemoteEntry &entry : m_entries) {
        auto *nameItem = new QStandardItem(entry.isDir ? folderIcon : fileIcon, entry.name);
        nameItem->setEditable(false);
        nameItem->setDragEnabled(!entry.isDir);
        nameItem->setDropEnabled(false);

        auto *sizeItem = new QStandardItem(
            entry.isDir ? QString() : QLocale().formattedDataSize(qint64(entry.size), 1));
        sizeItem->setEditable(false);
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

        auto *dateItem = new QStandardItem(
            entry.mtime ? QDateTime::fromSecsSinceEpoch(qint64(entry.mtime))
                             .toString(QStringLiteral("yyyy/MM/dd HH:mm"))
                       : QString());
        dateItem->setEditable(false);
        dateItem->setTextAlignment(Qt::AlignCenter);

        auto *attrItem = new QStandardItem(attributeText(entry.attr));
        attrItem->setEditable(false);
        attrItem->setTextAlignment(Qt::AlignCenter);

        for (QStandardItem *item : { nameItem, sizeItem, dateItem, attrItem }) {
            item->setData(entry.isDir, IsDirRole);
            item->setData(entry.name, NameRole);
            item->setData(entry.size, SizeRole);
            item->setData(entry.mtime, MtimeRole);
            item->setData(entry.attr, AttrRole);
        }
        m_model->appendRow({ nameItem, sizeItem, dateItem, attrItem });
    }

    m_tree->scrollToTop();
    updateTransferButtons();
}

void RemotePane::applyRemotePath(const QString &path)
{
    m_remotePathText = path;
    m_tree->setProperty("remoteBasePath", path);
    m_remotePath->setText(path);
    m_remotePath->setPlaceholderText(tr("リモートのパス（A:/…）"));
    syncDriveCombo();
    updateTransferButtons();
}

void RemotePane::syncDriveCombo()
{
    if (m_remotePathText.size() < 2 || m_remotePathText.at(1) != QLatin1Char(':'))
        return;

    const QString letter = m_remotePathText.left(1).toUpper();
    const int index = m_driveCombo->findData(letter);
    if (index < 0 || index == m_driveCombo->currentIndex())
        return;

    m_updatingDrives = true;
    m_driveCombo->setCurrentIndex(index);
    m_updatingDrives = false;
}

void RemotePane::setConnectedUi(bool connected)
{
    m_portCombo->setEnabled(!connected);
    m_baudCombo->setEnabled(!connected);
    m_scanButton->setEnabled(!connected);
    m_connectButton->setText(connected ? tr("切断") : tr("接続"));
    m_remotePath->setEnabled(connected);
    m_stack->setCurrentWidget(connected ? m_treePage : m_emptyPage);
    updateTransferButtons();
}

void RemotePane::updateTransferButtons()
{
    const bool idle = m_link->isOpen() && !m_protocol->isBusy();
    m_driveCombo->setEnabled(idle);
    m_refreshButton->setEnabled(idle);
    m_upButton->setEnabled(idle && !m_remotePathText.isEmpty());
    m_uploadButton->setEnabled(idle && !m_remotePathText.isEmpty());
    m_downloadButton->setEnabled(idle && !selectedRemoteFiles().isEmpty());
    const int selected = int(selectedItems().size());
    m_mkdirButton->setEnabled(idle && !m_remotePathText.isEmpty());
    m_renameButton->setEnabled(idle && selected == 1);
    m_deleteButton->setEnabled(idle && selected > 0);
}

QList<RemotePane::SelectedItem> RemotePane::selectedItems() const
{
    QList<SelectedItem> items;
    if (!m_tree->selectionModel())
        return items;
    for (const QModelIndex &index : m_tree->selectionModel()->selectedRows(0)) {
        const QString name = index.data(NameRole).toString();
        if (!name.isEmpty())
            items.append({ name, index.data(IsDirRole).toBool() });
    }
    return items;
}

void RemotePane::showContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    QAction *mk = menu.addAction(tr("新しいフォルダ…"), this, &RemotePane::createFolder);
    QAction *rn = menu.addAction(tr("名前の変更…"), this, &RemotePane::renameSelected);
    rn->setShortcut(QKeySequence(Qt::Key_F2));
    QAction *dl = menu.addAction(tr("削除…"), this, &RemotePane::deleteSelected);
    dl->setShortcut(QKeySequence::Delete);
    menu.addSeparator();
    menu.addAction(tr("更新"), this, &RemotePane::refreshRemote);
    mk->setEnabled(m_mkdirButton->isEnabled());
    rn->setEnabled(m_renameButton->isEnabled());
    dl->setEnabled(m_deleteButton->isEnabled());
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void RemotePane::createFolder()
{
    if (!m_mkdirButton->isEnabled())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("新しいフォルダ"),
                                               tr("%1 に作るフォルダの名前:").arg(m_remotePathText),
                                               QLineEdit::Normal, tr("新しいフォルダ"), &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char(':')) || name == QStringLiteral(".")
        || name == QStringLiteral("..")) {
        QMessageBox::warning(this, tr("新しいフォルダ"), tr("名前に / や : は使えません。"));
        return;
    }
    m_opErrors.clear();
    m_opsPending = 1;
    emit logMessage(tr("フォルダを作ります: %1").arg(joinRemotePath(m_remotePathText, name)));
    m_protocol->requestMkdir(joinRemotePath(m_remotePathText, name));
}

void RemotePane::renameSelected()
{
    if (!m_renameButton->isEnabled())
        return;
    const QList<SelectedItem> items = selectedItems();
    if (items.size() != 1)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("名前の変更"), tr("新しい名前:"), QLineEdit::Normal,
                                               items.first().name, &ok).trimmed();
    if (!ok || name.isEmpty() || name == items.first().name)
        return;
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char(':'))) {
        QMessageBox::warning(this, tr("名前の変更"), tr("名前に / や : は使えません（同じフォルダの中でだけ変えられます）。"));
        return;
    }
    m_opErrors.clear();
    m_opsPending = 1;
    emit logMessage(tr("名前を変えます: %1 → %2").arg(items.first().name, name));
    m_protocol->requestRename(joinRemotePath(m_remotePathText, items.first().name), name);
}

void RemotePane::deleteSelected()
{
    if (!m_deleteButton->isEnabled())
        return;
    const QList<SelectedItem> items = selectedItems();
    if (items.isEmpty())
        return;
    QStringList names;
    for (const SelectedItem &it : items)
        names.append(it.isDir ? it.name + QLatin1Char('/') : it.name);
    const int shown = 12;
    QString list = QStringList(names.mid(0, shown)).join(QLatin1Char('\n'));
    if (names.size() > shown)
        list += tr("\n…ほか %1 項目").arg(names.size() - shown);
    const auto answer = QMessageBox::question(
        this, tr("削除"),
        tr("%1 の %2 項目を消します。元に戻せません。\n\n%3\n\n（フォルダは空のときだけ消せます）")
            .arg(m_remotePathText).arg(items.size()).arg(list),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    m_opErrors.clear();
    m_opsPending = int(items.size());
    for (const SelectedItem &it : items) {
        emit logMessage(tr("消します: %1").arg(joinRemotePath(m_remotePathText, it.name)));
        m_protocol->requestDelete(joinRemotePath(m_remotePathText, it.name));
    }
}

void RemotePane::onOperationSucceeded(const QString &operation, const QString &path)
{
    emit logMessage(tr("%1 しました: %2").arg(operation == QStringLiteral("DEL") ? tr("削除")
                                              : operation == QStringLiteral("REN") ? tr("名前を変更")
                                                                                     : tr("フォルダを作成"),
                                              path));
    if (m_opsPending > 0 && --m_opsPending == 0)
        finishOperations();
}

void RemotePane::finishOperations()
{
    if (!m_remotePathText.isEmpty())
        requestList(m_remotePathText);
    if (!m_opErrors.isEmpty()) {
        const QString text = m_opErrors.join(QLatin1Char('\n'));
        m_opErrors.clear();
        QMessageBox::warning(this, tr("リモートの操作"), tr("できなかった操作があります:\n\n%1").arg(text));
    }
}

QString RemotePane::currentRemotePath() const
{
    return m_remotePathText;
}

bool RemotePane::isIdle() const
{
    return m_link->isOpen() && !m_protocol->isBusy();
}

QStringList RemotePane::selectedRemoteFiles() const
{
    QStringList paths;
    if (!m_tree->selectionModel())
        return paths;

    const QModelIndexList rows = m_tree->selectionModel()->selectedRows(0);
    for (const QModelIndex &index : rows) {
        if (index.data(IsDirRole).toBool())
            continue;
        const QString name = index.data(NameRole).toString();
        if (!name.isEmpty())
            paths.append(joinRemotePath(m_remotePathText, name));
    }
    paths.removeDuplicates();
    paths.sort();
    return paths;
}

void RemotePane::updateCounters()
{
    m_counters->setText(tr("受信 %1").arg(QLocale().formattedDataSize(qint64(m_rxBytes), 1)));
}
