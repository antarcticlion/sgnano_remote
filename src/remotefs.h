#pragma once

#include <QString>
#include <QStringList>

// sgnano 本体の仕様（2026-10-24、2026-11-02 に 4 MB へ）: ドライブ S: は内蔵フラッシュ（4 MB、FAT12）。
// そのルートの SystemSettings・SystemFiles・SystemLogs はシステムのフォルダで、消す・名前を変えることはできない。
// 本体（src/storage/sysdisk.c の sysdisk_protected）と同じ見分け方をここにまとめ、画面と試しから使う。
// 2026-11-02 にルートへ bin・sbin が増えたが、本体はこの 2 つを守らない（消せる）。
namespace RemoteFs {

constexpr char kSystemDriveLetter = 'S';
constexpr quint32 kSystemDriveBytes = 4u * 1024u * 1024u;

QStringList protectedSystemFolders();

bool isSystemDrivePath(const QString &path);
bool isProtectedSystemPath(const QString &path);

// 本体のドライブの種類（FDRV_*）: 書き込みできる FAT・FDD・exFAT・NTFS か（COPY / SPLIT / JOIN / ZPUT の前の見分け）
bool isWritableDriveType(int type);

}
