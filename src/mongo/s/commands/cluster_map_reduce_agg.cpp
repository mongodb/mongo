/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/s/commands/cluster_map_reduce_agg.h"

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/map_reduce_gen.h"
#include "mongo/db/commands/map_reduce_global_variable_scope.h"
#include "mongo/db/commands/map_reduce_out_options.h"
#include "mongo/db/commands/mr_common.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongos_process_interface.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/explain_common.h"
#include "mongo/db/query/map_reduce_output_format.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_aggregate.h"
#include "mongo/s/query/cluster_aggregation_planner.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/string_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

using sharded_agg_helpers::PipelineDataSource;

namespace {

Rarely _sampler;

auto makeExpressionContext(OperationContext* opCtx,
                           const MapReduceCommandRequest& parsedMr,
                           const ChunkManager& cm,
                           boost::optional<ExplainOptions::Verbosity> verbosity) {
    // Populate the collection UUID and the appropriate collation to use.
    auto nss = parsedMr.getNamespace();

    // An aggregation against an unsharded collection which features a $merge (i.e. a mapReduce
    // command with an out option configured to 'reduce' or 'merge') would normally need to
    // contact the primary shard to obtain the collection default collation. However, this is not
    // necessary for mapReduce commands because we will always be merging on the _id field. As such,
    // the collection default collation has no impact on the selection of fields to merge on.
    const auto requiresCollationForParsingUnshardedAggregate = false;
    auto collationObj =
        cluster_aggregation_planner::getCollation(opCtx,
                                                  cm,
                                                  nss,
                                                  parsedMr.getCollation().get_value_or(BSONObj()),
                                                  requiresCollationForParsingUnshardedAggregate);

    std::unique_ptr<CollatorInterface> resolvedCollator;
    if (!collationObj.isEmpty()) {
        // This will be null if attempting to build an interface for the simple collator.
        resolvedCollator = uassertStatusOK(
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collationObj));
    }

    // Resolve involved namespaces.
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces.try_emplace(nss.coll(), nss, std::vector<BSONObj>{});
    if (parsedMr.getOutOptions().getOutputType() != OutputType::InMemory) {
        auto outNss = parsedMr.getOutOptions().getDatabaseName()
            ? NamespaceStringUtil::deserialize(boost::none,
                                               *(parsedMr.getOutOptions().getDatabaseName()),
                                               parsedMr.getOutOptions().getCollectionName(),
                                               SerializationContext::stateDefault())
            : NamespaceStringUtil::deserialize(parsedMr.getNamespace().dbName(),
                                               parsedMr.getOutOptions().getCollectionName());
        resolvedNamespaces.try_emplace(outNss.coll(), outNss, std::vector<BSONObj>{});
    }
    auto runtimeConstants = Variables::generateRuntimeConstants(opCtx);
    if (parsedMr.getScope()) {
        runtimeConstants.setJsScope(parsedMr.getScope()->getObj());
    }
    runtimeConstants.setIsMapReduce(true);
    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx,
        verbosity,
        false,  // fromMongos
        false,  // needsMerge
        true,   // allowDiskUse
        parsedMr.getBypassDocumentValidation().get_value_or(false),
        true,  // isMapReduceCommand
        nss,
        runtimeConstants,
        std::move(resolvedCollator),
        std::make_shared<MongosProcessInterface>(
            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor()),
        std::move(resolvedNamespaces),
        boost::none,  // uuid
        boost::none,
        false  // mayDbProfile: false because mongos has no profile collection.
    );
    expCtx->inMongos = true;
    if (!cm.hasRoutingTable() && collationObj.isEmpty()) {
        expCtx->setIgnoreCollator();
    }
    return expCtx;
}

Document serializeToCommand(BSONObj originalCmd,
                            const MapReduceCommandRequest& parsedMr,
                            Pipeline* pipeline) {
    MutableDocument translatedCmd;

    translatedCmd["aggregate"] = Value(parsedMr.getNamespace().coll());
    translatedCmd[AggregateCommandRequest::kPipelineFieldName] = Value(pipeline->serialize());
    translatedCmd[AggregateCommandRequest::kCursorFieldName] =
        Value(Document{{"batchSize", std::numeric_limits<long long>::max()}});
    translatedCmd[AggregateCommandRequest::kAllowDiskUseFieldName] = Value(true);
    translatedCmd[AggregateCommandRequest::kFromMongosFieldName] = Value(true);
    translatedCmd[AggregateCommandRequest::kLetFieldName] = Value(
        pipeline->getContext()->variablesParseState.serialize(pipeline->getContext()->variables));
    translatedCmd[AggregateCommandRequest::kIsMapReduceCommandFieldName] = Value(true);

    if (shouldBypassDocumentValidationForCommand(originalCmd)) {
        translatedCmd[bypassDocumentValidationCommandOption()] = Value(true);
    }

    if (originalCmd[AggregateCommandRequest::kCollationFieldName]) {
        translatedCmd[AggregateCommandRequest::kCollationFieldName] =
            Value(originalCmd[AggregateCommandRequest::kCollationFieldName]);
    }

    // Append generic command options.
    for (const auto& elem : CommandHelpers::appendGenericCommandArgs(originalCmd, BSONObj())) {
        translatedCmd[elem.fieldNameStringData()] = Value(elem);
    }
    return translatedCmd.freeze();
}

}  // namespace

