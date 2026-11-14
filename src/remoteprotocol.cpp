#include "remoteprotocol.h"

#include <QTimer>

namespace {

constexpr char kCommandMagic = '\x1e';
constexpr int kMaxRxBuffer = 1024 * 1024;
constexpr int kMaxQueuedRequests = 32;
// sgnano の uartcon.h（2026-11-14）: 頭の 0x1E と改行を除いた 1 行が 320 バイトまで
constexpr int kMaxCommandBytes = 320;

QByteArray unescapeField(const QByteArray &raw)
{
    QByteArray out;
    out.reserve(raw.size());

    for (int i = 0; i < raw.size(); ++i) {
        const char c = raw.at(i);
        if (c != '\\') {
            out.append(c);
            continue;
        }
        if (i + 1 >= raw.size()) {
            out.append(c);
            break;
        }

        const char next = raw.at(++i);
        switch (next) {
        case '\\':
            out.append('\\');
            break;
        case 't':
            out.append('\t');
            break;
        case 'n':
            out.append('\n');
            break;
        case 'r':
            out.append('\r');
            break;
        case 'x': {
            if (i + 2 < raw.size()) {
                bool ok = false;
                const int value = raw.mid(i + 1, 2).toInt(&ok, 16);
                if (ok) {
                    out.append(char(value));
                    i += 2;
                    break;
                }
            }
            out.append('x');
            break;
        }
        default:
            out.append(next);
            break;
        }
    }

    return out;
}

QString decodeField(const QByteArray &raw)
{
    return QString::fromUtf8(unescapeField(raw));
}

bool parseU64(const QByteArray &raw, quint64 *value)
{
    bool ok = false;
    const quint64 parsed = raw.toULongLong(&ok);
    if (ok && value)
        *value = parsed;
    return ok;
}

bool parseUInt(const QByteArray &raw, uint *value)
{
    bool ok = false;
    const uint parsed = raw.toUInt(&ok);
    if (ok && value)
        *value = parsed;
    return ok;
}

}

RemoteProtocol::RemoteProtocol(QObject *parent)
    : QObject(parent)
    , m_timeout(new QTimer(this))
    , m_listRetryTimer(new QTimer(this))
{
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, &RemoteProtocol::handleTimeout);

    m_listRetryTimer->setSingleShot(true);
    m_listRetryTimer->setInterval(250);
    connect(m_listRetryTimer, &QTimer::timeout, this, &RemoteProtocol::retryPendingList);
}

bool RemoteProtocol::isSynchronized() const
{
    return m_synchronized;
}

bool RemoteProtocol::isBusy() const
{
    return m_hasCurrent || m_hasListRetry || !m_queue.isEmpty();
}

void RemoteProtocol::feed(const QByteArray &data)
{
    if (data.isEmpty())
        return;

    m_rx.append(data);
    if (m_rx.size() > kMaxRxBuffer) {
        m_rx.clear();
        emit logMessage(tr("受信バッファがあふれたため古いデータを破棄しました"));
        return;
    }

    int start = 0;
    for (;;) {
        const int newline = m_rx.indexOf('\n', start);
        if (newline < 0)
            break;
        QByteArray line = m_rx.mid(start, newline - start);
        start = newline + 1;
        if (line.endsWith('\r'))
            line.chop(1);
        if (!line.isEmpty())
            handleLine(line);
    }

    if (start > 0)
        m_rx.remove(0, start);
}

void RemoteProtocol::reset()
{
    m_timeout->stop();
    m_listRetryTimer->stop();
    m_hasListRetry = false;
    m_listRetryRequest = Request();
    m_rx.clear();
    m_queue.clear();
    m_current = Request();
    m_hasCurrent = false;
    m_synchronized = false;
    m_listActive = false;
    m_listPath.clear();
    m_listCount = 0;
    m_listTruncated = false;
    m_pingAttempts = 0;
    m_listAttempts = 0;
    m_drives.clear();
    emit busyChanged(false);
}

