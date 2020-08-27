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

#include "mongo/db/exec/sbe/expression_test_base.h"

namespace mongo::sbe {
using SBEBsonSizeTest = EExpressionTestFixture;

TEST_F(SBEBsonSizeTest, ComputesSizeForBsonDocument) {
    value::ViewOfValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto bsonSizeExpr =
        sbe::makeE<sbe::EFunction>("bsonSize", sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*bsonSizeExpr);

    BSONObjBuilder objBuilder;
    objBuilder.append("name", "Test string element");
    objBuilder.append("age", 32);
    objBuilder.append("citizen", true);
    auto bsonObj = objBuilder.done();

    slotAccessor.reset(value::TypeTags::bsonObject, value::bitcastFrom(bsonObj.objdata()));
    auto [tag, val] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(tag, val);

    ASSERT_EQUALS(value::TypeTags::NumberInt32, tag);
    ASSERT_EQUALS(value::bitcastTo<uint32_t>(val), bsonObj.objsize());
}

TEST_F(SBEBsonSizeTest, ComputesSizeForSbeObject) {
    value::ViewOfValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto bsonSizeExpr =
        sbe::makeE<sbe::EFunction>("bsonSize", sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*bsonSizeExpr);

    auto [tagArg1, valArg1] = value::makeNewString("Test string element");
    auto [tagArg2, valArg2] = value::makeNewObject();
    auto obj = value::getObjectView(valArg2);
    obj->push_back("name", tagArg1, valArg1);
    obj->push_back("age", value::TypeTags::NumberInt32, value::bitcastFrom(32));
    obj->push_back("citizen", value::TypeTags::Boolean, value::bitcastFrom(true));
    value::ValueGuard argGuard(tagArg2, valArg2);

    slotAccessor.reset(value::TypeTags::Object, valArg2);
    auto [tag, val] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(tag, val);

    ASSERT_EQUALS(value::TypeTags::NumberInt32, tag);
    ASSERT_EQUALS(value::bitcastTo<uint32_t>(val), 54);
}

TEST_F(SBEBsonSizeTest, ReturnsNothingForNonObject) {
    value::ViewOfValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto bsonSizeExpr =
        sbe::makeE<sbe::EFunction>("bsonSize", sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*bsonSizeExpr);

    auto [tagArg1, valArg1] = value::makeNewString("Test string element");
    value::ValueGuard guard1(tagArg1, valArg1);

    auto [tagArg2, valArg2] = value::makeNewArray();
    value::ValueGuard guard2(tagArg2, valArg2);
    auto arr = value::getArrayView(valArg2);
    arr->push_back(value::TypeTags::NumberInt32, value::bitcastFrom(32));
    auto [tagArg3, valArg3] = value::copyValue(tagArg1, valArg1);
    arr->push_back(tagArg3, valArg3);

    std::vector<std::pair<value::TypeTags, value::Value>> testData = {
        std::make_pair(value::TypeTags::NumberInt32, value::bitcastFrom(12789)),
        std::make_pair(tagArg1, valArg1),
        std::make_pair(tagArg2, valArg2)};

    for (size_t i = 0; i < testData.size(); i++) {
        slotAccessor.reset(testData[i].first, testData[i].second);
        auto [tag, val] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(tag, val);
        ASSERT_EQUALS(value::TypeTags::Nothing, tag);
    }
}

}  // namespace mongo::sbe
