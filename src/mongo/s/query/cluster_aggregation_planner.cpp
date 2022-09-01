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

#include "mongo/platform/basic.h"

#include "mongo/s/query/cluster_aggregation_planner.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connpool.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/change_stream_invalidation_info.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/shard_id.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_query_knobs_gen.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/s/query/router_stage_limit.h"
#include "mongo/s/query/router_stage_pipeline.h"
#include "mongo/s/query/router_stage_remove_metadata_fields.h"
#include "mongo/s/query/router_stage_skip.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/transaction_router.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
namespace cluster_aggregation_planner {

MONGO_FAIL_POINT_DEFINE(shardedAggregateFailToDispatchExchangeConsumerPipeline);
MONGO_FAIL_POINT_DEFINE(shardedAggregateFailToEstablishMergingShardCursor);

using sharded_agg_helpers::DispatchShardPipelineResults;
using sharded_agg_helpers::SplitPipeline;

namespace {

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

AsyncRequestsSender::Response establishMergingShardCursor(OperationContext* opCtx,
                                                          const NamespaceString& nss,
                                                          const BSONObj mergeCmdObj,
                                                          const ShardId& mergingShardId) {
    if (MONGO_unlikely(shardedAggregateFailToEstablishMergingShardCursor.shouldFail())) {
        LOGV2(22834, "shardedAggregateFailToEstablishMergingShardCursor fail point enabled");
        uasserted(ErrorCodes::FailPointEnabled,
                  "Asserting on establishing merging shard cursor due to failpoint.");
    }

    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        nss.db().toString(),
        {{mergingShardId, mergeCmdObj}},
        ReadPreferenceSetting::get(opCtx),
        sharded_agg_helpers::getDesiredRetryPolicy(opCtx));
    const auto response = ars.next();
    tassert(6273807,
            "requested and received data from just one shard, but results are still pending",
            ars.done());
    return response;
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

BSONObj createCommandForMergingShard(Document serializedCommand,
                                     const boost::intrusive_ptr<ExpressionContext>& mergeCtx,
                                     const ShardId& shardId,
                                     bool mergingShardContributesData,
                                     const Pipeline* pipelineForMerging) {
    MutableDocument mergeCmd(serializedCommand);

    mergeCmd["pipeline"] = Value(pipelineForMerging->serialize());
    mergeCmd[AggregateCommandRequest::kFromMongosFieldName] = Value(true);

    mergeCmd[AggregateCommandRequest::kLetFieldName] =
        Value(mergeCtx->variablesParseState.serialize(mergeCtx->variables));

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

    // Attach the IGNORED chunk version to the command. On the shard, this will skip the actual
    // version check but will nonetheless mark the operation as versioned.
    auto mergeCmdObj = appendShardVersion(mergeCmd.freeze().toBson(), ShardVersion::IGNORED());

    // Attach the read and write concerns if needed, and return the final command object.
    return applyReadWriteConcern(mergeCtx->opCtx,
                                 !(txnRouter && mergingShardContributesData), /* appendRC */
                                 !mergeCtx->explain,                          /* appendWC */
                                 mergeCmdObj);
}

Status dispatchMergingPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               const ClusterAggregate::Namespaces& namespaces,
                               Document serializedCommand,
                               long long batchSize,
                               const boost::optional<ChunkManager>& cm,
                               DispatchShardPipelineResults&& shardDispatchResults,
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

    sharded_agg_helpers::partitionAndAddMergeCursorsSource(
        mergePipeline,
        std::move(shardDispatchResults.remoteCursors),
        shardDispatchResults.splitPipeline->shardCursorsSortSpec);

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
    invariant(cm);

    const ShardId mergingShardId = pickMergingShard(
        opCtx, shardDispatchResults.needsPrimaryShardMerge, targetedShards, cm->dbPrimary());
    const bool mergingShardContributesData =
        std::find(targetedShards.begin(), targetedShards.end(), mergingShardId) !=
        targetedShards.end();

