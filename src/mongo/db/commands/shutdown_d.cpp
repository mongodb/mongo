// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/shutdown.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


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
            auto& rss = rss::ReplicatedStorageService::get(opCtx);
            if (rss.getPersistenceProvider().shouldStepDownForShutdown()) {
                replCoord->stepDown(opCtx, false /* force */, waitTime, Days(1));
            }

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

        // Even if the ReplicationCoordinator failed to step down, ensure we still interrupt the
        // TransactionCoordinatorService (see SERVER-45009).
        TransactionCoordinatorService::get(opCtx)->interruptForStepDown();
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
               "after the attempted stepdown, any remaining time in 'timeout' is used for "
               "quiesce mode, where the database continues to allow operations to run, but directs "
               "clients to route new operations to other replica set members.";
    }

    static void beginShutdown(OperationContext* opCtx, bool force, Milliseconds timeout) {
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

        uassertStatusOK(stepDownForShutdown(opCtx, timeout, force));
    }
};
MONGO_REGISTER_COMMAND(CmdShutdownMongoD).forShard();

}  // namespace
}  // namespace mongo
