// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <ostream>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo::sbe {
using namespace std::literals::string_view_literals;

class SBEPrimBinaryTest : public GoldenEExpressionTestFixture {
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

    void runBinaryOpTest(std::ostream& os,
                         EPrimBinary::Op op,
                         const std::vector<value::TagValueOwned>& testValues) {
        value::ViewOfValueAccessor lhsAccessor;
        value::ViewOfValueAccessor rhsAccessor;
        auto lhsSlot = bindAccessor(&lhsAccessor);
        auto rhsSlot = bindAccessor(&rhsAccessor);

        auto expr =
            sbe::makeE<EPrimBinary>(op, makeE<EVariable>(lhsSlot), makeE<EVariable>(rhsSlot));
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

    void runBinaryOpCollationTest(std::ostream& os,
                                  EPrimBinary::Op op,
                                  const std::vector<value::TagValueOwned>& testValues,
                                  const std::vector<value::TagValueOwned>& collValues) {
        value::ViewOfValueAccessor lhsAccessor;
        value::ViewOfValueAccessor rhsAccessor;
        value::ViewOfValueAccessor collAccessor;

        auto lhsSlot = bindAccessor(&lhsAccessor);
        auto rhsSlot = bindAccessor(&rhsAccessor);
        auto collSlot = bindAccessor(&collAccessor);

        auto expr = sbe::makeE<EPrimBinary>(
            op, makeE<EVariable>(lhsSlot), makeE<EVariable>(rhsSlot), makeE<EVariable>(collSlot));
        printInputExpression(os, *expr);

        auto compiledExpr = compileExpression(*expr);
        printCompiledExpression(os, *compiledExpr);

        // Verify the operator table.
        for (const auto& lhs : testValues)
            for (const auto& rhs : testValues)
                for (const auto& coll : collValues) {
                    lhsAccessor.reset(lhs.tag(), lhs.value());
                    rhsAccessor.reset(rhs.tag(), rhs.value());
                    collAccessor.reset(coll.tag(), coll.value());
                    executeAndPrintVariation(os, *compiledExpr);
                }
    }

protected:
    std::vector<value::TagValueOwned> boolTestValues =
        makeOwnedVector({makeNothing(), makeBool(false), makeBool(true)});

    std::vector<value::TagValueOwned> numericTestValues =
        makeOwnedVector({makeNothing(),
                         makeInt32(12),
                         makeInt32(23),
                         makeInt64(123),
                         makeDouble(123.5),
                         value::makeCopyDecimal(Decimal128(223.5))});

    std::vector<value::TagValueOwned> mixedTestValues =
        makeOwnedVector({makeNothing(),
                         makeNull(),
                         makeBool(false),
                         makeBool(true),
                         makeInt32(12),
                         value::makeCopyDecimal(Decimal128(223.5)),
                         value::makeNewString("abc"sv),
                         makeTimestamp(Timestamp(1668792433))});

    std::vector<value::TagValueOwned> stringTestValues =
        makeOwnedVector({makeNothing(),
                         value::makeNewString("abc"),
                         value::makeNewString("ABC"),
                         value::makeNewString("abcdefghijkop"),
                         value::makeNewString("ABCDEFGHIJKOP")});

    std::vector<value::TagValueOwned> collTestValues =
        makeOwnedVector({makeNothing(),
                         value::makeCopyCollator(
                             CollatorInterfaceMock(CollatorInterfaceMock::MockType::kAlwaysEqual)),
                         value::makeCopyCollator(CollatorInterfaceMock(
                             CollatorInterfaceMock::MockType::kToLowerString))});
};

/* Logic Operators */

TEST_F(SBEPrimBinaryTest, TruthTableAnd) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::logicAnd, boolTestValues);
}

TEST_F(SBEPrimBinaryTest, TruthTableOr) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::logicOr, boolTestValues);
}

TEST_F(SBEPrimBinaryTest, BalancedAnd) {
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

    auto expr = makeBalancedBinaryExpr(EPrimBinary::Op::logicAnd, depth, slotIds);
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

TEST_F(SBEPrimBinaryTest, BalancedOr) {
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

    auto expr = makeBalancedBinaryExpr(EPrimBinary::Op::logicOr, depth, slotIds);
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

/* Arithmetic operators - Numeric */
TEST_F(SBEPrimBinaryTest, AddNumeric) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::add, numericTestValues);
}

TEST_F(SBEPrimBinaryTest, SubNumeric) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::sub, numericTestValues);
}

TEST_F(SBEPrimBinaryTest, MulNumeric) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::mul, numericTestValues);
}

TEST_F(SBEPrimBinaryTest, DivNumeric) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::div, numericTestValues);
}

/* Arithmetic operators - Mixed */

TEST_F(SBEPrimBinaryTest, AddMixed) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::add, mixedTestValues);
}

TEST_F(SBEPrimBinaryTest, SubMixed) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::sub, mixedTestValues);
}

TEST_F(SBEPrimBinaryTest, MulMixed) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::mul, mixedTestValues);
}

TEST_F(SBEPrimBinaryTest, DivMixed) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::div, mixedTestValues);
}

/* Comparison operators - Numeric*/

TEST_F(SBEPrimBinaryTest, EqNumeric) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::eq, numericTestValues);
}

TEST_F(SBEPrimBinaryTest, NeqNumeric) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::neq, numericTestValues);
}

