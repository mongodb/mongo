/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <functional>
#include <thread>
#include <atomic>

namespace Aws
{
    namespace Utils
    {
        namespace Threading
        {
            class PooledThreadExecutor;

            class AWS_CORE_API ThreadTask
            {
            public:
                ThreadTask(PooledThreadExecutor& executor);
                ~ThreadTask();

                /**
                * Rule of 5 stuff.
                * Don't copy or move
                */
                ThreadTask(const ThreadTask&) = delete;
                ThreadTask& operator =(const ThreadTask&) = delete;
                ThreadTask(ThreadTask&&) = delete;
                ThreadTask& operator =(ThreadTask&&) = delete;

                void StopProcessingWork();                

            protected:
                void MainTaskRunner();

            private:                
                std::atomic<bool> m_continue;
                PooledThreadExecutor& m_executor;
                std::thread m_thread;
            };
        }
    }
}
