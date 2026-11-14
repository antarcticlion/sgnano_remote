#pragma once

// Windows の COM ポート（2026-11-15、Windows 版のため）。
// 通常の遠隔コマンドは SerialLink の読み取りスレッドから、ZMODEM は ZmodemSerialIo から使う。
// Linux ではこのクラスは組み込まれない（seriallink.cpp / zmodemworker.cpp の POSIX の道を使う）。

#include <QMutex>
#include <QString>
#include <QStringList>

#ifdef Q_OS_WIN

#include <windows.h>

class Win32SerialPort
{
public:
    Win32SerialPort();
    ~Win32SerialPort();

    bool open(const QString &port, int baud, QString *error);
    void close();
    bool isOpen() const;

    // timeoutMs = 0 なら、いま届いている分だけを読む。0 は時間切れ、負は誤り
    int read(char *data, int maxSize, int timeoutMs, QString *error);
    int write(const char *data, int size, int timeoutMs, QString *error);
    bool flush(QString *error);
    int bytesAvailable();

    static QStringList availablePorts();
    static QString defaultPort();

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
    mutable QMutex m_mutex;
};

#endif