TEST_F(SBEPrimBinaryTest, LessNumeric) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::less, numericTestValues);
}

TEST_F(SBEPrimBinaryTest, LessEqMixed) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::lessEq, mixedTestValues);
}

TEST_F(SBEPrimBinaryTest, GreaterNumeric) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::greater, numericTestValues);
}

TEST_F(SBEPrimBinaryTest, GreaterEqNumeric) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::greaterEq, numericTestValues);
}

TEST_F(SBEPrimBinaryTest, Cmp3wNumeric) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::cmp3w, numericTestValues);
}

/* Comparison operators - Mixed */

TEST_F(SBEPrimBinaryTest, EqMixed) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::eq, mixedTestValues);
}

TEST_F(SBEPrimBinaryTest, NeqMixed) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::neq, mixedTestValues);
}

TEST_F(SBEPrimBinaryTest, LessMixed) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::less, mixedTestValues);
}

TEST_F(SBEPrimBinaryTest, LessEqNumeric) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::lessEq, numericTestValues);
}

TEST_F(SBEPrimBinaryTest, GreaterMixed) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::greater, mixedTestValues);
}

TEST_F(SBEPrimBinaryTest, GreaterEqMixed) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::greaterEq, mixedTestValues);
}

TEST_F(SBEPrimBinaryTest, Cmp3wMixed) {
    auto& os = gctx->outStream();
    runBinaryOpTest(os, EPrimBinary::Op::cmp3w, mixedTestValues);
}

/* Comparison operators - String */

TEST_F(SBEPrimBinaryTest, EqString) {
    auto& os = gctx->outStream();
    runBinaryOpCollationTest(os, EPrimBinary::Op::eq, stringTestValues, collTestValues);
}

TEST_F(SBEPrimBinaryTest, NeqString) {
    auto& os = gctx->outStream();
    runBinaryOpCollationTest(os, EPrimBinary::Op::neq, stringTestValues, collTestValues);
}

TEST_F(SBEPrimBinaryTest, LessString) {
    auto& os = gctx->outStream();
    runBinaryOpCollationTest(os, EPrimBinary::Op::less, stringTestValues, collTestValues);
}

TEST_F(SBEPrimBinaryTest, LessEqString) {
    auto& os = gctx->outStream();
    runBinaryOpCollationTest(os, EPrimBinary::Op::lessEq, stringTestValues, collTestValues);
}

TEST_F(SBEPrimBinaryTest, GreaterString) {
    auto& os = gctx->outStream();
    runBinaryOpCollationTest(os, EPrimBinary::Op::greater, stringTestValues, collTestValues);
}

TEST_F(SBEPrimBinaryTest, GreaterEqString) {
    auto& os = gctx->outStream();
    runBinaryOpCollationTest(os, EPrimBinary::Op::greaterEq, stringTestValues, collTestValues);
}

TEST_F(SBEPrimBinaryTest, Cmp3wString) {
    auto& os = gctx->outStream();
    runBinaryOpCollationTest(os, EPrimBinary::Op::cmp3w, stringTestValues, collTestValues);
}

/* Misc operators */

TEST_F(SBEPrimBinaryTest, FillEmpty) {
    auto& os = gctx->outStream();
    auto testValues = makeOwnedVector({makeNothing(), makeNull(), makeBool(false), makeBool(true)});
    runBinaryOpTest(os, EPrimBinary::Op::fillEmpty, testValues);
}

TEST_F(SBEPrimBinaryTest, FillEmptyWithConstant) {
    auto& os = gctx->outStream();

    auto testValues = makeOwnedVector({makeNothing(), makeNull(), makeBool(true), makeBool(true)});
    for (const auto& rhs : testValues) {

        os << "== VARIATION rhs constant: " << rhs.view() << std::endl;

        value::ViewOfValueAccessor lhsAccessor;
        auto lhsSlot = bindAccessor(&lhsAccessor);

        auto expr = sbe::makeE<EPrimBinary>(
            EPrimBinary::Op::fillEmpty, makeE<EVariable>(lhsSlot), makeC(rhs.tag(), rhs.value()));
        printInputExpression(os, *expr);

        auto compiledExpr = compileExpression(*expr);
        printCompiledExpression(os, *compiledExpr);

        // Verify the combination table.
        for (const auto& lhs : testValues) {

            lhsAccessor.reset(lhs.tag(), lhs.value());
            executeAndPrintVariation(os, *compiledExpr);
        }
    }
}

/* Regression tests */

TEST_F(SBEPrimBinaryTest, DivMemory) {
    // Regression test for https://jira.mongodb.org/browse/SERVER-71527
    auto& os = gctx->outStream();
    auto expr = sbe::makeE<EPrimBinary>(
        EPrimBinary::Op::div,
        sbe::makeE<EPrimBinary>(EPrimBinary::Op::add,
                                makeC(value::makeCopyDecimal(Decimal128(1))),
                                makeC(value::makeCopyDecimal(Decimal128(1)))),
        sbe::makeE<EPrimBinary>(EPrimBinary::Op::sub,
                                makeC(value::makeCopyDecimal(Decimal128(1))),
                                makeC(value::makeCopyDecimal(Decimal128(1)))));
    printInputExpression(os, *expr);
    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);
    executeAndPrintVariation(os, *compiledExpr);
}

}  // namespace mongo::sbe
