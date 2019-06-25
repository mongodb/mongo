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

#include "mongo/s/query/cluster_aggregate.h"

#include <boost/intrusive_ptr.hpp>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/mongos_process_interface.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/db/views/view.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/query/cluster_aggregation_planner.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_query_knobs_gen.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/query/owned_remote_cursor.h"
#include "mongo/s/query/router_stage_pipeline.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_utils.h"

namespace mongo {

using SplitPipeline = cluster_aggregation_planner::SplitPipeline;

MONGO_FAIL_POINT_DEFINE(clusterAggregateFailToEstablishMergingShardCursor);
MONGO_FAIL_POINT_DEFINE(clusterAggregateFailToDispatchExchangeConsumerPipeline);

constexpr unsigned ClusterAggregate::kMaxViewRetries;

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

BSONObj createCommandForMergingShard(const AggregationRequest& request,
                                     const boost::intrusive_ptr<ExpressionContext>& mergeCtx,
                                     const ShardId& shardId,
                                     bool mergingShardContributesData,
                                     const Pipeline* pipelineForMerging) {
    MutableDocument mergeCmd(request.serializeToCommandObj());

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

    // agg creates temp collection and should handle implicit create separately.
    return appendAllowImplicitCreate(mergeCmd.freeze().toBson(), true);
}

sharded_agg_helpers::DispatchShardPipelineResults dispatchExchangeConsumerPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& executionNss,
    const AggregationRequest& request,
    const LiteParsedPipeline& litePipe,
    BSONObj collationObj,
    sharded_agg_helpers::DispatchShardPipelineResults* shardDispatchResults) {
    invariant(!litePipe.hasChangeStream());
    auto opCtx = expCtx->opCtx;

    if (MONGO_FAIL_POINT(clusterAggregateFailToDispatchExchangeConsumerPipeline)) {
        log() << "clusterAggregateFailToDispatchExchangeConsumerPipeline fail point enabled.";
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
            litePipe,
            BSONObj(),
            std::move(producers),
            {},
            shardDispatchResults->splitPipeline->shardCursorsSortSpec,
            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor());

        consumerPipelines.emplace_back(std::move(consumerPipeline), nullptr, boost::none);

        auto consumerCmdObj =
            sharded_agg_helpers::createCommandForTargetedShards(opCtx,
                                                                request,
                                                                litePipe,
                                                                consumerPipelines.back(),
                                                                collationObj,
                                                                boost::none,
                                                                expCtx->getRuntimeConstants(),
                                                                false);

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
                                  << ": "
                                  << data.toString(),
                    queryPlannerElement);
            explain << "queryPlanner" << queryPlannerElement;
            if (auto executionStatsElement = data["executionStats"]) {
                explain << "executionStats" << executionStatsElement;
            }
        }
    }

    return Status::OK();
}

AsyncRequestsSender::Response establishMergingShardCursor(OperationContext* opCtx,
                                                          const NamespaceString& nss,
                                                          const AggregationRequest& request,
                                                          const BSONObj mergeCmdObj,
                                                          const ShardId& mergingShardId) {
    if (MONGO_FAIL_POINT(clusterAggregateFailToEstablishMergingShardCursor)) {
        log() << "clusterAggregateFailToEstablishMergingShardCursor fail point enabled.";
        uasserted(ErrorCodes::FailPointEnabled,
                  "Asserting on establishing merging shard cursor due to failpoint.");
    }

    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        nss.db().toString(),
        {{mergingShardId, mergeCmdObj}},
        ReadPreferenceSetting::get(opCtx),
        sharded_agg_helpers::getDesiredRetryPolicy(request));
    const auto response = ars.next();
    invariant(ars.done());
    return response;
}

