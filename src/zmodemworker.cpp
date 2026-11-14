#include "zmodemworker.h"

#include "uploadsplit.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocale>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <cstring>
#include <utility>

#ifdef Q_OS_WIN
#include "win32serial.h"
#else
#include <cerrno>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

// sgnano の uartcon.h（2026-11-14）の 1 行の上限（頭の 0x1E と改行を除く）
constexpr int kMaxRemoteCommandBytes = 320;

#ifndef Q_OS_WIN
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
#endif

QString joinRemotePath(const QString &base, const QString &name)
{
    if (base.isEmpty())
        return name;
    if (base.endsWith(QLatin1Char('/')))
        return base + name;
    return base + QLatin1Char('/') + name;
}

QString partName(const QString &baseName, int number)
{
    return QStringLiteral("%1.%2").arg(baseName).arg(number, 3, 10, QLatin1Char('0'));
}

}

#ifndef Q_OS_WIN
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
#else
// Windows の COM ポート（2026-11-15）。ZMODEM は専用のスレッドで動くので、
// Win32SerialPort の時間切れ付きの読み書きをそのまま使う。
class ZmodemSerialIo : public ZmodemIo
{
public:
    ZmodemSerialIo(QString port, int baud, std::atomic_bool *canceled)
        : m_portName(std::move(port))
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
        m_error.clear();
        return m_serial.open(m_portName, m_baud, error);
    }

    void close()
    {
        m_serial.close();
        m_rx.clear();
    }

    void pushBack(const QByteArray &data) override
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

            const int wait = int(std::min<qint64>(50, qint64(timeoutMs) - elapsed));
            if (wait <= 0)
                return ZmodemIo::Timeout;

            QString message;
            char c = 0;
            const int n = m_serial.read(&c, 1, wait, &message);
            if (n > 0)
                return quint8(c);
            if (n < 0) {
                fail(message.isEmpty() ? QStringLiteral("受信に失敗しました") : message);
                return ZmodemIo::Error;
            }
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
            if (m_canceled->load()) {
                fail(QStringLiteral("転送を中止しました"));
                return false;
            }

            QString message;
            const int n = m_serial.write(p, int(left), 5000, &message);
            if (n <= 0) {
                fail(message.isEmpty() ? QStringLiteral("送信に失敗しました") : message);
                return false;
            }
            p += n;
            left -= n;
            if (timer.elapsed() > 5000) {
                fail(QStringLiteral("送信が止まりました"));
                return false;
            }
        }
        return true;
    }

    bool flush() override
    {
        QString message;
        if (!m_serial.flush(&message)) {
            fail(message.isEmpty() ? QStringLiteral("送信を終えられません") : message);
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
        return m_serial.bytesAvailable() > 0;
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

    void fail(const QString &message)
    {
        if (m_error.isEmpty())
            m_error = message;
    }

    QString m_portName;
    int m_baud = 921600;
    std::atomic_bool *m_canceled = nullptr;
    Win32SerialPort m_serial;
    QByteArray m_rx;
    QString m_error;
};
#endif

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

bool ZmodemWorker::prepareUpload(QStringList *sendFiles, QList<JoinJob> *joins, QString *error)
{
    sendFiles->clear();
    joins->clear();

    // 環境変数が無いときは、64 MB 以下は 1 MB ずつ（実機の exFAT・NTFS で PSRAM が細いときも通りやすい）、
    // 64 MB を越えるものは 2 MB ずつ（64 個の JOIN 上限に収める）にする。
    const bool partSizeSet = !qEnvironmentVariableIsEmpty("SGNANO_ZM_PART");
    const quint32 configuredPartSize = UploadSplit::partSizeFromEnvironment();
    QSet<QString> remoteNames;
    int splitIndex = 0;
    for (const QString &path : m_files) {
        const QFileInfo info(path);
        if (!info.isFile()) {
            *error = QStringLiteral("ファイルがありません: %1").arg(path);
            return false;
        }
        // 同じ名前を 2 つ送ると、実機の同じフォルダで後から来たものが先のものを置き換えてしまう
        const QString remoteName = info.fileName().toCaseFolded();
        if (remoteNames.contains(remoteName)) {
            *error = QStringLiteral("同じ名前のファイルは一度に送れません: %1").arg(info.fileName());
            return false;
        }
        remoteNames.insert(remoteName);
        if (quint64(info.size()) <= UploadSplit::kDeviceFileMax) {
            sendFiles->append(path);
            continue;
        }

        if (m_remotePath.trimmed().isEmpty()) {
            *error = QStringLiteral("4 MB を越えるファイルを送るには、リモートのフォルダを開いてください");
            return false;
        }
        if (info.fileName().contains(QLatin1Char('\t')) || info.fileName().contains(QLatin1Char('\n'))
            || info.fileName().contains(QLatin1Char('\r'))) {
            *error = QStringLiteral("4 MB を越えるファイルの名前にはタブや改行を使えません: %1").arg(info.fileName());
            return false;
        }
        if (!m_splitDir) {
            m_splitDir = std::make_unique<QTemporaryDir>(
                QDir::tempPath() + QStringLiteral("/sgnano_remote_split_XXXXXX"));
            if (!m_splitDir->isValid()) {
                *error = QStringLiteral("分けたファイルを置く一時フォルダを作れません: %1")
                             .arg(m_splitDir->errorString());
                return false;
            }
        }

        // 同じ名前のファイルを 2 つ選んでも .001 がぶつからないよう、元のファイルごとに別の入れ物へ分ける
        const QString splitSubDir = m_splitDir->filePath(QString::number(++splitIndex));
        if (!QDir().mkpath(splitSubDir)) {
            *error = QStringLiteral("分けたファイルを置く一時フォルダを作れません: %1").arg(splitSubDir);
            return false;
        }

        const quint32 partSize = UploadSplit::partSizeForFile(
            quint64(info.size()), partSizeSet, configuredPartSize);
        QStringList parts;
        if (!UploadSplit::splitFile(path, splitSubDir, partSize, &parts, error))
            return false;

        JoinJob job;
        job.baseName = info.fileName();
        job.parts = int(parts.size());
        const QString joinPath = joinRemotePath(m_remotePath, partName(job.baseName, 1));
        if (int(sizeof("JOIN ") - 1) + joinPath.toUtf8().size() >= kMaxRemoteCommandBytes) {
            *error = QStringLiteral("結合のコマンドが長すぎます（パスを短くしてください）: %1").arg(joinPath);
            return false;
        }
        joins->append(job);
        sendFiles->append(parts);
        emit logMessage(tr("%1 は 4 MB を越えるため %2 個に分けて送り、実機で結合します（1 つ %3）")
                            .arg(info.fileName())
                            .arg(parts.size())
                            .arg(QLocale().formattedDataSize(qint64(partSize))));
    }
    return true;
}

bool ZmodemWorker::waitForRemote(ZmodemIo &io, QString *error)
{
    if (m_remotePath.isEmpty() || m_remotePath.contains(QLatin1Char('\n'))
        || m_remotePath.contains(QLatin1Char('\r')) || m_remotePath.contains(QLatin1Char('\t'))) {
        *error = QStringLiteral("リモートのパスが正しくありません");
        return false;
    }

    const bool download = m_mode == Mode::Download;
    QByteArray commandText = download ? QByteArrayLiteral("ZGET ") : QByteArrayLiteral("ZPUT ");
    commandText.append(m_remotePath.toUtf8());
    if (commandText.size() >= kMaxRemoteCommandBytes) {
        *error = QStringLiteral("リモートのパスが長すぎます（%1 バイトまで）").arg(kMaxRemoteCommandBytes - 1);
        return false;
    }

    QByteArray command;
    command.append('\x1e');
    command.append(commandText);
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

bool ZmodemWorker::waitForRemoteCommands(ZmodemIo &io, QString *error)
{
    emit logMessage(tr("実機が遠隔コマンドを受け付けるのを待っています…"));

    QByteArray pending;
    for (int attempt = 0; attempt < 15; ++attempt) {
        if (m_canceled.load()) {
            *error = QStringLiteral("転送を中止しました");
            return false;
        }
        if (!io.put(QByteArrayLiteral("\x1ePING\n")) || !io.flush()) {
            *error = io.errorString().isEmpty()
                ? QStringLiteral("sgnano に PING を送れませんでした")
                : io.errorString();
            return false;
        }

        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 500) {
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
            if (pending.contains("@SG\tPONG"))
                return true;
            if (pending.size() > 65536)
                pending.remove(0, pending.size() - 4096);
        }
    }

    *error = QStringLiteral("ZMODEM のあと、sgnano が PING に答えません");
    return false;
}

bool ZmodemWorker::runRemoteCommand(ZmodemIo &io, const QString &command, int timeoutMs,
                                    QString *response, QString *error)
{
    if (command.toUtf8().size() >= kMaxRemoteCommandBytes) {
        *error = QStringLiteral("遠隔コマンドが長すぎます: %1").arg(command);
        return false;
    }
    QByteArray packet;
    packet.append('\x1e');
    packet.append(command.toUtf8());
    packet.append('\n');
    if (!io.put(packet) || !io.flush()) {
        *error = io.errorString().isEmpty()
            ? QStringLiteral("sgnano に %1 を送れませんでした").arg(command)
            : io.errorString();
        return false;
    }

    QByteArray pending;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
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
        if (c < 0)
            continue;

        pending.append(char(c));
        if (pending.size() > 65536)
            pending.remove(0, pending.size() - 4096);
        for (;;) {
            const int newline = pending.indexOf('\n');
            if (newline < 0)
                break;
            QByteArray line = pending.left(newline);
            pending.remove(0, newline + 1);
            if (line.endsWith('\r'))
                line.chop(1);
            if (line.startsWith("@SG\tOK")) {
                if (response)
                    *response = QString::fromUtf8(line);
                return true;
            }
            if (line.startsWith("@SG\tERR")) {
                const QList<QByteArray> fields = line.split('\t');
                const int code = fields.size() > 2 ? fields.at(2).toInt() : 0;
                const QString why = fields.size() > 3
                    ? QString::fromUtf8(fields.mid(3).join('\t'))
                    : QStringLiteral("不明なエラー");
                *error = QStringLiteral("%1 に失敗しました: %2 (code %3)").arg(command, why).arg(code);
                return false;
            }
        }
    }

    *error = QStringLiteral("%1 の応答がありません").arg(command);
    return false;
}

