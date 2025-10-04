/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/RAIICounter.h>

#include <assert.h>
#include <limits>

namespace Aws
{
    namespace Utils
    {
        RAIICounter::RAIICounter(std::atomic<size_t>& iCount, std::condition_variable* cv)
          : m_count(iCount),
            m_cv(cv)
        {
            assert(m_count != std::numeric_limits<size_t>::max());
            m_count++;
        }

        RAIICounter::~RAIICounter()
        {
            assert(m_count > 0);
            m_count--;
            if(m_cv && m_count == 0)
            {
                m_cv->notify_all();
            }
        }

    }
}
