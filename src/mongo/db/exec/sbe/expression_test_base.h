// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/exec/sbe/vm/vm_printer.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/db/query/stage_builder/sbe/builder_state.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::sbe {

/**
 * Unit tests based on this fixture should follow this pattern:
 * 1) Create an accessor for each input to the test expression and give the accessor a slot with the
 *    'bindAccessor()' method.
 * 2) Create an EExpression and compile it with the 'compileExpression()' function. The expression
 *    can read values from the accessors created in step 1 using an EVariable with the corresponding
 *    SlotId.
 * 3) Assign values to the inputs by calling 'reset()' on their accessors.
 * 4) Run the expression with either the 'runCompiledExpression()' or---when the expression is
 *    expected to always return a bool---the 'runCompiledExpressionPredicate()' function.
 * 5) Test that the result was as expected.
 *
 * After setting up the compiled expression in steps 1--3, the test can run it multiple times with
 * different values, repeating steps 3--5.
 */
class EExpressionTestFixture : public virtual SBETestFixture {
protected:
    EExpressionTestFixture()
        : _env{std::make_unique<sbe::RuntimeEnvironment>()},
          _runtimeEnv{_env.runtimeEnv},
          _ctx{_env.ctx},
          _expCtx{make_intrusive<ExpressionContextForTest>()},
          _state{nullptr /* opCtx */,
                 _env,
                 nullptr /* planStageData */,
                 _variables,
                 nullptr /* yieldPolicy */,
                 &_slotIdGenerator,
                 &_frameIdGenerator,
                 nullptr /* spoolIdGenerator */,
                 nullptr /* inListsMap */,
                 nullptr /* collatorsMap */,
                 nullptr /* sortSpecMap */,
                 _expCtx,
                 false /* needsMerge */,
                 false /* allowDiskUse */,
                 *_expCtx->getIfrContext()} {
        _ctx.root = &_emptyStage;
    }

    value::SlotId bindAccessor(value::SlotAccessor* accessor) {
        auto slot = _slotIdGenerator.generate();
        _ctx.pushCorrelated(slot, accessor);
        boundAccessors.push_back(std::make_pair(slot, accessor));
        return slot;
    }

    RuntimeEnvironment* runtimeEnv() const {
        return _runtimeEnv;
    }

    value::SlotIdGenerator& slotIdGenerator() {
        return _slotIdGenerator;
    }

    sbe::value::SlotId registerSlot(std::string_view name,
                                    value::TypeTags tag,
                                    value::Value val,
                                    bool owned) {
        return _runtimeEnv->registerSlot(name, tag, val, owned, &_slotIdGenerator);
    }

    std::unique_ptr<vm::CodeFragment> compileExpression(const EExpression& expr) {
        return expr.compile(_ctx);
    }

    /**
     * Compiles 'expr' to bytecode when 'expr' is computing an aggregate. The current aggregate
     * value can be read out of the provided 'aggAccessor'.
     *
     * Note that when actually executing the resulting bytecode, the caller is responsible for
     * setting the value of 'aggAccessor' to the new resulting aggregate value.
     */
    std::unique_ptr<vm::CodeFragment> compileAggExpression(const EExpression& expr,
                                                           value::SlotAccessor* aggAccessor) {
        ON_BLOCK_EXIT([this] {
            _ctx.aggExpression = false;
            _ctx.accumulator = nullptr;
        });
        _ctx.aggExpression = true;
        _ctx.accumulator = aggAccessor;
        return expr.compile(_ctx);
    }

    /**
     * Compile and run the given expression.
     */
    FastTuple<bool, value::TypeTags, value::Value> runExpression(const EExpression& expr) {
        auto compiledExpr = expr.compile(_ctx);
        return _vm.run(compiledExpr.get()).releaseToRaw();
    }