bool ZmodemWorker::joinUploadedFiles(ZmodemIo &io, const QList<JoinJob> &joins, QString *error)
{
    if (joins.isEmpty())
        return true;
    if (!waitForRemoteCommands(io, error))
        return false;

    for (const JoinJob &job : joins) {
        const QString firstPath = joinRemotePath(m_remotePath, partName(job.baseName, 1));
        emit logMessage(tr("実機で結合します: %1").arg(firstPath));

        QString response;
        if (!runRemoteCommand(io, QStringLiteral("JOIN ") + firstPath, 600000, &response, error))
            return false;
        if (!response.startsWith(QStringLiteral("@SG\tOK\tJOIN"))) {
            *error = QStringLiteral("sgnano が結合を受け付けませんでした: %1").arg(response);
            return false;
        }

        // 本体の JOIN は .001 .. を残す（scripts/zm.py と同じく PC から消す）
        for (int k = 1; k <= job.parts; ++k) {
            const QString partPath = joinRemotePath(m_remotePath, partName(job.baseName, k));
            QString ignored;
            QString deleteError;
            if (!runRemoteCommand(io, QStringLiteral("DEL ") + partPath, 60000, &ignored, &deleteError))
                emit logMessage(tr("分けたファイルを消せません: %1 (%2)").arg(partPath, deleteError));
        }
        emit logMessage(tr("実機で結合しました: %1（%2 個）").arg(job.baseName).arg(job.parts));
    }
    return true;
}

void ZmodemWorker::run()
{
    QString error;
    QStringList uploadFiles;
    QList<JoinJob> joins;
    if (m_mode == Mode::Upload && !prepareUpload(&uploadFiles, &joins, &error)) {
        emit finished(false, m_canceled.load(), error);
        return;
    }

    ZmodemSerialIo io(m_port, m_baud, &m_canceled);
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
        ok = zmodem.sendFiles(uploadFiles, &error);
        if (ok && !joins.isEmpty())
            ok = joinUploadedFiles(io, joins, &error);
    } else {
        QStringList received;
        ok = zmodem.receiveFiles(m_localDir, &received, &error);
        for (const QString &path : std::as_const(received))
            emit logMessage(tr("受け取りました: %1").arg(path));
    }

    const bool canceled = m_canceled.load();
    emit finished(ok, canceled, error);
}