    auto mergeCmdObj = createCommandForMergingShard(
        serializedCommand, expCtx, mergingShardId, mergingShardContributesData, mergePipeline);

    LOGV2_DEBUG(22835,
                1,
                "Dispatching merge pipeline to designated shard",
                "command"_attr = redact(mergeCmdObj));

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

    ClusterClientCursorParams params(requestedNss,
                                     APIParameters::get(opCtx),
                                     ReadPreferenceSetting::get(opCtx),
                                     repl::ReadConcernArgs::get(opCtx));

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
    if (!opCtx->inMultiDocumentTransaction()) {
        options.atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
    }
    CursorResponseBuilder responseBuilder(&replyBuilder, options);
    bool stashedResult = false;

    for (long long objCount = 0; objCount < batchSize; ++objCount) {
        ClusterQueryResult next;
        try {
            next = uassertStatusOK(ccc->next());
        } catch (const ExceptionFor<ErrorCodes::CloseChangeStream>&) {
            // This exception is thrown when a $changeStream stage encounters an event
            // that invalidates the cursor. We should close the cursor and return without
            // error.
            cursorState = ClusterCursorManager::CursorState::Exhausted;
            break;
        } catch (const ExceptionFor<ErrorCodes::ChangeStreamInvalidated>& ex) {
            // This exception is thrown when a change-stream cursor is invalidated. Set the PBRT
            // to the resume token of the invalidating event, and mark the cursor response as
            // invalidated. We always expect to have ExtraInfo for this error code.
            const auto extraInfo = ex.extraInfo<ChangeStreamInvalidationInfo>();
            tassert(5493706, "Missing ChangeStreamInvalidationInfo on exception", extraInfo);

            responseBuilder.setPostBatchResumeToken(extraInfo->getInvalidateResumeToken());
            responseBuilder.setInvalidated();
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
        auto authUser = AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName();
        clusterCursorId = uassertStatusOK(Grid::get(opCtx)->getCursorManager()->registerCursor(
            opCtx,
            ccc.releaseCursor(),
            requestedNss,
            ClusterCursorManager::CursorType::MultiTarget,
            ClusterCursorManager::CursorLifetime::Mortal,
            authUser));
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

DispatchShardPipelineResults dispatchExchangeConsumerPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& executionNss,
    Document serializedCommand,
    DispatchShardPipelineResults* shardDispatchResults) {
    auto opCtx = expCtx->opCtx;

    if (MONGO_unlikely(shardedAggregateFailToDispatchExchangeConsumerPipeline.shouldFail())) {
        LOGV2(22836, "shardedAggregateFailToDispatchExchangeConsumerPipeline fail point enabled");
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
        auto consumerPipeline = Pipeline::create(
            shardDispatchResults->splitPipeline->mergePipeline->getSources(), expCtx);

        sharded_agg_helpers::partitionAndAddMergeCursorsSource(
            consumerPipeline.get(),
            std::move(producers),
            shardDispatchResults->splitPipeline->shardCursorsSortSpec);

        consumerPipelines.emplace_back(std::move(consumerPipeline), nullptr, boost::none);

        auto consumerCmdObj =
            sharded_agg_helpers::createCommandForTargetedShards(expCtx,
                                                                serializedCommand,
                                                                consumerPipelines.back(),
                                                                boost::none, /* exchangeSpec */
                                                                false /* needsMerge */);

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
    auto mergePipeline = Pipeline::create({}, expCtx);
    mergePipeline->setSplitState(Pipeline::SplitState::kSplitForMerge);

    SplitPipeline splitPipeline{nullptr, std::move(mergePipeline), boost::none};

    // Relinquish ownership of the local consumer pipelines' cursors as each shard is now
    // responsible for its own producer cursors.
    for (const auto& pipeline : consumerPipelines) {
        const auto& mergeCursors =
            static_cast<DocumentSourceMergeCursors*>(pipeline.shardsPipeline->peekFront());
        mergeCursors->dismissCursorOwnership();
    }
    return DispatchShardPipelineResults{false,
                                        std::move(ownedCursors),
                                        {},
                                        std::move(splitPipeline),
                                        nullptr,
                                        BSONObj(),
                                        numConsumers};
}

ClusterClientCursorGuard convertPipelineToRouterStages(
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline, ClusterClientCursorParams&& cursorParams) {
    auto* opCtx = pipeline->getContext()->opCtx;

    // We expect the pipeline to be fully executable at this point, so if the pipeline was all skips
    // and limits we expect it to start with a $mergeCursors stage.
    auto mergeCursors =
        checked_cast<DocumentSourceMergeCursors*>(pipeline->getSources().front().get());
    // Replace the pipeline with RouterExecStages.
    std::unique_ptr<RouterExecStage> root = mergeCursors->convertToRouterStage();
    pipeline->popFront();
    while (!pipeline->getSources().empty()) {
        if (auto skip = pipeline->popFrontWithName(DocumentSourceSkip::kStageName)) {
            root = std::make_unique<RouterStageSkip>(
                opCtx, std::move(root), static_cast<DocumentSourceSkip*>(skip.get())->getSkip());
        } else if (auto limit = pipeline->popFrontWithName(DocumentSourceLimit::kStageName)) {
            root = std::make_unique<RouterStageLimit>(
                opCtx, std::move(root), static_cast<DocumentSourceLimit*>(limit.get())->getLimit());
        } else {
            // We previously checked that everything was a $mergeCursors, $skip, or $limit. We
            // already popped off the $mergeCursors, so everything else should be a $skip or a
            // $limit.
            MONGO_UNREACHABLE;
        }
    }
    // We are executing the pipeline without using an actual Pipeline, so we need to strip out any
    // Document metadata ourselves.
    return ClusterClientCursorImpl::make(
        opCtx,
        std::make_unique<RouterStageRemoveMetadataFields>(
            opCtx, std::move(root), Document::allMetadataFieldNames),
        std::move(cursorParams));
}

/**
 * Returns the output of the listCollections command filtered to the namespace 'nss'.
 */
BSONObj getUnshardedCollInfo(OperationContext* opCtx,
                             const ShardId& shardId,
                             const NamespaceString& nss) {
    auto shard = uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));
    ScopedDbConnection conn(shard->getConnString());
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

bool isMergeSkipOrLimit(const boost::intrusive_ptr<DocumentSource>& stage) {
    return (dynamic_cast<DocumentSourceLimit*>(stage.get()) ||
            dynamic_cast<DocumentSourceMergeCursors*>(stage.get()) ||
            dynamic_cast<DocumentSourceSkip*>(stage.get()));
}

bool isAllLimitsAndSkips(Pipeline* pipeline) {
    const auto stages = pipeline->getSources();
    return std::all_of(
        stages.begin(), stages.end(), [](const auto& stage) { return isMergeSkipOrLimit(stage); });
}

}  // namespace

ClusterClientCursorGuard buildClusterCursor(OperationContext* opCtx,
                                            std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                                            ClusterClientCursorParams&& cursorParams) {
    if (isAllLimitsAndSkips(pipeline.get())) {
        // We can optimize this Pipeline to avoid going through any DocumentSources at all and thus
        // skip the expensive BSON->Document->BSON conversion.
        return convertPipelineToRouterStages(std::move(pipeline), std::move(cursorParams));
    }
    return ClusterClientCursorImpl::make(
        opCtx, std::make_unique<RouterStagePipeline>(std::move(pipeline)), std::move(cursorParams));
}

AggregationTargeter AggregationTargeter::make(
    OperationContext* opCtx,
    const NamespaceString& executionNss,
    const std::function<std::unique_ptr<Pipeline, PipelineDeleter>()> buildPipelineFn,
    boost::optional<ChunkManager> cm,
    stdx::unordered_set<NamespaceString> involvedNamespaces,
    bool hasChangeStream,
    bool allowedToPassthrough,
    bool perShardCursor) {
    if (perShardCursor) {
        return {TargetingPolicy::kSpecificShardOnly, nullptr, cm};
    }

    // Check if any of the involved collections are sharded.
    bool involvesShardedCollections = [&]() {
        for (const auto& nss : involvedNamespaces) {
            const auto resolvedNsCM =
                uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, nss));
            if (resolvedNsCM.isSharded()) {
                return true;
            }
        }
        return false;
    }();

