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

#include "mongo/s/query/cluster_aggregation_planner.h"

#include <absl/container/node_hash_set.h>
#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/smart_ptr.hpp>
#include <cstddef>
#include <list>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/curop.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/change_stream_invalidation_info.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/shard_id.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/random.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/index_version.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/query/async_results_merger_params_gen.h"
#include "mongo/s/query/cluster_client_cursor.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_query_knobs_gen.h"
#include "mongo/s/query/cluster_query_result.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/query/owned_remote_cursor.h"
#include "mongo/s/query/router_exec_stage.h"
#include "mongo/s/query/router_stage_limit.h"
#include "mongo/s/query/router_stage_pipeline.h"
#include "mongo/s/query/router_stage_remove_metadata_fields.h"
#include "mongo/s/query/router_stage_skip.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/sharding_index_catalog_cache.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace cluster_aggregation_planner {

MONGO_FAIL_POINT_DEFINE(shardedAggregateFailToDispatchExchangeConsumerPipeline);
MONGO_FAIL_POINT_DEFINE(shardedAggregateFailToEstablishMergingShardCursor);
MONGO_FAIL_POINT_DEFINE(shardedAggregateHangBeforeDispatchMergingPipeline);

using sharded_agg_helpers::DispatchShardPipelineResults;
using sharded_agg_helpers::PipelineDataSource;
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
        nss.dbName(),
        {{mergingShardId, mergeCmdObj}},
        ReadPreferenceSetting::get(opCtx),
        sharded_agg_helpers::getDesiredRetryPolicy(opCtx));
    auto response = ars.next();
    tassert(6273807,
            "requested and received data from just one shard, but results are still pending",
            ars.done());
    return response;
}

ShardId pickMergingShard(OperationContext* opCtx,
                         const boost::optional<ShardId>& pipelineMergeShardId,
                         const std::vector<ShardId>& targetedShards) {
    // If we cannot merge on mongoS, establish the merge cursor on a shard. Perform the merging
    // command on random shard, unless the pipeline dictates that it needs to be run on a specific
    // shard for the database.
    return pipelineMergeShardId
        ? *pipelineMergeShardId
        : targetedShards[opCtx->getClient()->getPrng().nextInt32(targetedShards.size())];
}

