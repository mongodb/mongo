#include <aws/core/Aws.h>
#include "s3_log_system.h"
#include <cstdarg>

S3LogSystem::S3LogSystem(WT_EXTENSION_API *wtApi, uint32_t wtVerbosityLevel)
{
    // If the verbosity level is out of range it will default to AWS SDK Error level.
    if (verbosityMapping.find(wtVerbosityLevel) != verbosityMapping.end()) {
        awsLogLevel = verbosityMapping.at(wtVerbosityLevel);
    } else {
        awsLogLevel = Aws::Utils::Logging::LogLevel::Error;
    }
    this->wtApi = wtApi;
    this->wtVerbosityLevel = wtVerbosityLevel;
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
    wtApi->err_printf(wtApi, NULL, "%s : %s", tag, message.c_str());
}

void
S3LogSystem::LogVerboseMessage(int32_t verbosityLevel, const std::string &message)
{
    if (verbosityLevel <= wtVerbosityLevel)
        wtApi->err_printf(wtApi, NULL, "%s", message.c_str());
}

void
S3LogSystem::Flush()
{
    return;
}
