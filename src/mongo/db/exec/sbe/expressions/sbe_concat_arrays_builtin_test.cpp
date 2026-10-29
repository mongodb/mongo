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

class SBEBuiltinConcatArraysTest : public EExpressionTestFixture {
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

TEST_F(SBEBuiltinConcatArraysTest, ConcatArraysWithinLimitSucceeds) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto concatExpr =
        makeFunction(EFn::kConcatArrays, makeVariable(arrSlot1), makeVariable(arrSlot2));
    auto compiledExpr = compileExpression(*concatExpr);

    auto [arrTag1, arrVal1] = makeArray(makeLargeBsonArray(5, 1024));
    slotAccessor1.reset(arrTag1, arrVal1);
    auto [arrTag2, arrVal2] = makeArray(makeLargeBsonArray(5, 1024));
    slotAccessor2.reset(arrTag2, arrVal2);

    // 2 arrays * 5 elements * 1024 bytes = ~10KB; limit set well above that.
    unittest::ServerParameterGuard limit{"internalQueryMaxSingleExpressionMemoryUsageBytes",
                                         20 * 1024};
    auto [resTag, resVal] = runCompiledExpression(compiledExpr.get());
    value::TagValueOwned guard = value::TagValueOwned::fromRaw(resTag, resVal);

    ASSERT(value::isArray(resTag));
    ASSERT_EQUALS(value::getArrayView(resVal)->size(), 10u);
}

TEST_F(SBEBuiltinConcatArraysTest, ConcatArraysExceedsMemoryLimit) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto concatExpr =
        makeFunction(EFn::kConcatArrays, makeVariable(arrSlot1), makeVariable(arrSlot2));
    auto compiledExpr = compileExpression(*concatExpr);

    auto [arrTag1, arrVal1] = makeArray(makeLargeBsonArray(5, 1024));
    slotAccessor1.reset(arrTag1, arrVal1);
    auto [arrTag2, arrVal2] = makeArray(makeLargeBsonArray(5, 1024));
    slotAccessor2.reset(arrTag2, arrVal2);

    unittest::ServerParameterGuard limit{"internalQueryMaxSingleExpressionMemoryUsageBytes",
                                         10 * 1024};
    ASSERT_THROWS_CODE(runCompiledExpression(compiledExpr.get()),
                       AssertionException,
                       ErrorCodes::ExceededMemoryLimit);
}

}  // namespace mongo::sbe
