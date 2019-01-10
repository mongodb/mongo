
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/mongos_process_interface.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/scoped_collection_metadata.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_query_knobs.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/query/router_exec_stage.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(clusterAggregateHangBeforeEstablishingShardCursors);

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;
using std::unique_ptr;

namespace {
// Given a document representing an aggregation command such as
//
//   {aggregate: "myCollection", pipeline: [], ...},
//
// produces the corresponding explain command:
//
//   {explain: {aggregate: "myCollection", pipline: [], ...}, $queryOptions: {...}, verbosity: ...}
Document wrapAggAsExplain(Document aggregateCommand, ExplainOptions::Verbosity verbosity) {
    MutableDocument explainCommandBuilder;
    explainCommandBuilder["explain"] = Value(aggregateCommand);
    // Downstream host targeting code expects queryOptions at the top level of the command object.
    explainCommandBuilder[QueryRequest::kUnwrappedReadPrefField] =
        Value(aggregateCommand[QueryRequest::kUnwrappedReadPrefField]);

    // readConcern needs to be promoted to the top-level of the request.
    explainCommandBuilder[repl::ReadConcernArgs::kReadConcernFieldName] =
        Value(aggregateCommand[repl::ReadConcernArgs::kReadConcernFieldName]);

    // Add explain command options.
    for (auto&& explainOption : ExplainOptions::toBSON(verbosity)) {
        explainCommandBuilder[explainOption.fieldNameStringData()] = Value(explainOption);
    }

    return explainCommandBuilder.freeze();
}

std::vector<RemoteCursor> establishShardCursors(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const LiteParsedPipeline& litePipe,
    boost::optional<CachedCollectionRoutingInfo>& routingInfo,
    const BSONObj& cmdObj,
    const AggregationRequest& request,
    const ReadPreferenceSetting& readPref,
    const BSONObj& shardQuery) {
    LOG(1) << "Dispatching command " << redact(cmdObj) << " to establish cursors on shards";

    const bool mustRunOnAll = MongoSInterface::mustRunOnAllShards(nss, litePipe);
    std::set<ShardId> shardIds = MongoSInterface::getTargetedShards(
        opCtx, mustRunOnAll, routingInfo, shardQuery, request.getCollation());
    std::vector<std::pair<ShardId, BSONObj>> requests;

    // If we don't need to run on all shards, then we should always have a valid routing table.
    invariant(routingInfo || mustRunOnAll);

    if (mustRunOnAll) {
        // The pipeline contains a stage which must be run on all shards. Skip versioning and
        // enqueue the raw command objects.
        for (auto&& shardId : shardIds) {
            requests.emplace_back(std::move(shardId), cmdObj);
        }
    } else if (routingInfo->cm()) {
        // The collection is sharded. Use the routing table to decide which shards to target
        // based on the query and collation, and build versioned requests for them.
        for (auto& shardId : shardIds) {
            auto versionedCmdObj =
                appendShardVersion(cmdObj, routingInfo->cm()->getVersion(shardId));
            requests.emplace_back(std::move(shardId), std::move(versionedCmdObj));
        }
    } else {
        // The collection is unsharded. Target only the primary shard for the database.
        // Don't append shard version info when contacting the config servers.
        requests.emplace_back(routingInfo->db().primaryId(),
                              !routingInfo->db().primary()->isConfig()
                                  ? appendShardVersion(cmdObj, ChunkVersion::UNSHARDED())
                                  : cmdObj);
    }

    if (MONGO_FAIL_POINT(clusterAggregateHangBeforeEstablishingShardCursors)) {
        log() << "clusterAggregateHangBeforeEstablishingShardCursors fail point enabled.  Blocking "
                 "until fail point is disabled.";
        while (MONGO_FAIL_POINT(clusterAggregateHangBeforeEstablishingShardCursors)) {
            sleepsecs(1);
        }
    }

    return establishCursors(opCtx,
                            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                            nss,
                            readPref,
                            requests,
                            false /* do not allow partial results */,
                            MongoSInterface::getDesiredRetryPolicy(request));
}

/**
 * Determines the single shard to which the given query will be targeted, and its associated
 * shardVersion. Throws if the query targets more than one shard.
 */
std::pair<ShardId, ChunkVersion> getSingleTargetedShardForQuery(
    OperationContext* opCtx, const CachedCollectionRoutingInfo& routingInfo, BSONObj query) {
    if (auto chunkMgr = routingInfo.cm()) {
        std::set<ShardId> shardIds;
        chunkMgr->getShardIdsForQuery(opCtx, query, CollationSpec::kSimpleSpec, &shardIds);
        uassert(ErrorCodes::InternalError,
                str::stream() << "Unable to target lookup query to a single shard: "
                              << query.toString(),
                shardIds.size() == 1u);
        return {*shardIds.begin(), chunkMgr->getVersion(*shardIds.begin())};
    }

    return {routingInfo.db().primaryId(), ChunkVersion::UNSHARDED()};
}

/**
 * Returns the routing information for the namespace set on the passed ExpressionContext. Also
 * verifies that the ExpressionContext's UUID, if present, matches that of the routing table entry.
 */
StatusWith<CachedCollectionRoutingInfo> getCollectionRoutingInfo(
    const intrusive_ptr<ExpressionContext>& expCtx) {
    auto catalogCache = Grid::get(expCtx->opCtx)->catalogCache();
    auto swRoutingInfo = catalogCache->getCollectionRoutingInfo(expCtx->opCtx, expCtx->ns);
    // Additionally check that the ExpressionContext's UUID matches the collection routing info.
    if (swRoutingInfo.isOK() && expCtx->uuid && swRoutingInfo.getValue().cm()) {
        if (!swRoutingInfo.getValue().cm()->uuidMatches(*expCtx->uuid)) {
            return {ErrorCodes::NamespaceNotFound,
                    str::stream() << "The UUID of collection " << expCtx->ns.ns()
                                  << " changed; it may have been dropped and re-created."};
        }
    }
    return swRoutingInfo;
}

bool supportsUniqueKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       const BSONObj& index,
                       const std::set<FieldPath>& uniqueKeyPaths) {
    // Retrieve the collation from the index, or default to the simple collation.
    const auto collation = uassertStatusOK(
        CollatorFactoryInterface::get(expCtx->opCtx->getServiceContext())
            ->makeFromBSON(index.hasField(IndexDescriptor::kCollationFieldName)
                               ? index.getObjectField(IndexDescriptor::kCollationFieldName)
                               : CollationSpec::kSimpleSpec));

    // SERVER-5335: The _id index does not report to be unique, but in fact is unique.
    auto isIdIndex = index[IndexDescriptor::kIndexNameFieldName].String() == "_id_";
    return (isIdIndex || index.getBoolField(IndexDescriptor::kUniqueFieldName)) &&
        !index.hasField(IndexDescriptor::kPartialFilterExprFieldName) &&
        MongoProcessCommon::keyPatternNamesExactPaths(
               index.getObjectField(IndexDescriptor::kKeyPatternFieldName), uniqueKeyPaths) &&
        CollatorInterface::collatorsMatch(collation.get(), expCtx->getCollator());
}

}  // namespace

