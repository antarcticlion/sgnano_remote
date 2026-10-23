#include "zmodem.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace {

constexpr quint8 kZpad = '*';
constexpr quint8 kZdle = 0x18;
constexpr quint8 kZbin = 'A';
constexpr quint8 kZhex = 'B';
constexpr quint8 kZbin32 = 'C';

enum HeaderType {
    ZRQINIT,
    ZRINIT,
    ZSINIT,
    ZACK,
    ZFILE,
    ZSKIP,
    ZNAK,
    ZABORT,
    ZFIN,
    ZRPOS,
    ZDATA,
    ZEOF,
    ZFERR,
    ZCRC,
    ZCHALLENGE,
    ZCOMPL,
    ZCAN,
    ZFREECNT,
    ZCOMMAND,
    ZSTDERR,
};

enum DataEnd {
    ZCRCE = 'h',
    ZCRCG = 'i',
    ZCRCQ = 'j',
    ZCRCW = 'k',
    ZRUB0 = 'l',
    ZRUB1 = 'm',
};

constexpr quint8 kCanFdx = 0x01;
constexpr quint8 kCanOvIo = 0x02;
constexpr quint8 kZCBin = 1;

constexpr int kSubMax = 1024;             // 送る小包の大きさ（lrzsz の上限と同じ）
constexpr int kRxSubMax = 2048;           // 受け取る小包の上限
constexpr unsigned kHdrTimeoutMs = 10000;
constexpr unsigned kByteTimeoutMs = 5000;
constexpr int kMaxRestarts = 20;
constexpr int kMaxFileTries = 10;
constexpr int kMaxDataTries = 10;
constexpr int kMaxWakeTries = 20;

// sgnano 本体（src/filer/fzm.c）の受け取りの上限。これより大きいファイルは ZSKIP で断られる
constexpr quint32 kDeviceFileMax = 4u * 1024u * 1024u;
// 本体が受け取る名前の大きさ（同 fzm.c の char name[132]）
constexpr int kDeviceNameMax = 131;

constexpr int RTimeout = -1;
constexpr int RCancel = -2;
constexpr int RError = -3;
constexpr int RAbort = -4;
constexpr int GotEnd = 0x100;

constexpr int TxOk = 0;
constexpr int TxRestart = 1;
constexpr int TxFail = -1;
constexpr int TxSkip = 2;

quint16 crc16Upd(quint16 crc, quint8 c)
{
    crc ^= quint16(c) << 8;
    for (int i = 0; i < 8; ++i)
        crc = (crc & 0x8000u) ? quint16((crc << 1) ^ 0x1021u) : quint16(crc << 1);
    return crc;
}

quint32 crc32Upd(quint32 crc, quint8 c)
{
    crc ^= c;
    for (int i = 0; i < 8; ++i)
        crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    return crc;
}

int hexVal(int c)
{
    c &= 0x7F;
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool mustEscape(quint8 c)
{
    switch (c) {
    case kZdle:
    case 0x10:
    case 0x90:
    case 0x11:
    case 0x91:
    case 0x13:
    case 0x93:
    case 0x0D:
    case 0x8D:
        return true;
    default:
        return false;
    }
}

void appendEscaped(QByteArray &out, quint8 c)
{
    if (mustEscape(c)) {
        out.append(char(kZdle));
        out.append(char(c ^ 0x40));
    } else if (c == 0x7F) {
        out.append(char(kZdle));
        out.append(char(ZRUB0));
    } else if (c == 0xFF) {
        out.append(char(kZdle));
        out.append(char(ZRUB1));
    } else {
        out.append(char(c));
    }
}

void putPosition(quint8 h[4], quint32 pos)
{
    h[0] = quint8(pos);
    h[1] = quint8(pos >> 8);
    h[2] = quint8(pos >> 16);
    h[3] = quint8(pos >> 24);
}

quint32 headerPosition(const QByteArray &h)
{
    return quint32(quint8(h.at(0)))
        | (quint32(quint8(h.at(1))) << 8)
        | (quint32(quint8(h.at(2))) << 16)
        | (quint32(quint8(h.at(3))) << 24);
}

struct TxFile
{
    QString path;
    QByteArray name;
    quint32 size = 0;
    quint32 mtime = 0;
};

struct Tx
{
    QList<TxFile> files;
    quint32 pos = 0;
    quint32 size = 0;
    quint32 buffer = 4096;
    quint32 lastRpos = std::numeric_limits<quint32>::max();
    quint32 restartAt = 0;
    qint64 doneBase = 0;
    qint64 total = 0;
    int tries = 0;
};

struct Rx
{
    QFile file;
    QString name;
    QString localDir;
    QString target;
    QStringList *received = nullptr;
    quint32 pos = 0;
    quint32 size = 0;
    quint32 mtime = 0;
    int files = 0;
    bool open = false;
};

enum RxResult {
    RxContinue,
    RxDone,
    RxFail,
};

}