void RemoteProtocol::sync()
{
    reset();
    m_pingAttempts = 0;
    enqueue(RequestType::Ping);
}

void RemoteProtocol::requestCwd()
{
    enqueue(RequestType::Cwd);
}

void RemoteProtocol::requestDrives()
{
    enqueue(RequestType::Drives);
}

void RemoteProtocol::requestList(const QString &path)
{
    m_listRetryTimer->stop();
    m_hasListRetry = false;
    m_listRetryRequest = Request();
    m_listAttempts = 0;
    enqueue(RequestType::List, path);
}

void RemoteProtocol::requestStat(const QString &path)
{
    enqueue(RequestType::Stat, path);
}

void RemoteProtocol::requestDelete(const QString &path)
{
    enqueue(RequestType::Delete, path);
}

void RemoteProtocol::requestRename(const QString &path, const QString &newName)
{
    enqueue(RequestType::Rename, path, newName);
}

void RemoteProtocol::requestMkdir(const QString &path)
{
    enqueue(RequestType::Mkdir, path);
}

void RemoteProtocol::requestCopy(const QString &source, const QString &target)
{
    enqueue(RequestType::Copy, source, target);
}

void RemoteProtocol::requestJoin(const QString &path)
{
    enqueue(RequestType::Join, path);
}

void RemoteProtocol::requestSplit(const QString &path, quint32 partSize)
{
    enqueue(RequestType::Split, path, partSize ? QString::number(partSize) : QString());
}

void RemoteProtocol::requestSysFsck(bool repair)
{
    enqueue(RequestType::SysFsck, repair ? QStringLiteral("REPAIR") : QString());
}

namespace {

bool hasControl(const QString &s)
{
    for (const QChar c : s) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7F)
            return true;
    }
    return false;
}

}