Shard::RetryPolicy MongoSInterface::getDesiredRetryPolicy(const AggregationRequest& req) {
    // The idempotent retry policy will retry even for writeConcern failures, so only set it if the
    // pipeline does not support writeConcern.
    if (req.getWriteConcern()) {
        return Shard::RetryPolicy::kNotIdempotent;
    }
    return Shard::RetryPolicy::kIdempotent;
}

BSONObj MongoSInterface::createPassthroughCommandForShard(OperationContext* opCtx,
                                                          const AggregationRequest& request,
                                                          const boost::optional<ShardId>& shardId,
                                                          Pipeline* pipeline,
                                                          BSONObj collationObj) {
    // Create the command for the shards.
    MutableDocument targetedCmd(request.serializeToCommandObj());
    if (pipeline) {
        targetedCmd[AggregationRequest::kPipelineName] = Value(pipeline->serialize());
    }

    return MongoSInterface::genericTransformForShards(
        std::move(targetedCmd), opCtx, shardId, request, collationObj);
}

BSONObj MongoSInterface::genericTransformForShards(MutableDocument&& cmdForShards,
                                                   OperationContext* opCtx,
                                                   const boost::optional<ShardId>& shardId,
                                                   const AggregationRequest& request,
                                                   BSONObj collationObj) {
    cmdForShards[AggregationRequest::kFromMongosName] = Value(true);
    // If this is a request for an aggregation explain, then we must wrap the aggregate inside an
    // explain command.
    if (auto explainVerbosity = request.getExplain()) {
        cmdForShards.reset(wrapAggAsExplain(cmdForShards.freeze(), *explainVerbosity));
    }

    if (!collationObj.isEmpty()) {
        cmdForShards[AggregationRequest::kCollationName] = Value(collationObj);
    }

    if (opCtx->getTxnNumber()) {
        invariant(cmdForShards.peek()[OperationSessionInfo::kTxnNumberFieldName].missing(),
                  str::stream() << "Command for shards unexpectedly had the "
                                << OperationSessionInfo::kTxnNumberFieldName
                                << " field set: "
                                << cmdForShards.peek().toString());
        cmdForShards[OperationSessionInfo::kTxnNumberFieldName] =
            Value(static_cast<long long>(*opCtx->getTxnNumber()));
    }

    auto aggCmd = cmdForShards.freeze().toBson();

    if (shardId) {
        if (auto txnRouter = TransactionRouter::get(opCtx)) {
            aggCmd = txnRouter->attachTxnFieldsIfNeeded(*shardId, aggCmd);
        }
    }

    // agg creates temp collection and should handle implicit create separately.
    return appendAllowImplicitCreate(aggCmd, true);
}

