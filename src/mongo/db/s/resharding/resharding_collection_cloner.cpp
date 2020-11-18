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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_collection_cloner.h"

#include <utility>

#include "mongo/bson/json.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

namespace mongo {

ReshardingCollectionCloner::ReshardingCollectionCloner(ShardKeyPattern newShardKeyPattern,
                                                       NamespaceString sourceNss,
                                                       CollectionUUID sourceUUID,
                                                       ShardId recipientShard,
                                                       Timestamp atClusterTime,
                                                       NamespaceString outputNss)
    : _newShardKeyPattern(std::move(newShardKeyPattern)),
      _sourceNss(std::move(sourceNss)),
      _sourceUUID(std::move(sourceUUID)),
      _recipientShard(std::move(recipientShard)),
      _atClusterTime(atClusterTime),
      _outputNss(std::move(outputNss)) {}

std::unique_ptr<Pipeline, PipelineDeleter> ReshardingCollectionCloner::makePipeline(
    OperationContext* opCtx, std::shared_ptr<MongoProcessInterface> mongoProcessInterface) {
    using Doc = Document;
    using Arr = std::vector<Value>;
    using V = Value;

    // Assume that the input collection isn't a view. The collectionUUID parameter to
    // the aggregate would enforce this anyway.
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[_sourceNss.coll()] = {_sourceNss, std::vector<BSONObj>{}};

    // Assume that the config.cache.chunks collection isn't a view either.
    auto tempNss = constructTemporaryReshardingNss(_sourceNss.db(), _sourceUUID);
    auto tempCacheChunksNss =
        NamespaceString(NamespaceString::kConfigDb, "cache.chunks." + tempNss.ns());
    resolvedNamespaces[tempCacheChunksNss.coll()] = {tempCacheChunksNss, std::vector<BSONObj>{}};

    auto expCtx = make_intrusive<ExpressionContext>(opCtx,
                                                    boost::none, /* explain */
                                                    false,       /* fromMongos */
                                                    false,       /* needsMerge */
                                                    false,       /* allowDiskUse */
                                                    false,       /* bypassDocumentValidation */
                                                    false,       /* isMapReduceCommand */
                                                    _sourceNss,
                                                    boost::none, /* runtimeConstants */
                                                    nullptr,     /* collator */
                                                    std::move(mongoProcessInterface),
                                                    std::move(resolvedNamespaces),
                                                    _sourceUUID);

    Pipeline::SourceContainer stages;

    stages.emplace_back(DocumentSourceReplaceRoot::createFromBson(
        fromjson("{$replaceWith: {original: '$$ROOT'}}").firstElement(), expCtx));

    Arr extractShardKeyExpr;
    for (auto&& field : _newShardKeyPattern.toBSON()) {
        if (ShardKeyPattern::isHashedPatternEl(field)) {
            extractShardKeyExpr.emplace_back(
                Doc{{"$toHashedIndexKey", "$original." + field.fieldNameStringData()}});
        } else {
            extractShardKeyExpr.emplace_back("$original." + field.fieldNameStringData());
        }
    }

    stages.emplace_back(DocumentSourceLookUp::createFromBson(
        Doc{{"$lookup",
             Doc{{"from",
                  Doc{{"db", tempCacheChunksNss.db()}, {"coll", tempCacheChunksNss.coll()}}},
                 {"let", Doc{{"sk", extractShardKeyExpr}}},
                 {"pipeline",
                  Arr{V{Doc{{"$match",
                             Doc{{"$expr",
                                  Doc{{"$eq",
                                       Arr{V{"$shard"_sd}, V{_recipientShard.toString()}}}}}}}}},
                      V{Doc(fromjson("{$match: {$expr: {$let: {\
                            vars: {\
                                min: {$map: {input: {$objectToArray: '$_id'}, in: '$$this.v'}},\
                                max: {$map: {input: {$objectToArray: '$max'}, in: '$$this.v'}}\
                            },\
                            in: {$and: [\
                                {$gte: ['$$sk', '$$min']},\
                                {$cond: {\
                                    if: {$allElementsTrue: [{$map: {\
                                        input: '$$max',\
                                        in: {$eq: [{$type: '$$this'}, 'maxKey']}\
                                    }}]},\
                                    then: {$lte: ['$$sk', '$$max']},\
                                    else: {$lt:  ['$$sk', '$$max']}\
                                }}\
                            ]}\
                        }}}}"))}}},
                 {"as", "intersectingChunk"_sd}}}}
            .toBson()
            .firstElement(),
        expCtx));

    stages.emplace_back(
        DocumentSourceMatch::create(fromjson("{intersectingChunk: {$ne: []}}"), expCtx));
    stages.emplace_back(DocumentSourceReplaceRoot::createFromBson(
        fromjson("{$replaceWith: '$original'}").firstElement(), expCtx));
    return Pipeline::create(std::move(stages), expCtx);
}

std::unique_ptr<Pipeline, PipelineDeleter> ReshardingCollectionCloner::_targetAggregationRequest(
    OperationContext* opCtx, const Pipeline& pipeline) {
    AggregationRequest request(_sourceNss, pipeline.serializeToBson());
    request.setCollectionUUID(_sourceUUID);
    request.setHint(BSON("_id" << 1));
    request.setReadConcern(BSON(repl::ReadConcernArgs::kLevelFieldName
                                << repl::readConcernLevels::kSnapshotName
                                << repl::ReadConcernArgs::kAtClusterTimeFieldName
                                << _atClusterTime));
    // TODO SERVER-52692: Set read preference to nearest.
    // request.setUnwrappedReadPref();

    return sharded_agg_helpers::shardVersionRetry(
        opCtx,
        Grid::get(opCtx)->catalogCache(),
        _sourceNss,
        "targeting donor shards for resharding collection cloning"_sd,
        [&] {
            return sharded_agg_helpers::targetShardsAndAddMergeCursors(pipeline.getContext(),
                                                                       request);
        });
}

std::vector<InsertStatement> ReshardingCollectionCloner::_fillBatch(Pipeline& pipeline) {
    std::vector<InsertStatement> batch;

    int numBytes = 0;
    do {
        auto doc = pipeline.getNext();
        if (!doc) {
            break;
        }

        auto obj = doc->toBson();
        batch.emplace_back(obj.getOwned());
        numBytes += obj.objsize();
    } while (numBytes < resharding::gReshardingCollectionClonerBatchSizeInBytes);

    return batch;
}

void ReshardingCollectionCloner::_insertBatch(OperationContext* opCtx,
                                              std::vector<InsertStatement>& batch) {
    writeConflictRetry(opCtx, "ReshardingCollectionCloner::_insertBatch", _outputNss.ns(), [&] {
        AutoGetCollection outputColl(opCtx, _outputNss, MODE_IX);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Resharding collection cloner's output collection '" << _outputNss
                              << "' did not already exist",
                outputColl);
        WriteUnitOfWork wuow(opCtx);

        // Populate 'slots' with new optimes for each insert.
        // This also notifies the storage engine of each new timestamp.
        auto oplogSlots = repl::getNextOpTimes(opCtx, batch.size());
        for (auto [insert, slot] = std::make_pair(batch.begin(), oplogSlots.begin());
             slot != oplogSlots.end();
             ++insert, ++slot) {
            invariant(insert != batch.end());
            insert->oplogSlot = *slot;
        }

        uassertStatusOK(outputColl->insertDocuments(opCtx, batch.begin(), batch.end(), nullptr));
        wuow.commit();
    });
}

