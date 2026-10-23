#include "remotedeletequeue.h"

RemoteDeleteQueue::RemoteDeleteQueue(QObject *parent)
    : QObject(parent)
{
}

void RemoteDeleteQueue::start(const QStringList &paths)
{
    reset();
    if (paths.isEmpty())
        return;

    m_paths = paths;
    m_running = true;
    pump();
}

void RemoteDeleteQueue::operationFinished()
{
    if (!m_running || m_pending == 0)
        return;

    --m_pending;
    if (m_pending > 0)
        return;

    if (m_index < m_paths.size()) {
        pump();
        return;
    }

    // pump の中から呼ばれたときは、あちらでまとめて終わりにする
    if (!m_pumping)
        finish();
}

void RemoteDeleteQueue::abortRemaining()
{
    m_running = false;
    m_paths.clear();
    m_index = 0;
    m_pending = 0;
}

void RemoteDeleteQueue::reset()
{
    m_running = false;
    m_paths.clear();
    m_index = 0;
    m_pending = 0;
    // m_pumping は pump の実行中だけ true。ここでは触らない（pump の中から reset されても二重に走らない）
}

bool RemoteDeleteQueue::isActive() const
{
    return m_running;
}

bool RemoteDeleteQueue::hasRemaining() const
{
    return m_running && m_index < m_paths.size();
}

int RemoteDeleteQueue::pending() const
{
    return m_pending;
}

int RemoteDeleteQueue::remaining() const
{
    return m_running ? int(m_paths.size()) - m_index : 0;
}

int RemoteDeleteQueue::total() const
{
    return int(m_paths.size());
}

void RemoteDeleteQueue::pump()
{
    if (m_pumping)
        return;

    // 返事を待たずに詰め込むと RemoteProtocol の待ち行列（32）があふれるので、少しずつ送る
    m_pumping = true;
    while (m_running && m_index < m_paths.size() && m_pending < kBatchSize) {
        const QString path = m_paths.at(m_index++);
        ++m_pending;
        emit deleteRequested(path);
    }
    m_pumping = false;

    if (m_running && m_pending == 0 && m_index >= m_paths.size())
        finish();
}

void RemoteDeleteQueue::finish()
{
    if (!m_running)
        return;
    m_running = false;
    emit finished();
}