BSONObj establishMergingMongosCursor(OperationContext* opCtx,
                                     const AggregationRequest& request,
                                     const NamespaceString& requestedNss,
                                     const LiteParsedPipeline& liteParsedPipeline,
                                     std::unique_ptr<Pipeline, PipelineDeleter> pipelineForMerging,
                                     const PrivilegeVector& privileges) {

    ClusterClientCursorParams params(requestedNss, ReadPreferenceSetting::get(opCtx));

    params.originatingCommandObj = CurOp::get(opCtx)->opDescription().getOwned();
    params.tailableMode = pipelineForMerging->getContext()->tailableMode;
    // A batch size of 0 is legal for the initial aggregate, but not valid for getMores, the batch
    // size we pass here is used for getMores, so do not specify a batch size if the initial request
    // had a batch size of 0.
    params.batchSize = request.getBatchSize() == 0
        ? boost::none
        : boost::optional<long long>(request.getBatchSize());
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

/**
 *  Populates the "collation" and "uuid" parameters with the following semantics:
 *  - The "collation" parameter will be set to the default collation for the collection or the
 *    simple collation if there is no default. If the collection does not exist or if the aggregate
 *    is on the collectionless namespace, this will be set to an empty object.
 *  - The "uuid" is retrieved from the chunk manager for sharded collections or the listCollections
 *    output for unsharded collections. The UUID will remain unset if the aggregate is on the
 *    collectionless namespace.
 */
std::pair<BSONObj, boost::optional<UUID>> getCollationAndUUID(
    const boost::optional<CachedCollectionRoutingInfo>& routingInfo,
    const NamespaceString& nss,
    const AggregationRequest& request,
    const LiteParsedPipeline& litePipe) {
    const bool collectionIsSharded = (routingInfo && routingInfo->cm());
    const bool collectionIsNotSharded = (routingInfo && !routingInfo->cm());

    // If this is a change stream or a collectionless aggregation, we immediately return the user-
    // defined collation if one exists, or an empty BSONObj otherwise. Change streams never inherit
    // the collection's default collation, and since collectionless aggregations generally run on
    // the 'admin' database, the standard logic would attempt to resolve its non-existent UUID and
    // collation by sending a specious 'listCollections' command to the config servers.
    if (litePipe.hasChangeStream() || nss.isCollectionlessAggregateNS()) {
        return {request.getCollation(), boost::none};
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
    return {request.getCollation().isEmpty() ? getCollation() : request.getCollation(), getUUID()};
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

// "Resolve" involved namespaces into a map. We won't try to execute anything on a mongos, but we
// still have to populate this map so that any $lookups, etc. will be able to have a resolved view
// definition. It's okay that this is incorrect, we will repopulate the real namespace map on the
// mongod. Note that this function must be called before forwarding an aggregation command on an
// unsharded collection, in order to verify that the involved namespaces are allowed to be sharded.
auto resolveInvolvedNamespaces(OperationContext* opCtx, const LiteParsedPipeline& litePipe) {
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    for (auto&& nss : litePipe.getInvolvedNamespaces()) {
        resolvedNamespaces.try_emplace(nss.coll(), nss, std::vector<BSONObj>{});
    }
    return resolvedNamespaces;
}

// Build an appropriate ExpressionContext for the pipeline. This helper instantiates an appropriate
// collator, creates a MongoProcessInterface for use by the pipeline's stages, and optionally
// extracts the UUID from the collection info if present.
boost::intrusive_ptr<ExpressionContext> makeExpressionContext(
    OperationContext* opCtx,
    const AggregationRequest& request,
    const LiteParsedPipeline& litePipe,
    BSONObj collationObj,
    boost::optional<UUID> uuid,
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces) {

    std::unique_ptr<CollatorInterface> collation;
    if (!collationObj.isEmpty()) {
        // This will be null if attempting to build an interface for the simple collator.
        collation = uassertStatusOK(
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collationObj));
    }

    // Create the expression context, and set 'inMongos' to true. We explicitly do *not* set
    // mergeCtx->tempDir.
    auto mergeCtx = new ExpressionContext(opCtx,
                                          request,
                                          std::move(collation),
                                          std::make_shared<MongoSInterface>(),
                                          std::move(resolvedNamespaces),
                                          uuid);

    mergeCtx->inMongos = true;
    return mergeCtx;
}

// Runs a pipeline on mongoS, having first validated that it is eligible to do so. This can be a
// pipeline which is split for merging, or an intact pipeline which must run entirely on mongoS.
Status runPipelineOnMongoS(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                           const ClusterAggregate::Namespaces& namespaces,
                           const AggregationRequest& request,
                           const LiteParsedPipeline& litePipe,
                           std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                           BSONObjBuilder* result,
                           const PrivilegeVector& privileges) {
    // We should never receive a pipeline which cannot run on mongoS.
    invariant(!expCtx->explain);
    invariant(pipeline->canRunOnMongos());

    const auto& requestedNss = namespaces.requestedNss;
    const auto opCtx = expCtx->opCtx;

    // Verify that the first stage can produce input for the remainder of the pipeline.
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Aggregation pipeline must be run on mongoS, but "
                          << pipeline->getSources().front()->getSourceName()
                          << " is not capable of producing input",
            !pipeline->getSources().front()->constraints().requiresInputDocSource);

    // Register the new mongoS cursor, and retrieve the initial batch of results.
    auto cursorResponse = establishMergingMongosCursor(
        opCtx, request, requestedNss, litePipe, std::move(pipeline), privileges);

    // We don't need to storePossibleCursor or propagate writeConcern errors; a pipeline with
    // writing stages like $out can never run on mongoS. Filter the command response and return
    // immediately.
    CommandHelpers::filterCommandReplyForPassthrough(cursorResponse, result);
    return getStatusFromCommandResult(result->asTempObj());
}

Status dispatchMergingPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ClusterAggregate::Namespaces& namespaces,
    const AggregationRequest& request,
    const LiteParsedPipeline& litePipe,
    const boost::optional<CachedCollectionRoutingInfo>& routingInfo,
    sharded_agg_helpers::DispatchShardPipelineResults&& shardDispatchResults,
    BSONObjBuilder* result,
    const PrivilegeVector& privileges) {
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
        litePipe,
        shardDispatchResults.commandForTargetedShards,
        std::move(shardDispatchResults.remoteCursors),
        targetedShards,
        shardDispatchResults.splitPipeline->shardCursorsSortSpec,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor());

    // First, check whether we can merge on the mongoS. If the merge pipeline MUST run on mongoS,
    // then ignore the internalQueryProhibitMergingOnMongoS parameter.
    if (mergePipeline->requiredToRunOnMongos() ||
        (!internalQueryProhibitMergingOnMongoS.load() && mergePipeline->canRunOnMongos())) {
        return runPipelineOnMongoS(expCtx,
                                   namespaces,
                                   request,
                                   litePipe,
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
        request, expCtx, mergingShardId, mergingShardContributesData, mergePipeline);

    // Dispatch $mergeCursors to the chosen shard, store the resulting cursor, and return.
    auto mergeResponse = establishMergingShardCursor(
        opCtx, namespaces.executionNss, request, mergeCmdObj, mergingShardId);
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
                                      const PrivilegeVector& privileges,
                                      BSONObjBuilder* result) {
    uassert(51028, "Cannot specify exchange option to a mongos", !request.getExchangeSpec());
    uassert(51143,
            "Cannot specify runtime constants option to a mongos",
            !request.getRuntimeConstants());
    uassert(51089,
            str::stream() << "Internal parameter(s) [" << AggregationRequest::kNeedsMergeName
                          << ", "
                          << AggregationRequest::kFromMongosName
                          << ", "
                          << AggregationRequest::kMergeByPBRTName
                          << "] cannot be set to 'true' when sent to mongos",
            !request.needsMerge() && !request.isFromMongos() && !request.mergeByPBRT());
    auto executionNsRoutingInfoStatus =
        sharded_agg_helpers::getExecutionNsRoutingInfo(opCtx, namespaces.executionNss);
    boost::optional<CachedCollectionRoutingInfo> routingInfo;
    LiteParsedPipeline litePipe(request);
    const auto isSharded = [](OperationContext* opCtx, const NamespaceString& nss) -> bool {
        const auto resolvedNsRoutingInfo =
            uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, nss));
        return resolvedNsRoutingInfo.cm().get();
    };
    const bool involvesShardedCollections = litePipe.verifyIsSupported(
        opCtx, isSharded, request.getExplain(), serverGlobalParams.enableMajorityReadConcern);

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
    const bool mustRunOnAll =
        sharded_agg_helpers::mustRunOnAllShards(namespaces.executionNss, litePipe);

    // If we don't have a routing table, then this is a $changeStream which must run on all shards.
    invariant(routingInfo || (mustRunOnAll && litePipe.hasChangeStream()));

    auto resolvedNamespaces = resolveInvolvedNamespaces(opCtx, litePipe);

    // A pipeline is allowed to passthrough to the primary shard iff the following conditions are
    // met:
    //
    // 1. The namespace of the aggregate and any other involved namespaces are unsharded.
    // 2. Is allowed to be forwarded to shards.
    // 3. Does not need to run on all shards.
    // 4. Doesn't need transformation via DocumentSource::serialize().
    if (routingInfo && !routingInfo->cm() && !mustRunOnAll &&
        litePipe.allowedToPassthroughFromMongos() && !involvesShardedCollections) {
        const auto primaryShardId = routingInfo->db().primary()->getId();
        return aggPassthrough(
            opCtx, namespaces, primaryShardId, request, litePipe, privileges, result);
    }

    // Populate the collection UUID and the appropriate collation to use.
    auto collInfo = getCollationAndUUID(routingInfo, namespaces.executionNss, request, litePipe);
    BSONObj collationObj = collInfo.first;
    boost::optional<UUID> uuid = collInfo.second;

    // Build an ExpressionContext for the pipeline. This instantiates an appropriate collator,
    // resolves all involved namespaces, and creates a shared MongoProcessInterface for use by the
    // pipeline's stages.
    auto expCtx = makeExpressionContext(
        opCtx, request, litePipe, collationObj, uuid, std::move(resolvedNamespaces));

    // Parse and optimize the full pipeline.
    auto pipeline = uassertStatusOK(Pipeline::parse(request.getPipeline(), expCtx));
    pipeline->optimizePipeline();

    // Check whether the entire pipeline must be run on mongoS.
    if (pipeline->requiredToRunOnMongos()) {
        // If this is an explain write the explain output and return.
        if (expCtx->explain) {
            *result << "splitPipeline" << BSONNULL << "mongos"
                    << Document{{"host", getHostNameCachedAndPort()},
                                {"stages", pipeline->writeExplainOps(*expCtx->explain)}};
            return Status::OK();
        }

        return runPipelineOnMongoS(
            expCtx, namespaces, request, litePipe, std::move(pipeline), result, privileges);
    }

    // If not, split the pipeline as necessary and dispatch to the relevant shards.
    auto shardDispatchResults = sharded_agg_helpers::dispatchShardPipeline(
        expCtx, namespaces.executionNss, request, litePipe, std::move(pipeline), collationObj);

    // If the operation is an explain, then we verify that it succeeded on all targeted shards,
    // write the results to the output builder, and return immediately.
    if (expCtx->explain) {
        return appendExplainResults(std::move(shardDispatchResults), expCtx, result);
    }

    // If this isn't an explain, then we must have established cursors on at least one shard.
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
        shardDispatchResults = dispatchExchangeConsumerPipeline(expCtx,
                                                                namespaces.executionNss,
                                                                request,
                                                                litePipe,
                                                                collationObj,
                                                                &shardDispatchResults);
    }

    // If we reach here, we have a merge pipeline to dispatch.
    return dispatchMergingPipeline(expCtx,
                                   namespaces,
                                   request,
                                   litePipe,
                                   routingInfo,
                                   std::move(shardDispatchResults),
                                   result,
                                   privileges);
}

