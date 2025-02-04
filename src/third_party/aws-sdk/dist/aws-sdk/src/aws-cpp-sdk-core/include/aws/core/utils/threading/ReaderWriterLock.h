/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/threading/Semaphore.h>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <cstdint>

namespace Aws
{
    namespace Utils
    {
        namespace Threading
        {
            /**
             * This lock is optimized for frequent reads and infrequent writes.
             * However, writers get priority to the lock.
             */
            class AWS_CORE_API ReaderWriterLock
            {
            public:
                ReaderWriterLock();
                /**
                 * Enters the lock in Reader-mode.
                 * This call blocks until no writers are acquiring the lock.
                 */
                void LockReader();

                /**
                 * Decrements the readers count by one and if the count is zero, signals any waiting writers to acquire
                 * the lock.
                 * NOTE: Calling this function without a matching LockReader results in undefined behavior.
                 */
                void UnlockReader();

                /**
                 * Enters the lock in Writer-mode.
                 * This call blocks until no readers nor writers are acquiring the lock.
                 */
                void LockWriter();

                /**
                 * Decrements the number of writers by one and signals any waiting readers or writers to acquire the
                 * lock.
                 * NOTE: Calling this function without a matching LockWriter results in undefined behavior.
                 */
                void UnlockWriter();
            private:
                std::atomic<int64_t> m_readers;
                std::atomic<int64_t> m_holdouts;
                Semaphore m_readerSem;
                Semaphore m_writerSem;
                std::mutex m_writerLock;
            };

            class AWS_CORE_API ReaderLockGuard
            {
            public:
                explicit ReaderLockGuard(ReaderWriterLock& rwl) : m_rwlock(rwl), m_upgraded(false)
                {
                    m_rwlock.LockReader();
                }

                void UpgradeToWriterLock()
                {
                    m_rwlock.UnlockReader();
                    m_rwlock.LockWriter();
                    m_upgraded = true;
                }

                ~ReaderLockGuard()
                {
                    if(m_upgraded)
                    {
                        m_rwlock.UnlockWriter();
                    }
                    else
                    {
                        m_rwlock.UnlockReader();
                    }
                }
                // for VS2013
                ReaderLockGuard(const ReaderLockGuard&) = delete;
                ReaderLockGuard& operator=(const ReaderLockGuard&) = delete;
            private:
                ReaderWriterLock& m_rwlock;
                bool m_upgraded;
            };

            class AWS_CORE_API WriterLockGuard
            {
            public:
                explicit WriterLockGuard(ReaderWriterLock& rwl) : m_rwlock(rwl)
                {
                    m_rwlock.LockWriter();
                }

                ~WriterLockGuard()
                {
                    m_rwlock.UnlockWriter();
                }
                // for VS2013
                WriterLockGuard(const WriterLockGuard&) = delete;
                WriterLockGuard& operator=(const WriterLockGuard&) = delete;
            private:
                ReaderWriterLock& m_rwlock;
            };
        }
    }
}
