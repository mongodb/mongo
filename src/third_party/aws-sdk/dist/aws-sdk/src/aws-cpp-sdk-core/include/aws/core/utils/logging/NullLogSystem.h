/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/utils/logging/LogSystemInterface.h>
#include <aws/core/utils/logging/LogLevel.h>
#include <aws/core/utils/UnreferencedParam.h>

namespace Aws
{
    namespace Utils
    {
        namespace Logging
        {
            /**
             * Do nothing logger.
             */
            class AWS_CORE_API NullLogSystem : public LogSystemInterface
            {
            public:

                NullLogSystem() {}
                virtual ~NullLogSystem() {}

                virtual LogLevel GetLogLevel(void) const override { return LogLevel::Off; }

                virtual void Log(LogLevel logLevel, const char* tag, const char* formatStr, ...) override
                {
                    AWS_UNREFERENCED_PARAM(logLevel);
                    AWS_UNREFERENCED_PARAM(tag);
                    AWS_UNREFERENCED_PARAM(formatStr);
                }

                virtual void vaLog(LogLevel logLevel, const char* tag, const char* formatStr, va_list args) override
                {
                    AWS_UNREFERENCED_PARAM(logLevel);
                    AWS_UNREFERENCED_PARAM(tag);
                    AWS_UNREFERENCED_PARAM(formatStr);
                    AWS_UNREFERENCED_PARAM(args);
                }

                virtual void LogStream(LogLevel logLevel, const char* tag, const Aws::OStringStream &messageStream) override
                {
                    AWS_UNREFERENCED_PARAM(logLevel);
                    AWS_UNREFERENCED_PARAM(tag);
                    AWS_UNREFERENCED_PARAM(messageStream);
                }

                virtual void Flush() override {}
            };

        } // namespace Logging
    } // namespace Utils
} // namespace Aws