BSONObj MongoSInterface::createCommandForTargetedShards(
    OperationContext* opCtx,
    const AggregationRequest& request,
    const LiteParsedPipeline& litePipe,
    const cluster_aggregation_planner::SplitPipeline& splitPipeline,
    const BSONObj collationObj,
    const boost::optional<cluster_aggregation_planner::ShardedExchangePolicy> exchangeSpec,
    bool needsMerge) {

    // Create the command for the shards.
    MutableDocument targetedCmd(request.serializeToCommandObj());
    // If we've parsed a pipeline on mongos, always override the pipeline, in case parsing it
    // has defaulted any arguments or otherwise changed the spec. For example, $listSessions may
    // have detected a logged in user and appended that user name to the $listSessions spec to
    // send to the shards.
    targetedCmd[AggregationRequest::kPipelineName] =
        Value(splitPipeline.shardsPipeline->serialize());

    // When running on many shards with the exchange we may not need merging.
    if (needsMerge) {
        targetedCmd[AggregationRequest::kNeedsMergeName] = Value(true);

        // If this is a change stream aggregation, set the 'mergeByPBRT' flag on the command. This
        // notifies the shards that the mongoS is capable of merging streams based on resume token.
        // TODO SERVER-38539: the 'mergeByPBRT' flag is no longer necessary in 4.4.
        targetedCmd[AggregationRequest::kMergeByPBRTName] = Value(litePipe.hasChangeStream());

        // For split pipelines which need merging, do *not* propagate the writeConcern to the shards
        // part. Otherwise this is part of an exchange and in that case we should include the
        // writeConcern.
        targetedCmd[WriteConcernOptions::kWriteConcernField] = Value();
    }

    targetedCmd[AggregationRequest::kCursorName] =
        Value(DOC(AggregationRequest::kBatchSizeName << 0));

    targetedCmd[AggregationRequest::kExchangeName] =
        exchangeSpec ? Value(exchangeSpec->exchangeSpec.toBSON()) : Value();

    return genericTransformForShards(
        std::move(targetedCmd), opCtx, boost::none, request, collationObj);
}

std::set<ShardId> MongoSInterface::getTargetedShards(
    OperationContext* opCtx,
    bool mustRunOnAllShards,
    const boost::optional<CachedCollectionRoutingInfo>& routingInfo,
    const BSONObj shardQuery,
    const BSONObj collation) {
    if (mustRunOnAllShards) {
        // The pipeline begins with a stage which must be run on all shards.
        std::vector<ShardId> shardIds;
        Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx, &shardIds);
        return {shardIds.begin(), shardIds.end()};
    }

    // If we don't need to run on all shards, then we should always have a valid routing table.
    invariant(routingInfo);

    return getTargetedShardsForQuery(opCtx, *routingInfo, shardQuery, collation);
}

bool MongoSInterface::mustRunOnAllShards(const NamespaceString& nss,
                                         const LiteParsedPipeline& litePipe) {
    // The following aggregations must be routed to all shards:
    // - Any collectionless aggregation, such as non-localOps $currentOp.
    // - Any aggregation which begins with a $changeStream stage.
    return nss.isCollectionlessAggregateNS() || litePipe.hasChangeStream();
}

