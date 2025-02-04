/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#include <aws/core/utils/logging/AWSLogging.h>
#include <aws/core/utils/logging/LogSystemInterface.h>
#include <aws/core/utils/memory/stl/AWSStack.h>

#include <memory>
#include <thread>

using namespace Aws::Utils;
using namespace Aws::Utils::Logging;

static std::shared_ptr<LogSystemInterface> AWSLogSystem(nullptr);
static std::shared_ptr<LogSystemInterface> OldLogger(nullptr);

namespace Aws
{
namespace Utils
{
namespace Logging {

void InitializeAWSLogging(const std::shared_ptr<LogSystemInterface> &logSystem) {
    AWSLogSystem = logSystem;
}

void ShutdownAWSLogging(void) {
    InitializeAWSLogging(nullptr);
    // GetLogSystem returns a raw pointer
    // so this is a hack to let all other threads finish their log statement after getting a LogSystem pointer
    // otherwise we would need to perform ref-counting on each logging statement
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    OldLogger.reset();
}

LogSystemInterface *GetLogSystem() {
    return AWSLogSystem.get();
}

void PushLogger(const std::shared_ptr<LogSystemInterface> &logSystem)
{
    OldLogger = AWSLogSystem;
    AWSLogSystem = logSystem;
}

void PopLogger()
{
    AWSLogSystem = OldLogger;
    OldLogger = nullptr;
}

} // namespace Logging
} // namespace Utils
} // namespace Aws