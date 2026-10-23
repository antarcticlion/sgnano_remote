#pragma once

#include "zmodem.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>

// シリアルポートを開き、sgnano の ZPUT / ZGET を始めてから内蔵の ZMODEM を走らせる。
// 走らせるのは別のスレッド（MainWindow が QThread に移す）。進み具合と結果は signal で返す。
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
    bool waitForRemote(ZmodemIo &io, QString *error);

    QString m_port;
    int m_baud = 921600;
    Mode m_mode = Mode::Upload;
    QStringList m_files;
    QString m_localDir;
    QString m_remotePath;
    std::atomic_bool m_canceled{ false };
};
