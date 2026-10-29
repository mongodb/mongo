// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <memory>
#include <string>

namespace mongo::sbe {

class SBEIndexOfTest : public EExpressionTestFixture {
public:
    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        value::TagValueOwned runResult =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));
        ASSERT_EQUALS(runResult.tag(), sbe::value::TypeTags::Nothing);
    }

    void runAndAssertExpression(const vm::CodeFragment* compiledExpr, int expectedIndex) {
        value::TagValueOwned runResult =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));
        ASSERT_EQUALS(value::TypeTags::NumberInt32, runResult.tag());
        ASSERT_EQUALS(expectedIndex, value::bitcastTo<int32_t>(runResult.value()));
    }

    value::OwnedValueAccessor stringAccessor;
    value::SlotId stringSlot = bindAccessor(&stringAccessor);
    value::OwnedValueAccessor substringAccessor;
    value::SlotId substringSlot = bindAccessor(&substringAccessor);
    value::OwnedValueAccessor startIndexAccessor;
    value::SlotId startIndexSlot = bindAccessor(&startIndexAccessor);
    value::OwnedValueAccessor endIndexAccessor;
    value::SlotId endIndexSlot = bindAccessor(&endIndexAccessor);
    std::unique_ptr<sbe::EExpression> indexOfBytesExprFullArgs =
        sbe::makeE<sbe::EFunction>(EFn::kIndexOfBytes,
                                   sbe::makeEs(makeE<EVariable>(stringSlot),
                                               makeE<EVariable>(substringSlot),
                                               makeE<EVariable>(startIndexSlot),
                                               makeE<EVariable>(endIndexSlot)));
    std::unique_ptr<vm::CodeFragment> compiledBytesExprFullArgs =
        compileExpression(*indexOfBytesExprFullArgs);
    std::unique_ptr<sbe::EExpression> indexOfBytesExprStartIndex =
        sbe::makeE<sbe::EFunction>(EFn::kIndexOfBytes,
                                   sbe::makeEs(makeE<EVariable>(stringSlot),
                                               makeE<EVariable>(substringSlot),
                                               makeE<EVariable>(startIndexSlot)));
    std::unique_ptr<vm::CodeFragment> compiledBytesExprStartIndex =
        compileExpression(*indexOfBytesExprStartIndex);
    std::unique_ptr<sbe::EExpression> indexOfCPExprFullArgs =
        sbe::makeE<sbe::EFunction>(EFn::kIndexOfCP,
                                   sbe::makeEs(makeE<EVariable>(stringSlot),
                                               makeE<EVariable>(substringSlot),
                                               makeE<EVariable>(startIndexSlot),
                                               makeE<EVariable>(endIndexSlot)));
    std::unique_ptr<vm::CodeFragment> compiledCPExprFullArgs =
        compileExpression(*indexOfCPExprFullArgs);
    std::unique_ptr<sbe::EExpression> indexOfCPExprStartIndex =
        sbe::makeE<sbe::EFunction>(EFn::kIndexOfCP,
                                   sbe::makeEs(makeE<EVariable>(stringSlot),
                                               makeE<EVariable>(substringSlot),
                                               makeE<EVariable>(startIndexSlot)));
    std::unique_ptr<vm::CodeFragment> compiledCPExprStartIndex =
        compileExpression(*indexOfCPExprStartIndex);
};

TEST_F(SBEIndexOfTest, IndexOfBytesBasicTest) {
    // Test $indexOfBytes returns first substring found.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    runAndAssertExpression(compiledBytesExprStartIndex.get(), 3);
}

TEST_F(SBEIndexOfTest, IndexOfBytesStartIndexTest) {
    // Test $indexOfBytes returns after start index.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(4));
    runAndAssertExpression(compiledBytesExprStartIndex.get(), 5);
}

