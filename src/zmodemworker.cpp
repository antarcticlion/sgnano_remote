#include "zmodemworker.h"

#include <QElapsedTimer>
#include <QThread>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace {

speed_t speedForBaud(int baud, bool *ok)
{
    *ok = true;
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default:
        *ok = false;
        return B921600;
    }
}

QString errnoText(int value)
{
    return QString::fromLocal8Bit(std::strerror(value));
}

}

// ブロックして読み書きするシリアルポート。ZMODEM は 1 バイトずつ待つので、
// SerialLink の QSocketNotifier ではなく、専用のスレッドで poll して待つ。
class ZmodemSerialIo : public ZmodemIo
{
public:
    ZmodemSerialIo(QString port, int baud, std::atomic_bool *canceled)
        : m_port(std::move(port))
        , m_baud(baud)
        , m_canceled(canceled)
    {
    }

    ~ZmodemSerialIo() override
    {
        close();
    }

    bool open(QString *error)
    {
        close();

        bool speedOk = false;
        const speed_t speed = speedForBaud(m_baud, &speedOk);
        if (!speedOk) {
            m_error = QStringLiteral("未対応のボーレートです: %1").arg(m_baud);
            if (error)
                *error = m_error;
            return false;
        }

        const QByteArray path = m_port.toLocal8Bit();
        const int fd = ::open(path.constData(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            m_error = QStringLiteral("%1 を開けません: %2").arg(m_port, errnoText(errno));
            if (error)
                *error = m_error;
            return false;
        }

        struct termios tio {};
        if (::tcgetattr(fd, &tio) != 0) {
            m_error = QStringLiteral("シリアル設定を読めません: %1").arg(errnoText(errno));
            ::close(fd);
            if (error)
                *error = m_error;
            return false;
        }

        ::cfmakeraw(&tio);
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cflag &= ~CRTSCTS;
        tio.c_cflag &= ~HUPCL;
        tio.c_iflag &= ~(IXON | IXOFF | IXANY);
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        ::cfsetispeed(&tio, speed);
        ::cfsetospeed(&tio, speed);

        if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
            m_error = QStringLiteral("シリアル設定を反映できません: %1").arg(errnoText(errno));
            ::close(fd);
            if (error)
                *error = m_error;
            return false;
        }

        ::tcflush(fd, TCIOFLUSH);
        m_fd = fd;
        m_rx.clear();
        m_error.clear();
        return true;
    }

    void close()
    {
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
        m_rx.clear();
    }

    void pushBack(const QByteArray &data)
    {
        if (!data.isEmpty())
            m_rx.prepend(data);
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
            if (m_canceled->load())
                return ZmodemIo::Cancel;
            if (!m_error.isEmpty())
                return ZmodemIo::Error;

            const qint64 elapsed = timer.elapsed();
            if (elapsed >= qint64(timeoutMs))
                return ZmodemIo::Timeout;

            int wait = int(std::min<qint64>(50, qint64(timeoutMs) - elapsed));
            if (wait <= 0)
                return ZmodemIo::Timeout;

            struct pollfd pfd {};
            pfd.fd = m_fd;
            pfd.events = POLLIN;
            const int ready = ::poll(&pfd, 1, wait);
            if (ready < 0) {
                if (errno == EINTR)
                    continue;
                fail(QStringLiteral("受信を待てません: %1").arg(errnoText(errno)));
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
        if (!m_error.isEmpty())
            return false;

        const char *p = data.constData();
        qsizetype left = data.size();
        QElapsedTimer timer;
        timer.start();

        while (left > 0) {
            const ssize_t n = ::write(m_fd, p, size_t(left));
            if (n > 0) {
                p += n;
                left -= n;
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (timer.elapsed() > 5000) {
                    fail(QStringLiteral("送信が止まりました"));
                    return false;
                }
                struct pollfd pfd {};
                pfd.fd = m_fd;
                pfd.events = POLLOUT;
                const int ready = ::poll(&pfd, 1, 50);
                if (ready < 0 && errno != EINTR) {
                    fail(QStringLiteral("送信を待てません: %1").arg(errnoText(errno)));
                    return false;
                }
                continue;
            }

            fail(QStringLiteral("送信に失敗しました: %1").arg(errnoText(errno)));
            return false;
        }
        return true;
    }

    bool flush() override
    {
        if (m_fd < 0)
            return false;
        if (::tcdrain(m_fd) != 0) {
            fail(QStringLiteral("送信を終えられません: %1").arg(errnoText(errno)));
            return false;
        }
        return true;
    }

    bool rxReady() override
    {
        if (!m_rx.isEmpty())
            return true;
        if (!m_error.isEmpty())
            return true;
        if (m_fd < 0)
            return false;

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
                fail(QStringLiteral("シリアルポートが閉じられました"));
                return false;
            }
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;
            fail(QStringLiteral("受信に失敗しました: %1").arg(errnoText(errno)));
            return false;
        }
    }

    void fail(const QString &message)
    {
        if (m_error.isEmpty())
            m_error = message;
    }

    QString m_port;
    int m_baud = 921600;
    std::atomic_bool *m_canceled = nullptr;
    int m_fd = -1;
    QByteArray m_rx;
    QString m_error;
};