class Zmodem::Private
{
public:
    Private(ZmodemIo *io, Progress progress)
        : m_io(io)
        , m_progress(std::move(progress))
        , m_buf(kRxSubMax, '\0')
    {
    }

    bool sendFiles(const QStringList &paths, QString *error);
    bool receiveFiles(const QString &localDir, QStringList *received, QString *error);

private:
    bool put(const QByteArray &data);
    bool flush();
    bool sendHexHdr(int type, const quint8 h[4]);
    bool sendBinHdr(int type, const quint8 h[4]);
    bool sendData(const char *p, int n, quint8 end);
    bool sendPos(int type, quint32 pos);
    bool sendRinit();
    bool sendNak();
    void sendCancel();

    int rd(unsigned timeoutMs);
    int rdNoXon(unsigned timeoutMs);
    int zdlRead();
    int recvHdr(unsigned timeoutMs);
    int recvData(char *buf, int max, int *len);
    void drainHeaderTail();

    int txWake(Tx &x);
    int txZfile(Tx &x, const TxFile &file);
    int txStream(Tx &x, QFile &input);
    int txZeof(Tx &x, const TxFile &file);
    void txZfin();
    int txRewind(Tx &x, quint32 pos);
    qint64 txCheckBack();

    int rxHeader(Rx &rx, int type);
    int rxZfile(Rx &rx);
    int rxZdata(Rx &rx);
    int rxZeof(Rx &rx);
    int rxZfin(Rx &rx);
    bool rxOpen(Rx &rx, const QString &name, quint32 size, quint32 mtime, QString *error);
    bool rxWrite(Rx &rx, quint32 pos, const char *data, int n, QString *error);
    bool rxClose(Rx &rx, quint32 len, QString *error);
    void rxDiscard(Rx &rx);

    void progress(qint64 done, qint64 total)
    {
        if (m_progress)
            m_progress(done, total);
    }

    void setError(QString *error, const QString &text)
    {
        if (!error)
            return;
        const QString ioError = m_io->errorString();
        *error = ioError.isEmpty() ? text : ioError;
    }

    ZmodemIo *m_io = nullptr;
    Progress m_progress;
    QByteArray m_buf;
    QByteArray m_hdr = QByteArray(4, '\0');
    QString m_rxError;
    bool m_crc32 = false;
};

bool Zmodem::Private::put(const QByteArray &data)
{
    return m_io->put(data);
}

bool Zmodem::Private::flush()
{
    return m_io->flush();
}

bool Zmodem::Private::sendHexHdr(int type, const quint8 h[4])
{
    static const char hex[] = "0123456789abcdef";

    QByteArray out;
    out.reserve(32);
    out.append(char(kZpad));
    out.append(char(kZpad));
    out.append(char(kZdle));
    out.append(char(kZhex));

    quint16 crc = 0;
    const quint8 body[5] = { quint8(type), h[0], h[1], h[2], h[3] };
    for (int i = 0; i < 5; ++i) {
        out.append(hex[body[i] >> 4]);
        out.append(hex[body[i] & 15]);
        crc = crc16Upd(crc, body[i]);
    }
    out.append(hex[(crc >> 12) & 15]);
    out.append(hex[(crc >> 8) & 15]);
    out.append(hex[(crc >> 4) & 15]);
    out.append(hex[crc & 15]);
    out.append('\r');
    out.append(char(0x8A));
    if (type != ZFIN && type != ZACK)
        out.append(char(0x11));        // XON（lrzsz と同じ）

    return put(out);
}

bool Zmodem::Private::sendBinHdr(int type, const quint8 h[4])
{
    QByteArray out;
    out.reserve(16);
    out.append(char(kZpad));
    out.append(char(kZdle));
    out.append(char(kZbin));

    const quint8 body[5] = { quint8(type), h[0], h[1], h[2], h[3] };
    quint16 crc = 0;
    for (int i = 0; i < 5; ++i) {
        appendEscaped(out, body[i]);
        crc = crc16Upd(crc, body[i]);
    }
    appendEscaped(out, quint8(crc >> 8));
    appendEscaped(out, quint8(crc));

    return put(out);
}

