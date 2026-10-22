#pragma once

#include <QByteArray>
#include <QMainWindow>
#include <QProcess>
#include <QStringList>

class QCloseEvent;
class QLabel;
class QPlainTextEdit;
class QSplitter;
class QProgressDialog;
class LocalPane;
class RemotePane;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void appendLog(const QString &message);
    void updateRemoteStatus(bool connected, const QString &description);
    void startUpload();
    void startDownload();
    void cancelTransfer();
    void finishTransfer(int exitCode, QProcess::ExitStatus status);

private:
    void buildUi();
    void startUploadTo(const QStringList &files, const QString &remoteDir);
    void startDownloadFrom(const QString &remoteFile, const QString &localDir);
    void launchTransfer(bool download, const QStringList &files, const QString &localDir,
                        const QString &remotePath);
    void consumeTransferOutput(const QByteArray &data);
    void handleTransferOutputLine(const QString &line);
    void flushTransferOutput();

    QSplitter *m_splitter = nullptr;
    LocalPane *m_localPane = nullptr;
    RemotePane *m_remotePane = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QLabel *m_localStatus = nullptr;
    QLabel *m_remoteStatus = nullptr;
    QProcess *m_zmodem = nullptr;
    QProgressDialog *m_transferProgress = nullptr;
    QString m_transferPort;
    QString m_transferRefreshPath;
    QString m_transferProgressBaseText;
    QByteArray m_transferOutputBuffer;
    int m_transferBaud = 921600;
    bool m_transferDownload = false;
    bool m_transferCanceled = false;
    bool m_transferGroup = false;
};
