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

#include "mongo/s/query/planner/cluster_aggregation_planner.h"

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
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/global_catalog/router_role_api/collection_uuid_mismatch.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/change_stream_invalidation_info.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/version_context.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/random.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/query/cluster_query_knobs_gen.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/s/query/exec/cluster_client_cursor.h"
#include "mongo/s/query/exec/cluster_client_cursor_impl.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/s/query/exec/collect_query_stats_mongos.h"
#include "mongo/s/query/exec/document_source_merge_cursors.h"
#include "mongo/s/query/exec/establish_cursors.h"
#include "mongo/s/query/exec/owned_remote_cursor.h"
#include "mongo/s/query/exec/router_exec_stage.h"
#include "mongo/s/query/exec/router_stage_limit.h"
#include "mongo/s/query/exec/router_stage_pipeline.h"
#include "mongo/s/query/exec/router_stage_remove_metadata_fields.h"
#include "mongo/s/query/exec/router_stage_skip.h"
#include "mongo/s/query/exec/store_possible_cursor.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstddef>
#include <list>
#include <string>
#include <vector>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

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

    auto response = ars.next(true /* forMergeCursors*/);
    tassert(6273807,
            "requested and received data from just one shard, but results are still pending",
            ars.done());
    return response;
}

/**
 * Contacts the primary shard for the collection default collation.
 *
 * TODO SERVER-79159: This function can be deleted once all unsharded collections are tracked in the
 * sharding catalog (at this point, it wont't be necessary to contact the primary shard for
 * collation information).
 */
