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


#include <absl/container/node_hash_set.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongos_process_interface.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/explain_common.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/query_settings/query_settings_utils.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_stats/agg_key.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/tailable_mode_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/num_hosts_targeted_metrics.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_aggregate.h"
#include "mongo/s/query/cluster_aggregation_planner.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

constexpr unsigned ClusterAggregate::kMaxViewRetries;
using sharded_agg_helpers::PipelineDataSource;

namespace {

// "Resolve" involved namespaces into a map. We won't try to execute anything on a mongos, but we
// still have to populate this map so that any $lookups, etc. will be able to have a resolved view
// definition. It's okay that this is incorrect, we will repopulate the real namespace map on the
// mongod. Note that this function must be called before forwarding an aggregation command on an
// unsharded collection, in order to verify that the involved namespaces are allowed to be sharded.
auto resolveInvolvedNamespaces(const stdx::unordered_set<NamespaceString>& involvedNamespaces) {
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    for (auto&& nss : involvedNamespaces) {
        resolvedNamespaces.try_emplace(nss.coll(), nss, std::vector<BSONObj>{});
    }
    return resolvedNamespaces;
}

// Build an appropriate ExpressionContext for the pipeline. This helper instantiates an appropriate
// collator, creates a MongoProcessInterface for use by the pipeline's stages, and sets the
// collection UUID if provided.
boost::intrusive_ptr<ExpressionContext> makeExpressionContext(
    OperationContext* opCtx,
    const AggregateCommandRequest& request,
    const boost::optional<CollectionRoutingInfo>& cri,
    BSONObj collationObj,
    boost::optional<UUID> uuid,
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces,
    bool hasChangeStream) {

    std::unique_ptr<CollatorInterface> collation;
    if (!collationObj.isEmpty()) {
        // This will be null if attempting to build an interface for the simple collator.
        collation = uassertStatusOK(
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collationObj));
    }

    // Create the expression context, and set 'inMongos' to true. We explicitly do *not* set
    // mergeCtx->tempDir.
    auto mergeCtx = make_intrusive<ExpressionContext>(
        opCtx,
        request,
        std::move(collation),
        std::make_shared<MongosProcessInterface>(
            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor()),
        std::move(resolvedNamespaces),
        uuid);

    mergeCtx->inMongos = true;

    if ((!cri || !cri->cm.hasRoutingTable()) && collationObj.isEmpty()) {
        mergeCtx->setIgnoreCollator();
    }

    // Serialize the 'AggregateCommandRequest' and save it so that the original command can be
    // reconstructed for dispatch to a new shard, which is sometimes necessary for change streams
    // pipelines.
    if (hasChangeStream) {
        mergeCtx->originalAggregateCommand =
            aggregation_request_helper::serializeToCommandObj(request);
    }

    return mergeCtx;
}

void appendEmptyResultSetWithStatus(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    Status status,
                                    BSONObjBuilder* result) {
    // Rewrite ShardNotFound as NamespaceNotFound so that appendEmptyResultSet swallows it.
    if (status == ErrorCodes::ShardNotFound) {
        status = {ErrorCodes::NamespaceNotFound, status.reason()};
    }
    collectQueryStatsMongos(opCtx, std::move(CurOp::get(opCtx)->debug().queryStatsInfo.key));
    appendEmptyResultSet(opCtx, *result, status, nss);
}

