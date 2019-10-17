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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "sharded_agg_helpers.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connpool.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/curop.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/query/find_common.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/query/cluster_aggregation_planner.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_query_knobs_gen.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"

namespace mongo::sharded_agg_helpers {

using SplitPipeline = cluster_aggregation_planner::SplitPipeline;

MONGO_FAIL_POINT_DEFINE(shardedAggregateHangBeforeEstablishingShardCursors);
MONGO_FAIL_POINT_DEFINE(shardedAggregateFailToEstablishMergingShardCursor);
MONGO_FAIL_POINT_DEFINE(shardedAggregateFailToDispatchExchangeConsumerPipeline);

namespace {

bool mustRunOnAllShards(const NamespaceString& nss, bool hasChangeStream) {
    // The following aggregations must be routed to all shards:
    // - Any collectionless aggregation, such as non-localOps $currentOp.
    // - Any aggregation which begins with a $changeStream stage.
    return nss.isCollectionlessAggregateNS() || hasChangeStream;
}

Status appendCursorResponseToCommandResult(const ShardId& shardId,
                                           const BSONObj cursorResponse,
                                           BSONObjBuilder* result) {
    // If a write error was encountered, append it to the output buffer first.
    if (auto wcErrorElem = cursorResponse["writeConcernError"]) {
        appendWriteConcernErrorToCmdResponse(shardId, wcErrorElem, *result);
    }

    // Pass the results from the remote shard into our command response.
    result->appendElementsUnique(CommandHelpers::filterCommandReplyForPassthrough(cursorResponse));
    return getStatusFromCommandResult(result->asTempObj());
}

/**
 * Given a document representing an aggregation command such as
 * {aggregate: "myCollection", pipeline: [], ...},
 *
 * produces the corresponding explain command:
 * {explain: {aggregate: "myCollection", pipline: [], ...}, $queryOptions: {...}, verbosity: ...}
 */
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

/**
 * Open a $changeStream cursor on the 'config.shards' collection to watch for new shards.
 */
RemoteCursor openChangeStreamNewShardMonitor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                             Timestamp startMonitoringAtTime) {
    const auto& configShard = Grid::get(expCtx->opCtx)->shardRegistry()->getConfigShard();
    // Pipeline: {$changeStream: {startAtOperationTime: [now], allowToRunOnConfigDB: true}}
    AggregationRequest aggReq(
        ShardType::ConfigNS,
        {BSON(DocumentSourceChangeStream::kStageName
              << BSON(DocumentSourceChangeStreamSpec::kStartAtOperationTimeFieldName
                      << startMonitoringAtTime
                      << DocumentSourceChangeStreamSpec::kAllowToRunOnConfigDBFieldName << true))});
    aggReq.setFromMongos(true);
    aggReq.setNeedsMerge(true);
    aggReq.setBatchSize(0);
    auto configCursor =
        establishCursors(expCtx->opCtx,
                         Grid::get(expCtx->opCtx)->getExecutorPool()->getArbitraryExecutor(),
                         aggReq.getNamespaceString(),
                         ReadPreferenceSetting{ReadPreference::PrimaryPreferred},
                         {{configShard->getId(), aggReq.serializeToCommandObj().toBson()}},
                         false);
    invariant(configCursor.size() == 1);
    return std::move(*configCursor.begin());
}

Shard::RetryPolicy getDesiredRetryPolicy(OperationContext* opCtx) {
    // The idempotent retry policy will retry even for writeConcern failures, so only set it if the
    // pipeline does not support writeConcern.
    if (!opCtx->getWriteConcern().usedDefault) {
        return Shard::RetryPolicy::kNotIdempotent;
    }
    return Shard::RetryPolicy::kIdempotent;
}

BSONObj genericTransformForShards(MutableDocument&& cmdForShards,
                                  OperationContext* opCtx,
                                  boost::optional<ExplainOptions::Verbosity> explainVerbosity,
                                  const boost::optional<RuntimeConstants>& constants,
                                  BSONObj collationObj) {
    if (constants) {
        cmdForShards[AggregationRequest::kRuntimeConstants] = Value(constants.get().toBSON());
    }

    cmdForShards[AggregationRequest::kFromMongosName] = Value(true);
    // If this is a request for an aggregation explain, then we must wrap the aggregate inside an
    // explain command.
    if (explainVerbosity) {
        cmdForShards.reset(wrapAggAsExplain(cmdForShards.freeze(), *explainVerbosity));
    }

    if (!collationObj.isEmpty()) {
        cmdForShards[AggregationRequest::kCollationName] = Value(collationObj);
    }

    if (opCtx->getTxnNumber()) {
        invariant(cmdForShards.peek()[OperationSessionInfo::kTxnNumberFieldName].missing(),
                  str::stream() << "Command for shards unexpectedly had the "
                                << OperationSessionInfo::kTxnNumberFieldName
                                << " field set: " << cmdForShards.peek().toString());
        cmdForShards[OperationSessionInfo::kTxnNumberFieldName] =
            Value(static_cast<long long>(*opCtx->getTxnNumber()));
    }

    return appendAllowImplicitCreate(cmdForShards.freeze().toBson(), false);
}

std::vector<RemoteCursor> establishShardCursors(
    OperationContext* opCtx,
    const NamespaceString& nss,
    bool hasChangeStream,
    boost::optional<CachedCollectionRoutingInfo>& routingInfo,
    const std::set<ShardId>& shardIds,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref) {
    LOG(1) << "Dispatching command " << redact(cmdObj) << " to establish cursors on shards";

    const bool mustRunOnAll = mustRunOnAllShards(nss, hasChangeStream);
    std::vector<std::pair<ShardId, BSONObj>> requests;

    // If we don't need to run on all shards, then we should always have a valid routing table.
    invariant(routingInfo || mustRunOnAll);

    if (mustRunOnAll) {
        // The pipeline contains a stage which must be run on all shards. Skip versioning and
        // enqueue the raw command objects.
        for (const auto& shardId : shardIds) {
            requests.emplace_back(shardId, cmdObj);
        }
    } else if (routingInfo->cm()) {
        // The collection is sharded. Use the routing table to decide which shards to target
        // based on the query and collation, and build versioned requests for them.
        for (const auto& shardId : shardIds) {
            auto versionedCmdObj =
                appendShardVersion(cmdObj, routingInfo->cm()->getVersion(shardId));
            requests.emplace_back(shardId, std::move(versionedCmdObj));
        }
    } else {
        // The collection is unsharded. Target only the primary shard for the database.
        // Don't append shard version info when contacting the config servers.
        const auto cmdObjWithShardVersion = !routingInfo->db().primary()->isConfig()
            ? appendShardVersion(cmdObj, ChunkVersion::UNSHARDED())
            : cmdObj;
        requests.emplace_back(routingInfo->db().primaryId(),
                              appendDbVersionIfPresent(cmdObjWithShardVersion, routingInfo->db()));
    }

    if (MONGO_unlikely(shardedAggregateHangBeforeEstablishingShardCursors.shouldFail())) {
        log() << "shardedAggregateHangBeforeEstablishingShardCursors fail point enabled.  Blocking "
                 "until fail point is disabled.";
        while (MONGO_unlikely(shardedAggregateHangBeforeEstablishingShardCursors.shouldFail())) {
            sleepsecs(1);
        }
    }

    return establishCursors(opCtx,
                            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                            nss,
                            readPref,
                            requests,
                            false /* do not allow partial results */,
                            getDesiredRetryPolicy(opCtx));
}

std::set<ShardId> getTargetedShards(OperationContext* opCtx,
                                    bool mustRunOnAllShards,
                                    const boost::optional<CachedCollectionRoutingInfo>& routingInfo,
                                    const BSONObj shardQuery,
                                    const BSONObj collation) {
    if (mustRunOnAllShards) {
        // The pipeline begins with a stage which must be run on all shards.
        std::vector<ShardId> shardIds;
        Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx, &shardIds);
        return {std::make_move_iterator(shardIds.begin()), std::make_move_iterator(shardIds.end())};
    }