void RemoteProtocol::enqueue(RequestType type, const QString &argument, const QString &argument2)
{
    Request request;
    request.type = type;
    request.argument = argument;
    request.argument2 = argument2;

    switch (type) {
    case RequestType::Ping:
        request.command = QStringLiteral("PING");
        break;
    case RequestType::Cwd:
        request.command = QStringLiteral("CWD");
        break;
    case RequestType::Drives:
        request.command = QStringLiteral("DRIVES");
        break;
    case RequestType::List:
        if (argument.contains(QLatin1Char('\n')) || argument.contains(QLatin1Char('\r'))
            || argument.contains(QLatin1Char('\t'))) {
            emit requestFailed(QStringLiteral("LIST"), 2, tr("パスに制御文字が含まれています"));
            return;
        }
        request.command = argument.isEmpty() ? QStringLiteral("LIST")
                                             : QStringLiteral("LIST ") + argument;
        break;
    case RequestType::Stat:
        if (argument.contains(QLatin1Char('\n')) || argument.contains(QLatin1Char('\r'))
            || argument.contains(QLatin1Char('\t'))) {
            emit requestFailed(QStringLiteral("STAT"), 2, tr("パスに制御文字が含まれています"));
            return;
        }
        request.command = argument.isEmpty() ? QStringLiteral("STAT")
                                             : QStringLiteral("STAT ") + argument;
        break;
    case RequestType::Delete:
    case RequestType::Mkdir: {
        const QString name = type == RequestType::Delete ? QStringLiteral("DEL") : QStringLiteral("MKDIR");
        if (argument.trimmed().isEmpty() || hasControl(argument)) {
            emit requestFailed(name, 2, tr("パスが空か、制御文字が含まれています"));
            return;
        }
        request.command = name + QLatin1Char(' ') + argument;
        break;
    }
    case RequestType::Rename:
        // 実機は「元のパス<TAB>新しい名前」で受ける（名前に空白が入りうるのでタブで区切る）。新しい側が名前だけなら同じフォルダの中
        if (argument.trimmed().isEmpty() || argument2.trimmed().isEmpty() || hasControl(argument) || hasControl(argument2)) {
            emit requestFailed(QStringLiteral("REN"), 2, tr("名前が空か、制御文字が含まれています"));
            return;
        }
        request.command = QStringLiteral("REN ") + argument + QLatin1Char('\t') + argument2;
        break;
    case RequestType::Copy:
        // COPY 元<TAB>先（先はファイルのパス。同名は置き換える）
        if (argument.trimmed().isEmpty() || argument2.trimmed().isEmpty()
            || hasControl(argument) || hasControl(argument2)) {
            emit requestFailed(QStringLiteral("COPY"), 2, tr("パスが空か、制御文字が含まれています"));
            return;
        }
        request.command = QStringLiteral("COPY ") + argument + QLatin1Char('\t') + argument2;
        break;
    case RequestType::Join:
        if (argument.trimmed().isEmpty() || hasControl(argument)) {
            emit requestFailed(QStringLiteral("JOIN"), 2, tr("パスが空か、制御文字が含まれています"));
            return;
        }
        request.command = QStringLiteral("JOIN ") + argument;
        break;
    case RequestType::Split: {
        if (argument.trimmed().isEmpty() || hasControl(argument)) {
            emit requestFailed(QStringLiteral("SPLIT"), 2, tr("パスが空か、制御文字が含まれています"));
            return;
        }
        bool sizeOk = true;
        const quint32 partSize = argument2.isEmpty() ? 0 : argument2.toUInt(&sizeOk);
        if (!sizeOk || (partSize != 0 && (partSize < 512 || partSize % 512 != 0))) {
            emit requestFailed(QStringLiteral("SPLIT"), 2, tr("分ける大きさは 512 の倍数で指定してください"));
            return;
        }
        request.command = QStringLiteral("SPLIT ") + argument;
        if (partSize)
            request.command += QLatin1Char('\t') + QString::number(partSize);
        break;
    }
    case RequestType::SysFsck:
        if (argument != QStringLiteral("REPAIR") && !argument.isEmpty()) {
            emit requestFailed(QStringLiteral("SYSFSCK"), 2, tr("SYSFSCK の引数が正しくありません"));
            return;
        }
        request.command = argument.isEmpty() ? QStringLiteral("SYSFSCK")
                                             : QStringLiteral("SYSFSCK REPAIR");
        break;
    }

    if (request.command.toUtf8().size() >= kMaxCommandBytes) {
        emit requestFailed(request.command, 3, tr("コマンドが長すぎます"));
        return;
    }
    if (m_queue.size() >= kMaxQueuedRequests) {
        emit requestFailed(request.command, 3, tr("要求が多すぎます（待ち行列がいっぱいです）"));
        return;
    }

    m_queue.append(request);
    pump();
}

void RemoteProtocol::pump()
{
    if (m_hasCurrent || m_hasListRetry)
        return;
    if (m_queue.isEmpty()) {
        emit busyChanged(false);
        return;
    }

    m_current = m_queue.takeFirst();
    m_hasCurrent = true;

    if (m_current.type == RequestType::Ping)
        ++m_pingAttempts;
    if (m_current.type == RequestType::Drives)
        m_drives.clear();
    if (m_current.type == RequestType::List) {
        ++m_listAttempts;
        m_listActive = false;
        m_listPath.clear();
        m_listCount = 0;
        m_listTruncated = false;
    }

    int timeout = 5000;
    if (m_current.type == RequestType::Ping)
        timeout = 2500;
    else if (m_current.type == RequestType::List || m_current.type == RequestType::Stat)
        timeout = 10000;
    else if (m_current.type == RequestType::Delete || m_current.type == RequestType::Rename
             || m_current.type == RequestType::Mkdir)
        timeout = 35000;    // 実機は 1 回の操作を最長 30 秒待つ（NTFS・大きいフォルダ）
    else if (m_current.type == RequestType::Copy)
        timeout = 600000;   // 14MB の写しでも数分かかりうる
    else if (m_current.type == RequestType::Join || m_current.type == RequestType::Split)
        timeout = 600000;   // 10MB の結合が実機で約 21 秒。64 個（2 MB ずつで 128MB）まで見る
    else if (m_current.type == RequestType::SysFsck)
        timeout = 120000;

    emit busyChanged(true);
    m_timeout->start(timeout);

    QByteArray packet;
    packet.reserve(m_current.command.toUtf8().size() + 1);
    packet.append(kCommandMagic);
    packet.append(m_current.command.toUtf8());
    packet.append('\n');
    emit transmit(packet);
}

