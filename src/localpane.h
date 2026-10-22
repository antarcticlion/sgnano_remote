#pragma once

#include <QStringList>
#include <QWidget>

class QFileSystemModel;
class QLabel;
class QLineEdit;
class QSortFilterProxyModel;
class QTreeView;

class LocalPane : public QWidget
{
    Q_OBJECT

public:
    explicit LocalPane(QWidget *parent = nullptr);

    QString currentPath() const;
    QStringList selectedFiles() const;
    void refresh();

signals:
    void pathChanged(const QString &path);
    void remoteFilesDropped(const QStringList &remotePaths);

private slots:
    void goUp();
    void goHome();
    void navigateToPath();
    void openIndex(const QModelIndex &index);
    void refreshSummary();

private:
    void setCurrentDir(const QString &path);

    QFileSystemModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    QTreeView *m_view = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QLabel *m_summary = nullptr;
    QString m_currentPath;
};
