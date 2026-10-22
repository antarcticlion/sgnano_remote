// sgnano_remote の試し（2026-10-23）。
//   sgnano_remote_selftest                      … 実機なし: RemoteProtocol に答えを流し込み、DEL / REN / MKDIR の送る行と OK / ERR の受け取りを確かめる
//   sgnano_remote_selftest --device /dev/ttyACM0 … 実機あり: DRIVES の種類で HDD の exFAT（3）と NTFS（4）を選び、作る・名前を変える・消すを通す。
//                                                  作ったものは消してから終わる。FAT（1）には書かない（ドライブ文字はつなぐ順で変わるので、種類で選ぶ）
#include "remoteprotocol.h"
#include "seriallink.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>
#include <functional>

namespace {

int g_fail = 0;

void check(bool ok, const QString &what)
{
    std::printf("%s  %s\n", ok ? "OK" : "NG", qPrintable(what));
    if (!ok)
        ++g_fail;
}

// ---- 実機なし ----
struct Probe
{
    QList<QByteArray> sent;
    QStringList ok, failed, logs;
};

void hookProbe(RemoteProtocol &p, Probe &pr)
{
    QObject::connect(&p, &RemoteProtocol::transmit, [&pr](const QByteArray &d) { pr.sent.append(d); });
    QObject::connect(&p, &RemoteProtocol::operationSucceeded,
                     [&pr](const QString &op, const QString &path) { pr.ok.append(op + QLatin1Char('|') + path); });
    QObject::connect(&p, &RemoteProtocol::requestFailed, [&pr](const QString &c, int code, const QString &m) {
        pr.failed.append(QStringLiteral("%1|%2|%3").arg(c).arg(code).arg(m));
    });
    QObject::connect(&p, &RemoteProtocol::logMessage, [&pr](const QString &m) { pr.logs.append(m); });
}

void offlineTests()
{
    RemoteProtocol p;
    Probe pr;
    hookProbe(p, pr);

    // 同期を済ませた形にする（PING -> PONG）
    p.sync();
    p.feed("@SG\tPONG\tSGNANO\r\n");
    check(p.isSynchronized() && !p.isBusy(), QStringLiteral("PING -> PONG で同期する"));

    pr.sent.clear();
    p.requestDelete(QStringLiteral("B:/a dir/x.bin"));
    check(pr.sent.size() == 1 && pr.sent.last() == QByteArray("\x1e" "DEL B:/a dir/x.bin\n"), QStringLiteral("DEL の送る行"));
    p.feed("@SG\tOK\tDEL\tB:/a dir/x.bin\r\n");
    check(pr.ok.size() == 1 && pr.ok.last() == QStringLiteral("DEL|B:/a dir/x.bin") && !p.isBusy(), QStringLiteral("OK DEL を受けて終わる"));

    p.requestRename(QStringLiteral("B:/d/e.bin"), QStringLiteral("f g.bin"));
    check(pr.sent.last() == QByteArray("\x1e" "REN B:/d/e.bin\tf g.bin\n"), QStringLiteral("REN はタブで区切る"));
    p.feed("@SG\tOK\tREN\tB:/d/f g.bin\n");
    check(pr.ok.last() == QStringLiteral("REN|B:/d/f g.bin"), QStringLiteral("OK REN を受ける（結果のパス）"));

    p.requestMkdir(QStringLiteral("C:/新しい\\x"));
    check(pr.sent.last() == QString(QStringLiteral("\x1e") + QStringLiteral("MKDIR C:/新しい\\x\n")).toUtf8(), QStringLiteral("MKDIR の送る行（UTF-8）"));
    p.feed("@SG\tERR\t4\tDirectory not empty\n");
    check(pr.failed.size() == 1 && pr.failed.last() == QStringLiteral("MKDIR C:/新しい\\x|4|Directory not empty") && !p.isBusy(),
          QStringLiteral("ERR を requestFailed で返す（番号と理由）"));

    // 断る形: 空・制御文字
    const int sentBefore = int(pr.sent.size());
    p.requestDelete(QString());
    p.requestDelete(QStringLiteral("B:/a\tb"));
    p.requestRename(QStringLiteral("B:/a"), QStringLiteral("x\ny"));
    p.requestMkdir(QStringLiteral("  "));
    check(int(pr.sent.size()) == sentBefore && pr.failed.size() == 5, QStringLiteral("空のパス・制御文字は送らずに断る"));

    // 合わない OK（いま待っているのが LIST）は受け流し、LIST はそのまま続く
    p.requestList(QStringLiteral("B:/"));
    p.feed("@SG\tOK\tDEL\tB:/zz\n");
    check(p.isBusy() && pr.ok.size() == 2, QStringLiteral("待っていない OK は受け流す"));
    p.feed("@SG\tLIST\tBEGIN\tB:/\n@SG\tLIST\tEND\t0\t0\n");
    check(!p.isBusy(), QStringLiteral("LIST は続けて終わる"));

    // 並べた DEL は順に 1 つずつ送る
    pr.sent.clear();
    p.requestDelete(QStringLiteral("B:/1"));
    p.requestDelete(QStringLiteral("B:/2"));
    check(pr.sent.size() == 1, QStringLiteral("2 つ目は 1 つ目の答えまで送らない"));
    p.feed("@SG\tERR\t1\tNo such file or directory\n");
    check(pr.sent.size() == 2 && pr.sent.last() == QByteArray("\x1e" "DEL B:/2\n"), QStringLiteral("1 つ目が失敗しても 2 つ目を送る"));
    p.feed("@SG\tOK\tDEL\tB:/2\n");
    check(!p.isBusy(), QStringLiteral("並べた分が終わる"));
}

// ---- 実機あり ----
class Device
{
public:
    Device() { QObject::connect(&link, &SerialLink::dataReceived, &proto, &RemoteProtocol::feed);
               QObject::connect(&proto, &RemoteProtocol::transmit, [this](const QByteArray &d) { link.write(d); });
               QObject::connect(&proto, &RemoteProtocol::drivesReceived, [this](const QList<RemoteDrive> &d) { drives = d; });
               QObject::connect(&proto, &RemoteProtocol::entryReceived, [this](const RemoteEntry &e) { entries.append(e); });
               QObject::connect(&proto, &RemoteProtocol::listStarted, [this](const QString &) { entries.clear(); });
               QObject::connect(&proto, &RemoteProtocol::operationSucceeded, [this](const QString &op, const QString &) { lastOk = op; });
               QObject::connect(&proto, &RemoteProtocol::requestFailed, [this](const QString &, int code, const QString &m) {
                   lastErr = code; lastErrText = m; });
               QObject::connect(&proto, &RemoteProtocol::requestTimedOut, [this](const QString &c) { lastErr = -2; lastErrText = c; }); }

