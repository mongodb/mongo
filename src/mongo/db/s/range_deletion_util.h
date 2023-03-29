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

#include <boost/optional.hpp>
#include <list>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/type_chunk.h"

namespace mongo {

constexpr auto kRangeDeletionThreadName = "range-deleter"_sd;

/**
 * DO NOT USE - only necessary for the legacy range deleter
 *
 * Deletes a range of orphaned documents for the given namespace and collection UUID. Returns a
 * future which will be resolved when the range has finished being deleted. The resulting future
 * will contain an error in cases where the range could not be deleted successfully.
 *
 * The overall algorithm is as follows:
 * 1. Wait for the all active queries which could be using the range to resolve by waiting
 *    for the waitForActiveQueriesToComplete future to resolve.
 * 2. Waits for delayForActiveQueriesOnSecondariesToComplete seconds before deleting any documents,
 *    to give queries running on secondaries a chance to finish.
 * 3. Delete documents in a series of batches with up to numDocsToRemovePerBatch documents per
 *    batch, with a delay of delayBetweenBatches milliseconds in between batches.
 */
SharedSemiFuture<void> removeDocumentsInRange(
    const std::shared_ptr<executor::TaskExecutor>& executor,
    SemiFuture<void> waitForActiveQueriesToComplete,
    const NamespaceString& nss,
    const UUID& collectionUuid,
    const BSONObj& keyPattern,
    const ChunkRange& range,
    Seconds delayForActiveQueriesOnSecondariesToComplete);

/**
 * Delete the range in a sequence of batches until there are no more documents to delete or deletion
 * returns an error.
 */
Status deleteRangeInBatches(OperationContext* opCtx,
                            const DatabaseName& dbName,
                            const UUID& collectionUuid,
                            const BSONObj& keyPattern,
                            const ChunkRange& range);

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
 * Wrapper to run a safer step up/step down killable task within an operation context
 */
template <typename Callable>
auto withTemporaryOperationContext(Callable&& callable,
                                   const DatabaseName dbName,
                                   const UUID& collectionUUID,
                                   bool writeToRangeDeletionNamespace = false) {
    ThreadClient tc(kRangeDeletionThreadName, getGlobalServiceContext());
    {
        stdx::lock_guard<Client> lk(*tc.get());
        tc->setSystemOperationKillableByStepdown(lk);
    }
    auto uniqueOpCtx = Client::getCurrent()->makeOperationContext();
    auto opCtx = uniqueOpCtx.get();

    // Ensure that this operation will be killed by the RstlKillOpThread during step-up or stepdown.
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
    invariant(opCtx->shouldAlwaysInterruptAtStepDownOrUp());

    {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        Lock::GlobalLock lock(opCtx, MODE_IX);
        uassert(
            ErrorCodes::PrimarySteppedDown,
            str::stream()
                << "Not primary while running range deletion task for collection with UUID "
                << collectionUUID,
            replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet &&
                replCoord->canAcceptWritesFor(opCtx,
                                              NamespaceStringOrUUID(dbName, collectionUUID)) &&
                (!writeToRangeDeletionNamespace ||
                 replCoord->canAcceptWritesFor(opCtx, NamespaceString::kRangeDeletionNamespace)));
    }

    return callable(opCtx);
}

}  // namespace mongo
