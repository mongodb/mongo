/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/threading/Semaphore.h>
#include <algorithm>

using namespace Aws::Utils::Threading;

Semaphore::Semaphore(size_t initialCount, size_t maxCount)
    : m_count(initialCount), m_maxCount(maxCount)
{
}

void Semaphore::WaitOne()
{
    std::unique_lock<std::mutex> locker(m_mutex);
    if(0 == m_count)
    {
        m_syncPoint.wait(locker, [this] { return m_count > 0; });
    }
    --m_count;
}

bool Semaphore::WaitOneFor(size_t timeoutMs)
{
    std::unique_lock<std::mutex> locker(m_mutex);
    if(0 == m_count)
    {
        if(!m_syncPoint.wait_for(locker, std::chrono::milliseconds(timeoutMs), [this] { return m_count > 0; }))
        {
            return false; // timeout was reached
        }
    }
    --m_count;
    return true;
}

void Semaphore::Release()
{
    std::lock_guard<std::mutex> locker(m_mutex);
    m_count = (std::min)(m_maxCount, m_count + 1);
    m_syncPoint.notify_one();
}

void Semaphore::ReleaseAll()
{    
    std::lock_guard<std::mutex> locker(m_mutex);
    m_count = m_maxCount;
    m_syncPoint.notify_all();
}

