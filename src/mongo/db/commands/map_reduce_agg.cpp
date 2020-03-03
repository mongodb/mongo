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
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/query/explain_common.h"
#include "mongo/db/query/map_reduce_output_format.h"

namespace mongo::map_reduce_agg {

namespace {

auto makeExpressionContext(OperationContext* opCtx,
                           const MapReduce& parsedMr,
                           boost::optional<ExplainOptions::Verbosity> verbosity) {
    // AutoGetCollectionForReadCommand will throw if the sharding version for this connection is
    // out of date.
    AutoGetCollectionForReadCommand ctx(
        opCtx, parsedMr.getNamespace(), AutoGetCollection::ViewMode::kViewsPermitted);
    uassert(ErrorCodes::CommandNotSupportedOnView,
            "mapReduce on a view is not supported",
            !ctx.getView());

    auto resolvedCollator = PipelineD::resolveCollator(
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
        false,  // fromMongos
        false,  // needsmerge
        true,   // allowDiskUse
        parsedMr.getBypassDocumentValidation().get_value_or(false),
        true,  // isMapReduceCommand
        parsedMr.getNamespace(),
        runtimeConstants,
        std::move(resolvedCollator),
        MongoProcessInterface::create(opCtx),
        StringMap<ExpressionContext::ResolvedNamespace>{},  // resolvedNamespaces
        uuid,
        CurOp::get(opCtx)->dbProfileLevel() > 0  // mayDbProfile
    );
    expCtx->tempDir = storageGlobalParams.dbpath + "/_tmp";
    return expCtx;
}

}  // namespace

bool runAggregationMapReduce(OperationContext* opCtx,
                             const BSONObj& cmd,
                             BSONObjBuilder& result,
                             boost::optional<ExplainOptions::Verbosity> verbosity) {
    auto exhaustPipelineIntoBSONArray = [](auto&& pipeline) {
        BSONArrayBuilder bab;
        while (auto&& doc = pipeline->getNext())
            bab.append(doc->toBson());
        return bab.arr();
    };

    Timer cmdTimer;

    auto parsedMr = MapReduce::parse(IDLParserErrorContext("MapReduce"), cmd);
    auto expCtx = makeExpressionContext(opCtx, parsedMr, verbosity);
    auto runnablePipeline = [&]() {
        auto pipeline = map_reduce_common::translateFromMR(parsedMr, expCtx);
        return expCtx->mongoProcessInterface->attachCursorSourceToPipelineForLocalRead(
            pipeline.release());
    }();

    {
        auto planSummaryStr = PipelineD::getPlanSummaryStr(runnablePipeline.get());

        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setPlanSummary_inlock(std::move(planSummaryStr));
    }

    try {
        auto resultArray = exhaustPipelineIntoBSONArray(runnablePipeline);

        if (expCtx->explain) {
            result << "stages" << Value(runnablePipeline->writeExplainOps(*(expCtx->explain)));
            explain_common::generateServerInfo(&result);
        }

        PlanSummaryStats planSummaryStats;
        PipelineD::getPlanSummaryStats(runnablePipeline.get(), &planSummaryStats);
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
