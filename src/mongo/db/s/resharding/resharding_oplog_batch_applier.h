// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/hierarchical_cancelable_operation_context_factory.h"
#include "mongo/db/s/resharding/resharding_oplog_batch_preparer.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <limits>
#include <memory>

namespace mongo {

class ReshardingOplogApplicationRules;

class ReshardingOplogSessionApplication;

/**
 * Updates this shard's data based on oplog entries that already executed on some donor shard.
 *
 * Instances of this class are thread-safe.
 */
class ReshardingOplogBatchApplier {
public:
    using OplogBatch = ReshardingOplogBatchPreparer::OplogBatchToApply;

    ReshardingOplogBatchApplier(const ReshardingOplogApplicationRules& crudApplication,
                                const ReshardingOplogSessionApplication& sessionApplication);

    template <bool IsForSessionApplication>
    SemiFuture<void> applyBatch(
        OplogBatch batch,
        std::shared_ptr<executor::TaskExecutor> executor,
        CancellationToken cancelToken,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) const;

    /**
     * When set, each opCtx created by applyBatch() will have ReplicaSetWriteBlockBypass enabled,
     * allowing catch-up writes to bypass blockReplicaSetWrites. Called when the recipient enters
     * the critical section (kBlockingWrites), after which the oplog delta is bounded and must be
     * drained even if a replica set write block is active.
     */
    void setReplicaSetWriteBlockBypass() const;

private:
    /**
     * Helper to construct an opCtx and set non-deprioritizable state. Since this class exists
     * both outside of and within the critical section but has no concept of the resharding phases,
     * it is always non-deprioritizable.
     */
    CancelableOperationContext _makeOperationContext(
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) const;

    const ReshardingOplogApplicationRules& _crudApplication;
    const ReshardingOplogSessionApplication& _sessionApplication;

    mutable Atomic<bool> _bypassWriteBlock{false};
    mutable Atomic<long long> _lastWriteBlockWarningAt{std::numeric_limits<long long>::min()};
};

}  // namespace mongo