StatusWith<CachedCollectionRoutingInfo> MongoSInterface::getExecutionNsRoutingInfo(
    OperationContext* opCtx, const NamespaceString& execNss) {
    // First, verify that there are shards present in the cluster. If not, then we return the
    // stronger 'ShardNotFound' error rather than 'NamespaceNotFound'. We must do this because
    // $changeStream aggregations ignore NamespaceNotFound in order to allow streams to be opened on
    // a collection before its enclosing database is created. However, if there are no shards
    // present, then $changeStream should immediately return an empty cursor just as other
    // aggregations do when the database does not exist.
    std::vector<ShardId> shardIds;
    Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx, &shardIds);
    if (shardIds.size() == 0) {
        return {ErrorCodes::ShardNotFound, "No shards are present in the cluster"};
    }

    // This call to getCollectionRoutingInfoForTxnCmd will return !OK if the database does not
    // exist.
    return getCollectionRoutingInfoForTxnCmd(opCtx, execNss);
}

/**
 * Targets shards for the pipeline and returns a struct with the remote cursors or results, and
 * the pipeline that will need to be executed to merge the results from the remotes. If a stale
 * shard version is encountered, refreshes the routing table and tries again.
 */
MongoSInterface::DispatchShardPipelineResults MongoSInterface::dispatchShardPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& executionNss,
    const AggregationRequest& aggRequest,
    const LiteParsedPipeline& litePipe,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
    BSONObj collationObj) {
    // The process is as follows:
    // - First, determine whether we need to target more than one shard. If so, we split the
    // pipeline; if not, we retain the existing pipeline.
    // - Call establishShardCursors to dispatch the aggregation to the targeted shards.
    // - Stale shard version errors are thrown up to the top-level handler, causing a retry on the
    // entire aggregation commmand.
    auto cursors = std::vector<RemoteCursor>();
    auto shardResults = std::vector<AsyncRequestsSender::Response>();
    auto opCtx = expCtx->opCtx;

    const bool needsPrimaryShardMerge =
        (pipeline->needsPrimaryShardMerger() || internalQueryAlwaysMergeOnPrimaryShard.load());

    const bool needsMongosMerge = pipeline->needsMongosMerger();

    const auto shardQuery = pipeline->getInitialQuery();

    auto executionNsRoutingInfoStatus = getExecutionNsRoutingInfo(opCtx, executionNss);

    // If this is a $changeStream, we swallow NamespaceNotFound exceptions and continue.
    // Otherwise, uassert on all exceptions here.
    if (!(litePipe.hasChangeStream() &&
          executionNsRoutingInfoStatus == ErrorCodes::NamespaceNotFound)) {
        uassertStatusOK(executionNsRoutingInfoStatus);
    }

    auto executionNsRoutingInfo = executionNsRoutingInfoStatus.isOK()
        ? std::move(executionNsRoutingInfoStatus.getValue())
        : boost::optional<CachedCollectionRoutingInfo>{};

    // Determine whether we can run the entire aggregation on a single shard.
    const bool mustRunOnAll = mustRunOnAllShards(executionNss, litePipe);
    std::set<ShardId> shardIds = getTargetedShards(
        opCtx, mustRunOnAll, executionNsRoutingInfo, shardQuery, aggRequest.getCollation());

    // Don't need to split the pipeline if we are only targeting a single shard, unless:
    // - There is a stage that needs to be run on the primary shard and the single target shard
    //   is not the primary.
    // - The pipeline contains one or more stages which must always merge on mongoS.
    const bool needsSplit = (shardIds.size() > 1u || needsMongosMerge ||
                             (needsPrimaryShardMerge && executionNsRoutingInfo &&
                              *(shardIds.begin()) != executionNsRoutingInfo->db().primaryId()));

    boost::optional<cluster_aggregation_planner::ShardedExchangePolicy> exchangeSpec;
    boost::optional<cluster_aggregation_planner::SplitPipeline> splitPipeline;

    if (needsSplit) {
        splitPipeline = cluster_aggregation_planner::splitPipeline(std::move(pipeline));

        exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            opCtx, splitPipeline->mergePipeline.get());
    }

    // Generate the command object for the targeted shards.
    BSONObj targetedCommand = splitPipeline
        ? createCommandForTargetedShards(
              opCtx, aggRequest, litePipe, *splitPipeline, collationObj, exchangeSpec, true)
        : createPassthroughCommandForShard(
              opCtx, aggRequest, boost::none, pipeline.get(), collationObj);

    // Refresh the shard registry if we're targeting all shards.  We need the shard registry
    // to be at least as current as the logical time used when creating the command for
    // $changeStream to work reliably, so we do a "hard" reload.
    if (mustRunOnAll) {
        auto* shardRegistry = Grid::get(opCtx)->shardRegistry();
        if (!shardRegistry->reload(opCtx)) {
            shardRegistry->reload(opCtx);
        }
        // Rebuild the set of shards as the shard registry might have changed.
        shardIds = getTargetedShards(
            opCtx, mustRunOnAll, executionNsRoutingInfo, shardQuery, aggRequest.getCollation());
    }

    // Explain does not produce a cursor, so instead we scatter-gather commands to the shards.
    if (expCtx->explain) {
        if (mustRunOnAll) {
            // Some stages (such as $currentOp) need to be broadcast to all shards, and
            // should not participate in the shard version protocol.
            shardResults =
                scatterGatherUnversionedTargetAllShards(opCtx,
                                                        executionNss.db(),
                                                        targetedCommand,
                                                        ReadPreferenceSetting::get(opCtx),
                                                        Shard::RetryPolicy::kIdempotent);
        } else {
            // Aggregations on a real namespace should use the routing table to target
            // shards, and should participate in the shard version protocol.
            invariant(executionNsRoutingInfo);
            shardResults =
                scatterGatherVersionedTargetByRoutingTable(opCtx,
                                                           executionNss.db(),
                                                           executionNss,
                                                           *executionNsRoutingInfo,
                                                           targetedCommand,
                                                           ReadPreferenceSetting::get(opCtx),
                                                           Shard::RetryPolicy::kIdempotent,
                                                           shardQuery,
                                                           aggRequest.getCollation());
        }
    } else {
        cursors = establishShardCursors(opCtx,
                                        executionNss,
                                        litePipe,
                                        executionNsRoutingInfo,
                                        targetedCommand,
                                        aggRequest,
                                        ReadPreferenceSetting::get(opCtx),
                                        shardQuery);
        invariant(cursors.size() % shardIds.size() == 0,
                  str::stream() << "Number of cursors (" << cursors.size()
                                << ") is not a multiple of producers ("
                                << shardIds.size()
                                << ")");
    }

    // Convert remote cursors into a vector of "owned" cursors.
    std::vector<OwnedRemoteCursor> ownedCursors;
    for (auto&& cursor : cursors) {
        ownedCursors.emplace_back(OwnedRemoteCursor(opCtx, std::move(cursor), executionNss));
    }

    // Record the number of shards involved in the aggregation. If we are required to merge on
    // the primary shard, but the primary shard was not in the set of targeted shards, then we
    // must increment the number of involved shards.
    CurOp::get(opCtx)->debug().nShards =
        shardIds.size() + (needsPrimaryShardMerge && executionNsRoutingInfo &&
                           !shardIds.count(executionNsRoutingInfo->db().primaryId()));

    return DispatchShardPipelineResults{needsPrimaryShardMerge,
                                        std::move(ownedCursors),
                                        std::move(shardResults),
                                        std::move(splitPipeline),
                                        std::move(pipeline),
                                        targetedCommand,
                                        shardIds.size(),
                                        exchangeSpec};
}

