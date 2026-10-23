#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

// 一括削除の送信順を管理する（2026-10-23）。
// RemotePane から切り離しておき、8 件ずつの送信・途中失敗・タイムアウトを実機なしで試せるようにする。
class RemoteDeleteQueue : public QObject
{
    Q_OBJECT

public:
    // 一度に送る数。RemoteProtocol の待ち行列（32）をあふれさせないため、小さめにする
    static constexpr int kBatchSize = 8;

    explicit RemoteDeleteQueue(QObject *parent = nullptr);

    void start(const QStringList &paths);
    void operationFinished();    // DEL の OK / ERR が 1 つ返るたびに呼ぶ
    void abortRemaining();       // タイムアウトなどで、送っていない分をやめる（finished は出さない）
    void reset();

    bool isActive() const;       // start してから、全部の返事が終わるまで
    bool hasRemaining() const;   // まだ送っていないパスがある
    int pending() const;         // 送信済みで返事待ちの数
    int remaining() const;       // まだ送っていない数
    int total() const;

signals:
    void deleteRequested(const QString &path);
    void finished();

private:
    void pump();
    void finish();

    QStringList m_paths;
    int m_index = 0;
    int m_pending = 0;
    bool m_running = false;
    bool m_pumping = false;
};
