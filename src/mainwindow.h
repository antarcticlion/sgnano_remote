#pragma once

#include <QMainWindow>
#include <QStringList>

class QCloseEvent;
class QLabel;
class QPlainTextEdit;
class QSplitter;
class QProgressDialog;
class LocalPane;
class RemotePane;
class ZmodemWorker;

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
    void finishTransfer(bool ok, bool canceled, const QString &error);
    void onTransferProgress(qint64 done, qint64 total);

private:
    void buildUi();
    void startUploadTo(const QStringList &files, const QString &remoteDir);
    void startDownloadFrom(const QString &remoteFile, const QString &localDir);
    void launchTransfer(bool download, const QStringList &files, const QString &localDir,
                        const QString &remotePath);
    QSplitter *m_splitter = nullptr;
    LocalPane *m_localPane = nullptr;
    RemotePane *m_remotePane = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QLabel *m_localStatus = nullptr;
    QLabel *m_remoteStatus = nullptr;
    ZmodemWorker *m_transfer = nullptr;
    QProgressDialog *m_transferProgress = nullptr;
    QString m_transferRefreshPath;
    QString m_transferProgressBaseText;
    bool m_transferDownload = false;
    bool m_transferCanceled = false;
};
