// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
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

class SBECoerceToStringTest : public EExpressionTestFixture {
public:
    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));
        ASSERT_EQUALS(result.tag(), sbe::value::TypeTags::Nothing);
        ASSERT_EQUALS(result.value(), 0);
    }

    void runAndAssertExpression(const vm::CodeFragment* compiledExpr,
                                const std::string& expectedString) {
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));
        ASSERT_EQUALS(value::getStringView(result.tag(), result.value()), expectedString);
    }
};

TEST_F(SBECoerceToStringTest, BasicCoerceToString) {
    value::OwnedValueAccessor coerceToStringAccessor;
    auto coerceToStringSlot = bindAccessor(&coerceToStringAccessor);
    auto coerceToStringExpr = sbe::makeE<sbe::EFunction>(
        EFn::kCoerceToString, sbe::makeEs(makeE<EVariable>(coerceToStringSlot)));
    auto compiledExpr = compileExpression(*coerceToStringExpr);

    // Int32_t test.
    coerceToStringAccessor.reset(sbe::value::TypeTags::NumberInt32,
                                 value::bitcastFrom<int32_t>(42));
    runAndAssertExpression(compiledExpr.get(), "42");

    // Int64_t test.
    coerceToStringAccessor.reset(sbe::value::TypeTags::NumberInt64,
                                 value::bitcastFrom<int64_t>(42));
    runAndAssertExpression(compiledExpr.get(), "42");

    // Double test.
    coerceToStringAccessor.reset(sbe::value::TypeTags::NumberDouble,
                                 value::bitcastFrom<double>(42.213));
    runAndAssertExpression(compiledExpr.get(), "42.213");

    // Decimal test.
    auto [decTag, decVal] = value::makeCopyDecimal(Decimal128(42.213));
    coerceToStringAccessor.reset(decTag, decVal);
    runAndAssertExpression(compiledExpr.get(), "42.2130000000000");

    // BSONString test.
    auto bsonString = BSON("string" << "hello");
    auto bsonStringVal = value::bitcastFrom<const char*>(bsonString["string"].value());
    coerceToStringAccessor.reset(value::TypeTags::bsonString, bsonStringVal);
    runAndAssertExpression(compiledExpr.get(), "hello");

    // Date test.
    coerceToStringAccessor.reset(sbe::value::TypeTags::Date, value::bitcastFrom<int64_t>(4400));
    runAndAssertExpression(compiledExpr.get(), "1970-01-01T00:00:04.400Z");

    // TimeStamp test.
    coerceToStringAccessor.reset(sbe::value::TypeTags::Timestamp,
                                 value::bitcastFrom<uint64_t>(17179869186));
    Timestamp ts(17179869186);
    runAndAssertExpression(compiledExpr.get(), ts.toString());

    // Nothing test.
    coerceToStringAccessor.reset(sbe::value::TypeTags::Nothing, 0);
    runAndAssertNothing(compiledExpr.get());

    // BSONObj test.
    auto bsonObj = BSON("number" << 42);
    auto bsonData = value::bitcastFrom<const char*>(bsonObj.objdata());
    auto [bsonTag, bsonVal] = value::copyValue(value::TypeTags::bsonObject, bsonData);
    coerceToStringAccessor.reset(bsonTag, bsonVal);
    runAndAssertNothing(compiledExpr.get());

    // Array test.
    auto [arrTag, arrVal] = value::makeNewArray();
    coerceToStringAccessor.reset(arrTag, arrVal);
    runAndAssertNothing(compiledExpr.get());

    // ArraySet test.
    auto [arrSetTag, arrSetVal] = value::makeNewArraySet();
    coerceToStringAccessor.reset(arrSetTag, arrSetVal);
    runAndAssertNothing(compiledExpr.get());
}

}  // namespace mongo::sbe
