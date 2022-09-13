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

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <cstdint>
#include <string>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/map_reduce_agg.h"
#include "mongo/db/commands/map_reduce_javascript_code.h"
#include "mongo/db/commands/mr_common.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/disk_use_options_gen.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/plan_executor_pipeline.h"
#include "mongo/db/query/explain_common.h"
#include "mongo/db/query/map_reduce_output_format.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo::map_reduce_agg {

namespace {
Rarely _sampler;

auto makeExpressionContext(OperationContext* opCtx,
                           const MapReduceCommandRequest& parsedMr,
                           boost::optional<ExplainOptions::Verbosity> verbosity) {
    // AutoGetCollectionForReadCommand will throw if the sharding version for this connection is
    // out of date.
    AutoGetCollectionForReadCommandMaybeLockFree ctx(
        opCtx, parsedMr.getNamespace(), AutoGetCollectionViewMode::kViewsPermitted);
    uassert(ErrorCodes::CommandNotSupportedOnView,
            "mapReduce on a view is not supported",
            !ctx.getView());

    auto [resolvedCollator, _] = PipelineD::resolveCollator(
        opCtx, parsedMr.getCollation().get_value_or(BSONObj()), ctx.getCollection());

    // The UUID of the collection for the execution namespace of this aggregation.
    auto uuid =
        ctx.getCollection() ? boost::make_optional(ctx.getCollection()->uuid()) : boost::none;

    auto runtimeConstants = Variables::generateRuntimeConstants(opCtx);
    if (parsedMr.getScope()) {
        runtimeConstants.setJsScope(parsedMr.getScope()->getObj());
    }
    runtimeConstants.setIsMapReduce(true);

    // Manually build an ExpressionContext with the desired options for the translated
    // aggregation. The one option worth noting here is allowDiskUse, which is required to allow
    // the $group stage of the translated pipeline to spill to disk.
    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx,
        verbosity,
        false,                         // fromMongos
        false,                         // needsMerge
        allowDiskUseByDefault.load(),  // allowDiskUse
        parsedMr.getBypassDocumentValidation().get_value_or(false),
        true,  // isMapReduceCommand
        parsedMr.getNamespace(),
        runtimeConstants,
        std::move(resolvedCollator),
        MongoProcessInterface::create(opCtx),
        StringMap<ExpressionContext::ResolvedNamespace>{},  // resolvedNamespaces
        uuid,
        boost::none,                             // let
        CurOp::get(opCtx)->dbProfileLevel() > 0  // mayDbProfile
    );
    expCtx->tempDir = storageGlobalParams.dbpath + "/_tmp";
    return expCtx;
}

}  // namespace

bool runAggregationMapReduce(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             const BSONObj& cmd,
                             BSONObjBuilder& result,
                             boost::optional<ExplainOptions::Verbosity> verbosity) {

    if (_sampler.tick()) {
        LOGV2_WARNING(5725801,
                      "The map reduce command is deprecated. For more information, see "
                      "https://docs.mongodb.com/manual/core/map-reduce/");
    }

    auto exhaustPipelineIntoBSONArray = [](auto&& exec) {
        BSONArrayBuilder bab;
        BSONObj obj;
        while (exec->getNext(&obj, nullptr) == PlanExecutor::ADVANCED) {
            bab.append(obj);
        }
        return bab.arr();
    };

    Timer cmdTimer;

    // TODO SERVER-68721: create IDLParserContext with tenant id of dbName.
    auto parsedMr = MapReduceCommandRequest::parse(IDLParserContext("mapReduce"), cmd);
    auto expCtx = makeExpressionContext(opCtx, parsedMr, verbosity);
    auto runnablePipeline = [&]() {
        auto pipeline = map_reduce_common::translateFromMR(parsedMr, expCtx);
        return expCtx->mongoProcessInterface->attachCursorSourceToPipelineForLocalRead(
            pipeline.release());
    }();
    auto exec = plan_executor_factory::make(expCtx, std::move(runnablePipeline));
    auto&& explainer = exec->getPlanExplainer();

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setPlanSummary_inlock(explainer.getPlanSummary());
    }

    try {
        auto resultArray = exhaustPipelineIntoBSONArray(exec);

        if (expCtx->explain) {
            Explain::explainPipeline(
                exec.get(), false /* executePipeline  */, *expCtx->explain, cmd, &result);
        }

        PlanSummaryStats planSummaryStats;
        explainer.getSummaryStats(&planSummaryStats);
        CurOp::get(opCtx)->debug().setPlanSummaryMetrics(planSummaryStats);

        if (!expCtx->explain) {
            if (parsedMr.getOutOptions().getOutputType() == OutputType::InMemory) {
                map_reduce_output_format::appendInlineResponse(std::move(resultArray), &result);
            } else {
                // For output to collection, pipeline execution should not return any results.
                invariant(resultArray.isEmpty());

                map_reduce_output_format::appendOutResponse(
                    parsedMr.getOutOptions().getDatabaseName(),
                    parsedMr.getOutOptions().getCollectionName(),
                    &result);
            }
        }

        // The aggregation pipeline may change the namespace of the curop and we need to set it back
        // to the original namespace to correctly report command stats. One example when the
        // namespace can be changed is when the pipeline contains an $out stage, which executes an
        // internal command to create a temp collection, changing the curop namespace to the name of
        // this temp collection.
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setNS_inlock(parsedMr.getNamespace().ns());
        }

        return true;
    } catch (DBException& e) {
        uassert(ErrorCodes::CommandNotSupportedOnView,
                "mapReduce on a view is not supported",
                e.code() != ErrorCodes::CommandOnShardedViewNotSupportedOnMongod);

        e.addContext("MapReduce internal error");
        throw;
    }
}

}  // namespace mongo::map_reduce_agg
