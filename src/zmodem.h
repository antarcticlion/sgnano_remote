#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

// ZMODEM の読み書きの口。ブロックして待つ（時間切れ・中止・誤りは負の値で返す）。
// 実機ではシリアルポート（src/zmodemworker.cpp）、試しでは記憶の上でつなぐ（tests/zmodem_selftest.cpp）。
class ZmodemIo
{
public:
    enum : int {
        Timeout = -1,
        Cancel = -2,
        Error = -3,
    };

    virtual ~ZmodemIo() = default;

    // 1 バイト（0..255）か Timeout / Cancel / Error
    virtual int getc(unsigned timeoutMs) = 0;
    virtual bool put(const QByteArray &data) = 0;
    virtual bool flush() = 0;
    virtual bool rxReady() = 0;
    virtual QString errorString() const { return QString(); }
    // 読んだバイトを戻す（見出しのあとの先読みや、手順の前に読んだ分を戻すのに使う）
    virtual void pushBack(const QByteArray &) {}
};

// sgnano 本体の src/filer/zmodem.c と同じ決まりで、PC 側の送受信をする。
// - 外部の sz / rz / python は使わない
// - 送るときは 16 進の見出し（制御）と 2 進の見出し（ZFILE / ZDATA / ZEOF、CRC16）を使う
// - 受け取るときは 16 進・2 進（CRC16 / CRC32）のどれでも読む
class Zmodem
{
public:
    using Progress = std::function<void(qint64 done, qint64 total)>;

    explicit Zmodem(ZmodemIo *io, Progress progress = {});
    ~Zmodem();

    Zmodem(const Zmodem &) = delete;
    Zmodem &operator=(const Zmodem &) = delete;

    // 複数のファイルを 1 つの ZMODEM の並びで送る（相手は sgnano の zm_receive）。
    bool sendFiles(const QStringList &paths, QString *error);

    // localDir へ受け取る（相手は sgnano の zm_send）。受け取ったパスを received に入れる。
    bool receiveFiles(const QString &localDir, QStringList *received, QString *error);

private:
    class Private;
    std::unique_ptr<Private> d;
};
