// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/s/chunk_move_write_concern_options.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/s/request_types/migration_secondary_throttle_options.h"
#include "mongo/util/duration.h"

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

const Seconds kDefaultWriteTimeoutForMigration(60);
const WriteConcernOptions kDefaultWriteConcernForMigration(2,
                                                           WriteConcernOptions::SyncMode::NONE,
                                                           kDefaultWriteTimeoutForMigration);
const WriteConcernOptions kWriteConcernLocal(1,
                                             WriteConcernOptions::SyncMode::NONE,
                                             WriteConcernOptions::kNoTimeout);

WriteConcernOptions getDefaultWriteConcernForMigration(OperationContext* opCtx) {
    repl::ReplicationCoordinator* replCoordinator = repl::ReplicationCoordinator::get(opCtx);
    if (replCoordinator->getSettings().isReplSet()) {
        Status status =
            replCoordinator->checkIfWriteConcernCanBeSatisfied(kDefaultWriteConcernForMigration);
        if (status.isOK()) {
            return kDefaultWriteConcernForMigration;
        }
    }

    return kWriteConcernLocal;
}

}  // namespace

StatusWith<WriteConcernOptions> ChunkMoveWriteConcernOptions::getEffectiveWriteConcern(
    OperationContext* opCtx, const MigrationSecondaryThrottleOptions& options) {
    auto secondaryThrottle = options.getSecondaryThrottle();
    if (secondaryThrottle == MigrationSecondaryThrottleOptions::kDefault) {
        secondaryThrottle = MigrationSecondaryThrottleOptions::kOff;
    }

    if (secondaryThrottle == MigrationSecondaryThrottleOptions::kOff) {
        return kWriteConcernLocal;
    }

    WriteConcernOptions writeConcern;

    if (options.isWriteConcernSpecified()) {
        writeConcern = options.getWriteConcern();

        repl::ReplicationCoordinator* replCoordinator = repl::ReplicationCoordinator::get(opCtx);

        Status status = replCoordinator->checkIfWriteConcernCanBeSatisfied(writeConcern);
        if (!status.isOK() && status != ErrorCodes::NoReplicationEnabled) {
            return status;
        }
    } else {
        writeConcern = getDefaultWriteConcernForMigration(opCtx);
    }

    if (writeConcern.needToWaitForOtherNodes() &&
        writeConcern.wTimeout == WriteConcernOptions::kNoTimeout) {
        // Don't allow no timeout
        writeConcern.wTimeout = duration_cast<Milliseconds>(kDefaultWriteTimeoutForMigration);
    }

    return writeConcern;
}

}  // namespace mongo