    // If we don't need to run on all shards, then we should always have a valid routing table.
    invariant(routingInfo);

    return getTargetedShardsForQuery(opCtx, *routingInfo, shardQuery, collation);
}

ShardId pickMergingShard(OperationContext* opCtx,
                         bool needsPrimaryShardMerge,
                         const std::vector<ShardId>& targetedShards,
                         ShardId primaryShard) {
    auto& prng = opCtx->getClient()->getPrng();
    // If we cannot merge on mongoS, establish the merge cursor on a shard. Perform the merging
    // command on random shard, unless the pipeline dictates that it needs to be run on the primary
    // shard for the database.
    return needsPrimaryShardMerge ? primaryShard
                                  : targetedShards[prng.nextInt32(targetedShards.size())];
}

StatusWith<CachedCollectionRoutingInfo> getExecutionNsRoutingInfo(OperationContext* opCtx,
                                                                  const NamespaceString& execNss) {
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

Status appendExplainResults(sharded_agg_helpers::DispatchShardPipelineResults&& dispatchResults,
                            const boost::intrusive_ptr<ExpressionContext>& mergeCtx,
                            BSONObjBuilder* result) {
    if (dispatchResults.splitPipeline) {
        auto* mergePipeline = dispatchResults.splitPipeline->mergePipeline.get();
        const char* mergeType = [&]() {
            if (mergePipeline->canRunOnMongos()) {
                return "mongos";
            } else if (dispatchResults.exchangeSpec) {
                return "exchange";
            } else if (mergePipeline->needsPrimaryShardMerger()) {
                return "primaryShard";
            } else {
                return "anyShard";
            }
        }();

        *result << "mergeType" << mergeType;

        MutableDocument pipelinesDoc;
        pipelinesDoc.addField("shardsPart",
                              Value(dispatchResults.splitPipeline->shardsPipeline->writeExplainOps(
                                  *mergeCtx->explain)));
        if (dispatchResults.exchangeSpec) {
            BSONObjBuilder bob;
            dispatchResults.exchangeSpec->exchangeSpec.serialize(&bob);
            bob.append("consumerShards", dispatchResults.exchangeSpec->consumerShards);
            pipelinesDoc.addField("exchange", Value(bob.obj()));
        }
        pipelinesDoc.addField("mergerPart",
                              Value(mergePipeline->writeExplainOps(*mergeCtx->explain)));

        *result << "splitPipeline" << pipelinesDoc.freeze();
    } else {
        *result << "splitPipeline" << BSONNULL;
    }

    BSONObjBuilder shardExplains(result->subobjStart("shards"));
    for (const auto& shardResult : dispatchResults.remoteExplainOutput) {
        invariant(shardResult.shardHostAndPort);

        uassertStatusOK(shardResult.swResponse.getStatus());
        uassertStatusOK(getStatusFromCommandResult(shardResult.swResponse.getValue().data));

        auto shardId = shardResult.shardId.toString();
        const auto& data = shardResult.swResponse.getValue().data;
        BSONObjBuilder explain(shardExplains.subobjStart(shardId));
        explain << "host" << shardResult.shardHostAndPort->toString();
        if (auto stagesElement = data["stages"]) {
            explain << "stages" << stagesElement;
        } else {
            auto queryPlannerElement = data["queryPlanner"];
            uassert(51157,
                    str::stream() << "Malformed explain response received from shard " << shardId
                                  << ": " << data.toString(),
                    queryPlannerElement);
            explain << "queryPlanner" << queryPlannerElement;
            if (auto executionStatsElement = data["executionStats"]) {
                explain << "executionStats" << executionStatsElement;
            }
        }
    }

    return Status::OK();
}

BSONObj createCommandForTargetedShards(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Document serializedCommand,
    const cluster_aggregation_planner::SplitPipeline& splitPipeline,
    const boost::optional<cluster_aggregation_planner::ShardedExchangePolicy> exchangeSpec,
    bool needsMerge) {
    // Create the command for the shards.
    MutableDocument targetedCmd(serializedCommand);
    // If we've parsed a pipeline on mongos, always override the pipeline, in case parsing it
    // has defaulted any arguments or otherwise changed the spec. For example, $listSessions may
    // have detected a logged in user and appended that user name to the $listSessions spec to
    // send to the shards.
    targetedCmd[AggregationRequest::kPipelineName] =
        Value(splitPipeline.shardsPipeline->serialize());

    // When running on many shards with the exchange we may not need merging.
    if (needsMerge) {
        targetedCmd[AggregationRequest::kNeedsMergeName] = Value(true);

        // If there aren't any stages like $out in the pipeline being sent to the shards, remove the
        // write concern. The write concern should only be applied when there are writes performed
        // to avoid mistakenly waiting for writes which didn't happen.
        const auto& shardsPipe = splitPipeline.shardsPipeline->getSources();
        if (!std::any_of(shardsPipe.begin(), shardsPipe.end(), [](const auto& stage) {
                return stage->constraints().writesPersistentData();
            })) {
            targetedCmd[WriteConcernOptions::kWriteConcernField] = Value();
        }
    }

    targetedCmd[AggregationRequest::kCursorName] =
        Value(DOC(AggregationRequest::kBatchSizeName << 0));

    targetedCmd[AggregationRequest::kExchangeName] =
        exchangeSpec ? Value(exchangeSpec->exchangeSpec.toBSON()) : Value();

    return genericTransformForShards(std::move(targetedCmd),
                                     expCtx->opCtx,
                                     expCtx->explain,
                                     expCtx->getRuntimeConstants(),
                                     expCtx->collation);
}

sharded_agg_helpers::DispatchShardPipelineResults dispatchExchangeConsumerPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& executionNss,
    Document serializedCommand,
    sharded_agg_helpers::DispatchShardPipelineResults* shardDispatchResults) {
    auto opCtx = expCtx->opCtx;

    if (MONGO_unlikely(shardedAggregateFailToDispatchExchangeConsumerPipeline.shouldFail())) {
        log() << "shardedAggregateFailToDispatchExchangeConsumerPipeline fail point enabled.";
        uasserted(ErrorCodes::FailPointEnabled,
                  "Asserting on exhange consumer pipeline dispatch due to failpoint.");
    }

    // For all consumers construct a request with appropriate cursor ids and send to shards.
    std::vector<std::pair<ShardId, BSONObj>> requests;
    auto numConsumers = shardDispatchResults->exchangeSpec->consumerShards.size();
    std::vector<SplitPipeline> consumerPipelines;
    for (size_t idx = 0; idx < numConsumers; ++idx) {
        // Pick this consumer's cursors from producers.
        std::vector<OwnedRemoteCursor> producers;
        for (size_t p = 0; p < shardDispatchResults->numProducers; ++p) {
            producers.emplace_back(
                std::move(shardDispatchResults->remoteCursors[p * numConsumers + idx]));
        }

        // Create a pipeline for a consumer and add the merging stage.
        auto consumerPipeline = uassertStatusOK(Pipeline::create(
            shardDispatchResults->splitPipeline->mergePipeline->getSources(), expCtx));

        cluster_aggregation_planner::addMergeCursorsSource(
            consumerPipeline.get(),
            BSONObj(),
            std::move(producers),
            {},
            shardDispatchResults->splitPipeline->shardCursorsSortSpec,
            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
            false);

        consumerPipelines.emplace_back(std::move(consumerPipeline), nullptr, boost::none);

        auto consumerCmdObj = createCommandForTargetedShards(
            expCtx, serializedCommand, consumerPipelines.back(), boost::none, false);

        requests.emplace_back(shardDispatchResults->exchangeSpec->consumerShards[idx],
                              consumerCmdObj);
    }
    auto cursors = establishCursors(opCtx,
                                    Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                                    executionNss,
                                    ReadPreferenceSetting::get(opCtx),
                                    requests,
                                    false /* do not allow partial results */);

    // Convert remote cursors into a vector of "owned" cursors.
    std::vector<OwnedRemoteCursor> ownedCursors;
    for (auto&& cursor : cursors) {
        ownedCursors.emplace_back(OwnedRemoteCursor(opCtx, std::move(cursor), executionNss));
    }

    // The merging pipeline is just a union of the results from each of the shards involved on the
    // consumer side of the exchange.
    auto mergePipeline = uassertStatusOK(Pipeline::create({}, expCtx));
    mergePipeline->setSplitState(Pipeline::SplitState::kSplitForMerge);

    SplitPipeline splitPipeline{nullptr, std::move(mergePipeline), boost::none};

    // Relinquish ownership of the local consumer pipelines' cursors as each shard is now
    // responsible for its own producer cursors.
    for (const auto& pipeline : consumerPipelines) {
        const auto& mergeCursors =
            static_cast<DocumentSourceMergeCursors*>(pipeline.shardsPipeline->peekFront());
        mergeCursors->dismissCursorOwnership();
    }
    return sharded_agg_helpers::DispatchShardPipelineResults{false,
                                                             std::move(ownedCursors),
                                                             {} /*TODO SERVER-36279*/,
                                                             std::move(splitPipeline),
                                                             nullptr,
                                                             BSONObj(),
                                                             numConsumers};
}

