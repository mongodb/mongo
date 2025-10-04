/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/threading/DefaultExecutor.h>
#include <aws/core/utils/threading/ThreadTask.h>

#include <cassert>

using namespace Aws::Utils::Threading;

bool DefaultExecutor::SubmitToThread(std::function<void()>&&  fx)
{
    // Generalized lambda capture is C++14, using std::bind as a workaround to force moving fx (instead of copying)
    std::function<void()> main = std::bind(
            [this](std::function<void()>& storedFx)
            {
                storedFx();
                Detach(std::this_thread::get_id());
            },
            std::move(fx)
        );

    State expected;
    do
    {
        expected = State::Free;
        if(m_state.compare_exchange_strong(expected, State::Locked))
        {
            std::thread t(std::move(main));
            const auto id = t.get_id(); // copy the id before we std::move the thread
            m_threads.emplace(id, std::move(t));
            m_state = State::Free;
            return true;
        }
    }
    while(expected != State::Shutdown);
    return false;
}

void DefaultExecutor::Detach(std::thread::id id)
{
    State expected;
    do
    {
        expected = State::Free;
        if(m_state.compare_exchange_strong(expected, State::Locked))
        {
            auto it = m_threads.find(id);
            assert(it != m_threads.end());
            it->second.detach();
            m_threads.erase(it);
            m_state = State::Free;
            return;
        }
    } 
    while(expected != State::Shutdown);
}

void DefaultExecutor::WaitUntilStopped()
{
    auto expected = State::Free;
    while(!m_state.compare_exchange_strong(expected, State::Shutdown))
    {
        //spin while currently detaching threads finish
        assert(expected == State::Locked);
        expected = State::Free;
    }
}

DefaultExecutor::~DefaultExecutor()
{
    WaitUntilStopped();

    auto it = m_threads.begin();
    while(!m_threads.empty())
    {
        it->second.join();
        it = m_threads.erase(it);
    }
}
