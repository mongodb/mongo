/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/global_index/global_index_cloner_fetcher.h"

#include <fmt/format.h>

#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/s/resharding/document_source_resharding_ownership_match.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kGlobalIndex

namespace mongo {
namespace global_index {

namespace {

boost::intrusive_ptr<ExpressionContext> makeExpressionContext(OperationContext* opCtx,
                                                              const NamespaceString& nss,
                                                              const UUID& collUUID) {
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[NamespaceString::kRsOplogNamespace.coll()] = {
        NamespaceString::kRsOplogNamespace, std::vector<BSONObj>()};
    return make_intrusive<ExpressionContext>(opCtx,
                                             boost::none, /* explain */
                                             false,       /* fromMongos */
                                             false,       /* needsMerge */
                                             false,       /* allowDiskUse */
                                             false,       /* bypassDocumentValidation */
                                             false,       /* isMapReduceCommand */
                                             nss,
                                             boost::none, /* runtimeConstants */
                                             nullptr,     /* collator */
                                             MongoProcessInterface::create(opCtx),
                                             std::move(resolvedNamespaces),
                                             collUUID); /* collUUID */
}

BSONObj buildInitialReplaceRootForCloner(const KeyPattern& globalIndexKeyPattern,
                                         const KeyPattern& sourceShardKeyPattern) {
    BSONObjBuilder replaceRootBuilder;

    BSONObjBuilder idBuilder(replaceRootBuilder.subobjStart("_id"));
    for (const auto& globalIndexKey : globalIndexKeyPattern.toBSON()) {
        idBuilder.append(globalIndexKey.fieldName(), "${}"_format(globalIndexKey.fieldName()));
    }
    idBuilder.doneFast();

    // The next section tries to build the following documentKey expression:
    // {documentKey: {$arrayToObject: [[{k: '_id', v: '$_id'}, ...]]}}
    //
    // Note: $arrayToObject is used as a work around to output valid shard key patterns with dotted
    // field names.
    BSONObjBuilder docKeyBuilder(replaceRootBuilder.subobjStart("documentKey"));

    BSONArrayBuilder arrayToObjectArgumentPassingArrayBuilder(
        docKeyBuilder.subarrayStart("$arrayToObject"));
    BSONArrayBuilder arrayToObjectBuilder(arrayToObjectArgumentPassingArrayBuilder.subarrayStart());

    arrayToObjectBuilder.append(BSON("k"
                                     << "_id"
                                     << "v"
                                     << "$_id"));

    for (const auto& shardKey : sourceShardKeyPattern.toBSON()) {
        // Output missing fields with explicit null value otherwise $arrayToObject complains
        // "$arrayToObject requires an object keys of 'k' and 'v'. Found incorrect number of keys:1"
        BSONObj value(
            BSON("$ifNull" << BSON_ARRAY("${}"_format(shardKey.fieldName()) << BSONNULL)));
        arrayToObjectBuilder.append(BSON("k" << shardKey.fieldName() << "v" << value));
    }

    arrayToObjectBuilder.doneFast();
    arrayToObjectArgumentPassingArrayBuilder.doneFast();
    docKeyBuilder.doneFast();

    return replaceRootBuilder.obj();
}

Pipeline::SourceContainer buildPipelineForCloner(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ShardId& myShardId,
    const KeyPattern& globalIndexKeyPattern,
    const KeyPattern& sourceShardKeyPattern) {
    Pipeline::SourceContainer stages;

    // TODO: SERVER-67564 Add matcher for resumability

    stages.emplace_back(DocumentSourceSort::create(expCtx, BSON("_id" << 1)));

    stages.emplace_back(DocumentSourceReshardingOwnershipMatch::create(
        myShardId, ShardKeyPattern{globalIndexKeyPattern}, expCtx));

    const BSONObj replaceWithExpression =
        BSON("$replaceRoot" << BSON("newRoot" << buildInitialReplaceRootForCloner(
                                        globalIndexKeyPattern, sourceShardKeyPattern)));

    stages.emplace_back(
        DocumentSourceReplaceRoot::createFromBson(replaceWithExpression.firstElement(), expCtx));

    return stages;
}

}  // namespace

GlobalIndexClonerFetcher::GlobalIndexClonerFetcher(NamespaceString nss,
                                                   UUID collUUID,
                                                   UUID indexUUID,
                                                   ShardId myShardId,
                                                   Timestamp minFetchTimestamp,
                                                   KeyPattern sourceShardKeyPattern,
                                                   KeyPattern globalIndexPattern)
    : _nss(std::move(nss)),
      _collUUID(std::move(collUUID)),
      _indexUUID(std::move(indexUUID)),
      _myShardId(std::move(myShardId)),
      _minFetchTimestamp(std::move(minFetchTimestamp)),
      _sourceShardKeyPattern(std::move(sourceShardKeyPattern)),
      _globalIndexKeyPattern(std::move(globalIndexPattern)) {}

boost::optional<GlobalIndexClonerFetcher::FetchedEntry> GlobalIndexClonerFetcher::getNext(
    OperationContext* opCtx) {
    if (!_pipeline) {
        _pipeline = _restartPipeline(opCtx);
    }

    _pipeline->reattachToOperationContext(opCtx);
    ON_BLOCK_EXIT([this] { _pipeline->detachFromOperationContext(); });

    ScopeGuard guard([&] {
        _pipeline->dispose(opCtx);
        _pipeline.reset();
    });

    auto next = _pipeline->getNext();
    guard.dismiss();

    if (next) {
        auto nextBSON = next->toBson();

        auto idElement = nextBSON["_id"];

        BSONObjBuilder documentKeyBuilder;
        documentKeyBuilder.append(idElement);

        ShardKeyPattern sourceKey(_sourceShardKeyPattern);
        documentKeyBuilder.appendElementsUnique(sourceKey.extractShardKeyFromDoc(nextBSON));

        ShardKeyPattern globalIndexKey(_globalIndexKeyPattern);
        return GlobalIndexClonerFetcher::FetchedEntry{
            documentKeyBuilder.done(), globalIndexKey.extractShardKeyFromDoc(nextBSON)};
    }

    return boost::none;
}

std::unique_ptr<Pipeline, PipelineDeleter> GlobalIndexClonerFetcher::makePipeline(
    OperationContext* opCtx) {
    // Assume that the input collection isn't a view. The collectionUUID parameter to
    // the aggregate would enforce this anyway.
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[_nss.coll()] = {_nss, std::vector<BSONObj>{}};

    auto expCtx = makeExpressionContext(opCtx, _nss, _collUUID);
    auto pipelineStages =
        buildPipelineForCloner(expCtx, _myShardId, _globalIndexKeyPattern, _sourceShardKeyPattern);

    return Pipeline::create(std::move(pipelineStages), std::move(expCtx));
}

std::unique_ptr<Pipeline, PipelineDeleter> GlobalIndexClonerFetcher::_targetAggregationRequest(
    const Pipeline& pipeline) {
    auto opCtx = pipeline.getContext()->opCtx;
    // We associate the aggregation cursors established on each donor shard with a logical session
    // to prevent them from killing the cursor when it is idle locally. Due to the cursor's merging
    // behavior across all donor shards, it is possible for the cursor to be active on one donor
    // shard while idle for a long period on another donor shard.
    opCtx->setLogicalSessionId(makeLogicalSessionId(opCtx));

    AggregateCommandRequest request(_nss, pipeline.serializeToBson());
    request.setCollectionUUID(_collUUID);

    request.setReadConcern(BSON(repl::ReadConcernArgs::kLevelFieldName
                                << repl::readConcernLevels::kMajorityName
                                << repl::ReadConcernArgs::kAfterClusterTimeFieldName
                                << _minFetchTimestamp));

    // The read preference on the request is merely informational (e.g. for profiler entries) -- the
    // pipeline's opCtx setting is actually used when sending the request.
    auto readPref = ReadPreferenceSetting{ReadPreference::Nearest};
    request.setUnwrappedReadPref(readPref.toContainingBSON());
    ReadPreferenceSetting::get(opCtx) = readPref;

    return shardVersionRetry(opCtx,
                             Grid::get(opCtx)->catalogCache(),
                             _nss,
                             "targeting donor shards for global index collection cloning"_sd,
                             [&] {
                                 return sharded_agg_helpers::targetShardsAndAddMergeCursors(
                                     pipeline.getContext(), request);
                             });
}

std::unique_ptr<Pipeline, PipelineDeleter> GlobalIndexClonerFetcher::_restartPipeline(
    OperationContext* opCtx) {
    // The BlockingResultsMerger underlying by the $mergeCursors stage records how long the
    // recipient spent waiting for documents from the donor shards. It doing so requires the CurOp
    // to be marked as having started.
    auto* curOp = CurOp::get(opCtx);
    curOp->ensureStarted();
    ON_BLOCK_EXIT([curOp] { curOp->done(); });

    auto pipeline = _targetAggregationRequest(*makePipeline(opCtx));

    pipeline->detachFromOperationContext();
    pipeline.get_deleter().dismissDisposal();
    return pipeline;
}

}  // namespace global_index
}  // namespace mongo
