/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <algorithm>
#include <benchmark/benchmark.h>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/bson_scan.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_bm_fixture.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/intrusive_counter.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {
template <typename T>
std::string debugPrint(const T* sbeElement) {
    return sbeElement ? sbe::DebugPrinter{}.print(sbeElement->debugPrint()) : nullptr;
}

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.bm");

class SbeExpressionBenchmarkFixture : public ExpressionBenchmarkFixture {
public:
    SbeExpressionBenchmarkFixture() : _env(std::make_unique<sbe::RuntimeEnvironment>()) {
        _inputSlotId = _env->registerSlot(
            "input"_sd, sbe::value::TypeTags::Nothing, 0, false, &_slotIdGenerator);
        _timeZoneDB = std::make_unique<TimeZoneDatabase>();
        _env->registerSlot("timeZoneDB"_sd,
                           sbe::value::TypeTags::timeZoneDB,
                           sbe::value::bitcastFrom<TimeZoneDatabase*>(_timeZoneDB.get()),
                           false,
                           &_slotIdGenerator);
        _inputSlotAccessor = _env->getAccessor(_inputSlotId);
    }

    void benchmarkExpression(BSONObj expressionSpec,
                             benchmark::State& benchmarkState,
                             const std::vector<Document>& documents) override final {
        QueryTestServiceContext serviceContext;
        auto opCtx = serviceContext.makeOperationContext();
        auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx.get(), kNss);
        auto expression =
            Expression::parseExpression(expCtx.get(), expressionSpec, expCtx->variablesParseState);

        if (expCtx->sbeCompatibility == SbeCompatibility::notCompatible) {
            benchmarkState.SkipWithError("expression is not supported by SBE");
            return;
        }

        expression = expression->optimize();

        LOGV2_DEBUG(6979800,
                    1,
                    "running sbe expression benchmark on expression",
                    "expression"_attr = expression
                                            ->serialize(SerializationOptions{
                                                .verbosity = boost::make_optional(
                                                    ExplainOptions::Verbosity::kQueryPlanner)})
                                            .toString());

        // This stage makes it possible to execute the benchmark in cases when
        // stage_builder::generateExpression adds more stages.
        // There is 10 ns overhead for using PlanStage instead of executing
        // expressions directly.
        // It can be removed when stage_builder::generateExpressions
        // always return EExpression.
        auto stage = sbe::makeS<sbe::BSONScanStage>(
            convertToBson(documents), boost::make_optional(_inputSlotId), kEmptyPlanNodeId);

        stage_builder::StageBuilderState state{
            opCtx.get(),
            _env,
            _planStageData.get(),
            _variables,
            nullptr /* yieldPolicy */,
            &_slotIdGenerator,
            &_frameIdGenerator,
            &_spoolIdGenerator,
            &_inListsSet,
            &_collatorsMap,
            &_sortSpecMap,
            _expCtx,
            false /* needsMerge */,
            false /* allowDiskUse */
        };

        auto rootSlot =
            stage_builder::TypedSlot{_inputSlotId, stage_builder::TypeSignature::kAnyScalarType};

        stage_builder::PlanStageSlots slots;
        slots.setResultObj(rootSlot);

        auto evalExpr = stage_builder::generateExpression(state, expression.get(), rootSlot, slots);

        LOGV2_DEBUG(6979801,
                    1,
                    "sbe expression benchmark PlanStage",
                    "stage"_attr = debugPrint(stage.get()));

        auto expr = evalExpr.extractExpr(state);
        LOGV2_DEBUG(6979802,
                    1,
                    "sbe expression benchmark EExpression",
                    "expression"_attr = debugPrint(expr.get()));

        stage->attachToOperationContext(opCtx.get());
        stage->prepare(_env.ctx);

        _env.ctx.root = stage.get();
        sbe::vm::CodeFragment code = expr->compileDirect(_env.ctx);
        sbe::vm::ByteCode vm;
        stage->open(/*reopen =*/false);
        for (auto keepRunning : benchmarkState) {
            for (auto st = stage->getNext(); st == sbe::PlanState::ADVANCED;
                 st = stage->getNext()) {
                executeExpr(vm, &code);
            }
            benchmark::ClobberMemory();
            stage->open(/*reopen = */ true);
        }
    }

private:
    std::vector<BSONObj> convertToBson(const std::vector<Document>& documents) {
        std::vector<BSONObj> result;
        result.reserve(documents.size());
        std::transform(documents.begin(),
                       documents.end(),
                       std::back_inserter(result),
                       [](const auto& doc) { return doc.toBson(); });
        return result;
    }

    void executeExpr(sbe::vm::ByteCode& vm, const sbe::vm::CodeFragment* compiledExpr) const {
        auto [owned, tag, val] = vm.run(compiledExpr);
        if (owned) {
            sbe::value::releaseValue(tag, val);
        }
    }

    stage_builder::Environment _env;
    std::unique_ptr<stage_builder::PlanStageStaticData> _planStageData;
    Variables _variables;
    sbe::value::SlotIdGenerator _slotIdGenerator;
    sbe::value::FrameIdGenerator _frameIdGenerator;
    sbe::value::SpoolIdGenerator _spoolIdGenerator;
    stage_builder::StageBuilderState::InListsSet _inListsSet;
    stage_builder::StageBuilderState::CollatorsMap _collatorsMap;
    stage_builder::StageBuilderState::SortSpecMap _sortSpecMap;
    boost::intrusive_ptr<ExpressionContext> _expCtx;

    sbe::value::SlotId _inputSlotId;
    std::unique_ptr<TimeZoneDatabase> _timeZoneDB;
    sbe::RuntimeEnvironment::Accessor* _inputSlotAccessor;
};

BENCHMARK_EXPRESSIONS(SbeExpressionBenchmarkFixture)

}  // namespace
}  // namespace mongo
