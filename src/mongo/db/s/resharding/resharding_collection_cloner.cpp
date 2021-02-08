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
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/s/stale_shard_version_helpers.h"
#include "mongo/util/future_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

bool collectionHasSimpleCollation(OperationContext* opCtx, const NamespaceString& nss) {
    auto catalogCache = Grid::get(opCtx)->catalogCache();
    auto sourceChunkMgr = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));

    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Expected collection " << nss << " to be sharded",
            sourceChunkMgr.isSharded());

    return !sourceChunkMgr.getDefaultCollator();
}

}  // namespace

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
    OperationContext* opCtx,
    std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
    Value resumeId) {
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

    // sharded_agg_helpers::targetShardsAndAddMergeCursors() ignores the collation set on the
    // AggregationRequest (or lack thereof) and instead only considers the collator set on the
    // ExpressionContext. Setting nullptr as the collator on the ExpressionContext means that the
    // aggregation pipeline is always using the "simple" collation, even when the collection default
    // collation for _sourceNss is non-simple. The chunk ranges in the $lookup stage must be
    // compared using the simple collation because collections are always sharded using the simple
    // collation. However, resuming by _id is only efficient (i.e. non-blocking seek/sort) when the
    // aggregation pipeline would be using the collection's default collation. We cannot do both so
    // we choose to disallow automatic resuming for collections with non-simple default collations.
    uassert(4929303,
            "Cannot resume cloning when sharded collection has non-simple default collation",
            resumeId.missing() || collectionHasSimpleCollation(opCtx, _sourceNss));

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

    if (!resumeId.missing()) {
        stages.emplace_back(DocumentSourceMatch::create(
            Doc{{"$expr",
                 Doc{{"$gte", Arr{V{"$_id"_sd}, V{Doc{{"$literal", std::move(resumeId)}}}}}}}}
                .toBson(),
            expCtx));
    }

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

    // We use $arrayToObject to synthesize the $sortKeys needed by the AsyncResultsMerger to merge
    // the results from all of the donor shards by {_id: 1}. This expression wouldn't be correct if
    // the aggregation pipeline was using a non-"simple" collation.
    stages.emplace_back(
        DocumentSourceReplaceRoot::createFromBson(fromjson("{$replaceWith: {$mergeObjects: [\
            '$original',\
            {$arrayToObject: {$concatArrays: [[{\
                k: {$literal: '$sortKey'},\
                v: ['$original._id']\
            }]]}}\
        ]}}")
                                                      .firstElement(),
                                                  expCtx));

    return Pipeline::create(std::move(stages), std::move(expCtx));
}

Value ReshardingCollectionCloner::_findHighestInsertedId(OperationContext* opCtx) {
    AutoGetCollection outputColl(opCtx, _outputNss, MODE_IS);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Resharding collection cloner's output collection '" << _outputNss
                          << "' did not already exist",
            outputColl);

    auto qr = std::make_unique<QueryRequest>(_outputNss);
    qr->setLimit(1);
    qr->setSort(BSON("_id" << -1));

    auto recordId = Helpers::findOne(opCtx, *outputColl, std::move(qr), true /* requireIndex */);
    if (recordId.isNull()) {
        return Value{};
    }

    auto doc = outputColl->docFor(opCtx, recordId).value();
    auto value = Value{doc["_id"]};
    uassert(4929300,
            "Missing _id field for document in temporary resharding collection",
            !value.missing());

    return value;
}