BSONObj createCommandForMergingShard(Document serializedCommand,
                                     const boost::intrusive_ptr<ExpressionContext>& mergeCtx,
                                     const ShardId& shardId,
                                     bool mergingShardContributesData,
                                     const Pipeline* pipelineForMerging) {
    MutableDocument mergeCmd(serializedCommand);

    mergeCmd["pipeline"] = Value(pipelineForMerging->serialize());
    mergeCmd[AggregationRequest::kFromMongosName] = Value(true);

    mergeCmd[AggregationRequest::kRuntimeConstants] =
        Value(mergeCtx->getRuntimeConstants().toBSON());

    // If the user didn't specify a collation already, make sure there's a collation attached to
    // the merge command, since the merging shard may not have the collection metadata.
    if (mergeCmd.peek()["collation"].missing()) {
        mergeCmd["collation"] = mergeCtx->getCollator()
            ? Value(mergeCtx->getCollator()->getSpec().toBSON())
            : Value(Document{CollationSpec::kSimpleSpec});
    }

    const auto txnRouter = TransactionRouter::get(mergeCtx->opCtx);
    if (txnRouter && mergingShardContributesData) {
        // Don't include a readConcern since we can only include read concerns on the _first_
        // command sent to a participant per transaction. Assuming the merging shard is a
        // participant, it will already have received another 'aggregate' command earlier which
        // contained a readConcern.
        mergeCmd.remove("readConcern");
    }

    return appendAllowImplicitCreate(mergeCmd.freeze().toBson(), false);
}

