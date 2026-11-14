// sgnano_remote の試し（2026-10-23、2026-10-24 に S: を追加）。
//   sgnano_remote_selftest                      … 実機なし: RemoteProtocol に答えを流し込み、DEL / REN / MKDIR の送る行と OK / ERR の受け取りを確かめる。
//                                                  RemoteDeleteQueue の 8 件ずつの送信・途中失敗・タイムアウトも確かめる。
//                                                  S: の見分けとシステムのフォルダの守り（src/remotefs.cpp）も確かめる
//   sgnano_remote_selftest --device /dev/ttyACM0 … 実機あり: DRIVES の種類で HDD の exFAT（3）と NTFS（4）を選び、作る・名前を変える・消すを通す。
//                                                  作ったものは消してから終わる。FAT（1）には書かない（ドライブ文字はつなぐ順で変わるので、種類で選ぶ）。
//                                                  S:（内蔵フラッシュ）と 3 つのシステムのフォルダは見るだけ
//   sgnano_remote_selftest --device-many /dev/ttyACM0 … 実機あり: 34 個のフォルダを 8 件ずつ消せることを確かめる
#include "remotedeletequeue.h"
#include "remotefs.h"
#include "remoteprotocol.h"
#include "seriallink.h"
#include "zmodemworker.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
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
    QList<RemoteDrive> drives;
    RemoteFsckResult fsck;
    bool fsckSeen = false;
};

void hookProbe(RemoteProtocol &p, Probe &pr)
{
    QObject::connect(&p, &RemoteProtocol::transmit, [&pr](const QByteArray &d) { pr.sent.append(d); });
    QObject::connect(&p, &RemoteProtocol::drivesReceived,
                     [&pr](const QList<RemoteDrive> &d) { pr.drives = d; });
    QObject::connect(&p, &RemoteProtocol::operationSucceeded,
                     [&pr](const QString &op, const QString &path) { pr.ok.append(op + QLatin1Char('|') + path); });
    QObject::connect(&p, &RemoteProtocol::fsckFinished,
                     [&pr](const RemoteFsckResult &r) { pr.fsck = r; pr.fsckSeen = true; });
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

    // 待ち行列がいっぱいのときは、黙って捨てずに requestFailed で返す
    RemoteProtocol full;
    Probe fullProbe;
    hookProbe(full, fullProbe);
    full.sync();
    full.feed("@SG\tPONG\tSGNANO\r\n");
    fullProbe.sent.clear();
    for (int i = 0; i < 33; ++i)
        full.requestDelete(QStringLiteral("B:/q%1").arg(i));
    check(fullProbe.sent.size() == 1 && fullProbe.failed.isEmpty(),
          QStringLiteral("待ち行列が埋まるまでは DEL を受け付ける"));
    full.requestDelete(QStringLiteral("B:/q33"));
    check(fullProbe.sent.size() == 1 && fullProbe.failed.size() == 1
              && fullProbe.failed.last().startsWith(QStringLiteral("DEL B:/q33|3|")),
          QStringLiteral("待ち行列がいっぱいなら requestFailed で返す"));
}

// ---- S: のシステムのドライブ（sgnano 本体 2026-10-24） ----
void systemDriveTests()
{
    // 本体（src/storage/sysdisk.c の sysdisk_protected）と同じ見分け方
    check(RemoteFs::isSystemDrivePath(QStringLiteral("S:/")), QStringLiteral("S:/ をシステムのドライブと見分ける"));
    check(RemoteFs::isSystemDrivePath(QStringLiteral("s:/SystemFiles")), QStringLiteral("小文字の s: も見分ける"));
    check(!RemoteFs::isSystemDrivePath(QStringLiteral("A:/")), QStringLiteral("A: はシステムのドライブではない"));
    check(!RemoteFs::isSystemDrivePath(QStringLiteral("S")), QStringLiteral("S（コロンなし）はパスではない"));

    for (const QString &name : RemoteFs::protectedSystemFolders()) {
        check(RemoteFs::isProtectedSystemPath(QStringLiteral("S:/") + name),
              QStringLiteral("S:/%1 はシステムのフォルダ").arg(name));
    }
    check(RemoteFs::isProtectedSystemPath(QStringLiteral("s:/systemfiles/")),
          QStringLiteral("大文字小文字・終わりの / を無視して見分ける"));
    check(!RemoteFs::isProtectedSystemPath(QStringLiteral("S:/SystemSettings/foo")),
          QStringLiteral("システムのフォルダの中は守らない"));
    check(!RemoteFs::isProtectedSystemPath(QStringLiteral("A:/SystemSettings")),
          QStringLiteral("S: 以外の同名フォルダは守らない"));
    check(!RemoteFs::isProtectedSystemPath(QStringLiteral("S:/SystemSettings2")),
          QStringLiteral("名前が違えば守らない"));
    check(!RemoteFs::isProtectedSystemPath(QStringLiteral("S:/")),
          QStringLiteral("S: のルートは守らない"));

    // DRIVES の S:（本体は種類 FAT = 1、最後の 1 つとして返す）
    RemoteProtocol p;
    Probe pr;
    hookProbe(p, pr);
    p.sync();
    p.feed("@SG\tPONG\tSGNANO\r\n");
    p.requestDrives();
    p.feed("@SG\tDRV\tA\t3\t1\r\n"
           "@SG\tDRV\tS\t1\t1\r\n"
           "@SG\tDRIVES\tEND\t2\r\n");
    check(pr.drives.size() == 2
              && pr.drives.at(0).letter == QStringLiteral("A") && !pr.drives.at(0).isSystem()
              && pr.drives.at(1).letter == QStringLiteral("S") && pr.drives.at(1).isSystem()
              && pr.drives.at(1).type == 1 && pr.drives.at(1).ready && !p.isBusy(),
          QStringLiteral("DRIVES で S:（内蔵フラッシュ、FAT）を読む"));
}

