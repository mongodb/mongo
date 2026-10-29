// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

    void runNaryOpTest(std::ostream& os,
                       EPrimNary::Op op,
                       const std::vector<value::TagValueOwned>& testValues) {
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
        for (const auto& lhs : testValues)
            for (const auto& rhs : testValues) {
                lhsAccessor.reset(lhs.tag(), lhs.value());
                rhsAccessor.reset(rhs.tag(), rhs.value());
                executeAndPrintVariation(os, *compiledExpr);
            }
    }

protected:
    std::vector<value::TagValueOwned> boolTestValues =
        makeOwnedVector({makeNothing(), makeBool(false), makeBool(true)});
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
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        TypedValue expected = makeBool(true);
        ASSERT_THAT(std::make_pair(result.tag(), result.value()), ValueEq(expected));
    }

    // One of the values is false.
    for (int falsePosition = 0; falsePosition < numSlots; falsePosition++) {
        accessors[falsePosition]->reset(value::TypeTags::Boolean, value::bitcastFrom<bool>(false));

        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        TypedValue expected = makeBool(false);
        ASSERT_THAT(std::make_pair(result.tag(), result.value()), ValueEq(expected));

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


            value::TagValueOwned result =
                value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

            TypedValue expected = nothingPosition < falsePosition ? makeNothing() : makeBool(false);
            ASSERT_THAT(std::make_pair(result.tag(), result.value()), ValueEq(expected));

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
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        TypedValue expected = makeBool(false);
        ASSERT_THAT(std::make_pair(result.tag(), result.value()), ValueEq(expected));
    }

    // One of the values is true.
    for (int truePosition = 0; truePosition < numSlots; truePosition++) {
        accessors[truePosition]->reset(value::TypeTags::Boolean, value::bitcastFrom<bool>(true));

        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        TypedValue expected = makeBool(true);
        ASSERT_THAT(std::make_pair(result.tag(), result.value()), ValueEq(expected));

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


            value::TagValueOwned result =
                value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

            TypedValue expected = nothingPosition < truePosition ? makeNothing() : makeBool(true);
            ASSERT_THAT(std::make_pair(result.tag(), result.value()), ValueEq(expected));

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
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        TypedValue expected = makeInt64(28);
        ASSERT_THAT(std::make_pair(result.tag(), result.value()), ValueEq(expected));
    }

    for (int idx = 0; idx < numSlots; ++idx) {
        accessors[idx]->reset(value::TypeTags::Nothing, 0);

        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        TypedValue expected = makeNothing();
        ASSERT_THAT(std::make_pair(result.tag(), result.value()), ValueEq(expected));
        accessors[idx]->reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(idx));
    }

    for (int idx = 0; idx < numSlots; ++idx) {
        accessors[idx]->reset(value::TypeTags::NumberDouble,
                              value::bitcastFrom<double>(static_cast<double>(idx)));

        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        TypedValue expected = makeDouble(28.0);
        ASSERT_THAT(std::make_pair(result.tag(), result.value()), ValueEq(expected));
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
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        TypedValue expected = makeInt64(40320);
        ASSERT_THAT(std::make_pair(result.tag(), result.value()), ValueEq(expected));
    }

    for (int idx = 0; idx < numSlots; ++idx) {
        accessors[idx]->reset(value::TypeTags::Nothing, 0);

        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        TypedValue expected = makeNothing();
        ASSERT_THAT(std::make_pair(result.tag(), result.value()), ValueEq(expected));
        accessors[idx]->reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(idx + 1));
    }

    for (int idx = 0; idx < numSlots; ++idx) {
        accessors[idx]->reset(value::TypeTags::NumberDouble,
                              value::bitcastFrom<double>(static_cast<double>(idx + 1)));

        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        TypedValue expected = makeDouble(40320.0);
        ASSERT_THAT(std::make_pair(result.tag(), result.value()), ValueEq(expected));
        accessors[idx]->reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(idx + 1));
    }
}

}  // namespace mongo::sbe
