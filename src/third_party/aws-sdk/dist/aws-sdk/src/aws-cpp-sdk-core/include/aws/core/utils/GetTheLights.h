/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStack.h>
#include <functional>
#include <atomic>

namespace Aws
{
    namespace Utils
    {
        /**
         * Make initialization and cleanup of shared resources less painful.
         * If you have this problem. Create a static instance of GetTheLights,
         * have each actor call Enter the room with your callable.
         *
         * When you are finished with the shared resources call LeaveRoom(). The last caller will
         * have its callable executed.
         */
        class AWS_CORE_API GetTheLights
        {
        public:
            GetTheLights();
            void EnterRoom(std::function<void()>&&);
            void LeaveRoom(std::function<void()>&&);
        private:
            std::atomic<int> m_value;
        };
    }
}