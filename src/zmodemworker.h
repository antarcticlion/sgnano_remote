#pragma once

#include "zmodem.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <QTemporaryDir>

#include <atomic>
#include <memory>

// シリアルポートを開き、sgnano の ZPUT / ZGET を始めてから内蔵の ZMODEM を走らせる。
// 走らせるのは別のスレッド（MainWindow が QThread に移す）。進み具合と結果は signal で返す。
// 4 MB を越えるファイルは PC で name.001 .. に分けて送り、ZMODEM のあとに本体の JOIN でつなぐ
// （2026-11-14。sgnano の scripts/zm.py と同じ形）。
class ZmodemWorker : public QObject
{
    Q_OBJECT

public:
    enum class Mode {
        Upload,     // PC -> sgnano（sgnano は受け取る）
        Download,   // sgnano -> PC（sgnano は送る）
    };

    ZmodemWorker(QString port, int baud, Mode mode, QStringList files,
                 QString localDir, QString remotePath, QObject *parent = nullptr);

    // どのスレッドから呼んでもよい（中の読み書きは 50ms ごとに見る）
    void cancel();

public slots:
    void run();

signals:
    void progress(qint64 done, qint64 total);
    void logMessage(const QString &message);
    void finished(bool ok, bool canceled, const QString &error);

private:
    struct JoinJob {
        QString baseName;
        int parts = 0;
    };

    bool prepareUpload(QStringList *sendFiles, QList<JoinJob> *joins, QString *error);
    bool waitForRemote(ZmodemIo &io, QString *error);
    bool waitForRemoteCommands(ZmodemIo &io, QString *error);
    bool runRemoteCommand(ZmodemIo &io, const QString &command, int timeoutMs,
                          QString *response, QString *error);
    bool joinUploadedFiles(ZmodemIo &io, const QList<JoinJob> &joins, QString *error);

    QString m_port;
    int m_baud = 921600;
    Mode m_mode = Mode::Upload;
    QStringList m_files;
    QString m_localDir;
    QString m_remotePath;
    std::unique_ptr<QTemporaryDir> m_splitDir;
    std::atomic_bool m_canceled{ false };
};
