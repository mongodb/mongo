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
#pragma once

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/uuid.h"

#include <boost/optional.hpp>

namespace mongo {

namespace rangedeletionutil {

constexpr auto kRangeDeletionThreadName = "range-deleter"_sd;

/**
 * Delete the range in a sequence of batches until there are no more documents to delete or deletion
 * returns an error. If successful, returns the number of deleted documents and bytes.
 */
StatusWith<std::pair<int, int>> deleteRangeInBatches(OperationContext* opCtx,
                                                     const DatabaseName& dbName,
                                                     const UUID& collectionUuid,
                                                     const BSONObj& keyPattern,
                                                     const ChunkRange& range);


/**
 * Check if there is at least one range deletion task for the specified collection.
 */
bool hasAtLeastOneRangeDeletionTaskForCollection(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 const UUID& collectionUuid);

/**
 * - Retrieves source collection's persistent range deletion tasks from `config.rangeDeletions`
 * - Associates tasks to the target collection
 * - Stores tasks in `config.rangeDeletionsForRename`
 */
void snapshotRangeDeletionsForRename(OperationContext* opCtx,
                                     const NamespaceString& fromNss,
                                     const NamespaceString& toNss);

/**
 * Copies `config.rangeDeletionsForRename` tasks for the specified namespace to
 * `config.rangeDeletions`.
 */
void restoreRangeDeletionTasksForRename(OperationContext* opCtx, const NamespaceString& nss);

/**
 * - Deletes range deletion tasks for the FROM namespace from `config.rangeDeletions`.
 * - Deletes range deletion tasks for the TO namespace from `config.rangeDeletionsForRename`
 */
void deleteRangeDeletionTasksForRename(OperationContext* opCtx,
                                       const NamespaceString& fromNss,
                                       const NamespaceString& toNss);

/**
 * Updates the range deletion task document to increase or decrease numOrphanedDocs
 */
void persistUpdatedNumOrphans(OperationContext* opCtx,
                              const UUID& collectionUuid,
                              const ChunkRange& range,
                              long long changeInOrphans);

/**
 * Removes range deletion task documents from `config.rangeDeletions` for the specified range and
 * collection
 */
void removePersistentRangeDeletionTask(OperationContext* opCtx,
                                       const UUID& collectionUuid,
                                       const ChunkRange& range);

/**
 * Removes all range deletion task documents from `config.rangeDeletions` for the specified
 * collection
 */
void removePersistentRangeDeletionTasksByUUID(OperationContext* opCtx, const UUID& collectionUuid);

/**
 * Creates a query object that can used to find overlapping ranges in the pending range deletions
 * collection.
 */
BSONObj overlappingRangeDeletionsQuery(const ChunkRange& range, const UUID& uuid);

/**
 * Checks the pending range deletions collection to see if there are any pending ranges that
 * conflict with the passed in range.
 */
size_t checkForConflictingDeletions(OperationContext* opCtx,
                                    const ChunkRange& range,
                                    const UUID& uuid);
/**
 * Writes the range deletion task document to config.rangeDeletions and waits for majority write
 * concern.
 */
void persistRangeDeletionTaskLocally(OperationContext* opCtx,
                                     const RangeDeletionTask& deletionTask,
                                     const WriteConcernOptions& writeConcern);

/**
 * Retrieves the value of 'numOrphanedDocs' from the recipient shard's range deletion task document.
 */
long long retrieveNumOrphansFromShard(OperationContext* opCtx,
                                      const ShardId& shardId,
                                      const UUID& migrationId);

/**
 * Retrieves the shard key pattern from the local range deletion task.
 */
boost::optional<KeyPattern> getShardKeyPatternFromRangeDeletionTask(OperationContext* opCtx,
                                                                    const UUID& migrationId);

/**
 * Deletes the range deletion task document with the specified id from config.rangeDeletions and
 * waits for majority write concern.
 */
void deleteRangeDeletionTaskLocally(OperationContext* opCtx,
                                    const UUID& collectionUuid,
                                    const ChunkRange& range,
                                    const WriteConcernOptions& writeConcern);

/**
 * Deletes the range deletion task document with the specified id from config.rangeDeletions on the
 * specified shard and waits for majority write concern.
 */
void deleteRangeDeletionTaskOnRecipient(OperationContext* opCtx,
                                        const ShardId& recipientId,
                                        const UUID& collectionUuid,
                                        const ChunkRange& range,
                                        const UUID& migrationId);

/**
 * Removes the 'pending' flag from the range deletion task document with the specified id from
 * config.rangeDeletions and waits for majority write concern. This marks the range as ready for
 * deletion.
 */
void markAsReadyRangeDeletionTaskLocally(OperationContext* opCtx,
                                         const UUID& collectionUuid,
                                         const ChunkRange& range);


/**
 * Removes the 'pending' flag from the range deletion task document with the specified id from
 * config.rangeDeletions on the specified shard and waits for majority write concern. This marks the
 * range as ready for deletion.
 */
void markAsReadyRangeDeletionTaskOnRecipient(OperationContext* opCtx,
                                             const ShardId& recipientId,
                                             const UUID& collectionUuid,
                                             const ChunkRange& range,
                                             const UUID& migrationId);


/**
 * Updates each document in the `config.rangeDeletions` collection by setting the
 * `preMigrationShardVersion` field to the default value `ChunkVersion::IGNORED()`, but only for
 * documents where the field is not already set.
 *
 * TODO SERVER-103046: Remove once 9.0 becomes last lts.
 */
void setPreMigrationShardVersionOnRangeDeletionTasks(OperationContext* opCtx);
}  // namespace rangedeletionutil
}  // namespace mongo