bool Zmodem::Private::sendData(const char *p, int n, quint8 end)
{
    QByteArray out;
    out.reserve(n * 2 + 8);
    quint16 crc = 0;
    const quint8 *data = reinterpret_cast<const quint8 *>(p);
    for (int i = 0; i < n; ++i) {
        appendEscaped(out, data[i]);
        crc = crc16Upd(crc, data[i]);
    }
    out.append(char(kZdle));
    out.append(char(end));
    crc = crc16Upd(crc, end);
    appendEscaped(out, quint8(crc >> 8));
    appendEscaped(out, quint8(crc));
    if (end == ZCRCW)
        out.append(char(0x11));        // XON（lrzsz と同じ）

    return put(out);
}

bool Zmodem::Private::sendPos(int type, quint32 pos)
{
    quint8 h[4];
    putPosition(h, pos);
    return sendHexHdr(type, h) && flush();
}

bool Zmodem::Private::sendRinit()
{
    const quint8 h[4] = {
        quint8(kRxSubMax),
        quint8(kRxSubMax >> 8),
        0,
        quint8(kCanFdx | kCanOvIo),
    };
    return sendHexHdr(ZRINIT, h) && flush();
}

bool Zmodem::Private::sendNak()
{
    const quint8 zero[4] = { 0, 0, 0, 0 };
    return sendHexHdr(ZNAK, zero) && flush();
}

void Zmodem::Private::sendCancel()
{
    QByteArray out(10, char(kZdle));
    out.append(QByteArray(8, char(0x08)));
    m_io->put(out);
    m_io->flush();
}

int Zmodem::Private::rd(unsigned timeoutMs)
{
    const int c = m_io->getc(timeoutMs);
    if (c == ZmodemIo::Timeout)
        return RTimeout;
    if (c == ZmodemIo::Cancel)
        return RCancel;
    if (c < 0)
        return RError;
    return c;
}

int Zmodem::Private::rdNoXon(unsigned timeoutMs)
{
    for (;;) {
        const int c = rd(timeoutMs);
        if (c < 0)
            return c;
        const int b = c & 0x7F;
        if (b == 0x11 || b == 0x13)
            continue;
        return c;
    }
}

int Zmodem::Private::zdlRead()
{
    int c = rd(kByteTimeoutMs);
    if (c < 0)
        return c;

    if (c != kZdle) {
        while ((c & 0x7F) == 0x11 || (c & 0x7F) == 0x13) {
            c = rd(kByteTimeoutMs);
            if (c < 0)
                return c;
        }
        return c;
    }

    int count = 1;
    for (;;) {
        c = rd(kByteTimeoutMs);
        if (c < 0)
            return c;
        if (c == kZdle) {
            if (++count >= 5)
                return RAbort;
            continue;
        }
        if ((c & 0x7F) == 0x11 || (c & 0x7F) == 0x13)
            continue;
        break;
    }

    switch (c) {
    case ZCRCE:
    case ZCRCG:
    case ZCRCQ:
    case ZCRCW:
        return GotEnd | c;
    case ZRUB0:
        return 0x7F;
    case ZRUB1:
        return 0xFF;
    default:
        if ((c & 0x60) == 0x40)
            return c ^ 0x40;
        return RError;
    }
}

int Zmodem::Private::recvHdr(unsigned timeoutMs)
{
    int cans = 0;
    for (;;) {
        int c = rd(timeoutMs);
        if (c < 0)
            return c;
        if (c == kZdle) {
            if (++cans >= 5)
                return RAbort;
            continue;
        }
        cans = 0;
        if ((c & 0x7F) != kZpad)
            continue;

        do {
            c = rd(kByteTimeoutMs);
        } while (c >= 0 && (c & 0x7F) == kZpad);
        if (c < 0)
            return c;
        if (c != kZdle)
            continue;

        c = rd(kByteTimeoutMs);
        if (c < 0)
            return c;

        quint8 body[5] = { 0, 0, 0, 0, 0 };
        if (c == kZhex) {
            quint8 v[7] = { 0, 0, 0, 0, 0, 0, 0 };
            for (int i = 0; i < 7; ++i) {
                const int h1 = rdNoXon(kByteTimeoutMs);
                if (h1 < 0)
                    return h1;
                const int h2 = rdNoXon(kByteTimeoutMs);
                if (h2 < 0)
                    return h2;
                const int a = hexVal(h1);
                const int b = hexVal(h2);
                if (a < 0 || b < 0)
                    return RError;
                v[i] = quint8((a << 4) | b);
            }
            quint16 crc = 0;
            for (int i = 0; i < 5; ++i)
                crc = crc16Upd(crc, v[i]);
            if (crc != quint16((v[5] << 8) | v[6]))
                return RError;
            std::memcpy(body, v, 5);
            m_crc32 = false;
            drainHeaderTail();
        } else if (c == kZbin || c == kZbin32) {
            const bool c32 = c == kZbin32;
            for (int i = 0; i < 5; ++i) {
                const int d = zdlRead();
                if (d < 0 || d > 0xFF)
                    return d < 0 ? d : RError;
                body[i] = quint8(d);
            }
            if (c32) {
                quint32 crc = 0xFFFFFFFFu;
                quint32 got = 0;
                for (int i = 0; i < 5; ++i)
                    crc = crc32Upd(crc, body[i]);
                for (int i = 0; i < 4; ++i) {
                    const int d = zdlRead();
                    if (d < 0 || d > 0xFF)
                        return d < 0 ? d : RError;
                    got |= quint32(d) << (8 * i);
                }
                if (~crc != got)
                    return RError;
            } else {
                quint16 crc = 0;
                for (int i = 0; i < 5; ++i)
                    crc = crc16Upd(crc, body[i]);
                const int d1 = zdlRead();
                const int d2 = zdlRead();
                if (d1 < 0 || d1 > 0xFF || d2 < 0 || d2 > 0xFF)
                    return RError;
                if (crc != quint16((d1 << 8) | d2))
                    return RError;
            }
            m_crc32 = c32;
        } else {
            continue;
        }

        m_hdr[0] = char(body[1]);
        m_hdr[1] = char(body[2]);
        m_hdr[2] = char(body[3]);
        m_hdr[3] = char(body[4]);
        return body[0];
    }
}