// ---- 2026-11-14 の sgnano に合わせた遠隔コマンド ----
void latestFirmwareTests()
{
    RemoteProtocol p;
    Probe pr;
    hookProbe(p, pr);
    p.sync();
    p.feed("@SG\tPONG\tSGNANO\r\n");
    pr.sent.clear();

    // DRIVES は 2026-11-08 から台番号（msc）と区画の先頭（plba）も返す。古い形（5 欄）も読める
    p.requestDrives();
    p.feed("@SG\tDRV\tA\t3\t1\t2\t239223\r\n"
           "@SG\tDRV\tS\t1\t1\r\n"
           "@SG\tDRIVES\tEND\t2\r\n");
    check(pr.drives.size() == 2
              && pr.drives.at(0).letter == QStringLiteral("A") && pr.drives.at(0).msc == 2
              && pr.drives.at(0).plba == 239223
              && pr.drives.at(1).letter == QStringLiteral("S") && pr.drives.at(1).msc == -1
              && !p.isBusy(),
          QStringLiteral("DRIVES の追加の欄（msc / plba）を読み、古い 5 欄も読む"));

    // COPY 元<TAB>先（2026-10-24）
    p.requestCopy(QStringLiteral("B:/a b.txt"), QStringLiteral("C:/c d.txt"));
    check(pr.sent.last() == QByteArray("\x1e" "COPY B:/a b.txt\tC:/c d.txt\n"),
          QStringLiteral("COPY はタブで区切る"));
    p.feed("@SG\tOK\tCOPY\tC:/c d.txt\n");
    check(pr.ok.last() == QStringLiteral("COPY|C:/c d.txt") && !p.isBusy(),
          QStringLiteral("OK COPY を受けて終わる"));

    // JOIN / SPLIT は結果のあとに数が付く（2026-10-24）
    p.requestJoin(QStringLiteral("B:/big.001"));
    check(pr.sent.last() == QByteArray("\x1e" "JOIN B:/big.001\n"), QStringLiteral("JOIN の送る行"));
    p.feed("@SG\tOK\tJOIN\tB:/big\t3\n");
    check(pr.ok.last() == QStringLiteral("JOIN|B:/big"), QStringLiteral("OK JOIN を受ける（数は読み飛ばす）"));

    p.requestSplit(QStringLiteral("B:/big"), 4u * 1024u * 1024u);
    check(pr.sent.last() == QByteArray("\x1e" "SPLIT B:/big\t4194304\n"),
          QStringLiteral("SPLIT はタブで大きさを付ける"));
    p.feed("@SG\tOK\tSPLIT\tB:/big\t3\n");
    check(pr.ok.last() == QStringLiteral("SPLIT|B:/big"), QStringLiteral("OK SPLIT を受ける"));

    // SYSFSCK（2026-11-12）
    p.requestSysFsck(false);
    check(pr.sent.last() == QByteArray("\x1e" "SYSFSCK\n"), QStringLiteral("SYSFSCK の送る行"));
    p.feed("@SG\tSYSFSCK\tCHECK\tfiles=5\tdirs=3\tsize_fixed=1\tfreed=2\tcross=0\tbad=0\tfat2=1\n");
    check(pr.fsckSeen && !pr.fsck.repair && pr.fsck.files == 5 && pr.fsck.dirs == 3
              && pr.fsck.sizeFixed == 1 && pr.fsck.freed == 2 && pr.fsck.fat2Fixed == 1 && !p.isBusy(),
          QStringLiteral("SYSFSCK の結果を読む"));

    pr.fsckSeen = false;
    p.requestSysFsck(true);
    check(pr.sent.last() == QByteArray("\x1e" "SYSFSCK REPAIR\n"), QStringLiteral("SYSFSCK REPAIR の送る行"));
    p.feed("@SG\tSYSFSCK\tREPAIR\tfiles=0\tdirs=0\tsize_fixed=0\tfreed=0\tcross=0\tbad=0\tfat2=0\n");
    check(pr.fsckSeen && pr.fsck.repair, QStringLiteral("SYSFSCK REPAIR の結果を読む"));

    // 512 の倍数でない分割の大きさは送らない
    const int sentBefore = int(pr.sent.size());
    const int failedBefore = int(pr.failed.size());
    p.requestSplit(QStringLiteral("B:/big"), 1000);
    check(int(pr.sent.size()) == sentBefore && int(pr.failed.size()) == failedBefore + 1,
          QStringLiteral("512 の倍数でない SPLIT は送らずに断る"));

    // 1 行の上限（2026-11-14 の uartcon.h は 320 バイト。頭の 0x1E と改行を除く）
    const QString fits(315, QLatin1Char('a'));       // "DEL " 4 + 315 = 319
    p.requestDelete(fits);
    check(pr.sent.last() == QByteArray("\x1e" "DEL ") + fits.toUtf8() + '\n',
          QStringLiteral("319 バイトのコマンドは送る"));
    p.feed(QByteArray("@SG\tOK\tDEL\t") + fits.toUtf8() + '\n');

    const int beforeTooLong = int(pr.sent.size());
    const int failTooLong = int(pr.failed.size());
    p.requestDelete(QString(316, QLatin1Char('a')));   // "DEL " 4 + 316 = 320
    check(int(pr.sent.size()) == beforeTooLong && int(pr.failed.size()) == failTooLong + 1,
          QStringLiteral("320 バイトのコマンドは送らずに断る"));

    // 本体のドライブの種類（FDRV_FAT_MSC=1, FAT_FLOPPY=2, EXFAT=3, NTFS=4 が書き込み可）
    check(RemoteFs::isWritableDriveType(1) && RemoteFs::isWritableDriveType(2)
              && RemoteFs::isWritableDriveType(3) && RemoteFs::isWritableDriveType(4)
              && !RemoteFs::isWritableDriveType(5) && !RemoteFs::isWritableDriveType(6),
          QStringLiteral("書き込みできるドライブの種類を見分ける"));

    // 2026-11-02 に S: のルートへ増えた bin・sbin は、本体は守らない（消せる）
    check(!RemoteFs::isProtectedSystemPath(QStringLiteral("S:/bin"))
              && !RemoteFs::isProtectedSystemPath(QStringLiteral("S:/sbin")),
          QStringLiteral("S:/bin・S:/sbin はシステムのフォルダではない"));
}

