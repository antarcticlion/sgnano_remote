#include "win32serial.h"

#ifdef Q_OS_WIN

#include <QSettings>

#include <algorithm>

namespace {

QString windowsError(DWORD code)
{
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    QString text;
    if (length && buffer)
        text = QString::fromWCharArray(buffer, int(length)).trimmed();
    if (buffer)
        LocalFree(buffer);

    if (text.isEmpty())
        text = QStringLiteral("Windows error %1").arg(code);
    return text;
}

bool isComPort(const QString &name)
{
    return name.size() > 3 && name.left(3).compare(QStringLiteral("COM"), Qt::CaseInsensitive) == 0;
}

}

Win32SerialPort::Win32SerialPort() = default;

Win32SerialPort::~Win32SerialPort()
{
    close();
}

bool Win32SerialPort::open(const QString &port, int baud, QString *error)
{
    close();

    if (port.isEmpty() || baud <= 0) {
        if (error)
            *error = QStringLiteral("COM ポート名またはボーレートが正しくありません");
        return false;
    }

    QString path = port;
    if (!path.startsWith(QStringLiteral("\\\\.\\")))
        path.prepend(QStringLiteral("\\\\.\\"));

    const HANDLE handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()),
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (error)
            *error = QStringLiteral("%1 を開けません: %2").arg(port, windowsError(GetLastError()));
        return false;
    }

    (void)SetupComm(handle, 64u * 1024u, 64u * 1024u);

    DCB dcb {};
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(handle, &dcb)) {
        const QString message = QStringLiteral("シリアル設定を読めません: %1").arg(windowsError(GetLastError()));
        CloseHandle(handle);
        if (error)
            *error = message;
        return false;
    }

    dcb.BaudRate = DWORD(baud);
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fAbortOnError = FALSE;

    if (!SetCommState(handle, &dcb)) {
        const QString message = QStringLiteral("シリアル設定を反映できません: %1").arg(windowsError(GetLastError()));
        CloseHandle(handle);
        if (error)
            *error = message;
        return false;
    }

    COMMTIMEOUTS timeouts {};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 100;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 1000;
    if (!SetCommTimeouts(handle, &timeouts)) {
        const QString message = QStringLiteral("シリアル設定（時間切れ）を反映できません: %1").arg(windowsError(GetLastError()));
        CloseHandle(handle);
        if (error)
            *error = message;
        return false;
    }

    (void)PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);

    QMutexLocker locker(&m_mutex);
    m_handle = handle;
    return true;
}

void Win32SerialPort::close()
{
    QMutexLocker locker(&m_mutex);
    if (m_handle != INVALID_HANDLE_VALUE) {
        // 読み取りは最大 100ms で戻るので、ここで閉じてよい
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
}

bool Win32SerialPort::isOpen() const
{
    QMutexLocker locker(&m_mutex);
    return m_handle != INVALID_HANDLE_VALUE;
}

int Win32SerialPort::read(char *data, int maxSize, int timeoutMs, QString *error)
{
    if (!data || maxSize <= 0)
        return 0;

    QMutexLocker locker(&m_mutex);
    if (m_handle == INVALID_HANDLE_VALUE) {
        if (error)
            *error = QStringLiteral("シリアルポートが開かれていません");
        return -1;
    }

    COMMTIMEOUTS timeouts {};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = timeoutMs > 0 ? DWORD(timeoutMs) : 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 1000;
    if (!SetCommTimeouts(m_handle, &timeouts)) {
        if (error)
            *error = QStringLiteral("受信の時間切れを設定できません: %1").arg(windowsError(GetLastError()));
        return -1;
    }

    DWORD got = 0;
    if (!ReadFile(m_handle, data, DWORD(maxSize), &got, nullptr)) {
        const DWORD code = GetLastError();
        if (code == ERROR_OPERATION_ABORTED || code == ERROR_INVALID_HANDLE)
            return 0;
        if (error)
            *error = QStringLiteral("受信に失敗しました: %1").arg(windowsError(code));
        return -1;
    }
    return int(got);
}

int Win32SerialPort::write(const char *data, int size, int timeoutMs, QString *error)
{
    if (!data || size <= 0)
        return 0;

    QMutexLocker locker(&m_mutex);
    if (m_handle == INVALID_HANDLE_VALUE) {
        if (error)
            *error = QStringLiteral("シリアルポートが開かれていません");
        return -1;
    }

    COMMTIMEOUTS timeouts {};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 100;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = timeoutMs > 0 ? DWORD(timeoutMs) : 1000;
    if (!SetCommTimeouts(m_handle, &timeouts)) {
        if (error)
            *error = QStringLiteral("送信の時間切れを設定できません: %1").arg(windowsError(GetLastError()));
        return -1;
    }

    int total = 0;
    while (total < size) {
        DWORD wrote = 0;
        if (!WriteFile(m_handle, data + total, DWORD(size - total), &wrote, nullptr)) {
            if (error)
                *error = QStringLiteral("送信に失敗しました: %1").arg(windowsError(GetLastError()));
            return -1;
        }
        if (wrote == 0) {
            if (error)
                *error = QStringLiteral("送信が止まりました");
            return -1;
        }
        total += int(wrote);
    }
    return total;
}

bool Win32SerialPort::flush(QString *error)
{
    QMutexLocker locker(&m_mutex);
    if (m_handle == INVALID_HANDLE_VALUE) {
        if (error)
            *error = QStringLiteral("シリアルポートが開かれていません");
        return false;
    }
    if (!FlushFileBuffers(m_handle)) {
        if (error)
            *error = QStringLiteral("送信を終えられません: %1").arg(windowsError(GetLastError()));
        return false;
    }
    return true;
}

int Win32SerialPort::bytesAvailable()
{
    QMutexLocker locker(&m_mutex);
    if (m_handle == INVALID_HANDLE_VALUE)
        return 0;

    COMSTAT status {};
    DWORD errors = 0;
    if (!ClearCommError(m_handle, &errors, &status))
        return 0;
    return int(status.cbInQue);
}

QStringList Win32SerialPort::availablePorts()
{
    QStringList ports;

    // つながっている COM ポートは、レジストリの SERIALCOMM に並ぶ
    QSettings registry(QStringLiteral("HKEY_LOCAL_MACHINE\\HARDWARE\\DEVICEMAP\\SERIALCOMM"),
                       QSettings::NativeFormat);
    const QStringList keys = registry.allKeys();
    for (const QString &key : keys) {
        const QString port = registry.value(key).toString().trimmed();
        if (isComPort(port) && !ports.contains(port))
            ports.append(port);
    }

    std::sort(ports.begin(), ports.end(), [](const QString &left, const QString &right) {
        return left.mid(3).toInt() < right.mid(3).toInt();
    });
    return ports;
}

QString Win32SerialPort::defaultPort()
{
    const QStringList ports = availablePorts();
    return ports.isEmpty() ? QStringLiteral("COM3") : ports.first();
}

#endif
