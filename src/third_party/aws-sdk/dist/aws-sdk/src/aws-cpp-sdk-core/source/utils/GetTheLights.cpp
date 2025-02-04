/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/core/utils/GetTheLights.h>
#include <cassert>

namespace Aws
{
    namespace Utils
    {
        GetTheLights::GetTheLights() : m_value(0)
        {
        }

        void GetTheLights::EnterRoom(std::function<void()> &&callable)
        {
            int cpy = ++m_value;
            assert(cpy > 0);
            if(cpy == 1)
            {
                callable();
            }
        }

        void GetTheLights::LeaveRoom(std::function<void()> &&callable)
        {
            int cpy = --m_value;
            assert(cpy >= 0);
            if(cpy == 0)
            {
                callable();
            }
        }
    }
}