BSONObj getUntrackedCollectionCollation(OperationContext* opCtx,
                                        const CollectionRoutingInfo& cri,
                                        const NamespaceString& nss) {
    auto shard = uassertStatusOK(
        Grid::get(opCtx)->shardRegistry()->getShard(opCtx, cri.getDbPrimaryShardId()));
    BSONObj cmdObj = BSON("listCollections" << 1 << "filter" << BSON("name" << nss.coll())
                                            << "cursor" << BSONObj());
    auto cursorResult = uassertStatusOK(
        shard->runExhaustiveCursorCommand(opCtx,
                                          ReadPreferenceSetting{ReadPreference::PrimaryPreferred},
                                          nss.dbName(),
                                          cmdObj,
                                          Milliseconds(-1)));
    std::vector<BSONObj> all = cursorResult.docs;

    // Collection or collection info does not exist; return an empty collation object.
    if (all.empty() || all.front().isEmpty()) {
        return BSONObj();
    }

    auto collectionInfo = all.front();

    // We inspect 'info' to infer the collection default collation.
    BSONObj collationToReturn = CollationSpec::kSimpleSpec;
    if (collectionInfo["options"].type() == BSONType::object) {
        BSONObj collectionOptions = collectionInfo["options"].Obj();
        BSONElement collationElement;
        auto status = bsonExtractTypedField(
            collectionOptions, "collation", BSONType::object, &collationElement);
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
                                     const NamespaceString& nss,
                                     const ShardId& shardId,
                                     const std::vector<ShardId>& targetedShards,
                                     bool hasSpecificMergeShard,
                                     const boost::optional<CollectionRoutingInfo>& cri,
                                     bool mergingShardContributesData,
                                     const Pipeline* pipelineForMerging,
                                     bool requestQueryStatsFromRemotes) {
    MutableDocument mergeCmd(serializedCommand);

    mergeCmd["pipeline"] = Value(pipelineForMerging->serialize());
    if (auto isTranslated = pipelineForMerging->isTranslated()) {
        mergeCmd[AggregateCommandRequest::kTranslatedForViewlessTimeseriesFieldName] =
            Value(isTranslated);
    }

    aggregation_request_helper::setFromRouter(
        VersionContext::getDecoration(mergeCtx->getOperationContext()), mergeCmd, Value(true));

    mergeCmd[AggregateCommandRequest::kLetFieldName] =
        Value(mergeCtx->variablesParseState.serialize(mergeCtx->variables));

    if (requestQueryStatsFromRemotes) {
        mergeCmd[AggregateCommandRequest::kIncludeQueryStatsMetricsFieldName] = Value(true);
    }

    // If the user didn't specify a collation already, make sure there's a collation attached to
    // the merge command, since the merging shard may not have the collection metadata.
    if (mergeCmd.peek()["collation"].missing()) {
        mergeCmd["collation"] = [&]() {
            // When getIgnoreCollator() returns 'false', we can guarantee that we have correctly
            // obtained the collection-default collation (in the case of an untracked collection,
            // this means that we contacted the primary shard). As such, we know that the `nullptr`
            // collation really means the simple collation here.
            if (!mergeCtx->getIgnoreCollator()) {
                return Value(mergeCtx->getCollator() ? mergeCtx->getCollator()->getSpec().toBSON()
                                                     : CollationSpec::kSimpleSpec);
            } else if (cri && !cri->hasRoutingTable()) {
                // If we are dispatching a merging pipeline to a specific shard, and the main
                // namespace is untracked, we must contact the primary shard to determine whether or
                // not there exists a collection default collation. This is unfortunate, but
                // necessary, because while the shards part of the pipeline will discover the
                // collection default collation upon dispatch to the shard which owns the untracked
                // collection. The same is not true for the merging pipeline, however, because the
                // merging shard has no knowledge of the collection default collator.
                //
                // Note also that, unlike tracked collections, which do have information about any
                // collection default collations in the routing information, the same is not true
                // for untracked collections.
                //
                // TODO SERVER-79159: Once all unsharded collections are tracked in the sharding
                // catalog, this 'else' block can be deleted.

                // We should only be contacting the primary shard if the only shard that we are
                // targeting is the primary shard and a stage has designated a specific merging
                // shard.
                tassert(8596500,
                        "Contacting primary shard for collation in unexpected case",
                        targetedShards.size() == 1 &&
                            targetedShards[0] == cri->getDbPrimaryShardId() &&
                            hasSpecificMergeShard);

                if (auto untrackedDefaultCollation =
                        getUntrackedCollectionCollation(mergeCtx->getOperationContext(), *cri, nss);
                    !untrackedDefaultCollation.isEmpty()) {
                    return Value(untrackedDefaultCollation);
                }
            }
            return Value(Document{CollationSpec::kSimpleSpec});
        }();
    }

    const auto txnRouter = TransactionRouter::get(mergeCtx->getOperationContext());
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
            mergeCmd[GenericArguments::kRequestGossipRoutingCacheFieldName] =
                Value(arrayBuilder.arr());
        }
    }

    // Attach the IGNORED chunk version to the command. On the shard, this will skip the actual
    // version check but will nonetheless mark the operation as versioned.
    auto mergeCmdObj = appendShardVersion(mergeCmd.freeze().toBson(),
                                          ShardVersionFactory::make(ChunkVersion::IGNORED()));

    // Attach query settings to the command.
    if (auto querySettingsBSON = mergeCtx->getQuerySettings().toBSON();
        !querySettingsBSON.isEmpty()) {
        mergeCmd[AggregateCommandRequest::kQuerySettingsFieldName] = Value(querySettingsBSON);
    }

    // Attach the read and write concerns if needed, and return the final command object.
    return applyReadWriteConcern(mergeCtx->getOperationContext(),
                                 !(txnRouter && mergingShardContributesData), /* appendRC */
                                 !mergeCtx->getExplain(),                     /* appendWC */
                                 mergeCmdObj);
}

