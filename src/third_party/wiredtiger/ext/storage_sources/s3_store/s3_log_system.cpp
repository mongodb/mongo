/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include <aws/core/Aws.h>
#include "s3_log_system.h"
#include <cstdarg>

// Constructor for S3LogSystem that calls to set the WiredTiger verbosity level.
S3LogSystem::S3LogSystem(WT_EXTENSION_API *wtApi, uint32_t wtVerbosityLevel) : _wtApi(wtApi)
{
    SetWtVerbosityLevel(wtVerbosityLevel);
}

// Overrides the interface's Log method to write the AWS SDK log stream to WiredTiger's log stream
// using variadic style through a helper function. Inherited from AWS's LogSystemInterface.
void
S3LogSystem::Log(Aws::Utils::Logging::LogLevel logLevel, const char *tag, const char *format, ...)
{
    std::stringstream ss;
    std::va_list args;
    va_list tmpArgs;
    va_start(args, format);
    char *outputBuff = nullptr;
    int requiredLength;

#ifdef _WIN32
    requiredLength = _vscprintf(formatStr, tmpArgs) + 1;
    outputBuff = (char *)malloc(requiredLength);
    vsnprintf_s(outputBuff, requiredLength, _TRUNCATE, formatStr, args);
#else
    requiredLength = vsnprintf(nullptr, 0, format, tmpArgs) + 1;
    outputBuff = (char *)malloc(requiredLength);
    vsnprintf(outputBuff, requiredLength, format, args);
#endif
    va_end(tmpArgs);
    ss << outputBuff << std::endl;
    free(outputBuff);
    LogAwsMessage(tag, ss.str());
    va_end(args);
}

// Overrides the interface's LogStream method to write the AWS SDK log stream to WiredTiger's log
// stream through a helper function. Inherited from AWS's LogSystemInterface.
void
S3LogSystem::LogStream(
  Aws::Utils::Logging::LogLevel logLevel, const char *tag, const Aws::OStringStream &messageStream)
{
    LogAwsMessage(tag, messageStream.rdbuf()->str().c_str());
}

// Directs the message to WiredTiger's log streams.
void
S3LogSystem::LogAwsMessage(const char *tag, const std::string &message) const
{
    _wtApi->err_printf(_wtApi, NULL, "%s : %s", tag, message.c_str());
}

// Directs the message to WiredTiger's log streams matched at WiredTiger's log stream levels.
void
S3LogSystem::LogVerboseMessage(int32_t verbosityLevel, const std::string &message) const
{
    if (verbosityLevel <= _wtVerbosityLevel) {
        // Use err_printf for error and warning messages and use msg_printf for notice, info and
        // debug messages.
        if (verbosityLevel < WT_VERBOSE_NOTICE)
            _wtApi->err_printf(_wtApi, NULL, "%s", message.c_str());
        else
            _wtApi->msg_printf(_wtApi, NULL, "%s", message.c_str());
    }
}

// Sets the WiredTiger verbosity level by mapping the AWS SDK log level.
void
S3LogSystem::SetWtVerbosityLevel(int32_t wtVerbosityLevel)
{
    _wtVerbosityLevel = wtVerbosityLevel;
    // If the verbosity level is out of range it will default to AWS SDK Error level.
    if (verbosityMapping.find(_wtVerbosityLevel) != verbosityMapping.end())
        _awsLogLevel = verbosityMapping.at(_wtVerbosityLevel);
    else
        _awsLogLevel = Aws::Utils::Logging::LogLevel::Error;
}
