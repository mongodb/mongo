/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#include <aws/core/utils/logging/FormattedLogSystem.h>

#include <aws/core/utils/DateTime.h>
#include <aws/core/platform/Time.h>

#include <fstream>
#include <cstdarg>
#include <stdio.h>
#include <thread>

using namespace Aws::Utils;
using namespace Aws::Utils::Logging;

static const char* GetLogPrefix(const LogLevel logLevel)
{
    switch (logLevel)
    {
        case LogLevel::Off:
            assert(!"This should never happen.");
            break;
        case LogLevel::Error:
            return "[ERROR] ";
        case LogLevel::Fatal:
            return "[FATAL] ";
        case LogLevel::Warn:
            return "[WARN] ";
        case LogLevel::Info:
            return "[INFO] ";
        case LogLevel::Debug:
            return "[DEBUG] ";
        case LogLevel::Trace:
            return "[TRACE] ";
    }
    return "[UNKNOWN] ";
}

static void AppendTimeStamp(Aws::String& statement)
{
    static const size_t TS_LEN = sizeof("2000-01-01 00:00:00.000") - 1;
    const size_t oldStatementSz = statement.size();
    const size_t newStatementSz = oldStatementSz + TS_LEN;
    statement.resize(newStatementSz);

    auto now = std::chrono::system_clock::now();
    std::time_t time = std::chrono::system_clock::to_time_t(now);
    struct tm gmtTimeStamp;
    Aws::Time::GMTime(&gmtTimeStamp, time);

    auto len = std::strftime(&statement[oldStatementSz], TS_LEN, "%Y-%m-%d %H:%M:%S", &gmtTimeStamp);
    if (len)
    {
        int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        ms = ms - ms / 1000 * 1000; // calculate the milliseconds as fraction.
        statement[oldStatementSz + len++] = '.';
        int divisor = 100;
        while(divisor)
        {
            auto digit = ms / divisor;
            statement[oldStatementSz + len++] = char('0' + digit);
            ms = ms - divisor * digit;
            divisor /= 10;
        }
        statement[oldStatementSz + len] = '\0';
    }
}

static Aws::String CreateLogPrefixLine(LogLevel logLevel, const char* tag, const size_t statementSize = 80)
{
    Aws::String prefix;
    static const size_t THREAD_ID_LEN = 16;
    static const size_t PREFIX_LEN = sizeof("[UNKNOWN] 2000-01-01 20:00:00.000  [] ") - 1 + THREAD_ID_LEN;
    const size_t statementLen = PREFIX_LEN + strlen(tag) + statementSize;

    prefix.reserve(statementLen);
    prefix = GetLogPrefix(logLevel);
    AppendTimeStamp(prefix);

    prefix += ' ';
    prefix += tag;
    prefix += " [";
    prefix += [](){
        Aws::StringStream strStr;
        strStr << std::this_thread::get_id();
        return strStr.str();
    }();
    prefix += "] ";

    return prefix;
}

FormattedLogSystem::FormattedLogSystem(LogLevel logLevel) :
    m_logLevel(logLevel)
{
}

void FormattedLogSystem::Log(LogLevel logLevel, const char* tag, const char* formatStr, ...)
{
    va_list args;
    va_start(args, formatStr);
    vaLog(logLevel, tag, formatStr, args);
    va_end(args);
}

void FormattedLogSystem::vaLog(LogLevel logLevel, const char* tag, const char* formatStr, va_list args)
{
    va_list tmp_args; //unfortunately you cannot consume a va_list twice
    va_copy(tmp_args, args); //so we have to copy it
#ifdef _WIN32
    const int requiredLength = _vscprintf(formatStr, tmp_args) + 1;
#else
    const int requiredLength = vsnprintf(nullptr, 0, formatStr, tmp_args) + 1;
#endif
    va_end(tmp_args);

    Aws::String statement = CreateLogPrefixLine(logLevel, tag, requiredLength);

    const size_t oldStatementSz = statement.size();
    const size_t newStatementSz = oldStatementSz + requiredLength;
    // assert(statement.capacity() >= newStatementSz);
    // assert(statement.capacity() < 3 * newStatementSz / 2);
    statement.resize(newStatementSz);
    assert(statement.size() == newStatementSz);
#ifdef _WIN32
    vsnprintf_s(&statement[oldStatementSz], requiredLength, _TRUNCATE, formatStr, args);
#else
    vsnprintf(&statement[oldStatementSz], requiredLength, formatStr, args);
#endif // _WIN32

    statement[newStatementSz - 1] = '\n';

    ProcessFormattedStatement(std::move(statement));
}

void FormattedLogSystem::LogStream(LogLevel logLevel, const char* tag, const Aws::OStringStream &message_stream)
{
    auto message = message_stream.str();
    ProcessFormattedStatement(CreateLogPrefixLine(logLevel, tag, message.size()) + std::move(message) + "\n");
    if (LogLevel::Fatal == logLevel)
    {
        Flush();
    }
}
