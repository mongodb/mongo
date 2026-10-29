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

class SBEToUpperToLowerTest : public EExpressionTestFixture {
public:
    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        value::TagValueOwned runResult =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));
        ASSERT_EQUALS(runResult.tag(), sbe::value::TypeTags::Nothing);
        ASSERT_EQUALS(runResult.value(), 0);
    }

    void runAndAssertExpression(const vm::CodeFragment* compiledExpr,
                                const std::string& expectedString) {
        value::TagValueOwned runResult =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));
        ASSERT_EQUALS(value::getStringView(runResult.tag(), runResult.value()), expectedString);
    }
};

TEST_F(SBEToUpperToLowerTest, BasicToUpper) {
    value::OwnedValueAccessor toUpperAccessor;
    auto toUpperSlot = bindAccessor(&toUpperAccessor);
    auto toUpperExpr =
        sbe::makeE<sbe::EFunction>(EFn::kToUpper, sbe::makeEs(makeE<EVariable>(toUpperSlot)));
    auto compiledExpr = compileExpression(*toUpperExpr);

    // SmallString test.
    auto [strTag, strVal] = sbe::value::makeNewString("hello");
    toUpperAccessor.reset(strTag, strVal);
    runAndAssertExpression(compiledExpr.get(), "HELLO");

    // BigString test.
    auto [strTag2, strVal2] = sbe::value::makeNewString("abcdefgHIJKLMNOPQRStuvwxyz123456789");
    toUpperAccessor.reset(strTag2, strVal2);
    runAndAssertExpression(compiledExpr.get(), "ABCDEFGHIJKLMNOPQRSTUVWXYZ123456789");

    // BSONString test.
    auto bsonStringObj = BSON("string" << "hello");
    auto bsonStringVal = value::bitcastFrom<const char*>(bsonStringObj["string"].value());
    toUpperAccessor.reset(value::TypeTags::bsonString, bsonStringVal);
    runAndAssertExpression(compiledExpr.get(), "HELLO");

    // Int32_t test.
    toUpperAccessor.reset(sbe::value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(42));
    runAndAssertNothing(compiledExpr.get());

    // Int64_t test.
    toUpperAccessor.reset(sbe::value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledExpr.get());

    // Double test.
    toUpperAccessor.reset(sbe::value::TypeTags::NumberDouble, value::bitcastFrom<double>(42.213));
    runAndAssertNothing(compiledExpr.get());

    // Decimal test.
    auto [decTag, decVal] = value::makeCopyDecimal(Decimal128(42.213));
    toUpperAccessor.reset(decTag, decVal);
    runAndAssertNothing(compiledExpr.get());

    // Date test.
    toUpperAccessor.reset(sbe::value::TypeTags::Date, value::bitcastFrom<int64_t>(4400));
    runAndAssertNothing(compiledExpr.get());

    // Timestamp test.
    toUpperAccessor.reset(sbe::value::TypeTags::Timestamp,
                          value::bitcastFrom<uint64_t>(17179869186));
    runAndAssertNothing(compiledExpr.get());

    // BSONObj test.
    auto bsonObj = BSON("number" << 42);
    auto bsonData = value::bitcastFrom<const char*>(bsonObj.objdata());
    auto [bsonTag, bsonVal] = value::copyValue(value::TypeTags::bsonObject, bsonData);
    toUpperAccessor.reset(bsonTag, bsonVal);
    runAndAssertNothing(compiledExpr.get());

    // Array test.
    auto [arrTag, arrVal] = value::makeNewArray();
    toUpperAccessor.reset(arrTag, arrVal);
    runAndAssertNothing(compiledExpr.get());

    // ArraySet test.
    auto [arrSetTag, arrSetVal] = value::makeNewArraySet();
    toUpperAccessor.reset(arrSetTag, arrSetVal);
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBEToUpperToLowerTest, BasicToLower) {
    value::OwnedValueAccessor toLowerAccessor;
    auto toLowerSlot = bindAccessor(&toLowerAccessor);
    auto toLowerExpr =
        sbe::makeE<sbe::EFunction>(EFn::kToLower, sbe::makeEs(makeE<EVariable>(toLowerSlot)));
    auto compiledExpr = compileExpression(*toLowerExpr);

    // SmallString test.
    auto [strTag, strVal] = sbe::value::makeNewString("HELLO");
    toLowerAccessor.reset(strTag, strVal);
    runAndAssertExpression(compiledExpr.get(), "hello");

    // BigString test.
    auto [strTag2, strVal2] = sbe::value::makeNewString("abcdefgHIJKLMNOPQRStuvwxyz123456789");
    toLowerAccessor.reset(strTag2, strVal2);
    runAndAssertExpression(compiledExpr.get(), "abcdefghijklmnopqrstuvwxyz123456789");

    // BSONString test.
    auto bsonStringObj = BSON("string" << "HELLO");
    auto bsonStringVal = value::bitcastFrom<const char*>(bsonStringObj["string"].value());
    toLowerAccessor.reset(value::TypeTags::bsonString, bsonStringVal);
    runAndAssertExpression(compiledExpr.get(), "hello");

    // Int32_t test.
    toLowerAccessor.reset(sbe::value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(42));
    runAndAssertNothing(compiledExpr.get());

    // Int64_t test.
    toLowerAccessor.reset(sbe::value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledExpr.get());

    // Double test.
    toLowerAccessor.reset(sbe::value::TypeTags::NumberDouble, value::bitcastFrom<double>(42.213));
    runAndAssertNothing(compiledExpr.get());

    // Decimal test.
    auto [decTag, decVal] = value::makeCopyDecimal(Decimal128(42.213));
    toLowerAccessor.reset(decTag, decVal);
    runAndAssertNothing(compiledExpr.get());

    // Date test.
    toLowerAccessor.reset(sbe::value::TypeTags::Date, value::bitcastFrom<int64_t>(4400));
    runAndAssertNothing(compiledExpr.get());

    // Timestamp test.
    toLowerAccessor.reset(sbe::value::TypeTags::Timestamp,
                          value::bitcastFrom<uint64_t>(17179869186));
    runAndAssertNothing(compiledExpr.get());

    // BSONObj test.
    auto bsonObj = BSON("number" << 42);
    auto bsonData = value::bitcastFrom<const char*>(bsonObj.objdata());
    auto [bsonTag, bsonVal] = value::copyValue(value::TypeTags::bsonObject, bsonData);
    toLowerAccessor.reset(bsonTag, bsonVal);
    runAndAssertNothing(compiledExpr.get());

    // Array test.
    auto [arrTag, arrVal] = value::makeNewArray();
    toLowerAccessor.reset(arrTag, arrVal);
    runAndAssertNothing(compiledExpr.get());

    // ArraySet test.
    auto [arrSetTag, arrSetVal] = value::makeNewArraySet();
    toLowerAccessor.reset(arrSetTag, arrSetVal);
    runAndAssertNothing(compiledExpr.get());
}

}  // namespace mongo::sbe