void RemoteProtocol::completeCurrent()
{
    if (!m_hasCurrent)
        return;

    m_timeout->stop();
    if (m_current.type == RequestType::List)
        m_listAttempts = 0;
    m_hasCurrent = false;
    m_current = Request();
    m_listActive = false;
    m_listPath.clear();
    m_listCount = 0;
    m_listTruncated = false;

    pump();
}

void RemoteProtocol::failCurrent(int code, const QString &message)
{
    if (!m_hasCurrent) {
        emit logMessage(tr("エラー応答: %1").arg(message));
        return;
    }

    const QString command = m_current.command;
    if (m_current.type == RequestType::Ping)
        m_synchronized = false;
    if (m_current.type == RequestType::List)
        m_listAttempts = 0;

    emit requestFailed(command, code, message);
    completeCurrent();
}

void RemoteProtocol::handleTimeout()
{
    if (!m_hasCurrent)
        return;

    const QString command = m_current.command;
    const bool retryPing = m_current.type == RequestType::Ping && m_pingAttempts < 3;
    const bool retryList = m_current.type == RequestType::List && m_listAttempts < 2;

    if (retryList) {
        emit logMessage(tr("LIST の応答がないため再試行します (%1/2)")
                            .arg(m_listAttempts + 1));
        m_listRetryRequest = m_current;
        m_hasListRetry = true;
        m_hasCurrent = false;
        m_current = Request();
        m_listActive = false;
        m_listPath.clear();
        m_listCount = 0;
        m_listTruncated = false;
        m_rx.clear();
        m_listRetryTimer->start();
        return;
    }

    m_hasCurrent = false;
    m_current = Request();
    m_listActive = false;
    m_listPath.clear();
    m_listCount = 0;
    m_listTruncated = false;
    m_rx.clear();

    if (retryPing) {
        emit logMessage(tr("PING の応答がないため再試行します (%1/3)").arg(m_pingAttempts + 1));
        enqueue(RequestType::Ping);
        return;
    }

    m_queue.clear();
    m_synchronized = false;
    m_listAttempts = 0;

    emit requestTimedOut(command);
    emit busyChanged(false);
}

void RemoteProtocol::retryPendingList()
{
    if (!m_hasListRetry || m_hasCurrent)
        return;

    Request request = m_listRetryRequest;
    m_listRetryRequest = Request();
    m_hasListRetry = false;
    m_queue.prepend(request);
    pump();
}

void RemoteProtocol::handleLine(const QByteArray &line)
{
    if (!line.startsWith("@SG")) {
        emit consoleLine(QString::fromUtf8(line));
        return;
    }

    const QList<QByteArray> fields = line.split('\t');
    if (fields.size() < 2) {
        emit logMessage(tr("不正な応答: %1").arg(QString::fromUtf8(line)));
        return;
    }

    handleFields(fields);
}