Status ClusterAggregate::aggPassthrough(OperationContext* opCtx,
                                        const Namespaces& namespaces,
                                        const ShardId& shardId,
                                        const AggregationRequest& aggRequest,
                                        const LiteParsedPipeline& liteParsedPipeline,
                                        const PrivilegeVector& privileges,
                                        BSONObjBuilder* out) {
    // Format the command for the shard. This adds the 'fromMongos' field, wraps the command as an
    // explain if necessary, and rewrites the result into a format safe to forward to shards.
    BSONObj cmdObj = CommandHelpers::filterCommandRequestForPassthrough(
        sharded_agg_helpers::createPassthroughCommandForShard(
            opCtx, aggRequest, boost::none, nullptr, BSONObj()));

    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        namespaces.executionNss.db().toString(),
        {{shardId,
          shardId != ShardRegistry::kConfigServerShardId
              ? appendShardVersion(std::move(cmdObj), ChunkVersion::UNSHARDED())
              : std::move(cmdObj)}},
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
    if (aggRequest.getExplain()) {
        // If this was an explain, then we get back an explain result object rather than a cursor.
        result = response.swResponse.getValue().data;
    } else {
        auto tailMode = liteParsedPipeline.hasChangeStream()
            ? TailableModeEnum::kTailableAndAwaitData
            : TailableModeEnum::kNormal;
        result = uassertStatusOK(
            storePossibleCursor(opCtx,
                                shardId,
                                *response.shardHostAndPort,
                                response.swResponse.getValue().data,
                                namespaces.requestedNss,
                                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                                Grid::get(opCtx)->getCursorManager(),
                                privileges,
                                tailMode));
    }

    // First append the properly constructed writeConcernError. It will then be skipped
    // in appendElementsUnique.
    if (auto wcErrorElem = result["writeConcernError"]) {
        appendWriteConcernErrorToCmdResponse(shardId, wcErrorElem, *out);
    }

    out->appendElementsUnique(CommandHelpers::filterCommandReplyForPassthrough(result));

    return getStatusFromCommandResult(out->asTempObj());
}

