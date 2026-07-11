// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/signal_handlers.h"

#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "bits/types/siginfo_t.h"
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/log_process_details.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_util.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/duration.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/signal_win32.h"  // IWYU pragma: keep
#include "mongo/util/stacktrace.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <memory>
#include <random>
#include <thread>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

void logRotationProcessing(LogRotationState* rotation) {
    {
        // Rate limit: 1 second per signal
        auto now = time(nullptr);
        if (rotation->previous != rotation->kNever && difftime(now, rotation->previous) <= 1.0)
            return;
        rotation->previous = now;
    }

    if (auto status = logv2::rotateLogs(serverGlobalParams.logRenameOnRotate, {}, {});
        !status.isOK()) {
        LOGV2_ERROR(4719800, "Log rotation failed", "error"_attr = status);
    }
    if (rotation->logFileStatus == LogFileStatus::kNeedToRotateLogFile) {
        logProcessDetailsForLogRotate(getGlobalServiceContext());
    }
}

MONGO_INITIALIZER_GENERAL(ServerSignalProcessingSetup, (), ())
(InitializerContext* context) {
    setLogRotationCallback(logRotationProcessing);
}

}  // namespace mongo