void updateHostsTargetedMetrics(OperationContext* opCtx,
                                const NamespaceString& executionNss,
                                const boost::optional<ChunkManager>& cm,
                                const stdx::unordered_set<NamespaceString>& involvedNamespaces) {
    if (!cm)
        return;

    // Create a set of ShardIds that own a chunk belonging to any of the collections involved in
    // this pipeline. This will be used to determine whether the pipeline targeted all of the shards
    // that own chunks for any collection involved or not.
    //
    // Note that this will only take into account collections that are sharded. If the namespace is
    // an unsharded collection we will not track which shard owns it here. This is to preserve
    // semantics of the tracked host metrics since unsharded collections are tracked separately on
    // its own value.
    std::set<ShardId> shardsOwningChunks = [&]() {
        std::set<ShardId> shardsIds;

        if (cm->isSharded()) {
            std::set<ShardId> shardIdsForNs;
            cm->getAllShardIds(&shardIdsForNs);
            for (const auto& shardId : shardIdsForNs) {
                shardsIds.insert(shardId);
            }
        }

        for (const auto& nss : involvedNamespaces) {
            if (nss == executionNss)
                continue;

            const auto [resolvedNsCM, _] =
                uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, nss));
            if (resolvedNsCM.isSharded()) {
                std::set<ShardId> shardIdsForNs;
                resolvedNsCM.getAllShardIds(&shardIdsForNs);
                for (const auto& shardId : shardIdsForNs) {
                    shardsIds.insert(shardId);
                }
            }
        }

        return shardsIds;
    }();

    auto nShardsTargeted = CurOp::get(opCtx)->debug().nShards;
    if (nShardsTargeted > 0) {
        // shardsOwningChunks will only contain something if we targeted a sharded collection in the
        // pipeline.
        bool hasTargetedShardedCollection = !shardsOwningChunks.empty();
        auto targetType = NumHostsTargetedMetrics::get(opCtx).parseTargetType(
            opCtx, nShardsTargeted, shardsOwningChunks.size(), hasTargetedShardedCollection);
        NumHostsTargetedMetrics::get(opCtx).addNumHostsTargeted(
            NumHostsTargetedMetrics::QueryType::kAggregateCmd, targetType);
    }
}

/**
 * Performs validations related to API versioning, time-series stages, and general command
 * validation.
 * Throws UserAssertion if any of the validations fails
 *     - validation of API versioning on each stage on the pipeline
 *     - validation of API versioning on 'AggregateCommandRequest' request
 *     - validation of time-series related stages
 *     - validation of command parameters
 */
void performValidationChecks(const OperationContext* opCtx,
                             const AggregateCommandRequest& request,
                             const LiteParsedPipeline& liteParsedPipeline) {
    liteParsedPipeline.validate(opCtx);
    aggregation_request_helper::validateRequestForAPIVersion(opCtx, request);
    aggregation_request_helper::validateRequestFromClusterQueryWithoutShardKey(request);
}

/**
 * Rebuilds the pipeline and uses a different granularity value for the 'bucketMaxSpanSeconds' field
 * in the $_internalUnpackBucket stage.
 */
std::vector<BSONObj> rebuildPipelineWithTimeSeriesGranularity(
    const std::vector<BSONObj>& pipeline,
    boost::optional<BucketGranularityEnum> granularity,
    boost::optional<int32_t> maxSpanSeconds) {
    int32_t bucketSpan = 0;

    if (maxSpanSeconds) {
        bucketSpan = *maxSpanSeconds;
    } else {
        bucketSpan = timeseries::getMaxSpanSecondsFromGranularity(
            granularity.get_value_or(BucketGranularityEnum::Seconds));
    }

    std::vector<BSONObj> newPipeline;
    for (auto& stage : pipeline) {
        if (stage.firstElementFieldNameStringData() ==
                DocumentSourceInternalUnpackBucket::kStageNameInternal ||
            stage.firstElementFieldNameStringData() ==
                DocumentSourceInternalUnpackBucket::kStageNameExternal) {
            BSONObjBuilder newOptions;
            for (auto& elem : stage.firstElement().Obj()) {
                if (elem.fieldNameStringData() ==
                    DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds) {
                    newOptions.append(DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds,
                                      bucketSpan);
                } else {
                    newOptions.append(elem);
                }
            }
            newPipeline.push_back(
                BSON(stage.firstElementFieldNameStringData() << newOptions.obj()));
            continue;
        }
        newPipeline.push_back(stage);
    }
    return newPipeline;
}

/**
 * Builds an expCtx with which to parse the request's pipeline, then parses the pipeline and
 * registers the pre-optimized pipeline with query stats collection.
 */