    // Determine whether this aggregation must be dispatched to all shards in the cluster.
    const bool mustRunOnAll =
        sharded_agg_helpers::mustRunOnAllShards(executionNss, hasChangeStream);

    // If we don't have a routing table, then this is a $changeStream which must run on all shards.
    invariant(cm || (mustRunOnAll && hasChangeStream));

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
    if (cm && !cm->isSharded() && !mustRunOnAll && allowedToPassthrough &&
        !involvesShardedCollections) {
        return AggregationTargeter{TargetingPolicy::kPassthrough, nullptr, cm};
    } else {
        auto pipeline = buildPipelineFn();
        auto policy = pipeline->requiredToRunOnMongos() ? TargetingPolicy::kMongosRequired
                                                        : TargetingPolicy::kAnyShard;
        return AggregationTargeter{policy, std::move(pipeline), cm};
    }
}

Status runPipelineOnPrimaryShard(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 const ClusterAggregate::Namespaces& namespaces,
                                 const ChunkManager& cm,
                                 boost::optional<ExplainOptions::Verbosity> explain,
                                 Document serializedCommand,
                                 const PrivilegeVector& privileges,
                                 BSONObjBuilder* out) {
    return runPipelineOnSpecificShardOnly(expCtx,
                                          namespaces,
                                          boost::optional<DatabaseVersion>(cm.dbVersion()),
                                          explain,
                                          serializedCommand,
                                          privileges,
                                          cm.dbPrimary(),
                                          false,
                                          out);
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
    auto shardDispatchResults = sharded_agg_helpers::dispatchShardPipeline(
        serializedCommand, hasChangeStream, std::move(targeter.pipeline));

    // If the operation is an explain, then we verify that it succeeded on all targeted
    // shards, write the results to the output builder, and return immediately.
    if (expCtx->explain) {
        return sharded_agg_helpers::appendExplainResults(
            std::move(shardDispatchResults), expCtx, result);
    }

    // If this isn't an explain, then we must have established cursors on at least one
    // shard.
    invariant(shardDispatchResults.remoteCursors.size() > 0);

    // If we sent the entire pipeline to a single shard, store the remote cursor and return.
    if (!shardDispatchResults.splitPipeline) {
        tassert(4457012,
                "pipeline was split, but more than one remote cursor is present",
                shardDispatchResults.remoteCursors.size() == 1);
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
                                   targeter.cm,
                                   std::move(shardDispatchResults),
                                   result,
                                   privileges,
                                   hasChangeStream);
}

