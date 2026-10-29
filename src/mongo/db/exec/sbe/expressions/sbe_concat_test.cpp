// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace mongo::sbe {
class SBEConcatTest : public EExpressionTestFixture {
protected:
    void runAndAssertExpression(const vm::CodeFragment* compiledExpr,
                                std::string_view expectedVal) {
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));

        ASSERT(value::isString(result.tag()));
        ASSERT_EQUALS(value::getStringView(result.tag(), result.value()), expectedVal);
    }

    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));

        ASSERT_EQUALS(value::TypeTags::Nothing, result.tag());
    }
};

TEST_F(SBEConcatTest, ComputesEmptyStringsConcat) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto argSlot1 = bindAccessor(&slotAccessor1);
    auto argSlot2 = bindAccessor(&slotAccessor2);
    auto concatExpr = sbe::makeE<sbe::EFunction>(
        EFn::kConcat, sbe::makeEs(makeE<EVariable>(argSlot1), makeE<EVariable>(argSlot2)));
    auto compiledExpr = compileExpression(*concatExpr);

    auto [tag1, val1] = value::makeNewString("");
    auto [tag2, val2] = value::makeNewString("");
    slotAccessor1.reset(tag1, val1);
    slotAccessor2.reset(tag2, val2);
    runAndAssertExpression(compiledExpr.get(), "");
}

TEST_F(SBEConcatTest, ComputesSingleStringConcat) {
    value::OwnedValueAccessor slotAccessor1;
    auto argSlot1 = bindAccessor(&slotAccessor1);
    auto concatExpr =
        sbe::makeE<sbe::EFunction>(EFn::kConcat, sbe::makeEs(makeE<EVariable>(argSlot1)));
    auto compiledExpr = compileExpression(*concatExpr);

    auto [tag1, val1] = value::makeNewString("Test");
    slotAccessor1.reset(tag1, val1);
    runAndAssertExpression(compiledExpr.get(), "Test");
}

TEST_F(SBEConcatTest, ComputesStringConcat) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto argSlot1 = bindAccessor(&slotAccessor1);
    auto argSlot2 = bindAccessor(&slotAccessor2);
    auto concatExpr = sbe::makeE<sbe::EFunction>(
        EFn::kConcat, sbe::makeEs(makeE<EVariable>(argSlot1), makeE<EVariable>(argSlot2)));
    auto compiledExpr = compileExpression(*concatExpr);

    auto [tag1, val1] = value::makeNewString("F");
    auto [tag2, val2] = value::makeNewString("1");
    ASSERT_EQUALS(value::TypeTags::StringSmall, tag1);
    slotAccessor1.reset(tag1, val1);
    slotAccessor2.reset(tag2, val2);
    runAndAssertExpression(compiledExpr.get(), "F1");

    auto [bigStringTag1, bigStringVal1] = value::makeNewString("Make sure that ");
    auto [bigStringTag2, bigStringVal2] = value::makeNewString("this is a long string.");
    ASSERT_EQUALS(value::TypeTags::StringBig, bigStringTag1);
    slotAccessor1.reset(bigStringTag1, bigStringVal1);
    slotAccessor2.reset(bigStringTag2, bigStringVal2);
    runAndAssertExpression(compiledExpr.get(), "Make sure that this is a long string.");

    auto bsonString1 = BSON("key" << "bson ");
    auto bsonString2 = BSON("key" << "string");
    auto bsonStringVal1 = value::bitcastFrom<const char*>(bsonString1["key"].value());
    auto bsonStringVal2 = value::bitcastFrom<const char*>(bsonString2["key"].value());
    slotAccessor1.reset(value::TypeTags::bsonString, bsonStringVal1);
    slotAccessor2.reset(value::TypeTags::bsonString, bsonStringVal2);
    runAndAssertExpression(compiledExpr.get(), "bson string");
}

