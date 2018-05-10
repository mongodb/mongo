/**
*    Copyright (C) 2016 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/s/commands/cluster_aggregate.h"

#include <boost/intrusive_ptr.hpp>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/cluster_aggregation_planner.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_merge_cursors.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/db/views/view.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/commands/pipeline_s.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_query_knobs.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/query/router_stage_update_on_add_shard.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_utils.h"

namespace mongo {

MONGO_FP_DECLARE(clusterAggregateHangBeforeEstablishingShardCursors);

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

Status appendExplainResults(
    const std::vector<AsyncRequestsSender::Response>& shardResults,
    const boost::intrusive_ptr<ExpressionContext>& mergeCtx,
    const std::unique_ptr<Pipeline, PipelineDeleter>& pipelineForTargetedShards,
    const std::unique_ptr<Pipeline, PipelineDeleter>& pipelineForMerging,
    BSONObjBuilder* result) {
    if (pipelineForTargetedShards->isSplitForShards()) {
        *result << "mergeType"
                << (pipelineForMerging->canRunOnMongos()
                        ? "mongos"
                        : pipelineForMerging->needsPrimaryShardMerger() ? "primaryShard"
                                                                        : "anyShard")
                << "splitPipeline"
                << Document{
                       {"shardsPart",
                        pipelineForTargetedShards->writeExplainOps(*mergeCtx->explain)},
                       {"mergerPart", pipelineForMerging->writeExplainOps(*mergeCtx->explain)}};
    } else {
        *result << "splitPipeline" << BSONNULL;
    }

    BSONObjBuilder shardExplains(result->subobjStart("shards"));
    for (const auto& shardResult : shardResults) {
        invariant(shardResult.shardHostAndPort);
        shardExplains.append(shardResult.shardId.toString(),
                             BSON("host" << shardResult.shardHostAndPort->toString() << "stages"
                                         << shardResult.swResponse.getValue().data["stages"]));
    }

    return Status::OK();
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

bool mustRunOnAllShards(const NamespaceString& nss,
                        const boost::optional<CachedCollectionRoutingInfo>& routingInfo,
                        const LiteParsedPipeline& litePipe) {
    // Non-existent routing table is only valid for $changeStream aggregations which have been
    // opened prior to the creation of the target database.
    invariant(routingInfo || litePipe.hasChangeStream());
    // The following aggregations must be routed to all shards:
    // - Any collectionless aggregation such as $currentOp
    // - $changeStream on a non-existent database
    // - $changeStream on a sharded collection
    const bool dbExists = static_cast<bool>(routingInfo);
    const bool nsIsSharded = dbExists && static_cast<bool>(routingInfo->cm());
    return !dbExists || nss.isCollectionlessAggregateNS() ||
        (nsIsSharded && litePipe.hasChangeStream());
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

    // This call to getCollectionRoutingInfo will return !OK if the database does not exist.
    return Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, execNss);
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
        return {shardIds.begin(), shardIds.end()};
    }

    // If we don't need to run on all shards, then we should always have a valid routing table.
    invariant(routingInfo);

    return getTargetedShardsForQuery(opCtx, *routingInfo, shardQuery, collation);
}

BSONObj createCommandForTargetedShards(
    OperationContext* opCtx,
    const AggregationRequest& request,
    const BSONObj originalCmdObj,
    const std::unique_ptr<Pipeline, PipelineDeleter>& pipelineForTargetedShards,
    boost::optional<LogicalTime> atClusterTime) {
    // Create the command for the shards.
    MutableDocument targetedCmd(request.serializeToCommandObj());
    targetedCmd[AggregationRequest::kFromMongosName] = Value(true);

    // If 'pipelineForTargetedShards' is 'nullptr', this is an unsharded direct passthrough.
    if (pipelineForTargetedShards) {
        targetedCmd[AggregationRequest::kPipelineName] =
            Value(pipelineForTargetedShards->serialize());

        if (pipelineForTargetedShards->isSplitForShards()) {
            targetedCmd[AggregationRequest::kNeedsMergeName] = Value(true);
            targetedCmd[AggregationRequest::kCursorName] =
                Value(DOC(AggregationRequest::kBatchSizeName << 0));
        }
    }

    // If this pipeline is not split, ensure that the write concern is propagated if present.
    if (!pipelineForTargetedShards || !pipelineForTargetedShards->isSplitForShards()) {
        targetedCmd["writeConcern"] = Value(originalCmdObj["writeConcern"]);
    }

    // If this is a request for an aggregation explain, then we must wrap the aggregate inside an
    // explain command.
    if (auto explainVerbosity = request.getExplain()) {
        targetedCmd.reset(wrapAggAsExplain(targetedCmd.freeze(), *explainVerbosity));
    }

    if (opCtx->getTxnNumber()) {
        invariant(!targetedCmd.hasField(OperationSessionInfo::kTxnNumberFieldName));
        targetedCmd[OperationSessionInfo::kTxnNumberFieldName] =
            Value(static_cast<long long>(*opCtx->getTxnNumber()));
    }

    // TODO: SERVER-34078
    BSONObj cmdObj =
        (atClusterTime ? appendAtClusterTime(targetedCmd.freeze().toBson(), *atClusterTime)
                       : targetedCmd.freeze().toBson());

    // agg creates temp collection and should handle implicit create separately.
    return appendAllowImplicitCreate(cmdObj, true);
}

BSONObj createCommandForMergingShard(
    const AggregationRequest& request,
    const boost::intrusive_ptr<ExpressionContext>& mergeCtx,
    const BSONObj originalCmdObj,
    const std::unique_ptr<Pipeline, PipelineDeleter>& pipelineForMerging) {
    MutableDocument mergeCmd(request.serializeToCommandObj());

    mergeCmd["pipeline"] = Value(pipelineForMerging->serialize());
    mergeCmd[AggregationRequest::kFromMongosName] = Value(true);
    mergeCmd["writeConcern"] = Value(originalCmdObj["writeConcern"]);

    // If the user didn't specify a collation already, make sure there's a collation attached to
    // the merge command, since the merging shard may not have the collection metadata.
    if (mergeCmd.peek()["collation"].missing()) {
        mergeCmd["collation"] = mergeCtx->getCollator()
            ? Value(mergeCtx->getCollator()->getSpec().toBSON())
            : Value(Document{CollationSpec::kSimpleSpec});
    }

    // agg creates temp collection and should handle implicit create separately.
    return appendAllowImplicitCreate(mergeCmd.freeze().toBson(), true);
}

std::vector<RemoteCursor> establishShardCursors(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const LiteParsedPipeline& litePipe,
    boost::optional<CachedCollectionRoutingInfo>& routingInfo,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    const BSONObj& shardQuery,
    const BSONObj& collation) {
    LOG(1) << "Dispatching command " << redact(cmdObj) << " to establish cursors on shards";

    const bool mustRunOnAll = mustRunOnAllShards(nss, routingInfo, litePipe);
    std::set<ShardId> shardIds =
        getTargetedShards(opCtx, mustRunOnAll, routingInfo, shardQuery, collation);
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
                            false /* do not allow partial results */);
}