std::unique_ptr<Pipeline, PipelineDeleter> parsePipelineAndRegisterQueryStats(
    OperationContext* opCtx,
    const stdx::unordered_set<NamespaceString>& involvedNamespaces,
    const NamespaceString& executionNss,
    AggregateCommandRequest& request,
    boost::optional<CollectionRoutingInfo> cri,
    bool hasChangeStream,
    bool shouldDoFLERewrite,
    bool requiresCollationForParsingUnshardedAggregate) {
    // Populate the collation. If this is a change stream, take the user-defined collation if one
    // exists, or an empty BSONObj otherwise. Change streams never inherit the collection's default
    // collation, and since collectionless aggregations generally run on the 'admin'
    // database, the standard logic would attempt to resolve its non-existent UUID and
    // collation by sending a specious 'listCollections' command to the config servers.
    auto collationObj = hasChangeStream ? request.getCollation().value_or(BSONObj())
                                        : cluster_aggregation_planner::getCollation(
                                              opCtx,
                                              cri ? boost::make_optional(cri->cm) : boost::none,
                                              executionNss,
                                              request.getCollation().value_or(BSONObj()),
                                              requiresCollationForParsingUnshardedAggregate);

    // Build an ExpressionContext for the pipeline. This instantiates an appropriate collator,
    // includes all involved namespaces, and creates a shared MongoProcessInterface for use by
    // the pipeline's stages.
    boost::intrusive_ptr<ExpressionContext> expCtx =
        makeExpressionContext(opCtx,
                              request,
                              cri,
                              collationObj,
                              boost::none /* uuid */,
                              resolveInvolvedNamespaces(involvedNamespaces),
                              hasChangeStream);

    auto pipeline = Pipeline::parse(request.getPipeline(), expCtx);
    // Skip query stats recording for queryable encryption queries.
    if (!shouldDoFLERewrite) {
        query_stats::registerRequest(opCtx, executionNss, [&]() {
            return std::make_unique<query_stats::AggKey>(
                request, *pipeline, expCtx, involvedNamespaces, executionNss);
        });
    }

    // Perform the query settings lookup and attach it to the ExpressionContext.
    expCtx->setQuerySettings(query_settings::lookupQuerySettingsForAgg(
        expCtx, request, *pipeline, involvedNamespaces, executionNss));

    return pipeline;
}

}  // namespace

Status ClusterAggregate::runAggregate(OperationContext* opCtx,
                                      const Namespaces& namespaces,
                                      AggregateCommandRequest& request,
                                      const PrivilegeVector& privileges,
                                      BSONObjBuilder* result) {
    return runAggregate(opCtx, namespaces, request, {request}, privileges, result);
}

Status ClusterAggregate::runAggregate(OperationContext* opCtx,
                                      const Namespaces& namespaces,
                                      AggregateCommandRequest& request,
                                      const LiteParsedPipeline& liteParsedPipeline,
                                      const PrivilegeVector& privileges,
                                      BSONObjBuilder* result) {
    return runAggregate(
        opCtx, namespaces, request, liteParsedPipeline, privileges, boost::none, result);
}

