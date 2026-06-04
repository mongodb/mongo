/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/idl/server_parameter_test_controller.h"
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
    auto concatExpr = makeFunction("concatArrays", makeVariable(arrSlot1), makeVariable(arrSlot2));
    auto compiledExpr = compileExpression(*concatExpr);

    auto [arrTag1, arrVal1] = makeArray(makeLargeBsonArray(5, 1024));
    slotAccessor1.reset(arrTag1, arrVal1);
    auto [arrTag2, arrVal2] = makeArray(makeLargeBsonArray(5, 1024));
    slotAccessor2.reset(arrTag2, arrVal2);

    // 2 arrays * 5 elements * 1024 bytes = ~10KB; limit set well above that.
    RAIIServerParameterControllerForTest limit("internalQueryMaxExpressionOutputBytes", 20 * 1024);
    auto [resTag, resVal] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(resTag, resVal);

    ASSERT(value::isArray(resTag));
    ASSERT_EQUALS(value::getArraySize(resTag, resVal), 10u);
}

TEST_F(SBEBuiltinConcatArraysTest, ConcatArraysExceedsMemoryLimit) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto concatExpr = makeFunction("concatArrays", makeVariable(arrSlot1), makeVariable(arrSlot2));
    auto compiledExpr = compileExpression(*concatExpr);

    auto [arrTag1, arrVal1] = makeArray(makeLargeBsonArray(5, 1024));
    slotAccessor1.reset(arrTag1, arrVal1);
    auto [arrTag2, arrVal2] = makeArray(makeLargeBsonArray(5, 1024));
    slotAccessor2.reset(arrTag2, arrVal2);

    RAIIServerParameterControllerForTest limit("internalQueryMaxExpressionOutputBytes", 10 * 1024);
    ASSERT_THROWS_CODE(runCompiledExpression(compiledExpr.get()),
                       AssertionException,
                       ErrorCodes::ExceededMemoryLimit);
}

}  // namespace mongo::sbe
