/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/threading/ReaderWriterLock.h>
#include <cstdint>
#include <limits>
#include <cassert>

using namespace Aws::Utils::Threading;

static const int64_t MaxReaders = (std::numeric_limits<std::int32_t>::max)();

ReaderWriterLock::ReaderWriterLock() :
    m_readers(0),
    m_holdouts(0),
    m_readerSem(0, static_cast<size_t>(MaxReaders)),
    m_writerSem(0, 1)
{
}

void ReaderWriterLock::LockReader()
{
    if (++m_readers < 0)
    {
        m_readerSem.WaitOne();
    }
}

void ReaderWriterLock::UnlockReader()
{
    if (--m_readers < 0 && --m_holdouts == 0)
    {
        m_writerSem.Release();
    }
}

void ReaderWriterLock::LockWriter()
{
    m_writerLock.lock();
    if(const auto current = m_readers.fetch_sub(MaxReaders))
    {
        assert(current > 0);
        const auto holdouts = m_holdouts.fetch_add(current) + current;
        assert(holdouts >= 0);
        if(holdouts > 0)
        {
            m_writerSem.WaitOne();
        }
    }
}

void ReaderWriterLock::UnlockWriter()
{
    assert(m_holdouts == 0);
    const auto current = m_readers.fetch_add(MaxReaders) + MaxReaders;
    assert(current >= 0);
    for(int64_t r = 0; r < current; r++)
    {
        m_readerSem.Release();
    }
    m_writerLock.unlock();
}