/**
 * Invokes the 'callable' function with a fresh OperationContext.
 *
 * The OperationContext is configured so the RstlKillOpThread would always interrupt the operation
 * on step-up or stepdown, regardless of whether the operation has acquired any locks. This
 * interruption is best-effort to stop doing wasteful work on stepdown as quickly as possible. It
 * isn't required for the ReshardingCollectionCloner's correctness. In particular, it is possible
 * for an OperationContext to be constructed after stepdown has finished, for the
 * ReshardingCollectionCloner to run a getMore on the aggregation against the donor shards, and for
 * the ReshardingCollectionCloner to only discover afterwards the recipient had already stepped down
 * from a NotPrimary error when inserting a batch of documents locally.
 *
 * Note that the recipient's primary-only service is responsible for managing the
 * ReshardingCollectionCloner and would shut down the ReshardingCollectionCloner's task executor
 * following the recipient stepping down.
 *
 * Also note that the ReshardingCollectionCloner is only created after step-up as part of the
 * recipient's primary-only service and therefore would never be interrupted by step-up.
 */
template <typename Callable>
auto ReshardingCollectionCloner::_withTemporaryOperationContext(ServiceContext* serviceContext,
                                                                Callable&& callable) {
    auto* client = Client::getCurrent();
    {
        stdx::lock_guard<Client> lk(*client);
        invariant(client->canKillSystemOperationInStepdown(lk));
    }

    auto opCtx = client->makeOperationContext();
    opCtx->setAlwaysInterruptAtStepDownOrUp();

    // The BlockingResultsMerger underlying by the $mergeCursors stage records how long the
    // recipient spent waiting for documents from the donor shards. It doing so requires the CurOp
    // to be marked as having started.
    auto* curOp = CurOp::get(opCtx.get());
    curOp->ensureStarted();
    {
        ON_BLOCK_EXIT([curOp] { curOp->done(); });
        return callable(opCtx.get());
    }
}

ExecutorFuture<void> ReshardingCollectionCloner::_insertBatchesUntilPipelineExhausted(
    ServiceContext* serviceContext,
    std::shared_ptr<executor::TaskExecutor> executor,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline) {
    bool moreToCome = _withTemporaryOperationContext(serviceContext, [&](auto* opCtx) {
        pipeline->reattachToOperationContext(opCtx);
        auto batch = _fillBatch(*pipeline);
        pipeline->detachFromOperationContext();

        if (batch.empty()) {
            return false;
        }

        _insertBatch(opCtx, batch);
        return true;
    });

    if (!moreToCome) {
        return ExecutorFuture(std::move(executor));
    }

    return ExecutorFuture(executor, std::move(pipeline))
        .then([this, serviceContext, executor](auto pipeline) {
            return _insertBatchesUntilPipelineExhausted(
                serviceContext, std::move(executor), std::move(pipeline));
        });
}

ExecutorFuture<void> ReshardingCollectionCloner::run(
    ServiceContext* serviceContext, std::shared_ptr<executor::TaskExecutor> executor) {
    return ExecutorFuture(executor)
        .then([this, serviceContext] {
            return _withTemporaryOperationContext(serviceContext, [&](auto* opCtx) {
                auto pipeline = _targetAggregationRequest(
                    opCtx, *makePipeline(opCtx, MongoProcessInterface::create(opCtx)));

                pipeline->detachFromOperationContext();
                return pipeline;
            });
        })
        .then([this, serviceContext, executor](auto pipeline) {
            return _insertBatchesUntilPipelineExhausted(
                serviceContext, std::move(executor), std::move(pipeline));
        });
}

}  // namespace mongo
