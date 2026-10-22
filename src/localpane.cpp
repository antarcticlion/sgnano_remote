#include "localpane.h"

#include "transfermime.h"

#include <QDateTime>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMimeData>
#include <QSortFilterProxyModel>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QToolButton>
#include <QUrl>
#include <QTreeView>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace {

class LocalTreeView final : public QTreeView
{
public:
    explicit LocalTreeView(QWidget *parent = nullptr)
        : QTreeView(parent)
    {
    }

    void setRemoteDropHandler(std::function<void(const QStringList &)> handler)
    {
        m_dropHandler = std::move(handler);
    }

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData() && event->mimeData()->hasFormat(remotePathsMimeType())) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        event->ignore();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->mimeData() && event->mimeData()->hasFormat(remotePathsMimeType())) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        event->ignore();
    }

    void dropEvent(QDropEvent *event) override
    {
        if (!event->mimeData() || !event->mimeData()->hasFormat(remotePathsMimeType())) {
            event->ignore();
            return;
        }

        const QStringList paths = QString::fromUtf8(
            event->mimeData()->data(remotePathsMimeType())).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (paths.isEmpty()) {
            event->ignore();
            return;
        }

        event->setDropAction(Qt::CopyAction);
        event->accept();
        if (m_dropHandler)
            m_dropHandler(paths);
    }

private:
    std::function<void(const QStringList &)> m_dropHandler;
};

class DirFirstProxyModel final : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

protected:
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override
    {
        const auto *fs = qobject_cast<const QFileSystemModel *>(sourceModel());
        if (fs) {
            const bool leftDir = fs->isDir(left);
            const bool rightDir = fs->isDir(right);
            if (leftDir != rightDir)
                return leftDir;
            if (left.column() == 1)
                return fs->size(left) < fs->size(right);
            if (left.column() == 3)
                return fs->lastModified(left) < fs->lastModified(right);
        }
        return QSortFilterProxyModel::lessThan(left, right);
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
            switch (section) {
            case 0: return tr("名前");
            case 1: return tr("大きさ");
            case 3: return tr("更新日時");
            default: break;
            }
        }
        return QSortFilterProxyModel::headerData(section, orientation, role);
    }
};

QString expandPath(const QString &text)
{
    QString path = text.trimmed();
    if (path == QStringLiteral("~"))
        path = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    else if (path.startsWith(QStringLiteral("~/")))
        path = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + path.mid(1);
    if (path.isEmpty())
        path = QDir::currentPath();
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

}

LocalPane::LocalPane(QWidget *parent)
    : QWidget(parent)
{
    auto *title = new QLabel(tr("ローカル"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);

    auto *rootLabel = new QLabel(tr("LOCAL"), this);
    rootLabel->setForegroundRole(QPalette::PlaceholderText);

    auto *upButton = new QToolButton(this);
    upButton->setText(tr("上へ"));
    upButton->setToolTip(tr("親フォルダへ移動"));
    upButton->setAutoRaise(false);

    auto *homeButton = new QToolButton(this);
    homeButton->setText(tr("ホーム"));
    homeButton->setToolTip(tr("ホームフォルダへ移動"));
    homeButton->setAutoRaise(false);

    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setClearButtonEnabled(true);
    m_pathEdit->setPlaceholderText(tr("パスを入力して Enter"));
    m_pathEdit->setMinimumWidth(240);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->addWidget(title);
    header->addWidget(rootLabel);
    header->addStretch(1);
    header->addWidget(upButton);
    header->addWidget(homeButton);

    auto *pathRow = new QHBoxLayout;
    pathRow->setContentsMargins(0, 0, 0, 0);
    pathRow->addWidget(m_pathEdit, 1);

    m_model = new QFileSystemModel(this);
    m_model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs);
    m_model->setRootPath(QDir::rootPath());
    m_model->setReadOnly(true);

    m_proxy = new DirFirstProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setDynamicSortFilter(true);
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->sort(0, Qt::AscendingOrder);

    auto *view = new LocalTreeView(this);
    view->setRemoteDropHandler([this](const QStringList &paths) {
        emit remoteFilesDropped(paths);
    });
    m_view = view;
    m_view->setModel(m_proxy);
    m_view->setRootIsDecorated(false);
    m_view->setItemsExpandable(false);
    m_view->setUniformRowHeights(true);
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setSortingEnabled(true);
    m_view->sortByColumn(0, Qt::AscendingOrder);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setDragEnabled(true);
    m_view->setAcceptDrops(true);
    m_view->setDragDropMode(QAbstractItemView::DragDrop);
    m_view->setDefaultDropAction(Qt::CopyAction);
    m_view->setDropIndicatorShown(false);
    m_view->setToolTip(tr("リモートのファイルをここへドロップすると、このフォルダへダウンロードします"));
    m_view->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_view->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_view->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_view->hideColumn(2);
    for (int column = 4; column < m_model->columnCount(); ++column)
        m_view->hideColumn(column);

    m_summary = new QLabel(this);
    m_summary->setForegroundRole(QPalette::PlaceholderText);

    auto *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);
    layout->addLayout(header);
    layout->addLayout(pathRow);
    layout->addWidget(m_view, 1);
    layout->addWidget(line);
    layout->addWidget(m_summary);

    connect(upButton, &QToolButton::clicked, this, &LocalPane::goUp);
    connect(homeButton, &QToolButton::clicked, this, &LocalPane::goHome);
    connect(m_pathEdit, &QLineEdit::returnPressed, this, &LocalPane::navigateToPath);
    connect(m_view, &QTreeView::activated, this, &LocalPane::openIndex);
    connect(m_model, &QFileSystemModel::directoryLoaded, this, [this](const QString &path) {
        if (QDir::cleanPath(path) != m_currentPath)
            return;
        const QModelIndex source = m_model->index(path);
        if (!source.isValid())
            return;
        const QModelIndex proxy = m_proxy->mapFromSource(source);
        if (!proxy.isValid())
            return;
        if (m_proxy->canFetchMore(proxy))
            m_proxy->fetchMore(proxy);
        m_view->setRootIndex(proxy);
        refreshSummary();
    });

    setCurrentDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
}

