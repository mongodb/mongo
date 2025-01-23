/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/logging/LogLevel.h>

#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <cassert>

using namespace Aws::Utils::Logging;

namespace Aws
{
namespace Utils
{
namespace Logging
{

Aws::String GetLogLevelName(LogLevel logLevel) 
{ 
	switch (logLevel)
	{
	case LogLevel::Fatal:
		return "FATAL";
	case LogLevel::Error:
		return "ERROR";
	case LogLevel::Warn:
		return "WARN";
	case LogLevel::Info:
		return "INFO";
	case LogLevel::Debug:
		return "DEBUG";
	case LogLevel::Trace:
		return "TRACE";
	case LogLevel::Off:
		return "OFF";
	default:
		assert(0);
		return "";
	}   
}

} // namespace Logging
} // namespace Utils
} // namespace Aws
