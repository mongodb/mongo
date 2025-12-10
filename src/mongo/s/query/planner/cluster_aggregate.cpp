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


#include "mongo/s/query/planner/cluster_aggregate.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/cluster_ddl.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_hint_translation.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/desugarer.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_diagnostic_printer.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongos_process_interface.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/search/search_helper_bson_obj.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_docs_needed_bounds.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/explain_common.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_stats/agg_key.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/shard_key_diagnostic_printer.h"
#include "mongo/db/query/tailable_mode_gen.h"
#include "mongo/db/query/util/retry.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/sharding_environment/client/num_hosts_targeted_metrics.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/version_context.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/query/exec/collect_query_stats_mongos.h"
#include "mongo/s/query/planner/cluster_aggregation_planner.h"
#include "mongo/s/transaction_router.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_set.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

constexpr unsigned ClusterAggregate::kMaxViewRetries;
using sharded_agg_helpers::PipelineDataSource;

namespace {
// Ticks for server-side Javascript deprecation log messages.
Rarely _samplerAccumulatorJs, _samplerFunctionJs;

// "Resolve" involved namespaces into a map. We won't try to execute anything on a mongos, but we
// still have to populate this map so that any $lookups, etc. will be able to have a resolved view
// definition. It's okay that this is incorrect, we will repopulate the real namespace map on the
// mongod. Note that this function must be called before forwarding an aggregation command on an
// unsharded collection, in order to verify that the involved namespaces are allowed to be sharded.
auto resolveInvolvedNamespaces(const stdx::unordered_set<NamespaceString>& involvedNamespaces) {
    ResolvedNamespaceMap resolvedNamespaces;
    for (auto&& nss : involvedNamespaces) {
        resolvedNamespaces.try_emplace(nss, nss, std::vector<BSONObj>{});
    }
    return resolvedNamespaces;
}

Document serializeForPassthrough(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 const AggregateCommandRequest& request,
                                 const NamespaceString& executionNs) {
    auto req = request;

    // Reset all generic arguments besides those needed for the aggregation itself.
    // Other generic arguments that need to be set like txnNumber, lsid, etc. will be attached
    // later.
    // TODO: SERVER-90827 Only reset arguments not suitable for passing through to shards.
    auto maxTimeMS = req.getMaxTimeMS();
    auto readConcern = req.getReadConcern();
    auto writeConcern = req.getWriteConcern();
    auto rawData = req.getRawData();
    req.setGenericArguments({});
    req.setMaxTimeMS(maxTimeMS);
    req.setReadConcern(std::move(readConcern));
    req.setWriteConcern(std::move(writeConcern));
    req.setRawData(rawData);
    aggregation_request_helper::addQuerySettingsToRequest(req, expCtx);

    auto cmdObj =
        isRawDataOperation(expCtx->getOperationContext()) && req.getNamespace() != executionNs
        ? rewriteCommandForRawDataOperation<AggregateCommandRequest>(req.toBSON(),
                                                                     executionNs.coll())
        : req.toBSON();

    return Document(cmdObj);
}

// Build an appropriate ExpressionContext for the pipeline. This helper instantiates an appropriate
// collator, creates a MongoProcessInterface for use by the pipeline's stages, and sets the
// collection UUID if provided.
boost::intrusive_ptr<ExpressionContext> makeExpressionContext(
    OperationContext* opCtx,
    const AggregateCommandRequest& request,
    const boost::optional<CollectionRoutingInfo>& cri,
    const NamespaceString& executionNs,
    const NamespaceString& requestNs,
    BSONObj collationObj,
    boost::optional<UUID> uuid,
    ResolvedNamespaceMap resolvedNamespaces,
    bool hasChangeStream,
    boost::optional<ExplainOptions::Verbosity> verbosity,
    ExpressionContextCollationMatchesDefault collationMatchesDefault) {

    std::unique_ptr<CollatorInterface> collation;
    if (!collationObj.isEmpty()) {
        // This will be null if attempting to build an interface for the simple collator.
        collation = uassertStatusOK(
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collationObj));
    }

    // Create the expression context, and set 'inRouter' to true. We explicitly do *not* set
    // mergeCtx->tempDir.
    const bool canBeRejected = query_settings::canPipelineBeRejected(request.getPipeline());
    auto mergeCtx = ExpressionContextBuilder{}
                        .fromRequest(opCtx, request)
                        .explain(verbosity)
                        .collator(std::move(collation))
                        .mongoProcessInterface(std::make_shared<MongosProcessInterface>(
                            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor()))
                        .resolvedNamespace(std::move(resolvedNamespaces))
                        .originalNs(requestNs)
                        .mayDbProfile(true)
                        .inRouter(true)
                        .collUUID(uuid)
                        .canBeRejected(canBeRejected)
                        .collationMatchesDefault(collationMatchesDefault)
                        .build();

    if (!(cri && cri->hasRoutingTable()) && collationObj.isEmpty()) {
        mergeCtx->setIgnoreCollator();
    }