std::unique_ptr<Pipeline, PipelineDeleter> MongoSInterface::makePipeline(
    const std::vector<BSONObj>& rawPipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const MakePipelineOptions pipelineOptions) {
    // Explain is not supported for auxiliary lookups.
    invariant(!expCtx->explain);

    auto pipeline = uassertStatusOK(Pipeline::parse(rawPipeline, expCtx));
    if (pipelineOptions.optimize) {
        pipeline->optimizePipeline();
    }
    if (pipelineOptions.attachCursorSource) {
        // 'attachCursorSourceToPipeline' handles any complexity related to sharding.
        pipeline = attachCursorSourceToPipeline(expCtx, pipeline.release());
    }

    return pipeline;
}


std::unique_ptr<Pipeline, PipelineDeleter> MongoSInterface::attachCursorSourceToPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, Pipeline* ownedPipeline) {
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline(ownedPipeline,
                                                        PipelineDeleter(expCtx->opCtx));

    invariant(pipeline->getSources().empty() ||
              !dynamic_cast<DocumentSourceMergeCursors*>(pipeline->getSources().front().get()));

    // Generate the command object for the targeted shards.
    std::vector<BSONObj> rawStages = [&pipeline]() {
        auto serialization = pipeline->serialize();
        std::vector<BSONObj> stages;
        stages.reserve(serialization.size());

        for (const auto& stageObj : serialization) {
            invariant(stageObj.getType() == BSONType::Object);
            stages.push_back(stageObj.getDocument().toBson());
        }

        return stages;
    }();

    AggregationRequest aggRequest(expCtx->ns, rawStages);
    LiteParsedPipeline liteParsedPipeline(aggRequest);
    auto shardDispatchResults = MongoSInterface::dispatchShardPipeline(
        expCtx, expCtx->ns, aggRequest, liteParsedPipeline, std::move(pipeline), expCtx->collation);

    std::vector<ShardId> targetedShards;
    targetedShards.reserve(shardDispatchResults.remoteCursors.size());
    for (auto&& remoteCursor : shardDispatchResults.remoteCursors) {
        targetedShards.emplace_back(remoteCursor->getShardId().toString());
    }

    std::unique_ptr<Pipeline, PipelineDeleter> mergePipeline;
    boost::optional<BSONObj> shardCursorsSortSpec = boost::none;
    if (shardDispatchResults.splitPipeline) {
        mergePipeline = std::move(shardDispatchResults.splitPipeline->mergePipeline);
        shardCursorsSortSpec = shardDispatchResults.splitPipeline->shardCursorsSortSpec;
    } else {
        mergePipeline = std::move(shardDispatchResults.pipelineForSingleShard);
    }

    cluster_aggregation_planner::addMergeCursorsSource(
        mergePipeline.get(),
        liteParsedPipeline,
        shardDispatchResults.commandForTargetedShards,
        std::move(shardDispatchResults.remoteCursors),
        targetedShards,
        shardCursorsSortSpec,
        Grid::get(expCtx->opCtx)->getExecutorPool()->getArbitraryExecutor());

    return mergePipeline;
}

