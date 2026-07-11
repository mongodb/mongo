// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"

namespace mongo {

/**
 * Kicks off asynchronous work that notifies RangeDeleterService once every MoveRangeCoordinator
 * recovered for the current term has finished, resolving the recovery job registered on their
 * behalf in migrationutil::registerMigrationRecoveryJobs(). Returns immediately; the wait and
 * notification happen on a separate executor.
 *
 * Invoked from ShardingCoordinatorService::_onServiceInitialization() via the
 * moveRangeRecoveryNotifier constructor parameter, rather than called directly, so unit tests that
 * construct the service without a fully initialized sharding environment (Grid, RangeDeleter) can
 * default to a no-op. Only mongod_main.cpp, which wires up the real production instance, should
 * pass this function in. Note that this hook fires too late (during onStepUpComplete) to register
 * the job itself; that must happen earlier, during onStepUpBegin, which is why
 * registerMigrationRecoveryJobs() owns that half.
 */
void asyncNotifyRangeDeleterAfterMoveRangeRecovery(ServiceContext* serviceContext);

}  // namespace mongo
