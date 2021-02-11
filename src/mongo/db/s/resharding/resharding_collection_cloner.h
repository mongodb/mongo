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

#include <memory>
#include <vector>

#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/future.h"

namespace mongo {

namespace executor {

class TaskExecutor;

}  // namespace executor

class OperationContext;
class ServiceContext;

/**
 * Responsible for copying data from multiple source shards that will belong to this shard based on
 * the new resharding chunk distribution.
 */
class ReshardingCollectionCloner {
public:
    ReshardingCollectionCloner(ShardKeyPattern newShardKeyPattern,
                               NamespaceString sourceNss,
                               CollectionUUID sourceUUID,
                               ShardId recipientShard,
                               Timestamp atClusterTime,
                               NamespaceString outputNss);

    std::unique_ptr<Pipeline, PipelineDeleter> makePipeline(
        OperationContext* opCtx,
        std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
        Value resumeId = Value());

    ExecutorFuture<void> run(std::shared_ptr<executor::TaskExecutor> executor,
                             CancelationToken cancelToken);

private:
    std::unique_ptr<Pipeline, PipelineDeleter> _targetAggregationRequest(OperationContext* opCtx,
                                                                         const Pipeline& pipeline);

    std::vector<InsertStatement> _fillBatch(Pipeline& pipeline);
    void _insertBatch(OperationContext* opCtx, std::vector<InsertStatement>& batch);

    template <typename Callable>
    auto _withTemporaryOperationContext(Callable&& callable);

    const ShardKeyPattern _newShardKeyPattern;
    const NamespaceString _sourceNss;
    const CollectionUUID _sourceUUID;
    const ShardId _recipientShard;
    const Timestamp _atClusterTime;
    const NamespaceString _outputNss;
};

}  // namespace mongo