    bool open(const QString &port)
    {
        QString err;
        if (!link.open(port, 921600, &err)) { std::printf("cannot open %s: %s\n", qPrintable(port), qPrintable(err)); return false; }
        link.write(QByteArray(10, '\x18'));                // アプリと同じく、残った ZMODEM を取り消してから
        wait(700);
        proto.reset();
        proto.sync();
        return waitIdle(10000) && proto.isSynchronized();
    }
    void wait(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }
    bool waitIdle(int ms)
    {
        QElapsedTimer t; t.start();
        while (proto.isBusy() && t.elapsed() < ms) wait(20);
        return !proto.isBusy();
    }
    // 1 つ送って終わるまで待つ。OK なら true
    bool run(const std::function<void()> &f, int ms = 40000)
    {
        lastOk.clear(); lastErr = 0; lastErrText.clear();
        f();
        waitIdle(ms);
        return lastErr == 0;
    }
    bool listHas(const QString &dir, const QString &name, bool *isDir = nullptr)
    {
        entries.clear();
        run([&] { proto.requestList(dir); }, 15000);
        for (const RemoteEntry &e : entries)
            if (e.name == name) { if (isDir) *isDir = e.isDir; return true; }
        return false;
    }

    SerialLink link;
    RemoteProtocol proto;
    QList<RemoteDrive> drives;
    QList<RemoteEntry> entries;
    QString lastOk, lastErrText;
    int lastErr = 0;
};

void deviceTests(const QString &port)
{
    Device d;
    check(d.open(port), QStringLiteral("実機と同期する（%1）").arg(port));
    if (g_fail) return;
    d.run([&] { d.proto.requestDrives(); });
    QString letters;
    for (const RemoteDrive &r : d.drives) letters += QStringLiteral("%1=%2 ").arg(r.letter).arg(r.type);
    std::printf("    drives: %s\n", qPrintable(letters));
    for (int type : { 3, 4 }) {
        QString drv;
        int n = 0;
        for (const RemoteDrive &r : d.drives) if (r.type == type && r.ready) { drv = r.letter; ++n; }
        const QString label = type == 3 ? QStringLiteral("exFAT") : QStringLiteral("NTFS");
        if (n != 1) { check(false, QStringLiteral("%1 の区画がちょうど 1 つある（%2 個）").arg(label).arg(n)); continue; }
        const QString root = drv + QStringLiteral(":/");
        const QString dir = root + QStringLiteral("!qt_selftest");
        std::printf("  -- %s %s\n", qPrintable(label), qPrintable(root));
        if (d.listHas(root, QStringLiteral("!qt_selftest"))) {   // 前の回の残り
            d.run([&] { d.proto.requestDelete(dir + QStringLiteral("/sub dir")); });
            d.run([&] { d.proto.requestDelete(dir + QStringLiteral("/sub")); });
            d.run([&] { d.proto.requestDelete(dir); });
        }
        check(d.run([&] { d.proto.requestMkdir(dir); }) && d.lastOk == QStringLiteral("MKDIR"), QStringLiteral("%1: MKDIR").arg(label));
        bool isDir = false;
        check(d.listHas(root, QStringLiteral("!qt_selftest"), &isDir) && isDir, QStringLiteral("%1: 作ったフォルダが一覧にある").arg(label));
        check(d.run([&] { d.proto.requestMkdir(dir + QStringLiteral("/sub")); }), QStringLiteral("%1: 中にもう 1 つ MKDIR").arg(label));
        check(d.run([&] { d.proto.requestRename(dir + QStringLiteral("/sub"), QStringLiteral("sub dir")); }) && d.lastOk == QStringLiteral("REN"),
              QStringLiteral("%1: REN（空白を含む名前）").arg(label));
        check(d.listHas(dir, QStringLiteral("sub dir")) && !d.listHas(dir, QStringLiteral("sub")), QStringLiteral("%1: 名前が変わっている").arg(label));
        const bool refused = !d.run([&] { d.proto.requestDelete(dir); }) && d.lastErr == 4;   // 理由は run のあとで読む（引数の評価の順は決まっていない）
        check(refused, QStringLiteral("%1: 中身のあるフォルダの DEL は断られる（%2）").arg(label, d.lastErrText));
        check(!d.run([&] { d.proto.requestDelete(dir + QStringLiteral("/nothing")); }) && d.lastErr == 1, QStringLiteral("%1: 無いものの DEL は誤り").arg(label));
        check(d.run([&] { d.proto.requestDelete(dir + QStringLiteral("/sub dir")); }), QStringLiteral("%1: DEL（中のフォルダ）").arg(label));
        check(d.run([&] { d.proto.requestDelete(dir); }) && d.lastOk == QStringLiteral("DEL"), QStringLiteral("%1: DEL（空になったフォルダ）").arg(label));
        check(!d.listHas(root, QStringLiteral("!qt_selftest")), QStringLiteral("%1: 残っていない").arg(label));
    }
    d.link.close();
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    offlineTests();
    const int at = int(args.indexOf(QStringLiteral("--device")));
    if (at > 0 && at + 1 < args.size())
        deviceTests(args.at(at + 1));
    std::printf("%s\n", g_fail ? qPrintable(QStringLiteral("selftest: %1 NG").arg(g_fail)) : "selftest: all ok");
    return g_fail ? 1 : 0;
}