BSONObj createCommandForMergingShard(Document serializedCommand,
                                     const boost::intrusive_ptr<ExpressionContext>& mergeCtx,
                                     const ShardId& shardId,
                                     const boost::optional<ShardingIndexesCatalogCache> sii,
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

    // Request the merging shard to gossip back the routing metadata versions for the collections
    // involved in the decision of the merging shard. For the merging part of the pipeline, only the
    // first stage that involves secondary collections can have effect on the merging decision, so
    // just request gossiping for these.
    if (feature_flags::gShardedAggregationCatalogCacheGossiping.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        stdx::unordered_set<NamespaceString> collectionsInvolvedInMergingShardChoice;
        for (const auto& source : pipelineForMerging->getSources()) {
            source->addInvolvedCollections(&collectionsInvolvedInMergingShardChoice);
            if (!collectionsInvolvedInMergingShardChoice.empty()) {
                // Only consider the first stage that involves secondary collections.
                break;
            }
        }

        BSONArrayBuilder arrayBuilder;
        for (const auto& nss : collectionsInvolvedInMergingShardChoice) {
            arrayBuilder.append(
                NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
        }

        if (arrayBuilder.arrSize() > 0) {
            mergeCmd[Generic_args_unstable_v1::kRequestGossipRoutingCacheFieldName] =
                Value(arrayBuilder.arr());
        }
    }

    // Attach the IGNORED chunk version to the command. On the shard, this will skip the actual
    // version check but will nonetheless mark the operation as versioned.
    auto mergeCmdObj = appendShardVersion(
        mergeCmd.freeze().toBson(),
        ShardVersionFactory::make(ChunkVersion::IGNORED(),
                                  sii ? boost::make_optional(sii->getCollectionIndexes())
                                      : boost::none));

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
                               const boost::optional<CollectionRoutingInfo>& cri,
                               DispatchShardPipelineResults&& shardDispatchResults,
                               BSONObjBuilder* result,
                               const PrivilegeVector& privileges,
                               bool hasChangeStream) {
    // We should never be in a situation where we call this function on a non-merge pipeline.
    tassert(6525900,
            "tried to dispatch merge pipeline but the pipeline was not split",
            shardDispatchResults.splitPipeline);
    auto* mergePipeline = shardDispatchResults.splitPipeline->mergePipeline.get();
    tassert(6525901,
            "tried to dispatch merge pipeline but there was no merge portion of the split pipeline",
            mergePipeline);
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
        (!internalQueryProhibitMergingOnMongoS.load() && mergePipeline->canRunOnMongos().isOK() &&
         !shardDispatchResults.mergeShardId)) {
        return runPipelineOnMongoS(namespaces,
                                   batchSize,
                                   std::move(shardDispatchResults.splitPipeline->mergePipeline),
                                   result,
                                   privileges);
    }

    // If we are not merging on mongoS, then this is not a $changeStream aggregation, and we
    // therefore must have a valid routing table.
    invariant(cri);

    const ShardId mergingShardId =
        pickMergingShard(opCtx, shardDispatchResults.mergeShardId, targetedShards);
    const bool mergingShardContributesData =
        std::find(targetedShards.begin(), targetedShards.end(), mergingShardId) !=
        targetedShards.end();

    auto mergeCmdObj = createCommandForMergingShard(serializedCommand,
                                                    expCtx,
                                                    mergingShardId,
                                                    cri->sii,
                                                    mergingShardContributesData,
                                                    mergePipeline);

    LOGV2_DEBUG(22835,
                1,
                "Dispatching merge pipeline to designated shard",
                "command"_attr = redact(mergeCmdObj),
                "mergingShardId"_attr = mergingShardId);

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

    // If the mergingShard returned an error and did not accept ownership it is our responsibility
    // to kill the cursors.
    uassertStatusOK(getStatusFromCommandResult(mergeResponse.swResponse.getValue().data));

    // If we didn't get an error from the merging shard, ownership for the shard cursors has been
    // transferred to the merging shard. Dismiss the ownership in the current merging pipeline such
    // that when it goes out of scope it does not attempt to kill the cursors.
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
                                     repl::ReadConcernArgs::get(opCtx),
                                     [&] {
                                         if (!opCtx->getLogicalSessionId())
                                             return OperationSessionInfoFromClient();

                                         OperationSessionInfoFromClient osi{
                                             *opCtx->getLogicalSessionId(), opCtx->getTxnNumber()};
                                         if (TransactionRouter::get(opCtx)) {
                                             osi.setAutocommit(false);
                                         }
                                         return osi;
                                     }());

    params.originatingCommandObj = CurOp::get(opCtx)->opDescription().getOwned();
    params.tailableMode = pipelineForMerging->getContext()->tailableMode;
    // A batch size of 0 is legal for the initial aggregate, but not valid for getMores, the batch
    // size we pass here is used for getMores, so do not specify a batch size if the initial request
    // had a batch size of 0.
    params.batchSize = batchSize == 0 ? boost::none : boost::make_optional(batchSize);
    params.originatingPrivileges = privileges;

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

    bool exhausted = cursorState != ClusterCursorManager::CursorState::NotExhausted;
    int nShards = ccc->getNumRemotes();

    auto&& opDebug = CurOp::get(opCtx)->debug();
    // Fill out the aggregation metrics in CurOp, and record queryStats metrics, before detaching
    // the cursor from its opCtx.
    opDebug.nShards = std::max(opDebug.nShards, nShards);
    opDebug.cursorExhausted = exhausted;
    opDebug.additiveMetrics.nBatches = 1;
    CurOp::get(opCtx)->setEndOfOpMetrics(responseBuilder.numDocs());
    if (exhausted) {
        collectQueryStatsMongos(opCtx, ccc->getKey());
    } else {
        collectQueryStatsMongos(opCtx, ccc);
    }

    ccc->detachFromOperationContext();

    CursorId clusterCursorId = 0;
    if (!exhausted) {
        auto authUser = AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName();
        clusterCursorId = uassertStatusOK(Grid::get(opCtx)->getCursorManager()->registerCursor(
            opCtx,
            ccc.releaseCursor(),
            requestedNss,
            ClusterCursorManager::CursorType::MultiTarget,
            ClusterCursorManager::CursorLifetime::Mortal,
            authUser));
        opDebug.cursorid = clusterCursorId;
    }

    responseBuilder.done(clusterCursorId, requestedNss);

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
    tassert(7163600,
            "dispatchExchangeConsumerPipeline() must not be called for explain operation",
            !expCtx->explain);
    auto opCtx = expCtx->opCtx;

    if (MONGO_unlikely(shardedAggregateFailToDispatchExchangeConsumerPipeline.shouldFail())) {
        LOGV2(22836, "shardedAggregateFailToDispatchExchangeConsumerPipeline fail point enabled");
        uasserted(ErrorCodes::FailPointEnabled,
                  "Asserting on exhange consumer pipeline dispatch due to failpoint.");
    }

    // For all consumers construct a request with appropriate cursor ids and send to shards.
    std::vector<AsyncRequestsSender::Request> requests;
    std::vector<SplitPipeline> consumerPipelines;
    auto numConsumers = shardDispatchResults->exchangeSpec->consumerShards.size();
    requests.reserve(numConsumers);
    consumerPipelines.reserve(numConsumers);
    for (size_t idx = 0; idx < numConsumers; ++idx) {
        // Pick this consumer's cursors from producers.
        std::vector<OwnedRemoteCursor> producers;
        producers.reserve(shardDispatchResults->numProducers);
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
                                                                false /* needsMerge */,
                                                                boost::none /* explain */);

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
    ownedCursors.reserve(cursors.size());
    for (auto&& cursor : cursors) {
        ownedCursors.emplace_back(OwnedRemoteCursor(opCtx, std::move(cursor), executionNss));
    }

    // The merging pipeline is just a union of the results from each of the shards involved on the
    // consumer side of the exchange.
    auto mergePipeline = Pipeline::create({}, expCtx);
    mergePipeline->setSplitState(Pipeline::SplitState::kSplitForMerge);

    SplitPipeline splitPipeline{nullptr, std::move(mergePipeline), boost::none};

    // Relinquish ownership of the consumer pipelines' cursors. These cursors are now set up to be
    // merged by a set of $mergeCursors pipelines that we just dispatched to the shards above. Now
    // that we've established those pipelines on the shards, we are no longer responsible for
    // ensuring they are cleaned up. If there was a problem establishing the cursors then
    // establishCursors() would have thrown and mongos would kill all the consumer cursors itself.
    for (const auto& pipeline : consumerPipelines) {
        const auto& mergeCursors =
            static_cast<DocumentSourceMergeCursors*>(pipeline.shardsPipeline->peekFront());
        mergeCursors->dismissCursorOwnership();
    }
    return DispatchShardPipelineResults{boost::none /* mergeShardId  */,
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
 * Contacts the primary shard for the collection default collation.
 */
BSONObj getUntrackedCollectionCollation(OperationContext* opCtx,
                                        const ShardId& shardId,
                                        const NamespaceString& nss) {
    auto shard = uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));
    ScopedDbConnection conn(shard->getConnString());
    std::list<BSONObj> all = conn->getCollectionInfos(nss.dbName(), BSON("name" << nss.coll()));

    // Collection or collection info does not exist; return an empty collation object.
    if (all.empty() || all.front().isEmpty()) {
        return BSONObj();
    }

    auto collectionInfo = all.front();

    // We inspect 'info' to infer the collection default collation.
    BSONObj collationToReturn = CollationSpec::kSimpleSpec;
    if (collectionInfo["options"].type() == BSONType::Object) {
        BSONObj collectionOptions = collectionInfo["options"].Obj();
        BSONElement collationElement;
        auto status = bsonExtractTypedField(
            collectionOptions, "collation", BSONType::Object, &collationElement);
        if (status.isOK()) {
            collationToReturn = collationElement.Obj().getOwned();
            uassert(ErrorCodes::BadValue,
                    "Default collation in collection metadata cannot be empty.",
                    !collationToReturn.isEmpty());
        } else if (status != ErrorCodes::NoSuchKey) {
            uassertStatusOK(status);
        }
    }
    return collationToReturn;
}

