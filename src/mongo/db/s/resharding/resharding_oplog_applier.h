// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/hierarchical_cancelable_operation_context_factory.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/db/s/resharding/resharding_donor_oplog_iterator.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_oplog_application.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_metrics.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_progress_gen.h"
#include "mongo/db/s/resharding/resharding_oplog_batch_applier.h"
#include "mongo/db/s/resharding/resharding_oplog_batch_preparer.h"
#include "mongo/db/s/resharding/resharding_oplog_session_application.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <list>
#include <memory>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

class ServiceContext;

/**
 * Applies oplog entries from a specific donor for resharding.
 *
 * @param sourceId combines the resharding run's UUID with the donor ShardId.
 * @param allStashNss are the namespaces of the stash collections. There is one stash collection for
 *                    each donor. This ReshardingOplogApplier will write documents as necessary to
 *                    the stash collection at `myStashIdx` and may need to read and delete documents
 *                    from any of the other stash collections.
 * @param myStashIdx -- see above.
 *
 * This is not thread safe.
 */
class ReshardingOplogApplier {
public:
    class Env {
    public:
        Env(ServiceContext* service, ReshardingOplogApplierMetrics* applierMetrics)
            : _service(service), _applierMetrics(applierMetrics) {}

        ServiceContext* service() const {
            return _service;
        }

        ReshardingOplogApplierMetrics* applierMetrics() {
            return _applierMetrics;
        }

    private:
        ServiceContext* _service;
        ReshardingOplogApplierMetrics* _applierMetrics;
    };

    ReshardingOplogApplier(
        std::unique_ptr<Env> env,
        std::size_t oplogBatchTaskCount,
        ReshardingSourceId sourceId,
        NamespaceString oplogBufferNss,
        NamespaceString outputNss,
        std::vector<NamespaceString> allStashNss,
        size_t myStashIdx,
        ChunkManager sourceChunkMgr,
        std::unique_ptr<ReshardingDonorOplogIteratorInterface> oplogIterator,
        bool isCapped = false,
        boost::optional<ForwardableOperationMetadata> forwardableOpMetadata = boost::none);

    /**
     * Schedules work to repeatedly apply batches of oplog entries from a donor shard.
     *
     * Returns a future that becomes ready when either:
     *   (a) all documents have been applied, or
     *   (b) the cancellation token was canceled due to a stepdown or abort.
     */
    SemiFuture<void> run(std::shared_ptr<executor::TaskExecutor> executor,
                         std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
                         CancellationToken cancelToken,
                         std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    static boost::optional<ReshardingOplogApplierProgress> checkStoredProgress(
        OperationContext* opCtx, const ReshardingSourceId& id);

    void setReplicaSetWriteBlockBypass();

    static NamespaceString ensureStashCollectionExists(OperationContext* opCtx,
                                                       const UUID& existingUUID,
                                                       const ShardId& donorShardId,
                                                       const CollectionOptions& options);

private:
    using OplogBatch = std::vector<repl::OplogEntry>;

    /**
     * Helper to construct an opCtx and set non-deprioritizable state. Since this class exists
     * both outside of and within the critical section but has no concept of the resharding phases,
     * it is always non-deprioritizable.
     */
    CancelableOperationContext _makeOperationContext(
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) const;

    /**
     * Setup the worker threads to apply the ops in the current buffer in parallel. Waits for all
     * worker threads to finish (even when some of them finished early due to an error).
     */
    SemiFuture<void> _applyBatch(
        std::shared_ptr<executor::TaskExecutor> executor,
        CancellationToken cancelToken,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    /**
     * Records the progress made by this applier to storage.
     */
    void _clearAppliedOpsAndStoreProgress(OperationContext* opCtx);

    /**
     * Returns true if the recipient has been configured to estimate the remaining time based on
     * the exponential moving average of the time it takes to fetch and apply oplog entries.
     */
    bool _needToEstimateRemainingTimeBasedOnMovingAverage(OperationContext* opCtx);

    /**
     * Updates the average time to apply oplog entries based on the last oplog entry in the current
     * batch if it is not empty.
     */
    void _updateAverageTimeToApplyOplogEntries(OperationContext* opCtx);

    std::unique_ptr<Env> _env;

    // Identifier for the oplog source.
    const ReshardingSourceId _sourceId;

    const ReshardingOplogBatchPreparer _batchPreparer;

    const ReshardingOplogApplicationRules _crudApplication;
    const ReshardingOplogSessionApplication _sessionApplication;
    const ReshardingOplogBatchApplier _batchApplier;

    // Buffer for the current batch of oplog entries to apply.
    OplogBatch _currentBatchToApply;

    // Buffer for internally generated oplog entries that needs to be processed for this batch.
    std::list<repl::OplogEntry> _currentDerivedOpsForCrudWriters;
    std::list<repl::OplogEntry> _currentDerivedOpsForSessionWriters;

    // The source of the oplog entries to be applied.
    std::unique_ptr<ReshardingDonorOplogIteratorInterface> _oplogIter;

    const boost::optional<ForwardableOperationMetadata> _forwardableOpMetadata;
    boost::optional<bool> _supportEstimatingRemainingTimeBasedOnMovingAverage;
};

}  // namespace mongo
