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

class SBEReplaceOneExprTest : public EExpressionTestFixture {
protected:
    void runAndAssertExpression(const vm::CodeFragment* compiledExpr,
                                const std::string& expectedVal) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);
        ASSERT_TRUE(sbe::value::isString(tag));
        ASSERT_EQUALS(sbe::value::getStringView(tag, val), expectedVal);
    }

    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);
        ASSERT_EQUALS(value::TypeTags::Nothing, tag);
    }

    void bindStringToSlot(value::OwnedValueAccessor& slot, const std::string& str) {
        auto [tag, val] = sbe::value::makeNewString(str);
        slot.reset(tag, val);
    }

    void bindStringToSlot(value::OwnedValueAccessor& slot,
                          value::TypeTags expectedTag,
                          const std::string& str) {
        auto [tag, val] = sbe::value::makeNewString(str);
        ASSERT_EQUALS(expectedTag, tag);
        slot.reset(tag, val);
    }
};

TEST_F(SBEReplaceOneExprTest, EmptyStrings) {
    value::OwnedValueAccessor inputAccessor, findAccessor, replaceAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto findSlot = bindAccessor(&findAccessor);
    auto replaceSlot = bindAccessor(&replaceAccessor);
    auto replaceOneExpr = sbe::makeE<sbe::EFunction>("replaceOne",
                                                     sbe::makeEs(makeE<EVariable>(inputSlot),
                                                                 makeE<EVariable>(findSlot),
                                                                 makeE<EVariable>(replaceSlot)));
    auto compiledExpr = compileExpression(*replaceOneExpr);

    auto bindSlots = [&](const std::string& inputStr,
                         const std::string& findStr,
                         const std::string& replaceStr) {
        bindStringToSlot(inputAccessor, inputStr);
        bindStringToSlot(findAccessor, findStr);
        bindStringToSlot(replaceAccessor, replaceStr);
    };

    // Builtin should return nothing when find string is empty.

    bindSlots("", "", "");
    runAndAssertNothing(compiledExpr.get());

    bindSlots("aaa", "", "");
    runAndAssertNothing(compiledExpr.get());

    bindSlots("", "", "aaa");
    runAndAssertNothing(compiledExpr.get());

    bindSlots("baaababaaa", "", "aaa");
    runAndAssertNothing(compiledExpr.get());

    bindSlots("", "aaa", "");
    runAndAssertExpression(compiledExpr.get(), "");

    bindSlots("", "aaa", "baaababaaa");
    runAndAssertExpression(compiledExpr.get(), "");

    bindSlots("baaababaaa", "aaa", "");
    runAndAssertExpression(compiledExpr.get(), "bbabaaa");
}

TEST_F(SBEReplaceOneExprTest, SmallStrings) {
    value::OwnedValueAccessor inputAccessor, findAccessor, replaceAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto findSlot = bindAccessor(&findAccessor);
    auto replaceSlot = bindAccessor(&replaceAccessor);
    auto replaceOneExpr = sbe::makeE<sbe::EFunction>("replaceOne",
                                                     sbe::makeEs(makeE<EVariable>(inputSlot),
                                                                 makeE<EVariable>(findSlot),
                                                                 makeE<EVariable>(replaceSlot)));
    auto compiledExpr = compileExpression(*replaceOneExpr);

    auto bindSlots = [&](const std::string& inputStr,
                         const std::string& findStr,
                         const std::string& replaceStr) {
        bindStringToSlot(inputAccessor, value::TypeTags::StringSmall, inputStr);
        bindStringToSlot(findAccessor, value::TypeTags::StringSmall, findStr);
        bindStringToSlot(replaceAccessor, value::TypeTags::StringSmall, replaceStr);
    };

    // Test find and replace string.
    bindSlots("AA", "A", "B");
    runAndAssertExpression(compiledExpr.get(), "BA");

    // Test not finding string.
    bindSlots("AA", "B", "A");
    runAndAssertExpression(compiledExpr.get(), "AA");
}

