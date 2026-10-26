#include "mainwindow.h"

#include "localpane.h"
#include "remotepane.h"
#include "zmodemworker.h"

#include <QAction>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFontDatabase>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QThread>
#include <QTime>
#include <QTimer>

#include <limits>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("sgnano Remote Filer"));
    setWindowIcon(QIcon::fromTheme(QStringLiteral("folder-remote")));
    resize(1240, 780);

    buildUi();

    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    const QByteArray splitterState = settings.value(QStringLiteral("window/splitter")).toByteArray();
    if (!splitterState.isEmpty())
        m_splitter->restoreState(splitterState);
    const QByteArray dockState = settings.value(QStringLiteral("window/dock")).toByteArray();
    if (!dockState.isEmpty())
        restoreState(dockState);

    m_localStatus->setText(tr("ローカル: %1").arg(m_localPane->currentPath()));
    m_remoteStatus->setText(tr("リモート: 未接続"));

    appendLog(tr("sgnano_remote を起動しました"));
    appendLog(tr("sgnano のコンソールはデバッグプローブ経由の UART、921600 baud です"));
}

void MainWindow::buildUi()
{
    m_localPane = new LocalPane(this);
    m_remotePane = new RemotePane(this);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(6);
    m_splitter->addWidget(m_localPane);
    m_splitter->addWidget(m_remotePane);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({ 620, 620 });
    setCentralWidget(m_splitter);

    auto *logDock = new QDockWidget(tr("シリアルログ"), this);
    logDock->setObjectName(QStringLiteral("serialLogDock"));
    logDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);

    m_log = new QPlainTextEdit(logDock);
    m_log->setReadOnly(true);
    m_log->setUndoRedoEnabled(false);
    m_log->setMaximumBlockCount(2000);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setPlaceholderText(tr("接続・切断・受信データのログ"));
    logDock->setWidget(m_log);
    addDockWidget(Qt::BottomDockWidgetArea, logDock);
    resizeDocks({ logDock }, { 150 }, Qt::Vertical);

    auto *fileMenu = menuBar()->addMenu(tr("ファイル(&F)"));
    auto *quitAction = fileMenu->addAction(tr("終了(&Q)"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    auto *viewMenu = menuBar()->addMenu(tr("表示(&V)"));
    QAction *logAction = logDock->toggleViewAction();
    logAction->setText(tr("シリアルログ"));
    logAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+L")));
    viewMenu->addAction(logAction);

    auto *remoteMenu = menuBar()->addMenu(tr("操作(&O)"));
    auto *connectAction = remoteMenu->addAction(tr("接続/切断(&C)"));
    connectAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    connect(connectAction, &QAction::triggered, this, [this] {
        if (!m_transfer)
            m_remotePane->toggleConnection();
    });

    auto *refreshLocalAction = remoteMenu->addAction(tr("ファイル一覧更新(ホスト)(&H)"));
    refreshLocalAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    connect(refreshLocalAction, &QAction::triggered, m_localPane, &LocalPane::refresh);

    auto *refreshRemoteAction = remoteMenu->addAction(tr("ファイル一覧更新(リモート)(&R)"));
    refreshRemoteAction->setShortcut(QKeySequence::Refresh);
    connect(refreshRemoteAction, &QAction::triggered, this, [this] {
        if (!m_transfer)
            m_remotePane->refreshRemote();
    });

    auto *upAction = remoteMenu->addAction(tr("リモートで上へ(&U)"));
    upAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    connect(upAction, &QAction::triggered, m_remotePane, &RemotePane::goUpRemote);

    remoteMenu->addSeparator();
    auto *uploadAction = remoteMenu->addAction(tr("アップロード(&A)"));
    uploadAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_U));
    connect(uploadAction, &QAction::triggered, this, &MainWindow::startUpload);

    auto *downloadAction = remoteMenu->addAction(tr("ダウンロード(&D)"));
    downloadAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    connect(downloadAction, &QAction::triggered, this, &MainWindow::startDownload);

    m_localStatus = new QLabel(this);
    m_remoteStatus = new QLabel(this);
    m_remoteStatus->setMinimumWidth(260);
    m_remoteStatus->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statusBar()->addWidget(m_localStatus, 1);
    statusBar()->addPermanentWidget(m_remoteStatus);

    connect(m_localPane, &LocalPane::pathChanged, this, [this](const QString &path) {
        m_localStatus->setText(tr("ローカル: %1").arg(path));
    });
    connect(m_remotePane, &RemotePane::logMessage, this, &MainWindow::appendLog);
    connect(m_remotePane, &RemotePane::connectionStateChanged, this, &MainWindow::updateRemoteStatus);
    connect(m_remotePane, &RemotePane::uploadRequested, this, &MainWindow::startUpload);
    connect(m_remotePane, &RemotePane::downloadRequested, this, &MainWindow::startDownload);
    connect(m_remotePane, &RemotePane::localFilesDropped, this, [this](const QStringList &files) {
        startUploadTo(files, m_remotePane->currentRemotePath().trimmed());
    }, Qt::QueuedConnection);
    connect(m_localPane, &LocalPane::remoteFilesDropped, this, [this](const QStringList &paths) {
        if (paths.size() > 1) {
            QMessageBox::information(this, tr("ダウンロード"),
                                     tr("ドロップで受け取れるのは一度に1ファイルです。"));
            return;
        }
        if (paths.isEmpty())
            return;
        startDownloadFrom(paths.first(), m_localPane->currentPath());
    }, Qt::QueuedConnection);
    m_remotePane->refreshPorts();
}