Status dispatchMergingPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               RoutingContext& routingCtx,
                               const ClusterAggregate::Namespaces& namespaces,
                               Document serializedCommand,
                               long long batchSize,
                               DispatchShardPipelineResults shardDispatchResults,
                               BSONObjBuilder* result,
                               const PrivilegeVector& privileges,
                               bool hasChangeStream,
                               bool requestQueryStatsFromRemotes) {
    // We should never be in a situation where we call this function on a non-merge pipeline.
    tassert(6525900,
            "tried to dispatch merge pipeline but the pipeline was not split",
            shardDispatchResults.splitPipeline);
    auto mergePipeline = std::move(shardDispatchResults.splitPipeline->mergePipeline);
    tassert(6525901,
            "tried to dispatch merge pipeline but there was no merge portion of the split pipeline",
            mergePipeline);
    auto* opCtx = expCtx->getOperationContext();

    std::vector<ShardId> targetedShards;
    targetedShards.reserve(shardDispatchResults.remoteCursors.size());
    for (auto&& remoteCursor : shardDispatchResults.remoteCursors) {
        targetedShards.emplace_back(std::string{remoteCursor->getShardId()});
    }

    sharded_agg_helpers::partitionAndAddMergeCursorsSource(
        mergePipeline.get(),
        std::move(shardDispatchResults.remoteCursors),
        shardDispatchResults.splitPipeline->shardCursorsSortSpec,
        requestQueryStatsFromRemotes);

    // First, check whether we can merge on the router. If the merge pipeline MUST run on router,
    // then ignore the internalQueryProhibitMergingOnMongoS parameter.
    if (mergePipeline->requiredToRunOnRouter() ||
        (!internalQueryProhibitMergingOnMongoS.load() && mergePipeline->canRunOnRouter().isOK() &&
         !shardDispatchResults.mergeShardId)) {

        return runPipelineOnMongoS(namespaces,
                                   batchSize,
                                   std::move(mergePipeline),
                                   result,
                                   privileges,
                                   requestQueryStatsFromRemotes);
    }

    const ShardId mergingShardId =
        pickMergingShard(opCtx, shardDispatchResults.mergeShardId, targetedShards);
    const bool mergingShardContributesData =
        std::find(targetedShards.begin(), targetedShards.end(), mergingShardId) !=
        targetedShards.end();

    const auto& cri = routingCtx.hasNss(namespaces.executionNss)
        ? boost::optional<CollectionRoutingInfo>(
              routingCtx.getCollectionRoutingInfo(namespaces.executionNss))
        : boost::none;
    auto mergeCmdObj = createCommandForMergingShard(serializedCommand,
                                                    expCtx,
                                                    namespaces.requestedNss,
                                                    mergingShardId,
                                                    targetedShards,
                                                    shardDispatchResults.mergeShardId.has_value(),
                                                    cri,
                                                    mergingShardContributesData,
                                                    mergePipeline.get(),
                                                    requestQueryStatsFromRemotes);

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
                            expCtx->getTailableMode()));

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
                                     std::unique_ptr<Pipeline> pipelineForMerging,
                                     const PrivilegeVector& privileges,
                                     bool requestQueryStatsFromRemotes) {
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
    params.tailableMode = pipelineForMerging->getContext()->getTailableMode();
    // A batch size of 0 is legal for the initial aggregate, but not valid for getMores, the batch
    // size we pass here is used for getMores, so do not specify a batch size if the initial request
    // had a batch size of 0.
    params.batchSize = batchSize == 0 ? boost::none : boost::make_optional(batchSize);
    params.originatingPrivileges = privileges;
    params.requestQueryStatsFromRemotes = requestQueryStatsFromRemotes;

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
        const auto& nextObj = *next.getResult();

        if (!FindCommon::haveSpaceForNext(nextObj, objCount, responseBuilder.bytesUsed())) {
            ccc->queueResult(std::move(next));
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
        opDebug.additiveMetrics.aggregateDataBearingNodeMetrics(ccc->takeRemoteMetrics());
        collectQueryStatsMongos(opCtx, ccc->takeKey());
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
    RoutingContext& routingCtx,
    const NamespaceString& executionNss,
    Document serializedCommand,
    DispatchShardPipelineResults* shardDispatchResults,
    bool requestQueryStatsFromRemotes) {
    tassert(7163600,
            "dispatchExchangeConsumerPipeline() must not be called for explain operation",
            !expCtx->getExplain());
    auto opCtx = expCtx->getOperationContext();

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
            shardDispatchResults->splitPipeline->shardCursorsSortSpec,
            requestQueryStatsFromRemotes);

        consumerPipelines.push_back(SplitPipeline::shardsOnly(std::move(consumerPipeline)));

        auto consumerCmdObj =
            sharded_agg_helpers::createCommandForTargetedShards(expCtx,
                                                                serializedCommand,
                                                                consumerPipelines.back(),
                                                                boost::none, /* exchangeSpec */
                                                                false /* needsMerge */,
                                                                boost::none /* explain */,
                                                                boost::none /* readConcern */,
                                                                requestQueryStatsFromRemotes);

        requests.emplace_back(shardDispatchResults->exchangeSpec->consumerShards[idx],
                              consumerCmdObj);
    }
    auto cursors = establishCursors(opCtx,
                                    Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                                    executionNss,
                                    ReadPreferenceSetting::get(opCtx),
                                    requests,
                                    false /* do not allow partial results */,
                                    &routingCtx);

    // Convert remote cursors into a vector of "owned" cursors.
    std::vector<OwnedRemoteCursor> ownedCursors;
    ownedCursors.reserve(cursors.size());
    for (auto&& cursor : cursors) {
        ownedCursors.emplace_back(opCtx, std::move(cursor), executionNss);
    }

    // The merging pipeline is just a union of the results from each of the shards involved on the
    // consumer side of the exchange.
    auto mergePipeline = Pipeline::create({}, expCtx);
    mergePipeline->setSplitState(PipelineSplitState::kSplitForMerge);

    auto splitPipeline = SplitPipeline::mergeOnly(std::move(mergePipeline));

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

