/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <ostream>
#include <utility>
#include <vector>

namespace mongo::sbe {

class SBEPrimNaryTest : public GoldenEExpressionTestFixture {
public:
    std::unique_ptr<EExpression> makeNaryExpr(EPrimNary::Op op, const value::SlotVector& slotIds) {
        std::vector<std::unique_ptr<EExpression>> args;
        args.reserve(slotIds.size());
        for (auto& slot : slotIds) {
            args.emplace_back(makeE<EVariable>(slot));
        }
        return sbe::makeE<EPrimNary>(op, std::move(args));
    }

    void runNaryOpTest(std::ostream& os, EPrimNary::Op op, std::vector<TypedValue>& testValues) {
        value::ViewOfValueAccessor lhsAccessor;
        value::ViewOfValueAccessor rhsAccessor;
        auto lhsSlot = bindAccessor(&lhsAccessor);
        auto rhsSlot = bindAccessor(&rhsAccessor);

        auto expr = sbe::makeE<EPrimNary>(
            op, makeVEs(makeE<EVariable>(lhsSlot), makeE<EVariable>(rhsSlot)));
        printInputExpression(os, *expr);

        auto compiledExpr = compileExpression(*expr);
        printCompiledExpression(os, *compiledExpr);

        // Verify the operator table
        for (auto lhs : testValues)
            for (auto rhs : testValues) {
                lhsAccessor.reset(lhs.first, lhs.second);
                rhsAccessor.reset(rhs.first, rhs.second);
                executeAndPrintVariation(os, *compiledExpr);
            }
    }

protected:
    std::vector<TypedValue> boolTestValues = {makeNothing(), makeBool(false), makeBool(true)};
    ValueVectorGuard boolTestValuesGuard{boolTestValues};
};

/* Logic Operators */

TEST_F(SBEPrimNaryTest, TruthTableAnd) {
    auto& os = gctx->outStream();
    runNaryOpTest(os, EPrimNary::Op::logicAnd, boolTestValues);
}

TEST_F(SBEPrimNaryTest, TruthTableOr) {
    auto& os = gctx->outStream();
    runNaryOpTest(os, EPrimNary::Op::logicOr, boolTestValues);
}

TEST_F(SBEPrimNaryTest, BalancedAnd) {
    auto& os = gctx->outStream();

    std::vector<std::unique_ptr<value::ViewOfValueAccessor>> accessors;
    value::SlotVector slotIds;

    int depth = 3;
    int numSlots = 1 << depth;
    for (int i = 0; i < numSlots; i++) {
        accessors.emplace_back(std::make_unique<value::ViewOfValueAccessor>());
        accessors.back()->reset(value::TypeTags::Boolean, value::bitcastFrom<bool>(true));
        slotIds.push_back(bindAccessor(accessors.back().get()));
    }

    auto expr = makeNaryExpr(EPrimNary::Op::logicAnd, slotIds);
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    // All values are true.
    {
        auto [tag, val] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(tag, val);

        TypedValue expected = makeBool(true);
        ASSERT_THAT(std::make_pair(tag, val), ValueEq(expected));
    }

    // One of the values is false.
    for (int falsePosition = 0; falsePosition < numSlots; falsePosition++) {
        accessors[falsePosition]->reset(value::TypeTags::Boolean, value::bitcastFrom<bool>(false));

        auto [tag, val] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(tag, val);

        TypedValue expected = makeBool(false);
        ASSERT_THAT(std::make_pair(tag, val), ValueEq(expected));

        accessors[falsePosition]->reset(value::TypeTags::Boolean, value::bitcastFrom<bool>(true));
    }

    // One of the values is true and one is nothing.
    for (int nothingPosition = 0; nothingPosition < numSlots; nothingPosition++)
        for (int falsePosition = 0; falsePosition < numSlots; falsePosition++) {
            if (nothingPosition == falsePosition)
                continue;

            accessors[nothingPosition]->reset(value::TypeTags::Nothing, 0);
            accessors[falsePosition]->reset(value::TypeTags::Boolean,
                                            value::bitcastFrom<bool>(false));


            auto [tag, val] = runCompiledExpression(compiledExpr.get());
            value::ValueGuard guard(tag, val);

            TypedValue expected = nothingPosition < falsePosition ? makeNothing() : makeBool(false);
            ASSERT_THAT(std::make_pair(tag, val), ValueEq(expected));

            accessors[falsePosition]->reset(value::TypeTags::Boolean,
                                            value::bitcastFrom<bool>(true));

            accessors[nothingPosition]->reset(value::TypeTags::Boolean,
                                              value::bitcastFrom<bool>(true));
        }
}

TEST_F(SBEPrimNaryTest, BalancedOr) {
    auto& os = gctx->outStream();

    std::vector<std::unique_ptr<value::ViewOfValueAccessor>> accessors;
    value::SlotVector slotIds;

    int depth = 3;
    int numSlots = 1 << depth;

    for (int i = 0; i < numSlots; i++) {
        accessors.emplace_back(std::make_unique<value::ViewOfValueAccessor>());
        accessors.back()->reset(value::TypeTags::Boolean, value::bitcastFrom<bool>(false));
        slotIds.push_back(bindAccessor(accessors.back().get()));
    }

    auto expr = makeNaryExpr(EPrimNary::Op::logicOr, slotIds);
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    // All values are false.
    {
        auto [tag, val] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(tag, val);

        TypedValue expected = makeBool(false);
        ASSERT_THAT(std::make_pair(tag, val), ValueEq(expected));
    }

    // One of the values is true.
    for (int truePosition = 0; truePosition < numSlots; truePosition++) {
        accessors[truePosition]->reset(value::TypeTags::Boolean, value::bitcastFrom<bool>(true));

        auto [tag, val] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(tag, val);

        TypedValue expected = makeBool(true);
        ASSERT_THAT(std::make_pair(tag, val), ValueEq(expected));

        accessors[truePosition]->reset(value::TypeTags::Boolean, value::bitcastFrom<bool>(false));
    }

    // One of the values is true and one is nothing.
    for (int nothingPosition = 0; nothingPosition < numSlots; nothingPosition++)
        for (int truePosition = 0; truePosition < numSlots; truePosition++) {
            if (nothingPosition == truePosition)
                continue;

            accessors[nothingPosition]->reset(value::TypeTags::Nothing, 0);
            accessors[truePosition]->reset(value::TypeTags::Boolean,
                                           value::bitcastFrom<bool>(true));


            auto [tag, val] = runCompiledExpression(compiledExpr.get());
            value::ValueGuard guard(tag, val);

            TypedValue expected = nothingPosition < truePosition ? makeNothing() : makeBool(true);
            ASSERT_THAT(std::make_pair(tag, val), ValueEq(expected));

            accessors[truePosition]->reset(value::TypeTags::Boolean,
                                           value::bitcastFrom<bool>(false));

            accessors[nothingPosition]->reset(value::TypeTags::Boolean,
                                              value::bitcastFrom<bool>(false));
        }
}

TEST_F(SBEPrimNaryTest, NaryAdd) {
    auto& os = gctx->outStream();

    std::vector<std::unique_ptr<value::ViewOfValueAccessor>> accessors;
    value::SlotVector slotIds;

    int depth = 3;
    int numSlots = 1 << depth;
    for (int i = 0; i < numSlots; i++) {
        accessors.emplace_back(std::make_unique<value::ViewOfValueAccessor>());
        accessors.back()->reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(i));
        slotIds.push_back(bindAccessor(accessors.back().get()));
    }

