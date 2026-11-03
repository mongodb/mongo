// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <vector>

#include <boost/optional/optional.hpp>

/**
 * Per-node attempt counts for unfinished two-phase index builds, persisted in
 * 'local.system.indexBuilds', so a node can abort a build that keeps crashing it. A build's count
 * starts at 0 when it first runs, is incremented after each unclean shutdown, and is made durable
 * before the build runs.
 */
[[MONGO_MOD_PUBLIC]];
namespace mongo::index_builds::build_attempts {

/**
 * Records 'buildUUID' with a count of 0 if it has no entry, and waits for its count to be durable.
 * Returns true if the count is within maxIndexBuildAttemptsAfterUncleanShutdown. Call without
 * holding locks, so no storage snapshot stays open.
 */
bool checkWithinAttemptLimit(OperationContext* opCtx, const UUID& buildUUID);

/**
 * Creates the attempt counts collection if it doesn't exist. Returns true if this call created it.
 */
bool createCollection(OperationContext* opCtx);

/**
 * Increments every entry's count, without waiting for the increments to be durable.
 */
void incrementAttempts(OperationContext* opCtx);

/**
 * Deletes the count for 'buildUUID', if it exists, in the caller's WriteUnitOfWork.
 */
void removeEntry(OperationContext* opCtx, const UUID& buildUUID);

/**
 * Deletes the counts for 'buildUUIDs' that exist, in the caller's WriteUnitOfWork.
 */
void removeEntries(OperationContext* opCtx, const std::vector<UUID>& buildUUIDs);

/**
 * Returns the count for 'buildUUID', or boost::none if there is none.
 */
boost::optional<int> read(OperationContext* opCtx, const UUID& buildUUID);

}  // namespace mongo::index_builds::build_attempts