TEST_F(SBEReplaceOneExprTest, BigStrings) {
    value::OwnedValueAccessor inputAccessor, findAccessor, replaceAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto findSlot = bindAccessor(&findAccessor);
    auto replaceSlot = bindAccessor(&replaceAccessor);
    auto replaceOneExpr = sbe::makeE<sbe::EFunction>("replaceOne",
                                                     sbe::makeEs(makeE<EVariable>(inputSlot),
                                                                 makeE<EVariable>(findSlot),
                                                                 makeE<EVariable>(replaceSlot)));
    auto compiledExpr = compileExpression(*replaceOneExpr);

    auto bindSlots = [&](const std::string& inputStr,
                         const std::string& findStr,
                         const std::string& replaceStr) {
        bindStringToSlot(inputAccessor, value::TypeTags::StringBig, inputStr);
        bindStringToSlot(findAccessor, value::TypeTags::StringBig, findStr);
        bindStringToSlot(replaceAccessor, value::TypeTags::StringBig, replaceStr);
    };

    // Test find and replace string.
    bindSlots("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
              "aaaaaaaaaaaaaaaaaaaaaaaa",
              "bbbbbbbbbbbbbbbbbbbbbbbb");
    runAndAssertExpression(compiledExpr.get(), "bbbbbbbbbbbbbbbbbbbbbbbbaaaaaaaaaaaaaaaaaaaaaaaaa");

    // Test not finding string.
    bindSlots("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
              "bbbbbbbbbbbbbbbbbbbbbbbb",
              "aaaaaaaaaaaaaaaaaaaaaaaa");
    runAndAssertExpression(compiledExpr.get(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
}

TEST_F(SBEReplaceOneExprTest, BsonStrings) {
    value::OwnedValueAccessor inputAccessor, findAccessor, replaceAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto findSlot = bindAccessor(&findAccessor);
    auto replaceSlot = bindAccessor(&replaceAccessor);
    auto replaceOneExpr = sbe::makeE<sbe::EFunction>("replaceOne",
                                                     sbe::makeEs(makeE<EVariable>(inputSlot),
                                                                 makeE<EVariable>(findSlot),
                                                                 makeE<EVariable>(replaceSlot)));
    auto compiledExpr = compileExpression(*replaceOneExpr);

    auto bindSlots = [&](const BSONObj& bson) {
        auto inputVal = value::bitcastFrom<const char*>(bson["in"].value());
        inputAccessor.reset(false, value::TypeTags::bsonString, inputVal);

        auto findVal = value::bitcastFrom<const char*>(bson["find"].value());
        findAccessor.reset(false, value::TypeTags::bsonString, findVal);

        auto replaceVal = value::bitcastFrom<const char*>(bson["replace"].value());
        replaceAccessor.reset(false, value::TypeTags::bsonString, replaceVal);
    };

    // Test find and replace string.
    auto bson = BSON("in"
                     << "this is a string"
                     << "find"
                     << "is"
                     << "replace"
                     << "at");
    bindSlots(bson);
    runAndAssertExpression(compiledExpr.get(), "that is a string");

    // Test not finding string.
    bson = BSON("in"
                << "this is a string"
                << "find"
                << "at"
                << "replace"
                << "is");
    bindSlots(bson);
    runAndAssertExpression(compiledExpr.get(), "this is a string");
}

TEST_F(SBEReplaceOneExprTest, ComputesNothingIfNotString) {
    value::OwnedValueAccessor inputAccessor, findAccessor, replaceAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto findSlot = bindAccessor(&findAccessor);
    auto replaceSlot = bindAccessor(&replaceAccessor);
    auto replaceOneExpr = sbe::makeE<sbe::EFunction>("replaceOne",
                                                     sbe::makeEs(makeE<EVariable>(inputSlot),
                                                                 makeE<EVariable>(findSlot),
                                                                 makeE<EVariable>(replaceSlot)));
    auto compiledExpr = compileExpression(*replaceOneExpr);

    auto [strTypeTag, strValue] = sbe::value::makeNewString("a str");
    const Decimal128 dec(123.4);

    inputAccessor.reset(strTypeTag, strValue);
    findAccessor.reset(strTypeTag, strValue);
    replaceAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(76));
    runAndAssertNothing(compiledExpr.get());

    inputAccessor.reset(strTypeTag, strValue);
    findAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(21987654));
    replaceAccessor.reset(strTypeTag, strValue);
    runAndAssertNothing(compiledExpr.get());

    auto [decTypeTag, decValue] = value::makeCopyDecimal(dec);
    inputAccessor.reset(decTypeTag, decValue);
    findAccessor.reset(strTypeTag, strValue);
    replaceAccessor.reset(strTypeTag, strValue);
    runAndAssertNothing(compiledExpr.get());

    auto [objTypeTag, objValue] = value::makeNewObject();
    inputAccessor.reset(objTypeTag, objValue);
    auto [arrTypeTag, arrValue] = value::makeNewArray();
    findAccessor.reset(arrTypeTag, arrValue);
    replaceAccessor.reset(strTypeTag, strValue);
    runAndAssertNothing(compiledExpr.get());

    inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(1.0));
    findAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.0));
    replaceAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.0));
    runAndAssertNothing(compiledExpr.get());
}

}  // namespace mongo::sbe