void MainWindow::appendLog(const QString &message)
{
    const QString stamp = QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    m_log->appendPlainText(QStringLiteral("%1  %2").arg(stamp, message));
}

void MainWindow::updateRemoteStatus(bool connected, const QString &description)
{
    m_remoteStatus->setText(connected ? tr("リモート: 接続済み (%1)").arg(description)
                                      : tr("リモート: %1").arg(description));
}

namespace {

QString remoteParentPath(const QString &path)
{
    QString clean = path;
    while (clean.endsWith(QLatin1Char('/')) && clean.size() > 3)
        clean.chop(1);

    const int slash = clean.lastIndexOf(QLatin1Char('/'));
    if (slash < 0)
        return clean;
    if (slash == 2 && clean.size() > 1 && clean.at(1) == QLatin1Char(':'))
        return clean.left(3);
    return clean.left(slash);
}

}

void MainWindow::startUpload()
{
    const QStringList files = m_localPane->selectedFiles();
    if (files.isEmpty()) {
        QMessageBox::information(this, tr("アップロード"),
                                 tr("ローカル側でファイルを選択してください。"));
        return;
    }

    const QString remoteDir = m_remotePane->currentRemotePath().trimmed();
    if (remoteDir.isEmpty()) {
        QMessageBox::information(this, tr("アップロード"),
                                 tr("右ペインで送り先のフォルダを開いてください。"));
        return;
    }

    startUploadTo(files, remoteDir);
}

void MainWindow::startUploadTo(const QStringList &files, const QString &remoteDir)
{
    if (m_transfer || files.isEmpty() || remoteDir.isEmpty())
        return;
    if (!m_remotePane->isIdle()) {
        QMessageBox::information(this, tr("アップロード"),
                                 tr("いまは転送や一覧の読み込み中です。"));
        return;
    }

    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this, tr("ZMODEM アップロード"),
        tr("選択した %1 個のファイルを\n%2\nへ送ります。\n\n"
           "同名ファイルは上書きされます。実行しますか？")
            .arg(files.size())
            .arg(remoteDir),
        QMessageBox::Ok | QMessageBox::Cancel);
    if (answer != QMessageBox::Ok)
        return;

    launchTransfer(false, files, QString(), remoteDir);
}

void MainWindow::startDownload()
{
    const QStringList remoteFiles = m_remotePane->selectedRemoteFiles();
    if (remoteFiles.isEmpty()) {
        QMessageBox::information(this, tr("ダウンロード"),
                                 tr("右ペインでファイルを1つ選択してください。"));
        return;
    }
    if (remoteFiles.size() > 1) {
        QMessageBox::information(this, tr("ダウンロード"),
                                 tr("ダウンロードは一度に1ファイルです。"));
        return;
    }

    startDownloadFrom(remoteFiles.first(), m_localPane->currentPath());
}

void MainWindow::startDownloadFrom(const QString &remoteFile, const QString &localDir)
{
    if (m_transfer || remoteFile.isEmpty() || localDir.isEmpty())
        return;
    if (!m_remotePane->isIdle()) {
        QMessageBox::information(this, tr("ダウンロード"),
                                 tr("いまは転送や一覧の読み込み中です。"));
        return;
    }

    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this, tr("ZMODEM ダウンロード"),
        tr("sgnano の\n%1\nを\n%2\nへ受け取ります。\n\n"
           "同名ファイルは上書きされます。実行しますか？")
            .arg(remoteFile, localDir),
        QMessageBox::Ok | QMessageBox::Cancel);
    if (answer != QMessageBox::Ok)
        return;

    launchTransfer(true, QStringList(), localDir, remoteFile);
}