void RemoteProtocol::handleFields(const QList<QByteArray> &fields)
{
    const QByteArray &kind = fields.at(1);

    if (kind == "PONG") {
        if (!m_hasCurrent || m_current.type != RequestType::Ping) {
            emit logMessage(tr("予期しない PONG を受信しました"));
            return;
        }
        m_synchronized = true;
        m_pingAttempts = 0;
        emit synchronized();
        completeCurrent();
        return;
    }

    if (kind == "CWD") {
        if (!m_hasCurrent || m_current.type != RequestType::Cwd || fields.size() < 3) {
            emit logMessage(tr("予期しない CWD 応答を受信しました"));
            return;
        }
        emit cwdReceived(decodeField(fields.at(2)));
        completeCurrent();
        return;
    }

    if (kind == "DRV") {
        handleDrive(fields);
        return;
    }

    if (kind == "DRIVES") {
        if (!m_hasCurrent || m_current.type != RequestType::Drives || fields.size() < 4
            || fields.at(2) != "END") {
            emit logMessage(tr("予期しない DRIVES 応答を受信しました"));
            return;
        }
        emit drivesReceived(m_drives);
        completeCurrent();
        return;
    }

    if (kind == "LIST") {
        if (!m_hasCurrent || m_current.type != RequestType::List || fields.size() < 3) {
            emit logMessage(tr("予期しない LIST 応答を受信しました"));
            return;
        }

        const QByteArray &sub = fields.at(2);
        if (sub == "BEGIN") {
            if (fields.size() < 4) {
                failCurrent(0, tr("LIST 応答が不正です"));
                return;
            }
            m_listActive = true;
            m_listPath = decodeField(fields.at(3));
            m_listCount = 0;
            m_listTruncated = false;
            emit listStarted(m_listPath);
            return;
        }

        if (sub == "END") {
            if (!m_listActive || fields.size() < 5) {
                emit logMessage(tr("予期しない LIST END を受信しました"));
                return;
            }
            bool truncatedOk = false;
            const int count = fields.at(3).toInt();
            const bool truncated = fields.at(4).toInt(&truncatedOk) != 0;
            Q_UNUSED(truncatedOk);
            finishList(count, truncated);
            return;
        }

        if (sub == "D" || sub == "F") {
            handleListEntry(fields);
            return;
        }

        emit logMessage(tr("不明な LIST 応答: %1").arg(QString::fromUtf8(fields.join('\t'))));
        return;
    }

    if (kind == "STAT") {
        handleStat(fields);
        return;
    }

    if (kind == "SYSFSCK") {
        handleFsck(fields);
        return;
    }

    if (kind == "ERR") {
        handleError(fields);
        return;
    }

    if (kind == "OK") {
        handleOk(fields);
        return;
    }

    emit logMessage(tr("不明な応答: %1").arg(QString::fromUtf8(fields.join('\t'))));
}

void RemoteProtocol::handleDrive(const QList<QByteArray> &fields)
{
    if (!m_hasCurrent || m_current.type != RequestType::Drives || fields.size() < 5) {
        emit logMessage(tr("予期しない DRV 応答を受信しました"));
        return;
    }

    RemoteDrive drive;
    drive.letter = QString::fromLatin1(fields.at(2));
    drive.type = fields.at(3).toInt();
    drive.ready = fields.at(4).toInt() != 0;
    // 2026-11-08 の本体からは、USB の台（msc）と区画の先頭（plba）も返す。古い本体でも動くよう、無ければ 0 のまま
    if (fields.size() >= 6)
        drive.msc = fields.at(5).toInt();
    if (fields.size() >= 7)
        parseU64(fields.at(6), &drive.plba);
    m_drives.append(drive);
}

void RemoteProtocol::handleListEntry(const QList<QByteArray> &fields)
{
    if (!m_listActive || fields.size() < 7) {
        emit logMessage(tr("不正な LIST エントリを受信しました"));
        return;
    }

    RemoteEntry entry;
    entry.isDir = fields.at(2) == "D";
    entry.name = decodeField(fields.at(6));
    if (!parseU64(fields.at(3), &entry.size) || !parseU64(fields.at(4), &entry.mtime)
        || !parseUInt(fields.at(5), &entry.attr)) {
        emit logMessage(tr("LIST エントリを解析できません: %1")
                            .arg(QString::fromUtf8(fields.join('\t'))));
        return;
    }

    ++m_listCount;
    emit entryReceived(entry);
}

void RemoteProtocol::finishList(int count, bool truncated)
{
    if (count != m_listCount) {
        emit logMessage(tr("LIST の件数が一致しません（応答 %1、受信 %2）")
                            .arg(count)
                            .arg(m_listCount));
    }
    m_listTruncated = truncated;
    emit listFinished(count, truncated);
    completeCurrent();
}