// ---- 一括削除の順番（RemotePane から切り出した RemoteDeleteQueue） ----
void deleteQueueTests()
{
    // 34 件を 8 件ずつ送り、最後の 2 件まで完走する
    {
        RemoteDeleteQueue q;
        QStringList sent;
        int finished = 0;
        QObject::connect(&q, &RemoteDeleteQueue::deleteRequested,
                         [&sent](const QString &p) { sent.append(p); });
        QObject::connect(&q, &RemoteDeleteQueue::finished, [&finished] { ++finished; });

        QStringList paths;
        for (int i = 0; i < 34; ++i)
            paths.append(QStringLiteral("B:/d%1").arg(i, 2, 10, QLatin1Char('0')));

        q.start(paths);
        check(sent.size() == 8 && q.pending() == 8 && q.remaining() == 26 && q.isActive(),
              QStringLiteral("一括削除は最初に 8 件だけ送る"));

        for (int i = 0; i < 7; ++i)
            q.operationFinished();
        check(sent.size() == 8 && q.pending() == 1 && finished == 0,
              QStringLiteral("7 件の返事では次の 8 件を送らない"));

        q.operationFinished();
        check(sent.size() == 16 && q.pending() == 8 && q.remaining() == 18,
              QStringLiteral("8 件終わったら次の 8 件を送る"));

        for (int i = 0; i < 8; ++i)
            q.operationFinished();
        check(sent.size() == 24 && q.pending() == 8 && q.remaining() == 10,
              QStringLiteral("3 回目の 8 件"));

        for (int i = 0; i < 8; ++i)
            q.operationFinished();
        check(sent.size() == 32 && q.pending() == 8 && q.remaining() == 2,
              QStringLiteral("4 回目の 8 件"));

        for (int i = 0; i < 8; ++i)
            q.operationFinished();
        check(sent.size() == 34 && q.pending() == 2 && q.remaining() == 0 && q.isActive(),
              QStringLiteral("34 件目まで送る（最後は 2 件）"));

        q.operationFinished();
        check(q.isActive() && q.pending() == 1 && finished == 0,
              QStringLiteral("最後の 2 件のうち 1 件では終わらない"));

        q.operationFinished();
        check(!q.isActive() && q.pending() == 0 && finished == 1,
              QStringLiteral("34 件の一括削除が終わる"));

        q.operationFinished();
        check(finished == 1, QStringLiteral("終わったあとの余分な返事では finished を出さない"));
    }

    // 途中で 1 件失敗しても、残りの返事がそろえば次の 8 件へ進む
    {
        RemoteDeleteQueue q;
        QStringList sent;
        int finished = 0;
        QObject::connect(&q, &RemoteDeleteQueue::deleteRequested,
                         [&sent](const QString &p) { sent.append(p); });
        QObject::connect(&q, &RemoteDeleteQueue::finished, [&finished] { ++finished; });

        QStringList paths;
        for (int i = 0; i < 10; ++i)
            paths.append(QStringLiteral("B:/e%1").arg(i));

        q.start(paths);
        check(sent.size() == 8 && q.pending() == 8, QStringLiteral("途中失敗の試し: 最初の 8 件"));
        q.operationFinished();   // 1 件目が ERR だったことにする
        check(sent.size() == 8 && q.pending() == 7 && q.remaining() == 2,
              QStringLiteral("1 件失敗しても、まだ次の 8 件は送らない"));

        for (int i = 0; i < 7; ++i)
            q.operationFinished();
        check(sent.size() == 10 && q.pending() == 2 && q.remaining() == 0,
              QStringLiteral("8 件分の返事がそろったら残り 2 件を送る"));

        q.operationFinished();
        q.operationFinished();
        check(!q.isActive() && finished == 1, QStringLiteral("途中失敗があっても一括削除は完走する"));
    }

    // タイムアウト: 送っていない分をやめ、それ以上送らない
    {
        RemoteDeleteQueue q;
        QStringList sent;
        int finished = 0;
        QObject::connect(&q, &RemoteDeleteQueue::deleteRequested,
                         [&sent](const QString &p) { sent.append(p); });
        QObject::connect(&q, &RemoteDeleteQueue::finished, [&finished] { ++finished; });

        QStringList paths;
        for (int i = 0; i < 20; ++i)
            paths.append(QStringLiteral("B:/f%1").arg(i));

        q.start(paths);
        check(sent.size() == 8 && q.remaining() == 12, QStringLiteral("タイムアウトの試し: 最初の 8 件"));
        const int remaining = q.remaining();
        q.abortRemaining();
        check(remaining == 12 && !q.isActive() && !q.hasRemaining() && q.pending() == 0 && finished == 0,
              QStringLiteral("タイムアウトで残りを中止する"));

        for (int i = 0; i < 20; ++i)
            q.operationFinished();
        check(sent.size() == 8 && finished == 0, QStringLiteral("中止したあとは送らない"));
    }

    // 返事が同期的に返る場合（待ち行列がいっぱいで即座に失敗する場合など）も、再入で崩れない
    {
        RemoteDeleteQueue q;
        QStringList sent;
        int finished = 0;
        QObject::connect(&q, &RemoteDeleteQueue::deleteRequested, [&q, &sent](const QString &p) {
            sent.append(p);
            q.operationFinished();
        });
        QObject::connect(&q, &RemoteDeleteQueue::finished, [&finished] { ++finished; });

        QStringList paths;
        for (int i = 0; i < 34; ++i)
            paths.append(QStringLiteral("B:/g%1").arg(i));

        q.start(paths);
        check(sent.size() == 34 && finished == 1 && !q.isActive() && q.pending() == 0,
              QStringLiteral("同期的な返事でも 34 件を送り切る"));
    }

    // start し直すと前の並びは捨てる
    {
        RemoteDeleteQueue q;
        QStringList sent;
        QObject::connect(&q, &RemoteDeleteQueue::deleteRequested,
                         [&sent](const QString &p) { sent.append(p); });

        q.start({ QStringLiteral("B:/old1"), QStringLiteral("B:/old2") });
        q.start({ QStringLiteral("B:/new") });
        check(sent.size() == 3 && sent.last() == QStringLiteral("B:/new") && q.total() == 1 && q.pending() == 1,
              QStringLiteral("start し直すと前の並びを捨てる"));
    }

    // RemoteProtocol と組んだ通しの試し: 34 件のうち 1 件だけ ERR でも、残りを送り切る
    {
        RemoteProtocol p;
        Probe pr;
        hookProbe(p, pr);
        p.sync();
        p.feed("@SG\tPONG\tSGNANO\r\n");
        pr.sent.clear();

        RemoteDeleteQueue q;
        int finished = 0;
        QObject::connect(&q, &RemoteDeleteQueue::deleteRequested,
                         [&p](const QString &path) { p.requestDelete(path); });
        QObject::connect(&p, &RemoteProtocol::operationSucceeded,
                         [&q](const QString &, const QString &) { q.operationFinished(); });
        QObject::connect(&p, &RemoteProtocol::requestFailed,
                         [&q](const QString &, int, const QString &) { q.operationFinished(); });
        QObject::connect(&q, &RemoteDeleteQueue::finished, [&finished] { ++finished; });

        QStringList paths;
        for (int i = 0; i < 34; ++i)
            paths.append(QStringLiteral("B:/h%1").arg(i));

        q.start(paths);
        check(pr.sent.size() == 1, QStringLiteral("通しの試し: 最初は 1 つだけ送る"));

        int fed = 0;
        while (q.isActive() && fed < 100) {
            if (pr.sent.isEmpty())
                break;
            ++fed;
            QByteArray command = pr.sent.last();
            command.remove(0, 1);   // 頭の 0x1E
            if (command.endsWith('\n'))
                command.chop(1);
            const QString path = QString::fromUtf8(command).mid(4);   // "DEL " の後ろ
            if (fed == 11)
                p.feed("@SG\tERR\t1\tNo such file or directory\n");
            else
                p.feed(QStringLiteral("@SG\tOK\tDEL\t%1\r\n").arg(path).toUtf8());
        }

        check(finished == 1 && !q.isActive() && !p.isBusy() && fed == 34,
              QStringLiteral("通しの試し: 34 件を送り切る"));
        check(pr.sent.size() == 34 && pr.ok.size() == 33 && pr.failed.size() == 1,
              QStringLiteral("通しの試し: 1 件の ERR があっても残りを送る"));
    }
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
               QObject::connect(&proto, &RemoteProtocol::statReceived, [this](const RemoteStat &s) { stat = s; statSeen = true; });
               QObject::connect(&proto, &RemoteProtocol::fsckFinished, [this](const RemoteFsckResult &r) { fsck = r; fsckSeen = true; });
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
    bool statPath(const QString &path, RemoteStat *out = nullptr)
    {
        statSeen = false;
        stat = RemoteStat();
        const bool ok = run([&] { proto.requestStat(path); }, 15000);
        if (out)
            *out = stat;
        return ok && statSeen;
    }
    bool runFsck(bool repair, RemoteFsckResult *out = nullptr)
    {
        fsckSeen = false;
        fsck = RemoteFsckResult();
        lastOk.clear(); lastErr = 0; lastErrText.clear();
        proto.requestSysFsck(repair);
        waitIdle(120000);
        if (out)
            *out = fsck;
        return fsckSeen && lastErr == 0;
    }
    bool listNames(const QString &dir, QStringList *names)
    {
        entries.clear();
        const bool ok = run([&] { proto.requestList(dir); }, 15000);
        if (names) {
            names->clear();
            for (const RemoteEntry &e : entries)
                names->append(e.name);
        }
        return ok;
    }
    bool listHas(const QString &dir, const QString &name, bool *isDir = nullptr)
    {
        entries.clear();
        run([&] { proto.requestList(dir); }, 15000);
        for (const RemoteEntry &e : entries)
            if (e.name == name) { if (isDir) *isDir = e.isDir; return true; }
        return false;
    }
    // 空のフォルダを消す。前の回の残りを片付けるときだけ使う（深さは 8 まで）
    bool removeTree(const QString &dir, int depth = 0)
    {
        if (depth > 8)
            return false;
        entries.clear();
        if (!run([&] { proto.requestList(dir); }, 15000))
            return false;
        const QList<RemoteEntry> children = entries;
        for (const RemoteEntry &e : children) {
            const QString child = dir + QLatin1Char('/') + e.name;
            if (e.isDir) {
                if (!removeTree(child, depth + 1))
                    return false;
            } else if (!run([&] { proto.requestDelete(child); })) {
                return false;
            }
        }
        return run([&] { proto.requestDelete(dir); });
    }

    SerialLink link;
    RemoteProtocol proto;
    QList<RemoteDrive> drives;
    QList<RemoteEntry> entries;
    RemoteStat stat;
    RemoteFsckResult fsck;
    bool statSeen = false;
    bool fsckSeen = false;
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

    // S:（内蔵フラッシュ）と、そのルートのシステムのフォルダ。消す・名前を変える試しはしない
    // （古い本体や不具合のある本体で本当に消えてしまわないよう、見るだけにする。アプリが名前で断ることは
    //  実機なしの試し（RemoteFs::isProtectedSystemPath）で確かめる）
    const RemoteDrive *systemDrive = nullptr;
    for (const RemoteDrive &r : d.drives) {
        if (r.isSystem())
            systemDrive = &r;
    }
    check(systemDrive && systemDrive->ready, QStringLiteral("S:（内蔵フラッシュ）がある"));
    if (systemDrive && systemDrive->ready) {
        const QString root = systemDrive->letter + QStringLiteral(":/");
        QStringList names;
        check(d.listNames(root, &names), QStringLiteral("S: のルートを読む"));
        for (const QString &name : RemoteFs::protectedSystemFolders()) {
            bool found = false;
            bool isDir = false;
            uint attr = 0;
            for (const RemoteEntry &e : d.entries) {
                if (e.name == name) { found = true; isDir = e.isDir; attr = e.attr; break; }
            }
            check(found && isDir, QStringLiteral("S:/%1 がある").arg(name));
            check(found && (attr & 0x04) != 0, QStringLiteral("S:/%1 はシステムの属性").arg(name));
        }

        // 2026-11-12 の SYSFSCK（読みだけ。修復は実機の大事な中身を変えるので、この試しではしない）
        RemoteFsckResult fsck;
        check(d.runFsck(false, &fsck),
              QStringLiteral("S: の SYSFSCK（検査）が答える（ファイル %1・フォルダ %2）").arg(fsck.files).arg(fsck.dirs));
        check(!fsck.repair, QStringLiteral("SYSFSCK の戻りは CHECK"));
    }

    bool writableTested = false;
    for (int type : { 3, 4 }) {
        QString drv;
        int n = 0;
        for (const RemoteDrive &r : d.drives) if (r.type == type && r.ready) { drv = r.letter; ++n; }
        const QString label = type == 3 ? QStringLiteral("exFAT") : QStringLiteral("NTFS");
        if (n == 0) {
            std::printf("  -- %s: 準備できた区画が無いので飛ばす\n", qPrintable(label));
            continue;
        }
        writableTested = true;
        if (n > 1)
            std::printf("  -- %s: %d 個のうち %s を試す\n", qPrintable(label), n, qPrintable(drv));
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
    check(writableTested, QStringLiteral("exFAT か NTFS の準備できた区画がある"));
    d.link.close();
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(data) == data.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

void waitMs(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// 実機で 4 MB を越えるファイルを送る（2026-11-14 の分割 → ZMODEM → JOIN → 分割の削除）と、
// そのファイルをそのまま受け取る（本体のストリーミング送信）。exFAT と NTFS の両方で確かめる。
void deviceLargeTests(const QString &port)
{
    constexpr int kSize = 4 * 1024 * 1024 + 123;
    // 既定は 2 MB ずつ（環境変数で切り分けられる）

    Device d;
    check(d.open(port), QStringLiteral("実機と同期する（%1）").arg(port));
    if (g_fail)
        return;
    d.run([&] { d.proto.requestDrives(); });

    QStringList targets;
    for (const RemoteDrive &r : d.drives) {
        if ((r.type == 3 || r.type == 4) && r.ready)
            targets.append(r.letter);
    }
    check(!targets.isEmpty(), QStringLiteral("exFAT / NTFS のドライブがある"));
    if (targets.isEmpty()) {
        d.link.close();
        return;
    }

    QTemporaryDir localDir;
    QTemporaryDir receivedDir;
    check(localDir.isValid() && receivedDir.isValid(), QStringLiteral("ローカルの一時フォルダを作る"));
    if (!localDir.isValid() || !receivedDir.isValid()) {
        d.link.close();
        return;
    }

    QByteArray data(kSize, '\0');
    for (int i = 0; i < kSize; ++i)
        data[i] = char((i * 37 + i / 1021 + i / 65536) & 0xFF);
    const QString name = QStringLiteral("!qt_large.bin");
    const QString source = localDir.filePath(name);
    check(writeFile(source, data), QStringLiteral("4 MB + 123 バイトの元のファイルを書く"));

    constexpr int kSmallSize = 64 * 1024;
    QByteArray small(kSmallSize, '\0');
    for (int i = 0; i < kSmallSize; ++i)
        small[i] = char((i * 13 + 7) & 0xFF);
    const QString smallName = QStringLiteral("!qt_small.bin");
    const QString smallSource = localDir.filePath(smallName);
    check(writeFile(smallSource, small), QStringLiteral("COPY / SPLIT / JOIN 用の 64 KB のファイルを書く"));
    if (g_fail) {
        d.link.close();
        return;
    }

    for (const QString &letter : targets) {
        const QString root = letter + QStringLiteral(":/");
        const QString remoteDir = root + QStringLiteral("!qt_selftest_large");
        const QString remoteFile = remoteDir + QLatin1Char('/') + name;

        // 前の回の残りを片付ける
        if (d.listHas(root, QStringLiteral("!qt_selftest_large")))
            d.removeTree(remoteDir);
        check(d.run([&] { d.proto.requestMkdir(remoteDir); }), QStringLiteral("%1: 試験用フォルダを作る").arg(letter));
        if (g_fail) {
            d.link.close();
            return;
        }
        d.link.close();

        bool upOk = false;
        bool upCanceled = false;
        QString upError;
        {
            ZmodemWorker worker(port, 921600, ZmodemWorker::Mode::Upload,
                                QStringList{ source, smallSource }, QString(), remoteDir);
            QObject::connect(&worker, &ZmodemWorker::logMessage,
                             [](const QString &message) { std::printf("    %s\n", qPrintable(message)); });
            QObject::connect(&worker, &ZmodemWorker::finished,
                             [&upOk, &upCanceled, &upError](bool ok, bool canceled, const QString &error) {
                                 upOk = ok;
                                 upCanceled = canceled;
                                 upError = error;
                             });
            worker.run();
        }
        check(upOk, QStringLiteral("%1: 4 MB 超のアップロード（分割→ZMODEM→JOIN→分割の削除）%2")
                        .arg(letter, upCanceled ? QStringLiteral("（中止）") : QString()));
        if (!upOk)
            std::printf("    upload error: %s\n", qPrintable(upError));

        waitMs(800);
        Device checkDevice;
        if (checkDevice.open(port)) {
            RemoteStat st;
            check(upOk && checkDevice.statPath(remoteFile, &st) && !st.isDir && st.size == quint64(kSize),
                  QStringLiteral("%1: 結合したファイルの大きさが %2 バイト").arg(letter).arg(kSize));
            if (upOk) {
                const QString smallRemote = remoteDir + QLatin1Char('/') + smallName;
                RemoteStat smallStat;
                check(checkDevice.statPath(smallRemote, &smallStat)
                          && !smallStat.isDir && smallStat.size == quint64(kSmallSize),
                      QStringLiteral("%1: 64 KB のファイルも送れている").arg(letter));

                // 2026-10-24 の COPY（リモート内の写し）。大きいファイルの同じ exFAT の中の写しは
                // 本体の制限（積み残し #48）で失敗することがあるので、ここでは小さいファイルで試す
                const QString copyPath = smallRemote + QStringLiteral(".copy");
                const bool copyOk = checkDevice.run([&] { checkDevice.proto.requestCopy(smallRemote, copyPath); }, 300000);
                const int copyErr = checkDevice.lastErr;
                const QString copyErrText = checkDevice.lastErrText;
                RemoteStat copyStat;
                check(copyOk && checkDevice.statPath(copyPath, &copyStat)
                          && !copyStat.isDir && copyStat.size == quint64(kSmallSize),
                      QStringLiteral("%1: COPY（リモート内）で同じ大きさのファイルができる").arg(letter));
                if (!copyOk)
                    std::printf("    copy error: %d %s\n", copyErr, qPrintable(copyErrText));
                if (copyOk) {
                    // 本体の SPLIT / JOIN（2026-10-24。64 KB を 4 MB の区切りで 1 つに分けて戻す）
                    const bool splitOk = checkDevice.run(
                        [&] { checkDevice.proto.requestSplit(copyPath, 4u * 1024u * 1024u); }, 300000);
                    const int splitErr = checkDevice.lastErr;
                    const QString splitErrText = checkDevice.lastErrText;
                    check(splitOk, QStringLiteral("%1: SPLIT（実機）").arg(letter));
                    if (!splitOk)
                        std::printf("    split error: %d %s\n", splitErr, qPrintable(splitErrText));
                    // SPLIT のあとの一覧の読み直しが終わってから JOIN する（実機の画面と同じ順番）
                    waitMs(800);
                    QStringList splitNames;
                    checkDevice.listNames(remoteDir, &splitNames);
                    std::printf("    split names: %s\n", qPrintable(splitNames.join(QStringLiteral(", "))));
                    const bool joinOk = checkDevice.run(
                        [&] { checkDevice.proto.requestJoin(copyPath + QStringLiteral(".001")); }, 300000);
                    const int joinErr = checkDevice.lastErr;
                    const QString joinErrText = checkDevice.lastErrText;
                    RemoteStat joinedStat;
                    check(joinOk && checkDevice.statPath(copyPath, &joinedStat)
                              && !joinedStat.isDir && joinedStat.size == quint64(kSmallSize),
                          QStringLiteral("%1: JOIN（実機）で元の大きさに戻る").arg(letter));
                    if (!joinOk)
                        std::printf("    join error: %d %s\n", joinErr, qPrintable(joinErrText));
                }
            }
            checkDevice.link.close();
        } else {
            check(false, QStringLiteral("%1: アップロードのあと実機につなぎ直す").arg(letter));
        }

        bool downOk = false;
        QString downError;
        if (upOk) {
            ZmodemWorker worker(port, 921600, ZmodemWorker::Mode::Download, QStringList(),
                                receivedDir.path(), remoteFile);
            QObject::connect(&worker, &ZmodemWorker::logMessage,
                             [](const QString &message) { std::printf("    %s\n", qPrintable(message)); });
            QObject::connect(&worker, &ZmodemWorker::finished,
                             [&downOk, &downError](bool ok, bool, const QString &error) {
                                 downOk = ok;
                                 downError = error;
                             });
            worker.run();
            check(downOk, QStringLiteral("%1: 4 MB 超のダウンロード（ストリーミング）").arg(letter));
            if (!downOk)
                std::printf("    download error: %s\n", qPrintable(downError));
            check(downOk && readFile(receivedDir.filePath(name)) == data,
                  QStringLiteral("%1: 受け取った中身が元と同じ").arg(letter));
        }

        waitMs(800);
        Device cleanup;
        if (cleanup.open(port)) {
            if (cleanup.listHas(root, QStringLiteral("!qt_selftest_large")))
                cleanup.removeTree(remoteDir);   // 途中で失敗して .001 が残っていても片付ける
            cleanup.link.close();
        }
    }
    d.link.close();
}

// 実機で 34 個のフォルダを 8 件ずつ消す（34 件以上でも RemoteDeleteQueue が詰まらないことの確認）
void deviceManyTests(const QString &port)
{
    constexpr int kCount = 34;
    Device d;
    check(d.open(port), QStringLiteral("実機と同期する（%1）").arg(port));
    if (g_fail) return;
    d.run([&] { d.proto.requestDrives(); });
    QString letters;
    for (const RemoteDrive &r : d.drives) letters += QStringLiteral("%1=%2 ").arg(r.letter).arg(r.type);
    std::printf("    drives: %s\n", qPrintable(letters));

    bool writableTested = false;
    for (int type : { 3, 4 }) {
        QString drv;
        int n = 0;
        for (const RemoteDrive &r : d.drives) if (r.type == type && r.ready) { drv = r.letter; ++n; }
        const QString label = type == 3 ? QStringLiteral("exFAT") : QStringLiteral("NTFS");
        if (n == 0) {
            std::printf("  -- %s: 準備できた区画が無いので飛ばす\n", qPrintable(label));
            continue;
        }
        writableTested = true;
        if (n > 1)
            std::printf("  -- %s: %d 個のうち %s を試す\n", qPrintable(label), n, qPrintable(drv));

        const QString root = drv + QStringLiteral(":/");
        const QString baseName = QStringLiteral("!qt_selftest_many");
        const QString base = root + baseName;
        std::printf("  -- %s %s\n", qPrintable(label), qPrintable(root));

        if (d.listHas(root, baseName)) {   // 前の回の残り
            check(d.removeTree(base), QStringLiteral("%1: 前の回の残りを片付ける").arg(label));
            d.waitIdle(5000);
        }

        if (!d.run([&] { d.proto.requestMkdir(base); })) {
            check(false, QStringLiteral("%1: 台のフォルダを作る（%2）").arg(label, d.lastErrText));
            continue;
        }

        QStringList paths;
        bool created = true;
        for (int i = 0; i < kCount; ++i) {
            const QString path = base + QStringLiteral("/d%1").arg(i, 2, 10, QLatin1Char('0'));
            if (!d.run([&] { d.proto.requestMkdir(path); })) { created = false; break; }
            paths.append(path);
        }
        check(created && paths.size() == kCount,
              QStringLiteral("%1: %2 個のフォルダを作る（%3 個）").arg(label).arg(kCount).arg(paths.size()));

        QStringList names;
        check(d.listNames(base, &names) && names.size() == kCount,
              QStringLiteral("%1: %2 個そろっている（%3 個）").arg(label).arg(kCount).arg(names.size()));
        if (!created || paths.size() != kCount) {
            d.removeTree(base);
            continue;
        }

        RemoteDeleteQueue q;
        QEventLoop loop;
        int ok = 0;
        int err = 0;
        int timeouts = 0;
        bool queueFinished = false;
        const auto c1 = QObject::connect(&q, &RemoteDeleteQueue::deleteRequested,
                                         [&d](const QString &path) { d.proto.requestDelete(path); });
        const auto c2 = QObject::connect(&d.proto, &RemoteProtocol::operationSucceeded,
                                         [&q, &ok](const QString &, const QString &) {
                                             ++ok;
                                             q.operationFinished();
                                         });
        const auto c3 = QObject::connect(&d.proto, &RemoteProtocol::requestFailed,
                                         [&q, &err](const QString &, int, const QString &) {
                                             ++err;
                                             q.operationFinished();
                                         });
        const auto c4 = QObject::connect(&d.proto, &RemoteProtocol::requestTimedOut,
                                         [&q, &timeouts, &loop](const QString &) {
                                             ++timeouts;
                                             q.abortRemaining();
                                             loop.quit();
                                         });
        const auto c5 = QObject::connect(&q, &RemoteDeleteQueue::finished, [&queueFinished, &loop] {
            queueFinished = true;
            loop.quit();
        });
        QTimer::singleShot(300000, &loop, &QEventLoop::quit);

        q.start(paths);
        loop.exec();
        QObject::disconnect(c1);
        QObject::disconnect(c2);
        QObject::disconnect(c3);
        QObject::disconnect(c4);
        QObject::disconnect(c5);

        check(queueFinished && ok == kCount && err == 0 && timeouts == 0,
              QStringLiteral("%1: %2 件を 8 件ずつ消せる（OK %3 / ERR %4 / タイムアウト %5）")
                  .arg(label).arg(kCount).arg(ok).arg(err).arg(timeouts));
        if (!queueFinished || err > 0 || timeouts > 0) {
            q.abortRemaining();
            d.waitIdle(5000);
        }

        names.clear();
        check(d.listNames(base, &names) && names.isEmpty(),
              QStringLiteral("%1: %2 件とも残っていない（%3 個）").arg(label).arg(kCount).arg(names.size()));
        check(d.run([&] { d.proto.requestDelete(base); }), QStringLiteral("%1: 台のフォルダを消す").arg(label));
    }
    check(writableTested, QStringLiteral("exFAT か NTFS の準備できた区画がある"));
    d.link.close();
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    offlineTests();
    systemDriveTests();
    latestFirmwareTests();
    deleteQueueTests();
    const int many = int(args.indexOf(QStringLiteral("--device-many")));
    if (many > 0 && many + 1 < args.size())
        deviceManyTests(args.at(many + 1));
    const int large = int(args.indexOf(QStringLiteral("--device-large")));
    if (large > 0 && large + 1 < args.size())
        deviceLargeTests(args.at(large + 1));
    const int at = int(args.indexOf(QStringLiteral("--device")));
    if (at > 0 && at + 1 < args.size())
        deviceTests(args.at(at + 1));
    std::printf("%s\n", g_fail ? qPrintable(QStringLiteral("selftest: %1 NG").arg(g_fail)) : "selftest: all ok");
    return g_fail ? 1 : 0;
}