// 16 進の見出しのあとに付く CR / LF / XON を、いま届いている分だけ捨てる。
// 残すと、次に「相手の知らせが来たか」を見るときに、見出しでないバイトを読みながら
// 時間切れ（100ms）まで待つことになる。
void Zmodem::Private::drainHeaderTail()
{
    for (int i = 0; i < 3; ++i) {
        if (!m_io->rxReady())
            break;
        const int c = m_io->getc(0);
        if (c < 0)
            break;

        switch (c) {
        case '\r':
        case '\n':
        case 0x8A:
        case 0x11:
        case 0x13:
        case 0x90:
        case 0x91:
        case 0x93:
            continue;
        default:
            m_io->pushBack(QByteArray(1, char(c)));
            return;
        }
    }
}

int Zmodem::Private::recvData(char *buf, int max, int *len)
{
    int n = 0;
    quint32 c32 = 0xFFFFFFFFu;
    quint16 c16 = 0;
    for (;;) {
        const int c = zdlRead();
        if (c < 0)
            return c;
        if (c & GotEnd) {
            const quint8 end = quint8(c);
            if (m_crc32) {
                c32 = crc32Upd(c32, end);
                quint32 got = 0;
                for (int i = 0; i < 4; ++i) {
                    const int d = zdlRead();
                    if (d < 0 || d > 0xFF)
                        return d < 0 ? d : RError;
                    got |= quint32(d) << (8 * i);
                }
                if (~c32 != got)
                    return RError;
            } else {
                c16 = crc16Upd(c16, end);
                const int d1 = zdlRead();
                const int d2 = zdlRead();
                if (d1 < 0 || d1 > 0xFF || d2 < 0 || d2 > 0xFF)
                    return RError;
                if (c16 != quint16((d1 << 8) | d2))
                    return RError;
            }
            *len = n;
            return end;
        }
        if (n >= max)
            return RError;
        buf[n++] = char(c);
        if (m_crc32)
            c32 = crc32Upd(c32, quint8(c));
        else
            c16 = crc16Upd(c16, quint8(c));
    }
}

int Zmodem::Private::txWake(Tx &x)
{
    const quint8 zero[4] = { 0, 0, 0, 0 };
    int last = RTimeout;

    for (int i = 0; i < kMaxWakeTries; ++i) {
        if (!sendHexHdr(ZRQINIT, zero) || !flush())
            return TxFail;

        last = recvHdr(3000);
        if (last == ZCHALLENGE) {
            if (!sendHexHdr(ZACK, reinterpret_cast<const quint8 *>(m_hdr.constData())) || !flush())
                return TxFail;
            --i;
            continue;
        }
        if (last == ZRINIT) {
            const unsigned announced = unsigned(quint8(m_hdr.at(0)))
                | (unsigned(quint8(m_hdr.at(1))) << 8);
            x.buffer = announced ? announced : 4096;
            return TxOk;
        }
        if (last == RCancel || last == RAbort || last == ZCAN || last == ZABORT)
            return TxFail;
    }

    sendCancel();
    return TxFail;
}