BSONObj createPassthroughCommandForShard(
    OperationContext* opCtx,
    Document serializedCommand,
    boost::optional<ExplainOptions::Verbosity> explainVerbosity,
    const boost::optional<RuntimeConstants>& constants,
    Pipeline* pipeline,
    BSONObj collationObj) {
    // Create the command for the shards.
    MutableDocument targetedCmd(serializedCommand);
    if (pipeline) {
        targetedCmd[AggregationRequest::kPipelineName] = Value(pipeline->serialize());
    }

    return genericTransformForShards(
        std::move(targetedCmd), opCtx, explainVerbosity, constants, collationObj);
}

/**
 * Targets shards for the pipeline and returns a struct with the remote cursors or results, and
 * the pipeline that will need to be executed to merge the results from the remotes. If a stale
 * shard version is encountered, refreshes the routing table and tries again.
 */
DispatchShardPipelineResults dispatchShardPipeline(
    Document serializedCommand,
    bool hasChangeStream,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline) {
    auto expCtx = pipeline->getContext();

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

    auto executionNsRoutingInfoStatus = getExecutionNsRoutingInfo(opCtx, expCtx->ns);

    // If this is a $changeStream, we swallow NamespaceNotFound exceptions and continue.
    // Otherwise, uassert on all exceptions here.
    if (!(hasChangeStream && executionNsRoutingInfoStatus == ErrorCodes::NamespaceNotFound)) {
        uassertStatusOK(executionNsRoutingInfoStatus);
    }

    auto executionNsRoutingInfo = executionNsRoutingInfoStatus.isOK()
        ? std::move(executionNsRoutingInfoStatus.getValue())
        : boost::optional<CachedCollectionRoutingInfo>{};

    // Determine whether we can run the entire aggregation on a single shard.
    const bool mustRunOnAll = mustRunOnAllShards(expCtx->ns, hasChangeStream);
    std::set<ShardId> shardIds = getTargetedShards(
        opCtx, mustRunOnAll, executionNsRoutingInfo, shardQuery, expCtx->collation);

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
        LOG(5) << "Splitting pipeline: "
               << "targeting = " << shardIds.size()
               << " shards, needsMongosMerge = " << needsMongosMerge
               << ", needsPrimaryShardMerge = " << needsPrimaryShardMerge;
        splitPipeline = cluster_aggregation_planner::splitPipeline(std::move(pipeline));

        exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            opCtx, splitPipeline->mergePipeline.get());
    }

    // Generate the command object for the targeted shards.
    BSONObj targetedCommand = splitPipeline
        ? createCommandForTargetedShards(
              expCtx, serializedCommand, *splitPipeline, exchangeSpec, true)
        : createPassthroughCommandForShard(expCtx->opCtx,
                                           serializedCommand,
                                           expCtx->explain,
                                           expCtx->getRuntimeConstants(),
                                           pipeline.get(),
                                           expCtx->collation);

    // A $changeStream pipeline must run on all shards, and will also open an extra cursor on the
    // config server in order to monitor for new shards. To guarantee that we do not miss any
    // shards, we must ensure that the list of shards to which we initially dispatch the pipeline is
    // at least as current as the logical time at which the stream begins scanning for new shards.
    // We therefore set 'shardRegistryReloadTime' to the current clusterTime and then hard-reload
    // the shard registry. We don't refresh for other pipelines that must run on all shards (e.g.
    // $currentOp) because, unlike $changeStream, those pipelines may not have been forced to split
    // if there was only one shard in the cluster when the command began execution. If a shard was
    // added since the earlier targeting logic ran, then refreshing here may cause us to illegally
    // target an unsplit pipeline to more than one shard.
    auto shardRegistryReloadTime = LogicalClock::get(opCtx)->getClusterTime().asTimestamp();
    if (hasChangeStream) {
        auto* shardRegistry = Grid::get(opCtx)->shardRegistry();
        if (!shardRegistry->reload(opCtx)) {
            shardRegistry->reload(opCtx);
        }
        // Rebuild the set of shards as the shard registry might have changed.
        shardIds = getTargetedShards(
            opCtx, mustRunOnAll, executionNsRoutingInfo, shardQuery, expCtx->collation);
    }

    // If there were no shards when we began execution, we wouldn't have run this aggregation in the
    // first place. Here, we double-check that the shards have not been removed mid-operation.
    uassert(ErrorCodes::ShardNotFound,
            "Unexpectedly found 0 shards while preparing to dispatch aggregation requests. Were "
            "the shards removed mid-operation?",
            shardIds.size() > 0);

    // Explain does not produce a cursor, so instead we scatter-gather commands to the shards.
    if (expCtx->explain) {
        if (mustRunOnAll) {
            // Some stages (such as $currentOp) need to be broadcast to all shards, and
            // should not participate in the shard version protocol.
            shardResults =
                scatterGatherUnversionedTargetAllShards(opCtx,
                                                        expCtx->ns.db(),
                                                        targetedCommand,
                                                        ReadPreferenceSetting::get(opCtx),
                                                        Shard::RetryPolicy::kIdempotent);
        } else {
            // Aggregations on a real namespace should use the routing table to target
            // shards, and should participate in the shard version protocol.
            invariant(executionNsRoutingInfo);
            shardResults =
                scatterGatherVersionedTargetByRoutingTable(opCtx,
                                                           expCtx->ns.db(),
                                                           expCtx->ns,
                                                           *executionNsRoutingInfo,
                                                           targetedCommand,
                                                           ReadPreferenceSetting::get(opCtx),
                                                           Shard::RetryPolicy::kIdempotent,
                                                           shardQuery,
                                                           expCtx->collation);
        }
    } else {
        cursors = establishShardCursors(opCtx,
                                        expCtx->ns,
                                        hasChangeStream,
                                        executionNsRoutingInfo,
                                        shardIds,
                                        targetedCommand,
                                        ReadPreferenceSetting::get(opCtx));
        invariant(cursors.size() % shardIds.size() == 0,
                  str::stream() << "Number of cursors (" << cursors.size()
                                << ") is not a multiple of producers (" << shardIds.size() << ")");

        // For $changeStream, we must open an extra cursor on the 'config.shards' collection, so
        // that we can monitor for the addition of new shards inline with real events.
        if (hasChangeStream && expCtx->ns.db() != ShardType::ConfigNS.db()) {
            cursors.emplace_back(openChangeStreamNewShardMonitor(expCtx, shardRegistryReloadTime));
        }
    }

    // Convert remote cursors into a vector of "owned" cursors.
    std::vector<OwnedRemoteCursor> ownedCursors;
    for (auto&& cursor : cursors) {
        auto cursorNss = cursor.getCursorResponse().getNSS();
        ownedCursors.emplace_back(opCtx, std::move(cursor), std::move(cursorNss));
    }

    // Record the number of shards involved in the aggregation. If we are required to merge on
    // the primary shard, but the primary shard was not in the set of targeted shards, then we
    // must increment the number of involved shards.
    CurOp::get(opCtx)->debug().nShards = shardIds.size() +
        (needsPrimaryShardMerge && executionNsRoutingInfo &&
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

AsyncRequestsSender::Response establishMergingShardCursor(OperationContext* opCtx,
                                                          const NamespaceString& nss,
                                                          const BSONObj mergeCmdObj,
                                                          const ShardId& mergingShardId) {
    if (MONGO_unlikely(shardedAggregateFailToEstablishMergingShardCursor.shouldFail())) {
        log() << "shardedAggregateFailToEstablishMergingShardCursor fail point enabled.";
        uasserted(ErrorCodes::FailPointEnabled,
                  "Asserting on establishing merging shard cursor due to failpoint.");
    }

    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        nss.db().toString(),
        {{mergingShardId, mergeCmdObj}},
        ReadPreferenceSetting::get(opCtx),
        getDesiredRetryPolicy(opCtx));
    const auto response = ars.next();
    invariant(ars.done());
    return response;
}