Status ClusterAggregate::runAggregate(OperationContext* opCtx,
                                      const Namespaces& namespaces,
                                      AggregateCommandRequest& request,
                                      const LiteParsedPipeline& liteParsedPipeline,
                                      const PrivilegeVector& privileges,
                                      boost::optional<CollectionRoutingInfo> cri,
                                      BSONObjBuilder* result) {
    // Perform some validations on the LiteParsedPipeline and request before continuing with the
    // aggregation command.
    performValidationChecks(opCtx, request, liteParsedPipeline);

    uassert(51028, "Cannot specify exchange option to a mongos", !request.getExchange());
    uassert(51143,
            "Cannot specify runtime constants option to a mongos",
            !request.getLegacyRuntimeConstants());
    uassert(51089,
            str::stream() << "Internal parameter(s) ["
                          << AggregateCommandRequest::kNeedsMergeFieldName << ", "
                          << AggregateCommandRequest::kFromMongosFieldName
                          << "] cannot be set to 'true' when sent to mongos",
            !request.getNeedsMerge() && !request.getFromMongos());
    uassert(ErrorCodes::BadValue,
            "Aggregate queries on mongoS may not request or provide a resume token",
            !request.getRequestResumeToken() && !request.getResumeAfter());

    const auto isSharded = [](OperationContext* opCtx, const NamespaceString& nss) {
        const auto [resolvedNsCM, _] =
            uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, nss));
        return resolvedNsCM.isSharded();
    };

    liteParsedPipeline.verifyIsSupported(
        opCtx, isSharded, request.getExplain(), serverGlobalParams.enableMajorityReadConcern);
    auto hasChangeStream = liteParsedPipeline.hasChangeStream();
    const auto& involvedNamespaces = liteParsedPipeline.getInvolvedNamespaces();
    auto shouldDoFLERewrite = ::mongo::shouldDoFLERewrite(request);
    auto startsWithQueue = liteParsedPipeline.startsWithQueue();
    auto requiresCollationForParsingUnshardedAggregate =
        liteParsedPipeline.requiresCollationForParsingUnshardedAggregate();
    auto pipelineDataSource = hasChangeStream ? PipelineDataSource::kChangeStream
        : startsWithQueue                     ? PipelineDataSource::kQueue
                                              : PipelineDataSource::kNormal;

    // If the routing table is not already taken by the higher level, fill it now.
    if (!cri) {
        // If the routing table is valid, we obtain a reference to it. If the table is not valid,
        // then either the database does not exist, or there are no shards in the cluster. In the
        // latter case, we always return an empty cursor. In the former case, if the requested
        // aggregation is a $changeStream, we allow the operation to continue so that stream cursors
        // can be established on the given namespace before the database or collection is actually
        // created. If the database does not exist and this is not a $changeStream, then we return
        // an empty cursor.
        auto executionNsRoutingInfoStatus =
            sharded_agg_helpers::getExecutionNsRoutingInfo(opCtx, namespaces.executionNss);

        if (!executionNsRoutingInfoStatus.isOK()) {
            uassert(CollectionUUIDMismatchInfo(request.getDbName(),
                                               *request.getCollectionUUID(),
                                               request.getNamespace().coll().toString(),
                                               boost::none),
                    "Database does not exist",
                    executionNsRoutingInfoStatus != ErrorCodes::NamespaceNotFound ||
                        !request.getCollectionUUID());

            if (liteParsedPipeline.startsWithCollStats()) {
                uassertStatusOKWithContext(executionNsRoutingInfoStatus,
                                           "Unable to retrieve information for $collStats stage");
            }
        }

        if (executionNsRoutingInfoStatus.isOK()) {
            cri = executionNsRoutingInfoStatus.getValue();
        } else if (!((hasChangeStream || startsWithQueue) &&
                     executionNsRoutingInfoStatus == ErrorCodes::NamespaceNotFound)) {
            // To achieve parity with mongod-style responses, parse and validate the query
            // even though the namespace is not found.
            try {
                auto pipeline = parsePipelineAndRegisterQueryStats(
                    opCtx,
                    involvedNamespaces,
                    namespaces.executionNss,
                    request,
                    cri,
                    hasChangeStream,
                    shouldDoFLERewrite,
                    requiresCollationForParsingUnshardedAggregate);
                pipeline->validateCommon(false);
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
                LOGV2_DEBUG(8396400,
                            4,
                            "Skipping query stats due to NamespaceNotFound",
                            "status"_attr = ex.toStatus());
                // ignore redundant NamespaceNotFound errors.
            }

            // if validation is ok, just return empty result
            appendEmptyResultSetWithStatus(
                opCtx, namespaces.requestedNss, executionNsRoutingInfoStatus.getStatus(), result);
            return Status::OK();
        }
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;
    const auto pipelineBuilder = [&]() {
        auto pipeline =
            parsePipelineAndRegisterQueryStats(opCtx,
                                               involvedNamespaces,
                                               namespaces.executionNss,
                                               request,
                                               cri,
                                               hasChangeStream,
                                               shouldDoFLERewrite,
                                               requiresCollationForParsingUnshardedAggregate);
        expCtx = pipeline->getContext();

        // If the aggregate command supports encrypted collections, do rewrites of the pipeline to
        // support querying against encrypted fields.
        if (shouldDoFLERewrite) {
            if (!request.getEncryptionInformation()->getCrudProcessed().value_or(false)) {
                pipeline = processFLEPipelineS(opCtx,
                                               namespaces.executionNss,
                                               request.getEncryptionInformation().value(),
                                               std::move(pipeline));
                request.getEncryptionInformation()->setCrudProcessed(true);
            }
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
        }

        // Optimize the pipeline if:
        // - We have a valid routing table.
        // - We know the collection's collation.
        // - We have a change stream or need to do a FLE rewrite.
        // This is because the results of optimization may depend on knowing the collation.
        // TODO SERVER-81991: Determine whether this is necessary once all unsharded collections are
        // tracked as unsplittable collections in the sharding catalog.
        if ((cri && cri->cm.hasRoutingTable()) || requiresCollationForParsingUnshardedAggregate ||
            hasChangeStream || shouldDoFLERewrite) {
            pipeline->optimizePipeline();

            // Validate the pipeline post-optimization.
            const bool alreadyOptimized = true;
            pipeline->validateCommon(alreadyOptimized);
        }
        return pipeline;
    };

    auto targeter = cluster_aggregation_planner::AggregationTargeter::make(
        opCtx,
        pipelineBuilder,
        cri,
        pipelineDataSource,
        request.getPassthroughToShard().has_value());

    uassert(
        6487500,
        fmt::format("Cannot use {} with an aggregation that executes entirely on mongos",
                    AggregateCommandRequest::kCollectionUUIDFieldName),
        !request.getCollectionUUID() ||
            targeter.policy !=
                cluster_aggregation_planner::AggregationTargeter::TargetingPolicy::kMongosRequired);

    if (!expCtx) {
        // When the AggregationTargeter chooses a "specific shard only" policy, it does not call
        // the 'pipelineBuilder' function, so we've yet to construct an expression context or
        // register query stats. Because this is a passthrough, we only need a bare minimum
        // expression context on mongos.
        tassert(7972400,
                "Expected to have a 'kSpecificShardOnly' targetting policy",
                targeter.policy ==
                    cluster_aggregation_planner::AggregationTargeter::kSpecificShardOnly);

        std::unique_ptr<CollatorInterface> collation = nullptr;
        if (auto collationObj = request.getCollation()) {
            // This will be null if attempting to build an interface for the simple collator.
            collation = uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                            ->makeFromBSON(*collationObj));
        }

        expCtx = make_intrusive<ExpressionContext>(opCtx,
                                                   std::move(collation),
                                                   namespaces.executionNss,
                                                   boost::none /* runtimeConstants */,
                                                   request.getLet());
        expCtx->addResolvedNamespaces(involvedNamespaces);

        // Skip query stats recording for queryable encryption queries.
        if (!shouldDoFLERewrite) {
            // We want to hold off parsing the pipeline until it's clear we must. Because of that,
            // we wait to parse the pipeline until this callback is invoked within
            // query_stats::registerRequest.
            query_stats::registerRequest(opCtx, namespaces.executionNss, [&]() {
                auto pipeline = Pipeline::parse(request.getPipeline(), expCtx);
                return std::make_unique<query_stats::AggKey>(
                    request, *pipeline, expCtx, involvedNamespaces, namespaces.executionNss);
            });
        }
    }

    if (request.getExplain()) {
        explain_common::generateServerInfo(result);
        explain_common::generateServerParameters(opCtx, result);
    }

    auto status = [&]() {
        switch (targeter.policy) {
            case cluster_aggregation_planner::AggregationTargeter::TargetingPolicy::
                kMongosRequired: {
                // If this is an explain write the explain output and return.
                auto expCtx = targeter.pipeline->getContext();
                if (expCtx->explain) {
                    auto opts = SerializationOptions{.verbosity = boost::make_optional(
                                                         ExplainOptions::Verbosity::kQueryPlanner)};
                    *result << "splitPipeline" << BSONNULL << "mongos"
                            << Document{{"host",
                                         prettyHostNameAndPort(
                                             expCtx->opCtx->getClient()->getLocalPort())},
                                        {"stages", targeter.pipeline->writeExplainOps(opts)}};
                    return Status::OK();
                }

                return cluster_aggregation_planner::runPipelineOnMongoS(
                    namespaces,
                    request.getCursor().getBatchSize().value_or(
                        aggregation_request_helper::kDefaultBatchSize),
                    std::move(targeter.pipeline),
                    result,
                    privileges);
            }

            case cluster_aggregation_planner::AggregationTargeter::TargetingPolicy::kAnyShard: {
                const bool eligibleForSampling = !request.getExplain();
                return cluster_aggregation_planner::dispatchPipelineAndMerge(
                    opCtx,
                    std::move(targeter),
                    aggregation_request_helper::serializeToCommandDoc(request),
                    request.getCursor().getBatchSize().value_or(
                        aggregation_request_helper::kDefaultBatchSize),
                    namespaces,
                    privileges,
                    result,
                    pipelineDataSource,
                    eligibleForSampling);
            }
            case cluster_aggregation_planner::AggregationTargeter::TargetingPolicy::
                kSpecificShardOnly: {
                // Mark expCtx as tailable and await data so CCC behaves accordingly.
                expCtx->tailableMode = TailableModeEnum::kTailableAndAwaitData;

                uassert(6273801,
                        "per shard cursor pipeline must contain $changeStream",
                        hasChangeStream);

                ShardId shardId(std::string(request.getPassthroughToShard()->getShard()));

                // This is an aggregation pipeline started internally, so it is not eligible for
                // sampling.
                const bool eligibleForSampling = false;

                return cluster_aggregation_planner::runPipelineOnSpecificShardOnly(
                    expCtx,
                    namespaces,
                    request.getExplain(),
                    aggregation_request_helper::serializeToCommandDoc(request),
                    privileges,
                    shardId,
                    eligibleForSampling,
                    result);
            }

                MONGO_UNREACHABLE;
        }
        MONGO_UNREACHABLE;
    }();

    if (status.isOK()) {
        updateHostsTargetedMetrics(opCtx,
                                   namespaces.executionNss,
                                   cri ? boost::make_optional(cri->cm) : boost::none,
                                   involvedNamespaces);
        // Report usage statistics for each stage in the pipeline.
        liteParsedPipeline.tickGlobalStageCounters();
        // Add 'command' object to explain output.
        if (expCtx->explain) {
            explain_common::appendIfRoom(
                aggregation_request_helper::serializeToCommandObj(request), "command", result);
        }
    }
    return status;
}