bool isMergeSkipOrLimit(const boost::intrusive_ptr<DocumentSource>& stage) {
    return (dynamic_cast<DocumentSourceLimit*>(stage.get()) ||
            dynamic_cast<DocumentSourceMergeCursors*>(stage.get()) ||
            dynamic_cast<DocumentSourceSkip*>(stage.get()));
}

bool isAllLimitsAndSkips(Pipeline* pipeline) {
    const auto& stages = pipeline->getSources();
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
    const std::function<std::unique_ptr<Pipeline, PipelineDeleter>()> buildPipelineFn,
    boost::optional<CollectionRoutingInfo> cri,
    PipelineDataSource pipelineDataSource,
    bool perShardCursor) {
    if (perShardCursor) {
        return {TargetingPolicy::kSpecificShardOnly, nullptr /* pipeline */, cri};
    }

    tassert(7972401,
            "Aggregation did not have a routing table and does not feature either a $changeStream "
            "or a $documents stage",
            cri || pipelineDataSource == PipelineDataSource::kChangeStream ||
                pipelineDataSource == PipelineDataSource::kQueue);
    auto pipeline = buildPipelineFn();
    auto policy = pipeline->requiredToRunOnMongos() ? TargetingPolicy::kMongosRequired
                                                    : TargetingPolicy::kAnyShard;
    if (!cri && pipelineDataSource == PipelineDataSource::kQueue) {
        // If we don't have a routing table and there is a $documents stage, we must run on
        // mongos.
        policy = TargetingPolicy::kMongosRequired;
    }
    return AggregationTargeter{policy, std::move(pipeline), cri};
}