int Zmodem::Private::txZfile(Tx &x, const TxFile &file)
{
    const quint8 h[4] = { 0, 0, 0, kZCBin };
    QByteArray info = file.name;
    info.append('\0');
    info.append(QByteArray::number(file.size));
    info.append(' ');
    info.append(QByteArray::number(file.mtime, 8));
    info.append(" 100644 0 1 ");
    info.append(QByteArray::number(file.size));
    info.append('\0');

    int tries = 0;
    bool sent = false;
    for (;;) {
        if (!sent) {
            if (!sendBinHdr(ZFILE, h) || !sendData(info.constData(), info.size(), ZCRCW) || !flush())
                return TxFail;
            sent = true;
        }

        const int t = recvHdr(kHdrTimeoutMs);
        // ZRQINIT への返事（ZRINIT）が遅れて届くことがある。ZFILE は届いているので送り直さない
        if (t == ZRINIT)
            continue;
        if (t == ZSKIP)
            return TxSkip;
        if (t == ZRPOS) {
            x.pos = headerPosition(m_hdr);
            if (x.size && x.pos > x.size)
                x.pos = x.size;
            x.tries = 0;
            return TxOk;
        }
        if (t == RCancel || t == RAbort || t == ZCAN || t == ZABORT)
            return TxFail;

        if (++tries > kMaxFileTries) {
            sendCancel();
            return TxFail;
        }
        sent = false;
    }
}

int Zmodem::Private::txRewind(Tx &x, quint32 pos)
{
    if (x.size && pos > x.size)
        pos = x.size;
    if (x.lastRpos == std::numeric_limits<quint32>::max() || pos > x.lastRpos)
        x.tries = 0;
    x.lastRpos = x.restartAt = pos;
    x.pos = pos;
    if (++x.tries > kMaxRestarts) {
        sendCancel();
        return TxFail;
    }
    return TxRestart;
}

qint64 Zmodem::Private::txCheckBack()
{
    while (m_io->rxReady()) {
        const int t = recvHdr(100);
        if (t == ZRPOS)
            return qint64(headerPosition(m_hdr));
        if (t == RCancel || t == RAbort || t == ZCAN || t == ZABORT)
            return -2;
        if (t < 0)
            break;
    }
    return -1;
}

int Zmodem::Private::txStream(Tx &x, QFile &input)
{
    if (x.pos > x.size)
        x.pos = x.size;

    quint8 h[4];
    putPosition(h, x.pos);
    if (!sendBinHdr(ZDATA, h))
        return TxFail;

    if (x.pos >= x.size) {
        if (!sendData(nullptr, 0, ZCRCE) || !flush())
            return TxFail;
    }

    quint32 sinceAck = 0;
    while (x.pos < x.size) {
        quint32 n = quint32(std::min<qint64>(kSubMax, qint64(x.size) - x.pos));
        if (x.buffer && n > x.buffer)
            n = x.buffer;
        if (n == 0)
            n = 1;

        if (!input.seek(qint64(x.pos)))
            return TxFail;
        QByteArray chunk(int(n), '\0');
        const qint64 got = input.read(chunk.data(), n);
        if (got != qint64(n))
            return TxFail;

        x.pos += n;
        sinceAck += n;

        quint8 end = ZCRCG;
        if (x.pos >= x.size)
            end = ZCRCE;
        else if (x.buffer && sinceAck + kSubMax > x.buffer)
            end = ZCRCW;

        if (!sendData(chunk.constData(), chunk.size(), end))
            return TxFail;
        progress(x.doneBase + x.pos, x.total);

        if (end == ZCRCW) {
            if (!flush())
                return TxFail;
            sinceAck = 0;

            const int t = recvHdr(kHdrTimeoutMs);
            if (t == ZACK) {
                putPosition(h, x.pos);
                if (!sendBinHdr(ZDATA, h))
                    return TxFail;
                continue;
            }
            if (t == ZRPOS) {
                const int r = txRewind(x, headerPosition(m_hdr));
                if (r == TxFail)
                    return TxFail;
                putPosition(h, x.pos);
                if (!sendBinHdr(ZDATA, h))
                    return TxFail;
                sinceAck = 0;
                continue;
            }
            if (t == RCancel || t == RAbort || t == ZCAN || t == ZABORT)
                return TxFail;
            if (++x.tries > kMaxDataTries) {
                sendCancel();
                return TxFail;
            }
            return TxRestart;
        }

        const qint64 back = txCheckBack();
        if (back == -2)
            return TxFail;
        if (back >= 0) {
            if (quint32(back) == x.lastRpos && x.pos - x.restartAt < 4096u)
                continue;
            const int r = txRewind(x, quint32(back));
            if (r == TxFail)
                return TxFail;
            return TxRestart;
        }
    }

    return TxOk;
}