    // Serialize the 'AggregateCommandRequest' and save it so that the original command can be
    // reconstructed for dispatch to a new shard, which is sometimes necessary for change streams
    // pipelines.
    if (hasChangeStream) {
        mergeCtx->setOriginalAggregateCommand(
            serializeForPassthrough(mergeCtx, request, executionNs).toBson());
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
                                const boost::optional<CollectionRoutingInfo>& cri,
                                const stdx::unordered_set<NamespaceString>& involvedNamespaces) {
    if (!cri)
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

        if (cri->isSharded()) {
            std::set<ShardId> shardIdsForNs;
            // Note: It is fine to use 'getAllShardIds_UNSAFE_NotPointInTime' here because the
            // result is only used to update stats.
            cri->getChunkManager().getAllShardIds_UNSAFE_NotPointInTime(&shardIdsForNs);
            for (const auto& shardId : shardIdsForNs) {
                shardsIds.insert(shardId);
            }
        }

        for (const auto& nss : involvedNamespaces) {
            if (nss == executionNss || nss.isCollectionlessAggregateNS())
                continue;

            // We acquire CRIs for each involved nss through the RoutingContext here, and will rely
            // on whatever info is in the cached routing tables regardless of staleness. It is okay
            // that we haven't validated the tables authoritatively against the shards because this
            // function is only involved in metric tracking, but not query correctness, so it is
            // permissible to see stale info.
            auto resolvedNsCtx = uassertStatusOK(getRoutingContextForTxnCmd(opCtx, {nss}));
            const auto& resolvedNsCri = resolvedNsCtx->getCollectionRoutingInfo(nss);
            if (resolvedNsCri.isSharded()) {
                std::set<ShardId> shardIdsForNs;
                // Note: It is fine to use 'getAllShardIds_UNSAFE_NotPointInTime' here because the
                // result is only used to update stats.
                resolvedNsCri.getChunkManager().getAllShardIds_UNSAFE_NotPointInTime(
                    &shardIdsForNs);
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

    uassert(51028, "Cannot specify exchange option to a router", !request.getExchange());
    uassert(51143,
            "Cannot specify runtime constants option to a router",
            !request.getLegacyRuntimeConstants());
    uassert(51089,
            str::stream() << "Internal parameter(s) ["
                          << AggregateCommandRequest::kNeedsMergeFieldName << ", "
                          << AggregateCommandRequest::kFromRouterFieldName
                          << "] cannot be set to 'true' when sent to router",
            !request.getNeedsMerge() && !aggregation_request_helper::getFromRouter(request));
    uassert(ErrorCodes::BadValue,
            "Aggregate queries on router may not request or provide a resume token",
            !request.getRequestResumeToken() && !request.getResumeAfter() && !request.getStartAt());
}

/**
 * Appends the give 'bucketMaxSpanSeconds' attribute to the given object buidler for a timeseries
 * unpack stage. 'tsFields' will contain values from the catalog if they were found. Otherwise, we
 * will use the value produced by the kickback exception.
 */
void appendBucketMaxSpan(BSONObjBuilder& bob,
                         const TypeCollectionTimeseriesFields* tsFields,
                         BSONElement oldMaxSpanSeconds) {
    if (tsFields) {
        int32_t bucketSpan = 0;
        auto maxSpanSeconds = tsFields->getBucketMaxSpanSeconds();
        auto granularity = tsFields->getGranularity();
        if (maxSpanSeconds) {
            bucketSpan = *maxSpanSeconds;
        } else {
            bucketSpan = timeseries::getMaxSpanSecondsFromGranularity(
                granularity.get_value_or(BucketGranularityEnum::Seconds));
        }
        bob.append(DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds, bucketSpan);
    } else {
        bob.append(oldMaxSpanSeconds);
    }
}

/**
 * Rebuilds the pipeline and possibly updating the granularity value for the 'bucketMaxSpanSeconds'
 * field in the $_internalUnpackBucket stage, since it may be out of date if a 'collMod' operation
 * changed it after the DB primary threw the kickback exception. Also removes the
 * 'usesExtendedRange' field altogether, since an accurate value for this can only be known on the
 * mongod for the respective shard.
 */
std::vector<BSONObj> patchPipelineForTimeSeriesQuery(
    const std::vector<BSONObj>& pipeline, const TypeCollectionTimeseriesFields* tsFields) {

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
                    appendBucketMaxSpan(newOptions, tsFields, elem);
                } else if (elem.fieldNameStringData() ==
                           DocumentSourceInternalUnpackBucket::kUsesExtendedRange) {
                    // Omit so that target shard can amend with correct value.
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
std::unique_ptr<Pipeline> parsePipelineAndRegisterQueryStats(
    OperationContext* opCtx,
    const ClusterAggregate::Namespaces& nsStruct,
    AggregateCommandRequest& request,
    const LiteParsedPipeline& liteParsedPipeline,
    boost::optional<CollectionRoutingInfo> cri,
    bool hasChangeStream,
    bool shouldDoFLERewrite,
    bool requiresCollationForParsingUnshardedAggregate,
    boost::optional<ResolvedView> resolvedView,
    boost::optional<AggregateCommandRequest> originalRequest,
    boost::optional<ExplainOptions::Verbosity> verbosity) {
    // Populate the collation. If this is a change stream, take the user-defined collation if one
    // exists, or an empty BSONObj otherwise. Change streams never inherit the collection's default
    // collation, and since collectionless aggregations generally run on the 'admin'
    // database, the standard logic would attempt to resolve its non-existent UUID and
    // collation by sending a specious 'listCollections' command to the config servers.
    auto [collationObj, collationMatchesDefault] = hasChangeStream
        ? std::pair(request.getCollation().value_or(BSONObj()),
                    ExpressionContextCollationMatchesDefault::kYes)
        : cluster_aggregation_planner::getCollation(opCtx,
                                                    cri,
                                                    nsStruct.executionNss,
                                                    request.getCollation().value_or(BSONObj()),
                                                    requiresCollationForParsingUnshardedAggregate);

    // Build an ExpressionContext for the pipeline. This instantiates an appropriate collator,
    // includes all involved namespaces, and creates a shared MongoProcessInterface for use by
    // the pipeline's stages.
    boost::intrusive_ptr<ExpressionContext> expCtx =
        makeExpressionContext(opCtx,
                              request,
                              cri,
                              nsStruct.executionNss,
                              nsStruct.requestedNss,
                              collationObj,
                              boost::none /* uuid */,
                              resolveInvolvedNamespaces(liteParsedPipeline.getInvolvedNamespaces()),
                              hasChangeStream,
                              verbosity,
                              collationMatchesDefault);

    // If the routing table exists, then the collection is tracked in the router role and we can
    // validate if it is timeseries. If the collection is untracked, this validation will happen in
    // the shard role.
    if (request.getIsHybridSearch() && cri && cri->hasRoutingTable()) {
        uassert(10557300,
                "$rankFusion and $scoreFusion are unsupported on timeseries collections",
                !(cri->getChunkManager().isTimeseriesCollection()));
    }

    if (resolvedView && originalRequest) {
        const auto& viewName = nsStruct.requestedNss;
        // If applicable, ensure that the resolved namespace is added to the resolvedNamespaces map
        // on the expCtx before calling parseFromLiteParsed(). This is necessary for search on views
        // as parseFromLiteParsed() will first check if a view exists directly on the stage
        // specification and if none is found, will then check for the view using the expCtx. As
        // such, it's necessary to add the resolved namespace to the expCtx prior to any call to
        // parseFromLiteParsed().
        search_helpers::checkAndSetViewOnExpCtx(
            expCtx, originalRequest->getPipeline(), *resolvedView, viewName);

        if (request.getIsHybridSearch()) {
            uassert(ErrorCodes::OptionNotSupportedOnView,
                    "$rankFusion and $scoreFusion are currently unsupported on views",
                    feature_flags::gFeatureFlagSearchHybridScoringFull
                        .isEnabledUseLatestFCVWhenUninitialized(
                            VersionContext::getDecoration(opCtx),
                            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

            // This step here to add the view to the ExpressionContext ResolvedNamespaceMap exists
            // to support $unionWiths whose sub-pipelines run on a view (which is also the same view
            // as the top-level query) on a sharded cluster, where some different stage (like
            // $rankFusion or $scoreFusion) desugar into a $unionWith on the view.
            //
            // Note that there is an unintuitive expectation for any query running a $unionWith on a
            // view in a sharded cluster (regardless if the original user query contains the
            // $unionWith, or the query desugars into a $unionWith), where the view that the
            // $unionWith runs on must be in ExpressionContext ResolvedNamespace map with the
            // following mapping: {view_name -> {view_name, empty BSON pipeline}} Opposed to the
            // intuitive / expected mapping of: {view_name -> {coll_name, view BSON pipeline
            // definition}}, prior to Pipeline parsing.
            //
            // This is due to an unfortunate particular of how DocumentSourceUnionWith serializes to
            // BSON, where it always writes the 'coll' argument as the original user namespace
            // (instead of the resolved namespace, if it exists), regardless of if the internal
            // Pipeline maintained in DocumentSourceUnionWith has a resolved view definition
            // pre-pended to it or not.
            //
            // So if we were to include the "expected" mapping in the ResolvedNamespaceMap, both
            // mongos and mongod would end up prepending the view definition to the unionWith
            // sub-pipeline (as the serialized BSON of the $unionWith sent down to mongod would
            // include both the unresolved/user namespace in the 'coll' argument and the
            // sub-pipeline would already have the view definition pre-pended.
            //
            // You can observe this same insertion of {view_name -> {view_name, empty BSON
            // pipeline}} into the ResolvedNamespaceMap in PipelineBuilder::PipelineBuilder() in
            // 'sharding_catalog_manager.cpp'.
            //
            // The difference is that that the PipelineBuilder call happens before desugar, when
            // processing a LiteParsedPipeline. So this analogous call handles the cases where the
            // $unionWith appears after desugar, but placing the ExpressionContext
            // ResolvedNamespaceMap into the same required state.
            //
            // Technically, the view that the $unionWith runs on could be different from the view of
            // the top level query, however we currently don't have any stages that desugar in such
            // a way. So for now, we are gating this operation to only occur when the query is a
            // Hybrid Search, as in those desugarings the view the $unionWith will run on is
            // guaranteed to be the same as the top-level of the query.
            expCtx->addResolvedNamespace(viewName,
                                         ResolvedNamespace(viewName,
                                                           std::vector<BSONObj>(),
                                                           boost::none,
                                                           false /*involvedNamespaceIsAView*/));
        }
    }

    auto pipeline = Pipeline::parseFromLiteParsed(liteParsedPipeline, expCtx);
    if (cri && cri->hasRoutingTable()) {
        pipeline->validateWithCollectionMetadata(cri.get());
    }

    if (request.getTranslatedForViewlessTimeseries()) {
        pipeline->setTranslated();
    }

    // Compute QueryShapeHash and record it in CurOp.
    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::AggCmdShape>(
            request,
            nsStruct.executionNss,
            liteParsedPipeline.getInvolvedNamespaces(),
            *pipeline,
            expCtx);
    }};
    auto queryShapeHash = CurOp::get(opCtx)->debug().ensureQueryShapeHash(opCtx, [&]() {
        return shape_helpers::computeQueryShapeHash(expCtx, deferredShape, nsStruct.executionNss);
    });

    // Perform the query settings lookup and attach it to 'expCtx'.
    // In case query settings have already been looked up (in case the agg request is
    // running over a view) we avoid performing query settings lookup.
    expCtx->setQuerySettingsIfNotPresent(std::move(request.getQuerySettings()).value_or_eval([&] {
        auto& service = query_settings::QuerySettingsService::get(opCtx);
        return service.lookupQuerySettingsWithRejectionCheck(
            expCtx, queryShapeHash, nsStruct.executionNss);
    }));

    // Skip query stats recording for queryable encryption queries.
    if (!shouldDoFLERewrite) {
        query_stats::registerRequest(
            opCtx,
            nsStruct.executionNss,
            [&]() {
                uassertStatusOKWithContext(deferredShape->getStatus(),
                                           "Failed to compute query shape");
                return std::make_unique<query_stats::AggKey>(
                    expCtx,
                    request,
                    std::move(deferredShape->getValue()),
                    std::move(liteParsedPipeline.getInvolvedNamespaces()));
            },
            hasChangeStream);
    }

    // Find stages with stage expanders and desugar. We desugar after registering query stats to
    // ensure that the query shape is representative of the user's original query.
    Desugarer(pipeline.get())();

    return pipeline;
}

Status _parseQueryStatsAndReturnEmptyResult(
    OperationContext* opCtx,
    const Status& status,
    const ClusterAggregate::Namespaces& namespaces,
    AggregateCommandRequest& request,
    const LiteParsedPipeline& liteParsedPipeline,
    boost::optional<ResolvedView> resolvedView,
    boost::optional<AggregateCommandRequest> originalRequest,
    boost::optional<ExplainOptions::Verbosity> verbosity,
    BSONObjBuilder* result) {

    // By forcing the validation checks to be done explicitly, instead of indirectly via a callback
    // function (runAggregateImpl) in runAggregate(...) that gets passed to
    // router.routeWithRoutingContext(...), this code ensures that the router always performs
    // lite parsed pipeline validation. This is critical for $rankFusion and $scoreFusion because
    // both stages are fully desugared by the time they are sent to the shards (meaning they don't
    // contain $rankFusion/$scoreFusion DocumentSources) so the lite parsed pipeline validation
    // performed on the shards will NOT catch any validation errors. Without this explicit check,
    // it's possible for the router.routeWithRoutingContext(...) to error early before the callback
    // function, runAggregateImpl(...), is executed. The catch clause catches the error and
    // execution continues to pipeline parsing and so on. Thus, lite parsed pipeline validation
    // never happens on the sharding node for single shard/sharded cluster with unsharded collection
    // topologies.
    try {
        performValidationChecks(opCtx, request, liteParsedPipeline);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    const auto hasChangeStream = liteParsedPipeline.hasChangeStream();
    const auto shouldDoFLERewrite = request.getEncryptionInformation().has_value();
    const auto requiresCollationForParsingUnshardedAggregate =
        liteParsedPipeline.requiresCollationForParsingUnshardedAggregate();

    try {
        auto pipeline =
            parsePipelineAndRegisterQueryStats(opCtx,
                                               namespaces,
                                               request,
                                               liteParsedPipeline,
                                               boost::none /* CollectionRoutingInfo */,
                                               hasChangeStream,
                                               shouldDoFLERewrite,
                                               requiresCollationForParsingUnshardedAggregate,
                                               resolvedView,
                                               originalRequest,
                                               verbosity);

        pipeline->validateCommon(false);
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
        // Ignore redundant NamespaceNotFound errors.
        LOGV2_DEBUG(8396400,
                    4,
                    "Skipping query stats due to NamespaceNotFound",
                    "status"_attr = ex.toStatus());
    }

    // If validation is ok, just return empty result
    appendEmptyResultSetWithStatus(opCtx, namespaces.requestedNss, status, result);
    return Status::OK();
}

Status runAggregateImpl(OperationContext* opCtx,
                        RoutingContext& originalRoutingCtx,
                        ClusterAggregate::Namespaces namespaces,
                        AggregateCommandRequest& req,
                        const LiteParsedPipeline& liteParsedPipeline,
                        const PrivilegeVector& privileges,
                        boost::optional<ResolvedView> resolvedView,
                        boost::optional<AggregateCommandRequest> originalRequest,
                        boost::optional<ExplainOptions::Verbosity> verbosity,
                        BSONObjBuilder* res) {
    const auto pipelineDataSource = sharded_agg_helpers::getPipelineDataSource(liteParsedPipeline);
    if (!originalRoutingCtx.hasNss(namespaces.executionNss) &&
        sharded_agg_helpers::checkIfMustRunOnAllShards(namespaces.executionNss,
                                                       pipelineDataSource)) {
        // Verify that there are shards present in the cluster. We must do this whenever we haven't
        // acquired a routing context and all shards need to be targeted (like $changeStream), in
        // order to immediately return an empty cursor just as other aggregations do when the
        // database does not exist.
        const auto shardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
        if (shardIds.empty()) {
            // Return an empty cursor if there are no shards and we need to target all the shards.
            return _parseQueryStatsAndReturnEmptyResult(
                opCtx,
                Status{ErrorCodes::ShardNotFound, "No shards are present in the cluster"},
                namespaces,
                req,
                liteParsedPipeline,
                resolvedView,
                originalRequest,
                verbosity,
                res);
        }
    }

    boost::optional<CollectionRoutingInfoTargeter> collectionTargeter;
    auto& routingCtx = std::invoke([&]() -> RoutingContext& {
        if (originalRoutingCtx.hasNss(namespaces.executionNss)) {
            collectionTargeter = CollectionRoutingInfoTargeter(opCtx, namespaces.executionNss);
            return performTimeseriesTranslationAccordingToRoutingInfo(
                opCtx,
                namespaces.executionNss,
                *collectionTargeter,
                originalRoutingCtx,
                [&](const NamespaceString& tranlatedNss) {
                    namespaces.executionNss = tranlatedNss;
                });
        }
        return originalRoutingCtx;
    });

    // Given that in single shard/sharded cluster unsharded collection scenarios the query doesn't
    // go through retryOnViewError mongos doesn't know it's running against a view, and then it
    // passes the desugared query to the shard, so the shard knows it is running against a view but
    // it doesn't know it used to be $rankFusion/$scoreFusion. This means that we need the request
    // to persist this flag in order to do LiteParsedPipeline validation.
    if (liteParsedPipeline.hasHybridSearchStage()) {
        req.setIsHybridSearch(true);
    }

    // Start with clean `result` and `request` variables this function has been retried due to
    // collection placement changes.
    BSONObjBuilder result;
    AggregateCommandRequest request{req};

    performValidationChecks(opCtx, request, liteParsedPipeline);

    const auto isSharded = [](OperationContext* opCtx, const NamespaceString& nss) {
        // Note that if this decision is stale, and a collection has transitioned from sharded to
        // unsharded, we may uassert on valid pipelines with $out as it cannot output to sharded
        // collections.
        auto swRoutingCtx = getRoutingContextForTxnCmd(opCtx, {nss});

        // If the ns is not found we assume its unsharded. It might be implicitly created as
        // unsharded if this query does writes. An existing collection could also be concurrently
        // sharded in between here and lock acquisition elsewhere reguardless so shardedness still
        // needs to be checked after parsing too.
        if (swRoutingCtx.getStatus() == ErrorCodes::NamespaceNotFound) {
            return false;
        }
        uassertStatusOK(swRoutingCtx.getStatus());
        auto& routingCtx = swRoutingCtx.getValue();
        return routingCtx->getCollectionRoutingInfo(nss).isSharded();
    };

    const auto isExplain = request.getExplain().get_value_or(false);
    const auto hasChangeStream = liteParsedPipeline.hasChangeStream();
    const auto shouldDoFLERewrite = request.getEncryptionInformation().has_value();
    const auto requiresCollationForParsingUnshardedAggregate =
        liteParsedPipeline.requiresCollationForParsingUnshardedAggregate();
    CurOp::get(opCtx)->debug().isChangeStreamQuery = hasChangeStream;

    liteParsedPipeline.verifyIsSupported(opCtx, isSharded, isExplain);

    const auto& involvedNamespaces = liteParsedPipeline.getInvolvedNamespaces();

    const auto& cri = routingCtx.hasNss(namespaces.executionNss)
        ? boost::optional<CollectionRoutingInfo>(
              routingCtx.getCollectionRoutingInfo(namespaces.executionNss))
        : boost::none;

    // Create an RAII object that prints the collection's shard key in the case of a tassert
    // or crash.
    ScopedDebugInfo shardKeyDiagnostics(
        "ShardKeyDiagnostics",
        diagnostic_printers::ShardKeyDiagnosticPrinter{
            (cri && cri->isSharded()) ? cri->getChunkManager().getShardKeyPattern().toBSON()
                                      : BSONObj()});

    // pipelineBuilder will be invoked within AggregationTargeter::make() if and only if it chooses
    // any policy other than "specific shard only".
    auto [pipeline, expCtx] =
        [&]() -> std::tuple<std::unique_ptr<Pipeline>, boost::intrusive_ptr<ExpressionContext>> {
        auto pipeline =
            parsePipelineAndRegisterQueryStats(opCtx,
                                               namespaces,
                                               request,
                                               liteParsedPipeline,
                                               cri,
                                               hasChangeStream,
                                               shouldDoFLERewrite,
                                               requiresCollationForParsingUnshardedAggregate,
                                               resolvedView,
                                               originalRequest,
                                               verbosity);
        const boost::intrusive_ptr<ExpressionContext>& pipelineCtx = pipeline->getContext();

        // If cri is valueful, then the database definitely exists and the cluster has shards. If
        // the routing table also exists, the collection exists and is tracked in the router role,
        // so it is appropriate to attempt the translation. If any of these conditions are false,
        // then either cri is none or the routing table is absent, and the translation will either
        // be performed in the shard role or the database is nonexistent or the cluster has no
        // shards to execute the query anyway.
        const auto routingTableIsAvailable = cri && cri->hasRoutingTable();
        if (routingTableIsAvailable) {
            // The only supported rewrites are for viewless timeseries collections.
            aggregation_hint_translation::translateIndexHintIfRequired(
                pipelineCtx, cri.get(), request);
            pipeline->performPreOptimizationRewrites(pipelineCtx, cri.get());
        }

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
            CurOp::get(opCtx)->setShouldOmitDiagnosticInformation(lk, true);
        }

        pipelineCtx->initializeReferencedSystemVariables();

        // Optimize the pipeline if:
        // - We have a valid routing table.
        // - We know the collection's collation.
        // - We have a change stream.
        // - Need to do a FLE rewrite.
        // - It's a collectionless aggregation, and so a collection default collation cannot exist.
        // This is because the results of optimization may depend on knowing the collation.
        // TODO SERVER-81991: Determine whether this is necessary once all unsharded collections are
        // tracked as unsplittable collections in the sharding catalog.
        if (routingTableIsAvailable || requiresCollationForParsingUnshardedAggregate ||
            hasChangeStream || shouldDoFLERewrite ||
            pipelineCtx->getNamespaceString().isCollectionlessAggregateNS()) {
            pipeline_optimization::optimizePipeline(*pipeline);

            // Validate the pipeline post-optimization.
            const bool alreadyOptimized = true;
            pipeline->validateCommon(alreadyOptimized);
        }

        // TODO SERVER-89546: Ideally extractDocsNeededBounds should be called internally within
        // DocumentSourceSearch.
        if (search_helpers::isSearchPipeline(pipeline.get())) {
            // We won't reach this if the whole pipeline passes through with the "specific shard"
            // policy. That's okay since the shard will have access to the entire pipeline to
            // extract accurate bounds.
            auto bounds = extractDocsNeededBounds(*pipeline.get());
            auto searchStage = dynamic_cast<mongo::DocumentSourceSearch*>(pipeline->peekFront());
            searchStage->setDocsNeededBounds(bounds);
        }
        return {std::move(pipeline), pipelineCtx};
    }();

    // Create an RAII object that prints useful information about the ExpressionContext in the case
    // of a tassert or crash.
    ScopedDebugInfo expCtxDiagnostics("ExpCtxDiagnostics",
                                      diagnostic_printers::ExpressionContextPrinter{expCtx});

    auto targeter = cluster_aggregation_planner::AggregationTargeter::make(
        opCtx,
        std::move(pipeline),
        namespaces.executionNss,
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

    if (request.getExplain()) {
        explain_common::generateServerInfo(&result);
        explain_common::generateServerParameters(expCtx, &result);
    }

    // Here we modify the original 'request' object by copying the query settings from 'expCtx' into
    // it.
    //
    // In case when the original 'request' fails with the 'CommandOnShardedViewNotSupportedOnMongod'
    // exception, we retrieve the view definition and run the resolved/expanded request. The
    // resolved/expanded request must use the query settings matching the original request.
    //
    // By attaching the query settings to the original request object we can re-use the query
    // settings even though the original 'expCtx' object has been already destroyed.
    const auto& querySettings = expCtx->getQuerySettings();
    if (!query_settings::isDefault(querySettings)) {
        request.setQuerySettings(querySettings);
        req.setQuerySettings(querySettings);
    }

    // Need to explicitly assign expCtx because lambdas can't capture structured bindings.
    auto status = [&](auto& expCtx) {
        bool requestQueryStatsFromRemotes = query_stats::shouldRequestRemoteMetrics(
            CurOp::get(expCtx->getOperationContext())->debug());
        try {
            switch (targeter.policy) {
                case cluster_aggregation_planner::AggregationTargeter::TargetingPolicy::
                    kMongosRequired: {
                    routingCtx.skipValidation();
                    // If this is an explain write the explain output and return.
                    auto expCtx = targeter.pipeline->getContext();
                    if (expCtx->getExplain()) {
                        auto opts =
                            SerializationOptions{.verbosity = boost::make_optional(
                                                     ExplainOptions::Verbosity::kQueryPlanner)};
                        result << "splitPipeline" << BSONNULL << "mongos"
                               << Document{{"host",
                                            prettyHostNameAndPort(expCtx->getOperationContext()
                                                                      ->getClient()
                                                                      ->getLocalPort())},
                                           {"stages", targeter.pipeline->writeExplainOps(opts)}};
                        return Status::OK();
                    }
                    return cluster_aggregation_planner::runPipelineOnMongoS(
                        namespaces,
                        request.getCursor().getBatchSize().value_or(
                            aggregation_request_helper::kDefaultBatchSize),
                        std::move(targeter.pipeline),
                        &result,
                        privileges,
                        requestQueryStatsFromRemotes);
                }

                case cluster_aggregation_planner::AggregationTargeter::TargetingPolicy::kAnyShard: {
                    const bool eligibleForSampling = !request.getExplain();
                    return cluster_aggregation_planner::dispatchPipelineAndMerge(
                        opCtx,
                        routingCtx,
                        std::move(targeter),
                        serializeForPassthrough(expCtx, request, namespaces.executionNss),
                        request.getCursor().getBatchSize().value_or(
                            aggregation_request_helper::kDefaultBatchSize),
                        namespaces,
                        privileges,
                        &result,
                        pipelineDataSource,
                        eligibleForSampling,
                        requestQueryStatsFromRemotes);
                }
                case cluster_aggregation_planner::AggregationTargeter::TargetingPolicy::
                    kSpecificShardOnly: {
                    // Mark expCtx as tailable and await data so CCC behaves accordingly.
                    expCtx->setTailableMode(TailableModeEnum::kTailableAndAwaitData);
                    expCtx->setInRouter(false);

                    uassert(6273801,
                            "per shard cursor pipeline must contain $changeStream",
                            hasChangeStream);

                    ShardId shardId(std::string(request.getPassthroughToShard()->getShard()));

                    return cluster_aggregation_planner::runPipelineOnSpecificShardOnly(
                        expCtx,
                        routingCtx,
                        namespaces,
                        expCtx->getExplain(),
                        serializeForPassthrough(expCtx, request, namespaces.executionNss),
                        privileges,
                        shardId,
                        &result,
                        requestQueryStatsFromRemotes);
                }

                    MONGO_UNREACHABLE;
            }
            MONGO_UNREACHABLE;
        } catch (const DBException& dbe) {
            return dbe.toStatus();
        }
    }(expCtx);

    if (status.code() != ErrorCodes::CommandOnShardedViewNotSupportedOnMongod) {
        // Increment counters and set flags even in case of failed aggregate commands.
        // But for views on a sharded cluster, aggregation runs twice. First execution fails
        // because the namespace is a view, and then it is re-run with resolved view pipeline
        // and namespace.
        liteParsedPipeline.tickGlobalStageCounters();

        if (expCtx->getServerSideJsConfig().accumulator && _samplerAccumulatorJs.tick()) {
            LOGV2_WARNING(
                8996506,
                "$accumulator is deprecated. For more information, see "
                "https://www.mongodb.com/docs/manual/reference/operator/aggregation/accumulator/");
        }

        if (expCtx->getServerSideJsConfig().function && _samplerFunctionJs.tick()) {
            LOGV2_WARNING(
                8996507,
                "$function is deprecated. For more information, see "
                "https://www.mongodb.com/docs/manual/reference/operator/aggregation/function/");
        }
    }

    if (status.isOK()) {
        updateHostsTargetedMetrics(opCtx, namespaces.executionNss, cri, involvedNamespaces);
        if (expCtx->getExplain()) {
            explain_common::generateQueryShapeHash(expCtx->getOperationContext(), &result);
            // Add 'command' object to explain output. If this command was done as passthrough, it
            // will already be there.
            if (targeter.policy !=
                cluster_aggregation_planner::AggregationTargeter::kSpecificShardOnly) {
                explain_common::appendIfRoom(
                    serializeForPassthrough(expCtx, request, namespaces.requestedNss).toBson(),
                    "command",
                    &result);
            }
            collectQueryStatsMongos(opCtx,
                                    std::move(CurOp::get(opCtx)->debug().queryStatsInfo.key));
        }

        // Populate `result` and `req` once we know this function is not going to be implicitly
        // retried by the CollectionRouter.
        res->appendElementsUnique(result.done());
        req = request;
    }

    return status;
}

}  // namespace


Status ClusterAggregate::runAggregate(OperationContext* opCtx,
                                      const Namespaces& namespaces,
                                      AggregateCommandRequest& request,
                                      const PrivilegeVector& privileges,
                                      boost::optional<ExplainOptions::Verbosity> verbosity,
                                      BSONObjBuilder* result,
                                      StringData comment) {
    return runAggregate(
        opCtx, namespaces, request, {request}, privileges, verbosity, result, comment);
}

Status ClusterAggregate::runAggregate(OperationContext* opCtx,
                                      const Namespaces& namespaces,
                                      AggregateCommandRequest& request,
                                      const LiteParsedPipeline& liteParsedPipeline,
                                      const PrivilegeVector& privileges,
                                      boost::optional<ExplainOptions::Verbosity> verbosity,
                                      BSONObjBuilder* result,
                                      StringData comment) {

    const bool requiresCollectionRouter = std::invoke([&]() {
        const auto pipelineDataSource =
            sharded_agg_helpers::getPipelineDataSource(liteParsedPipeline);
        return (pipelineDataSource == PipelineDataSource::kNormal &&
                !sharded_agg_helpers::checkIfMustRunOnAllShards(namespaces.executionNss,
                                                                pipelineDataSource));
    });

    if (!requiresCollectionRouter) {
        RoutingContext emptyRoutingCtx{opCtx, std::vector<NamespaceString>{}};
        return runAggregateImpl(opCtx,
                                emptyRoutingCtx,
                                namespaces,
                                request,
                                liteParsedPipeline,
                                privileges,
                                boost::none /* resolvedView */,
                                boost::none /* originalRequest */,
                                verbosity,
                                result);
    }

    sharding::router::CollectionRouter router(opCtx, namespaces.executionNss);

    bool isExplain = verbosity.has_value();
    if (isExplain) {
        // Implicitly create the database for explain commands since, right now, there is no way
        // to respond properly when the database doesn't exist.
        // Before, the database was implicitly created by the CollectionRoutingInfoTargeter class,
        // (for context, it's a legacy class to store the routing information), now that we are
        // using the RoutingContext instead, we still need to create a database until SERVER-108882
        // gets addressed.
        // TODO (SERVER-108882) Stop creating the db once explain can be executed when th db
        // doesn't exist.
        router.createDbImplicitlyOnRoute();
    }

    // We'll use routerBodyStarted to distinguish whether an error was thrown before or after the
    // body function was executed.
    bool routerBodyStarted = false;
    auto bodyFn = [&](OperationContext* opCtx, RoutingContext& routingCtx) {
        routerBodyStarted = true;
        uassertStatusOK(runAggregateImpl(opCtx,
                                         routingCtx,
                                         namespaces,
                                         request,
                                         liteParsedPipeline,
                                         privileges,
                                         boost::none /* resolvedView */,
                                         boost::none /* originalRequest */,
                                         verbosity,
                                         result));
        return Status::OK();
    };

    // Route the command and capture the returned status.
    Status status = std::invoke([&]() -> Status {
        try {
            return router.routeWithRoutingContext(comment, bodyFn);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    });

    // Error handling for exceptions raised prior to executing the runAggregation operation.
    if (!status.isOK() && !routerBodyStarted) {
        uassert(CollectionUUIDMismatchInfo(request.getDbName(),
                                           *request.getCollectionUUID(),
                                           std::string{request.getNamespace().coll()},
                                           boost::none),
                "Database does not exist",
                status != ErrorCodes::NamespaceNotFound || !request.getCollectionUUID());

        if (liteParsedPipeline.startsWithCollStats()) {
            uassertStatusOKWithContext(status,
                                       "Unable to retrieve information for $collStats stage");
        }

        // Return an empty cursor with the given status.
        return _parseQueryStatsAndReturnEmptyResult(opCtx,
                                                    status,
                                                    namespaces,
                                                    request,
                                                    liteParsedPipeline,
                                                    boost::none /*ResolvedView*/,
                                                    boost::none /*OriginalRequest*/,
                                                    verbosity,
                                                    result);
    }

    return status;
}

Status ClusterAggregate::runAggregateWithRoutingCtx(
    OperationContext* opCtx,
    RoutingContext& routingCtx,
    const ClusterAggregate::Namespaces& namespaces,
    AggregateCommandRequest& request,
    const LiteParsedPipeline& liteParsedPipeline,
    const PrivilegeVector& privileges,
    boost::optional<ResolvedView> resolvedView,
    boost::optional<AggregateCommandRequest> originalRequest,
    boost::optional<ExplainOptions::Verbosity> verbosity,
    BSONObjBuilder* result) {

    return runAggregateImpl(opCtx,
                            routingCtx,
                            namespaces,
                            request,
                            liteParsedPipeline,
                            privileges,
                            resolvedView,
                            originalRequest,
                            verbosity,
                            result);
}

Status ClusterAggregate::retryOnViewError(OperationContext* opCtx,
                                          const AggregateCommandRequest& request,
                                          const ResolvedView& resolvedView,
                                          const NamespaceString& requestedNss,
                                          const PrivilegeVector& privileges,
                                          boost::optional<ExplainOptions::Verbosity> verbosity,
                                          BSONObjBuilder* result) {
    auto body = [&](ResolvedView& currentResolvedView) {
        auto resolvedAggRequest =
            PipelineResolver::buildRequestWithResolvedPipeline(currentResolvedView, request);

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
        nsStruct.executionNss = currentResolvedView.getNamespace();

        uassert(ErrorCodes::OptionNotSupportedOnView,
                "$rankFusion and $scoreFusion are unsupported on timeseries collections",
                !(currentResolvedView.timeseries() && request.getIsHybridSearch()));

        sharding::router::CollectionRouter router(opCtx, nsStruct.executionNss);
        router.routeWithRoutingContext(
            "ClusterAggregate::retryOnViewError",
            [&](OperationContext* opCtx, RoutingContext& routingCtx) {
                // For a sharded time-series collection, the routing is based on both routing table
                // and the bucketMaxSpanSeconds value. We need to make sure we use the
                // bucketMaxSpanSeconds of the same version as the routing table, instead of the one
                // attached in the view error. This way the shard versioning check can correctly
                // catch stale routing information.
                //
                // In addition, we should be sure to remove the 'usesExtendedRange' value from the
                // unpack stage, since the value on the target shard may be different.
                //
                // TODO SERVER-111172: Remove this timeseries specific handling after 9.0 is LTS.
                //
                // Note: this logic only needs to run for view-ful timeseries collections
                // but not viewless. There are two main cases to handle here inside
                // 'patchPipelineForTimeseriesQuery'; altering the 'bucketMaxSpanSeconds' field
                // and the altering the 'usesExtendedRange' field on the $_internalUnpackBucket
                // stage. These fields need to be altered for different reasons, but for both the
                // possibility for their incorrect value at this point in the retry loop stems from
                // them being set on the primary shard that is, for one reason or another, not
                // correct here back in the router. Viewless timeseries avoids this kickback loop
                // between the router and primary shard, and the possibility for these cases thus
                // does not arise.
                if (nsStruct.executionNss.isTimeseriesBucketsCollection()) {
                    const TypeCollectionTimeseriesFields* timeseriesFields =
                        [&]() -> const TypeCollectionTimeseriesFields* {
                        const auto& cri =
                            routingCtx.getCollectionRoutingInfo(nsStruct.executionNss);
                        if (cri.isSharded()) {
                            return cri.getChunkManager().getTimeseriesFields().get_ptr();
                        }
                        return nullptr;
                    }();

                    const auto patchedPipeline = patchPipelineForTimeSeriesQuery(
                        resolvedAggRequest.getPipeline(), timeseriesFields);
                    resolvedAggRequest.setPipeline(patchedPipeline);
                }

                uassertStatusOK(
                    runAggregateWithRoutingCtx(opCtx,
                                               routingCtx,
                                               nsStruct,
                                               resolvedAggRequest,
                                               LiteParsedPipeline(resolvedAggRequest, true),
                                               privileges,
                                               boost::make_optional(currentResolvedView),
                                               boost::make_optional(request),
                                               verbosity,
                                               result));
            });

        return Status::OK();
    };

    // If the underlying namespace was changed to a view during retry, then re-run the aggregation
    // on the new resolved namespace.
    auto onError = [&](ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& ex,
                       ResolvedView& currentResolvedView) {
        currentResolvedView = *ex.extraInfo<ResolvedView>();
    };

    return retryOnWithState<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>(
        "ClusterAggregate::retryOnViewError", resolvedView, kMaxViewRetries, body, onError);
}

}  // namespace mongo
