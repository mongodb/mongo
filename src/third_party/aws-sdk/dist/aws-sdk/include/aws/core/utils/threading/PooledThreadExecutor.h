/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/utils/threading/Executor.h>

#include <aws/core/utils/memory/stl/AWSQueue.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/threading/Semaphore.h>
#include <functional>
#include <mutex>
#include <atomic>

namespace Aws
{
    namespace Utils
    {
        namespace Threading
        {
            class ThreadTask;

            enum class OverflowPolicy
            {
                QUEUE_TASKS_EVENLY_ACROSS_THREADS,
                REJECT_IMMEDIATELY
            };

            /**
            * Thread Pool Executor implementation.
            */
            class AWS_CORE_API PooledThreadExecutor : public Executor
            {
            public:
                PooledThreadExecutor(size_t poolSize, OverflowPolicy overflowPolicy = OverflowPolicy::QUEUE_TASKS_EVENLY_ACROSS_THREADS);
                ~PooledThreadExecutor();

                /**
                * Rule of 5 stuff.
                * Don't copy or move
                */
                PooledThreadExecutor(const PooledThreadExecutor&) = delete;
                PooledThreadExecutor& operator =(const PooledThreadExecutor&) = delete;
                PooledThreadExecutor(PooledThreadExecutor&&) = delete;
                PooledThreadExecutor& operator =(PooledThreadExecutor&&) = delete;

                /**
                * Call to ensure the threadpool can be safely destroyed. It blocks until all threads finished.
                */
                void WaitUntilStopped() override;

            protected:
                bool SubmitToThread(std::function<void()>&&) override;

            private:
                Aws::Queue<std::function<void()>*> m_tasks;
                mutable std::mutex m_queueLock;
                Aws::Utils::Threading::Semaphore m_sync;
                Aws::Vector<ThreadTask*> m_threadTaskHandles;
                size_t m_poolSize = 0;
                OverflowPolicy m_overflowPolicy = OverflowPolicy::QUEUE_TASKS_EVENLY_ACROSS_THREADS;
                bool m_stopped{false};

                /**
                 * Once you call this, you are responsible for freeing the memory pointed to by task.
                 */
                std::function<void()>* PopTask();
                bool HasTasks() const;

                friend class ThreadTask;
            };
        } // namespace Threading
    } // namespace Utils
} // namespace Aws