int Zmodem::Private::txZeof(Tx &x, const TxFile &file)
{
    Q_UNUSED(file);
    quint8 h[4];
    putPosition(h, x.size);

    for (int tries = 0; tries < kMaxFileTries; ++tries) {
        if (!sendBinHdr(ZEOF, h) || !flush())
            return TxFail;

        const int t = recvHdr(kHdrTimeoutMs);
        if (t == ZRINIT)
            return TxOk;
        if (t == ZRPOS)
            return txRewind(x, headerPosition(m_hdr));
        if (t == RCancel || t == RAbort || t == ZCAN || t == ZABORT)
            return TxFail;
    }

    sendCancel();
    return TxFail;
}

void Zmodem::Private::txZfin()
{
    const quint8 zero[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 5; ++i) {
        if (!sendHexHdr(ZFIN, zero) || !flush())
            break;
        const int t = recvHdr(3000);
        if (t == ZFIN || t < 0)
            break;
    }
    m_io->put(QByteArrayLiteral("OO"));
    m_io->flush();
}

bool Zmodem::Private::sendFiles(const QStringList &paths, QString *error)
{
    if (!m_io) {
        setError(error, QStringLiteral("ZMODEM の読み書きの口がありません"));
        return false;
    }
    if (paths.isEmpty()) {
        setError(error, QStringLiteral("送るファイルがありません"));
        return false;
    }

    Tx x;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.isFile()) {
            setError(error, QStringLiteral("ファイルがありません: %1").arg(path));
            return false;
        }
        if (info.size() < 0 || quint64(info.size()) > std::numeric_limits<quint32>::max()) {
            setError(error, QStringLiteral("ZMODEM で送れない大きさです: %1").arg(path));
            return false;
        }
        if (quint64(info.size()) > kDeviceFileMax) {
            setError(error, QStringLiteral("sgnano は 4 MB までしか受け取れません: %1").arg(info.fileName()));
            return false;
        }
        const QByteArray name = info.fileName().toUtf8();
        if (name.isEmpty() || name.size() > kDeviceNameMax) {
            setError(error, QStringLiteral("sgnano が受け取れる名前は %1 バイトまでです: %2")
                              .arg(kDeviceNameMax)
                              .arg(info.fileName()));
            return false;
        }
        TxFile file;
        file.path = path;
        file.name = name;
        file.size = quint32(info.size());
        const qint64 mtime = info.lastModified().toSecsSinceEpoch();
        file.mtime = mtime > 0 ? quint32(mtime) : 0;
        x.files.append(file);
        x.total += file.size;
    }

    if (txWake(x) != TxOk) {
        setError(error, QStringLiteral("sgnano が ZMODEM を受け付けませんでした"));
        return false;
    }

    qint64 done = 0;
    QStringList skipped;
    for (const TxFile &file : std::as_const(x.files)) {
        QFile input(file.path);
        if (!input.open(QIODevice::ReadOnly)) {
            sendCancel();
            setError(error, QStringLiteral("%1 を読めません: %2").arg(file.path, input.errorString()));
            return false;
        }

        x.pos = 0;
        x.size = file.size;
        x.lastRpos = std::numeric_limits<quint32>::max();
        x.restartAt = 0;
        x.tries = 0;
        x.doneBase = done;

        const int start = txZfile(x, file);
        if (start == TxFail) {
            setError(error, QStringLiteral("%1 を送れませんでした").arg(file.name));
            return false;
        }
        if (start == TxSkip) {
            skipped.append(QString::fromUtf8(file.name));
            done += file.size;
            progress(done, x.total);
            continue;
        }

        for (;;) {
            const int stream = txStream(x, input);
            if (stream == TxFail) {
                setError(error, QStringLiteral("%1 を送れませんでした").arg(file.name));
                return false;
            }
            if (stream == TxRestart)
                continue;

            const int eof = txZeof(x, file);
            if (eof == TxOk)
                break;
            if (eof == TxFail) {
                setError(error, QStringLiteral("%1 の終わりを受け取れませんでした").arg(file.name));
                return false;
            }
        }

        done += file.size;
        progress(done, x.total);
    }

    txZfin();

    if (!skipped.isEmpty()) {
        setError(error, QStringLiteral("sgnano が受け取らなかったファイルがあります: %1")
                          .arg(skipped.join(QStringLiteral(", "))));
        return false;
    }
    return true;
}

