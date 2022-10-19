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

#include <benchmark/benchmark.h>

#include "mongo/db/exec/sbe/stages/bson_scan.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/pipeline/expression_bm_fixture.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {
template <typename T>
std::string debugPrint(const T* sbeElement) {
    return sbeElement ? sbe::DebugPrinter{}.print(sbeElement->debugPrint()) : nullptr;
}

const NamespaceString kNss{"test.bm"};

class SbeExpressionBenchmarkFixture : public ExpressionBenchmarkFixture {
public:
    SbeExpressionBenchmarkFixture() : _planStageData(std::make_unique<sbe::RuntimeEnvironment>()) {
        _inputSlotId = _planStageData.env->registerSlot(
            "input"_sd, sbe::value::TypeTags::Nothing, 0, false, &_slotIdGenerator);
        _timeZoneDB = std::make_unique<TimeZoneDatabase>();
        _planStageData.env->registerSlot(
            "timeZoneDB"_sd,
            sbe::value::TypeTags::timeZoneDB,
            sbe::value::bitcastFrom<TimeZoneDatabase*>(_timeZoneDB.get()),
            false,
            &_slotIdGenerator);
        _inputSlotAccessor = _planStageData.env->getAccessor(_inputSlotId);
    }

    void benchmarkExpression(BSONObj expressionSpec,
                             benchmark::State& benchmarkState,
                             const std::vector<Document>& documents) override final {
        QueryTestServiceContext serviceContext;
        auto opCtx = serviceContext.makeOperationContext();
        auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx.get(), kNss);
        auto expression =
            Expression::parseExpression(expCtx.get(), expressionSpec, expCtx->variablesParseState);

        if (!expCtx->sbeCompatible) {
            benchmarkState.SkipWithError("expression is not supported by SBE");
            return;
        }

        expression = expression->optimize();

        LOGV2_DEBUG(6979800,
                    1,
                    "running sbe expression benchmark on expression",
                    "expression"_attr = expression->serialize(/*explain = */ true).toString());

        // This stage makes it possible to execute the benchmark in cases when
        // stage_builder::generateExpression adds more stages.
        // There is 10 ns overhead for using PlanStage instead of executing
        // expressions directly.
        // It can be removed when stage_builder::generateExpressions
        // always return EExpression.
        stage_builder::EvalStage bsonScanStage(
            std::make_unique<sbe::BSONScanStage>(
                convertToBson(documents), boost::make_optional(_inputSlotId), kEmptyPlanNodeId),
            {_inputSlotId});

        stage_builder::StageBuilderState state{
            opCtx.get(),
            &_planStageData,
            _variables,
            &_slotIdGenerator,
            &_frameIdGenerator,
            &_spoolIdGenerator,
            false /* needsMerge */,
            false /* allowDiskUse */
        };
        auto [evalExpr, evalStage] =
            stage_builder::generateExpression(state,
                                              expression.get(),
                                              std::move(bsonScanStage),
                                              boost::make_optional(_inputSlotId),
                                              kEmptyPlanNodeId);

        auto stage = evalStage.extractStage(kEmptyPlanNodeId);
        LOGV2_DEBUG(6979801,
                    1,
                    "sbe expression benchmark PlanStage",
                    "stage"_attr = debugPrint(stage.get()));

        auto expr = evalExpr.extractExpr();
        LOGV2_DEBUG(6979802,
                    1,
                    "sbe expression benchmark EExpression",
                    "expression"_attr = debugPrint(expr.get()));

        stage->attachToOperationContext(opCtx.get());
        stage->prepare(_planStageData.ctx);

        _planStageData.ctx.root = stage.get();
        sbe::vm::CodeFragment code = expr->compileDirect(_planStageData.ctx);
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

    stage_builder::PlanStageData _planStageData;
    Variables _variables;
    sbe::value::SlotIdGenerator _slotIdGenerator;
    sbe::value::FrameIdGenerator _frameIdGenerator;
    sbe::value::SpoolIdGenerator _spoolIdGenerator;

    sbe::value::SlotId _inputSlotId;
    std::unique_ptr<TimeZoneDatabase> _timeZoneDB;
    sbe::RuntimeEnvironment::Accessor* _inputSlotAccessor;
};

BENCHMARK_EXPRESSIONS(SbeExpressionBenchmarkFixture)

}  // namespace
}  // namespace mongo