Status dispatchMergingPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ClusterAggregate::Namespaces& namespaces,
    Document serializedCommand,
    long long batchSize,
    const boost::optional<CachedCollectionRoutingInfo>& routingInfo,
    sharded_agg_helpers::DispatchShardPipelineResults&& shardDispatchResults,
    BSONObjBuilder* result,
    const PrivilegeVector& privileges,
    bool hasChangeStream) {
    // We should never be in a situation where we call this function on a non-merge pipeline.
    invariant(shardDispatchResults.splitPipeline);
    auto* mergePipeline = shardDispatchResults.splitPipeline->mergePipeline.get();
    invariant(mergePipeline);
    auto* opCtx = expCtx->opCtx;

    std::vector<ShardId> targetedShards;
    targetedShards.reserve(shardDispatchResults.remoteCursors.size());
    for (auto&& remoteCursor : shardDispatchResults.remoteCursors) {
        targetedShards.emplace_back(remoteCursor->getShardId().toString());
    }

    cluster_aggregation_planner::addMergeCursorsSource(
        mergePipeline,
        shardDispatchResults.commandForTargetedShards,
        std::move(shardDispatchResults.remoteCursors),
        targetedShards,
        shardDispatchResults.splitPipeline->shardCursorsSortSpec,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        hasChangeStream);

    // First, check whether we can merge on the mongoS. If the merge pipeline MUST run on mongoS,
    // then ignore the internalQueryProhibitMergingOnMongoS parameter.
    if (mergePipeline->requiredToRunOnMongos() ||
        (!internalQueryProhibitMergingOnMongoS.load() && mergePipeline->canRunOnMongos())) {
        return runPipelineOnMongoS(namespaces,
                                   batchSize,
                                   std::move(shardDispatchResults.splitPipeline->mergePipeline),
                                   result,
                                   privileges);
    }

    // If we are not merging on mongoS, then this is not a $changeStream aggregation, and we
    // therefore must have a valid routing table.
    invariant(routingInfo);

    const ShardId mergingShardId = pickMergingShard(opCtx,
                                                    shardDispatchResults.needsPrimaryShardMerge,
                                                    targetedShards,
                                                    routingInfo->db().primaryId());
    const bool mergingShardContributesData =
        std::find(targetedShards.begin(), targetedShards.end(), mergingShardId) !=
        targetedShards.end();

    auto mergeCmdObj = createCommandForMergingShard(
        serializedCommand, expCtx, mergingShardId, mergingShardContributesData, mergePipeline);

    LOG(1) << "Dispatching merge pipeline " << redact(mergeCmdObj) << " to designated shard";

    // Dispatch $mergeCursors to the chosen shard, store the resulting cursor, and return.
    auto mergeResponse =
        establishMergingShardCursor(opCtx, namespaces.executionNss, mergeCmdObj, mergingShardId);
    uassertStatusOK(mergeResponse.swResponse);

    auto mergeCursorResponse = uassertStatusOK(
        storePossibleCursor(opCtx,
                            mergingShardId,
                            *mergeResponse.shardHostAndPort,
                            mergeResponse.swResponse.getValue().data,
                            namespaces.requestedNss,
                            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                            Grid::get(opCtx)->getCursorManager(),
                            privileges,
                            expCtx->tailableMode));

    // Ownership for the shard cursors has been transferred to the merging shard. Dismiss the
    // ownership in the current merging pipeline such that when it goes out of scope it does not
    // attempt to kill the cursors.
    auto mergeCursors = static_cast<DocumentSourceMergeCursors*>(mergePipeline->peekFront());
    mergeCursors->dismissCursorOwnership();

    return appendCursorResponseToCommandResult(mergingShardId, mergeCursorResponse, result);
}

