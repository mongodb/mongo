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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/logv2/log.h"

#include "mongo/db/commands/shutdown.h"
#include "mongo/util/fail_point.h"

namespace mongo {

namespace shutdown_detail {

MONGO_FAIL_POINT_DEFINE(crashOnShutdown);
int* volatile illegalAddress;  // NOLINT - used for fail point only

void finishShutdown(bool force, long long timeoutSecs) {
    ShutdownTaskArgs shutdownArgs;
    shutdownArgs.isUserInitiated = true;

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
          "timeoutSecs"_attr = timeoutSecs);

#if defined(_WIN32)
    // Signal the ServiceMain thread to shutdown.
    if (ntservice::shouldStartService()) {
        shutdownNoTerminate(shutdownArgs);

        // Client expects us to abruptly close the socket as part of exiting
        // so this function is not allowed to return.
        // The ServiceMain thread will quit for us so just sleep until it does.
        while (true)
            sleepsecs(60);  // Loop forever
        return;
    }
#endif
    shutdown(EXIT_CLEAN, shutdownArgs);  // this never returns
}

}  // namespace shutdown_detail

}  // namespace mongo