std::pair<BSONObj, boost::optional<UUID>> getCollationAndUUID(
    OperationContext* opCtx,
    const boost::optional<ChunkManager>& cm,
    const NamespaceString& nss,
    const BSONObj& collation) {
    const bool collectionIsSharded = (cm && cm->isSharded());
    const bool collectionIsNotSharded = (cm && !cm->isSharded());

    // If this is a collectionless aggregation, we immediately return the user-
    // defined collation if one exists, or an empty BSONObj otherwise. Collectionless aggregations
    // generally run on the 'admin' database, the standard logic would attempt to resolve its
    // non-existent UUID and collation by sending a specious 'listCollections' command to the config
    // servers.
    if (nss.isCollectionlessAggregateNS()) {
        return {collation, boost::none};
    }

    // If the collection is unsharded, obtain collInfo from the primary shard.
    const auto unshardedCollInfo =
        collectionIsNotSharded ? getUnshardedCollInfo(opCtx, cm->dbPrimary(), nss) : BSONObj();

    // Return the collection UUID if available, or boost::none otherwise.
    const auto getUUID = [&]() -> boost::optional<UUID> {
        if (collectionIsSharded) {
            return cm->getUUID();
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
            return cm->getDefaultCollator() ? cm->getDefaultCollator()->getSpec().toBSON()
                                            : CollationSpec::kSimpleSpec;
        }
    };

    // If the user specified an explicit collation, we always adopt it. Otherwise,
    // obtain the collection default or simple collation as appropriate, and return
    // it along with the collection's UUID.
    return {collation.isEmpty() ? getCollation() : collation, getUUID()};
}