BSONObj establishMergingMongosCursor(OperationContext* opCtx,
                                     long long batchSize,
                                     const NamespaceString& requestedNss,
                                     std::unique_ptr<Pipeline, PipelineDeleter> pipelineForMerging,
                                     const PrivilegeVector& privileges) {

    ClusterClientCursorParams params(requestedNss, ReadPreferenceSetting::get(opCtx));

    params.originatingCommandObj = CurOp::get(opCtx)->opDescription().getOwned();
    params.tailableMode = pipelineForMerging->getContext()->tailableMode;
    // A batch size of 0 is legal for the initial aggregate, but not valid for getMores, the batch
    // size we pass here is used for getMores, so do not specify a batch size if the initial request
    // had a batch size of 0.
    params.batchSize = batchSize == 0 ? boost::none : boost::make_optional(batchSize);
    params.lsid = opCtx->getLogicalSessionId();
    params.txnNumber = opCtx->getTxnNumber();
    params.originatingPrivileges = privileges;

    if (TransactionRouter::get(opCtx)) {
        params.isAutoCommit = false;
    }

    auto ccc = cluster_aggregation_planner::buildClusterCursor(
        opCtx, std::move(pipelineForMerging), std::move(params));

    auto cursorState = ClusterCursorManager::CursorState::NotExhausted;

    rpc::OpMsgReplyBuilder replyBuilder;
    CursorResponseBuilder::Options options;
    options.isInitialResponse = true;

    CursorResponseBuilder responseBuilder(&replyBuilder, options);
    bool stashedResult = false;

    for (long long objCount = 0; objCount < batchSize; ++objCount) {
        ClusterQueryResult next;
        try {
            next = uassertStatusOK(ccc->next(RouterExecStage::ExecContext::kInitialFind));
        } catch (const ExceptionFor<ErrorCodes::CloseChangeStream>&) {
            // This exception is thrown when a $changeStream stage encounters an event
            // that invalidates the cursor. We should close the cursor and return without
            // error.
            cursorState = ClusterCursorManager::CursorState::Exhausted;
            break;
        }

        // Check whether we have exhausted the pipeline's results.
        if (next.isEOF()) {
            // We reached end-of-stream. If the cursor is not tailable, then we mark it as
            // exhausted. If it is tailable, usually we keep it open (i.e. "NotExhausted") even when
            // we reach end-of-stream. However, if all the remote cursors are exhausted, there is no
            // hope of returning data and thus we need to close the mongos cursor as well.
            if (!ccc->isTailable() || ccc->remotesExhausted()) {
                cursorState = ClusterCursorManager::CursorState::Exhausted;
            }
            break;
        }

        // If this result will fit into the current batch, add it. Otherwise, stash it in the cursor
        // to be returned on the next getMore.
        auto nextObj = *next.getResult();

        if (!FindCommon::haveSpaceForNext(nextObj, objCount, responseBuilder.bytesUsed())) {
            ccc->queueResult(nextObj);
            stashedResult = true;
            break;
        }

        // Set the postBatchResumeToken. For non-$changeStream aggregations, this will be empty.
        responseBuilder.setPostBatchResumeToken(ccc->getPostBatchResumeToken());
        responseBuilder.append(nextObj);
    }

    // For empty batches, or in the case where the final result was added to the batch rather than
    // being stashed, we update the PBRT here to ensure that it is the most recent available.
    if (!stashedResult) {
        responseBuilder.setPostBatchResumeToken(ccc->getPostBatchResumeToken());
    }

    ccc->detachFromOperationContext();

    int nShards = ccc->getNumRemotes();
    CursorId clusterCursorId = 0;

    if (cursorState == ClusterCursorManager::CursorState::NotExhausted) {
        auto authUsers = AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames();
        clusterCursorId = uassertStatusOK(Grid::get(opCtx)->getCursorManager()->registerCursor(
            opCtx,
            ccc.releaseCursor(),
            requestedNss,
            ClusterCursorManager::CursorType::MultiTarget,
            ClusterCursorManager::CursorLifetime::Mortal,
            authUsers));
    }

    // Fill out the aggregation metrics in CurOp.
    if (clusterCursorId > 0) {
        CurOp::get(opCtx)->debug().cursorid = clusterCursorId;
    }
    CurOp::get(opCtx)->debug().nShards = std::max(CurOp::get(opCtx)->debug().nShards, nShards);
    CurOp::get(opCtx)->debug().cursorExhausted = (clusterCursorId == 0);
    CurOp::get(opCtx)->debug().nreturned = responseBuilder.numDocs();

    responseBuilder.done(clusterCursorId, requestedNss.ns());

    auto bodyBuilder = replyBuilder.getBodyBuilder();
    CommandHelpers::appendSimpleCommandStatus(bodyBuilder, true);
    bodyBuilder.doneFast();

    return replyBuilder.releaseBody();
}

/**
 * Returns the output of the listCollections command filtered to the namespace 'nss'.
 */
BSONObj getUnshardedCollInfo(const Shard* primaryShard, const NamespaceString& nss) {
    ScopedDbConnection conn(primaryShard->getConnString());
    std::list<BSONObj> all =
        conn->getCollectionInfos(nss.db().toString(), BSON("name" << nss.coll()));
    if (all.empty()) {
        // Collection does not exist, return an empty object.
        return BSONObj();
    }
    return all.front();
}


/**
 * Returns the collection default collation or the simple collator if there is no default. If the
 * collection does not exist, then returns an empty BSON Object.
 */
BSONObj getDefaultCollationForUnshardedCollection(const BSONObj collectionInfo) {
    if (collectionInfo.isEmpty()) {
        // Collection does not exist, return an empty object.
        return BSONObj();
    }

    BSONObj defaultCollation = CollationSpec::kSimpleSpec;
    if (collectionInfo["options"].type() == BSONType::Object) {
        BSONObj collectionOptions = collectionInfo["options"].Obj();
        BSONElement collationElement;
        auto status = bsonExtractTypedField(
            collectionOptions, "collation", BSONType::Object, &collationElement);
        if (status.isOK()) {
            defaultCollation = collationElement.Obj().getOwned();
            uassert(ErrorCodes::BadValue,
                    "Default collation in collection metadata cannot be empty.",
                    !defaultCollation.isEmpty());
        } else if (status != ErrorCodes::NoSuchKey) {
            uassertStatusOK(status);
        }
    }
    return defaultCollation;
}

}  // namespace

