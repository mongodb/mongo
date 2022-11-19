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

#include "mongo/db/exec/sbe/expression_test_base.h"

namespace mongo::sbe {

class SBEPrimBinaryTest : public EExpressionTestFixture {
public:
    std::unique_ptr<EExpression> makeBalancedBinaryExpr(EPrimBinary::Op op,
                                                        int depth,
                                                        const value::SlotVector& slotIds,
                                                        int offset = 0) {
        if (depth > 0) {
            return sbe::makeE<EPrimBinary>(
                op,
                makeBalancedBinaryExpr(op, depth - 1, slotIds, offset),
                makeBalancedBinaryExpr(op, depth - 1, slotIds, offset + (1 << (depth - 1))));
        } else {
            return makeE<EVariable>(slotIds[offset]);
        }
    }
};

TEST_F(SBEPrimBinaryTest, TruthTableAnd) {
    GoldenTestContext gctx(&goldenTestConfigSbe);
    gctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    auto& os = gctx.outStream();

    value::ViewOfValueAccessor lhsAccessor;
    value::ViewOfValueAccessor rhsAccessor;

    auto lhsSlot = bindAccessor(&lhsAccessor);
    auto rhsSlot = bindAccessor(&rhsAccessor);

    auto expr = sbe::makeE<EPrimBinary>(
        EPrimBinary::Op::logicAnd, makeE<EVariable>(lhsSlot), makeE<EVariable>(rhsSlot));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    // Verify the truth table
    std::vector<TypedValue> testValues = {makeNothing(), makeBool(false), makeBool(true)};
    for (auto lhs : testValues)
        for (auto rhs : testValues) {
            lhsAccessor.reset(lhs.first, lhs.second);
            rhsAccessor.reset(rhs.first, rhs.second);
            executeAndPrintVariation(os, *compiledExpr);
        }
}

TEST_F(SBEPrimBinaryTest, TruthTableOr) {
    GoldenTestContext gctx(&goldenTestConfigSbe);
    gctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    auto& os = gctx.outStream();

    value::ViewOfValueAccessor lhsAccessor;
    value::ViewOfValueAccessor rhsAccessor;

    auto lhsSlot = bindAccessor(&lhsAccessor);
    auto rhsSlot = bindAccessor(&rhsAccessor);

    auto expr = sbe::makeE<EPrimBinary>(
        EPrimBinary::Op::logicOr, makeE<EVariable>(lhsSlot), makeE<EVariable>(rhsSlot));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    // Verify the truth table
    std::vector<TypedValue> testValues = {makeNothing(), makeBool(false), makeBool(true)};
    for (auto lhs : testValues)
        for (auto rhs : testValues) {
            lhsAccessor.reset(lhs.first, lhs.second);
            rhsAccessor.reset(rhs.first, rhs.second);
            executeAndPrintVariation(os, *compiledExpr);
        }
}

TEST_F(SBEPrimBinaryTest, BalancedAnd) {
    GoldenTestContext gctx(&goldenTestConfigSbe);
    gctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    auto& os = gctx.outStream();

    std::vector<std::unique_ptr<value::ViewOfValueAccessor>> accessors;
    value::SlotVector slotIds;

    int depth = 3;
    int numSlots = 1 << depth;

    for (int i = 0; i < numSlots; i++) {
        accessors.emplace_back(std::make_unique<value::ViewOfValueAccessor>());
        accessors.back()->reset(value::TypeTags::Boolean, value::bitcastFrom<bool>(true));
        slotIds.push_back(bindAccessor(accessors.back().get()));
    }

    auto expr = makeBalancedBinaryExpr(EPrimBinary::Op::logicAnd, depth, slotIds);
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    // All values are true
    {
        auto [tag, val] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(tag, val);

        TypedValue expected = makeBool(true);
        ASSERT_THAT(std::make_pair(tag, val), ValueEq(expected));
    }

    // One of the values is false
    for (int falsePosition = 0; falsePosition < numSlots; falsePosition++) {
        accessors[falsePosition]->reset(value::TypeTags::Boolean, value::bitcastFrom<bool>(false));

        auto [tag, val] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(tag, val);

        TypedValue expected = makeBool(false);
        ASSERT_THAT(std::make_pair(tag, val), ValueEq(expected));

        accessors[falsePosition]->reset(value::TypeTags::Boolean, value::bitcastFrom<bool>(true));
    }

    // One of the values is true and one is nothing
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


TEST_F(SBEPrimBinaryTest, BalancedOr) {
    GoldenTestContext gctx(&goldenTestConfigSbe);
    gctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    auto& os = gctx.outStream();

    std::vector<std::unique_ptr<value::ViewOfValueAccessor>> accessors;
    value::SlotVector slotIds;

    int depth = 3;
    int numSlots = 1 << depth;

    for (int i = 0; i < numSlots; i++) {
        accessors.emplace_back(std::make_unique<value::ViewOfValueAccessor>());
        accessors.back()->reset(value::TypeTags::Boolean, value::bitcastFrom<bool>(false));
        slotIds.push_back(bindAccessor(accessors.back().get()));
    }

    auto expr = makeBalancedBinaryExpr(EPrimBinary::Op::logicOr, depth, slotIds);
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    // All values are false
    {
        auto [tag, val] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(tag, val);

        TypedValue expected = makeBool(false);
        ASSERT_THAT(std::make_pair(tag, val), ValueEq(expected));
    }

    // One of the values is true
    for (int truePosition = 0; truePosition < numSlots; truePosition++) {
        accessors[truePosition]->reset(value::TypeTags::Boolean, value::bitcastFrom<bool>(true));

        auto [tag, val] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(tag, val);

        TypedValue expected = makeBool(true);
        ASSERT_THAT(std::make_pair(tag, val), ValueEq(expected));

        accessors[truePosition]->reset(value::TypeTags::Boolean, value::bitcastFrom<bool>(false));
    }

    // One of the values is true and one is nothing
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

}  // namespace mongo::sbe
