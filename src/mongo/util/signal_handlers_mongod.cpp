/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/util/signal_handlers.h"

#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "bits/types/siginfo_t.h"
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/log_process_details.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_util.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/thread.h"
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
#include "mongo/util/thread_util.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <memory>
#include <random>


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
