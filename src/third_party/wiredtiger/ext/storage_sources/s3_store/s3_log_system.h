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
#ifndef S3LOGSYSTEM
#define S3LOGSYSTEM

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

#include <aws/core/utils/logging/LogSystemInterface.h>
#include <aws/core/utils/logging/LogLevel.h>

#include <atomic>

// Mapping the desired WiredTiger extension verbosity level to a rough equivalent AWS
// SDK verbosity level.
static const std::map<int32_t, Aws::Utils::Logging::LogLevel> verbosityMapping = {
  {WT_VERBOSE_ERROR, Aws::Utils::Logging::LogLevel::Fatal},
  {WT_VERBOSE_WARNING, Aws::Utils::Logging::LogLevel::Error},
  {WT_VERBOSE_WARNING, Aws::Utils::Logging::LogLevel::Warn},
  {WT_VERBOSE_INFO, Aws::Utils::Logging::LogLevel::Info},
  {WT_VERBOSE_DEBUG_1, Aws::Utils::Logging::LogLevel::Debug},
  {WT_VERBOSE_DEBUG_2, Aws::Utils::Logging::LogLevel::Debug},
  {WT_VERBOSE_DEBUG_3, Aws::Utils::Logging::LogLevel::Debug},
  {WT_VERBOSE_DEBUG_4, Aws::Utils::Logging::LogLevel::Debug},
  {WT_VERBOSE_DEBUG_5, Aws::Utils::Logging::LogLevel::Trace}};

/*
 * Provides the S3 Store with a logger implementation that redirects the generated logs to
 * WiredTiger's logging streams. This class implements AWS's LogSystemInterface class, an interface
 * for logging implementations. Functions are derived from the interface to incorporate the logging
 * with WiredTiger's logging system.
 */
class S3LogSystem : public Aws::Utils::Logging::LogSystemInterface {
public:
    S3LogSystem(WT_EXTENSION_API *wtApi, uint32_t wtVerbosityLevel);
    Aws::Utils::Logging::LogLevel
    GetLogLevel(void) const override
    {
        return (_awsLogLevel);
    }
    void Log(
      Aws::Utils::Logging::LogLevel logLevel, const char *tag, const char *format, ...) override;
    void LogStream(Aws::Utils::Logging::LogLevel logLevel, const char *tag,
      const Aws::OStringStream &messageStream) override;

    // Sends error messages to WiredTiger's error level log stream.
    void
    LogErrorMessage(const std::string &message) const
    {
        LogVerboseMessage(WT_VERBOSE_ERROR, message);
    }

    // Sends error messages to WiredTiger's debug level log stream.
    void
    LogDebugMessage(const std::string &message) const
    {
        LogVerboseMessage(WT_VERBOSE_DEBUG_1, message);
    }

    // Sets the WiredTiger Extension's verbosity level and matches the AWS log levels
    // to this.
    void SetWtVerbosityLevel(int32_t wtVerbosityLevel);

    // Inherited from AWS LogSystemInterface and is not used.
    void
    Flush() override final
    {
    }

private:
    void LogAwsMessage(const char *tag, const std::string &message) const;
    void LogVerboseMessage(int32_t verbosityLevel, const std::string &message) const;
    std::atomic<Aws::Utils::Logging::LogLevel> _awsLogLevel;
    WT_EXTENSION_API *_wtApi;
    int32_t _wtVerbosityLevel;
};
#endif