    /**
     * The caller takes ownership of the Value returned by this function and must call
     * 'releaseValue()' on it. The preferred way to ensure the Value is properly released is to
     * immediately store it in a TagValueOwned.
     */
    std::pair<value::TypeTags, value::Value> runCompiledExpression(
        const vm::CodeFragment* compiledExpr) {
        auto res = _vm.run(compiledExpr);
        return res.releaseToOwnedRaw();
    }

    bool runCompiledExpressionPredicate(const vm::CodeFragment* compiledExpr) {
        return _vm.runPredicate(compiledExpr);
    }

    void printInputExpression(std::ostream& os, const EExpression& expr) {
        os << "-- INPUT EXPRESSION:" << std::endl;
        os << DebugPrinter().print(expr.debugPrint()) << std::endl << std::endl;
    }

    void printCompiledExpression(std::ostream& os, const vm::CodeFragment& code) {
        os << "-- COMPILED EXPRESSION:" << std::endl;
        vm::CodeFragmentPrinter(vm::CodeFragment::PrintFormat::Stable).print(os, code);
        os << std::endl << std::endl;
    }

    void executeAndPrintVariation(std::ostream& os, const vm::CodeFragment& code) {
        auto valuePrinter = makeValuePrinter(os);

        os << "-- EXECUTE VARIATION:" << std::endl;
        if (!boundAccessors.empty()) {
            bool first = true;
            os << "SLOTS: [";
            for (auto& p : boundAccessors) {
                if (!first) {
                    os << ", ";
                } else {
                    first = false;
                }
                os << p.first << ": ";

                auto [tag, val] = p.second->getViewOfValue();
                valuePrinter.writeValueToStream(tag, val);
            }
            os << "]" << std::endl;
        }

        try {
            auto res = _vm.run(&code);
            os << "RESULT: ";
            valuePrinter.writeValueToStream(res.tag(), res.value());
            os << std::endl;
        } catch (const DBException& e) {
            os << "EXCEPTION: " << e.toString() << std::endl;
        }
        os << std::endl;
    }

    void runAndAssertNothing(const vm::CodeFragment* compiledExpression) {
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpression));
        ASSERT_EQUALS(result.tag(), sbe::value::TypeTags::Nothing);
    }

    static std::pair<value::TypeTags, value::Value> makeEmptyState(
        size_t maxSize, int32_t memLimit = std::numeric_limits<int32_t>::max()) {
        auto [stateTag, stateVal] = value::makeNewArray();
        auto state = value::getArrayView(stateVal);

        state->push_back_raw(value::makeNewArray() /* internalArr */);
        state->push_back_raw(makeInt64(0) /* StartIdx */);
        state->push_back_raw(makeInt64(maxSize) /* MaxSize */);
        state->push_back_raw(makeInt32(0) /* MemUsage */);
        state->push_back_raw(makeInt32(memLimit) /* MemLimit */);
        state->push_back_raw(makeBool(true) /* IsGroupAccum */);

        return {stateTag, stateVal};
    }

    static std::unique_ptr<EConstant> makeC(value::TypeTags tag, value::Value value) {
        return std::make_unique<EConstant>(tag, value);
    }

    static std::unique_ptr<EConstant> makeC(const TypedValue p) {
        return makeC(p.first, p.second);
    }

protected:
    value::SlotIdGenerator _slotIdGenerator;
    CoScanStage _emptyStage{kEmptyPlanNodeId};
    stage_builder::Environment _env;
    RuntimeEnvironment* _runtimeEnv;
    CompileCtx& _ctx;
    vm::ByteCode _vm;
    std::vector<std::pair<value::SlotId, value::SlotAccessor*>> boundAccessors;
    sbe::value::FrameIdGenerator _frameIdGenerator;
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
    Variables _variables;
    stage_builder::StageBuilderState _state;
};


class GoldenEExpressionTestFixture : public EExpressionTestFixture, public GoldenSBETestFixture {};

}  // namespace mongo::sbe
