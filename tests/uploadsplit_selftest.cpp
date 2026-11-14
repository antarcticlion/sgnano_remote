// 4 MB を越えるファイルを PC で name.001 .. に分ける部分（src/uploadsplit.cpp）の実機なしの試し（2026-11-14）。
// sgnano 本体の ZMODEM は 1 ファイル 4 MB まで、JOIN は 64 個までなので、その境目を確かめる。
#include "uploadsplit.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const QString &what)
{
    std::printf("%s  %s\n", ok ? "OK" : "NG", qPrintable(what));
    if (!ok)
        ++failures;
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

void environmentTests()
{
    qunsetenv("SGNANO_ZM_PART");
    check(UploadSplit::partSizeFromEnvironment() == UploadSplit::kDefaultPartSize,
          QStringLiteral("SGNANO_ZM_PART が無ければ 2 MB"));

    qputenv("SGNANO_ZM_PART", "1048576");
    check(UploadSplit::partSizeFromEnvironment() == 1024u * 1024u,
          QStringLiteral("SGNANO_ZM_PART=1048576 を使う"));

    qputenv("SGNANO_ZM_PART", "1000");
    check(UploadSplit::partSizeFromEnvironment() == UploadSplit::kDefaultPartSize,
          QStringLiteral("512 の倍数でない SGNANO_ZM_PART は既定へ"));

    qputenv("SGNANO_ZM_PART", "8388608");
    check(UploadSplit::partSizeFromEnvironment() == UploadSplit::kDefaultPartSize,
          QStringLiteral("4 MB を越える SGNANO_ZM_PART は既定へ"));
    qunsetenv("SGNANO_ZM_PART");
}

void partSizeTests()
{
    check(UploadSplit::partSizeForFile(4u * 1024u * 1024u, false, 0) == UploadSplit::kSmallPartSize,
          QStringLiteral("環境変数が無ければ 4 MB は 1 MB ずつ"));
    check(UploadSplit::partSizeForFile(UploadSplit::kSmallFileMax, false, 0) == UploadSplit::kSmallPartSize,
          QStringLiteral("64 MB ちょうどは 1 MB ずつ（64 個）"));
    check(UploadSplit::partSizeForFile(UploadSplit::kSmallFileMax + 1u, false, 0) == UploadSplit::kLargePartSize,
          QStringLiteral("64 MB を越えたら 2 MB ずつ"));
    check(UploadSplit::partSizeForFile(4u * 1024u * 1024u, true, UploadSplit::kLargePartSize) == UploadSplit::kLargePartSize,
          QStringLiteral("環境変数の指定はそのまま使う"));
}

void splitTests()
{
    QTemporaryDir sourceDir;
    QTemporaryDir targetDir;
    check(sourceDir.isValid() && targetDir.isValid(), QStringLiteral("一時フォルダを作る"));
    if (!sourceDir.isValid() || !targetDir.isValid())
        return;

    // 2 MB ずつに分けると 3 つ（2 MB + 2 MB + 1 MB + 123 バイト）
    const int size = 5 * 1024 * 1024 + 123;
    QByteArray data(size, '\0');
    for (int i = 0; i < size; ++i)
        data[i] = char((i * 29 + i / 1021) & 0xFF);
    const QString big = sourceDir.filePath(QStringLiteral("big.bin"));
    check(writeFile(big, data), QStringLiteral("5 MB の元のファイルを書く"));

    QStringList parts;
    QString error;
    const quint32 partSize = 2u * 1024u * 1024u;
    const bool splitOk = UploadSplit::splitFile(big, targetDir.path(), partSize, &parts, &error);
    check(splitOk, QStringLiteral("5 MB を 2 MB ずつに分ける: %1").arg(error));
    check(parts.size() == 3, QStringLiteral("3 つに分かれる（%1）").arg(parts.size()));
    if (parts.size() == 3) {
        check(parts.at(0).endsWith(QStringLiteral("big.bin.001")), QStringLiteral("1 つ目の名前は .001"));
        check(parts.at(1).endsWith(QStringLiteral("big.bin.002")), QStringLiteral("2 つ目の名前は .002"));
        check(parts.at(2).endsWith(QStringLiteral("big.bin.003")), QStringLiteral("3 つ目の名前は .003"));
        check(QFile(big).size() == size, QStringLiteral("元のファイルはそのまま"));
        check(QFile(parts.at(0)).size() == qint64(partSize), QStringLiteral("1 つ目は 2 MB"));
        check(QFile(parts.at(1)).size() == qint64(partSize), QStringLiteral("2 つ目は 2 MB"));
        check(QFile(parts.at(2)).size() == qint64(size - 2 * qint64(partSize)),
              QStringLiteral("3 つ目は残り"));

        QByteArray joined;
        for (const QString &part : parts)
            joined += readFile(part);
        check(joined == data, QStringLiteral("つなぐと元と同じ"));
    }

    // 65 個になる分け方は、本体の JOIN の上限（64）を越えるので断る
    QTemporaryDir tooManyDir;
    const QString many = sourceDir.filePath(QStringLiteral("many.bin"));
    check(writeFile(many, QByteArray(65 * 512, 'x')), QStringLiteral("65 個になるファイルを書く"));
    QStringList manyParts;
    QString manyError;
    const bool manyRefused = !UploadSplit::splitFile(many, tooManyDir.path(), 512, &manyParts, &manyError);
    check(manyRefused && manyError.contains(QStringLiteral("64")),
          QStringLiteral("65 個に分かれるファイルは断る（%1）").arg(manyError));
    check(QDir(tooManyDir.path()).entryList(QDir::Files).isEmpty(), QStringLiteral("断ったときは何も作らない"));

    // 名前が長すぎる（.001 を足すと 131 バイトを越える）ものは断る
    QTemporaryDir longNameDir;
    const QString longName(128, QLatin1Char('n'));
    const QString longPath = sourceDir.filePath(longName);
    check(writeFile(longPath, QByteArray(1024, 'y')), QStringLiteral("長い名前のファイルを書く"));
    QStringList longParts;
    QString longError;
    check(!UploadSplit::splitFile(longPath, longNameDir.path(), 512, &longParts, &longError)
              && longError.contains(QStringLiteral("131")),
          QStringLiteral("131 バイトを越える名前は断る"));

    // 空のファイル・512 の倍数でない大きさも断る
    const QString empty = sourceDir.filePath(QStringLiteral("empty.bin"));
    check(writeFile(empty, QByteArray()), QStringLiteral("空のファイルを書く"));
    QStringList emptyParts;
    QString emptyError;
    check(!UploadSplit::splitFile(empty, targetDir.path(), 512, &emptyParts, &emptyError),
          QStringLiteral("空のファイルは分けない"));
    QStringList badParts;
    QString badError;
    check(!UploadSplit::splitFile(big, targetDir.path(), 1000, &badParts, &badError),
          QStringLiteral("512 の倍数でない大きさは断る"));
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    environmentTests();
    partSizeTests();
    splitTests();
    std::printf("uploadsplit selftest: %s\n", failures == 0 ? "all ok" : "failed");
    return failures == 0 ? 0 : 1;
}
