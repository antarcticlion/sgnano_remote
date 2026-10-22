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

signals:
    void transmit(const QByteArray &data);
    void synchronized();
    void drivesReceived(const QList<RemoteDrive> &drives);
    void cwdReceived(const QString &path);
    void listStarted(const QString &path);
    void entryReceived(const RemoteEntry &entry);
    void listFinished(int count, bool truncated);
    void statReceived(const RemoteStat &stat);
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
        Stat
    };

    struct Request
    {
        RequestType type = RequestType::Ping;
        QString argument;
        QString command;
    };

    void enqueue(RequestType type, const QString &argument = QString());
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
