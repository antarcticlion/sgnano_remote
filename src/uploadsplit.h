#pragma once

#include <QString>
#include <QStringList>

// sgnano 本体（src/filer/fzm.c、2026-11-14）の ZMODEM は 1 ファイル 4 MB までしか受け取らない。
// 4 MB を越えるファイルは PC で name.001 .. に分けて送り、本体の JOIN でつなぐ（scripts/zm.py と同じ形）。
// 分ける大きさは SGNANO_ZM_PART で替えられる（既定 2 MB。PSRAM の連続した空きが細いときは 1048576 など）。
namespace UploadSplit {

constexpr quint32 kDeviceFileMax = 4u * 1024u * 1024u;
constexpr quint32 kDefaultPartSize = 2u * 1024u * 1024u;   // 環境変数が無いときの保険（実際は partSizeForFile が選ぶ）
constexpr quint32 kSmallPartSize = 1u * 1024u * 1024u;   // exFAT・NTFS で PSRAM が細いときも通りやすい
constexpr quint32 kLargePartSize = 2u * 1024u * 1024u;   // 64 MB を越えるファイルを 64 個以内に収める
constexpr int kMaxParts = 64;             // 本体の JOIN の上限（src/filer/fsplit.c の SJ_MAX_FILES）
constexpr quint64 kSmallFileMax = quint64(kSmallPartSize) * quint64(kMaxParts);   // 64 MB
constexpr int kMaxNameBytes = 131;        // 本体が受け取る名前の大きさ（src/filer/fzm.c の char name[132]）

quint32 partSizeFromEnvironment();
// configured = false のとき、64 MB 以下は 1 MB、64 MB 超は 2 MB（JOIN の 64 個上限に収める）
quint32 partSizeForFile(quint64 fileSize, bool configured, quint32 configuredSize);

// path を outDir の下へ name.001 .. に分ける。partSize は 512 の倍数（512..4 MB）。
// 分けたファイルのパスを parts に入れる（作れなければ false と error）。
bool splitFile(const QString &path, const QString &outDir, quint32 partSize,
               QStringList *parts, QString *error);

}
