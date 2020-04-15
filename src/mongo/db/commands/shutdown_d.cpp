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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/commands/shutdown.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/logv2/log.h"

namespace mongo {

Status stepDownForShutdown(OperationContext* opCtx,
                           const Milliseconds& waitTime,
                           bool forceShutdown) noexcept {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    // If this is a single node replica set, then we don't have to wait
    // for any secondaries. Ignore stepdown.
    if (replCoord->getConfig().getNumMembers() != 1) {
        try {
            replCoord->stepDown(opCtx, false /* force */, waitTime, Seconds(120));
        } catch (const ExceptionFor<ErrorCodes::NotMaster>&) {
            // Ignore not master errors.
        } catch (const DBException& e) {
            if (!forceShutdown) {
                return e.toStatus();
            }
            // Ignore stepDown errors on force shutdown.
            LOGV2_WARNING(4719000, "Error stepping down during force shutdown", "error"_attr = e);
        }

        // Even if the ReplicationCoordinator failed to step down, ensure we still shut down the
        // TransactionCoordinatorService (see SERVER-45009)
        TransactionCoordinatorService::get(opCtx)->onStepDown();
    }
    return Status::OK();
}

namespace {

class CmdShutdownMongoD : public CmdShutdown<CmdShutdownMongoD> {
public:
    std::string help() const override {
        return "shutdown the database.  must be ran against admin db and "
               "either (1) ran from localhost or (2) authenticated. If "
               "this is a primary in a replica set and there is no member "
               "within 10 seconds of its optime, it will not shutdown "
               "without force : true.  You can also specify timeoutSecs : "
               "N to wait N seconds for other members to catch up.";
    }

    static void beginShutdown(OperationContext* opCtx, bool force, long long timeoutSecs) {
        // This code may race with a new index build starting up. We may get 0 active index builds
        // from the IndexBuildsCoordinator shutdown to proceed, but there is nothing to prevent a
        // new index build from starting after that check.
        if (!force) {
            auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
            auto numIndexBuilds = indexBuildsCoord->getActiveIndexBuildCount(opCtx);
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    str::stream() << "Index builds in progress while processing shutdown command "
                                     "without {force: true}: "
                                  << numIndexBuilds,
                    numIndexBuilds == 0U);
        }

        uassertStatusOK(stepDownForShutdown(opCtx, Seconds(timeoutSecs), force));
    }

} cmdShutdownMongoD;

}  // namespace
}  // namespace mongo