bool Zmodem::Private::rxOpen(Rx &rx, const QString &name, quint32 size, quint32 mtime, QString *error)
{
    QString base = name;
    const int slash = std::max(base.lastIndexOf(QLatin1Char('/')), base.lastIndexOf(QLatin1Char('\\')));
    if (slash >= 0)
        base = base.mid(slash + 1);
    if (base.isEmpty() || base == QStringLiteral(".") || base == QStringLiteral("..")) {
        setError(error, QStringLiteral("受け取れない名前です: %1").arg(name));
        return false;
    }

    // 送り手が ZFILE を送り直したときは、開いたままの前の書きかけを閉じる
    if (rx.file.isOpen())
        rx.file.close();

    rx.file.setFileName(QDir(rx.localDir).filePath(base));
    rx.name = base;
    rx.target = rx.file.fileName();
    rx.size = size;
    rx.mtime = mtime;
    rx.pos = 0;
    rx.open = true;

    if (rx.file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        progress(0, rx.size);
        return true;
    }

    rx.open = false;
    setError(error, QStringLiteral("%1 を書けません: %2").arg(rx.target, rx.file.errorString()));
    return false;
}

bool Zmodem::Private::rxWrite(Rx &rx, quint32 pos, const char *data, int n, QString *error)
{
    if (!rx.open)
        return false;
    if (rx.size && quint64(pos) + quint64(n) > rx.size) {
        setError(error, QStringLiteral("%1 の大きさを超えるデータが来ました").arg(rx.name));
        return false;
    }
    if (!rx.file.seek(qint64(pos))) {
        setError(error, QStringLiteral("%1 を書けません: %2").arg(rx.target, rx.file.errorString()));
        return false;
    }
    if (rx.file.write(data, n) != n) {
        setError(error, QStringLiteral("%1 を書けません: %2").arg(rx.target, rx.file.errorString()));
        return false;
    }
    rx.pos = pos + quint32(n);
    progress(rx.pos, rx.size);
    return true;
}

bool Zmodem::Private::rxClose(Rx &rx, quint32 len, QString *error)
{
    if (!rx.open)
        return false;
    if (rx.size && len != rx.size) {
        setError(error, QStringLiteral("%1 が途中で終わりました").arg(rx.name));
        return false;
    }
    if (rx.mtime)
        rx.file.setFileTime(QDateTime::fromSecsSinceEpoch(qint64(rx.mtime)),
                            QFileDevice::FileModificationTime);
    if (!rx.file.flush()) {
        setError(error, QStringLiteral("%1 を書けません: %2").arg(rx.target, rx.file.errorString()));
        return false;
    }
    rx.file.close();
    rx.open = false;
    if (rx.received)
        rx.received->append(rx.target);
    ++rx.files;
    progress(rx.size, rx.size);
    return true;
}

void Zmodem::Private::rxDiscard(Rx &rx)
{
    if (!rx.open)
        return;
    rx.file.close();
    rx.file.remove();
    rx.open = false;
}

int Zmodem::Private::rxZfile(Rx &rx)
{
    int len = 0;
    const int end = recvData(m_buf.data(), m_buf.size(), &len);
    if (end < 0) {
        sendNak();
        return RxContinue;
    }

    const QByteArray data(m_buf.constData(), len);
    const int nul = data.indexOf('\0');
    if (nul < 0) {
        sendNak();
        return RxContinue;
    }

    const QString name = QString::fromUtf8(data.left(nul));
    const QByteArray info = data.mid(nul + 1);
    const QList<QByteArray> fields = info.split(' ');
    quint32 size = 0;
    quint32 mtime = 0;
    if (fields.size() >= 1)
        size = fields.at(0).toUInt();
    if (fields.size() >= 2)
        mtime = fields.at(1).toUInt(nullptr, 8);

    if (!rxOpen(rx, name, size, mtime, &m_rxError)) {
        sendCancel();
        return RxFail;
    }

    return sendPos(ZRPOS, 0) ? RxContinue : RxFail;
}

int Zmodem::Private::rxZdata(Rx &rx)
{
    if (!rx.open) {
        sendRinit();
        return RxContinue;
    }
    if (headerPosition(m_hdr) != rx.pos) {
        sendPos(ZRPOS, rx.pos);
        return RxContinue;
    }

    for (;;) {
        int len = 0;
        const int end = recvData(m_buf.data(), m_buf.size(), &len);
        if (end == RCancel)
            return RxFail;
        if (end == RAbort) {
            sendCancel();
            return RxFail;
        }
        if (end < 0) {
            sendPos(ZRPOS, rx.pos);
            return RxContinue;
        }
        if (len > 0) {
            if (!rxWrite(rx, rx.pos, m_buf.constData(), len, &m_rxError)) {
                sendCancel();
                return RxFail;
            }
        }
        if (end == ZCRCW) {
            sendPos(ZACK, rx.pos);
            return RxContinue;
        }
        if (end == ZCRCQ) {
            sendPos(ZACK, rx.pos);
            continue;
        }
        if (end == ZCRCE)
            return RxContinue;
        // ZCRCG: 続く
    }
}

