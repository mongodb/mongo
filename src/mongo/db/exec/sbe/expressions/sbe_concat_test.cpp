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
class SBEConcatTest : public EExpressionTestFixture {
protected:
    void runAndAssertExpression(const vm::CodeFragment* compiledExpr,
                                std::string_view expectedVal) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);

        ASSERT(value::isString(tag));
        ASSERT_EQUALS(value::getStringView(tag, val), expectedVal);
    }

    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);

        ASSERT_EQUALS(value::TypeTags::Nothing, tag);
    }
};

TEST_F(SBEConcatTest, ComputesEmptyStringsConcat) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto argSlot1 = bindAccessor(&slotAccessor1);
    auto argSlot2 = bindAccessor(&slotAccessor2);
    auto concatExpr = sbe::makeE<sbe::EFunction>(
        "concat", sbe::makeEs(makeE<EVariable>(argSlot1), makeE<EVariable>(argSlot2)));
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
    auto concatExpr = sbe::makeE<sbe::EFunction>("concat", sbe::makeEs(makeE<EVariable>(argSlot1)));
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
        "concat", sbe::makeEs(makeE<EVariable>(argSlot1), makeE<EVariable>(argSlot2)));
    auto compiledExpr = compileExpression(*concatExpr);

    auto [tag1, val1] = value::makeSmallString("F");
    auto [tag2, val2] = value::makeSmallString("1");
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

    auto bsonString1 = BSON("key"
                            << "bson ");
    auto bsonString2 = BSON("key"
                            << "string");
    auto bsonStringVal1 = value::bitcastFrom(bsonString1["key"].value());
    auto bsonStringVal2 = value::bitcastFrom(bsonString2["key"].value());
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
    auto concatExpr = sbe::makeE<sbe::EFunction>("concat",
                                                 sbe::makeEs(makeE<EVariable>(argSlot1),
                                                             makeE<EVariable>(argSlot2),
                                                             makeE<EVariable>(argSlot3),
                                                             makeE<EVariable>(argSlot4)));
    auto compiledExpr = compileExpression(*concatExpr);

    auto bsonString = BSON("key"
                           << "Test ");
    auto bsonStringVal = value::bitcastFrom(bsonString["key"].value());
    auto [tag2, val2] = value::makeSmallString("for ");
    auto [tag3, val3] = value::makeNewString("many strings ");
    auto [tag4, val4] = value::makeSmallString("concat");
    slotAccessor1.reset(value::TypeTags::bsonString, bsonStringVal);
    slotAccessor2.reset(tag2, val2);
    slotAccessor3.reset(tag3, val3);
    slotAccessor4.reset(tag4, val4);
    runAndAssertExpression(compiledExpr.get(), "Test for many strings concat");
}

TEST_F(SBEConcatTest, ReturnsNothingForNonStringsConcat) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto argSlot1 = bindAccessor(&slotAccessor1);
    auto argSlot2 = bindAccessor(&slotAccessor2);
    auto concatExpr = sbe::makeE<sbe::EFunction>(
        "concat", sbe::makeEs(makeE<EVariable>(argSlot1), makeE<EVariable>(argSlot2)));
    auto compiledExpr = compileExpression(*concatExpr);

    auto [tag1, val1] = value::makeNewString("abc");
    slotAccessor1.reset(tag1, val1);
    slotAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{100}));
    runAndAssertNothing(compiledExpr.get());
}

}  // namespace mongo::sbe