std::unique_ptr<Pipeline, PipelineDeleter> targetShardsAndAddMergeCursors(
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
    auto hasChangeStream = liteParsedPipeline.hasChangeStream();
    auto shardDispatchResults = dispatchShardPipeline(
        aggRequest.serializeToCommandObj(), hasChangeStream, std::move(pipeline));

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
        // We have not split the pipeline, and will execute entirely on the remote shards. Set up an
        // empty local pipeline which we will attach the merge cursors stage to.
        mergePipeline = uassertStatusOK(Pipeline::parse(std::vector<BSONObj>(), expCtx));
    }

    cluster_aggregation_planner::addMergeCursorsSource(
        mergePipeline.get(),
        shardDispatchResults.commandForTargetedShards,
        std::move(shardDispatchResults.remoteCursors),
        targetedShards,
        shardCursorsSortSpec,
        Grid::get(expCtx->opCtx)->getExecutorPool()->getArbitraryExecutor(),
        hasChangeStream);

    return mergePipeline;
}

StatusWith<AggregationTargeter> AggregationTargeter::make(
    OperationContext* opCtx,
    const NamespaceString& executionNss,
    const std::function<std::unique_ptr<Pipeline, PipelineDeleter>(
        boost::optional<CachedCollectionRoutingInfo>)> buildPipelineFn,
    stdx::unordered_set<NamespaceString> involvedNamespaces,
    bool hasChangeStream,
    bool allowedToPassthrough) {

    // Check if any of the involved collections are sharded.
    bool involvesShardedCollections = [&]() {
        for (const auto& nss : involvedNamespaces) {
            const auto resolvedNsRoutingInfo =
                uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, nss));
            if (resolvedNsRoutingInfo.cm()) {
                return true;
            }
        }
        return false;
    }();

    // Determine whether this aggregation must be dispatched to all shards in the cluster.
    const bool mustRunOnAll = mustRunOnAllShards(executionNss, hasChangeStream);

    // If the routing table is valid, we obtain a reference to it. If the table is not valid, then
    // either the database does not exist, or there are no shards in the cluster. In the latter
    // case, we always return an empty cursor. In the former case, if the requested aggregation is a
    // $changeStream, we allow the operation to continue so that stream cursors can be established
    // on the given namespace before the database or collection is actually created. If the database
    // does not exist and this is not a $changeStream, then we return an empty cursor.
    boost::optional<CachedCollectionRoutingInfo> routingInfo;
    auto executionNsRoutingInfoStatus = getExecutionNsRoutingInfo(opCtx, executionNss);
    if (executionNsRoutingInfoStatus.isOK()) {
        routingInfo = std::move(executionNsRoutingInfoStatus.getValue());
    } else if (!(hasChangeStream &&
                 executionNsRoutingInfoStatus == ErrorCodes::NamespaceNotFound)) {
        return executionNsRoutingInfoStatus.getStatus();
    }

    // If we don't have a routing table, then this is a $changeStream which must run on all shards.
    invariant(routingInfo || (mustRunOnAll && hasChangeStream));

    // A pipeline is allowed to passthrough to the primary shard iff the following conditions are
    // met:
    //
    // 1. The namespace of the aggregate and any other involved namespaces are unsharded.
    // 2. Is allowed to be forwarded to shards. For example, $currentOp with localOps: true should
    //    run locally on mongos and cannot be forwarded to a shard.
    // 3. Does not need to run on all shards. For example, a pipeline with a $changeStream or
    //    $currentOp.
    // 4. Doesn't need transformation via DocumentSource::serialize(). For example, list sessions
    //    needs to include information about users that can only be deduced on mongos.
    if (routingInfo && !routingInfo->cm() && !mustRunOnAll && allowedToPassthrough &&
        !involvesShardedCollections) {
        return AggregationTargeter{TargetingPolicy::kPassthrough, nullptr, routingInfo};
    } else {
        auto pipeline = buildPipelineFn(routingInfo);
        auto policy = pipeline->requiredToRunOnMongos() ? TargetingPolicy::kMongosRequired
                                                        : TargetingPolicy::kAnyShard;
        return AggregationTargeter{policy, std::move(pipeline), routingInfo};
    }
}

Status runPipelineOnPrimaryShard(OperationContext* opCtx,
                                 const ClusterAggregate::Namespaces& namespaces,
                                 const CachedDatabaseInfo& dbInfo,
                                 boost::optional<ExplainOptions::Verbosity> explain,
                                 Document serializedCommand,
                                 const PrivilegeVector& privileges,
                                 BSONObjBuilder* out) {
    // Format the command for the shard. This adds the 'fromMongos' field, wraps the command as an
    // explain if necessary, and rewrites the result into a format safe to forward to shards.
    BSONObj cmdObj =
        CommandHelpers::filterCommandRequestForPassthrough(createPassthroughCommandForShard(
            opCtx, serializedCommand, explain, boost::none, nullptr, BSONObj()));

    const auto shardId = dbInfo.primary()->getId();
    const auto cmdObjWithShardVersion = (shardId != ShardRegistry::kConfigServerShardId)
        ? appendShardVersion(std::move(cmdObj), ChunkVersion::UNSHARDED())
        : std::move(cmdObj);

    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        namespaces.executionNss.db().toString(),
        {{shardId, appendDbVersionIfPresent(cmdObjWithShardVersion, dbInfo)}},
        ReadPreferenceSetting::get(opCtx),
        Shard::RetryPolicy::kIdempotent);
    auto response = ars.next();
    invariant(ars.done());

    uassertStatusOK(response.swResponse);
    auto commandStatus = getStatusFromCommandResult(response.swResponse.getValue().data);

    if (ErrorCodes::isStaleShardVersionError(commandStatus.code())) {
        uassertStatusOK(commandStatus.withContext("command failed because of stale config"));
    } else if (ErrorCodes::isSnapshotError(commandStatus.code())) {
        uassertStatusOK(
            commandStatus.withContext("command failed because can not establish a snapshot"));
    }

    BSONObj result;
    if (explain) {
        // If this was an explain, then we get back an explain result object rather than a cursor.
        result = response.swResponse.getValue().data;
    } else {
        result = uassertStatusOK(
            storePossibleCursor(opCtx,
                                shardId,
                                *response.shardHostAndPort,
                                response.swResponse.getValue().data,
                                namespaces.requestedNss,
                                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                                Grid::get(opCtx)->getCursorManager(),
                                privileges,
                                TailableModeEnum::kNormal));
    }

    // First append the properly constructed writeConcernError. It will then be skipped
    // in appendElementsUnique.
    if (auto wcErrorElem = result["writeConcernError"]) {
        appendWriteConcernErrorToCmdResponse(shardId, wcErrorElem, *out);
    }

    out->appendElementsUnique(CommandHelpers::filterCommandReplyForPassthrough(result));

    return getStatusFromCommandResult(out->asTempObj());
}