ZmodemWorker::ZmodemWorker(QString port, int baud, Mode mode, QStringList files,
                           QString localDir, QString remotePath, QObject *parent)
    : QObject(parent)
    , m_port(std::move(port))
    , m_baud(baud)
    , m_mode(mode)
    , m_files(std::move(files))
    , m_localDir(std::move(localDir))
    , m_remotePath(std::move(remotePath))
{
}

void ZmodemWorker::cancel()
{
    m_canceled.store(true);
}

bool ZmodemWorker::waitForRemote(ZmodemIo &io, QString *error)
{
    if (m_remotePath.isEmpty() || m_remotePath.contains(QLatin1Char('\n'))
        || m_remotePath.contains(QLatin1Char('\r')) || m_remotePath.contains(QLatin1Char('\t'))) {
        *error = QStringLiteral("リモートのパスが正しくありません");
        return false;
    }

    const bool download = m_mode == Mode::Download;
    QByteArray command;
    command.append('\x1e');
    command.append(download ? QByteArrayLiteral("ZGET ") : QByteArrayLiteral("ZPUT "));
    command.append(m_remotePath.toUtf8());
    command.append('\n');

    if (!io.put(command) || !io.flush()) {
        *error = io.errorString().isEmpty()
            ? QStringLiteral("sgnano に ZMODEM を頼めませんでした")
            : io.errorString();
        return false;
    }

    QByteArray pending;
    QString lastError;
    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < 15000) {
        if (m_canceled.load()) {
            *error = QStringLiteral("転送を中止しました");
            return false;
        }

        const int c = io.getc(100);
        if (c == ZmodemIo::Cancel) {
            *error = QStringLiteral("転送を中止しました");
            return false;
        }
        if (c == ZmodemIo::Error) {
            *error = io.errorString().isEmpty()
                ? QStringLiteral("シリアルポートの読み書きに失敗しました")
                : io.errorString();
            return false;
        }
        if (c >= 0)
            pending.append(char(c));
        if (pending.size() > 65536)
            pending.remove(0, pending.size() - 4096);

        const int ready = pending.indexOf("@SG\tZMODEM\tREADY");
        if (ready >= 0) {
            const int newline = pending.indexOf('\n', ready);
            if (newline >= 0) {
                io.pushBack(pending.mid(newline + 1));
                return true;
            }
            continue;
        }

        const int zmodem = pending.indexOf(QByteArray("**\x18" "B"));
        if (zmodem >= 0) {
            io.pushBack(pending.mid(zmodem));
            return true;
        }

        const int rejected = pending.indexOf("@SG\tERR");
        if (rejected >= 0) {
            const int newline = pending.indexOf('\n', rejected);
            if (newline < 0)
                continue;

            const QByteArray line = pending.mid(rejected, newline - rejected);
            const QList<QByteArray> fields = line.split('\t');
            const int code = fields.size() > 2 ? fields.at(2).toInt() : 0;
            QString why = QString::fromUtf8(line);
            if (fields.size() > 3)
                why = QString::fromUtf8(fields.at(3));
            lastError = QStringLiteral("sgnano が断りました: %1").arg(why);

            if (code == 13) {                 // ドライブの準備がまだ。少し待って送り直す
                pending.clear();
                QThread::msleep(500);
                if (m_canceled.load()) {
                    *error = QStringLiteral("転送を中止しました");
                    return false;
                }
                if (!io.put(command) || !io.flush()) {
                    *error = io.errorString().isEmpty()
                        ? QStringLiteral("sgnano に ZMODEM を頼めませんでした")
                        : io.errorString();
                    return false;
                }
                continue;
            }

            *error = lastError;
            return false;
        }
    }

    *error = lastError.isEmpty()
        ? QStringLiteral("sgnano が ZMODEM を始めませんでした")
        : lastError;
    return false;
}

void ZmodemWorker::run()
{
    ZmodemSerialIo io(m_port, m_baud, &m_canceled);
    QString error;
    if (!io.open(&error)) {
        emit finished(false, false, error);
        return;
    }

    if (!waitForRemote(io, &error)) {
        const bool canceled = m_canceled.load();
        emit finished(false, canceled, error);
        return;
    }

    emit logMessage(m_mode == Mode::Download
                        ? tr("内蔵 ZMODEM で受け取ります: %1").arg(m_remotePath)
                        : tr("内蔵 ZMODEM で送ります: %1").arg(m_files.join(QStringLiteral(", "))));

    Zmodem zmodem(&io, [this](qint64 done, qint64 total) {
        emit progress(done, total);
    });

    bool ok = false;
    if (m_mode == Mode::Upload) {
        ok = zmodem.sendFiles(m_files, &error);
    } else {
        QStringList received;
        ok = zmodem.receiveFiles(m_localDir, &received, &error);
        for (const QString &path : std::as_const(received))
            emit logMessage(tr("受け取りました: %1").arg(path));
    }

    const bool canceled = m_canceled.load();
    emit finished(ok, canceled, error);
}
