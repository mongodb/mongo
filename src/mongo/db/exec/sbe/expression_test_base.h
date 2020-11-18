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

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/vm/vm.h"

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
class EExpressionTestFixture : public mongo::unittest::Test {
protected:
    EExpressionTestFixture() : _ctx{std::make_unique<sbe::RuntimeEnvironment>()} {
        _ctx.root = &_emptyStage;
    }

    value::SlotId bindAccessor(value::SlotAccessor* accessor) {
        auto slot = _slotIdGenerator.generate();
        _ctx.pushCorrelated(slot, accessor);
        return slot;
    }

    std::unique_ptr<vm::CodeFragment> compileExpression(const EExpression& expr) {
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

    static std::pair<value::TypeTags, value::Value> makeBsonArray(const BSONArray& ba) {
        int numBytes = ba.objsize();
        uint8_t* data = new uint8_t[numBytes];
        memcpy(data, reinterpret_cast<const uint8_t*>(ba.objdata()), numBytes);
        return {value::TypeTags::bsonArray, value::bitcastFrom<uint8_t*>(data)};
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

    static std::pair<value::TypeTags, value::Value> makeNothing() {
        return {value::TypeTags::Nothing, value::bitcastFrom<int64_t>(0)};
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

private:
    value::SlotIdGenerator _slotIdGenerator;
    CoScanStage _emptyStage{kEmptyPlanNodeId};
    CompileCtx _ctx;
    vm::ByteCode _vm;
};

}  // namespace mongo::sbe