TEST_F(SBEIndexOfTest, IndexOfBytesIndexTest) {
    // Test $indexOfBytes returns after start index and before end index.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(4));
    endIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(7));
    runAndAssertExpression(compiledBytesExprFullArgs.get(), 5);
}

TEST_F(SBEIndexOfTest, IndexOfBytesNotFoundTest) {
    // Test $indexOfBytes returns -1 when substring is not found.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("zzz");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    runAndAssertExpression(compiledBytesExprStartIndex.get(), -1);
}

TEST_F(SBEIndexOfTest, IndexOfBytesInvalidBoundsTest1) {
    // Test $indexOfBytes returns -1 for invalid bounds where start > end.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(7));
    endIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(4));
    runAndAssertExpression(compiledBytesExprFullArgs.get(), -1);
}

TEST_F(SBEIndexOfTest, IndexOfBytesInvalidBoundsTest2) {
    // Test $indexOfBytes returns -1 for invalid bounds where start > length of string.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(25));
    runAndAssertExpression(compiledBytesExprFullArgs.get(), -1);
}

TEST_F(SBEIndexOfTest, IndexOfBytesStringTypeTest) {
    // Test $indexOfBytes returns nothing if the first argument is not a string.
    stringAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(9));
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    runAndAssertNothing(compiledBytesExprStartIndex.get());
}

TEST_F(SBEIndexOfTest, IndexOfBytesSubstringTypeTest) {
    // Test $indexOfBytes returns nothing if the second argument is not a string.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    substringAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(7));
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    runAndAssertNothing(compiledBytesExprStartIndex.get());
}

TEST_F(SBEIndexOfTest, IndexOfBytesNegativeStartIndexTest) {
    // Test $indexOfBytes returns nothing if the start index is negative.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(-4));
    runAndAssertNothing(compiledBytesExprStartIndex.get());
}

TEST_F(SBEIndexOfTest, IndexOfBytesStartIndexTypeTest) {
    // Test $indexOfBytes returns nothing if the start index does not have type NumberInt64.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(42.213));
    runAndAssertNothing(compiledBytesExprStartIndex.get());
}

TEST_F(SBEIndexOfTest, IndexOfBytesNegativeEndIndexTest) {
    // Test $indexOfBytes returns nothing if the end index is negative.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    endIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(-4));
    runAndAssertNothing(compiledBytesExprFullArgs.get());
}

TEST_F(SBEIndexOfTest, IndexOfBytesEndIndexTypeTest) {
    // Test $indexOfBytes returns nothing if the end index does not have type NumberInt64.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    endIndexAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(42.213));
    runAndAssertNothing(compiledBytesExprFullArgs.get());
}

TEST_F(SBEIndexOfTest, IndexOfCPBasicTest) {
    // Test $indexOfCP returns first substring found.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    runAndAssertExpression(compiledCPExprStartIndex.get(), 3);
}

TEST_F(SBEIndexOfTest, IndexOfCPStartIndexTest) {
    // Test $indexOfCP returns after start index.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(4));
    runAndAssertExpression(compiledCPExprStartIndex.get(), 5);
}

TEST_F(SBEIndexOfTest, IndexOfCPIndexTest) {
    // Test $indexOfCP returns after start index and before end index.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(4));
    endIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(7));
    runAndAssertExpression(compiledCPExprFullArgs.get(), 5);
}

TEST_F(SBEIndexOfTest, IndexOfCPNotFoundTest) {
    // Test $indexOfCP returns -1 when substring is not found.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("zzz");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    runAndAssertExpression(compiledCPExprStartIndex.get(), -1);
}

TEST_F(SBEIndexOfTest, IndexOfCPInvalidBoundsTest1) {
    // Test $indexOfCP returns -1 for invalid bounds where start > end.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(7));
    endIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(4));
    runAndAssertExpression(compiledCPExprFullArgs.get(), -1);
}

