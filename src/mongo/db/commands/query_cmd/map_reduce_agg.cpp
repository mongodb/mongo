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
#include "mongo/db/commands/query_cmd/map_reduce_agg.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/query_cmd/map_reduce_gen.h"
#include "mongo/db/commands/query_cmd/map_reduce_global_variable_scope.h"
#include "mongo/db/commands/query_cmd/map_reduce_out_options.h"
#include "mongo/db/commands/query_cmd/mr_common.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/disk_use_options_gen.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_diagnostic_printer.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/explain_diagnostic_printer.h"
#include "mongo/db/query/map_reduce_output_format.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/string_map.h"
#include "mongo/util/timer.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo::map_reduce_agg {

namespace {
Rarely _sampler;

auto makeExpressionContext(OperationContext* opCtx,
                           const MapReduceCommandRequest& parsedMr,
                           boost::optional<ExplainOptions::Verbosity> verbosity) {
    AutoStatsTracker statsTracker(opCtx,
                                  parsedMr.getNamespace(),
                                  Top::LockType::ReadLocked,
                                  AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                  DatabaseProfileSettings::get(opCtx->getServiceContext())
                                      .getDatabaseProfileLevel(parsedMr.getNamespace().dbName()));

    // acquireCollectionOrViewMaybeLockFree will throw if the sharding version for this connection
    // is out of date.
    const auto collOrView =
        acquireCollectionOrViewMaybeLockFree(opCtx,
                                             CollectionOrViewAcquisitionRequest::fromOpCtx(
                                                 opCtx,
                                                 parsedMr.getNamespace(),
                                                 AcquisitionPrerequisites::kRead,
                                                 AcquisitionPrerequisites::ViewMode::kCanBeView));
    uassert(ErrorCodes::CommandNotSupportedOnView,
            "mapReduce on a view is not supported",
            !collOrView.isView());

    const auto& coll = collOrView.getCollection();

    auto [resolvedCollator, _] = resolveCollator(
        opCtx, parsedMr.getCollation().get_value_or(BSONObj()), coll.getCollectionPtr());

    // The UUID of the collection for the execution namespace of this aggregation.
    auto uuid = coll.exists() ? boost::make_optional(coll.uuid()) : boost::none;

    auto runtimeConstants = Variables::generateRuntimeConstants(opCtx);
    if (parsedMr.getScope()) {
        runtimeConstants.setJsScope(parsedMr.getScope()->getObj());
    }
    runtimeConstants.setIsMapReduce(true);

    // Manually build an ExpressionContext with the desired options for the translated
    // aggregation. The one option worth noting here is allowDiskUse, which is required to allow
    // the $group stage of the translated pipeline to spill to disk.
    auto expCtx =
        ExpressionContextBuilder{}
            .opCtx(opCtx)
            .collator(std::move(resolvedCollator))
            .mongoProcessInterface(MongoProcessInterface::create(opCtx))
            .ns(parsedMr.getNamespace())
            .mayDbProfile(CurOp::get(opCtx)->dbProfileLevel() > 0)
            .allowDiskUse(allowDiskUseByDefault.load())
            .bypassDocumentValidation(parsedMr.getBypassDocumentValidation().get_value_or(false))
            .isMapReduceCommand(true)
            .collUUID(uuid)
            .explain(verbosity)
            .runtimeConstants(runtimeConstants)
            .tmpDir(boost::filesystem::path(storageGlobalParams.dbpath) / "_tmp")
            .build();
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

    const auto& vts = auth::ValidatedTenancyScope::get(opCtx);
    auto sc = vts
        ? SerializationContext::stateCommandRequest(vts->hasTenantId(), vts->isFromAtlasProxy())
        : SerializationContext::stateStorageRequest();
    auto parsedMr = MapReduceCommandRequest::parse(
        cmd, IDLParserContext("mapReduce", vts, dbName.tenantId(), sc));

    // Start the query planning timer right after parsing.
    auto curop = CurOp::get(opCtx);
    curop->beginQueryPlanningTimer();

    auto expCtx = makeExpressionContext(opCtx, parsedMr, verbosity);

    // Create an RAII object that prints useful information about the ExpressionContext in the case
    // of a tassert or crash.
    ScopedDebugInfo expCtxDiagnostics("ExpCtxDiagnostics",
                                      diagnostic_printers::ExpressionContextPrinter{expCtx});
    auto runnablePipeline = [&]() {
        auto pipeline = map_reduce_common::translateFromMR(parsedMr, expCtx);
        return expCtx->getMongoProcessInterface()->attachCursorSourceToPipelineForLocalRead(
            pipeline.release());
    }();
    auto exec = plan_executor_factory::make(expCtx, std::move(runnablePipeline));
    auto&& explainer = exec->getPlanExplainer();
    // Store the plan summary string in CurOp.
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curop->setPlanSummary(lk, explainer.getPlanSummary());
    }

    try {
        // Capture diagnostics to be logged in the case of a failure.
        ScopedDebugInfo explainDiagnostics(
            "explainDiagnostics", diagnostic_printers::ExplainDiagnosticPrinter{exec.get()});

        auto resultArray = exhaustPipelineIntoBSONArray(exec);

        if (expCtx->getExplain()) {
            auto pipelineExec = dynamic_cast<PlanExecutorPipeline*>(exec.get());
            tassert(
                10610100, "The plan executor is not of type 'PlanExecutorPipeline'", pipelineExec);
            Explain::explainPipeline(
                pipelineExec, false /* executePipeline  */, *expCtx->getExplain(), cmd, &result);
        }

        PlanSummaryStats planSummaryStats;
        explainer.getSummaryStats(&planSummaryStats);
        CurOp::get(opCtx)->debug().setPlanSummaryMetrics(std::move(planSummaryStats));

        if (!expCtx->getExplain()) {
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
            CurOp::get(opCtx)->setNS(lk, parsedMr.getNamespace());
        }
        uassertStatusOK(
            result.asTempObj().validateBSONObjSize().addContext("mapReduce command failed"));
        return true;
    } catch (DBException& e) {
        uassert(ErrorCodes::CommandNotSupportedOnView,
                "mapReduce on a view is not supported",
                e.code() != ErrorCodes::CommandOnShardedViewNotSupportedOnMongod);

        e.addContext(str::stream() << "Executor error during MapReduce command on namespace: "
                                   << parsedMr.getNamespace().toStringForErrorMsg());
        throw;
    }
}

}  // namespace mongo::map_reduce_agg
