// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/hierarchical_cancelable_operation_context_factory.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace executor {

class TaskExecutor;

}  // namespace executor

class OperationContext;
class MongoProcessInterface;

class ReshardingMetrics;
class ServiceContext;

/**
 * Responsible for copying data from multiple source shards that will belong to this shard based on
 * the new resharding chunk distribution.
 */
class ReshardingCollectionCloner {
public:
    ReshardingCollectionCloner(
        ReshardingMetrics* metrics,
        const UUID& reshardingUUID,
        ShardKeyPattern newShardKeyPattern,
        NamespaceString sourceNss,
        const UUID& sourceUUID,
        ShardId recipientShard,
        Timestamp atClusterTime,
        NamespaceString outputNss,
        bool storeProgress,
        bool relaxed,
        boost::optional<ForwardableOperationMetadata> forwardableOpMetadata = boost::none);

    std::pair<std::vector<BSONObj>, boost::intrusive_ptr<ExpressionContext>>
    makeRawNaturalOrderPipeline(OperationContext* opCtx,
                                std::shared_ptr<MongoProcessInterface> mongoProcessInterface);

    /**
     * Schedules work to repeatedly fetch and insert batches of documents.
     *
     * Returns a future that becomes ready when either:
     *   (a) all documents have been fetched and inserted, or
     *   (b) the cancellation token was canceled due to a stepdown or abort.
     */
    SemiFuture<void> run(std::shared_ptr<executor::TaskExecutor> executor,
                         std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
                         CancellationToken cancelToken,
                         std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    /**
     * Inserts a single batch of documents and its resume information if provided.
     */
    void writeOneBatch(OperationContext* opCtx,
                       TxnNumber& txnNum,
                       std::vector<InsertStatement>& batch,
                       ShardId donorShard,
                       HostAndPort donorHost,
                       BSONObj resumeToken);

private:
    sharded_agg_helpers::DispatchShardPipelineResults _queryOnceWithNaturalOrder(
        OperationContext* opCtx, std::shared_ptr<MongoProcessInterface> mongoProcessInterface);

    void _writeOnceWithNaturalOrder(OperationContext* opCtx,
                                    std::shared_ptr<executor::TaskExecutor> executor,
                                    std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
                                    CancellationToken cancelToken,
                                    std::vector<OwnedRemoteCursor> remoteCursors);

    void _runOnceWithNaturalOrder(OperationContext* opCtx,
                                  std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
                                  std::shared_ptr<executor::TaskExecutor> executor,
                                  std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
                                  CancellationToken cancelToken);

    ReshardingMetrics* _metrics;
    const UUID _reshardingUUID;
    const ShardKeyPattern _newShardKeyPattern;
    const NamespaceString _sourceNss;
    const UUID _sourceUUID;
    const ShardId _recipientShard;
    const Timestamp _atClusterTime;
    const NamespaceString _outputNss;
    const bool _storeProgress;
    const bool _relaxed;
    const boost::optional<ForwardableOperationMetadata> _forwardableOpMetadata;

    Atomic<long long> _lastWriteBlockWarningAt{std::numeric_limits<long long>::min()};
};

}  // namespace mongo
