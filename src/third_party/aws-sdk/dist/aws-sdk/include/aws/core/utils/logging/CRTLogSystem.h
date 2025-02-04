/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/logging/LogLevel.h>
#include <aws/common/logging.h>

#include <atomic>

namespace Aws
{
    namespace Utils
    {
        namespace Logging
        {
            enum class LogLevel : int;

            /**
             * Interface for CRT (common runtime libraries) logging implementations.
             * A wrapper on the top of aws_logger, the logging interface used by common runtime libraries.
             */
            class AWS_CORE_API CRTLogSystemInterface
            {
            public:
                virtual ~CRTLogSystemInterface() = default;

                /**
                 * Gets the currently configured log level.
                 */
                virtual LogLevel GetLogLevel() const = 0;
                /**
                 * Set a new log level. This has the immediate effect of changing the log output to the new level.
                 */
                virtual void SetLogLevel(LogLevel logLevel) = 0;

                /**
                 * Handle the logging information from common runtime libraries.
                 * Redirect them to C++ SDK logging system by default.
                 */
                virtual void Log(LogLevel logLevel, const char* subjectName, const char* formatStr, va_list args) = 0;

                /**
                 * Wrapper on top of CRT's aws_logger::clean_up method.
                 */
                virtual void CleanUp() {return;}
            };
        } // namespace Logging
    } // namespace Utils
} // namespace Aws

// for backward compatibility
#include <aws/core/utils/logging/DefaultCRTLogSystem.h>
