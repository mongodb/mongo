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
    }

    void benchmarkExpression(BSONObj expressionSpec,
                             benchmark::State& benchmarkState,
                             const std::vector<Document>& documents) override final {
        std::vector<BSONObj> bsonDocuments = convertToBson(documents);
        QueryTestServiceContext serviceContext;

        auto opContext = serviceContext.makeOperationContext();
        auto exprContext = make_intrusive<ExpressionContextForTest>(opContext.get(), _nss);
        auto expression = Expression::parseExpression(
            exprContext.get(), expressionSpec, exprContext->variablesParseState);

        if (!exprContext->sbeCompatible) {
            benchmarkState.SkipWithError("expression is not supported by SBE");
            return;
        }

        expression = expression->optimize();

        stage_builder::StageBuilderState state{
            opContext.get(),
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
                                              stage_builder::EvalStage{},
                                              boost::make_optional(_inputSlotId),
                                              kEmptyPlanNodeId);
        tassert(6979800, "Unexpected: EvalStage.stage is not null", evalStage.stageIsNull());

        auto expr = evalExpr.extractExpr();
        auto compiledExpr = expr->compile(_planStageData.ctx);
        sbe::vm::ByteCode _vm;

        LOGV2_DEBUG(
            6979802,
            1,
            "running sbe expression benchmark on expression {expression}, sbe representation {sbe}",
            "expression"_attr = expression->serialize(/*explain = */ true).toString(),
            "sbe"_attr = sbe::DebugPrinter{}.print(expr->debugPrint()));

        for (auto keepRunning : benchmarkState) {
            for (const auto& document : bsonDocuments) {
                _planStageData.env->resetSlot(
                    _inputSlotId,
                    sbe::value::TypeTags::bsonObject,
                    sbe::value::bitcastFrom<const char*>(document.objdata()),
                    false);
                benchmark::DoNotOptimize(_vm.run(compiledExpr.get()));
            }
            benchmark::ClobberMemory();
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

    NamespaceString _nss{"test.bm"};

    stage_builder::PlanStageData _planStageData;
    Variables _variables;
    sbe::value::SlotIdGenerator _slotIdGenerator;
    sbe::value::FrameIdGenerator _frameIdGenerator;
    sbe::value::SpoolIdGenerator _spoolIdGenerator;

    sbe::value::SlotId _inputSlotId;
    std::unique_ptr<TimeZoneDatabase> _timeZoneDB;
};

BENCHMARK_EXPRESSIONS(SbeExpressionBenchmarkFixture)

}  // namespace
}  // namespace mongo
