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
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <memory>
#include <string>

namespace mongo::sbe {

class SBEReplaceOneExprTest : public EExpressionTestFixture {
protected:
    void runAndAssertExpression(const vm::CodeFragment* compiledExpr,
                                const std::string& expectedVal) {
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));
        ASSERT_TRUE(sbe::value::isString(result.tag()));
        ASSERT_EQUALS(sbe::value::getStringView(result.tag(), result.value()), expectedVal);
    }

    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));
        ASSERT_EQUALS(value::TypeTags::Nothing, result.tag());
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
    auto replaceOneExpr = sbe::makeE<sbe::EFunction>(EFn::kReplaceOne,
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
    auto replaceOneExpr = sbe::makeE<sbe::EFunction>(EFn::kReplaceOne,
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
    auto replaceOneExpr = sbe::makeE<sbe::EFunction>(EFn::kReplaceOne,
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
    auto replaceOneExpr = sbe::makeE<sbe::EFunction>(EFn::kReplaceOne,
                                                     sbe::makeEs(makeE<EVariable>(inputSlot),
                                                                 makeE<EVariable>(findSlot),
                                                                 makeE<EVariable>(replaceSlot)));
    auto compiledExpr = compileExpression(*replaceOneExpr);

    auto bindSlots = [&](const BSONObj& bson) {
        auto inputVal = value::bitcastFrom<const char*>(bson["in"].value());
        inputAccessor.reset(value::TagValueView{value::TypeTags::bsonString, inputVal});

        auto findVal = value::bitcastFrom<const char*>(bson["find"].value());
        findAccessor.reset(value::TagValueView{value::TypeTags::bsonString, findVal});

        auto replaceVal = value::bitcastFrom<const char*>(bson["replace"].value());
        replaceAccessor.reset(value::TagValueView{value::TypeTags::bsonString, replaceVal});
    };

    // Test find and replace string.
    auto bson = BSON("in" << "this is a string"
                          << "find"
                          << "is"
                          << "replace"
                          << "at");
    bindSlots(bson);
    runAndAssertExpression(compiledExpr.get(), "that is a string");

    // Test not finding string.
    bson = BSON("in" << "this is a string"
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
    auto replaceOneExpr = sbe::makeE<sbe::EFunction>(EFn::kReplaceOne,
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
