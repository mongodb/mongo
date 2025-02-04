/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/utils/threading/Executor.h>

#include <aws/core/utils/memory/stl/AWSList.h>
#include <functional>

namespace Aws
{
    namespace Utils
    {
        namespace Threading
        {
            /**
            * An executor that does not spawn any thread, instead, tasks are executed in the current thread
            * TODO: add await functionality to avoid deadlocking if thread waits for another async task.
            */
            class AWS_CORE_API SameThreadExecutor : public Executor
            {
            public:
                virtual ~SameThreadExecutor();
                void WaitUntilStopped() override;
            protected:
                bool SubmitToThread(std::function<void()>&& task) override;

                using TaskFunc = std::function<void()>;
                Aws::List<TaskFunc> m_tasks;
            };
        } // namespace Threading
    } // namespace Utils
} // namespace Aws