bool runAggregationMapReduce(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             const BSONObj& cmd,
                             BSONObjBuilder& result,
                             boost::optional<ExplainOptions::Verbosity> verbosity) {
    auto parsedMr =
        MapReduceCommandRequest::parse(IDLParserContext("mapReduce",
                                                        false /* apiStrict */,
                                                        auth::ValidatedTenancyScope::get(opCtx),
                                                        dbName.tenantId(),
                                                        SerializationContext::stateDefault()),
                                       cmd);
    stdx::unordered_set<NamespaceString> involvedNamespaces{parsedMr.getNamespace()};
    auto resolvedOutNss = parsedMr.getOutOptions().getDatabaseName()
        ? NamespaceStringUtil::deserialize(boost::none,
                                           *(parsedMr.getOutOptions().getDatabaseName()),
                                           parsedMr.getOutOptions().getCollectionName(),
                                           SerializationContext::stateDefault())
        : NamespaceStringUtil::deserialize(parsedMr.getNamespace().dbName(),
                                           parsedMr.getOutOptions().getCollectionName());

    if (_sampler.tick()) {
        LOGV2_WARNING(5725800,
                      "The map reduce command is deprecated. For more information, see "
                      "https://docs.mongodb.com/manual/core/map-reduce/");
    }

    if (parsedMr.getOutOptions().getOutputType() != OutputType::InMemory) {
        involvedNamespaces.insert(resolvedOutNss);
    }

    auto cri = uassertStatusOK(
        sharded_agg_helpers::getExecutionNsRoutingInfo(opCtx, parsedMr.getNamespace()));
    auto expCtx = makeExpressionContext(opCtx, parsedMr, cri.cm, verbosity);

    const auto pipelineBuilder = [&]() {
        return map_reduce_common::translateFromMR(parsedMr, expCtx);
    };

    auto namespaces =
        ClusterAggregate::Namespaces{parsedMr.getNamespace(), parsedMr.getNamespace()};

    // Auth has already been checked for the original mapReduce command, no need to recheck here.
    PrivilegeVector privileges;

    // This holds the raw results from the aggregation, which will be reformatted to match the
    // expected mapReduce output.
    BSONObjBuilder tempResults;

    auto targeter =
        cluster_aggregation_planner::AggregationTargeter::make(opCtx,
                                                               pipelineBuilder,
                                                               cri,
                                                               PipelineDataSource::kNormal,
                                                               false);  // perShardCursor
    try {
        switch (targeter.policy) {
            case cluster_aggregation_planner::AggregationTargeter::TargetingPolicy::
                kMongosRequired: {
                // Pipelines generated from mapReduce should never be required to run on mongos.
                MONGO_UNREACHABLE_TASSERT(31291);
            }
            case cluster_aggregation_planner::AggregationTargeter::TargetingPolicy::kAnyShard: {
                if (verbosity) {
                    explain_common::generateServerInfo(&result);
                    explain_common::generateServerParameters(opCtx, &result);
                }
                auto serialized = serializeToCommand(cmd, parsedMr, targeter.pipeline.get());
                // When running explain, we don't explicitly pass the specified verbosity here
                // because each stage of the constructed pipeline is aware of said verbosity through
                // a pointer to the constructed ExpressionContext.
                uassertStatusOK(cluster_aggregation_planner::dispatchPipelineAndMerge(
                    opCtx,
                    std::move(targeter),
                    std::move(serialized),
                    std::numeric_limits<long long>::max(),
                    namespaces,
                    privileges,
                    &tempResults,
                    PipelineDataSource::kNormal,
                    expCtx->eligibleForSampling()));
                break;
            }

            case cluster_aggregation_planner::AggregationTargeter::TargetingPolicy::
                kSpecificShardOnly: {
                // It should not be possible to pass $_passthroughToShard to a map reduce command.
                MONGO_UNREACHABLE_TASSERT(6273805);
            }
        }
    } catch (DBException& e) {
        uassert(ErrorCodes::CommandNotSupportedOnView,
                "mapReduce on a view is not supported",
                e.code() != ErrorCodes::CommandOnShardedViewNotSupportedOnMongod);

        e.addContext("MapReduce internal error xxx");
        throw;
    }
    auto aggResults = tempResults.done();

    // If explain() was run, we simply append the output to result.
    if (verbosity) {
        map_reduce_output_format::appendExplainResponse(result, aggResults);
    } else if (parsedMr.getOutOptions().getOutputType() == OutputType::InMemory) {
        // If the inline results could not fit into a single batch, then kill the remote
        // operation(s) and return an error since mapReduce does not support a cursor-style
        // response.
        if (aggResults["cursor"]["id"].Long() != 0) {
            uassertStatusOK(Grid::get(opCtx)->getCursorManager()->killCursor(
                opCtx, aggResults["cursor"]["id"].Long()));
            uasserted(31301, "MapReduce inline results are greater than the allowed 16MB limit");
        }

        auto exhaustedResults = [&]() {
            BSONArrayBuilder bab;
            for (auto&& elem : aggResults["cursor"]["firstBatch"].Obj())
                bab.append(elem.embeddedObject());
            return bab.arr();
        }();
        map_reduce_output_format::appendInlineResponse(std::move(exhaustedResults), &result);
    } else {
        map_reduce_output_format::appendOutResponse(parsedMr.getOutOptions().getDatabaseName(),
                                                    parsedMr.getOutOptions().getCollectionName(),
                                                    &result);
    }

    return true;
}

}  // namespace mongo