struct DispatchShardPipelineResults {
    // True if this pipeline was split, and the second half of the pipeline needs to be run on the
    // primary shard for the database.
    bool needsPrimaryShardMerge;

    // Populated if this *is not* an explain, this vector represents the cursors on the remote
    // shards.
    std::vector<RemoteCursor> remoteCursors;

    // Populated if this *is* an explain, this vector represents the results from each shard.
    std::vector<AsyncRequestsSender::Response> remoteExplainOutput;

    // The half of the pipeline that was sent to each shard, or the entire pipeline if there was
    // only one shard targeted.
    std::unique_ptr<Pipeline, PipelineDeleter> pipelineForTargetedShards;

    // The merging half of the pipeline if more than one shard was targeted, otherwise nullptr.
    std::unique_ptr<Pipeline, PipelineDeleter> pipelineForMerging;

    // The command object to send to the targeted shards.
    BSONObj commandForTargetedShards;
};

/**
 * Targets shards for the pipeline and returns a struct with the remote cursors or results, and
 * the pipeline that will need to be executed to merge the results from the remotes. If a stale
 * shard version is encountered, refreshes the routing table and tries again.
 */
DispatchShardPipelineResults dispatchShardPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& executionNss,
    BSONObj originalCmdObj,
    const AggregationRequest& aggRequest,
    const LiteParsedPipeline& liteParsedPipeline,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline) {
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

    auto pipelineForTargetedShards = std::move(pipeline);
    std::unique_ptr<Pipeline, PipelineDeleter> pipelineForMerging;

    auto executionNsRoutingInfoStatus = getExecutionNsRoutingInfo(opCtx, executionNss);

    // If this is a $changeStream, we swallow NamespaceNotFound exceptions and continue.
    // Otherwise, uassert on all exceptions here.
    if (!(liteParsedPipeline.hasChangeStream() &&
          executionNsRoutingInfoStatus == ErrorCodes::NamespaceNotFound)) {
        uassertStatusOK(executionNsRoutingInfoStatus);
    }

    auto executionNsRoutingInfo = executionNsRoutingInfoStatus.isOK()
        ? std::move(executionNsRoutingInfoStatus.getValue())
        : boost::optional<CachedCollectionRoutingInfo>{};

    // Determine whether we can run the entire aggregation on a single shard.
    const bool mustRunOnAll =
        mustRunOnAllShards(executionNss, executionNsRoutingInfo, liteParsedPipeline);
    std::set<ShardId> shardIds = getTargetedShards(
        opCtx, mustRunOnAll, executionNsRoutingInfo, shardQuery, aggRequest.getCollation());

    auto atClusterTime = computeAtClusterTime(
        opCtx, mustRunOnAll, shardIds, executionNss, shardQuery, aggRequest.getCollation());

    invariant(!atClusterTime || *atClusterTime != LogicalTime::kUninitialized);

    // Don't need to split the pipeline if we are only targeting a single shard, unless:
    // - There is a stage that needs to be run on the primary shard and the single target shard
    //   is not the primary.
    // - The pipeline contains one or more stages which must always merge on mongoS.
    const bool needsSplit = (shardIds.size() > 1u || needsMongosMerge ||
                             (needsPrimaryShardMerge && executionNsRoutingInfo &&
                              *(shardIds.begin()) != executionNsRoutingInfo->db().primaryId()));

    if (needsSplit) {
        pipelineForMerging = std::move(pipelineForTargetedShards);
        pipelineForTargetedShards = pipelineForMerging->splitForSharded();
    }

    // Generate the command object for the targeted shards.
    BSONObj targetedCommand = createCommandForTargetedShards(
        opCtx, aggRequest, originalCmdObj, pipelineForTargetedShards, atClusterTime);

    // Refresh the shard registry if we're targeting all shards.  We need the shard registry
    // to be at least as current as the logical time used when creating the command for
    // $changeStream to work reliably, so we do a "hard" reload.
    if (mustRunOnAll) {
        auto* shardRegistry = Grid::get(opCtx)->shardRegistry();
        if (!shardRegistry->reload(opCtx)) {
            shardRegistry->reload(opCtx);
        }
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
                                        liteParsedPipeline,
                                        executionNsRoutingInfo,
                                        targetedCommand,
                                        ReadPreferenceSetting::get(opCtx),
                                        shardQuery,
                                        aggRequest.getCollation());
    }

    // Record the number of shards involved in the aggregation. If we are required to merge on
    // the primary shard, but the primary shard was not in the set of targeted shards, then we
    // must increment the number of involved shards.
    CurOp::get(opCtx)->debug().nShards =
        shardIds.size() + (needsPrimaryShardMerge && executionNsRoutingInfo &&
                           !shardIds.count(executionNsRoutingInfo->db().primaryId()));

    return DispatchShardPipelineResults{needsPrimaryShardMerge,
                                        std::move(cursors),
                                        std::move(shardResults),
                                        std::move(pipelineForTargetedShards),
                                        std::move(pipelineForMerging),
                                        targetedCommand};
}

