/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/logging/CRTLogging.h>
#include <aws/core/utils/logging/CRTLogSystem.h>
#include <aws/core/utils/logging/LogLevel.h>
#include <aws/core/utils/threading/ReaderWriterLock.h>
#include <aws/common/logging.h>

#include <memory>
#include <mutex>
#include <assert.h>
#include <cstdarg>

using namespace Aws::Utils;
using namespace Aws::Utils::Logging;

namespace Aws
{
namespace Utils
{
namespace Logging {

static std::shared_ptr<CRTLogSystemInterface> CRTLogSystem(nullptr);
static aws_logger s_sdkCrtLogger;

static int s_aws_logger_redirect_log(
        struct aws_logger *logger,
        enum aws_log_level log_level,
        aws_log_subject_t subject,
        const char *format, ...)
{
    AWS_UNREFERENCED_PARAM(logger);
    CRTLogSystemInterface* pLogger = CRTLogSystem.get();
    if (!pLogger)
    {
        return AWS_OP_ERR;
    }
    assert(logger->p_impl == &s_sdkCrtLogger);
    Aws::Utils::Logging::LogLevel logLevel = static_cast<LogLevel>(log_level);
    const char* subjectName = aws_log_subject_name(subject);
    va_list args;
    va_start(args, format);
    pLogger->Log(logLevel, subjectName, format, args);
    va_end(args);
    return AWS_OP_SUCCESS;
}

static enum aws_log_level s_aws_logger_redirect_get_log_level(struct aws_logger *logger, aws_log_subject_t subject)
{
    AWS_UNREFERENCED_PARAM(logger);
    AWS_UNREFERENCED_PARAM(subject);
    CRTLogSystemInterface* pLogger = CRTLogSystem.get();
    if (pLogger)
    {
        assert(logger->p_impl == &s_sdkCrtLogger);
        return (aws_log_level) (pLogger->GetLogLevel());
    }
    return AWS_LL_NONE;
}

static void s_aws_logger_redirect_clean_up(struct aws_logger *logger)
{
    AWS_UNREFERENCED_PARAM(logger);
    CRTLogSystemInterface* pLogger = CRTLogSystem.get();
    if (pLogger)
    {
        assert(logger->p_impl == &s_sdkCrtLogger);
        return pLogger->CleanUp();
    }
}

static int s_aws_logger_redirect_set_log_level(struct aws_logger *logger, enum aws_log_level log_level)
{
    AWS_UNREFERENCED_PARAM(logger);
    CRTLogSystemInterface* pLogger = CRTLogSystem.get();
    if (!pLogger)
    {
        return AWS_OP_ERR;
    }
    assert(logger->p_impl == &s_sdkCrtLogger);
    pLogger->SetLogLevel(static_cast<LogLevel>(log_level));
    return AWS_OP_SUCCESS;

}

static struct aws_logger_vtable s_aws_logger_redirect_vtable = {
        s_aws_logger_redirect_log, // .log
        s_aws_logger_redirect_get_log_level, // .get_log_level
        s_aws_logger_redirect_clean_up, // .clean_up
        s_aws_logger_redirect_set_log_level // set_log_level
};


/**
 * Installs SDK wrapper over CRT logger.
 * This wrapper will redirect all CRT logger calls to the set SDK "CRTLogSystem"
 * This method is not thread-safe (as the most other global Init/Shutdown APIs of the SDK).
 */
void SetUpCrtLogsRedirection()
{
    s_sdkCrtLogger.vtable = &s_aws_logger_redirect_vtable;
    s_sdkCrtLogger.allocator = Aws::get_aws_allocator();
    s_sdkCrtLogger.p_impl = &s_sdkCrtLogger;

    aws_logger_set(&s_sdkCrtLogger);
}

void InitializeCRTLogging(const std::shared_ptr<CRTLogSystemInterface>& inputCrtLogSystem) {
    SetUpCrtLogsRedirection();
    CRTLogSystem = inputCrtLogSystem;
}

void ShutdownCRTLogging() {
    if (aws_logger_get() == &s_sdkCrtLogger)
    {
        aws_logger_set(nullptr);
    }
    // GetLogSystem returns a raw pointer
    // so this is a hack to let all other threads finish their log statement after getting a LogSystem pointer
    // otherwise we would need to perform ref-counting on each logging statement
    auto tmpLogger = std::move(CRTLogSystem);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    tmpLogger.reset();
}

} // namespace Logging
} // namespace Utils
} // namespace Aws