TEST_F(SBEConcatTest, ComputesManyStringsConcat) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    value::OwnedValueAccessor slotAccessor3;
    value::OwnedValueAccessor slotAccessor4;
    auto argSlot1 = bindAccessor(&slotAccessor1);
    auto argSlot2 = bindAccessor(&slotAccessor2);
    auto argSlot3 = bindAccessor(&slotAccessor3);
    auto argSlot4 = bindAccessor(&slotAccessor4);
    auto concatExpr = sbe::makeE<sbe::EFunction>(EFn::kConcat,
                                                 sbe::makeEs(makeE<EVariable>(argSlot1),
                                                             makeE<EVariable>(argSlot2),
                                                             makeE<EVariable>(argSlot3),
                                                             makeE<EVariable>(argSlot4)));
    auto compiledExpr = compileExpression(*concatExpr);

    auto bsonString = BSON("key" << "Test ");
    auto bsonStringVal = value::bitcastFrom<const char*>(bsonString["key"].value());
    auto [tag2, val2] = value::makeNewString("for ");
    auto [tag3, val3] = value::makeNewString("many strings ");
    auto [tag4, val4] = value::makeNewString("concat");
    slotAccessor1.reset(value::TypeTags::bsonString, bsonStringVal);
    slotAccessor2.reset(tag2, val2);
    slotAccessor3.reset(tag3, val3);
    slotAccessor4.reset(tag4, val4);
    runAndAssertExpression(compiledExpr.get(), "Test for many strings concat");
}

TEST_F(SBEConcatTest, ComputesManyMoreStringsConcat) {
    const size_t smallArityLimit = std::numeric_limits<vm::SmallArityType>::max();

    for (auto arity : {smallArityLimit / 2,
                       smallArityLimit,
                       smallArityLimit - 1,
                       smallArityLimit + 1,
                       smallArityLimit * 10}) {
        EExpression::Vector args;
        args.reserve(arity);
        for (size_t idx = 0; idx < arity; ++idx) {
            args.push_back(makeE<EConstant>("x"));
        }

        auto concatExpr = makeE<EFunction>(EFn::kConcat, std::move(args));
        auto compiledExpr = compileExpression(*concatExpr);
        runAndAssertExpression(compiledExpr.get(), std::string(arity, 'x'));
    }
}

TEST_F(SBEConcatTest, ConcatThrowsWhenMemoryLimitExceeded) {
    SimpleMemoryUsageTracker tracker(MemoryUsageLimit{5});
    _vm.setMemoryTracker(&tracker);
    ScopeGuard clearTracker([&] { _vm.setMemoryTracker(nullptr); });

    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto slot1 = bindAccessor(&slotAccessor1);
    auto slot2 = bindAccessor(&slotAccessor2);
    auto expr =
        makeE<EFunction>(EFn::kConcat, makeEs(makeE<EVariable>(slot1), makeE<EVariable>(slot2)));
    auto compiled = compileExpression(*expr);

    auto [t1, v1] = value::makeNewString("hello");
    auto [t2, v2] = value::makeNewString("world");
    slotAccessor1.reset(t1, v1);
    slotAccessor2.reset(t2, v2);

    ASSERT_THROWS_CODE(
        runCompiledExpression(compiled.get()), DBException, ErrorCodes::ExceededMemoryLimit);
}

TEST_F(SBEConcatTest, ReturnsNothingForNonStringsConcat) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto argSlot1 = bindAccessor(&slotAccessor1);
    auto argSlot2 = bindAccessor(&slotAccessor2);
    auto concatExpr = sbe::makeE<sbe::EFunction>(
        EFn::kConcat, sbe::makeEs(makeE<EVariable>(argSlot1), makeE<EVariable>(argSlot2)));
    auto compiledExpr = compileExpression(*concatExpr);

    auto [tag1, val1] = value::makeNewString("abc");
    slotAccessor1.reset(tag1, val1);
    slotAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(100));
    runAndAssertNothing(compiledExpr.get());
}

}  // namespace mongo::sbe