Status runPipelineOnMongoS(const ClusterAggregate::Namespaces& namespaces,
                           long long batchSize,
                           std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                           BSONObjBuilder* result,
                           const PrivilegeVector& privileges) {
    auto expCtx = pipeline->getContext();

    // We should never receive a pipeline which cannot run on mongoS.
    invariant(!expCtx->explain);
    uassertStatusOKWithContext(pipeline->canRunOnMongos(),
                               "pipeline is required to run on mongoS, but cannot");


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
                                PipelineDataSource pipelineDataSource,
                                bool eligibleForSampling) {
    auto expCtx = targeter.pipeline->getContext();
    // If not, split the pipeline as necessary and dispatch to the relevant shards.
    auto shardDispatchResults =
        sharded_agg_helpers::dispatchShardPipeline(serializedCommand,
                                                   pipelineDataSource,
                                                   eligibleForSampling,
                                                   std::move(targeter.pipeline),
                                                   expCtx->explain,
                                                   targeter.cri);

    // Check for valid usage of SEARCH_META. We wait until after we've dispatched pipelines to the
    // shards in the event that we need to resolve any views.
    // TODO PM-1966: We can resume doing this at parse time once views are tracked in the catalog.
    auto svcCtx = opCtx->getServiceContext();
    if (svcCtx) {
        if (shardDispatchResults.pipelineForSingleShard) {
            search_helpers::assertSearchMetaAccessValid(
                shardDispatchResults.pipelineForSingleShard->getSources(), expCtx.get());
        } else {
            tassert(7972499,
                    "Must have split pipeline if 'pipelineForSingleShard' not present",
                    shardDispatchResults.splitPipeline);
            search_helpers::assertSearchMetaAccessValid(
                shardDispatchResults.splitPipeline->shardsPipeline->getSources(),
                shardDispatchResults.splitPipeline->mergePipeline->getSources(),
                expCtx.get());
        }
    }

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

    shardedAggregateHangBeforeDispatchMergingPipeline.pauseWhileSet();

    // If we reach here, we have a merge pipeline to dispatch.
    return dispatchMergingPipeline(expCtx,
                                   namespaces,
                                   serializedCommand,
                                   batchSize,
                                   targeter.cri,
                                   std::move(shardDispatchResults),
                                   result,
                                   privileges,
                                   pipelineDataSource == PipelineDataSource::kChangeStream);
}

BSONObj getCollation(OperationContext* opCtx,
                     const boost::optional<ChunkManager>& cm,
                     const NamespaceString& nss,
                     const BSONObj& collation,
                     bool requiresCollationForParsingUnshardedAggregate) {
    // If this is a collectionless aggregation or if the user specified an explicit collation,
    // we immediately return the user-defined collation if one exists, or an empty BSONObj
    // otherwise.
    if (nss.isCollectionlessAggregateNS() || !collation.isEmpty() || !cm) {
        return collation;
    }

    // If the target collection is untracked, we will contact the primary shard to discover this
    // information if it is necessary for pipeline parsing. Otherwise, we infer the collation once
    // the command is executed on the primary shard.
    if (!cm->hasRoutingTable()) {
        return requiresCollationForParsingUnshardedAggregate
            ? getUntrackedCollectionCollation(opCtx, cm->dbPrimary(), nss)
            : BSONObj();
    }

    // Return the default collator if one exists, otherwise return the simple collation.
    if (auto defaultCollator = cm->getDefaultCollator()) {
        return defaultCollator->getSpec().toBSON();
    }

    return CollationSpec::kSimpleSpec;
}

Status runPipelineOnSpecificShardOnly(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const ClusterAggregate::Namespaces& namespaces,
                                      boost::optional<ExplainOptions::Verbosity> explain,
                                      Document serializedCommand,
                                      const PrivilegeVector& privileges,
                                      ShardId shardId,
                                      bool eligibleForSampling,
                                      BSONObjBuilder* out) {
    auto opCtx = expCtx->opCtx;

    tassert(6273804,
            "Per shard cursors are supposed to pass fromMongos: false to shards",
            !expCtx->inMongos);
    // By using an initial batchSize of zero all of the events will get returned through
    // the getMore path and have metadata stripped out.
    boost::optional<int> overrideBatchSize = 0;

    // Format the command for the shard. This wraps the command as an explain if necessary, and
    // rewrites the result into a format safe to forward to shards.
    BSONObj cmdObj = sharded_agg_helpers::createPassthroughCommandForShard(
        expCtx, serializedCommand, explain, nullptr /* pipeline */, boost::none, overrideBatchSize);

    if (eligibleForSampling) {
        if (auto sampleId = analyze_shard_key::tryGenerateSampleId(
                opCtx,
                namespaces.executionNss,
                analyze_shard_key::SampledCommandNameEnum::kAggregate)) {
            cmdObj = analyze_shard_key::appendSampleId(std::move(cmdObj), std::move(*sampleId));
        }
    }

    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        namespaces.executionNss.dbName(),
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
            boost::optional<BSONObj>(change_stream_constants::kSortSpec) /* routerSort */));
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