QString LocalPane::currentPath() const
{
    return m_currentPath;
}

QStringList LocalPane::selectedFiles() const
{
    QStringList paths;
    if (!m_view->selectionModel())
        return paths;
    const QModelIndexList rows = m_view->selectionModel()->selectedRows(0);
    for (const QModelIndex &proxyIndex : rows) {
        const QModelIndex source = m_proxy->mapToSource(proxyIndex);
        const QFileInfo info = m_model->fileInfo(source);
        if (info.isFile())
            paths.append(info.absoluteFilePath());
    }
    paths.sort();
    return paths;
}

void LocalPane::refresh()
{
    const QModelIndex source = m_model->index(m_currentPath);
    if (source.isValid())
        m_view->setRootIndex(m_proxy->mapFromSource(source));
    refreshSummary();
}

void LocalPane::goUp()
{
    QDir dir(m_currentPath);
    if (dir.cdUp())
        setCurrentDir(dir.absolutePath());
}

void LocalPane::goHome()
{
    setCurrentDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
}

void LocalPane::navigateToPath()
{
    setCurrentDir(expandPath(m_pathEdit->text()));
}

void LocalPane::openIndex(const QModelIndex &index)
{
    const QModelIndex source = m_proxy->mapToSource(index);
    if (!source.isValid()) {
        refreshSummary();
        return;
    }
    const QString path = m_model->filePath(source);
    if (m_model->isDir(source)) {
        setCurrentDir(path);
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void LocalPane::refreshSummary()
{
    const QDir dir(m_currentPath);
    const int count = dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
    const QStorageInfo storage(m_currentPath);
    QString text = tr("%1 項目").arg(QLocale().toString(count));
    if (storage.isValid() && storage.isReady()) {
        text += tr("  ・  空き %1 / %2")
                    .arg(QLocale().formattedDataSize(storage.bytesAvailable(), 2),
                         QLocale().formattedDataSize(storage.bytesTotal(), 2));
    }
    m_summary->setText(text);
}

void LocalPane::setCurrentDir(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir()) {
        m_pathEdit->setText(m_currentPath);
        return;
    }

    const QString cleaned = QDir::cleanPath(info.absoluteFilePath());
    const QModelIndex source = m_model->index(cleaned);
    if (!source.isValid()) {
        m_pathEdit->setText(m_currentPath);
        return;
    }

    m_currentPath = cleaned;
    m_pathEdit->setText(cleaned);
    const QModelIndex proxyIndex = m_proxy->mapFromSource(source);
    m_view->setRootIndex(proxyIndex);
    if (m_model->canFetchMore(source))
        m_model->fetchMore(source);
    if (m_proxy->canFetchMore(proxyIndex))
        m_proxy->fetchMore(proxyIndex);
    m_view->clearSelection();
    refreshSummary();
    emit pathChanged(m_currentPath);
}
