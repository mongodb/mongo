/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#pragma once

#include "mongo/unittest/unittest.h"

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/values/value_printer.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/exec/sbe/vm/vm_printer.h"
#include "mongo/unittest/golden_test.h"

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
    EExpressionTestFixture(std::unique_ptr<sbe::RuntimeEnvironment> runtimeEnv)
        : _runtimeEnv(runtimeEnv.get()), _ctx(std::move(runtimeEnv)) {
        _ctx.root = &_emptyStage;
    }

    EExpressionTestFixture()
        : EExpressionTestFixture(std::make_unique<sbe::RuntimeEnvironment>()) {}

    value::SlotId bindAccessor(value::SlotAccessor* accessor) {
        auto slot = _slotIdGenerator.generate();
        _ctx.pushCorrelated(slot, accessor);
        boundAccessors.push_back(std::make_pair(slot, accessor));
        return slot;
    }

    const RuntimeEnvironment* runtimeEnv() const {
        return _runtimeEnv;
    }

    sbe::value::SlotId registerSlot(StringData name,
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
     * The caller takes ownership of the Value returned by this function and must call
     * 'releaseValue()' on it. The preferred way to ensure the Value is properly released is to
     * immediately store it in a ValueGuard.
     */
    std::pair<value::TypeTags, value::Value> runCompiledExpression(
        const vm::CodeFragment* compiledExpr) {
        auto [owned, tag, val] = _vm.run(compiledExpr);
        if (owned) {
            return {tag, val};
        } else {
            // It is possible that this result is a "view" into memory that is owned somewhere else.
            // By creating a copy, we ensure it is safe for the caller to call 'releaseValue()' on
            // the copied Value.
            return value::copyValue(tag, val);
        }
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
        vm::CodeFragmentPrinter(vm::CodeFragmentPrinter::PrintFormat::Stable).print(os, code);
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
            auto [owned, tag, val] = _vm.run(&code);
            value::ValueGuard guard(owned, tag, val);
            os << "RESULT: ";
            valuePrinter.writeValueToStream(tag, val);
            os << std::endl;
        } catch (const DBException& e) {
            os << "EXCEPTION: " << e.toString() << std::endl;
        }
        os << std::endl;
    }

    void runAndAssertNothing(const vm::CodeFragment* compiledExpression) {
        auto [resultTag, resultValue] = runCompiledExpression(compiledExpression);
        value::ValueGuard guard(resultTag, resultValue);
        ASSERT_EQUALS(resultTag, sbe::value::TypeTags::Nothing);
    }

    static std::pair<value::TypeTags, value::Value> makeBsonArray(const BSONArray& ba) {
        return value::copyValue(value::TypeTags::bsonArray,
                                value::bitcastFrom<const char*>(ba.objdata()));
    }


    static std::pair<value::TypeTags, value::Value> makeBsonObject(const BSONObj& bo) {
        return value::copyValue(value::TypeTags::bsonObject,
                                value::bitcastFrom<const char*>(bo.objdata()));
    }

    static std::pair<value::TypeTags, value::Value> makeArraySet(const BSONArray& arr) {
        auto [tmpTag, tmpVal] = makeBsonArray(arr);
        value::ValueGuard tmpGuard{tmpTag, tmpVal};

        value::ArrayEnumerator enumerator{tmpTag, tmpVal};

        auto [arrTag, arrVal] = value::makeNewArraySet();
        value::ValueGuard guard{arrTag, arrVal};

        auto arrView = value::getArraySetView(arrVal);

        while (!enumerator.atEnd()) {
            auto [tag, val] = enumerator.getViewOfValue();
            enumerator.advance();

            auto [copyTag, copyVal] = value::copyValue(tag, val);
            arrView->push_back(copyTag, copyVal);
        }
        guard.reset();

        return {arrTag, arrVal};
    }

    static std::pair<value::TypeTags, value::Value> makeArray(const BSONArray& arr) {
        auto [tmpTag, tmpVal] = makeBsonArray(arr);
        value::ValueGuard tmpGuard{tmpTag, tmpVal};

        value::ArrayEnumerator enumerator{tmpTag, tmpVal};

        auto [arrTag, arrVal] = value::makeNewArray();
        value::ValueGuard guard{arrTag, arrVal};

        auto arrView = value::getArrayView(arrVal);

        while (!enumerator.atEnd()) {
            auto [tag, val] = enumerator.getViewOfValue();
            enumerator.advance();

            auto [copyTag, copyVal] = value::copyValue(tag, val);
            arrView->push_back(copyTag, copyVal);
        }
        guard.reset();

        return {arrTag, arrVal};
    }

    static std::pair<value::TypeTags, value::Value> makeObject(const BSONObj& obj) {
        auto [tmpTag, tmpVal] = makeBsonObject(obj);
        value::ValueGuard tmpGuard{tmpTag, tmpVal};

        value::ObjectEnumerator enumerator{tmpTag, tmpVal};

        auto [objTag, objVal] = value::makeNewObject();
        value::ValueGuard guard{objTag, objVal};

        auto objView = value::getObjectView(objVal);

        while (!enumerator.atEnd()) {
            auto [tag, val] = enumerator.getViewOfValue();
            auto [copyTag, copyVal] = value::copyValue(tag, val);
            objView->push_back(enumerator.getFieldName(), copyTag, copyVal);
            enumerator.advance();
        }
        guard.reset();

        return {objTag, objVal};
    }

    static std::pair<value::TypeTags, value::Value> makeNothing() {
        return {value::TypeTags::Nothing, value::bitcastFrom<int64_t>(0)};
    }

    static std::pair<value::TypeTags, value::Value> makeNull() {
        return {value::TypeTags::Null, value::bitcastFrom<int64_t>(0)};
    }

    static std::pair<value::TypeTags, value::Value> makeInt32(int32_t value) {
        return {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(value)};
    }

    static std::pair<value::TypeTags, value::Value> makeInt64(int64_t value) {
        return {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(value)};
    }

    static std::pair<value::TypeTags, value::Value> makeDouble(double value) {
        return {value::TypeTags::NumberDouble, value::bitcastFrom<double>(value)};
    }

    static std::pair<value::TypeTags, value::Value> makeBool(bool value) {
        return {value::TypeTags::Boolean, value::bitcastFrom<bool>(value)};
    }

    static std::pair<value::TypeTags, value::Value> makeTimestamp(Timestamp timestamp) {
        return {value::TypeTags::Timestamp, value::bitcastFrom<uint64_t>(timestamp.asULL())};
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
    RuntimeEnvironment* _runtimeEnv;
    CompileCtx _ctx;
    vm::ByteCode _vm;
    std::vector<std::pair<value::SlotId, value::SlotAccessor*>> boundAccessors;
};


class GoldenEExpressionTestFixture : public EExpressionTestFixture, public GoldenSBETestFixture {};

}  // namespace mongo::sbe