std::unique_ptr<Pipeline, PipelineDeleter> ReshardingCollectionCloner::_targetAggregationRequest(
    OperationContext* opCtx, const Pipeline& pipeline) {
    AggregateCommand request(_sourceNss, pipeline.serializeToBson());
    request.setCollectionUUID(_sourceUUID);

    auto hint = collectionHasSimpleCollation(opCtx, _sourceNss)
        ? boost::optional<BSONObj>{BSON("_id" << 1)}
        : boost::none;

    if (hint) {
        request.setHint(*hint);
    }

    request.setReadConcern(BSON(repl::ReadConcernArgs::kLevelFieldName
                                << repl::readConcernLevels::kSnapshotName
                                << repl::ReadConcernArgs::kAtClusterTimeFieldName
                                << _atClusterTime));
    request.setUnwrappedReadPref(ReadPreferenceSetting{ReadPreference::Nearest}.toContainingBSON());

    return shardVersionRetry(opCtx,
                             Grid::get(opCtx)->catalogCache(),
                             _sourceNss,
                             "targeting donor shards for resharding collection cloning"_sd,
                             [&] {
                                 // We use the hint as an implied sort for $mergeCursors because
                                 // the aggregation pipeline synthesizes the necessary $sortKeys
                                 // fields in the result set.
                                 return sharded_agg_helpers::targetShardsAndAddMergeCursors(
                                     pipeline.getContext(), request, hint);
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
auto ReshardingCollectionCloner::_withTemporaryOperationContext(Callable&& callable) {
    auto& client = cc();
    {
        stdx::lock_guard<Client> lk(client);
        invariant(client.canKillSystemOperationInStepdown(lk));
    }

    auto opCtx = client.makeOperationContext();
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

ExecutorFuture<void> ReshardingCollectionCloner::run(
    std::shared_ptr<executor::TaskExecutor> executor, CancelationToken cancelToken) {
    struct ChainContext {
        std::unique_ptr<Pipeline, PipelineDeleter> pipeline;
        bool moreToCome = true;
    };

    auto chainCtx = std::make_shared<ChainContext>();

    return AsyncTry([this, chainCtx] {
               if (!chainCtx->pipeline) {
                   chainCtx->pipeline = _withTemporaryOperationContext([&](auto* opCtx) {
                       auto idToResumeFrom = _findHighestInsertedId(opCtx);
                       auto pipeline = _targetAggregationRequest(
                           opCtx,
                           *makePipeline(
                               opCtx, MongoProcessInterface::create(opCtx), idToResumeFrom));

                       if (!idToResumeFrom.missing()) {
                           // Skip inserting the first document retrieved after resuming because
                           // $gte was used in the aggregation pipeline.
                           auto firstDoc = pipeline->getNext();
                           uassert(4929301,
                                   str::stream()
                                       << "Expected pipeline to retrieve document with _id: "
                                       << redact(idToResumeFrom.toString()),
                                   firstDoc);

                           // Note that the following uassert() could throw because we're using the
                           // simple string comparator and the collection could have a non-simple
                           // collation. However, it would still be correct to throw an exception
                           // because it would mean the collection being resharded contains multiple
                           // documents with the same _id value as far as global uniqueness is
                           // concerned.
                           const auto& firstId = (*firstDoc)["_id"];
                           uassert(4929302,
                                   str::stream()
                                       << "Expected pipeline to retrieve document with _id: "
                                       << redact(idToResumeFrom.toString())
                                       << ", but got _id: " << redact(firstId.toString()),
                                   ValueComparator::kInstance.evaluate(firstId == idToResumeFrom));
                       }

                       pipeline->detachFromOperationContext();
                       pipeline.get_deleter().dismissDisposal();
                       return pipeline;
                   });
               }

               chainCtx->moreToCome = _withTemporaryOperationContext([&](auto* opCtx) {
                   chainCtx->pipeline->reattachToOperationContext(opCtx);
                   auto batch = _fillBatch(*chainCtx->pipeline);
                   chainCtx->pipeline->detachFromOperationContext();

                   if (batch.empty()) {
                       return false;
                   }

                   _insertBatch(opCtx, batch);
                   return true;
               });
           })
        .until([this, chainCtx](Status status) {
            if (status.isOK() && chainCtx->moreToCome) {
                return false;
            }

            if (chainCtx->pipeline) {
                _withTemporaryOperationContext([&](auto* opCtx) {
                    chainCtx->pipeline->dispose(opCtx);
                    chainCtx->pipeline.reset();
                });
            }

            if (status.isA<ErrorCategory::CancelationError>() ||
                status.isA<ErrorCategory::NotPrimaryError>()) {
                // Cancellation and NotPrimary errors indicate the primary-only service Instance
                // will be shut down or is shutting down now. Don't retry and leave resuming to when
                // the RecipientStateMachine is restarted on the new primary.
                return true;
            }

            if (status.isA<ErrorCategory::RetriableError>() ||
                status.isA<ErrorCategory::CursorInvalidatedError>() ||
                status == ErrorCodes::Interrupted) {
                // Do retry on any other types of retryable errors though. Also retry on errors from
                // stray killCursors and killOp commands being run.
                LOGV2(5269300,
                      "Transient error while cloning sharded collection",
                      "sourceNamespace"_attr = _sourceNss,
                      "outputNamespace"_attr = _outputNss,
                      "readTimestamp"_attr = _atClusterTime,
                      "error"_attr = redact(status));
                return false;
            }

            if (!status.isOK()) {
                LOGV2(5352400,
                      "Operation-fatal error for resharding while cloning sharded collection",
                      "sourceNamespace"_attr = _sourceNss,
                      "outputNamespace"_attr = _outputNss,
                      "readTimestamp"_attr = _atClusterTime,
                      "error"_attr = redact(status));
            }

            return true;
        })
        .on(std::move(executor), std::move(cancelToken));
}

}  // namespace mongo
