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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/chunk_move_write_concern_options.h"

#include "mongo/base/status_with.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/s/request_types/migration_secondary_throttle_options.h"

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
    if (replCoordinator->getReplicationMode() == mongo::repl::ReplicationCoordinator::modeReplSet) {
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
        if (opCtx->getServiceContext()->getStorageEngine()->supportsDocLocking()) {
            secondaryThrottle = MigrationSecondaryThrottleOptions::kOff;
        } else {
            secondaryThrottle = MigrationSecondaryThrottleOptions::kOn;
        }
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
        writeConcern.wTimeout = durationCount<Milliseconds>(kDefaultWriteTimeoutForMigration);
    }

    return writeConcern;
}

}  // namespace mongo
