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

namespace {
MONGO_FAIL_POINT_DEFINE(hangInShutdownBeforeStepdown);
MONGO_FAIL_POINT_DEFINE(hangInShutdownAfterStepdown);
}  // namespace

Status stepDownForShutdown(OperationContext* opCtx,
                           const Milliseconds& waitTime,
                           bool forceShutdown) noexcept {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    // If this is a single node replica set, then we don't have to wait
    // for any secondaries. Ignore stepdown.
    if (replCoord->getConfig().getNumMembers() != 1) {
        try {
            if (MONGO_unlikely(hangInShutdownBeforeStepdown.shouldFail())) {
                LOGV2(5436600, "hangInShutdownBeforeStepdown failpoint enabled");
                hangInShutdownBeforeStepdown.pauseWhileSet(opCtx);
            }

            // Specify a high freeze time, so that if there is a stall during shut down, the node
            // does not run for election.
            replCoord->stepDown(opCtx, false /* force */, waitTime, Days(1));

            if (MONGO_unlikely(hangInShutdownAfterStepdown.shouldFail())) {
                LOGV2(4695100, "hangInShutdownAfterStepdown failpoint enabled");
                hangInShutdownAfterStepdown.pauseWhileSet(opCtx);
            }
        } catch (const ExceptionFor<ErrorCodes::NotWritablePrimary>&) {
            // Ignore NotWritablePrimary errors.
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
        return "Shuts down the database. Must be run against the admin database and either (1) run "
               "from localhost or (2) run while authenticated with the shutdown privilege. If the "
               "node is the primary of a replica set, waits up to 'timeoutSecs' for an electable "
               "node to be caught up before stepping down. If 'force' is false and no electable "
               "node was able to catch up, does not shut down. If the node is in state SECONDARY "
               "after the attempted stepdown, any remaining time in 'timeoutSecs' is used for "
               "quiesce mode, where the database continues to allow operations to run, but directs "
               "clients to route new operations to other replica set members.";
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