ClusterClientCursorGuard convertPipelineToRouterStages(std::unique_ptr<Pipeline> pipeline,
                                                       ClusterClientCursorParams&& cursorParams) {
    auto* opCtx = pipeline->getContext()->getOperationContext();

    // We expect the pipeline to be fully executable at this point, so if the pipeline was all skips
    // and limits we expect it to start with a $mergeCursors stage.
    auto sourceIt = pipeline->getSources().begin();
    auto mergeCursors = checked_cast<DocumentSourceMergeCursors*>(sourceIt->get());

    // Replace the pipeline with RouterExecStages.
    std::unique_ptr<RouterExecStage> root = mergeCursors->convertToRouterStage();
    for (++sourceIt; sourceIt != pipeline->getSources().end(); ++sourceIt) {
        if (auto skip = dynamic_cast<DocumentSourceSkip*>(sourceIt->get())) {
            root = std::make_unique<RouterStageSkip>(opCtx, std::move(root), skip->getSkip());
        } else if (auto limit = dynamic_cast<DocumentSourceLimit*>(sourceIt->get())) {
            root = std::make_unique<RouterStageLimit>(opCtx, std::move(root), limit->getLimit());
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
                                            std::unique_ptr<Pipeline> pipeline,
                                            ClusterClientCursorParams&& cursorParams) {
    if (isAllLimitsAndSkips(pipeline.get())) {
        // We can optimize this Pipeline to avoid going through any DocumentSources at all and thus
        // skip the expensive BSON->Document->BSON conversion.
        return convertPipelineToRouterStages(std::move(pipeline), std::move(cursorParams));
    }
    return ClusterClientCursorImpl::make(
        opCtx, std::make_unique<RouterStagePipeline>(std::move(pipeline)), std::move(cursorParams));
}

AggregationTargeter AggregationTargeter::make(OperationContext* opCtx,
                                              std::unique_ptr<Pipeline> pipeline,
                                              const NamespaceString& execNss,
                                              boost::optional<CollectionRoutingInfo> cri,
                                              PipelineDataSource pipelineDataSource,
                                              bool perShardCursor) {
    // TODO SERVER-97620 get this info from pipeline instead of passing in a separate field for it.
    tassert(7972401,
            "Aggregation did not have a routing table and does not feature either a $changeStream "
            " or a collectionless aggregate.",
            cri || pipelineDataSource == PipelineDataSource::kChangeStream ||
                execNss.isCollectionlessAggregateNS());

    auto policy = pipeline->requiredToRunOnRouter() ? TargetingPolicy::kMongosRequired
                                                    : TargetingPolicy::kAnyShard;
    if (pipelineDataSource == PipelineDataSource::kGeneratesOwnDataOnce &&
        !pipeline->needsSpecificShardMerger() && pipeline->canRunOnRouter().isOK()) {
        // If we don't have a routing table, the first stage is marked as `kGeneratesOwnDataOnce`,
        // we aren't required to merge on a specific ShardId, and the pipeline can run on the
        // router, we will run on mongos.
        policy = TargetingPolicy::kMongosRequired;
    }
    if (perShardCursor) {
        policy = TargetingPolicy::kSpecificShardOnly;
    }

    return AggregationTargeter{policy, std::move(pipeline)};
}

Status runPipelineOnMongoS(const ClusterAggregate::Namespaces& namespaces,
                           long long batchSize,
                           std::unique_ptr<Pipeline> pipeline,
                           BSONObjBuilder* result,
                           const PrivilegeVector& privileges,
                           bool requestQueryStatsFromRemotes) {
    auto expCtx = pipeline->getContext();

    // We should never receive a pipeline which cannot run on router.
    invariant(!expCtx->getExplain());
    uassertStatusOKWithContext(pipeline->canRunOnRouter(),
                               "pipeline is required to run on router, but cannot");


    // Verify that the first stage can produce input for the remainder of the pipeline.
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Aggregation pipeline must be run on mongoS, but "
                          << pipeline->getSources().front()->getSourceName()
                          << " is not capable of producing input",
            !pipeline->getSources().front()->constraints().requiresInputDocSource);

    // Register the new mongoS cursor, and retrieve the initial batch of results.
    auto cursorResponse = establishMergingMongosCursor(expCtx->getOperationContext(),
                                                       batchSize,
                                                       namespaces.requestedNss,
                                                       std::move(pipeline),
                                                       privileges,
                                                       requestQueryStatsFromRemotes);

    // We don't need to storePossibleCursor or propagate writeConcern errors; a pipeline with
    // writing stages like $out can never run on mongoS. Filter the command response and return
    // immediately.
    CommandHelpers::filterCommandReplyForPassthrough(cursorResponse, result);
    return getStatusFromCommandResult(result->asTempObj());
}

Status dispatchPipelineAndMerge(OperationContext* opCtx,
                                RoutingContext& routingCtx,
                                AggregationTargeter targeter,
                                Document serializedCommand,
                                long long batchSize,
                                const ClusterAggregate::Namespaces& namespaces,
                                const PrivilegeVector& privileges,
                                BSONObjBuilder* result,
                                PipelineDataSource pipelineDataSource,
                                bool eligibleForSampling,
                                bool requestQueryStatsFromRemotes) {
    auto expCtx = targeter.pipeline->getContext();

    // If not, split the pipeline as necessary and dispatch to the relevant shards.
    auto shardDispatchResults =
        sharded_agg_helpers::dispatchShardPipeline(routingCtx,
                                                   serializedCommand,
                                                   pipelineDataSource,
                                                   eligibleForSampling,
                                                   std::move(targeter.pipeline),
                                                   expCtx->getExplain(),
                                                   namespaces.executionNss,
                                                   requestQueryStatsFromRemotes);

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
    if (expCtx->getExplain()) {
        return sharded_agg_helpers::appendExplainResults(
            std::move(shardDispatchResults), expCtx, result);
    }

    // If this isn't an explain, then we must have established cursors on at least one
    // shard.
    invariant(shardDispatchResults.remoteCursors.size() > 0);

    // If we sent the entire pipeline to a single shard, store the remote cursor and return.
    if (!shardDispatchResults.splitPipeline) {
        tassert(4457012,
                "pipeline was not split, but more than one remote cursor is present",
                shardDispatchResults.remoteCursors.size() == 1);
        auto&& remoteCursor = std::move(shardDispatchResults.remoteCursors.front());
        const auto shardId = std::string{remoteCursor->getShardId()};
        const auto reply = uassertStatusOK(storePossibleCursor(opCtx,
                                                               namespaces.requestedNss,
                                                               std::move(remoteCursor),
                                                               privileges,
                                                               expCtx->getTailableMode()));
        return appendCursorResponseToCommandResult(shardId, reply, result);
    }

    // If we have the exchange spec then dispatch all consumers.
    if (shardDispatchResults.exchangeSpec) {
        shardDispatchResults = dispatchExchangeConsumerPipeline(expCtx,
                                                                routingCtx,
                                                                namespaces.executionNss,
                                                                serializedCommand,
                                                                &shardDispatchResults,
                                                                requestQueryStatsFromRemotes);
    }

    shardedAggregateHangBeforeDispatchMergingPipeline.pauseWhileSet();

    // If we reach here, we have a merge pipeline to dispatch.
    return dispatchMergingPipeline(expCtx,
                                   routingCtx,
                                   namespaces,
                                   serializedCommand,
                                   batchSize,
                                   std::move(shardDispatchResults),
                                   result,
                                   privileges,
                                   pipelineDataSource == PipelineDataSource::kChangeStream,
                                   requestQueryStatsFromRemotes);
}

BSONObj getCollation(OperationContext* opCtx,
                     const boost::optional<CollectionRoutingInfo>& cri,
                     const NamespaceString& nss,
                     const BSONObj& collation,
                     bool requiresCollationForParsingUnshardedAggregate) {
    // If this is a collectionless aggregation or if the user specified an explicit collation,
    // we immediately return the user-defined collation if one exists, or an empty BSONObj
    // otherwise.
    if (nss.isCollectionlessAggregateNS() || !collation.isEmpty() || !cri) {
        return collation;
    }

    // If the target collection is untracked, we will contact the primary shard to discover this
    // information if it is necessary for pipeline parsing. Otherwise, we infer the collation once
    // the command is executed on the primary shard.
    if (!cri->hasRoutingTable()) {
        return requiresCollationForParsingUnshardedAggregate
            ? getUntrackedCollectionCollation(opCtx, *cri, nss)
            : BSONObj();
    }

    // Return the default collator if one exists, otherwise return the simple collation.
    if (auto defaultCollator = cri->getChunkManager().getDefaultCollator()) {
        return defaultCollator->getSpec().toBSON();
    }

    return CollationSpec::kSimpleSpec;
}

Status runPipelineOnSpecificShardOnly(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      RoutingContext& routingCtx,
                                      const ClusterAggregate::Namespaces& namespaces,
                                      boost::optional<ExplainOptions::Verbosity> explain,
                                      Document serializedCommand,
                                      const PrivilegeVector& privileges,
                                      ShardId shardId,
                                      BSONObjBuilder* out,
                                      bool requestQueryStatsFromRemotes) {
    auto opCtx = expCtx->getOperationContext();

    tassert(6273804,
            "Per shard cursors are supposed to pass fromRouter: false to shards",
            !expCtx->getInRouter());
    // By using an initial batchSize of zero all of the events will get returned through
    // the getMore path and have metadata stripped out.
    boost::optional<int> overrideBatchSize = 0;

    // Format the command for the shard. This wraps the command as an explain if necessary, and
    // rewrites the result into a format safe to forward to shards.
    BSONObj cmdObj =
        sharded_agg_helpers::createPassthroughCommandForShard(expCtx,
                                                              serializedCommand,
                                                              explain,
                                                              nullptr /* pipeline */,
                                                              boost::none,
                                                              overrideBatchSize,
                                                              requestQueryStatsFromRemotes);

    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        namespaces.executionNss.dbName(),
        {{shardId, cmdObj}},
        ReadPreferenceSetting::get(opCtx),
        Shard::RetryPolicy::kIdempotent);

    if (routingCtx.hasNss(namespaces.executionNss)) {
        routingCtx.onRequestSentForNss(namespaces.executionNss);
    }

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
        collectQueryStatsMongos(opCtx, std::move(CurOp::get(opCtx)->debug().queryStatsInfo.key));
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
            expCtx->getTailableMode(),
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