Status runPipelineOnMongoS(const ClusterAggregate::Namespaces& namespaces,
                           long long batchSize,
                           std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                           BSONObjBuilder* result,
                           const PrivilegeVector& privileges) {
    auto expCtx = pipeline->getContext();

    // We should never receive a pipeline which cannot run on mongoS.
    invariant(!expCtx->explain);
    invariant(pipeline->canRunOnMongos());

    // Verify that the first stage can produce input for the remainder of the pipeline.
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Aggregation pipeline must be run on mongoS, but "
                          << pipeline->getSources().front()->getSourceName()
                          << " is not capable of producing input",
            !pipeline->getSources().front()->constraints().requiresInputDocSource);

    // Register the new mongoS cursor, and retrieve the initial batch of results.
    auto cursorResponse = establishMergingMongosCursor(
        expCtx->opCtx, batchSize, namespaces.requestedNss, std::move(pipeline), privileges);

    // We don't need to storePossibleCursor or propagate writeConcern errors; a pipeline with
    // writing stages like $out can never run on mongoS. Filter the command response and return
    // immediately.
    CommandHelpers::filterCommandReplyForPassthrough(cursorResponse, result);
    return getStatusFromCommandResult(result->asTempObj());
}

Status dispatchPipelineAndMerge(OperationContext* opCtx,
                                AggregationTargeter targeter,
                                Document serializedCommand,
                                long long batchSize,
                                const ClusterAggregate::Namespaces& namespaces,
                                const PrivilegeVector& privileges,
                                BSONObjBuilder* result,
                                bool hasChangeStream) {
    auto expCtx = targeter.pipeline->getContext();
    // If not, split the pipeline as necessary and dispatch to the relevant shards.
    auto shardDispatchResults =
        dispatchShardPipeline(serializedCommand, hasChangeStream, std::move(targeter.pipeline));

    // If the operation is an explain, then we verify that it succeeded on all targeted
    // shards, write the results to the output builder, and return immediately.
    if (expCtx->explain) {
        return appendExplainResults(std::move(shardDispatchResults), expCtx, result);
    }

    // If this isn't an explain, then we must have established cursors on at least one
    // shard.
    invariant(shardDispatchResults.remoteCursors.size() > 0);

    // If we sent the entire pipeline to a single shard, store the remote cursor and return.
    if (!shardDispatchResults.splitPipeline) {
        invariant(shardDispatchResults.remoteCursors.size() == 1);
        auto&& remoteCursor = std::move(shardDispatchResults.remoteCursors.front());
        const auto shardId = remoteCursor->getShardId().toString();
        const auto reply = uassertStatusOK(storePossibleCursor(opCtx,
                                                               namespaces.requestedNss,
                                                               std::move(remoteCursor),
                                                               privileges,
                                                               expCtx->tailableMode));
        return appendCursorResponseToCommandResult(shardId, reply, result);
    }

    // If we have the exchange spec then dispatch all consumers.
    if (shardDispatchResults.exchangeSpec) {
        shardDispatchResults = dispatchExchangeConsumerPipeline(
            expCtx, namespaces.executionNss, serializedCommand, &shardDispatchResults);
    }

    // If we reach here, we have a merge pipeline to dispatch.
    return dispatchMergingPipeline(expCtx,
                                   namespaces,
                                   serializedCommand,
                                   batchSize,
                                   targeter.routingInfo,
                                   std::move(shardDispatchResults),
                                   result,
                                   privileges,
                                   hasChangeStream);
}

std::pair<BSONObj, boost::optional<UUID>> getCollationAndUUID(
    const boost::optional<CachedCollectionRoutingInfo>& routingInfo,
    const NamespaceString& nss,
    const BSONObj& collation) {
    const bool collectionIsSharded = (routingInfo && routingInfo->cm());
    const bool collectionIsNotSharded = (routingInfo && !routingInfo->cm());

    // If this is a collectionless aggregation, we immediately return the user-
    // defined collation if one exists, or an empty BSONObj otherwise. Collectionless aggregations
    // generally run on the 'admin' database, the standard logic would attempt to resolve its
    // non-existent UUID and collation by sending a specious 'listCollections' command to the config
    // servers.
    if (nss.isCollectionlessAggregateNS()) {
        return {collation, boost::none};
    }

    // If the collection is unsharded, obtain collInfo from the primary shard.
    const auto unshardedCollInfo = collectionIsNotSharded
        ? getUnshardedCollInfo(routingInfo->db().primary().get(), nss)
        : BSONObj();

    // Return the collection UUID if available, or boost::none otherwise.
    const auto getUUID = [&]() -> auto {
        if (collectionIsSharded) {
            return routingInfo->cm()->getUUID();
        } else {
            return unshardedCollInfo["info"] && unshardedCollInfo["info"]["uuid"]
                ? boost::optional<UUID>{uassertStatusOK(
                      UUID::parse(unshardedCollInfo["info"]["uuid"]))}
                : boost::optional<UUID>{boost::none};
        }
    };

    // If the collection exists, return its default collation, or the simple
    // collation if no explicit default is present. If the collection does not
    // exist, return an empty BSONObj.
    const auto getCollation = [&]() -> auto {
        if (!collectionIsSharded && !collectionIsNotSharded) {
            return BSONObj();
        }
        if (collectionIsNotSharded) {
            return getDefaultCollationForUnshardedCollection(unshardedCollInfo);
        } else {
            return routingInfo->cm()->getDefaultCollator()
                ? routingInfo->cm()->getDefaultCollator()->getSpec().toBSON()
                : CollationSpec::kSimpleSpec;
        }
    };

    // If the user specified an explicit collation, we always adopt it. Otherwise,
    // obtain the collection default or simple collation as appropriate, and return
    // it along with the collection's UUID.
    return {collation.isEmpty() ? getCollation() : collation, getUUID()};
}

}  // namespace mongo::sharded_agg_helpers