void RemoteProtocol::handleStat(const QList<QByteArray> &fields)
{
    if (!m_hasCurrent || m_current.type != RequestType::Stat || fields.size() < 7) {
        emit logMessage(tr("予期しない STAT 応答を受信しました"));
        return;
    }

    RemoteStat stat;
    stat.isDir = fields.at(2) == "D";
    stat.name = decodeField(fields.at(6));
    if (!parseU64(fields.at(3), &stat.size) || !parseU64(fields.at(4), &stat.mtime)
        || !parseUInt(fields.at(5), &stat.attr)) {
        failCurrent(0, tr("STAT 応答を解析できません"));
        return;
    }

    emit statReceived(stat);
    completeCurrent();
}

void RemoteProtocol::handleFsck(const QList<QByteArray> &fields)
{
    if (!m_hasCurrent || m_current.type != RequestType::SysFsck || fields.size() < 3) {
        emit logMessage(tr("予期しない SYSFSCK 応答を受信しました"));
        return;
    }

    RemoteFsckResult result;
    result.repair = fields.at(2) == "REPAIR";
    for (int i = 3; i < fields.size(); ++i) {
        const QByteArray &field = fields.at(i);
        const int equals = field.indexOf('=');
        if (equals <= 0)
            continue;
        bool ok = false;
        const quint64 value = field.mid(equals + 1).toULongLong(&ok);
        if (!ok)
            continue;
        const QByteArray key = field.left(equals);
        if (key == "files")
            result.files = quint32(value);
        else if (key == "dirs")
            result.dirs = quint32(value);
        else if (key == "size_fixed")
            result.sizeFixed = quint32(value);
        else if (key == "freed")
            result.freed = quint32(value);
        else if (key == "cross")
            result.cross = quint32(value);
        else if (key == "bad")
            result.bad = quint32(value);
        else if (key == "fat2")
            result.fat2Fixed = quint32(value);
    }

    emit fsckFinished(result);
    completeCurrent();
}

void RemoteProtocol::handleOk(const QList<QByteArray> &fields)
{
    const bool expected = m_hasCurrent
        && (m_current.type == RequestType::Delete || m_current.type == RequestType::Rename
            || m_current.type == RequestType::Mkdir || m_current.type == RequestType::Copy
            || m_current.type == RequestType::Join || m_current.type == RequestType::Split);
    if (!expected || fields.size() < 4) {
        emit logMessage(tr("予期しない OK 応答を受信しました: %1").arg(QString::fromUtf8(fields.join('\t'))));
        return;
    }
    const QString op = QString::fromLatin1(fields.at(2));
    QString want;
    switch (m_current.type) {
    case RequestType::Delete: want = QStringLiteral("DEL"); break;
    case RequestType::Rename: want = QStringLiteral("REN"); break;
    case RequestType::Mkdir: want = QStringLiteral("MKDIR"); break;
    case RequestType::Copy: want = QStringLiteral("COPY"); break;
    case RequestType::Join: want = QStringLiteral("JOIN"); break;
    case RequestType::Split: want = QStringLiteral("SPLIT"); break;
    default: break;
    }
    if (op != want) {
        emit logMessage(tr("予期しない OK 応答を受信しました: %1").arg(QString::fromUtf8(fields.join('\t'))));
        return;
    }
    emit operationSucceeded(op, decodeField(fields.at(3)));
    completeCurrent();
}

void RemoteProtocol::handleError(const QList<QByteArray> &fields)
{
    int code = -1;
    if (fields.size() >= 3) {
        bool ok = false;
        const int parsed = fields.at(2).toInt(&ok);
        if (ok)
            code = parsed;
    }
    const QString message = fields.size() >= 4
        ? QString::fromUtf8(fields.mid(3).join('\t'))
        : tr("不明なエラー");
    failCurrent(code, message);
}