TEST_F(SBEIndexOfTest, IndexOfCPInvalidBoundsTest2) {
    // Test $indexOfCP returns -1 for invalid bounds where start > length of string.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(25));
    runAndAssertExpression(compiledCPExprFullArgs.get(), -1);
}

TEST_F(SBEIndexOfTest, IndexOfCPUnicodeBasicTest) {
    // Test $indexOfCP returns code point for unicode characters larger than 1 byte.
    auto [strTag, strVal] = value::makeNewString("\u039C\u039FNG\u039F");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("NG");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    runAndAssertExpression(compiledCPExprStartIndex.get(), 2);
}

TEST_F(SBEIndexOfTest, IndexOfCPUnicodeSubstringTest) {
    // Test $indexOfCP finds substring with unicode characters larger than 1 byte.
    auto [strTag, strVal] = value::makeNewString("\u039C\u039FNG\u039F");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("\u039F");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    runAndAssertExpression(compiledCPExprStartIndex.get(), 1);
}

TEST_F(SBEIndexOfTest, IndexOfCPUnicodeIndexTest) {
    // Test $indexOfCP finds substring with unicode characters larger than 1 byte after given index.
    auto [strTag, strVal] = value::makeNewString("\u039C\u039FNG\u039F");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("\u039F");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2));
    runAndAssertExpression(compiledCPExprStartIndex.get(), 4);
}

TEST_F(SBEIndexOfTest, IndexOfCPUnicodeStartIndexTest) {
    // Test $indexOfCP returns -1 when given a start index less than the number of bytes
    // in the string but greater than the number of code points.
    auto [strTag, strVal] = value::makeNewString("\u039C\u039FNG\u039F");
    std::string str = "\u039C\u039FNG\u039F";
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("\u039F");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(6));
    runAndAssertExpression(compiledCPExprStartIndex.get(), -1);
}

TEST_F(SBEIndexOfTest, IndexOfCPUnicodeNotFoundTest) {
    // Test $indexOfCP does not attempt to read past end of string.
    auto [strTag, strVal] = value::makeNewString("abcd");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("cdef");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    runAndAssertExpression(compiledCPExprStartIndex.get(), -1);
}

TEST_F(SBEIndexOfTest, IndexOfCPStringTypeTest) {
    // Test $indexOfCP returns nothing if the first argument is not a string.
    stringAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(9));
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    runAndAssertNothing(compiledCPExprStartIndex.get());
}

TEST_F(SBEIndexOfTest, IndexOfCPSubstringTypeTest) {
    // Test $indexOfCP returns nothing if the second argument is not a string.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    substringAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(7));
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    runAndAssertNothing(compiledCPExprStartIndex.get());
}

TEST_F(SBEIndexOfTest, IndexOfCPNegativeStartIndexTest) {
    // Test $indexOfCP returns nothing if the start index is negative.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(-4));
    runAndAssertNothing(compiledCPExprStartIndex.get());
}

TEST_F(SBEIndexOfTest, IndexOfCPStartIndexTypeTest) {
    // Test $indexOfCP returns nothing if the start index does not have type NumberInt64.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(42.213));
    runAndAssertNothing(compiledCPExprStartIndex.get());
}

TEST_F(SBEIndexOfTest, IndexOfCPNegativeEndIndexTest) {
    // Test $indexOfCP returns nothing if the end index is negative.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    endIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(-4));
    runAndAssertNothing(compiledCPExprFullArgs.get());
}

TEST_F(SBEIndexOfTest, IndexOfCPEndIndexTypeTest) {
    // Test $indexOfCP returns nothing if the end index does not have type NumberInt64.
    auto [strTag, strVal] = value::makeNewString("cafeteria");
    stringAccessor.reset(strTag, strVal);
    auto [substrTag, substrVal] = value::makeNewString("e");
    substringAccessor.reset(substrTag, substrVal);
    startIndexAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    endIndexAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(42.213));
    runAndAssertNothing(compiledCPExprFullArgs.get());
}

}  // namespace mongo::sbe