Shard::CommandResponse establishMergingShardCursor(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const BSONObj mergeCmdObj,
                                                   const ShardId& mergingShardId) {
    const auto mergingShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, mergingShardId));

    return uassertStatusOK(
        mergingShard->runCommandWithFixedRetryAttempts(opCtx,
                                                       ReadPreferenceSetting::get(opCtx),
                                                       nss.db().toString(),
                                                       mergeCmdObj,
                                                       Shard::RetryPolicy::kIdempotent));
}

BSONObj establishMergingMongosCursor(OperationContext* opCtx,
                                     const AggregationRequest& request,
                                     const NamespaceString& requestedNss,
                                     BSONObj cmdToRunOnNewShards,
                                     const LiteParsedPipeline& liteParsedPipeline,
                                     std::unique_ptr<Pipeline, PipelineDeleter> pipelineForMerging,
                                     std::vector<RemoteCursor> cursors) {

    ClusterClientCursorParams params(requestedNss, ReadPreferenceSetting::get(opCtx));

    params.originatingCommandObj = CurOp::get(opCtx)->opDescription().getOwned();
    params.tailableMode = pipelineForMerging->getContext()->tailableMode;
    params.mergePipeline = std::move(pipelineForMerging);
    params.remotes = std::move(cursors);
    // A batch size of 0 is legal for the initial aggregate, but not valid for getMores, the batch
    // size we pass here is used for getMores, so do not specify a batch size if the initial request
    // had a batch size of 0.
    params.batchSize = request.getBatchSize() == 0
        ? boost::none
        : boost::optional<long long>(request.getBatchSize());
    params.lsid = opCtx->getLogicalSessionId();
    params.txnNumber = opCtx->getTxnNumber();

    if (liteParsedPipeline.hasChangeStream()) {
        // For change streams, we need to set up a custom stage to establish cursors on new shards
        // when they are added.  Be careful to extract the targeted shard IDs before the remote
        // cursors are transferred from the ClusterClientCursorParams to the AsyncResultsMerger.
        std::vector<ShardId> shardIds;
        for (const auto& remote : params.remotes) {
            shardIds.emplace_back(remote.getShardId().toString());
        }

        params.createCustomCursorSource = [cmdToRunOnNewShards,
                                           shardIds](OperationContext* opCtx,
                                                     executor::TaskExecutor* executor,
                                                     ClusterClientCursorParams* params) {
            return stdx::make_unique<RouterStageUpdateOnAddShard>(
                opCtx, executor, params, std::move(shardIds), cmdToRunOnNewShards);
        };
    }
    auto ccc = ClusterClientCursorImpl::make(
        opCtx, Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(), std::move(params));

    auto cursorState = ClusterCursorManager::CursorState::NotExhausted;
    BSONObjBuilder cursorResponse;

    CursorResponseBuilder responseBuilder(true, &cursorResponse);

    for (long long objCount = 0; objCount < request.getBatchSize(); ++objCount) {
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
            break;
        }

        responseBuilder.append(nextObj);
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

    CommandHelpers::appendSimpleCommandStatus(cursorResponse, true);

    return cursorResponse.obj();
}

