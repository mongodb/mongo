/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/threading/SameThreadExecutor.h>
#include <aws/core/utils/threading/ThreadTask.h>

#include <cassert>

using namespace Aws::Utils::Threading;

bool SameThreadExecutor::SubmitToThread(std::function<void()>&& task)
{
    m_tasks.push_back(std::move(task));
    return true;
}

SameThreadExecutor::~SameThreadExecutor()
{
    SameThreadExecutor::WaitUntilStopped();
}

void SameThreadExecutor::WaitUntilStopped()
{
    while(!m_tasks.empty())
    {
        auto task = std::move(m_tasks.front());
        m_tasks.pop_front();
        assert(task);
        if(task) {
            task();
        }
    }
}