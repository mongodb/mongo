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

#include "mongo/db/pipeline/sharded_agg_helpers.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/global_catalog/router_role_api/collection_uuid_mismatch.h"
#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_set_variable_from_subpipeline.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/semantic_analysis.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/explain_common.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/version_context.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/query/cluster_query_knobs_gen.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/s/query/exec/document_source_merge_cursors.h"
#include "mongo/s/query/exec/establish_cursors.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <iterator>
#include <list>
#include <map>
#include <set>
#include <string>

#include <absl/container/flat_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
namespace sharded_agg_helpers {

namespace {

MONGO_FAIL_POINT_DEFINE(shardedAggregateHangBeforeEstablishingShardCursors);

struct TargetingResults {
    BSONObj shardQuery;
    BSONObj shardTargetingCollation;
    boost::optional<ShardId> mergeShardId;
    std::set<ShardId> shardIds;
    bool needsSplit;
    bool mustRunOnAllShards;
    Timestamp shardRegistryReloadTime;
};

/**
 * Given a document representing an aggregation command such as
 * {aggregate: "myCollection", pipeline: [], ...},
 *
 * produces the corresponding explain command:
 * {explain: {aggregate: "myCollection", pipeline: [], ...}, $queryOptions: {...}, verbosity: ...}
 */
Document wrapAggAsExplain(Document aggregateCommand, ExplainOptions::Verbosity verbosity) {
    MutableDocument explainCommandBuilder;
    explainCommandBuilder["explain"] = Value(aggregateCommand);
    // Downstream host targeting code expects queryOptions at the top level of the command object.
    explainCommandBuilder[query_request_helper::kUnwrappedReadPrefField] =
        Value(aggregateCommand[query_request_helper::kUnwrappedReadPrefField]);

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
    const auto& configShard =
        Grid::get(expCtx->getOperationContext())->shardRegistry()->getConfigShard();
    // Pipeline: {$changeStream: {startAtOperationTime: [now], allowToRunOnConfigDB: true}}
    AggregateCommandRequest aggReq(
        NamespaceString::kConfigsvrShardsNamespace,
        {BSON(DocumentSourceChangeStream::kStageName
              << BSON(DocumentSourceChangeStreamSpec::kStartAtOperationTimeFieldName
                      << startMonitoringAtTime
                      << DocumentSourceChangeStreamSpec::kAllowToRunOnConfigDBFieldName << true))});
    aggregation_request_helper::setFromRouter(
        VersionContext::getDecoration(expCtx->getOperationContext()), aggReq, true);
    aggReq.setNeedsMerge(true);

    SimpleCursorOptions cursor;
    cursor.setBatchSize(0);
    aggReq.setCursor(cursor);
    setReadWriteConcern(expCtx->getOperationContext(), aggReq, true, !expCtx->getExplain());
    auto configCursor = establishCursors(expCtx->getOperationContext(),
                                         expCtx->getMongoProcessInterface()->taskExecutor,
                                         aggReq.getNamespace(),
                                         ReadPreferenceSetting{ReadPreference::SecondaryPreferred},
                                         {{configShard->getId(), aggReq.toBSON()}},
                                         false);
    invariant(configCursor.size() == 1);
    return std::move(*configCursor.begin());
}

BSONObj genericTransformForShards(MutableDocument&& cmdForShards,
                                  const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  boost::optional<ExplainOptions::Verbosity> explainVerbosity,
                                  boost::optional<BSONObj> readConcern) {
    cmdForShards[AggregateCommandRequest::kLetFieldName] =
        Value(expCtx->variablesParseState.serialize(expCtx->variables));

    aggregation_request_helper::setFromRouter(
        VersionContext::getDecoration(expCtx->getOperationContext()),
        cmdForShards,
        Value(expCtx->getInRouter()));

    if (auto collationObj = expCtx->getCollatorBSON();
        !collationObj.isEmpty() && !expCtx->getIgnoreCollator()) {
        cmdForShards[AggregateCommandRequest::kCollationFieldName] = Value(collationObj);
    }

    // If this is a request for an aggregation explain, then we must wrap the aggregate inside an
    // explain command.
    if (explainVerbosity) {
        cmdForShards.reset(wrapAggAsExplain(cmdForShards.freeze(), *explainVerbosity));
    }

    if (auto txnNumber = expCtx->getOperationContext()->getTxnNumber()) {
        auto cmdTxnNumber = cmdForShards.peek()[GenericArguments::kTxnNumberFieldName];

        if (cmdTxnNumber.missing()) {
            cmdForShards[GenericArguments::kTxnNumberFieldName] =
                Value(static_cast<long long>(*expCtx->getOperationContext()->getTxnNumber()));
        } else {
            massert(8579101,
                    fmt::format("aggregate command for shards had a different value for {} than "
                                "expected (found {}, expected {})",
                                GenericArguments::kTxnNumberFieldName,
                                cmdTxnNumber.coerceToInt(),
                                *txnNumber),
                    cmdTxnNumber.coerceToInt() == *txnNumber);
        }
    }

    if (readConcern) {
        cmdForShards["readConcern"] = Value(*readConcern);
    }

    auto shardCommand = cmdForShards.freeze().toBson();
    auto filteredCommand = CommandHelpers::filterCommandRequestForPassthrough(shardCommand);

    // TODO(SERVER-108928): rawData should be declared as should_forward_to_shards: true
    // If rawData was explicitly set on the aggregate command, it will have been stripped by the
    // call to filterCommandRequestForPassthrough. For rawData operations, it will be added back
    // by the egress hook. However, if sending a rawData aggregate command from a non-rawData
    // operation, we must add it back for it to be included in the outgoing network request.
    auto cmdRawData = shardCommand.getField(GenericArguments::kRawDataFieldName);
    if (!cmdRawData.eoo()) {
        auto isRawOpCtx = isRawDataOperation(expCtx->getOperationContext());
        tassert(10892200,
                "Trying to send a non-rawData command from a rawData operation",
                cmdRawData.boolean() || !isRawOpCtx);
        if (cmdRawData.boolean() && !isRawOpCtx) {
            filteredCommand = filteredCommand.addField(cmdRawData);
        }
    }

    // Apply RW concern to the final shard command.
    return applyReadWriteConcern(expCtx->getOperationContext(),
                                 true,              /* appendRC */
                                 !explainVerbosity, /* appendWC */
                                 filteredCommand);
}

std::vector<RemoteCursor> establishShardCursors(
    OperationContext* opCtx,
    RoutingContext& routingCtx,
    std::shared_ptr<executor::TaskExecutor> executor,
    const NamespaceString& nss,
    const ReadPreferenceSetting& readPref,
    const std::vector<AsyncRequestsSender::Request>& requests,
    AsyncRequestsSender::ShardHostMap designatedHostsMap,
    bool targetAllHosts) {
    tassert(8221800, "expected at least one shard request, found: 0", !requests.empty());
    const BSONObj& cmdObj = requests.begin()->cmdObj;
    LOGV2_DEBUG(20904,
                1,
                "Dispatching command {cmdObj} to establish cursors on shards",
                "cmdObj"_attr = redact(cmdObj));

    if (MONGO_unlikely(shardedAggregateHangBeforeEstablishingShardCursors.shouldFail())) {
        LOGV2(20905,
              "shardedAggregateHangBeforeEstablishingShardCursors fail point enabled.  Blocking "
              "until fail point is disabled.");
        while (MONGO_unlikely(shardedAggregateHangBeforeEstablishingShardCursors.shouldFail())) {
            sleepsecs(1);
        }
    }

    if (targetAllHosts) {
        // If we are running on all shard servers we should never designate a particular server.
        invariant(designatedHostsMap.empty());
        std::set<ShardId> shardIds;
        for (const auto& request : requests) {
            shardIds.emplace(request.shardId);
            tassert(
                8133500,
                str::stream() << "Expected same request for each shard when targeting every shard "
                                 "server, found different requests for shards: "
                              << cmdObj << " " << request.cmdObj,
                cmdObj.binaryEqual(request.cmdObj));
        }
        return establishCursorsOnAllHosts(opCtx,
                                          routingCtx,
                                          std::move(executor),
                                          nss,
                                          shardIds,
                                          cmdObj,
                                          false,
                                          getDesiredRetryPolicy(opCtx));
    } else {
        return establishCursors(opCtx,
                                std::move(executor),
                                nss,
                                readPref,
                                requests,
                                false /* do not allow partial results */,
                                &routingCtx,
                                getDesiredRetryPolicy(opCtx),
                                {} /* providedOpKeys */,
                                std::move(designatedHostsMap));
    }
}

std::set<ShardId> getTargetedShards(boost::intrusive_ptr<ExpressionContext> expCtx,
                                    PipelineDataSource pipelineDataSource,
                                    bool mustRunOnAllShards,
                                    const boost::optional<CollectionRoutingInfo>& cri,
                                    const BSONObj shardQuery,
                                    const BSONObj collation,
                                    const boost::optional<ShardId>& mergeShardId) {
    if (mustRunOnAllShards) {
        // The pipeline begins with a stage which must be run on all shards.
        auto shardIds = Grid::get(expCtx->getOperationContext())
                            ->shardRegistry()
                            ->getAllShardIds(expCtx->getOperationContext());
        return {std::make_move_iterator(shardIds.begin()), std::make_move_iterator(shardIds.end())};
    } else if (pipelineDataSource == PipelineDataSource::kGeneratesOwnDataOnce) {
        if (mergeShardId) {
            return {*mergeShardId};
        }

        // The output collection is nonexistent or sharded, so we cannot determine a single
        // mergeShardId. Designate the dbPrimary shard to run the shards part.
        const auto dbInfo =
            uassertStatusOK(Grid::get(expCtx->getOperationContext())
                                ->catalogCache()
                                ->getDatabase(expCtx->getOperationContext(),
                                              expCtx->getNamespaceString().dbName()));
        return {dbInfo->getPrimary()};
    }

    tassert(8361100, "Need CollectionRoutingInfo to target sharded query", cri);
    return getTargetedShardsForQuery(expCtx, *cri, shardQuery, collation);
}

bool stageCanRunInParallel(const boost::intrusive_ptr<DocumentSource>& stage,
                           const OrderedPathSet& nameOfShardKeyFieldsUponEntryToStage) {
    if (stage->distributedPlanLogic()) {
        return stage->canRunInParallelBeforeWriteStage(nameOfShardKeyFieldsUponEntryToStage);
    } else {
        // This stage is fine to execute in parallel on each stream. For example, a $match can be
        // applied to each stream in parallel.
        return true;
    }
}

std::string mapToString(const StringMap<std::string>& map) {
    StringBuilder sb;
    sb << "{";
    for (auto&& entry : map) {
        if (sb.len() != 1) {
            sb << ", ";
        }
        sb << entry.first << ": " << entry.second;
    }
    sb << "}";
    return sb.str();
}

BSONObj buildNewKeyPattern(const ShardKeyPattern& shardKey, StringMap<std::string> renames) {
    BSONObjBuilder newPattern;
    for (auto&& elem : shardKey.getKeyPattern().toBSON()) {
        auto it = renames.find(elem.fieldNameStringData());
        invariant(it != renames.end(),
                  str::stream() << "Could not find new name of shard key field \""
                                << elem.fieldName() << "\": rename map was "
                                << mapToString(renames));
        newPattern.appendAs(elem, it->second);
    }
    return newPattern.obj();
}

StringMap<std::string> computeShardKeyRenameMap(const Pipeline* mergePipeline,
                                                OrderedPathSet&& pathsOfShardKey) {
    auto traversalStart = mergePipeline->getSources().crbegin();
    auto traversalEnd = mergePipeline->getSources().crend();
    const auto leadingGroup =
        dynamic_cast<DocumentSourceGroup*>(mergePipeline->getSources().front().get());
    if (leadingGroup && leadingGroup->doingMerge()) {
        // A leading $group stage will not report to preserve any fields, since it blows away the
        // _id and replaces it with something new. It possibly renames some fields, but when
        // computing the new shard key we are interested in the name of the shard key *in the middle
        // of the $group*. The $exchange will be inserted between the shard-local groups and the
        // global groups. Thus we want to exclude this stage from our rename tracking.
        traversalEnd = std::prev(traversalEnd);
    }
    auto renameMap = semantic_analysis::renamedPaths(traversalStart, traversalEnd, pathsOfShardKey);
    invariant(renameMap,
              str::stream()
                  << "Analyzed pipeline was thought to preserve the shard key fields, but did not: "
                  << Value(mergePipeline->serialize()).toString());
    return *renameMap;
}

/**
 * Returns true if any stage in the pipeline would modify any of the fields in 'shardKeyPaths', or
 * if there is any stage in the pipeline requires a unified stream to do its computation like a
 * $limit would.
 *
 * Purposefully takes 'shardKeyPaths' by value so that it can be modified throughout.
 */
bool anyStageModifiesShardKeyOrNeedsMerge(OrderedPathSet shardKeyPaths,
                                          const Pipeline* mergePipeline) {
    const auto& stages = mergePipeline->getSources();
    for (auto it = stages.crbegin(); it != stages.crend(); ++it) {
        const auto& stage = *it;
        auto renames = semantic_analysis::renamedPaths(
            shardKeyPaths, *stage, semantic_analysis::Direction::kBackward);
        if (!renames) {
            return true;
        }
        shardKeyPaths.clear();
        for (auto&& rename : *renames) {
            shardKeyPaths.insert(rename.second);
        }
        if (!stageCanRunInParallel(stage, shardKeyPaths)) {
            // In order for this stage to work it needs a single input stream which it wouldn't get
            // if we inserted an exchange before it.
            return true;
        }
    }
    return false;
}

/**
 * Returns an ordered set of shard key paths from the given shard key pattern.
 */
OrderedPathSet getShardKeyPathsSet(const ShardKeyPattern& shardKey) {
    OrderedPathSet shardKeyPaths;
    for (auto&& path : shardKey.getKeyPatternFields()) {
        shardKeyPaths.emplace(std::string{path->dottedField()});
    }

    return shardKeyPaths;
}

boost::optional<ShardedExchangePolicy> walkPipelineBackwardsTrackingShardKey(
    OperationContext* opCtx, const Pipeline* mergePipeline, const ChunkManager& chunkManager) {

    const ShardKeyPattern& shardKey = chunkManager.getShardKeyPattern();
    OrderedPathSet shardKeyPaths = getShardKeyPathsSet(shardKey);
    if (anyStageModifiesShardKeyOrNeedsMerge(shardKeyPaths, mergePipeline)) {
        return boost::none;
    }

    // All the fields of the shard key are preserved by the pipeline, but they might be renamed. To
    // set up the $exchange, we need to build a fake shard key pattern which uses the names of the
    // shard key fields as they are at the split point of the pipeline.
    auto renames = computeShardKeyRenameMap(mergePipeline, std::move(shardKeyPaths));
    ShardKeyPattern newShardKey(buildNewKeyPattern(shardKey, renames));

    // Append the boundaries with the new names from the new shard key.
    auto translateBoundary = [&renames](const BSONObj& oldBoundary) {
        BSONObjBuilder bob;
        for (auto&& elem : oldBoundary) {
            bob.appendAs(elem, renames[elem.fieldNameStringData()]);
        }
        return bob.obj();
    };

    // Given the new shard key fields, build the distribution map.
    ExchangeSpec exchangeSpec;
    std::vector<BSONObj> boundaries;
    std::vector<int> consumerIds;
    std::map<ShardId, int> shardToConsumer;
    std::vector<ShardId> consumerShards;
    int numConsumers = 0;

    // The chunk manager enumerates the chunks in the ascending order from MinKey to MaxKey. Every
    // chunk has an associated range [from, to); i.e. inclusive lower bound and exclusive upper
    // bound. The chunk ranges must cover all domain without any holes. For the exchange we coalesce
    // ranges into a single vector of points. E.g. chunks [min,5], [5,10], [10,max] will produce
    // [min,5,10,max] vector. Number of points in the vector is always one greater than number of
    // chunks.
    // We also compute consumer indices for every chunk. From the example above (3 chunks) we may
    // get the vector [0,1,2]; i.e. the first chunk goes to the consumer 0 and so on. Note that
    // the consumer id may be repeated if the consumer hosts more than 1 chunk.
    chunkManager.forEachChunk([&](const auto& chunk) {
        if (boundaries.empty())
            boundaries.emplace_back(translateBoundary(chunk.getMin()));

        boundaries.emplace_back(translateBoundary(chunk.getMax()));
        if (shardToConsumer.find(chunk.getShardId()) == shardToConsumer.end()) {
            shardToConsumer.emplace(chunk.getShardId(), numConsumers++);
            consumerShards.emplace_back(chunk.getShardId());
        }
        consumerIds.emplace_back(shardToConsumer[chunk.getShardId()]);

        return true;
    });

    exchangeSpec.setPolicy(ExchangePolicyEnum::kKeyRange);
    exchangeSpec.setKey(newShardKey.toBSON());
    exchangeSpec.setBoundaries(std::move(boundaries));
    exchangeSpec.setConsumers(shardToConsumer.size());
    exchangeSpec.setConsumerIds(std::move(consumerIds));

    return ShardedExchangePolicy{std::move(exchangeSpec), std::move(consumerShards)};
}

/** Check if the first stage of `pipeline` can execute without an attached cursor source. */
bool firstStageCanExecuteWithoutCursor(const Pipeline& pipeline) {
    if (pipeline.empty()) {
        return false;
    }
    const auto firstStage = pipeline.getSources().front().get();

    // In this helper, we expect that we are viewing the first stage of a pipeline that does
    // not yet have a mergeCursors prepended to it.
    tassert(8375100,
            "Expected pipeline without a prepended mergeCursors",
            !dynamic_cast<const DocumentSourceMergeCursors*>(firstStage));

    auto constraints = firstStage->constraints();
    if (constraints.requiresInputDocSource) {
        return false;
    }
    // Here we check the hostRequirment because there is at least one stage ($indexStats) which
    // does not require input data, but is still expected to fan out and contact remote shards
    // nonetheless.
    return constraints.hostRequirement == StageConstraints::HostTypeRequirement::kLocalOnly ||
        constraints.hostRequirement == StageConstraints::HostTypeRequirement::kRunOnceAnyNode;
}

/**
 * Given a pipeline's ShardTargetingPolicy and the NamespaceString of its expression context,
 * returns true if the pipeline is required to have a cursor source that does a local read from the
 * current process.
 */
bool isRequiredToReadLocalData(const ShardTargetingPolicy& shardTargetingPolicy,
                               const NamespaceString& ns) {
    // Certain namespaces are shard-local; that is, they exist independently on every shard. For
    // these namespaces, a local cursor should always be used.
    const bool shouldAlwaysAttachLocalCursorForNamespace = ns.isShardLocalNamespace();

    return shardTargetingPolicy == ShardTargetingPolicy::kNotAllowed ||
        shouldAlwaysAttachLocalCursorForNamespace;
}

/**
 * Given a pipeline's TargetingResults and this process' ShardId, return true if we can use a local
 * read as the cursor source for the pipeline. Also considers the readConcern of the pipeline
 * (passed as argument), vs. the readConcern of the operation the pipeline is running under
 * (obtained from the provided opCtx).
 */
bool canUseLocalReadAsCursorSource(OperationContext* opCtx,
                                   const TargetingResults& targeting,
                                   const ShardId& localShardId,
                                   boost::optional<BSONObj> readConcern) {
    // If there is no targetingCri, we can't enter the shard role correctly, so we need to
    // fallback to remote read.
    bool useLocalRead = !targeting.needsSplit && targeting.shardIds.size() == 1 &&
        *targeting.shardIds.begin() == localShardId;

    // If subpipeline has a different read concern, we need to perform remote read to
    // satisfy it.
    useLocalRead = useLocalRead &&
        (!readConcern ||
         readConcern->woCompare(repl::ReadConcernArgs::get(opCtx).toBSONInner(),
                                BSONObj{} /* ordering */,
                                BSONObj::ComparisonRules::kConsiderFieldName |
                                    BSONObj::ComparisonRules::kIgnoreFieldOrder) == 0);
    return useLocalRead;
}

/**
 * Attempts to attach a cursor source to the passed in pipeline via a local read.
 * Possibly mutates pipelineToTarget by releasing ownership to the cursor source.
 * Returns a pipeline with a cursor source attach on success. On failure, returns nullptr.
 *
 * Failure may occur before or after the pipelineToTarget is released; callers must check
 * if the pointer was released before using it again.
 */
std::unique_ptr<Pipeline> tryAttachCursorSourceForLocalRead(
    OperationContext* opCtx,
    const ExpressionContext& expCtx,
    RoutingContext& routingCtx,
    std::unique_ptr<Pipeline>& pipelineToTarget,
    const AggregateCommandRequest& aggRequest,
    bool useCollectionDefaultCollator,
    const ShardId& localShardId) {
    const auto& nss = expCtx.getNamespaceString();
    const auto& targetingCri = routingCtx.getCollectionRoutingInfo(nss);

    try {
        auto shardVersion = [&] {
            auto sv = targetingCri.hasRoutingTable() ? targetingCri.getShardVersion(localShardId)
                                                     : ShardVersion::UNSHARDED();
            if (auto txnRouter = TransactionRouter::get(opCtx);
                txnRouter && opCtx->inMultiDocumentTransaction()) {
                if (auto optOriginalPlacementConflictTime = txnRouter.getPlacementConflictTime()) {
                    sv.setPlacementConflictTime(*optOriginalPlacementConflictTime);
                }
            }
            return sv;
        }();
        ScopedSetShardRole shardRole{
            opCtx,
            nss,
            shardVersion,
            boost::optional<DatabaseVersion>{!targetingCri.hasRoutingTable(),
                                             targetingCri.getDbVersion()}};

        // Mark routing table as validated as we have "sent" the versioned command to a shard by
        // entering the shard role for a local read.
        routingCtx.onRequestSentForNss(nss);

        // TODO SERVER-77402 Wrap this in a shardRoleRetry loop instead of
        // catching exceptions. attachCursorSourceToPipelineForLocalRead enters the
        // shard role but does not refresh the shard if the shard has stale metadata.
        // Proceeding to do normal shard targeting, which will go through the
        // service_entry_point and refresh the shard if needed.
        auto pipelineWithCursor =
            expCtx.getMongoProcessInterface()->attachCursorSourceToPipelineForLocalRead(
                pipelineToTarget.release(), aggRequest, useCollectionDefaultCollator);

        LOGV2_DEBUG(5837600,
                    3,
                    "Performing local read",
                    logAttrs(expCtx.getNamespaceString()),
                    "pipeline"_attr = pipelineWithCursor->serializeForLogging(),
                    "comment"_attr = expCtx.getOperationContext()->getComment());

        return pipelineWithCursor;
    } catch (ExceptionFor<ErrorCodes::StaleDbVersion>&) {
        // The current node has stale information about this collection, proceed with
        // shard targeting, which has logic to handle refreshing that may be needed.
    } catch (ExceptionFor<ErrorCategory::StaleShardVersionError>&) {
        // The current node has stale information about this collection, proceed with
        // shard targeting, which has logic to handle refreshing that may be needed.
    } catch (ExceptionFor<ErrorCodes::CommandNotSupportedOnView>&) {
        // The current node may be trying to run a pipeline on a namespace which is an
        // unresolved view, proceed with shard targeting,
    } catch (ExceptionFor<ErrorCodes::IllegalChangeToExpectedShardVersion>&) {
    } catch (ExceptionFor<ErrorCodes::IllegalChangeToExpectedDatabaseVersion>&) {
        // The current node's shard or database version of target namespace was updated
        // mid-operation. Proceed with remote request to re-initialize operation
        // context.
    }
    return nullptr;
}
}  // namespace

std::unique_ptr<Pipeline> runPipelineDirectlyOnSingleShard(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    AggregateCommandRequest request,
    ShardId shardId,
    bool requestQueryStatsFromRemotes) {
    invariant(!request.getExplain());

    auto readPreference = uassertStatusOK(ReadPreferenceSetting::fromContainingBSON(
        request.getUnwrappedReadPref().value_or(BSONObj())));

    auto* opCtx = expCtx->getOperationContext();
    const auto& nss = request.getNamespace();

    return routing_context_utils::withValidatedRoutingContext(
        opCtx, {nss}, [&](RoutingContext& routingCtx) {
            const auto& cri = routingCtx.getCollectionRoutingInfo(nss);

            auto requests = buildVersionedRequests(expCtx, nss, cri, {shardId}, request.toBSON());
            auto cursors = establishCursors(opCtx,
                                            expCtx->getMongoProcessInterface()->taskExecutor,
                                            nss,
                                            std::move(readPreference),
                                            requests,
                                            false /* allowPartialResults */,
                                            &routingCtx,
                                            Shard::RetryPolicy::kIdempotent);
            invariant(cursors.size() == 1);

            // Convert remote cursors into a vector of "owned" cursors.
            std::vector<OwnedRemoteCursor> ownedCursors;
            for (auto&& cursor : cursors) {
                auto cursorNss = cursor.getCursorResponse().getNSS();
                ownedCursors.emplace_back(opCtx, std::move(cursor), std::move(cursorNss));
            }

            // We have not split the pipeline, and will execute entirely on the remote shard. Set up
            // an empty local pipeline which we will attach the merge cursors stage to.
            auto mergePipeline = Pipeline::parse(std::vector<BSONObj>{}, expCtx);

            partitionAndAddMergeCursorsSource(mergePipeline.get(),
                                              std::move(ownedCursors),
                                              boost::none,
                                              requestQueryStatsFromRemotes);
            return mergePipeline;
        });
}

boost::optional<ShardedExchangePolicy> checkIfEligibleForExchange(OperationContext* opCtx,
                                                                  const Pipeline* mergePipeline) {
    if (internalQueryDisableExchange.load()) {
        return boost::none;
    }

    if (mergePipeline->empty()) {
        return boost::none;
    }

    auto mergeStage = dynamic_cast<DocumentSourceMerge*>(mergePipeline->getSources().back().get());
    if (!mergeStage) {
        // If there's no $merge stage we won't try to do an $exchange. For the $out stage there's no
        // point doing an $exchange because all the writes will go to a single node, so we should
        // just perform the merge on that host.
        return boost::none;
    }

    // Aquire a RoutingContext for the output ns of the merge stage and discard it at the end of the
    // scope without validating that a versioned request was sent to the shard. This is permissible
    // because the routing table acquisition here is used for a performance optimization, not a
    // query correctness decision, so it is okay if the routing tables are stale.
    auto routingCtx =
        uassertStatusOK(getRoutingContextForTxnCmd(opCtx, {mergeStage->getOutputNs()}));
    const auto& cri = routingCtx->getCollectionRoutingInfo(mergeStage->getOutputNs());
    if (!cri.isSharded()) {
        return boost::none;
    }

    // The collection is sharded and we have a $merge stage! Here we assume the $merge stage has
    // already verified that the shard key pattern is compatible with the unique key being used.
    // Assuming this, we just have to make sure the shard key is preserved (though possibly renamed)
    // all the way to the front of the merge pipeline. If this is the case then for any document
    // entering the merging pipeline we can predict which shard it will need to end up being
    // inserted on. With this ability we can insert an exchange on the shards to partition the
    // documents based on which shard will end up owning them. Then each shard can perform a merge
    // of only those documents which belong to it (optimistically, barring chunk migrations).
    return walkPipelineBackwardsTrackingShardKey(opCtx, mergePipeline, cri.getChunkManager());
}

BSONObj createPassthroughCommandForShard(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Document serializedCommand,
    boost::optional<ExplainOptions::Verbosity> explainVerbosity,
    Pipeline* pipeline,
    boost::optional<BSONObj> readConcern,
    boost::optional<int> overrideBatchSize,
    bool requestQueryStatsFromRemotes) {
    // Create the command for the shards.
    MutableDocument targetedCmd(serializedCommand);
    if (pipeline) {
        targetedCmd[AggregateCommandRequest::kPipelineFieldName] = Value(pipeline->serialize());
    }

    if (overrideBatchSize.has_value()) {
        if (serializedCommand[AggregateCommandRequest::kCursorFieldName].missing()) {
            targetedCmd[AggregateCommandRequest::kCursorFieldName] =
                Value(DOC(SimpleCursorOptions::kBatchSizeFieldName << Value(*overrideBatchSize)));
        } else {
            targetedCmd[AggregateCommandRequest::kCursorFieldName]
                       [SimpleCursorOptions::kBatchSizeFieldName] = Value(*overrideBatchSize);
        }
    }

    if (requestQueryStatsFromRemotes) {
        targetedCmd[AggregateCommandRequest::kIncludeQueryStatsMetricsFieldName] = Value(true);
    }

    if (expCtx->isHybridSearch()) {
        targetedCmd[AggregateCommandRequest::kIsHybridSearchFieldName] = Value(true);
    }

    auto filteredCommand = genericTransformForShards(
        std::move(targetedCmd), expCtx, explainVerbosity, std::move(readConcern));

    // Request the targeted shard to gossip back the routing metadata versions for the involved
    // collections.
    if (pipeline &&
        feature_flags::gShardedAggregationCatalogCacheGossiping.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        BSONArrayBuilder arrayBuilder;
        for (const auto& nss : pipeline->getInvolvedCollections()) {
            arrayBuilder.append(
                NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
        }

        if (arrayBuilder.arrSize() > 0) {
            filteredCommand = filteredCommand.addField(
                BSON(GenericArguments::kRequestGossipRoutingCacheFieldName << arrayBuilder.arr())
                    .firstElement());
        }
    }

    return filteredCommand;
}

BSONObj createCommandForTargetedShards(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       Document serializedCommand,
                                       const SplitPipeline& splitPipeline,
                                       const boost::optional<ShardedExchangePolicy> exchangeSpec,
                                       bool needsMerge,
                                       boost::optional<ExplainOptions::Verbosity> explain,
                                       boost::optional<BSONObj> readConcern,
                                       bool requestQueryStatsFromRemotes) {
    // Create the command for the shards.
    MutableDocument targetedCmd(serializedCommand);
    // If we've parsed a pipeline on mongos, always override the pipeline, in case parsing it
    // has defaulted any arguments or otherwise changed the spec. For example, $listSessions may
    // have detected a logged in user and appended that user name to the $listSessions spec to
    // send to the shards.
    targetedCmd[AggregateCommandRequest::kPipelineFieldName] =
        Value(splitPipeline.shardsPipeline->serialize());

    if (expCtx->isHybridSearch()) {
        targetedCmd[AggregateCommandRequest::kIsHybridSearchFieldName] = Value(true);
    }

    // When running on many shards with the exchange we may not need merging.
    if (needsMerge) {
        targetedCmd[AggregateCommandRequest::kNeedsMergeFieldName] = Value(true);

        // If there aren't any stages like $out in the pipeline being sent to the shards, remove the
        // write concern. The write concern should only be applied when there are writes performed
        // to avoid mistakenly waiting for writes which didn't happen.
        const auto& shardsPipe = splitPipeline.shardsPipeline->getSources();
        if (!std::any_of(shardsPipe.cbegin(), shardsPipe.cend(), [](const auto& stage) {
                return stage->constraints().writesPersistentData();
            })) {
            targetedCmd[WriteConcernOptions::kWriteConcernField] = Value();
        }
    }

    // Indicate to the shards whether they need to produce documents with their $sortKey populated.
    // Gate this behavior on FCV of the cluster. Otherwise, we can run into a situation of a mixed
    // version cluster where a new shard sends an internal aggregation to an old shard and the old
    // shard receiving the AggregateCommandRequest does not know how to parse the 'needsSortedMerge'
    // bit.
    // Suppress clang-tidy FCV check as we don't need a feature flag to gate this behavior, it
    // represents a bug fix.
    if (serverGlobalParams.featureCompatibility.acquireFCVSnapshot().isGreaterThanOrEqualTo(
            multiversion::FeatureCompatibilityVersion::kVersion_8_3)) {  // NOLINT
        targetedCmd[AggregateCommandRequest::kNeedsSortedMergeFieldName] =
            Value(splitPipeline.shardCursorsSortSpec.has_value());
    }

    // Request the targeted shards to gossip back the routing metadata versions for the involved
    // collections.
    if (feature_flags::gShardedAggregationCatalogCacheGossiping.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        BSONArrayBuilder arrayBuilder;
        for (const auto& nss : splitPipeline.shardsPipeline->getInvolvedCollections()) {
            arrayBuilder.append(
                NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
        }

        if (arrayBuilder.arrSize() > 0) {
            targetedCmd[GenericArguments::kRequestGossipRoutingCacheFieldName] =
                Value(arrayBuilder.arr());
        }
    }

    targetedCmd[AggregateCommandRequest::kCursorFieldName] =
        Value(DOC(aggregation_request_helper::kBatchSizeField << 0));

    targetedCmd[AggregateCommandRequest::kExchangeFieldName] =
        exchangeSpec ? Value(exchangeSpec->exchangeSpec.toBSON()) : Value();

    if (requestQueryStatsFromRemotes) {
        targetedCmd[AggregateCommandRequest::kIncludeQueryStatsMetricsFieldName] = Value(true);
    }

    return genericTransformForShards(
        std::move(targetedCmd), expCtx, explain, std::move(readConcern));
}

TargetingResults targetPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                const Pipeline* pipeline,
                                PipelineDataSource pipelineDataSource,
                                ShardTargetingPolicy shardTargetingPolicy,
                                const boost::optional<CollectionRoutingInfo>& cri) {
    const bool needsRouterMerge = pipeline->needsRouterMerger();

    auto shardQuery = pipeline->getInitialQuery();

    // A $changeStream update lookup attempts to retrieve a single document by documentKey. In this
    // case, we wish to target a single shard using the simple collation, but we also want to ensure
    // that we use the collection-default collation on the shard so that the lookup can use the _id
    // index. We therefore ignore the collation on the expCtx.
    const auto& shardTargetingCollation =
        shardTargetingPolicy == ShardTargetingPolicy::kForceTargetingWithSimpleCollation
        ? CollationSpec::kSimpleSpec
        : expCtx->getCollatorBSON();

    // Determine whether we can run the entire aggregation on a single shard.
    boost::optional<ShardId> mergeShardId = pipeline->needsSpecificShardMerger();
    const bool mustRunOnAllShards =
        checkIfMustRunOnAllShards(expCtx->getNamespaceString(), pipelineDataSource);
    std::set<ShardId> shardIds = getTargetedShards(expCtx,
                                                   pipelineDataSource,
                                                   mustRunOnAllShards,
                                                   cri,
                                                   shardQuery,
                                                   shardTargetingCollation,
                                                   mergeShardId);

    bool targetAllHosts = pipeline->needsAllShardHosts();
    // Don't need to split the pipeline if we are only targeting a single shard, unless:
    // - The pipeline contains one or more stages which must always merge on router.
    // - The pipeline requires the merge to be performed on a specific shard that is not targeted.
    const bool needsSplit = (shardIds.size() > 1u) || needsRouterMerge || targetAllHosts ||
        (mergeShardId && *(shardIds.begin()) != mergeShardId);

    if (mergeShardId) {
        tassert(8561400,
                "Expected no mergeShardId, or a valid one; got " + mergeShardId->toString(),
                mergeShardId->isValid());
    }

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
    const auto currentTime = VectorClock::get(expCtx->getOperationContext())->getTime();
    auto shardRegistryReloadTime = currentTime.clusterTime().asTimestamp();
    if (pipelineDataSource == PipelineDataSource::kChangeStream) {
        Grid::get(expCtx->getOperationContext())
            ->shardRegistry()
            ->reload(expCtx->getOperationContext());
        // Rebuild the set of shards as the shard registry might have changed.
        shardIds = getTargetedShards(expCtx,
                                     pipelineDataSource,
                                     mustRunOnAllShards,
                                     cri,
                                     shardQuery,
                                     shardTargetingCollation,
                                     mergeShardId);

        // Check that no shard has been removed since the change stream open time to detect a
        // possible event loss. It is important to execute it after retrieving the most recent list
        // of shards: anyShardRemovedSince() performs a snapshot read that might miss the effects of
        // a removeShard(sId) being committed in parallel; when this happens, the change stream
        // opening is expected to fail at a later stage with a ShardNotFound error which will be
        // returned to the client; upon retry, anyShardRemovedSince() will return an accurate
        // response.
        if (expCtx->getInRouter()) {
            const auto changeStreamOpeningTime =
                ResumeToken::parse(expCtx->getInitialPostBatchResumeToken()).getData().clusterTime;
            uassert(ErrorCodes::ChangeStreamHistoryLost,
                    "Change stream events no more available due to removed shard",
                    !Grid::get(expCtx->getOperationContext())
                         ->catalogClient()
                         ->anyShardRemovedSince(expCtx->getOperationContext(),
                                                changeStreamOpeningTime));
        }
    }

    return {std::move(shardQuery),
            shardTargetingCollation,
            mergeShardId,
            std::move(shardIds),
            needsSplit,
            mustRunOnAllShards,
            shardRegistryReloadTime};
}

std::vector<AsyncRequestsSender::Request> buildShardRequests(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const boost::optional<CollectionRoutingInfo>& cri,
    const std::set<ShardId>& shardIds,
    BSONObj targetedCommand,
    PipelineDataSource pipelineDataSource,
    bool eligibleForSampling,
    bool mustRunOnAllShards,
    bool targetAllHosts,
    const stdx::unordered_map<ShardId, BSONObj>& resumeTokenMap) {
    std::vector<AsyncRequestsSender::Request> requests;
    if (pipelineDataSource == PipelineDataSource::kGeneratesOwnDataOnce) {
        tassert(10337901, "Cannot acquire a routing table for collectionless aggregate", !cri);
        tassert(10337902,
                "Shards part for collectionless aggregate must be executed on a single node",
                shardIds.size() == 1);

        // Attach the IGNORED chunk version to the command. On the shard, this will skip the actual
        // version check but will nonetheless mark the operation as versioned.
        auto versionedCmd =
            appendShardVersion(targetedCommand, ShardVersionFactory::make(ChunkVersion::IGNORED()));

        requests.reserve(shardIds.size());
        requests.emplace_back(*shardIds.begin(), std::move(versionedCmd));
    } else if (const auto needsVersionedRequests = !mustRunOnAllShards && !targetAllHosts;
               needsVersionedRequests) {
        tassert(7742700,
                "Aggregations on a real namespace should use the routing table to target "
                "shards, and should participate in the shard version protocol",
                cri);
        requests = buildVersionedRequests(expCtx,
                                          expCtx->getNamespaceString(),
                                          *cri,
                                          shardIds,
                                          targetedCommand,
                                          eligibleForSampling);

    } else {
        // The pipeline contains a stage which must be run on all shards and/or on all targeted
        // shard hosts. Skip versioning and enqueue the raw command objects.
        requests.reserve(shardIds.size());
        const auto targetedSampleId = eligibleForSampling
            ? analyze_shard_key::tryGenerateTargetedSampleId(
                  expCtx->getOperationContext(),
                  expCtx->getNamespaceString(),
                  targetedCommand.firstElementFieldNameStringData(),
                  shardIds)
            : boost::none;

        for (const ShardId& shardId : shardIds) {
            auto shardCmdObj = targetedCommand;
            if (targetedSampleId && targetedSampleId->isFor(shardId)) {
                shardCmdObj =
                    analyze_shard_key::appendSampleId(shardCmdObj, targetedSampleId->getId());
            }
            requests.emplace_back(shardId, std::move(shardCmdObj));
        }
    }

    if (!resumeTokenMap.empty()) {
        // Resume tokens are particular to a host, so it will never make sense to use them when
        // running on all shard servers.
        invariant(!targetAllHosts);
        std::vector<AsyncRequestsSender::Request> requestsWithResumeTokens;
        requestsWithResumeTokens.reserve(requests.size());
        for (const auto& request : requests) {
            auto resumeTokenIt = resumeTokenMap.find(request.shardId);
            if (resumeTokenIt == resumeTokenMap.end()) {
                requestsWithResumeTokens.emplace_back(request);
            } else {
                requestsWithResumeTokens.emplace_back(
                    request.shardId,
                    request.cmdObj.addField(BSON(AggregateCommandRequest::kResumeAfterFieldName
                                                 << resumeTokenIt->second)
                                                .firstElement()));
            }
        }
        requests.swap(requestsWithResumeTokens);
    }
    return requests;
}

DispatchShardPipelineResults dispatchTargetedShardPipeline(
    RoutingContext& routingCtx,
    Document serializedCommand,
    const TargetingResults& shardTargetingResults,
    PipelineDataSource pipelineDataSource,
    bool eligibleForSampling,
    const NamespaceString& targetedNss,
    std::unique_ptr<Pipeline> pipeline,
    boost::optional<ExplainOptions::Verbosity> explain,
    bool requestQueryStatsFromRemotes,
    boost::optional<BSONObj> readConcern,
    AsyncRequestsSender::ShardHostMap designatedHostsMap,
    stdx::unordered_map<ShardId, BSONObj> resumeTokenMap) {
    tassert(8014500, "Pipeline must be defined in order to target it", pipeline);

    const auto& [shardQuery,
                 shardTargetingCollation,
                 mergeShardId,
                 shardIds,
                 needsSplit,
                 mustRunOnAllShards,
                 shardRegistryReloadTime] = shardTargetingResults;
    const size_t shardCount = shardIds.size();

    auto expCtx = pipeline->getContext();

    // The process is as follows:
    // - First, determine whether we need to target more than one shard. If so, we split the
    // pipeline; if not, we retain the existing pipeline.
    // - Call establishShardCursors to dispatch the aggregation to the targeted shards.
    // - Stale shard version errors are thrown up to the top-level handler, causing a retry on the
    // entire aggregation command.
    auto cursors = std::vector<RemoteCursor>();
    auto shardResults = std::vector<AsyncRequestsSender::Response>();
    auto opCtx = expCtx->getOperationContext();

    boost::optional<ShardedExchangePolicy> exchangeSpec;
    boost::optional<SplitPipeline> splitPipelines;
    const bool targetAllHosts = pipeline->needsAllShardHosts();
    const auto& cri = routingCtx.hasNss(targetedNss)
        ? boost::optional<CollectionRoutingInfo>(routingCtx.getCollectionRoutingInfo(targetedNss))
        : boost::none;

    if (needsSplit) {
        LOGV2_DEBUG(20906,
                    5,
                    "Splitting pipeline: targeting = {shardIds_size} shards, needsRouterMerge = "
                    "{needsRouterMerge}, needsSpecificShardMerger = {needsSpecificShardMerger}",
                    "shardIds_size"_attr = shardCount,
                    "needsRouterMerge"_attr = pipeline->needsRouterMerger(),
                    "needsSpecificShardMerger"_attr =
                        mergeShardId.has_value() ? mergeShardId->toString() : "false");

        boost::optional<OrderedPathSet> shardKeyPaths;
        if (cri && cri->isSharded()) {
            shardKeyPaths = getShardKeyPathsSet(cri->getChunkManager().getShardKeyPattern());
        }
        splitPipelines = SplitPipeline::split(std::move(pipeline), std::move(shardKeyPaths));

        // If the first stage of the pipeline is a $search stage, exchange optimization isn't
        // possible.
        // TODO SERVER-65349 Investigate relaxing this restriction.
        if (!splitPipelines || !splitPipelines->shardsPipeline ||
            !splitPipelines->shardsPipeline->peekFront() ||
            !search_helpers::isSearchPipeline(splitPipelines->shardsPipeline.get())) {
            exchangeSpec = checkIfEligibleForExchange(opCtx, splitPipelines->mergePipeline.get());
        }
    }

    // Generate the command object for the targeted shards.
    BSONObj targetedCommand =
        (splitPipelines ? createCommandForTargetedShards(expCtx,
                                                         serializedCommand,
                                                         *splitPipelines,
                                                         exchangeSpec,
                                                         true /* needsMerge */,
                                                         explain,
                                                         std::move(readConcern),
                                                         requestQueryStatsFromRemotes)
                        : createPassthroughCommandForShard(expCtx,
                                                           serializedCommand,
                                                           explain,
                                                           pipeline.get(),
                                                           std::move(readConcern),
                                                           boost::none,
                                                           requestQueryStatsFromRemotes));
    // If there were no shards when we began execution, we wouldn't have run this aggregation in the
    // first place. Here, we double-check that the shards have not been removed mid-operation.
    uassert(ErrorCodes::ShardNotFound,
            "Unexpectedly found 0 shards while preparing to dispatch aggregation requests. Were "
            "the shards removed mid-operation?",
            shardCount > 0);

    std::vector<AsyncRequestsSender::Request> requests = buildShardRequests(expCtx,
                                                                            cri,
                                                                            shardIds,
                                                                            targetedCommand,
                                                                            pipelineDataSource,
                                                                            eligibleForSampling,
                                                                            mustRunOnAllShards,
                                                                            targetAllHosts,
                                                                            resumeTokenMap);

    const auto& readPref = ReadPreferenceSetting::get(opCtx);
    if (explain) {
        shardResults = gatherResponses(opCtx,
                                       targetedNss.dbName(),
                                       targetedNss,
                                       readPref,
                                       Shard::RetryPolicy::kIdempotent,
                                       requests,
                                       &routingCtx);
    } else {
        try {
            cursors = establishShardCursors(opCtx,
                                            routingCtx,
                                            expCtx->getMongoProcessInterface()->taskExecutor,
                                            targetedNss,
                                            readPref,
                                            requests,
                                            std::move(designatedHostsMap),
                                            targetAllHosts);
        } catch (const ExceptionFor<ErrorCodes::StaleConfig>& e) {
            // Check to see if the command failed because of a stale shard version or something
            // else.
            auto staleInfo = e.extraInfo<StaleConfigInfo>();
            tassert(6441003, "StaleConfigInfo was null during sharded aggregation", staleInfo);
            throw;
        } catch (const ExceptionFor<ErrorCodes::CollectionUUIDMismatch>& ex) {
            uassertStatusOK(populateCollectionUUIDMismatch(opCtx, ex.toStatus()));
            MONGO_UNREACHABLE_TASSERT(6487201);
        }

        tassert(7937200,
                str::stream() << "Number of cursors (" << cursors.size()
                              << ") is not a multiple of the number of targeted shards ("
                              << shardCount
                              << ") and we were not targeting each mongod in each shard",
                targetAllHosts || cursors.size() % shardCount == 0);

        // For $changeStream, we must open an extra cursor on the 'config.shards' collection, so
        // that we can monitor for the addition of new shards inline with real events.
        if (pipelineDataSource == PipelineDataSource::kChangeStream &&
            !expCtx->getNamespaceString().isEqualDb(NamespaceString::kConfigsvrShardsNamespace)) {
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
    // a specific shard, but the merging shard was not in the set of targeted shards, then we
    // must increment the number of involved shards.
    CurOp::get(opCtx)->debug().nShards =
        shardCount + (mergeShardId && !shardIds.count(*mergeShardId));

    return DispatchShardPipelineResults{std::move(mergeShardId),
                                        std::move(ownedCursors),
                                        std::move(shardResults),
                                        std::move(splitPipelines),
                                        std::move(pipeline),
                                        targetedCommand,
                                        shardCount,
                                        exchangeSpec};
}

DispatchShardPipelineResults dispatchShardPipeline(
    RoutingContext& routingCtx,
    Document serializedCommand,
    PipelineDataSource pipelineDataSource,
    bool eligibleForSampling,
    std::unique_ptr<Pipeline> pipeline,
    boost::optional<ExplainOptions::Verbosity> explain,
    const NamespaceString& targetedNss,
    bool requestQueryStatsFromRemotes,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern,
    AsyncRequestsSender::ShardHostMap designatedHostsMap,
    stdx::unordered_map<ShardId, BSONObj> resumeTokenMap,
    std::set<ShardId> shardsToSkip) {
    const auto& expCtx = pipeline->getContext();
    const auto& cri = routingCtx.hasNss(targetedNss)
        ? boost::optional<CollectionRoutingInfo>(routingCtx.getCollectionRoutingInfo(targetedNss))
        : boost::none;

    TargetingResults targeting =
        targetPipeline(expCtx, pipeline.get(), pipelineDataSource, shardTargetingPolicy, cri);
    auto& shardIds = targeting.shardIds;
    for (const auto& shard : shardsToSkip) {
        shardIds.erase(shard);
    }

    // Return if we don't need to establish any cursors.
    if (shardIds.empty()) {
        tassert(7958303,
                "Expected no merge shard id when shardIds are empty",
                !targeting.mergeShardId.has_value());
        return DispatchShardPipelineResults{
            boost::none, {}, {}, boost::none, nullptr, BSONObj(), 0, boost::none};
    }
    return dispatchTargetedShardPipeline(routingCtx,
                                         std::move(serializedCommand),
                                         targeting,
                                         pipelineDataSource,
                                         eligibleForSampling,
                                         targetedNss,
                                         std::move(pipeline),
                                         std::move(explain),
                                         requestQueryStatsFromRemotes,
                                         std::move(readConcern),
                                         std::move(designatedHostsMap),
                                         std::move(resumeTokenMap));
}

/**
 * Build the AsyncResultsMergerParams from the cursor set and sort spec.
 */
AsyncResultsMergerParams buildArmParams(boost::intrusive_ptr<ExpressionContext> expCtx,
                                        std::vector<OwnedRemoteCursor> ownedCursors,
                                        boost::optional<BSONObj> shardCursorsSortSpec,
                                        bool requestQueryStatsFromRemotes) {
    AsyncResultsMergerParams armParams;
    armParams.setSort(std::move(shardCursorsSortSpec));
    armParams.setTailableMode(expCtx->getTailableMode());
    armParams.setNss(expCtx->getNamespaceString());
    armParams.setRequestQueryStatsFromRemotes(requestQueryStatsFromRemotes);

    if (auto lsid = expCtx->getOperationContext()->getLogicalSessionId()) {
        OperationSessionInfoFromClient sessionInfo(*lsid,
                                                   expCtx->getOperationContext()->getTxnNumber());

        if (TransactionRouter::get(expCtx->getOperationContext())) {
            sessionInfo.setAutocommit(false);
        }

        armParams.setOperationSessionInfo(sessionInfo);
    }

    // Convert owned cursors into a vector of remote cursors to be transferred to the merge
    // pipeline.
    std::vector<RemoteCursor> remoteCursors;
    remoteCursors.reserve(ownedCursors.size());
    for (auto&& cursor : ownedCursors) {
        // Transfer ownership of the remote cursor to the $mergeCursors stage.
        remoteCursors.emplace_back(cursor.releaseCursor());
    }
    armParams.setRemotes(std::move(remoteCursors));

    return armParams;
}

// Anonymous namespace for helpers of partitionCursorsAndAddMergeCursors.
namespace {
/**
 * Given the owned cursors vector, partitions the cursors into either one or two vectors. If
 * untyped cursors are present, returned pair will be {results, boost::none}. If results or meta are
 * present, the returned pair will be {results, meta}.
 */
std::pair<std::vector<OwnedRemoteCursor>, boost::optional<std::vector<OwnedRemoteCursor>>>
partitionCursors(std::vector<OwnedRemoteCursor> ownedCursors) {

    // Partition cursor set based on type/label.
    std::vector<OwnedRemoteCursor> resultsCursors;
    std::vector<OwnedRemoteCursor> metaCursors;
    std::vector<OwnedRemoteCursor> untypedCursors;
    for (OwnedRemoteCursor& ownedCursor : ownedCursors) {
        auto cursor = *ownedCursor;
        auto maybeCursorType = cursor->getCursorResponse().getCursorType();
        if (!maybeCursorType) {
            untypedCursors.push_back(std::move(ownedCursor));
        } else {
            switch (*maybeCursorType) {
                case CursorTypeEnum::DocumentResult:
                    resultsCursors.push_back(std::move(ownedCursor));
                    break;
                case CursorTypeEnum::SearchMetaResult:
                    metaCursors.push_back(std::move(ownedCursor));
                    break;
            }
        }
    }

    // Verify we don't have illegal mix of types and untyped cursors from the shards.
    bool haveTypedCursors = !resultsCursors.empty() || !metaCursors.empty();
    if (haveTypedCursors) {
        tassert(625305,
                "Received unexpected mix of labelled and unlabelled cursors.",
                untypedCursors.empty());
    }

    if (haveTypedCursors) {
        return {std::move(resultsCursors), std::move(metaCursors)};
    }
    return {std::move(untypedCursors), boost::none};
}


/**
 * Adds a merge cursors stage to the pipeline for metadata cursors. Should not be called if
 * the query did not generate metadata cursors.
 */
void injectMetaCursor(Pipeline* mergePipeline,
                      std::vector<OwnedRemoteCursor> metaCursors,
                      bool requestQueryStatsFromRemotes) {
    // Provide the "meta" cursors to the $setVariableFromSubPipeline stage.
    for (const auto& source : mergePipeline->getSources()) {
        if (auto* setVarStage =
                dynamic_cast<DocumentSourceSetVariableFromSubPipeline*>(source.get())) {

            // If $setVar is present, we must have a non-empty set of "meta" cursors.
            tassert(625307, "Missing meta cursor set.", !metaCursors.empty());

            auto armParams = sharded_agg_helpers::buildArmParams(mergePipeline->getContext(),
                                                                 std::move(metaCursors),
                                                                 {},
                                                                 requestQueryStatsFromRemotes);

            setVarStage->addSubPipelineInitialSource(DocumentSourceMergeCursors::create(
                mergePipeline->getContext(), std::move(armParams)));
            break;
        }
    }
}

/**
 * Adds a mergeCursors stage to the front of the pipeline to handle merging cursors from each
 * shard.
 */
void addMergeCursorsSource(Pipeline* mergePipeline,
                           std::vector<OwnedRemoteCursor> cursorsToMerge,
                           boost::optional<BSONObj> shardCursorsSortSpec,
                           bool requestQueryStatsFromRemotes) {

    auto armParams = sharded_agg_helpers::buildArmParams(mergePipeline->getContext(),
                                                         std::move(cursorsToMerge),
                                                         std::move(shardCursorsSortSpec),
                                                         requestQueryStatsFromRemotes);

    mergePipeline->addInitialSource(
        DocumentSourceMergeCursors::create(mergePipeline->getContext(), std::move(armParams)));
}

}  // namespace

void partitionAndAddMergeCursorsSource(Pipeline* mergePipeline,
                                       std::vector<OwnedRemoteCursor> cursors,
                                       boost::optional<BSONObj> shardCursorsSortSpec,
                                       bool requestQueryStatsFromRemotes) {
    auto [resultsCursors, metaCursors] = partitionCursors(std::move(cursors));
    // Whether or not cursors are typed/untyped, the first is always the results cursor.
    addMergeCursorsSource(mergePipeline,
                          std::move(resultsCursors),
                          shardCursorsSortSpec,
                          requestQueryStatsFromRemotes);
    if (metaCursors) {
        injectMetaCursor(mergePipeline, std::move(*metaCursors), requestQueryStatsFromRemotes);
    }
}

void mergeExplainOutputFromShards(const std::vector<AsyncRequestsSender::Response>& shardResponses,
                                  BSONObjBuilder* result) {
    BSONObjBuilder shardExplains(result->subobjStart("shards"));

    for (const auto& shardResult : shardResponses) {
        uassertStatusOK(shardResult.swResponse.getStatus());
        uassertStatusOK(getStatusFromCommandResult(shardResult.swResponse.getValue().data));

        invariant(shardResult.shardHostAndPort);

        auto shardId = shardResult.shardId.toString();
        const auto& data = shardResult.swResponse.getValue().data;
        BSONObjBuilder explain;
        explain << "host" << shardResult.shardHostAndPort->toString();

        // Add the per shard explainVersion to the final explain output.
        auto explainVersion = data["explainVersion"];
        explain << "explainVersion" << explainVersion;

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
        explain_common::appendIfRoom(explain.done(), shardId, &shardExplains);
    }
}

Status appendExplainResults(DispatchShardPipelineResults&& dispatchResults,
                            const boost::intrusive_ptr<ExpressionContext>& mergeCtx,
                            BSONObjBuilder* result) {
    if (dispatchResults.splitPipeline) {
        auto* mergePipeline = dispatchResults.splitPipeline->mergePipeline.get();
        auto specificMergeShardId = dispatchResults.mergeShardId;
        auto mergeType = [&]() -> std::string {
            if (mergePipeline->canRunOnRouter().isOK() && !specificMergeShardId) {
                if (mergeCtx->getInRouter()) {
                    if (feature_flags::gFeatureFlagAggMongosToRouter.isEnabled(
                            VersionContext::getDecoration(mergeCtx->getOperationContext()),
                            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                        return "router";
                    }
                    return "mongos";
                }
                return "local";
            } else if (dispatchResults.exchangeSpec) {
                return "exchange";
            } else if (specificMergeShardId) {
                return "specificShard";
            } else {
                return "anyShard";
            }
        }();

        *result << "mergeType" << mergeType;
        if (specificMergeShardId) {
            *result << "mergeShardId" << *specificMergeShardId;
        }

        MutableDocument pipelinesDoc;
        // We specify "queryPlanner" verbosity when building the output for "shardsPart" because
        // execution stats are reported by each shard individually.
        auto opts = SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)};
        pipelinesDoc.addField(
            "shardsPart",
            Value(dispatchResults.splitPipeline->shardsPipeline->writeExplainOps(opts)));
        if (dispatchResults.exchangeSpec) {
            BSONObjBuilder bob;
            dispatchResults.exchangeSpec->exchangeSpec.serialize(&bob);
            bob.append("consumerShards", dispatchResults.exchangeSpec->consumerShards);
            pipelinesDoc.addField("exchange", Value(bob.obj()));
        }
        // We specify "queryPlanner" verbosity because execution stats are not currently
        // supported when building the output for "mergerPart".
        auto explainOps = mergePipeline->writeExplainOps(opts);

        // No cursors to remote shards are established for an explain, and the $mergeCursors
        // aggregation stage which is normally built in addMergeCursorsSource() requires vectors of
        // cursors and ShardIDs. For explain output, we construct the armParams that would normally
        // be used in the serialization of the $mergeCursors stage and add it to the serialization
        // of the pipeline.
        auto armParams =
            // Since no cursors are actually established for an explain, construct ARM params with
            // an empty vector and then remove it from the explain BSON.
            buildArmParams(dispatchResults.splitPipeline->mergePipeline->getContext(),
                           std::vector<OwnedRemoteCursor>(),
                           std::move(dispatchResults.splitPipeline->shardCursorsSortSpec),
                           false /* requestQueryStatsFromRemotes */)
                .toBSON()
                .removeField(AsyncResultsMergerParams::kRemotesFieldName);

        // See DocumentSourceMergeCursors::serialize().
        explainOps.insert(explainOps.begin(), Value(Document{{"$mergeCursors"_sd, armParams}}));

        pipelinesDoc.addField("mergerPart", Value(explainOps));

        *result << "splitPipeline" << pipelinesDoc.freeze();
    } else {
        *result << "splitPipeline" << BSONNULL;
    }

    mergeExplainOutputFromShards(dispatchResults.remoteExplainOutput, result);

    return Status::OK();
}

BSONObj targetShardsForExplain(Pipeline* ownedPipeline) {
    auto expCtx = ownedPipeline->getContext();
    std::unique_ptr<Pipeline> pipeline(ownedPipeline);
    // The pipeline is going to be explained on the shards, and we don't want to send a
    // mergeCursors stage.
    invariant(pipeline->empty() ||
              !dynamic_cast<DocumentSourceMergeCursors*>(pipeline->getSources().front().get()));
    invariant(expCtx->getExplain());

    // Generate the command object for the targeted shards.
    auto rawStages = [&pipeline]() {
        auto serialization = pipeline->serialize();
        std::vector<BSONObj> stages;
        stages.reserve(serialization.size());

        for (const auto& stageObj : serialization) {
            invariant(stageObj.getType() == BSONType::object);
            stages.push_back(stageObj.getDocument().toBson());
        }

        return stages;
    }();
    AggregateCommandRequest aggRequest(expCtx->getNamespaceString(), rawStages);

    LiteParsedPipeline liteParsedPipeline(aggRequest);
    auto hasChangeStream = liteParsedPipeline.hasChangeStream();
    auto generatesOwnDataOnce = liteParsedPipeline.generatesOwnDataOnce();
    auto pipelineDataSource = hasChangeStream ? PipelineDataSource::kChangeStream
        : generatesOwnDataOnce                ? PipelineDataSource::kGeneratesOwnDataOnce
                                              : PipelineDataSource::kNormal;

    sharding::router::CollectionRouter router(expCtx->getOperationContext()->getServiceContext(),
                                              expCtx->getNamespaceString());
    aggregation_request_helper::addQuerySettingsToRequest(aggRequest, expCtx);
    return router.routeWithRoutingContext(
        expCtx->getOperationContext(),
        "collecting explain from shards"_sd,
        [&](OperationContext* opCtx, RoutingContext& routingCtx) {
            auto shardDispatchResults =
                dispatchShardPipeline(routingCtx,
                                      Document(aggRequest.toBSON()),
                                      pipelineDataSource,
                                      expCtx->eligibleForSampling(),
                                      pipeline->clone(),
                                      expCtx->getExplain(),
                                      expCtx->getNamespaceString(),
                                      false /* requestQueryStatsFromRemotes */);

            BSONObjBuilder explainBuilder;
            auto appendStatus =
                appendExplainResults(std::move(shardDispatchResults), expCtx, &explainBuilder);
            uassertStatusOK(appendStatus);

            return BSON("pipeline" << explainBuilder.done());
        });
}

Shard::RetryPolicy getDesiredRetryPolicy(OperationContext* opCtx) {
    // The idempotent retry policy will retry even for writeConcern failures, so only set it if the
    // pipeline does not support writeConcern.
    if (!opCtx->getWriteConcern().usedDefaultConstructedWC) {
        return Shard::RetryPolicy::kNotIdempotent;
    }
    return Shard::RetryPolicy::kIdempotent;
}

bool checkIfMustRunOnAllShards(const NamespaceString& nss, PipelineDataSource pipelineDataSource) {
    // The following aggregations must be routed to all shards:
    // - Any collectionless aggregation, such as non-localOps $currentOp.
    // - Any aggregation which begins with a $changeStream stage.
    return pipelineDataSource != PipelineDataSource::kGeneratesOwnDataOnce &&
        (nss.isCollectionlessAggregateNS() ||
         pipelineDataSource == PipelineDataSource::kChangeStream);
}

PipelineDataSource getPipelineDataSource(const LiteParsedPipeline& liteParsedPipeline) {
    if (liteParsedPipeline.hasChangeStream()) {
        return PipelineDataSource::kChangeStream;
    }
    if (liteParsedPipeline.generatesOwnDataOnce()) {
        return PipelineDataSource::kGeneratesOwnDataOnce;
    }
    return PipelineDataSource::kNormal;
}

std::unique_ptr<Pipeline> dispatchTargetedPipelineAndAddMergeCursors(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    RoutingContext& routingCtx,
    AggregateCommandRequest aggRequest,
    std::unique_ptr<Pipeline> pipeline,
    TargetingResults targeting,
    PipelineDataSource pipelineDataSource,
    boost::optional<BSONObj> shardCursorsSortSpec,
    boost::optional<BSONObj> readConcern,
    bool requestQueryStatsFromRemotes) {
    // The default value for 'allowDiskUse' and 'maxTimeMS' in the AggregateCommandRequest may not
    // match what was set on the originating command, so copy it from the ExpressionContext.
    aggRequest.setAllowDiskUse(expCtx->getAllowDiskUse());
    if (auto maxTimeMS = expCtx->getOperationContext()->getRemainingMaxTimeMillis();
        maxTimeMS < Microseconds::max()) {
        aggRequest.setMaxTimeMS(durationCount<Milliseconds>(maxTimeMS));
    }

    aggregation_request_helper::addQuerySettingsToRequest(aggRequest, expCtx);
    auto shardDispatchResults = dispatchTargetedShardPipeline(routingCtx,
                                                              Document(aggRequest.toBSON()),
                                                              targeting,
                                                              pipelineDataSource,
                                                              expCtx->eligibleForSampling(),
                                                              expCtx->getNamespaceString(),
                                                              std::move(pipeline),
                                                              boost::none /* explain */,
                                                              requestQueryStatsFromRemotes,
                                                              readConcern,
                                                              {} /* designatedHostsMap */,
                                                              {} /* resumeTokenMap */);

    std::vector<ShardId> targetedShards;
    targetedShards.reserve(shardDispatchResults.remoteCursors.size());
    for (auto&& remoteCursor : shardDispatchResults.remoteCursors) {
        targetedShards.emplace_back(std::string{remoteCursor->getShardId()});
    }

    std::unique_ptr<Pipeline> mergePipeline;
    if (shardDispatchResults.splitPipeline) {
        mergePipeline = std::move(shardDispatchResults.splitPipeline->mergePipeline);
        if (shardDispatchResults.splitPipeline->shardCursorsSortSpec) {
            uassert(4929304, "Split pipeline provides its own sort already", !shardCursorsSortSpec);
            shardCursorsSortSpec = shardDispatchResults.splitPipeline->shardCursorsSortSpec;
        }
    } else {
        // We have not split the pipeline, and will execute entirely on the remote shards. Set up an
        // empty local pipeline which we will attach the merge cursors stage to.
        mergePipeline = Pipeline::parse(std::vector<BSONObj>(), expCtx);
    }

    partitionAndAddMergeCursorsSource(mergePipeline.get(),
                                      std::move(shardDispatchResults.remoteCursors),
                                      shardCursorsSortSpec,
                                      requestQueryStatsFromRemotes);
    return mergePipeline;
}

std::unique_ptr<Pipeline> targetShardsAndAddMergeCursors(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::variant<std::unique_ptr<Pipeline>,
                 AggregateCommandRequest,
                 std::pair<AggregateCommandRequest, std::unique_ptr<Pipeline>>> targetRequest,
    boost::optional<BSONObj> shardCursorsSortSpec,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern,
    bool useCollectionDefaultCollator) {
    auto&& aggRequestPipelinePair = [&] {
        return visit(
            OverloadedVisitor{[&](std::unique_ptr<Pipeline>&& pipeline) {
                                  return std::make_pair(
                                      AggregateCommandRequest(expCtx->getNamespaceString(),
                                                              pipeline->serializeToBson()),
                                      std::move(pipeline));
                              },
                              [&](AggregateCommandRequest&& aggRequest) {
                                  auto rawPipeline = aggRequest.getPipeline();
                                  return std::make_pair(std::move(aggRequest),
                                                        Pipeline::parse(rawPipeline, expCtx));
                              },
                              [&](std::pair<AggregateCommandRequest, std::unique_ptr<Pipeline>>&&
                                      aggRequestPipelinePair) {
                                  return std::move(aggRequestPipelinePair);
                              }},
            std::move(targetRequest));
    }();
    const auto& aggRequest = aggRequestPipelinePair.first;
    auto&& pipeline = aggRequestPipelinePair.second;

    LOGV2_DEBUG(9497004,
                5,
                "Preparing pipeline for execution",
                "pipeline"_attr = pipeline->serializeForLogging());

    if (firstStageCanExecuteWithoutCursor(*pipeline)) {
        // There's no need to attach a cursor here - the first stage provides its own data and
        // is meant to be run locally (e.g. $documents).
        return std::move(pipeline);
    }

    tassert(9597602,
            "Pipeline should not start with $mergeCursors",
            pipeline->empty() ||
                !dynamic_cast<DocumentSourceMergeCursors*>(pipeline->getSources().front().get()));

    if (isRequiredToReadLocalData(shardTargetingPolicy, expCtx->getNamespaceString())) {
        tassert(8375101,
                "Only shard role operations can perform local reads.",
                expCtx->getOperationContext()->getService()->role().has(ClusterRole::ShardServer));
        return expCtx->getMongoProcessInterface()->attachCursorSourceToPipelineForLocalRead(
            pipeline.release(), aggRequest, useCollectionDefaultCollator);
    }

    LiteParsedPipeline liteParsedPipeline(aggRequest);
    auto hasChangeStream = liteParsedPipeline.hasChangeStream();
    auto generatesOwnDataOnce = liteParsedPipeline.generatesOwnDataOnce();
    auto pipelineDataSource = hasChangeStream ? PipelineDataSource::kChangeStream
        : generatesOwnDataOnce                ? PipelineDataSource::kGeneratesOwnDataOnce
                                              : PipelineDataSource::kNormal;

    // We're not required to read locally, and we need a cursor source. We need to perform routing
    // to see what shard(s) the pipeline targets.
    sharding::router::CollectionRouter router(expCtx->getOperationContext()->getServiceContext(),
                                              expCtx->getNamespaceString());
    return router.routeWithRoutingContext(
        expCtx->getOperationContext(),
        "targeting pipeline to attach cursors"_sd,
        [&](OperationContext* opCtx, RoutingContext& routingCtx) {
            auto pipelineToTarget = pipeline->clone();
            auto pipelineTargetingInfo =
                targetPipeline(expCtx,
                               pipelineToTarget.get(),
                               pipelineDataSource,
                               shardTargetingPolicy,
                               routingCtx.getCollectionRoutingInfo(expCtx->getNamespaceString()));

            const auto localShardId = expCtx->getMongoProcessInterface()->getShardId(opCtx);
            if (localShardId &&
                canUseLocalReadAsCursorSource(
                    opCtx, pipelineTargetingInfo, *localShardId, readConcern)) {
                if (auto pipelineWithCursor =
                        tryAttachCursorSourceForLocalRead(opCtx,
                                                          *expCtx,
                                                          routingCtx,
                                                          pipelineToTarget,
                                                          aggRequest,
                                                          useCollectionDefaultCollator,
                                                          *localShardId)) {
                    return pipelineWithCursor;
                }

                // Recreate 'pipelineToTarget' in case it was consumed by
                // tryAttachCursorSourceForLocalRead().
                if (!pipelineToTarget) {
                    pipelineToTarget = pipeline->clone();
                }
            }

            bool requestQueryStatsFromRemotes = query_stats::shouldRequestRemoteMetrics(
                CurOp::get(expCtx->getOperationContext())->debug());

            return dispatchTargetedPipelineAndAddMergeCursors(expCtx,
                                                              routingCtx,
                                                              aggRequest,
                                                              std::move(pipelineToTarget),
                                                              std::move(pipelineTargetingInfo),
                                                              pipelineDataSource,
                                                              std::move(shardCursorsSortSpec),
                                                              std::move(readConcern),
                                                              requestQueryStatsFromRemotes);
        });
}

std::unique_ptr<Pipeline> preparePipelineForExecution(Pipeline* ownedPipeline,
                                                      ShardTargetingPolicy shardTargetingPolicy,
                                                      boost::optional<BSONObj> readConcern) {
    auto expCtx = ownedPipeline->getContext();
    std::unique_ptr<Pipeline> pipeline(ownedPipeline);
    return targetShardsAndAddMergeCursors(expCtx,
                                          std::move(pipeline),
                                          boost::none /* shardCursorsSortSpec */,
                                          shardTargetingPolicy,
                                          readConcern);
}
}  // namespace sharded_agg_helpers
}  // namespace mongo
