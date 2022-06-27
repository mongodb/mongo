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
#include "mongo/util/pcre_util.h"

namespace mongo::sbe {
class SBERegexTest : public EExpressionTestFixture {
protected:
    void runAndAssertRegexCompile(const vm::CodeFragment* compiledExpr, StringData regexString) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);

        ASSERT_EQUALS(value::TypeTags::pcreRegex, tag);

        auto regex = value::getPcreRegexView(val);
        std::string res = str::stream()
            << "/" << regex->pattern() << "/" << pcre_util::optionsToFlags(regex->options());
        ASSERT_EQUALS(res, regexString);
    }

    void runAndAssertMatchExpression(const vm::CodeFragment* compiledExpr, bool expected) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);

        ASSERT(tag == value::TypeTags::Boolean);
        ASSERT_EQUALS(value::bitcastTo<bool>(val), expected);
    }

    void runAndAssertFindExpression(const vm::CodeFragment* compiledExpr,
                                    StringData expectedMatch,
                                    int idx) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);

        ASSERT(tag == value::TypeTags::Object);
        auto obj = value::getObjectView(val);

        auto [matchTag, matchVal] = obj->getField("match");
        value::ValueGuard matchGuard(matchTag, matchVal);
        ASSERT(value::isString(matchTag));
        ASSERT_EQUALS(value::getStringView(matchTag, matchVal), expectedMatch);

        auto [idxTag, idxVal] = obj->getField("idx");
        value::ValueGuard idxGuard(idxTag, idxVal);
        ASSERT_EQUALS(idxTag, value::TypeTags::NumberInt32);
        ASSERT_EQUALS(value::numericCast<int32_t>(idxTag, idxVal), idx);
    }

    void addMatchResult(value::Array* arrayPtr, StringData matchStr, int32_t idx) {
        auto [objTag, objVal] = value::makeNewObject();
        value::ValueGuard objGuard{objTag, objVal};
        auto obj = value::getObjectView(objVal);

        auto [matchStrTag, matchStrVal] = value::makeNewString(matchStr);
        auto [capturesTag, capturesVal] = value::makeNewArray();
        obj->push_back("match", matchStrTag, matchStrVal);
        obj->push_back("idx", value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(idx));
        obj->push_back("captures", capturesTag, capturesVal);
        objGuard.reset();
        arrayPtr->push_back(objTag, objVal);
    }

    void runAndAssertFindAllExpression(const vm::CodeFragment* compiledExpr,
                                       value::Array* expected) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);

        ASSERT(tag == value::TypeTags::Array);
        auto arr = value::getArrayView(val);

        ASSERT_EQUALS(arr->size(), expected->size());

        for (size_t idx = 0; idx < arr->size(); ++idx) {
            auto [objTag, objVal] = arr->getAt(idx);
            ASSERT(objTag == value::TypeTags::Object);
            auto [expObjTag, expObjVal] = expected->getAt(idx);
            ASSERT(expObjTag == value::TypeTags::Object);

            auto [matchTag, matchVal] = value::getObjectView(objVal)->getField("match");
            auto [expMatchTag, expMatchVal] = value::getObjectView(expObjVal)->getField("match");
            ASSERT_EQUALS(matchTag, expMatchTag);
            ASSERT_EQUALS(value::getStringView(matchTag, matchVal),
                          value::getStringView(expMatchTag, expMatchVal));

            auto [idxTag, idxVal] = value::getObjectView(objVal)->getField("idx");
            auto [expIdxTag, expIdxVal] = value::getObjectView(expObjVal)->getField("idx");
            ASSERT_EQUALS(idxTag, expIdxTag);
            ASSERT_EQUALS(value::numericCast<int64_t>(idxTag, idxVal),
                          value::numericCast<int64_t>(expIdxTag, expIdxVal));
        }
    }
};

