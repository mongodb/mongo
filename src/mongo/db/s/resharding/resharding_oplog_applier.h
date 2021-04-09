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

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/db/s/resharding/resharding_donor_oplog_iterator.h"
#include "mongo/db/s/resharding/resharding_oplog_application.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_progress_gen.h"
#include "mongo/db/s/resharding/resharding_oplog_batch_applier.h"
#include "mongo/db/s/resharding/resharding_oplog_batch_preparer.h"
#include "mongo/db/s/resharding/resharding_oplog_session_application.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/util/future.h"

namespace mongo {

class ReshardingMetrics;
class ServiceContext;

/**
 * Applies oplog entries from a specific donor for resharding.
 *
 * @param sourceId combines the resharding run's UUID with the donor ShardId.
 * @param oplogNs is the namespace for the collection containing oplog entries this
 *                ReshardingOplogApplier will read from and apply. There is one oplog per donor.
 * @param nsBeingResharded is the namespace of the collection being resharded.
 * @param collUUIDBeingResharded is the UUID of the collection being resharded.
 * @param allStashNss are the namespaces of the stash collections. There is one stash collection for
 *                    each donor. This ReshardingOplogApplier will write documents as necessary to
 *                    the stash collection at `myStashIdx` and may need to read and delete documents
 *                    from any of the other stash collections.
 * @param myStashIdx -- see above.
 * @param reshardingCloneFinishedTs is the timestamp that represents when cloning documents
 *                                  finished. Applying entries through this time implies the
 *                                  resharded collection contains a consistent snapshot of data at
 *                                  that timestamp.
 *
 * This is not thread safe.
 */
class ReshardingOplogApplier {
public:
    class Env {
    public:
        Env(ServiceContext* service, ReshardingMetrics* metrics)
            : _service(service), _metrics(metrics) {}
        ServiceContext* service() const {
            return _service;
        }
        ReshardingMetrics* metrics() const {
            return _metrics;
        }

    private:
        ServiceContext* _service;
        ReshardingMetrics* _metrics;
    };

    ReshardingOplogApplier(std::unique_ptr<Env> env,
                           ReshardingSourceId sourceId,
                           NamespaceString oplogNs,
                           NamespaceString nsBeingResharded,
                           UUID collUUIDBeingResharded,
                           std::vector<NamespaceString> allStashNss,
                           size_t myStashIdx,
                           Timestamp reshardingCloneFinishedTs,
                           std::unique_ptr<ReshardingDonorOplogIteratorInterface> oplogIterator,
                           const ChunkManager& sourceChunkMgr);

    /**
     * Applies oplog from the iterator until it has at least applied an oplog entry with timestamp
     * greater than or equal to reshardingCloneFinishedTs.
     * It is undefined to call applyUntilCloneFinishedTs more than once.
     */
    ExecutorFuture<void> applyUntilCloneFinishedTs(std::shared_ptr<executor::TaskExecutor> executor,
                                                   CancellationToken cancelToken,
                                                   CancelableOperationContextFactory factory);

    /**
     * Applies oplog from the iterator until it is exhausted or hits an error. It is an error to
     * call this without calling applyUntilCloneFinishedTs first.
     *
     * It is an error to call this when applyUntilCloneFinishedTs future returns an error.
     * It is undefined to call applyUntilDone more than once.
     */
    ExecutorFuture<void> applyUntilDone(std::shared_ptr<executor::TaskExecutor> executor,
                                        CancellationToken cancelToken,
                                        CancelableOperationContextFactory factory);

    static boost::optional<ReshardingOplogApplierProgress> checkStoredProgress(
        OperationContext* opCtx, const ReshardingSourceId& id);

    static NamespaceString ensureStashCollectionExists(OperationContext* opCtx,
                                                       const UUID& existingUUID,
                                                       const ShardId& donorShardId,
                                                       const CollectionOptions& options);

private:
    using OplogBatch = std::vector<repl::OplogEntry>;

    enum class Stage { kStarted, kErrorOccurred, kReachedCloningTS, kFinished };

    /**
     * Returns a future that becomes ready when the next batch of oplog entries have been collected
     * and applied.
     */
    ExecutorFuture<void> _scheduleNextBatch(std::shared_ptr<executor::TaskExecutor> executor,
                                            CancellationToken cancelToken,
                                            CancelableOperationContextFactory factory);

    /**
     * Setup the worker threads to apply the ops in the current buffer in parallel. Waits for all
     * worker threads to finish (even when some of them finished early due to an error).
     */
    SemiFuture<void> _applyBatch(std::shared_ptr<executor::TaskExecutor> executor,
                                 CancellationToken cancelToken,
                                 CancelableOperationContextFactory factory,
                                 bool isForSessionApplication);

    /**
     * Takes note that an error occurred and set the appropriate promise.
     *
     * Note: currently only supports being called on context where no other thread can modify
     * _stage variable.
     */
    Status _onError(Status status);

    /** The ServiceContext to use internally. */
    ServiceContext* _service() const {
        return _env->service();
    }

    /**
     * Records the progress made by this applier to storage. Returns the timestamp of the progress
     * recorded.
     */
    Timestamp _clearAppliedOpsAndStoreProgress(OperationContext* opCtx);

    static constexpr auto kClientName = "ReshardingOplogApplier"_sd;

    std::unique_ptr<Env> _env;

    // Identifier for the oplog source.
    const ReshardingSourceId _sourceId;

    // Namespace that contains the oplog from a source shard that this is going to apply.
    const NamespaceString _oplogNs;

    // Namespace of the real collection being resharded.
    const NamespaceString _nsBeingResharded;

    // UUID of the real collection being resharded.
    const UUID _uuidBeingResharded;

    // Namespace of collection where operations are going to get applied.
    const NamespaceString _outputNs;

    // The timestamp of the latest oplog entry on the source shard at the time when resharding
    // finished cloning from it.
    const Timestamp _reshardingCloneFinishedTs;

    const ReshardingOplogBatchPreparer _batchPreparer;

    const ReshardingOplogApplicationRules _crudApplication;
    const ReshardingOplogSessionApplication _sessionApplication;
    const ReshardingOplogBatchApplier _batchApplier;

    // Buffer for the current batch of oplog entries to apply.
    OplogBatch _currentBatchToApply;

    // Buffer for internally generated oplog entries that needs to be processed for this batch.
    std::list<repl::OplogEntry> _currentDerivedOps;

    // The source of the oplog entries to be applied.
    std::unique_ptr<ReshardingDonorOplogIteratorInterface> _oplogIter;

    // Tracks the current stage of this applier.
    Stage _stage{Stage::kStarted};
};

}  // namespace mongo
