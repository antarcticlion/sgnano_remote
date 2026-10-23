#include "remotefs.h"

namespace {

const QStringList &systemFolders()
{
    static const QStringList folders = {
        QStringLiteral("SystemSettings"),
        QStringLiteral("SystemFiles"),
        QStringLiteral("SystemLogs"),
    };
    return folders;
}

}

namespace RemoteFs {

QStringList protectedSystemFolders()
{
    return systemFolders();
}

bool isSystemDrivePath(const QString &path)
{
    return path.size() >= 2
        && (path.at(0) == QLatin1Char(kSystemDriveLetter) || path.at(0) == QLatin1Char('s'))
        && path.at(1) == QLatin1Char(':');
}

bool isProtectedSystemPath(const QString &path)
{
    if (!isSystemDrivePath(path))
        return false;

    // S: のルート直下だけを見る。中のファイル・フォルダは本体と同じく守らない。
    int begin = 2;
    while (begin < path.size()
           && (path.at(begin) == QLatin1Char('/') || path.at(begin) == QLatin1Char('\\'))) {
        ++begin;
    }
    int end = path.size();
    while (end > begin
           && (path.at(end - 1) == QLatin1Char('/') || path.at(end - 1) == QLatin1Char('\\'))) {
        --end;
    }
    if (begin == end)
        return false;

    const QString name = path.mid(begin, end - begin);
    for (const QString &folder : systemFolders()) {
        if (folder.compare(name, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

}