BSONObj getDefaultCollationForUnshardedCollection(const Shard* primaryShard,
                                                  const NamespaceString& nss) {
    ScopedDbConnection conn(primaryShard->getConnString());
    BSONObj defaultCollation;
    std::list<BSONObj> all =
        conn->getCollectionInfos(nss.db().toString(), BSON("name" << nss.coll()));
    if (all.empty()) {
        return defaultCollation;
    }
    BSONObj collectionInfo = all.front();
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

ShardId pickMergingShard(OperationContext* opCtx,
                         const DispatchShardPipelineResults& dispatchResults,
                         ShardId primaryShard) {
    auto& prng = opCtx->getClient()->getPrng();
    // If we cannot merge on mongoS, establish the merge cursor on a shard. Perform the merging
    // command on random shard, unless the pipeline dictates that it needs to be run on the primary
    // shard for the database.
    return dispatchResults.needsPrimaryShardMerge
        ? primaryShard
        : dispatchResults.remoteCursors[prng.nextInt32(dispatchResults.remoteCursors.size())]
              .getShardId()
              .toString();
}

// "Resolve" involved namespaces and verify that none of them are sharded. We won't try to execute
// anything on a mongos, but we still have to populate this map so that any $lookups, etc. will be
// able to have a resolved view definition. It's okay that this is incorrect, we will repopulate the
// real namespace map on the mongod. Note that this function must be called before forwarding an
// aggregation command on an unsharded collection, in order to validate that none of the involved
// collections are sharded.
StringMap<ExpressionContext::ResolvedNamespace> resolveInvolvedNamespaces(
    OperationContext* opCtx, const LiteParsedPipeline& litePipe) {
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    for (auto&& nss : litePipe.getInvolvedNamespaces()) {
        const auto resolvedNsRoutingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        uassert(
            28769, str::stream() << nss.ns() << " cannot be sharded", !resolvedNsRoutingInfo.cm());
        resolvedNamespaces.try_emplace(nss.coll(), nss, std::vector<BSONObj>{});
    }
    return resolvedNamespaces;
}

// Build an appropriate ExpressionContext for the pipeline. This helper validates that all involved
// namespaces are unsharded, obtains the appropriate user-defined or default collator, and creates a
// MongoProcessInterface for use by the pipeline's stages.
boost::intrusive_ptr<ExpressionContext> makeExpressionContext(
    OperationContext* opCtx,
    const NamespaceString& executionNss,
    const AggregationRequest& request,
    const LiteParsedPipeline& litePipe,
    const boost::optional<CachedCollectionRoutingInfo>& routingInfo) {
    // Determine the appropriate collation for the ExpressionContext.
    const bool collectionIsSharded = (routingInfo && routingInfo->cm());
    const bool collectionIsNotSharded = (routingInfo && !routingInfo->cm());
    std::unique_ptr<CollatorInterface> collation;

    if (!request.getCollation().isEmpty()) {
        collation = uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                        ->makeFromBSON(request.getCollation()));
    } else if (collectionIsSharded) {
        if (routingInfo->cm()->getDefaultCollator()) {
            collation = routingInfo->cm()->getDefaultCollator()->clone();
        }
    } else if (collectionIsNotSharded) {
        // Get collection metadata from primary chunk.
        auto collationObj = getDefaultCollationForUnshardedCollection(
            routingInfo->db().primary().get(), executionNss);
        if (!collationObj.isEmpty()) {
            collation = uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                            ->makeFromBSON(collationObj));
        }
    }

    // Create the expression context, and set 'inMongos' to true before returning it. We explicitly
    // do *not* set mergeCtx->tempDir. Note that we resolve the pipeline's involved namespaces here,
    // validating that none of them are sharded.
    auto mergeCtx = new ExpressionContext(opCtx,
                                          request,
                                          std::move(collation),
                                          std::make_shared<PipelineS::MongoSInterface>(),
                                          resolveInvolvedNamespaces(opCtx, litePipe));
    mergeCtx->inMongos = true;
    return mergeCtx;
}

