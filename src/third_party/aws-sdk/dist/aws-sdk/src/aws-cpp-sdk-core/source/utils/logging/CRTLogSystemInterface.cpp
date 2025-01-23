/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/logging/CRTLogSystem.h>

using namespace Aws::Utils;
using namespace Aws::Utils::Logging;

namespace Aws
{
    namespace Utils
    {
        namespace Logging
        {
            static_assert(Aws::Utils::Logging::LogLevel::Off == static_cast<LogLevel>(aws_log_level::AWS_LL_NONE), "Different underlying values between SDK and CRT log level enums");
            static_assert(Aws::Utils::Logging::LogLevel::Fatal == static_cast<LogLevel>(aws_log_level::AWS_LL_FATAL), "Different underlying values between SDK and CRT log level enums");
            static_assert(Aws::Utils::Logging::LogLevel::Error == static_cast<LogLevel>(aws_log_level::AWS_LL_ERROR), "Different underlying values between SDK and CRT log level enums");
            static_assert(Aws::Utils::Logging::LogLevel::Warn == static_cast<LogLevel>(aws_log_level::AWS_LL_WARN), "Different underlying values between SDK and CRT log level enums");
            static_assert(Aws::Utils::Logging::LogLevel::Info == static_cast<LogLevel>(aws_log_level::AWS_LL_INFO), "Different underlying values between SDK and CRT log level enums");
            static_assert(Aws::Utils::Logging::LogLevel::Debug == static_cast<LogLevel>(aws_log_level::AWS_LL_DEBUG), "Different underlying values between SDK and CRT log level enums");
            static_assert(Aws::Utils::Logging::LogLevel::Trace == static_cast<LogLevel>(aws_log_level::AWS_LL_TRACE), "Different underlying values between SDK and CRT log level enums");
        }
    }
}