Status runPipelineOnSpecificShardOnly(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const ClusterAggregate::Namespaces& namespaces,
                                      boost::optional<DatabaseVersion> dbVersion,
                                      boost::optional<ExplainOptions::Verbosity> explain,
                                      Document serializedCommand,
                                      const PrivilegeVector& privileges,
                                      ShardId shardId,
                                      bool forPerShardCursor,
                                      BSONObjBuilder* out) {
    auto opCtx = expCtx->opCtx;

    boost::optional<int> overrideBatchSize;
    if (forPerShardCursor) {
        tassert(6273804,
                "Per shard cursors are supposed to pass fromMongos: false to shards",
                !expCtx->inMongos);
        // By using an initial batchSize of zero all of the events will get returned through
        // the getMore path and have metadata stripped out.
        overrideBatchSize = 0;
    }

    // Format the command for the shard. This wraps the command as an explain if necessary, and
    // rewrites the result into a format safe to forward to shards.
    BSONObj cmdObj = sharded_agg_helpers::createPassthroughCommandForShard(expCtx,
                                                                           serializedCommand,
                                                                           explain,
                                                                           nullptr, /* pipeline */
                                                                           BSONObj(),
                                                                           boost::none,
                                                                           overrideBatchSize);

    if (!forPerShardCursor && shardId != ShardId::kConfigServerId) {
        cmdObj = appendShardVersion(std::move(cmdObj), ShardVersion::UNSHARDED());
    }
    if (!forPerShardCursor) {
        // Unless this is a per shard cursor, we need to send shard version info.
        tassert(6377400, "Missing shard versioning information", dbVersion.has_value());
        cmdObj = appendDbVersionIfPresent(std::move(cmdObj), *dbVersion);
    }

    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        namespaces.executionNss.db().toString(),
        {{shardId, cmdObj}},
        ReadPreferenceSetting::get(opCtx),
        Shard::RetryPolicy::kIdempotent);
    auto response = ars.next();
    tassert(6273806,
            "requested and received data from just one shard, but results are still pending",
            ars.done());

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
        result = uassertStatusOK(storePossibleCursor(
            opCtx,
            shardId,
            *response.shardHostAndPort,
            response.swResponse.getValue().data,
            namespaces.requestedNss,
            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
            Grid::get(opCtx)->getCursorManager(),
            privileges,
            expCtx->tailableMode,
            forPerShardCursor ? boost::optional<BSONObj>(change_stream_constants::kSortSpec)
                              : boost::none));
    }

    // First append the properly constructed writeConcernError. It will then be skipped
    // in appendElementsUnique.
    if (auto wcErrorElem = result["writeConcernError"]) {
        appendWriteConcernErrorToCmdResponse(shardId, wcErrorElem, *out);
    }

    out->appendElementsUnique(CommandHelpers::filterCommandReplyForPassthrough(result));

    return getStatusFromCommandResult(out->asTempObj());
}

}  // namespace cluster_aggregation_planner
}  // namespace mongo
