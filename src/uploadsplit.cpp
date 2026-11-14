#include "uploadsplit.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace {

bool validPartSize(quint32 partSize)
{
    return partSize >= 512u && partSize <= UploadSplit::kDeviceFileMax && partSize % 512u == 0;
}

void removeParts(const QStringList &parts)
{
    for (const QString &part : parts)
        QFile::remove(part);
}

}

namespace UploadSplit {

quint32 partSizeFromEnvironment()
{
    bool ok = false;
    const qint64 value = qEnvironmentVariableIntValue("SGNANO_ZM_PART", &ok);
    if (ok && value >= 512 && value <= qint64(kDeviceFileMax) && value % 512 == 0)
        return quint32(value);
    return kDefaultPartSize;
}

quint32 partSizeForFile(quint64 fileSize, bool configured, quint32 configuredSize)
{
    if (configured)
        return configuredSize;
    return fileSize <= kSmallFileMax ? kSmallPartSize : kLargePartSize;
}

bool splitFile(const QString &path, const QString &outDir, quint32 partSize,
               QStringList *parts, QString *error)
{
    if (parts)
        parts->clear();
    if (!validPartSize(partSize)) {
        if (error)
            *error = QStringLiteral("分ける大きさは 512 の倍数（512..4 MB）で指定してください");
        return false;
    }

    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable()) {
        if (error)
            *error = QStringLiteral("ファイルを読めません: %1").arg(path);
        return false;
    }

    const QString baseName = info.fileName();
    if (baseName.isEmpty() || baseName.toUtf8().size() + 4 > kMaxNameBytes) {
        if (error)
            *error = QStringLiteral("sgnano が受け取れる名前は %1 バイトまでです（.001 を足す分を含む）: %2")
                         .arg(kMaxNameBytes)
                         .arg(baseName);
        return false;
    }

    const quint64 total = quint64(info.size());
    const quint64 count = (total + partSize - 1u) / partSize;
    if (count == 0) {
        if (error)
            *error = QStringLiteral("空のファイルは分けられません: %1").arg(path);
        return false;
    }
    if (count > quint64(kMaxParts)) {
        if (error)
            *error = QStringLiteral("%1 は %2 個に分かれますが、sgnano が結合できるのは %3 個までです")
                         .arg(baseName)
                         .arg(count)
                         .arg(kMaxParts);
        return false;
    }

    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("%1 を読めません: %2").arg(path, input.errorString());
        return false;
    }

    QDir dir(outDir);
    QStringList made;
    for (quint64 k = 1; k <= count; ++k) {
        const QString partPath = dir.filePath(QStringLiteral("%1.%2")
                                                  .arg(baseName)
                                                  .arg(k, 3, 10, QLatin1Char('0')));
        QFile part(partPath);
        if (!part.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error)
                *error = QStringLiteral("%1 を書けません: %2").arg(partPath, part.errorString());
            removeParts(made);
            return false;
        }

        quint64 left = qMin<quint64>(partSize, total - (k - 1u) * partSize);
        while (left > 0) {
            const QByteArray chunk = input.read(qint64(qMin<quint64>(left, 1024u * 1024u)));
            if (chunk.isEmpty()) {
                if (error)
                    *error = QStringLiteral("%1 を読んでいる途中で終わりました").arg(path);
                part.close();
                removeParts(made + QStringList{ partPath });
                return false;
            }
            if (part.write(chunk) != chunk.size()) {
                if (error)
                    *error = QStringLiteral("%1 を書けません: %2").arg(partPath, part.errorString());
                part.close();
                removeParts(made + QStringList{ partPath });
                return false;
            }
            left -= quint64(chunk.size());
        }
        part.close();
        made.append(partPath);
    }

    if (parts)
        *parts = made;
    return true;
}

}