Status ClusterAggregate::retryOnViewError(OperationContext* opCtx,
                                          const AggregateCommandRequest& request,
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
        txnRouter.onViewResolutionError(opCtx, requestedNss);
    }

    // We pass both the underlying collection namespace and the view namespace here. The
    // underlying collection namespace is used to execute the aggregation on mongoD. Any cursor
    // returned will be registered under the view namespace so that subsequent getMore and
    // killCursors calls against the view have access.
    Namespaces nsStruct;
    nsStruct.requestedNss = requestedNss;
    nsStruct.executionNss = resolvedView.getNamespace();

    // For a sharded time-series collection, the routing is based on both routing table and the
    // bucketMaxSpanSeconds value. We need to make sure we use the bucketMaxSpanSeconds of the same
    // version as the routing table, instead of the one attached in the view error. This way the
    // shard versioning check can correctly catch stale routing information.
    boost::optional<CollectionRoutingInfo> snapshotCri;
    if (nsStruct.executionNss.isTimeseriesBucketsCollection()) {
        auto executionNsRoutingInfoStatus =
            sharded_agg_helpers::getExecutionNsRoutingInfo(opCtx, nsStruct.executionNss);
        if (executionNsRoutingInfoStatus.isOK()) {
            const auto& cri = executionNsRoutingInfoStatus.getValue();
            if (cri.cm.isSharded() && cri.cm.getTimeseriesFields()) {
                const auto patchedPipeline = rebuildPipelineWithTimeSeriesGranularity(
                    resolvedAggRequest.getPipeline(),
                    cri.cm.getTimeseriesFields()->getGranularity(),
                    cri.cm.getTimeseriesFields()->getBucketMaxSpanSeconds());
                resolvedAggRequest.setPipeline(patchedPipeline);
                snapshotCri = cri;
            }
        }
    }

    auto status = ClusterAggregate::runAggregate(
        opCtx, nsStruct, resolvedAggRequest, {resolvedAggRequest}, privileges, snapshotCri, result);

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
