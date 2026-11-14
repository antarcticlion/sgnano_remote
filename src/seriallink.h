#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#ifdef Q_OS_WIN
#include <QMutex>

#include <memory>
#endif

class QSocketNotifier;
class QTimer;
class Win32SerialPort;

class SerialLink : public QObject
{
    Q_OBJECT

public:
    explicit SerialLink(QObject *parent = nullptr);
    ~SerialLink() override;

    bool open(const QString &port, int baud, QString *error = nullptr);
    void close();
    bool isOpen() const;
    QString portName() const;
    int baudRate() const;
    qint64 bytesToWrite() const;

    qint64 write(const QByteArray &data);

signals:
    void opened(const QString &port, int baud);
    void closed();
    void dataReceived(const QByteArray &data);
    void errorOccurred(const QString &message);

private slots:
    void readReady();
    void writeReady();

private:
    void flushTx();
    void fail(const QString &message);

    int m_fd = -1;
    QString m_port;
    int m_baud = 921600;
    QSocketNotifier *m_readNotifier = nullptr;
    QSocketNotifier *m_writeNotifier = nullptr;
    QByteArray m_tx;

#ifdef Q_OS_WIN
    std::unique_ptr<Win32SerialPort> m_winPort;
    QTimer *m_winTimer = nullptr;
    mutable QMutex m_ioMutex;
#endif
};
