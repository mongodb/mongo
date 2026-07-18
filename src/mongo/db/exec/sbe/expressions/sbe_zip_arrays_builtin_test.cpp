// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>

namespace mongo::sbe {

// The builtin zipArrays takes the following arguments in order:
//   1) int32 input size (number of input arrays)
//   2) bool useLongestLength
//   3..N) input array slots
//   N+1..M) optional default values (same count as input arrays)
// See ExpressionZip visitor in gen_expression.cpp for reference.
class SBEBuiltinZipArraysTest : public EExpressionTestFixture {
protected:
    static BSONArray makeLargeBsonArray(int numElements, size_t valueSizeBytes) {
        BSONArrayBuilder builder;
        const std::string value(valueSizeBytes, 'a');
        for (int i = 0; i < numElements; ++i) {
            builder.append(value);
        }
        return builder.arr();
    }
};

TEST_F(SBEBuiltinZipArraysTest, ZipArraysWithinLimitSucceeds) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);

    EExpression::Vector args;
    args.emplace_back(makeInt32Constant(2));
    args.emplace_back(makeBoolConstant(false));
    args.emplace_back(makeVariable(arrSlot1));
    args.emplace_back(makeVariable(arrSlot2));
    auto zipExpr = makeFunction(EFn::kZipArrays, std::move(args));
    auto compiledExpr = compileExpression(*zipExpr);

    auto [arrTag1, arrVal1] = makeArray(makeLargeBsonArray(10, 1024));
    slotAccessor1.reset(arrTag1, arrVal1);
    auto [arrTag2, arrVal2] = makeArray(makeLargeBsonArray(10, 1024));
    slotAccessor2.reset(arrTag2, arrVal2);

    // 2 arrays * 10 elements * 1024 bytes zipped = ~20KB; limit set well above that.
    unittest::ServerParameterGuard limit{"internalQueryMaxSingleExpressionMemoryUsageBytes",
                                         30 * 1024};
    auto [resTag, resVal] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(resTag, resVal);

    ASSERT(value::isArray(resTag));
    ASSERT_EQUALS(value::getArrayView(resVal)->size(), 10u);
}

TEST_F(SBEBuiltinZipArraysTest, ZipArraysExceedsMemoryLimit) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);

    EExpression::Vector args;
    args.emplace_back(makeInt32Constant(2));
    args.emplace_back(makeBoolConstant(false));
    args.emplace_back(makeVariable(arrSlot1));
    args.emplace_back(makeVariable(arrSlot2));
    auto zipExpr = makeFunction(EFn::kZipArrays, std::move(args));
    auto compiledExpr = compileExpression(*zipExpr);

    auto [arrTag1, arrVal1] = makeArray(makeLargeBsonArray(10, 1024));
    slotAccessor1.reset(arrTag1, arrVal1);
    auto [arrTag2, arrVal2] = makeArray(makeLargeBsonArray(10, 1024));
    slotAccessor2.reset(arrTag2, arrVal2);

    unittest::ServerParameterGuard limit{"internalQueryMaxSingleExpressionMemoryUsageBytes",
                                         10 * 1024};
    ASSERT_THROWS_CODE(runCompiledExpression(compiledExpr.get()),
                       AssertionException,
                       ErrorCodes::ExceededMemoryLimit);
}

}  // namespace mongo::sbe
