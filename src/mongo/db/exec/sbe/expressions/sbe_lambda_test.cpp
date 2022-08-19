/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
using SBELambdaTest = EExpressionTestFixture;

TEST_F(SBELambdaTest, AddOneToArray) {
    value::ViewOfValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    FrameId frame = 10;
    auto [constTag, constVal] = makeInt32(1);
    auto lambdaExpr = sbe::makeE<sbe::EFunction>(
        "traverseP",
        sbe::makeEs(makeE<EVariable>(argSlot),
                    makeE<ELocalLambda>(frame,
                                        makeE<EPrimBinary>(EPrimBinary::Op::add,
                                                           makeE<EVariable>(frame, 0),
                                                           makeE<EConstant>(constTag, constVal))),
                    makeE<EConstant>(value::TypeTags::Nothing, 0)));
    auto compiledExpr = compileExpression(*lambdaExpr);
    auto bsonArr = BSON_ARRAY(1 << 2 << 3);

    slotAccessor.reset(value::TypeTags::bsonArray,
                       value::bitcastFrom<const char*>(bsonArr.objdata()));
    auto [tag, val] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(tag, val);

    auto resultArr = BSON_ARRAY(2 << 3 << 4);

    ASSERT_EQUALS(value::TypeTags::Array, tag);
    auto [compareTag, compareValue] = value::compareValue(
        tag, val, value::TypeTags::bsonArray, value::bitcastFrom<const char*>(resultArr.objdata()));
    ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
    ASSERT_EQ(compareValue, 0);
}

}  // namespace mongo::sbe
