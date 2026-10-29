// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/commands/shutdown.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/time_support.h"

#include <cstdlib>
#include <new>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace shutdown_detail {

MONGO_FAIL_POINT_DEFINE(crashOnShutdown);
int* volatile illegalAddress;  // NOLINT - used for fail point only

void finishShutdown(OperationContext* opCtx,
                    bool force,
                    Milliseconds timeout,
                    Milliseconds quiesceTime) {
    crashOnShutdown.execute([&](const BSONObj& data) {
        if (data["how"].str() == "fault") {
            ++*illegalAddress;
        }
        ::abort();
    });

    // Shared by mongos and mongod shutdown code paths
    LOGV2(4695400,
          "Terminating via shutdown command",
          "force"_attr = force,
          "timeout"_attr = timeout);

    // Only allow the first shutdown command to spawn a new thread and execute the shutdown.
    // Late arrivers will skip and wait until operations are killed.
    static StaticImmortal<Atomic<bool>> shutdownAlreadyInProgress{false};
    if (!shutdownAlreadyInProgress->swap(true)) {
        stdx::thread([quiesceTime] {
            ShutdownTaskArgs shutdownArgs;
            shutdownArgs.isUserInitiated = true;
            shutdownArgs.quiesceTime = quiesceTime;

#if defined(_WIN32)
            // Signal the ServiceMain thread to shutdown.
            if (ntservice::shouldStartService()) {
                shutdownNoTerminate(shutdownArgs);
                return;
            }
#endif
            shutdown(ExitCode::clean, shutdownArgs);  // this never returns
        }).detach();
    }

    // Client expects the shutdown command to abruptly close the socket as part of exiting.
    // This function is not allowed to return until the server interrupts its operation.
    // The following requires the shutdown task to kill all the operations after the server
    // stops accepting incoming connections.
    while (opCtx->checkForInterruptNoAssert().isOK())
        sleepsecs(1);

    iasserted({ErrorCodes::CloseConnectionForShutdownCommand,
               "Closing the connection running the shutdown command"});
}

}  // namespace shutdown_detail
}  // namespace mongo
