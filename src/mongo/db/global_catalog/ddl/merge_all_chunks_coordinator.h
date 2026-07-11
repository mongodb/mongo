// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/ddl/chunk_operation_sharding_coordinator.h"
#include "mongo/db/global_catalog/ddl/merge_all_chunks_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/merge_chunk_request_gen.h"
#include "mongo/db/s/active_migrations_registry.h"

namespace mongo {

/**
 * Coordinator that drives a mergeAllChunks operation on a single shard for a given collection.
 *
 * For mutual exclusion against concurrent chunk operations (moveChunk / split / merge) on the
 * same namespace, the coordinator registers a ScopedSplitMergeChunk with the
 * ActiveMigrationsRegistry on the entire shard key space (Min, Max). Conflicts are detected at
 * the namespace level - the ChunkRange itself is stored only as informational state - so the
 * registration effectively acts as a per-collection guard for the lifetime of the coordinator.
 */
class MergeAllChunksCoordinator final
    : public ChunkOperationShardingCoordinator<MergeAllChunksCoordinatorDocument> {
public:
    MergeAllChunksCoordinator(ShardingCoordinatorService* service, const BSONObj& initialStateDoc);

    void checkIfOptionsConflict(const BSONObj& doc) const final;

    void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override;

    /**
     * Waits for the merge to complete and returns the response built from the config server reply.
     */
    MergeAllChunksOnShardResponse getResponse(OperationContext* opCtx) {
        getCompletionFuture().get(opCtx);
        tassert(12117910, "Expected _response to be set", _response);
        return *_response;
    }

protected:
    bool isInCriticalSection(Phase phase) const override;

    bool _mustAlwaysMakeProgress() override;

    ChunkOperationsStatistics::ChunkOperationType chunkOperationMetricType() const override {
        return ChunkOperationsStatistics::ChunkOperationType::kMergeAllChunks;
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

    void _onCleanup(OperationContext* opCtx) override;

    const ShardsvrMergeAllChunksOnShardRequest _request;
    boost::optional<MergeAllChunksOnShardResponse> _response;
    boost::optional<ScopedSplitMergeChunk> _scopedSplitMergeChunk;

    bool _cleaningUp = false;

    // Reason document used to acquire and release the recoverable critical section.
    const BSONObj _critSecReason;
};

}  // namespace mongo
