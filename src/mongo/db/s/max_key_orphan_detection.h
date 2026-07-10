/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/type_chunk_range.h"
#include "mongo/util/uuid.h"

#include <vector>

namespace mongo {

class OperationContext;
class ServiceContext;

/**
 * Returns true iff 'shardKeyValue' is non-empty and every field is MaxKey.
 */
bool isGlobalMaxShardKey(const BSONObj& shardKeyValue);

/**
 * Runs a single synchronous MaxKey orphan sweep on this shard primary and persists the outcome to
 * config.maxKeyOrphanScanState. Short-circuits if a completed sweep is already recorded. Invoked by
 * the step-up launcher and by unit tests.
 */
void runMaxKeyOrphanDetection(OperationContext* opCtx, long long term);

/**
 * Spawns the asynchronous MaxKey orphan detector for 'term', cancelling any detector from a prior
 * term. Gated on featureFlagMaxKeyDetection.
 */
void launchMaxKeyOrphanDetectionOnStepUp(OperationContext* opCtx, long long term);

/**
 * Interrupts and joins any in-flight MaxKey orphan detector.
 */
void cancelMaxKeyOrphanDetection(ServiceContext* serviceContext);

/**
 * Returns true iff the task for 'range' still contains a MaxKey orphan the guard must preserve:
 * i.e. the shard key is not hashed, the range's upper bound is the global MaxKey, and a document
 * whose leading shard-key field is MaxKey exists locally within the range. The first two checks are
 * cheap; the local index scan only runs when both hold. May throw IndexNotFound when the shard-key
 * index needed to answer the question is unavailable.
 */
bool shouldSkipRangeDeletionForMaxKeyOrphans(OperationContext* opCtx,
                                             const DatabaseName& dbName,
                                             const UUID& collUuid,
                                             const BSONObj& shardKeyPattern,
                                             const ChunkRange& range);

/**
 * Returns the range-deletion task ids the guard must not delete because they still contain a MaxKey
 * orphan. One-shot per epoch, keyed on the 'blockedTasks' field of config.maxKeyOrphanScanState: if
 * present (even empty) it is returned as-is (rehydrate); otherwise every pending task in
 * config.rangeDeletions is classified via shouldSkipRangeDeletionForMaxKeyOrphans and persisted to
 * 'blockedTasks'. A task that cannot be classified is conservatively blocked; fatal errors are
 * rethrown so the caller can retry. Must run before range deletions proceed on a new primary.
 */
std::vector<UUID> loadOrComputeBlockedMaxKeyRangeDeletionTasks(OperationContext* opCtx);

}  // namespace mongo