boost::optional<Document> MongoSInterface::lookupSingleDocument(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    UUID collectionUUID,
    const Document& filter,
    boost::optional<BSONObj> readConcern,
    bool allowSpeculativeMajorityRead) {
    auto foreignExpCtx = expCtx->copyWith(nss, collectionUUID);

    // Create the find command to be dispatched to the shard in order to return the post-change
    // document.
    auto filterObj = filter.toBson();
    BSONObjBuilder cmdBuilder;
    bool findCmdIsByUuid(foreignExpCtx->uuid);
    if (findCmdIsByUuid) {
        foreignExpCtx->uuid->appendToBuilder(&cmdBuilder, "find");
    } else {
        cmdBuilder.append("find", nss.coll());
    }
    cmdBuilder.append("filter", filterObj);
    cmdBuilder.append("comment", expCtx->comment);
    if (readConcern) {
        cmdBuilder.append(repl::ReadConcernArgs::kReadConcernFieldName, *readConcern);
    }
    if (allowSpeculativeMajorityRead) {
        cmdBuilder.append("allowSpeculativeMajorityRead", true);
    }

    auto shardResult = std::vector<RemoteCursor>();
    auto findCmd = cmdBuilder.obj();
    size_t numAttempts = 0;
    while (++numAttempts <= kMaxNumStaleVersionRetries) {
        // Verify that the collection exists, with the correct UUID.
        auto catalogCache = Grid::get(expCtx->opCtx)->catalogCache();
        auto swRoutingInfo = getCollectionRoutingInfo(foreignExpCtx);
        if (swRoutingInfo == ErrorCodes::NamespaceNotFound) {
            return boost::none;
        }
        auto routingInfo = uassertStatusOK(std::move(swRoutingInfo));
        if (findCmdIsByUuid && routingInfo.cm()) {
            // Find by UUID and shard versioning do not work together (SERVER-31946).  In the
            // sharded case we've already checked the UUID, so find by namespace is safe.  In the
            // unlikely case that the collection has been deleted and a new collection with the same
            // name created through a different mongos, the shard version will be detected as stale,
            // as shard versions contain an 'epoch' field unique to the collection.
            findCmd = findCmd.addField(BSON("find" << nss.coll()).firstElement());
            findCmdIsByUuid = false;
        }

        // Get the ID and version of the single shard to which this query will be sent.
        auto shardInfo = getSingleTargetedShardForQuery(expCtx->opCtx, routingInfo, filterObj);

        // Dispatch the request. This will only be sent to a single shard and only a single result
        // will be returned. The 'establishCursors' method conveniently prepares the result into a
        // cursor response for us.
        try {
            shardResult = establishCursors(
                expCtx->opCtx,
                Grid::get(expCtx->opCtx)->getExecutorPool()->getArbitraryExecutor(),
                nss,
                ReadPreferenceSetting::get(expCtx->opCtx),
                {{shardInfo.first, appendShardVersion(findCmd, shardInfo.second)}},
                false);
            break;
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            // If it's an unsharded collection which has been deleted and re-created, we may get a
            // NamespaceNotFound error when looking up by UUID.
            return boost::none;
        } catch (const ExceptionForCat<ErrorCategory::StaleShardVersionError>&) {
            // If we hit a stale shardVersion exception, invalidate the routing table cache.
            catalogCache->onStaleShardVersion(std::move(routingInfo));
            continue;  // Try again if allowed.
        }
        break;  // Success!
    }

    invariant(shardResult.size() == 1u);

    auto& cursor = shardResult.front().getCursorResponse();
    auto& batch = cursor.getBatch();

    // We should have at most 1 result, and the cursor should be exhausted.
    uassert(ErrorCodes::InternalError,
            str::stream() << "Shard cursor was unexpectedly open after lookup: "
                          << shardResult.front().getHostAndPort()
                          << ", id: "
                          << cursor.getCursorId(),
            cursor.getCursorId() == 0);
    uassert(ErrorCodes::TooManyMatchingDocuments,
            str::stream() << "found more than one document matching " << filter.toString() << " ["
                          << batch.begin()->toString()
                          << ", "
                          << std::next(batch.begin())->toString()
                          << "]",
            batch.size() <= 1u);

    return (!batch.empty() ? Document(batch.front()) : boost::optional<Document>{});
}

