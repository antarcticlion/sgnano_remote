#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

class QTimer;

struct RemoteDrive
{
    QString letter;
    int type = 0;
    bool ready = false;

    // sgnano 本体（2026-10-24）では S: は内蔵フラッシュのシステムのドライブに決まっている。
    // USB のドライブは A から順に S を飛ばすので、文字だけで見分けられる。
    bool isSystem() const
    {
        return letter.size() == 1
            && (letter.at(0) == QLatin1Char('S') || letter.at(0) == QLatin1Char('s'));
    }
};

struct RemoteEntry
{
    QString name;
    bool isDir = false;
    quint64 size = 0;
    quint64 mtime = 0;
    uint attr = 0;
};

struct RemoteStat
{
    QString name;
    bool isDir = false;
    quint64 size = 0;
    quint64 mtime = 0;
    uint attr = 0;
};

class RemoteProtocol : public QObject
{
    Q_OBJECT

public:
    explicit RemoteProtocol(QObject *parent = nullptr);

    bool isSynchronized() const;
    bool isBusy() const;

public slots:
    void feed(const QByteArray &data);
    void reset();
    void sync();
    void requestCwd();
    void requestDrives();
    void requestList(const QString &path);
    void requestStat(const QString &path);
    // ファイルの操作（2026-10-23、sgnano の遠隔コマンド DEL / REN / MKDIR）。結果は operationSucceeded か requestFailed
    void requestDelete(const QString &path);
    void requestRename(const QString &path, const QString &newName);
    void requestMkdir(const QString &path);

signals:
    void transmit(const QByteArray &data);
    void synchronized();
    void drivesReceived(const QList<RemoteDrive> &drives);
    void cwdReceived(const QString &path);
    void listStarted(const QString &path);
    void entryReceived(const RemoteEntry &entry);
    void listFinished(int count, bool truncated);
    void statReceived(const RemoteStat &stat);
    void operationSucceeded(const QString &operation, const QString &path);   // operation = DEL / REN / MKDIR、path は実機が返した結果のパス
    void requestFailed(const QString &command, int code, const QString &message);
    void requestTimedOut(const QString &command);
    void consoleLine(const QString &line);
    void busyChanged(bool busy);
    void logMessage(const QString &message);

private:
    enum class RequestType {
        Ping,
        Cwd,
        Drives,
        List,
        Stat,
        Delete,
        Rename,
        Mkdir
    };

    struct Request
    {
        RequestType type = RequestType::Ping;
        QString argument;
        QString argument2;          // REN の新しい名前
        QString command;
    };

    void enqueue(RequestType type, const QString &argument = QString(), const QString &argument2 = QString());
    void pump();
    void completeCurrent();
    void failCurrent(int code, const QString &message);
    void handleTimeout();
    void retryPendingList();
    void handleLine(const QByteArray &line);
    void handleFields(const QList<QByteArray> &fields);
    void handleDrive(const QList<QByteArray> &fields);
    void handleListEntry(const QList<QByteArray> &fields);
    void handleStat(const QList<QByteArray> &fields);
    void handleError(const QList<QByteArray> &fields);
    void handleOk(const QList<QByteArray> &fields);
    void finishList(int count, bool truncated);

    QTimer *m_timeout = nullptr;
    QTimer *m_listRetryTimer = nullptr;
    QByteArray m_rx;
    QList<Request> m_queue;
    Request m_current;
    QList<RemoteDrive> m_drives;
    bool m_hasCurrent = false;
    bool m_synchronized = false;
    bool m_listActive = false;
    QString m_listPath;
    int m_listCount = 0;
    bool m_listTruncated = false;
    int m_pingAttempts = 0;
    int m_listAttempts = 0;
    bool m_hasListRetry = false;
    Request m_listRetryRequest;
};
