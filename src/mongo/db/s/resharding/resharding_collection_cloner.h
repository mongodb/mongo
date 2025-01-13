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

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/shard_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/uuid.h"

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
    ReshardingCollectionCloner(ReshardingMetrics* metrics,
                               const UUID& reshardingUUID,
                               ShardKeyPattern newShardKeyPattern,
                               NamespaceString sourceNss,
                               const UUID& sourceUUID,
                               ShardId recipientShard,
                               Timestamp atClusterTime,
                               NamespaceString outputNss,
                               bool storeProgress,
                               bool relaxed);

    std::pair<std::vector<BSONObj>, boost::intrusive_ptr<ExpressionContext>> makeRawPipeline(
        OperationContext* opCtx,
        std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
        Value resumeId = Value());

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
                         CancelableOperationContextFactory factory);

    /**
     * Fetches and inserts a single batch of documents.
     *
     * Returns true if there are more documents to be fetched and inserted, and returns false
     * otherwise.
     */
    bool doOneBatch(OperationContext* opCtx,
                    Pipeline& pipeline,
                    TxnNumber& txnNum,
                    ShardId donorShard,
                    HostAndPort donorHost,
                    BSONObj resumeToken,
                    // TODO(SERVER-77873): remove the useNaturalOrderCloner parameter.
                    bool useNaturalOrderCloner);

    /**
     * Inserts a single batch of documents and its resume information if provided.
     */
    void writeOneBatch(OperationContext* opCtx,
                       TxnNumber& txnNum,
                       std::vector<InsertStatement>& batch,
                       ShardId donorShard,
                       HostAndPort donorHost,
                       BSONObj resumeToken,
                       // TODO(SERVER-77873): remove the useNaturalOrderCloner parameter.
                       bool useNaturalOrderCloner);

private:
    std::unique_ptr<Pipeline, PipelineDeleter> _targetAggregationRequest(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    std::unique_ptr<Pipeline, PipelineDeleter> _restartPipeline(
        OperationContext* opCtx, std::shared_ptr<executor::TaskExecutor> executor);

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
};

}  // namespace mongo
