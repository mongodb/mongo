// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/database_name.h"
#include "mongo/db/index_builds/resumable_index_builds_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];
namespace mongo::index_builds::build_attempts {

/**
 * Per-node attempt counts for unfinished two-phase index builds, persisted in
 * 'local.system.indexBuilds'.
 */
namespace two_phase {

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

/**
 * If it exists, marks the collection's record store so that its size is tracked during replication
 * recovery.
 */
void trackSizeThroughRecovery(OperationContext* opCtx);

}  // namespace two_phase

/**
 * Resume-attempt counts for unfinished primary-driven index builds, persisted in the build's
 * replicated resume state and cleared when the build persists resume state.
 */
namespace primary_driven {

/**
 * Checks the primary-driven build's resume count against maxIndexBuildAttemptsAfterUncleanShutdown.
 * Returns false if over the limit. Resumes identified as planned handoffs do not increment the
 * count. Otherwise, attempts to persist the count with majority write concern and updates
 * 'resumeInfo' on success. Builds already in the drain phase are exempt from checking or recording
 * the count.
 */
bool checkWithinAttemptLimit(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             const std::string& ident,
                             ResumeIndexInfo& resumeInfo,
                             bool plannedHandoff);

}  // namespace primary_driven

}  // namespace mongo::index_builds::build_attempts
