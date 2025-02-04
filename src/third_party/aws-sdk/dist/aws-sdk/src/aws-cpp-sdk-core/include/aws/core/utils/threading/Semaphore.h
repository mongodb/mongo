/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <mutex>
#include <condition_variable>

namespace Aws
{
    namespace Utils
    {
        namespace Threading
        {
            class AWS_CORE_API Semaphore {
                public:
                    /**
                     * Initializes a new instance of Semaphore class specifying the initial number of entries and 
                     * the maximum number of concurrent entries.
                     */
                    Semaphore(size_t initialCount, size_t maxCount);
                    /**
                     * Blocks the current thread until it receives a signal.
                     */
                    void WaitOne();
                    /**
                     * Blocks the current thread until it receives a signal or timeout is reached.
                     * Returns false on timeout, and true if a signal is received
                     */
                    bool WaitOneFor(size_t timeoutMs);
                    /**
                     * Exits the semaphore once.
                     */
                    void Release();
                    /**
                     * Exit the semaphore up to the maximum number of entries available.
                     */
                    void ReleaseAll();
                private:
                    size_t m_count;
                    const size_t m_maxCount;
                    std::mutex m_mutex;
                    std::condition_variable m_syncPoint;
            };
        }
    }
}
