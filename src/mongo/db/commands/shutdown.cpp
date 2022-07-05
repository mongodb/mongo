/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/logv2/log.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/commands/shutdown.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/fail_point.h"

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
    static StaticImmortal<AtomicWord<bool>> shutdownAlreadyInProgress{false};
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
        })
            .detach();
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
