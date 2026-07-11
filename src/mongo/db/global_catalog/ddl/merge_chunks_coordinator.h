// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/ddl/chunk_operation_sharding_coordinator.h"
#include "mongo/db/global_catalog/ddl/merge_chunks_coordinator_document_gen.h"
#include "mongo/db/s/active_migrations_registry.h"

namespace mongo {

class MergeChunksCoordinator final
    : public ChunkOperationShardingCoordinator<MergeChunksCoordinatorDocument> {
public:
    MergeChunksCoordinator(ShardingCoordinatorService* service, const BSONObj& initialStateDoc);

    void checkIfOptionsConflict(const BSONObj& doc) const final;

    void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override;

protected:
    bool isInCriticalSection(Phase phase) const override;

    bool _mustAlwaysMakeProgress() override;

    ChunkOperationsStatistics::ChunkOperationType chunkOperationMetricType() const override {
        return ChunkOperationsStatistics::ChunkOperationType::kMergeChunks;
    }

private:
    ExecutorFuture<void> _acquireLocksAsync(OperationContext* opCtx,
                                            std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                            const CancellationToken& token) override;

    void _releaseLocks(OperationContext* opCtx) override;

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    ExecutorFuture<void> _cleanupOnAbort(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token,
                                         const Status& status) noexcept override;

    const ShardsvrMergeChunksRequest _request;
    boost::optional<ScopedSplitMergeChunk> _scopedSplitMergeChunk;

    // Reason document used to acquire and release the recoverable critical section.
    const BSONObj _critSecReason;

    // Set in kCheckPreconditions when the requested range already corresponds to a single chunk
    // on this shard — i.e. the merge is a no-op replay of an earlier successful commit. When
    // true the coordinator skips every subsequent phase (no critical section, no configsvr
    // commit, no shard catalog rewrite) and completes cleanly.
    bool _alreadyCommitted = false;
};

}  // namespace mongo