// Runs a pipeline on mongoS, having first validated that it is eligible to do so. This can be a
// pipeline which is split for merging, or an intact pipeline which must run entirely on mongoS.
Status runPipelineOnMongoS(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                           const ClusterAggregate::Namespaces& namespaces,
                           const AggregationRequest& request,
                           BSONObj cmdObj,
                           const LiteParsedPipeline& litePipe,
                           std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                           std::vector<RemoteCursor>&& cursors,
                           BSONObjBuilder* result) {
    // We should never receive a pipeline intended for the shards, or which cannot run on mongoS.
    invariant(!pipeline->isSplitForShards());
    invariant(pipeline->canRunOnMongos());

    const auto& requestedNss = namespaces.requestedNss;
    const auto opCtx = expCtx->opCtx;

    // If this is an unsplit mongoS-only pipeline, verify that the first stage can produce input for
    // the remainder of the pipeline.
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Aggregation pipeline must be run on mongoS, but "
                          << pipeline->getSources().front()->getSourceName()
                          << " is not capable of producing input",
            pipeline->isSplitForMerge() ||
                !pipeline->getSources().front()->constraints().requiresInputDocSource);

    // If this is an explain and the pipeline is not split, write the explain output and return.
    if (expCtx->explain && !pipeline->isSplitForMerge()) {
        *result << "splitPipeline" << BSONNULL << "mongos"
                << Document{{"host", getHostNameCachedAndPort()},
                            {"stages", pipeline->writeExplainOps(*expCtx->explain)}};
        return Status::OK();
    }

    // Register the new mongoS cursor, and retrieve the initial batch of results.
    auto cursorResponse = establishMergingMongosCursor(
        opCtx, request, requestedNss, cmdObj, litePipe, std::move(pipeline), std::move(cursors));

    // We don't need to storePossibleCursor or propagate writeConcern errors; an $out pipeline
    // can never run on mongoS. Filter the command response and return immediately.
    CommandHelpers::filterCommandReplyForPassthrough(cursorResponse, result);
    return getStatusFromCommandResult(result->asTempObj());
}