void MainWindow::launchTransfer(bool download, const QStringList &files, const QString &localDir,
                                const QString &remotePath)
{
    if (m_transfer)
        return;

    const QString title = download ? tr("ZMODEM ダウンロード")
                                   : tr("ZMODEM アップロード");
    const QString port = m_remotePane->currentPort();
    const int baud = m_remotePane->currentBaud();
    if (port.isEmpty()) {
        QMessageBox::warning(this, title, tr("シリアルポートが選択されていません。"));
        return;
    }

    m_transferDownload = download;
    m_transferCanceled = false;
    m_transferRefreshPath = download ? remoteParentPath(remotePath) : remotePath;

    m_remotePane->disconnectRemote();

    m_transferProgressBaseText = download
        ? tr("ダウンロード中:\n%1\n→ %2").arg(remotePath, localDir)
        : tr("アップロード中:\n%1\n→ %2").arg(files.join(QStringLiteral(", ")), remotePath);
    m_transferProgress = new QProgressDialog(m_transferProgressBaseText, tr("キャンセル"), 0, 0, this);
    m_transferProgress->setWindowTitle(title);
    m_transferProgress->setWindowModality(Qt::WindowModal);
    m_transferProgress->setAutoClose(false);
    m_transferProgress->setAutoReset(false);
    m_transferProgress->setMinimumDuration(0);
    m_transferProgress->setMinimumWidth(520);
    connect(m_transferProgress, &QProgressDialog::canceled, this, &MainWindow::cancelTransfer);
    m_transferProgress->show();

    if (download)
        appendLog(tr("内蔵 ZMODEM ダウンロード: %1 -> %2").arg(remotePath, localDir));
    else
        appendLog(tr("内蔵 ZMODEM アップロード: %1 -> %2")
                      .arg(files.join(QStringLiteral(", ")), remotePath));

    auto *thread = new QThread(this);
    auto *worker = new ZmodemWorker(port, baud,
                                    download ? ZmodemWorker::Mode::Download
                                             : ZmodemWorker::Mode::Upload,
                                    files, localDir, remotePath);
    worker->moveToThread(thread);
    m_transfer = worker;

    connect(thread, &QThread::started, worker, &ZmodemWorker::run);
    connect(worker, &ZmodemWorker::progress, this, &MainWindow::onTransferProgress);
    connect(worker, &ZmodemWorker::logMessage, this, &MainWindow::appendLog);
    connect(worker, &ZmodemWorker::finished, this, &MainWindow::finishTransfer);
    connect(worker, &ZmodemWorker::finished, thread, &QThread::quit);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    thread->start();
}

void MainWindow::onTransferProgress(qint64 done, qint64 total)
{
    if (!m_transferProgress || m_transferCanceled)
        return;
    if (total <= 0) {
        if (m_transferProgress->maximum() != 0)
            m_transferProgress->setRange(0, 0);
        return;
    }

    const int maxValue = int(qMin<qint64>(total, std::numeric_limits<int>::max()));
    const int value = qBound(0, int(double(done) / double(total) * maxValue), maxValue);
    if (m_transferProgress->maximum() != maxValue)
        m_transferProgress->setRange(0, maxValue);
    m_transferProgress->setValue(value);

    const int percent = maxValue > 0 ? int(double(value) / double(maxValue) * 100.0) : 0;
    m_transferProgress->setLabelText(
        m_transferProgressBaseText
        + QStringLiteral("\n\n%1 / %2  (%3%)")
              .arg(QLocale().formattedDataSize(done),
                   QLocale().formattedDataSize(total))
              .arg(percent));
}

void MainWindow::cancelTransfer()
{
    if (!m_transfer || m_transferCanceled)
        return;

    m_transferCanceled = true;
    appendLog(tr("内蔵 ZMODEM 転送をキャンセルしています…"));
    if (m_transferProgress) {
        m_transferProgress->setCancelButton(nullptr);
        m_transferProgress->setLabelText(tr("キャンセルしています…"));
    }
    m_transfer->cancel();
}

void MainWindow::finishTransfer(bool ok, bool canceled, const QString &error)
{
    if (!m_transfer)
        return;
    m_transfer = nullptr;

    if (m_transferProgress) {
        m_transferProgress->close();
        m_transferProgress->deleteLater();
        m_transferProgress = nullptr;
    }
    m_transferProgressBaseText.clear();

    const bool download = m_transferDownload;
    const QString refreshPath = m_transferRefreshPath;
    m_transferDownload = false;
    m_transferCanceled = false;
    m_transferRefreshPath.clear();

    if (canceled) {
        appendLog(tr("内蔵 ZMODEM 転送をキャンセルしました"));
        statusBar()->showMessage(tr("転送をキャンセルしました"), 5000);
    } else if (ok) {
        appendLog(download ? tr("内蔵 ZMODEM ダウンロード完了")
                           : tr("内蔵 ZMODEM アップロード完了"));
        statusBar()->showMessage(download ? tr("ダウンロードしました")
                                          : tr("アップロードしました"), 5000);
    } else {
        appendLog(tr("内蔵 ZMODEM 転送失敗: %1").arg(error));
        QMessageBox::warning(this, tr("ZMODEM"),
                             tr("転送に失敗しました。\n\n%1\n\n"
                                "右ペインのリモートパスと、sgnano の転送状態を確認してください。")
                                 .arg(error.isEmpty() ? tr("理由は分かりません") : error));
    }

    QTimer::singleShot(1500, this, [this, download, refreshPath] {
        if (download)
            m_localPane->refresh();
        if (!refreshPath.isEmpty())
            m_remotePane->refreshRemoteAt(refreshPath);
        m_remotePane->reconnectRemote();
    });
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_transfer) {
        QMessageBox::information(this, tr("ZMODEM"), tr("転送が終わるまで終了できません。"));
        event->ignore();
        return;
    }

    m_remotePane->saveSettings();
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/splitter"), m_splitter->saveState());
    settings.setValue(QStringLiteral("window/dock"), saveState());
    QMainWindow::closeEvent(event);
}