Status ClusterAggregate::retryOnViewError(OperationContext* opCtx,
                                          const AggregationRequest& request,
                                          const ResolvedView& resolvedView,
                                          const NamespaceString& requestedNss,
                                          const PrivilegeVector& privileges,
                                          BSONObjBuilder* result,
                                          unsigned numberRetries) {
    if (numberRetries >= kMaxViewRetries) {
        return Status(ErrorCodes::InternalError,
                      "Failed to resolve view after max number of retries.");
    }

    auto resolvedAggRequest = resolvedView.asExpandedViewAggregation(request);
    result->resetToEmpty();

    if (auto txnRouter = TransactionRouter::get(opCtx)) {
        txnRouter->onViewResolutionError(opCtx, requestedNss);
    }

    // We pass both the underlying collection namespace and the view namespace here. The
    // underlying collection namespace is used to execute the aggregation on mongoD. Any cursor
    // returned will be registered under the view namespace so that subsequent getMore and
    // killCursors calls against the view have access.
    Namespaces nsStruct;
    nsStruct.requestedNss = requestedNss;
    nsStruct.executionNss = resolvedView.getNamespace();

    auto status =
        ClusterAggregate::runAggregate(opCtx, nsStruct, resolvedAggRequest, privileges, result);

    // If the underlying namespace was changed to a view during retry, then re-run the aggregation
    // on the new resolved namespace.
    if (status.extraInfo<ResolvedView>()) {
        return ClusterAggregate::retryOnViewError(opCtx,
                                                  resolvedAggRequest,
                                                  *status.extraInfo<ResolvedView>(),
                                                  requestedNss,
                                                  privileges,
                                                  result,
                                                  numberRetries + 1);
    }

    return status;
}

}  // namespace mongo