int Zmodem::Private::rxZeof(Rx &rx)
{
    if (!rx.open) {
        sendRinit();
        return RxContinue;
    }
    if (headerPosition(m_hdr) != rx.pos) {
        sendPos(ZRPOS, rx.pos);
        return RxContinue;
    }

    if (!rxClose(rx, rx.pos, &m_rxError))
        return RxFail;
    return sendRinit() ? RxContinue : RxFail;
}

int Zmodem::Private::rxZfin(Rx &rx)
{
    sendPos(ZFIN, 0);
    for (int i = 0; i < 2; ++i) {
        if (rd(500) < 0)
            break;
    }
    if (rx.files <= 0)
        return RxFail;
    return RxDone;
}

int Zmodem::Private::rxHeader(Rx &rx, int type)
{
    switch (type) {
    case ZRQINIT:
        return sendRinit() ? RxContinue : RxFail;
    case ZSINIT: {
        int len = 0;
        if (recvData(m_buf.data(), m_buf.size(), &len) < 0) {
            sendNak();
            return RxContinue;
        }
        Q_UNUSED(len);
        return sendPos(ZACK, 1) ? RxContinue : RxFail;
    }
    case ZFILE:
        return rxZfile(rx);
    case ZDATA:
        return rxZdata(rx);
    case ZEOF:
        return rxZeof(rx);
    case ZFIN:
        return rxZfin(rx);
    case ZCAN:
    case ZABORT:
        sendPos(ZFIN, 0);
        return RxFail;
    case ZFREECNT:
        return sendPos(ZACK, 0x7FFFFFFFu) ? RxContinue : RxFail;
    case ZCOMMAND:
        return sendPos(ZCOMPL, 0) ? RxContinue : RxFail;
    default:
        return RxContinue;
    }
}

bool Zmodem::Private::receiveFiles(const QString &localDir, QStringList *received, QString *error)
{
    if (!m_io) {
        setError(error, QStringLiteral("ZMODEM の読み書きの口がありません"));
        return false;
    }
    const QDir dir(localDir);
    if (!dir.exists()) {
        setError(error, QStringLiteral("受け取り先のフォルダがありません: %1").arg(localDir));
        return false;
    }

    Rx rx;
    rx.localDir = dir.absolutePath();
    rx.received = received;
    m_rxError.clear();

    if (!sendRinit()) {
        setError(error, QStringLiteral("ZMODEM を始められませんでした"));
        return false;
    }

    int tries = 0;
    for (;;) {
        const int type = recvHdr(rx.open ? kHdrTimeoutMs : 3000);
        if (type == RCancel) {
            rxDiscard(rx);
            setError(error, QStringLiteral("受け取りを中止しました"));
            return false;
        }
        if (type == RAbort) {
            rxDiscard(rx);
            setError(error, QStringLiteral("送り手が ZMODEM を中止しました"));
            return false;
        }
        if (type == RTimeout || type == RError) {
            if (!m_io->errorString().isEmpty()) {
                rxDiscard(rx);
                setError(error, QStringLiteral("ZMODEM の通信が切れました"));
                return false;
            }
            if (++tries > (rx.open ? kMaxDataTries : 20)) {
                rxDiscard(rx);
                setError(error, rx.open
                                    ? QStringLiteral("%1 を受け取れませんでした").arg(rx.name)
                                    : QStringLiteral("sgnano が ZMODEM を始めませんでした"));
                return false;
            }
            if (rx.open)
                sendPos(ZRPOS, rx.pos);
            else
                sendRinit();
            continue;
        }
        tries = 0;

        const int result = rxHeader(rx, type);
        if (result == RxFail) {
            rxDiscard(rx);
            setError(error, m_rxError.isEmpty()
                                ? (rx.files > 0
                                       ? QStringLiteral("受け取りの途中で失敗しました")
                                       : QStringLiteral("ZMODEM を受け取れませんでした"))
                                : m_rxError);
            return false;
        }
        if (result == RxDone) {
            if (rx.files <= 0) {
                setError(error, QStringLiteral("ファイルを受け取れませんでした"));
                return false;
            }
            return true;
        }
    }
}

Zmodem::Zmodem(ZmodemIo *io, Progress progress)
    : d(std::make_unique<Private>(io, std::move(progress)))
{
}

Zmodem::~Zmodem() = default;

bool Zmodem::sendFiles(const QStringList &paths, QString *error)
{
    return d->sendFiles(paths, error);
}

bool Zmodem::receiveFiles(const QString &localDir, QStringList *received, QString *error)
{
    return d->receiveFiles(localDir, received, error);
}
