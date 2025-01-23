/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/utils/threading/Executor.h>

#include <aws/core/utils/memory/stl/AWSMap.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace Aws
{
    namespace Utils
    {
        namespace Threading
        {
            /**
            * Default Executor implementation. Simply spawns a thread and detaches it.
            */
            class AWS_CORE_API DefaultExecutor : public Executor
            {
            public:
                DefaultExecutor() : m_state(State::Free) {}
                ~DefaultExecutor();

                void WaitUntilStopped() override;
            protected:
                enum class State
                {
                    Free, Locked, Shutdown
                };
                bool SubmitToThread(std::function<void()>&&) override;
                void Detach(std::thread::id id);
                std::atomic<State> m_state;
                Aws::UnorderedMap<std::thread::id, std::thread> m_threads;
            };
        } // namespace Threading
    } // namespace Utils
} // namespace Aws
