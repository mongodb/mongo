#include <aws/core/Aws.h>
#include "s3_log_system.h"
#include <cstdarg>

S3LogSystem::S3LogSystem(WT_EXTENSION_API *wtApi, uint32_t wtVerbosityLevel) : _wtApi(wtApi)
{
    SetWtVerbosityLevel(wtVerbosityLevel);
}

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

void
S3LogSystem::LogStream(
  Aws::Utils::Logging::LogLevel logLevel, const char *tag, const Aws::OStringStream &messageStream)
{
    LogAwsMessage(tag, messageStream.rdbuf()->str().c_str());
}

void
S3LogSystem::LogAwsMessage(const char *tag, const std::string &message) const
{
    _wtApi->err_printf(_wtApi, NULL, "%s : %s", tag, message.c_str());
}

void
S3LogSystem::LogVerboseMessage(int32_t verbosityLevel, const std::string &message) const
{
    if (verbosityLevel <= _wtVerbosityLevel) {
        /* Use err_printf for error and warning messages and use msg_printf for notice, info and
         * debug messages. */
        if (verbosityLevel < -1)
            _wtApi->err_printf(_wtApi, NULL, "%s", message.c_str());
        else
            _wtApi->msg_printf(_wtApi, NULL, "%s", message.c_str());
    }
}

void
S3LogSystem::LogVerboseErrorMessage(const std::string &message) const
{
    LogVerboseMessage(WT_VERBOSE_ERROR, message);
}

void
S3LogSystem::LogVerboseDebugMessage(const std::string &message) const
{
    LogVerboseMessage(WT_VERBOSE_DEBUG, message);
}

void
S3LogSystem::SetWtVerbosityLevel(int32_t wtVerbosityLevel)
{
    _wtVerbosityLevel = wtVerbosityLevel;
    /* If the verbosity level is out of range it will default to AWS SDK Error level. */
    if (verbosityMapping.find(_wtVerbosityLevel) != verbosityMapping.end())
        _awsLogLevel = verbosityMapping.at(_wtVerbosityLevel);
    else
        _awsLogLevel = Aws::Utils::Logging::LogLevel::Error;
}

void
S3LogSystem::Flush()
{
    return;
}
