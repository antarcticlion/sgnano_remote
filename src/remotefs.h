#pragma once

#include <QString>
#include <QStringList>

// sgnano 本体の仕様（2026-10-24）: ドライブ S: は内蔵フラッシュ（2 MB、FAT12）。
// そのルートの SystemSettings・SystemFiles・SystemLogs はシステムのフォルダで、消す・名前を変えることはできない。
// 本体（src/storage/sysdisk.c の sysdisk_protected）と同じ見分け方をここにまとめ、画面と試しから使う。
namespace RemoteFs {

constexpr char kSystemDriveLetter = 'S';

QStringList protectedSystemFolders();

bool isSystemDrivePath(const QString &path);
bool isProtectedSystemPath(const QString &path);

}
