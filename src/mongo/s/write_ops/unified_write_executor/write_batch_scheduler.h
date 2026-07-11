// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/sharding_environment/mongos_server_parameters_gen.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/s/write_ops/unified_write_executor/write_batch_executor.h"
#include "mongo/s/write_ops/unified_write_executor/write_batch_response_processor.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_batcher.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace unified_write_executor {

class WriteBatchScheduler {
public:
    using CollectionsToCreate = ProcessorResult::CollectionsToCreate;

    struct RoundResult {
        bool madeProgress;
        bool metadataRefreshed;
    };

    WriteBatchScheduler(WriteCommandRef cmdRef,
                        WriteOpBatcher& batcher,
                        WriteBatchExecutor& executor,
                        WriteBatchResponseProcessor& processor,
                        boost::optional<OID> targetEpoch)
        : _cmdRef(std::move(cmdRef)),
          _nssSet(_cmdRef.getNssSet()),
          _batcher(batcher),
          _executor(executor),
          _processor(processor),
          _targetEpoch(targetEpoch) {}

    void run(OperationContext* opCtx);

protected:
    /**
     * Helper method for executing a batch and processing the responses. Returns a RoundResult
     * where 'madeProgress' is true if at least one op completed successfully, and
     * 'metadataRefreshed' is true if the routing cache returned a different ShardVersion or
     * DatabaseVersion compared to the stale versions from the previous round.
     *
     * This method takes care of marking ops for re-processing as needed, it will create collections
     * as needed if there were any CannotImplicitlyCreateCollection errors, and it will also inform
     * the batcher to stop making batches if an unrecoverable error has occurred.
     */
    RoundResult executeRound(OperationContext* opCtx);

    /**
     * Helper method for creating a RoutingContext.
     */
    StatusWith<std::unique_ptr<RoutingContext>> initRoutingContext(
        OperationContext* opCtx, const std::vector<NamespaceString>& nssList);

    /**
     * Helper method for handling the case where RoutingContext creation failed.
     */
    void handleInitRoutingContextError(OperationContext* opCtx, const Status& status);

    /**
     * This method is records errors for the remaining ops. If the write command is ordered or
     * running in a transaction, this method will only record one error (for the remaining op with
     * the lowest ID). Otherwise, this method will record errors for all remaining ops.
     */
    void recordErrorForRemainingOps(OperationContext* opCtx, const Status& status);

    /**
     * Helper method that calls getNextBatch(), handles target errors that occurred during batch
     * creation (if any), and then returns the next batch.
     */
    WriteBatch getNextBatchAndHandleTargetErrors(OperationContext* opCtx,
                                                 RoutingContext& routingCtx);

    /**
     * This method calls release() on 'routingCtx' for the appropriate namespaces in preparation
     * for executing the specified 'batch'.
     */
    void prepareRoutingContext(RoutingContext& routingCtx,
                               const std::vector<NamespaceString>& nssList,
                               const WriteBatch& batch);

    /**
     * Helper method for creating a collection.
     */
    bool createCollection(OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Helper method for creating multiple collections.
     */
    bool createCollections(OperationContext* opCtx, const CollectionsToCreate& collsToCreate);

    const int32_t _kMaxRoundsWithoutProgress = gMaxRoundsWithoutProgress.loadRelaxed();
    const WriteCommandRef _cmdRef;
    const std::set<NamespaceString> _nssSet;
    WriteOpBatcher& _batcher;
    WriteBatchExecutor& _executor;
    WriteBatchResponseProcessor& _processor;
    boost::optional<OID> _targetEpoch;

private:
    // The StaleConfig receivedVersion and originating ShardRef per namespace from the previous
    // round. Compared against the new RoutingContext's shard-specific version to detect productive
    // StaleConfig refreshes.
    stdx::unordered_map<NamespaceString, std::pair<ShardRef, ShardVersion>> _prevStaleVersions;

    // The StaleDbVersion receivedVersion per database from the previous round, compared against
    // the current round's DatabaseVersion to detect productive database-version refreshes.
    stdx::unordered_map<DatabaseName, DatabaseVersion> _prevStaleDbVersions;
};

}  // namespace unified_write_executor
}  // namespace mongo
