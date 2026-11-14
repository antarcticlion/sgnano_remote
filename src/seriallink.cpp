#include "seriallink.h"

#ifdef Q_OS_WIN
#include "win32serial.h"

#include <QTimer>
#else
#include <QSocketNotifier>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

#ifndef Q_OS_WIN
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
#endif

SerialLink::SerialLink(QObject *parent)
    : QObject(parent)
{
}

SerialLink::~SerialLink()
{
    close();
}

bool SerialLink::open(const QString &port, int baud, QString *error)
{
    close();

#ifdef Q_OS_WIN
    if (port.isEmpty() || baud <= 0) {
        const QString message = tr("シリアルポートまたはボーレートが正しくありません");
        if (error)
            *error = message;
        emit errorOccurred(message);
        return false;
    }

    auto serial = std::make_unique<Win32SerialPort>();
    QString message;
    if (!serial->open(port, baud, &message)) {
        if (error)
            *error = message;
        emit errorOccurred(message);
        return false;
    }

    {
        QMutexLocker locker(&m_ioMutex);
        m_winPort = std::move(serial);
        m_port = port;
        m_baud = baud;
        m_tx.clear();
    }

    // Windows は読み取りを短い時間切れ付きで QTimer から拾う。ブロックする読み取りの
    // スレッドを閉じるときの待ちで固まらないようにするため（2026-11-15）。
    m_winTimer = new QTimer(this);
    m_winTimer->setInterval(5);
    connect(m_winTimer, &QTimer::timeout, this, &SerialLink::readReady);
    m_winTimer->start();

    emit opened(m_port, m_baud);
    return true;
#else
    bool baudOk = false;
    const speed_t speed = speedForBaud(baud, &baudOk);
    if (!baudOk) {
        const QString message = tr("未対応のボーレートです: %1").arg(baud);
        if (error)
            *error = message;
        emit errorOccurred(message);
        return false;
    }

    const QByteArray path = port.toLocal8Bit();
    const int fd = ::open(path.constData(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        const QString message = tr("%1 を開けません: %2").arg(port, errnoText(errno));
        if (error)
            *error = message;
        emit errorOccurred(message);
        return false;
    }

    struct termios tio {};
    if (::tcgetattr(fd, &tio) != 0) {
        const QString message = tr("シリアル設定を読めません: %1").arg(errnoText(errno));
        ::close(fd);
        if (error)
            *error = message;
        emit errorOccurred(message);
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
        const QString message = tr("シリアル設定を反映できません: %1").arg(errnoText(errno));
        ::close(fd);
        if (error)
            *error = message;
        emit errorOccurred(message);
        return false;
    }

    ::tcflush(fd, TCIOFLUSH);

    m_fd = fd;
    m_port = port;
    m_baud = baud;
    m_tx.clear();

    m_readNotifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    m_writeNotifier = new QSocketNotifier(m_fd, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &SerialLink::readReady);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &SerialLink::writeReady);

    emit opened(m_port, m_baud);
    return true;
#endif
}

void SerialLink::close()
{
    const bool wasOpen = isOpen();

#ifdef Q_OS_WIN
    if (m_winTimer) {
        m_winTimer->stop();
        delete m_winTimer;
        m_winTimer = nullptr;
    }
    {
        QMutexLocker locker(&m_ioMutex);
        if (m_winPort) {
            m_winPort->close();
            m_winPort.reset();
        }
        m_port.clear();
        m_tx.clear();
    }
#else
    if (m_readNotifier) {
        m_readNotifier->setEnabled(false);
        m_readNotifier->deleteLater();
        m_readNotifier = nullptr;
    }
    if (m_writeNotifier) {
        m_writeNotifier->setEnabled(false);
        m_writeNotifier->deleteLater();
        m_writeNotifier = nullptr;
    }

    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }

    m_port.clear();
    m_tx.clear();
#endif

    if (wasOpen)
        emit closed();
}

bool SerialLink::isOpen() const
{
#ifdef Q_OS_WIN
    QMutexLocker locker(&m_ioMutex);
    return m_winPort && m_winPort->isOpen();
#else
    return m_fd >= 0;
#endif
}

QString SerialLink::portName() const
{
    return m_port;
}

int SerialLink::baudRate() const
{
    return m_baud;
}

qint64 SerialLink::bytesToWrite() const
{
#ifdef Q_OS_WIN
    return 0;
#else
    return m_tx.size();
#endif
}

qint64 SerialLink::write(const QByteArray &data)
{
    if (!isOpen()) {
        emit errorOccurred(tr("シリアルポートが開かれていません"));
        return -1;
    }
    if (data.isEmpty())
        return 0;

#ifdef Q_OS_WIN
    QString message;
    int written = -1;
    {
        QMutexLocker locker(&m_ioMutex);
        if (m_winPort)
            written = m_winPort->write(data.constData(), int(data.size()), 5000, &message);
    }
    if (written != data.size()) {
        fail(message.isEmpty() ? tr("送信に失敗しました") : message);
        return -1;
    }
    return written;
#else
    m_tx.append(data);
    flushTx();
    return isOpen() ? data.size() : -1;
#endif
}

void SerialLink::readReady()
{
#ifdef Q_OS_WIN
    if (!isOpen() || !m_winPort)
        return;

    QByteArray buffer;
    for (;;) {
        char chunk[4096];
        QString message;
        const int n = m_winPort->read(chunk, int(sizeof chunk), 0, &message);
        if (n > 0) {
            buffer.append(chunk, n);
            continue;
        }
        if (n < 0) {
            fail(tr("受信に失敗しました: %1").arg(message));
            return;
        }
        break;
    }
    if (!buffer.isEmpty())
        emit dataReceived(buffer);
#else
    if (!isOpen())
        return;

    QByteArray buffer;
    for (;;) {
        char chunk[4096];
        const ssize_t n = ::read(m_fd, chunk, sizeof chunk);
        if (n > 0) {
            buffer.append(chunk, int(n));
            continue;
        }
        if (n == 0)
            break;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        fail(tr("受信に失敗しました: %1").arg(errnoText(errno)));
        return;
    }

    if (!buffer.isEmpty())
        emit dataReceived(buffer);
#endif
}

void SerialLink::writeReady()
{
#ifdef Q_OS_WIN
    // Windows の write は待って書く
#else
    flushTx();
#endif
}

void SerialLink::flushTx()
{
#ifndef Q_OS_WIN
    if (!isOpen())
        return;

    while (!m_tx.isEmpty()) {
        const ssize_t n = ::write(m_fd, m_tx.constData(), size_t(m_tx.size()));
        if (n > 0) {
            m_tx.remove(0, int(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
            if (m_writeNotifier)
                m_writeNotifier->setEnabled(true);
            return;
        }
        fail(tr("送信に失敗しました: %1").arg(errnoText(errno)));
        return;
    }

    if (m_writeNotifier)
        m_writeNotifier->setEnabled(false);
#endif
}

void SerialLink::fail(const QString &message)
{
    emit errorOccurred(message);
    close();
}