    auto expr = makeNaryExpr(EPrimNary::Op::add, slotIds);
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    {
        auto [tag, val] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(tag, val);

        TypedValue expected = makeInt64(28);
        ASSERT_THAT(std::make_pair(tag, val), ValueEq(expected));
    }

    for (int idx = 0; idx < numSlots; ++idx) {
        accessors[idx]->reset(value::TypeTags::Nothing, 0);

        auto [tag, val] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(tag, val);

        TypedValue expected = makeNothing();
        ASSERT_THAT(std::make_pair(tag, val), ValueEq(expected));
        accessors[idx]->reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(idx));
    }

    for (int idx = 0; idx < numSlots; ++idx) {
        accessors[idx]->reset(value::TypeTags::NumberDouble,
                              value::bitcastFrom<double>(static_cast<double>(idx)));

        auto [tag, val] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(tag, val);

        TypedValue expected = makeDouble(28.0);
        ASSERT_THAT(std::make_pair(tag, val), ValueEq(expected));
        accessors[idx]->reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(idx));
    }
}

TEST_F(SBEPrimNaryTest, NaryMult) {
    auto& os = gctx->outStream();

    std::vector<std::unique_ptr<value::ViewOfValueAccessor>> accessors;
    value::SlotVector slotIds;

    int depth = 3;
    int numSlots = 1 << depth;
    for (int i = 0; i < numSlots; i++) {
        accessors.emplace_back(std::make_unique<value::ViewOfValueAccessor>());
        accessors.back()->reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(i + 1));
        slotIds.push_back(bindAccessor(accessors.back().get()));
    }

    auto expr = makeNaryExpr(EPrimNary::Op::mul, slotIds);
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    {
        auto [tag, val] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(tag, val);

        TypedValue expected = makeInt64(40320);
        ASSERT_THAT(std::make_pair(tag, val), ValueEq(expected));
    }

    for (int idx = 0; idx < numSlots; ++idx) {
        accessors[idx]->reset(value::TypeTags::Nothing, 0);

        auto [tag, val] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(tag, val);

        TypedValue expected = makeNothing();
        ASSERT_THAT(std::make_pair(tag, val), ValueEq(expected));
        accessors[idx]->reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(idx + 1));
    }

    for (int idx = 0; idx < numSlots; ++idx) {
        accessors[idx]->reset(value::TypeTags::NumberDouble,
                              value::bitcastFrom<double>(static_cast<double>(idx + 1)));

        auto [tag, val] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(tag, val);

        TypedValue expected = makeDouble(40320.0);
        ASSERT_THAT(std::make_pair(tag, val), ValueEq(expected));
        accessors[idx]->reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(idx + 1));
    }
}

}  // namespace mongo::sbe