Status dispatchMergingPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               const ClusterAggregate::Namespaces& namespaces,
                               const AggregationRequest& request,
                               BSONObj cmdObj,
                               const LiteParsedPipeline& litePipe,
                               const boost::optional<CachedCollectionRoutingInfo>& routingInfo,
                               DispatchShardPipelineResults& shardDispatchResults,
                               BSONObjBuilder* result) {
    // We should never be in a situation where we call this function on a non-merge pipeline.
    auto& mergingPipeline = shardDispatchResults.pipelineForMerging;
    invariant(mergingPipeline && mergingPipeline->isSplitForMerge());

    const auto opCtx = expCtx->opCtx;

    // First, check whether we can merge on the mongoS. If the merge pipeline MUST run on mongoS,
    // then ignore the internalQueryProhibitMergingOnMongoS parameter.
    if (mergingPipeline->requiredToRunOnMongos() ||
        (!internalQueryProhibitMergingOnMongoS.load() && mergingPipeline->canRunOnMongos())) {
        return runPipelineOnMongoS(expCtx,
                                   namespaces,
                                   request,
                                   shardDispatchResults.commandForTargetedShards,
                                   litePipe,
                                   std::move(mergingPipeline),
                                   std::move(shardDispatchResults.remoteCursors),
                                   result);
    }

    // If we are not merging on mongoS, then this is not a $changeStream aggregation, and we
    // therefore must have a valid routing table.
    invariant(routingInfo);

    // TODO SERVER-33683 allowing an aggregation within a transaction can lead to a deadlock in the
    // SessionCatalog when a pipeline with a $mergeCursors sends a getMore to itself.
    uassert(50732,
            "Cannot specify a transaction number in combination with an aggregation on mongos when "
            "merging on a shard",
            !opCtx->getTxnNumber());

    ShardId mergingShardId =
        pickMergingShard(opCtx, shardDispatchResults, routingInfo->db().primaryId());

    cluster_aggregation_planner::addMergeCursorsSource(
        mergingPipeline.get(),
        std::move(shardDispatchResults.remoteCursors),
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor());

    auto mergeCmdObj = createCommandForMergingShard(request, expCtx, cmdObj, mergingPipeline);

    // Dispatch $mergeCursors to the chosen shard, store the resulting cursor, and return.
    auto mergeResponse =
        establishMergingShardCursor(opCtx, namespaces.executionNss, mergeCmdObj, mergingShardId);

    auto mergeCursorResponse = uassertStatusOK(storePossibleCursor(
        opCtx, namespaces.requestedNss, mergingShardId, mergeResponse, expCtx->tailableMode));

    return appendCursorResponseToCommandResult(mergingShardId, mergeCursorResponse, result);
}

void appendEmptyResultSetWithStatus(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    Status status,
                                    BSONObjBuilder* result) {
    // Rewrite ShardNotFound as NamespaceNotFound so that appendEmptyResultSet swallows it.
    if (status == ErrorCodes::ShardNotFound) {
        status = {ErrorCodes::NamespaceNotFound, status.reason()};
    }
    appendEmptyResultSet(opCtx, *result, status, nss.ns());
}

}  // namespace

