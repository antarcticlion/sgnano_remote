// 内蔵 ZMODEM（src/zmodem.cpp）の実機なしの試し（2026-10-24）。
// socketpair の両端で送る側と受け取る側を別のスレッドで走らせ、中身・名前・空のファイル・
// 逃がす文字（ZDLE / XON / XOFF / CR / 0x7F / 0xFF）・300KB のファイルを確かめる。
#include "zmodem.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

int failures = 0;

void check(const QString &name, bool ok, const QString &extra = QString())
{
    std::printf("%s  %s%s\n", ok ? "OK" : "NG", qPrintable(name),
                extra.isEmpty() ? "" : qPrintable(QStringLiteral(" (") + extra + QLatin1Char(')')));
    if (!ok)
        ++failures;
}

QString errnoText(int value)
{
    return QString::fromLocal8Bit(std::strerror(value));
}

class SocketIo : public ZmodemIo
{
public:
    explicit SocketIo(int fd)
        : m_fd(fd)
    {
    }

    ~SocketIo() override
    {
        if (m_fd >= 0)
            ::close(m_fd);
    }

    int getc(unsigned timeoutMs) override
    {
        if (!m_rx.isEmpty())
            return takeByte();
        if (!m_error.isEmpty())
            return ZmodemIo::Error;

        QElapsedTimer timer;
        timer.start();
        for (;;) {
            const qint64 elapsed = timer.elapsed();
            if (elapsed >= qint64(timeoutMs))
                return ZmodemIo::Timeout;

            struct pollfd pfd {};
            pfd.fd = m_fd;
            pfd.events = POLLIN;
            const int wait = int(std::min<qint64>(50, qint64(timeoutMs) - elapsed));
            const int ready = ::poll(&pfd, 1, wait);
            if (ready < 0) {
                if (errno == EINTR)
                    continue;
                m_error = errnoText(errno);
                return ZmodemIo::Error;
            }
            if (ready == 0)
                continue;
            if (!readMore())
                return ZmodemIo::Error;
            if (!m_rx.isEmpty())
                return takeByte();
        }
    }

    bool put(const QByteArray &data) override
    {
        const char *p = data.constData();
        qsizetype left = data.size();
        while (left > 0) {
            const ssize_t n = ::write(m_fd, p, size_t(left));
            if (n > 0) {
                p += n;
                left -= n;
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            m_error = errnoText(errno);
            return false;
        }
        return true;
    }

    bool flush() override
    {
        return true;
    }

    bool rxReady() override
    {
        if (!m_rx.isEmpty())
            return true;
        if (!m_error.isEmpty())
            return true;

        struct pollfd pfd {};
        pfd.fd = m_fd;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, 0);
        if (ready <= 0)
            return false;
        readMore();
        return !m_rx.isEmpty() || !m_error.isEmpty();
    }

    QString errorString() const override
    {
        return m_error;
    }

    void pushBack(const QByteArray &data) override
    {
        m_rx.prepend(data);
    }

private:
    int takeByte()
    {
        const int c = quint8(m_rx.at(0));
        m_rx.remove(0, 1);
        return c;
    }

    bool readMore()
    {
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(m_fd, buf, sizeof buf);
            if (n > 0) {
                m_rx.append(buf, int(n));
                return true;
            }
            if (n == 0) {
                m_error = QStringLiteral("相手が切れました");
                return false;
            }
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;
            m_error = errnoText(errno);
            return false;
        }
    }

    int m_fd = -1;
    QByteArray m_rx;
    QString m_error;
};

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

void roundTripTest()
{
    QTemporaryDir sourceDir;
    QTemporaryDir targetDir;
    check(QStringLiteral("一時フォルダを作る"), sourceDir.isValid() && targetDir.isValid());
    if (!sourceDir.isValid() || !targetDir.isValid())
        return;

    QByteArray escapes;
    const char special[] = {
        char(0x18), char(0x11), char(0x13), char(0x91), char(0x93), char(0x10),
        char(0x90), char(0x0D), char(0x8D), char(0x7F), char(0xFF), '*'
    };
    for (int i = 0; i < 1000; ++i)
        escapes.append(special, int(sizeof special));

    QByteArray big;
    big.resize(300000);
    for (int i = 0; i < big.size(); ++i)
        big[i] = char((i * 37 + i / 251) & 0xFF);

    const QString smallName = QStringLiteral("small name.txt");
    const QString escapeName = QStringLiteral("escape.bin");
    const QString bigName = QStringLiteral("big.bin");
    const QString emptyName = QStringLiteral("empty.txt");
    const QByteArray small = QByteArrayLiteral("hello, sgnano\n");

    const QStringList names{ smallName, escapeName, bigName, emptyName };
    const QList<QByteArray> contents{ small, escapes, big, QByteArray() };
    QStringList paths;
    for (int i = 0; i < names.size(); ++i) {
        const QString path = sourceDir.filePath(names.at(i));
        check(QStringLiteral("元のファイルを書く: %1").arg(names.at(i)),
              writeFile(path, contents.at(i)));
        paths.append(path);
    }

    int fds[2] = { -1, -1 };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        check(QStringLiteral("socketpair を作る"), false, errnoText(errno));
        return;
    }

    SocketIo senderIo(fds[0]);
    SocketIo receiverIo(fds[1]);

    bool sendOk = false;
    bool receiveOk = false;
    QString sendError;
    QString receiveError;
    QStringList received;

    QThread *receiver = QThread::create([&] {
        Zmodem zmodem(&receiverIo);
        receiveOk = zmodem.receiveFiles(targetDir.path(), &received, &receiveError);
    });
    QThread *sender = QThread::create([&] {
        Zmodem zmodem(&senderIo);
        sendOk = zmodem.sendFiles(paths, &sendError);
    });

    receiver->start();
    sender->start();
    sender->wait();
    receiver->wait();
    delete sender;
    delete receiver;

    check(QStringLiteral("300KB・複数・空のファイルを送る"), sendOk, sendError);
    check(QStringLiteral("300KB・複数・空のファイルを受け取る"), receiveOk, receiveError);
    check(QStringLiteral("受け取ったファイルの数"), received.size() == names.size(),
          QStringLiteral("%1 / %2").arg(received.size()).arg(names.size()));

    for (int i = 0; i < names.size(); ++i) {
        const QString path = targetDir.filePath(names.at(i));
        check(QStringLiteral("中身が同じ: %1").arg(names.at(i)),
              QFileInfo::exists(path) && readFile(path) == contents.at(i));
    }
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    roundTripTest();
    std::printf("zmodem selftest: %s\n", failures == 0 ? "all ok" : "failed");
    return failures == 0 ? 0 : 1;
}