TEST_F(SBERegexTest, ComputesRegexCompile) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto patternSlot = bindAccessor(&slotAccessor1);
    auto optionsSlot = bindAccessor(&slotAccessor2);
    auto regexExpr = sbe::makeE<sbe::EFunction>(
        "regexCompile", sbe::makeEs(makeE<EVariable>(patternSlot), makeE<EVariable>(optionsSlot)));
    auto compiledExpr = compileExpression(*regexExpr);

    auto [patternTag, patternVal] = value::makeNewString("^Many");
    auto [optionsTag, optionsVal] = value::makeNewString("i");
    slotAccessor1.reset(patternTag, patternVal);
    slotAccessor2.reset(optionsTag, optionsVal);
    runAndAssertRegexCompile(compiledExpr.get(), "/^Many/i");
}

TEST_F(SBERegexTest, ComputesRegexMatch) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto regexSlot = bindAccessor(&slotAccessor1);
    auto inputSlot = bindAccessor(&slotAccessor2);
    auto regexExpr = sbe::makeE<sbe::EFunction>(
        "regexMatch", sbe::makeEs(makeE<EVariable>(regexSlot), makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*regexExpr);

    auto [regexTag, regexVal] = value::makeNewPcreRegex("line", "");
    auto [inputTag, inputVal] = value::makeNewString("Many lines of code");
    slotAccessor1.reset(regexTag, regexVal);
    slotAccessor2.reset(inputTag, inputVal);
    runAndAssertMatchExpression(compiledExpr.get(), true);

    std::tie(regexTag, regexVal) = value::makeNewPcreRegex("link", "");
    std::tie(inputTag, inputVal) = value::makeNewString("Example text");
    slotAccessor1.reset(regexTag, regexVal);
    slotAccessor2.reset(inputTag, inputVal);
    runAndAssertMatchExpression(compiledExpr.get(), false);
}

TEST_F(SBERegexTest, ComputesRegexFind) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto regexSlot = bindAccessor(&slotAccessor1);
    auto inputSlot = bindAccessor(&slotAccessor2);
    auto regexExpr = sbe::makeE<sbe::EFunction>(
        "regexFind", sbe::makeEs(makeE<EVariable>(regexSlot), makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*regexExpr);

    auto [regexTag, regexVal] = value::makeNewPcreRegex("line", "");
    auto [inputTag, inputVal] = value::makeNewString("Many lines of code");
    slotAccessor1.reset(regexTag, regexVal);
    slotAccessor2.reset(inputTag, inputVal);
    runAndAssertFindExpression(compiledExpr.get(), "line", 5);

    std::tie(regexTag, regexVal) = value::makeNewPcreRegex("line", "i");
    std::tie(inputTag, inputVal) = value::makeNewString("Many LINES of code");
    slotAccessor1.reset(regexTag, regexVal);
    slotAccessor2.reset(inputTag, inputVal);
    runAndAssertFindExpression(compiledExpr.get(), "LINE", 5);
}

TEST_F(SBERegexTest, ComputesRegexFindAll) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto regexSlot = bindAccessor(&slotAccessor1);
    auto inputSlot = bindAccessor(&slotAccessor2);
    auto regexExpr = sbe::makeE<sbe::EFunction>(
        "regexFindAll", sbe::makeEs(makeE<EVariable>(regexSlot), makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*regexExpr);

    auto [arrTag, arrVal] = value::makeNewArray();
    value::ValueGuard arrGuard{arrTag, arrVal};
    auto arrayView = value::getArrayView(arrVal);

    addMatchResult(arrayView, "line", 4);
    addMatchResult(arrayView, "line", 16);

    auto [regexTag, regexVal] = value::makeNewPcreRegex("line", "");
    auto [inputTag, inputVal] = value::makeNewString("One line or two lines of code");
    slotAccessor1.reset(regexTag, regexVal);
    slotAccessor2.reset(inputTag, inputVal);
    runAndAssertFindAllExpression(compiledExpr.get(), arrayView);
}

}  // namespace mongo::sbe
