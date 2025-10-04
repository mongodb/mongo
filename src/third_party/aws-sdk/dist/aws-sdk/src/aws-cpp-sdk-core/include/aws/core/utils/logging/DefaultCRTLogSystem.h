/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/utils/logging/CRTLogSystem.h>

#include <condition_variable>
#include <thread>


namespace Aws
{
    namespace Utils
    {
        namespace Logging
        {
            /**
             * The default CRT log system will just do a redirection of all logs from common runtime libraries to C++ SDK.
             */
            class AWS_CORE_API DefaultCRTLogSystem : public CRTLogSystemInterface
            {
            public:
                DefaultCRTLogSystem(LogLevel logLevel);
                virtual ~DefaultCRTLogSystem() = default;

                DefaultCRTLogSystem(DefaultCRTLogSystem&&) = delete;
                DefaultCRTLogSystem(const DefaultCRTLogSystem&) = delete;

                /**
                 * Gets the currently configured log level.
                 */
                LogLevel GetLogLevel() const override;
                /**
                 * Set a new log level. This has the immediate effect of changing the log output to the new level.
                 */
                void SetLogLevel(LogLevel logLevel) override;

                /**
                 * Handle the logging information from common runtime libraries.
                 * Redirect them to C++ SDK logging system by default.
                 */
                virtual void Log(LogLevel logLevel, const char* subjectName, const char* formatStr, va_list args) override;
            };

        } // namespace Logging
    } // namespace Utils
} // namespace Aws
