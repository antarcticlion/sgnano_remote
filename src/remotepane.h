#pragma once

#include "remoteprotocol.h"

#include <QByteArray>
#include <QList>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QModelIndex;
class QPushButton;
class QSortFilterProxyModel;
class QStandardItemModel;
class QStackedWidget;
class QToolButton;
class QTreeView;
class RemoteDeleteQueue;
class SerialLink;

class RemotePane : public QWidget
{
    Q_OBJECT

public:
    explicit RemotePane(QWidget *parent = nullptr);
    ~RemotePane() override;

    QString currentPort() const;
    int currentBaud() const;
    QString currentRemotePath() const;
    QStringList selectedRemoteFiles() const;
    bool isIdle() const;
    void saveSettings();

public slots:
    void refreshPorts();
    void disconnectRemote();
    void reconnectRemote();
    void toggleConnection();
    void refreshRemote();
    void refreshRemoteAt(const QString &path);
    void goUpRemote();
    // ファイルの操作（2026-10-23）: 選んだ項目を消す（確かめる）・名前を変える・表示中のフォルダに新しいフォルダを作る
    void deleteSelected();
    void renameSelected();
    void createFolder();

signals:
    void connectionStateChanged(bool connected, const QString &description);
    void logMessage(const QString &message);
    void uploadRequested();
    void downloadRequested();
    void localFilesDropped(const QStringList &files);

private slots:
    void onOpened(const QString &port, int baud);
    void onClosed();
    void onDataReceived(const QByteArray &data);
    void onError(const QString &message);
    void onProtocolSynchronized();
    void onDrivesReceived(const QList<RemoteDrive> &drives);
    void onCwdReceived(const QString &path);
    void onListStarted(const QString &path);
    void onEntryReceived(const RemoteEntry &entry);
    void onListFinished(int count, bool truncated);
    void onStatReceived(const RemoteStat &stat);
    void onRequestFailed(const QString &command, int code, const QString &message);
    void onRequestTimedOut(const QString &command);
    void onConsoleLine(const QString &line);
    void onBusyChanged(bool busy);
    void navigateRemotePath();
    void openRemoteIndex(const QModelIndex &index);
    void onDriveActivated(int index);
    void updateTransferButtons();
    void onOperationSucceeded(const QString &operation, const QString &path);
    void showContextMenu(const QPoint &pos);

private:
    void buildUi();
    void loadSettings();
    void setConnectedUi(bool connected);
    void updateCounters();
    void requestList(const QString &path);
    void populateTree();
    void applyRemotePath(const QString &path);
    void syncDriveCombo();
    struct SelectedItem { QString name; bool isDir = false; };
    QList<SelectedItem> selectedItems() const;
    bool operationsInProgress() const;
    bool canNavigate();
    void clearOperations();
    void operationFinished();
    void finishOperations();

    SerialLink *m_link = nullptr;
    RemoteProtocol *m_protocol = nullptr;
    RemoteDeleteQueue *m_deleteQueue = nullptr;
    QComboBox *m_portCombo = nullptr;
    QComboBox *m_baudCombo = nullptr;
    QToolButton *m_scanButton = nullptr;
    QPushButton *m_connectButton = nullptr;
    QComboBox *m_driveCombo = nullptr;
    QPushButton *m_upButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_uploadButton = nullptr;
    QPushButton *m_downloadButton = nullptr;
    QPushButton *m_mkdirButton = nullptr;
    QPushButton *m_renameButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLineEdit *m_remotePath = nullptr;
    QLabel *m_hint = nullptr;
    QLabel *m_counters = nullptr;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_emptyPage = nullptr;
    QWidget *m_treePage = nullptr;
    QTreeView *m_tree = nullptr;
    QStandardItemModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    QList<RemoteEntry> m_entries;
    QString m_remotePathText;
    QString m_pendingRefreshPath;
    bool m_updatingDrives = false;
    bool m_cancelingStray = false;
    quint64 m_openGeneration = 0;
    quint64 m_rxBytes = 0;
    int m_opsPending = 0;           // 送信済みで返事を待っている DEL / REN / MKDIR の数
    QStringList m_opErrors;         // そのまとまりの失敗（終わったらまとめて出す）
};
