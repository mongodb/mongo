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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connpool.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/map_reduce_agg.h"
#include "mongo/db/commands/map_reduce_gen.h"
#include "mongo/db/commands/mr_common.h"
#include "mongo/db/pipeline/mongos_process_interface.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/map_reduce_output_format.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/commands/cluster_map_reduce_agg.h"
#include "mongo/s/query/cluster_cursor_manager.h"

namespace mongo {
namespace {

auto makeExpressionContext(OperationContext* opCtx,
                           const MapReduce& parsedMr,
                           boost::optional<CachedCollectionRoutingInfo> routingInfo) {
    // Populate the collection UUID and the appropriate collation to use.
    auto nss = parsedMr.getNamespace();
    auto [collationObj, uuid] = sharded_agg_helpers::getCollationAndUUID(
        routingInfo, nss, parsedMr.getCollation().get_value_or(BSONObj()));

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
        auto outNss = NamespaceString{parsedMr.getOutOptions().getDatabaseName()
                                          ? *parsedMr.getOutOptions().getDatabaseName()
                                          : parsedMr.getNamespace().db(),
                                      parsedMr.getOutOptions().getCollectionName()};
        resolvedNamespaces.try_emplace(outNss.coll(), outNss, std::vector<BSONObj>{});
    }
    auto runtimeConstants = Variables::generateRuntimeConstants(opCtx);
    if (parsedMr.getScope()) {
        runtimeConstants.setJsScope(parsedMr.getScope()->getObj());
    }
    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx,
        boost::none,  // explain
        false,        // fromMongos
        false,        // needsmerge
        true,         // allowDiskUse
        parsedMr.getBypassDocumentValidation().get_value_or(false),
        nss,
        runtimeConstants,
        std::move(resolvedCollator),
        std::make_shared<MongoSInterface>(),
        std::move(resolvedNamespaces),
        boost::none);  // uuid
    expCtx->inMongos = true;
    return expCtx;
}

Document serializeToCommand(BSONObj originalCmd, const MapReduce& parsedMr, Pipeline* pipeline) {
    MutableDocument translatedCmd;

    translatedCmd["aggregate"] = Value(parsedMr.getNamespace().coll());
    translatedCmd[AggregationRequest::kPipelineName] = Value(pipeline->serialize());
    translatedCmd[AggregationRequest::kCursorName] =
        Value(Document{{"batchSize", std::numeric_limits<long long>::max()}});
    translatedCmd[AggregationRequest::kAllowDiskUseName] = Value(true);
    translatedCmd[AggregationRequest::kFromMongosName] = Value(true);
    translatedCmd[AggregationRequest::kRuntimeConstants] =
        Value(pipeline->getContext()->getRuntimeConstants().toBSON());

    // Append generic command options.
    for (const auto& elem : CommandHelpers::appendPassthroughFields(originalCmd, BSONObj())) {
        translatedCmd[elem.fieldNameStringData()] = Value(elem);
    }
    return translatedCmd.freeze();
}

}  // namespace

bool runAggregationMapReduce(OperationContext* opCtx,
                             const std::string& dbname,
                             const BSONObj& cmd,
                             std::string& errmsg,
                             BSONObjBuilder& result) {
    auto parsedMr = MapReduce::parse(IDLParserErrorContext("MapReduce"), cmd);
    stdx::unordered_set<NamespaceString> involvedNamespaces{parsedMr.getNamespace()};
    auto resolvedOutNss = NamespaceString{parsedMr.getOutOptions().getDatabaseName()
                                              ? *parsedMr.getOutOptions().getDatabaseName()
                                              : parsedMr.getNamespace().db(),
                                          parsedMr.getOutOptions().getCollectionName()};

    if (parsedMr.getOutOptions().getOutputType() != OutputType::InMemory) {
        involvedNamespaces.insert(resolvedOutNss);
    }

    const auto pipelineBuilder = [&](boost::optional<CachedCollectionRoutingInfo> routingInfo) {
        return map_reduce_common::translateFromMR(
            parsedMr, makeExpressionContext(opCtx, parsedMr, routingInfo));
    };

    auto namespaces =
        ClusterAggregate::Namespaces{parsedMr.getNamespace(), parsedMr.getNamespace()};

    // Auth has already been checked for the original mapReduce command, no need to recheck here.
    PrivilegeVector privileges;

    // This holds the raw results from the aggregation, which will be reformatted to match the
    // expected mapReduce output.
    BSONObjBuilder tempResults;

    auto targeter = uassertStatusOK(
        sharded_agg_helpers::AggregationTargeter::make(opCtx,
                                                       parsedMr.getNamespace(),
                                                       pipelineBuilder,
                                                       involvedNamespaces,
                                                       false,   // hasChangeStream
                                                       true));  // allowedToPassthrough

    switch (targeter.policy) {
        case sharded_agg_helpers::AggregationTargeter::TargetingPolicy::kPassthrough: {
            // For the passthrough case, the targeter will not build a pipeline since its not needed
            // in the normal aggregation path. For this translation, though, we need to build the
            // pipeline to serialize and send to the primary shard.
            auto serialized =
                serializeToCommand(cmd, parsedMr, pipelineBuilder(targeter.routingInfo).get());
            uassertStatusOK(
                sharded_agg_helpers::runPipelineOnPrimaryShard(opCtx,
                                                               namespaces,
                                                               targeter.routingInfo->db(),
                                                               boost::none,  // explain
                                                               std::move(serialized),
                                                               privileges,
                                                               &tempResults));
            break;
        }

        case sharded_agg_helpers::AggregationTargeter::TargetingPolicy::kMongosRequired: {
            // Pipelines generated from mapReduce should never be required to run on mongos.
            uasserted(31291, "Internal error during mapReduce translation");
            break;
        }

        case sharded_agg_helpers::AggregationTargeter::TargetingPolicy::kAnyShard: {
            auto serialized = serializeToCommand(cmd, parsedMr, targeter.pipeline.get());
            uassertStatusOK(
                sharded_agg_helpers::dispatchPipelineAndMerge(opCtx,
                                                              std::move(targeter),
                                                              std::move(serialized),
                                                              std::numeric_limits<long long>::max(),
                                                              namespaces,
                                                              privileges,
                                                              &tempResults,
                                                              false));  // hasChangeStream
            break;
        }
    }

    auto aggResults = tempResults.done();
    // TODO SERVER-43290: Add support for cluster MapReduce statistics.
    if (parsedMr.getOutOptions().getOutputType() == OutputType::InMemory) {
        // If the inline results could not fit into a single batch, then kill the remote
        // operation(s) and return an error since mapReduce does not support a cursor-style
        // response.
        if (aggResults["cursor"]["id"].Long() != 0) {
            uassertStatusOK(Grid::get(opCtx)->getCursorManager()->killCursor(
                opCtx, parsedMr.getNamespace(), aggResults["cursor"]["id"].Long()));
            uasserted(31301, "MapReduce inline results are greater than the allowed 16MB limit");
        }

        auto exhaustedResults = [&]() {
            BSONArrayBuilder bab;
            for (auto&& elem : aggResults["cursor"]["firstBatch"].Obj())
                bab.append(elem.embeddedObject());
            return bab.arr();
        }();
        map_reduce_output_format::appendInlineResponse(
            std::move(exhaustedResults), MapReduceStats::createForTest(), &result);
    } else {
        map_reduce_output_format::appendOutResponse(parsedMr.getOutOptions().getDatabaseName(),
                                                    parsedMr.getOutOptions().getCollectionName(),
                                                    MapReduceStats::createForTest(),
                                                    &result);
    }

    return true;
}

}  // namespace mongo