Status ClusterAggregate::runAggregate(OperationContext* opCtx,
                                      const Namespaces& namespaces,
                                      const AggregationRequest& request,
                                      BSONObj cmdObj,
                                      BSONObjBuilder* result) {
    auto executionNsRoutingInfoStatus = getExecutionNsRoutingInfo(opCtx, namespaces.executionNss);
    boost::optional<CachedCollectionRoutingInfo> routingInfo;
    LiteParsedPipeline litePipe(request);

    // If the routing table is valid, we obtain a reference to it. If the table is not valid, then
    // either the database does not exist, or there are no shards in the cluster. In the latter
    // case, we always return an empty cursor. In the former case, if the requested aggregation is a
    // $changeStream, we allow the operation to continue so that stream cursors can be established
    // on the given namespace before the database or collection is actually created. If the database
    // does not exist and this is not a $changeStream, then we return an empty cursor.
    if (executionNsRoutingInfoStatus.isOK()) {
        routingInfo = std::move(executionNsRoutingInfoStatus.getValue());
    } else if (!(litePipe.hasChangeStream() &&
                 executionNsRoutingInfoStatus == ErrorCodes::NamespaceNotFound)) {
        appendEmptyResultSetWithStatus(
            opCtx, namespaces.requestedNss, executionNsRoutingInfoStatus.getStatus(), result);
        return Status::OK();
    }

    // Determine whether this aggregation must be dispatched to all shards in the cluster.
    const bool mustRunOnAll = mustRunOnAllShards(namespaces.executionNss, routingInfo, litePipe);

    // If we don't have a routing table, then this is a $changeStream which must run on all shards.
    invariant(routingInfo || (mustRunOnAll && litePipe.hasChangeStream()));

    // If this pipeline is not on a sharded collection, is allowed to be forwarded to shards, does
    // not need to run on all shards, and doesn't need to go through DocumentSource::serialize(),
    // then go ahead and pass it through to the owning shard unmodified. Note that we first call
    // resolveInvolvedNamespaces to validate that none of the namespaces are sharded.
    if (routingInfo && !routingInfo->cm() && !mustRunOnAll &&
        litePipe.allowedToForwardFromMongos() && litePipe.allowedToPassthroughFromMongos()) {
        resolveInvolvedNamespaces(opCtx, litePipe);
        const auto primaryShardId = routingInfo->db().primary()->getId();
        return aggPassthrough(opCtx, namespaces, primaryShardId, cmdObj, request, litePipe, result);
    }

    // Build an ExpressionContext for the pipeline. This instantiates an appropriate collator,
    // resolves all involved namespaces, and creates a shared MongoProcessInterface for use by the
    // pipeline's stages.
    auto expCtx =
        makeExpressionContext(opCtx, namespaces.executionNss, request, litePipe, routingInfo);

    // Parse and optimize the full pipeline.
    auto pipeline = uassertStatusOK(Pipeline::parse(request.getPipeline(), expCtx));
    pipeline->optimizePipeline();

    // Check whether the entire pipeline must be run on mongoS.
    if (pipeline->requiredToRunOnMongos()) {
        return runPipelineOnMongoS(
            expCtx, namespaces, request, cmdObj, litePipe, std::move(pipeline), {}, result);
    }

    // If not, split the pipeline as necessary and dispatch to the relevant shards.
    auto shardDispatchResults = dispatchShardPipeline(
        expCtx, namespaces.executionNss, cmdObj, request, litePipe, std::move(pipeline));

    // If the operation is an explain, then we verify that it succeeded on all targeted shards,
    // write the results to the output builder, and return immediately.
    if (expCtx->explain) {
        uassertAllShardsSupportExplain(shardDispatchResults.remoteExplainOutput);
        return appendExplainResults(std::move(shardDispatchResults.remoteExplainOutput),
                                    expCtx,
                                    shardDispatchResults.pipelineForTargetedShards,
                                    shardDispatchResults.pipelineForMerging,
                                    result);
    }

    // If this isn't an explain, then we must have established cursors on at least one shard.
    invariant(shardDispatchResults.remoteCursors.size() > 0);

    // If we sent the entire pipeline to a single shard, store the remote cursor and return.
    if (!shardDispatchResults.pipelineForTargetedShards->isSplitForShards()) {
        invariant(shardDispatchResults.remoteCursors.size() == 1);
        auto& remoteCursor = shardDispatchResults.remoteCursors.front();
        const auto reply = uassertStatusOK(storePossibleCursor(
            opCtx, namespaces.requestedNss, remoteCursor, expCtx->tailableMode));
        return appendCursorResponseToCommandResult(
            remoteCursor.getShardId().toString(), reply, result);
    }

    // If we reach here, we have a merge pipeline to dispatch.
    return dispatchMergingPipeline(
        expCtx, namespaces, request, cmdObj, litePipe, routingInfo, shardDispatchResults, result);
}