BSONObj MongoSInterface::_reportCurrentOpForClient(OperationContext* opCtx,
                                                   Client* client,
                                                   CurrentOpTruncateMode truncateOps) const {
    BSONObjBuilder builder;

    CurOp::reportCurrentOpForClient(
        opCtx, client, (truncateOps == CurrentOpTruncateMode::kTruncateOps), &builder);

    return builder.obj();
}

std::vector<GenericCursor> MongoSInterface::getIdleCursors(
    const intrusive_ptr<ExpressionContext>& expCtx, CurrentOpUserMode userMode) const {
    invariant(hasGlobalServiceContext());
    auto cursorManager = Grid::get(expCtx->opCtx->getServiceContext())->getCursorManager();
    invariant(cursorManager);
    return cursorManager->getIdleCursors(expCtx->opCtx, userMode);
}

bool MongoSInterface::isSharded(OperationContext* opCtx, const NamespaceString& nss) {
    auto routingInfo = Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss);
    return routingInfo.isOK() && routingInfo.getValue().cm();
}

bool MongoSInterface::uniqueKeyIsSupportedByIndex(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    const std::set<FieldPath>& uniqueKeyPaths) const {
    const auto opCtx = expCtx->opCtx;
    const auto routingInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

    // Run an exhaustive listIndexes against the primary shard only.
    auto response = routingInfo.db().primary()->runExhaustiveCursorCommand(
        opCtx,
        ReadPreferenceSetting::get(opCtx),
        nss.db().toString(),
        BSON("listIndexes" << nss.coll()),
        opCtx->hasDeadline() ? opCtx->getRemainingMaxTimeMillis() : Milliseconds(-1));

    // If the namespace does not exist, then the unique key *must* be _id only.
    if (response.getStatus() == ErrorCodes::NamespaceNotFound) {
        return uniqueKeyPaths == std::set<FieldPath>{"_id"};
    }
    uassertStatusOK(response);

    const auto& indexes = response.getValue().docs;
    return std::any_of(
        indexes.begin(), indexes.end(), [&expCtx, &uniqueKeyPaths](const auto& index) {
            return supportsUniqueKey(expCtx, index, uniqueKeyPaths);
        });
}

}  // namespace mongo
