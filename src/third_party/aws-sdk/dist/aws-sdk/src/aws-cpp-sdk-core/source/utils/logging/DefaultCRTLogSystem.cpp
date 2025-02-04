/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/logging/DefaultCRTLogSystem.h>
#include <aws/core/utils/logging/AWSLogging.h>
#include <aws/core/utils/logging/LogSystemInterface.h>
#include <aws/core/utils/Array.h>
#include <aws/common/common.h>
#include <cstdarg>
#include <chrono>
#include <thread>
#include <mutex>

using namespace Aws::Utils;
using namespace Aws::Utils::Logging;

namespace Aws
{
    namespace Utils
    {
        namespace Logging
        {
            DefaultCRTLogSystem::DefaultCRTLogSystem(LogLevel logLevel)
            {
                AWS_UNREFERENCED_PARAM(logLevel); // will use one from the main SDK logger
            }

            LogLevel DefaultCRTLogSystem::GetLogLevel() const
            {
                LogSystemInterface* pLogSystem = Logging::GetLogSystem();
                if (pLogSystem)
                {
                    return pLogSystem->GetLogLevel();
                }
                return Aws::Utils::Logging::LogLevel::Off;
            }

            void DefaultCRTLogSystem::SetLogLevel(LogLevel logLevel)
            {
                AWS_UNREFERENCED_PARAM(logLevel); // will use one from the main SDK logger
            }

            void DefaultCRTLogSystem::Log(LogLevel logLevel, const char* subjectName, const char* formatStr, va_list args)
            {
                LogSystemInterface* pLogSystem = Logging::GetLogSystem();
                if (pLogSystem)
                {
                     pLogSystem->vaLog(logLevel, subjectName, formatStr, args);
                }
            }
        }
    }
}