void ClusterAggregate::uassertAllShardsSupportExplain(
    const std::vector<AsyncRequestsSender::Response>& shardResults) {
    for (const auto& result : shardResults) {
        auto status = result.swResponse.getStatus();
        if (status.isOK()) {
            status = getStatusFromCommandResult(result.swResponse.getValue().data);
        }
        uassert(17403,
                str::stream() << "Shard " << result.shardId.toString() << " failed: "
                              << causedBy(status),
                status.isOK());

        uassert(17404,
                str::stream() << "Shard " << result.shardId.toString()
                              << " does not support $explain",
                result.swResponse.getValue().data.hasField("stages"));
    }
}

Status ClusterAggregate::aggPassthrough(OperationContext* opCtx,
                                        const Namespaces& namespaces,
                                        const ShardId& shardId,
                                        BSONObj cmdObj,
                                        const AggregationRequest& aggRequest,
                                        const LiteParsedPipeline& liteParsedPipeline,
                                        BSONObjBuilder* out) {
    // Temporary hack. See comment on declaration for details.
    auto swShard = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId);
    if (!swShard.isOK()) {
        return swShard.getStatus();
    }
    auto shard = std::move(swShard.getValue());

    // aggPassthrough is for unsharded collections since changing primary shardId will cause SSV
    // error and hence shardId history does not need to be verified.
    auto atClusterTime = computeAtClusterTimeForOneShard(opCtx, shardId);

    // Format the command for the shard. This adds the 'fromMongos' field, wraps the command as an
    // explain if necessary, and rewrites the result into a format safe to forward to shards.
    cmdObj = CommandHelpers::filterCommandRequestForPassthrough(
        createCommandForTargetedShards(opCtx, aggRequest, cmdObj, nullptr, atClusterTime));

    auto cmdResponse = uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting::get(opCtx),
        namespaces.executionNss.db().toString(),
        !shard->isConfig() ? appendShardVersion(std::move(cmdObj), ChunkVersion::UNSHARDED())
                           : std::move(cmdObj),
        Shard::RetryPolicy::kIdempotent));

    if (ErrorCodes::isStaleShardVersionError(cmdResponse.commandStatus.code())) {
        uassertStatusOK(
            cmdResponse.commandStatus.withContext("command failed because of stale config"));
    } else if (ErrorCodes::isSnapshotError(cmdResponse.commandStatus.code())) {
        uassertStatusOK(cmdResponse.commandStatus.withContext(
            "command failed because can not establish a snapshot"));
    }

    BSONObj result;
    if (aggRequest.getExplain()) {
        // If this was an explain, then we get back an explain result object rather than a cursor.
        result = cmdResponse.response;
    } else {
        auto tailMode = liteParsedPipeline.hasChangeStream()
            ? TailableModeEnum::kTailableAndAwaitData
            : TailableModeEnum::kNormal;
        result = uassertStatusOK(storePossibleCursor(
            opCtx, namespaces.requestedNss, shard->getId(), cmdResponse, tailMode));
    }

    // First append the properly constructed writeConcernError. It will then be skipped
    // in appendElementsUnique.
    if (auto wcErrorElem = result["writeConcernError"]) {
        appendWriteConcernErrorToCmdResponse(shard->getId(), wcErrorElem, *out);
    }

    out->appendElementsUnique(CommandHelpers::filterCommandReplyForPassthrough(result));

    auto status = getStatusFromCommandResult(out->asTempObj());
    if (auto resolvedView = status.extraInfo<ResolvedView>()) {
        auto resolvedAggRequest = resolvedView->asExpandedViewAggregation(aggRequest);
        auto resolvedAggCmd = resolvedAggRequest.serializeToCommandObj().toBson();
        out->resetToEmpty();

        // We pass both the underlying collection namespace and the view namespace here. The
        // underlying collection namespace is used to execute the aggregation on mongoD. Any cursor
        // returned will be registered under the view namespace so that subsequent getMore and
        // killCursors calls against the view have access.
        Namespaces nsStruct;
        nsStruct.requestedNss = namespaces.requestedNss;
        nsStruct.executionNss = resolvedView->getNamespace();

        return ClusterAggregate::runAggregate(
            opCtx, nsStruct, resolvedAggRequest, resolvedAggCmd, out);
    }

    return status;
}

}  // namespace mongo
