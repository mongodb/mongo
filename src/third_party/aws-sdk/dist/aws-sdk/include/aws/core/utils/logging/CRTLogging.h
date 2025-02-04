/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <memory>

namespace Aws
{
    namespace Utils
    {
        namespace Logging
        {
            class CRTLogSystemInterface;

            /**
             * Initialize CRT (common runtime libraries) log system to handle loggings from common runtime libraries, including aws-c-auth, aws-c-http, aws-c-event-stream and etc.
             */
            AWS_CORE_API void InitializeCRTLogging(const std::shared_ptr<CRTLogSystemInterface>& crtLogSystem);

            /**
             * Shutdown CRT (common runtime libraries) log system.
             */
            AWS_CORE_API void ShutdownCRTLogging();

        } // namespace Logging
    } // namespace Utils
} // namespace Aws
