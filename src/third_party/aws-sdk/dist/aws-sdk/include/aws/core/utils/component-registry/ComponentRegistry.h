
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>

namespace Aws
{
    namespace Utils
    {
        /**
         * A helper utility set of methods to track currently allocated components (i.e. AWS SDK service clients)
         */
        namespace ComponentRegistry
        {
            /**
             * A callback method type alias to terminate component.
             */
            typedef void (*ComponentTerminateFn)(void* pClient, int64_t timeoutMs);

            /**
             * Initialize a component registry (i.e. init global dictionary).
             */
            AWS_CORE_API void InitComponentRegistry();
            /**
             * Shutdown a component registry (i.e. release global dictionary).
             */
            AWS_CORE_API void ShutdownComponentRegistry();
            /**
             * Register component (i.e. AWS SDK service client) as active.
             */
            AWS_CORE_API void RegisterComponent(const char* clientName, void* pClient, ComponentTerminateFn terminateMethod);
            /**
             * Remove component from a registry.
             */
            AWS_CORE_API void DeRegisterComponent(void* pClient);
            /**
             * Terminate all registered clients.
             */
            AWS_CORE_API void TerminateAllComponents();
        }
    }
}
