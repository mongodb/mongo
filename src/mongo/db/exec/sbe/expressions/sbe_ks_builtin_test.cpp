// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace mongo::sbe {

class SBEBuiltinKsTest : public EExpressionTestFixture {
protected:
    void runAndAssertExpression(value::TypeTags argTag,
                                value::Value argVal,
                                value::TypeTags expectedTag,
                                value::Value expectedVal) {
        auto version = static_cast<int64_t>(key_string::Version::V1);
        auto ordering = uint32_t{0};
        auto discriminator = static_cast<int64_t>(key_string::Discriminator::kInclusive);

        auto versionExpr =
            makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(version));
        auto orderingExpr =
            makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<uint32_t>(ordering));
        auto discriminatorExpr = makeE<EConstant>(value::TypeTags::NumberInt64,
                                                  value::bitcastFrom<int64_t>(discriminator));

        auto [copyTag, copyVal] = value::copyValue(argTag, argVal);

        auto ksExpr = makeE<EFunction>(EFn::kKs,
                                       makeEs(std::move(versionExpr),
                                              std::move(orderingExpr),
                                              makeE<EConstant>(copyTag, copyVal),
                                              std::move(discriminatorExpr)));

        auto compiledExpr = compileExpression(*ksExpr);
        value::TagValueOwned actual =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        auto [compareTag, compareVal] =
            value::compareValue(actual.tag(), actual.value(), expectedTag, expectedVal);
        ASSERT_EQUALS(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQUALS(value::bitcastTo<int32_t>(compareVal), 0);
    }

    std::pair<value::TypeTags, value::Value> makeViewOfObject(const BSONObj& obj) {
        return {value::TypeTags::bsonObject, value::bitcastFrom<const char*>(obj.objdata())};
    }
};

TEST_F(SBEBuiltinKsTest, NumericTests) {
    for (int32_t int32Value :
         {std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()}) {
        auto argTag = value::TypeTags::NumberInt32;
        auto argVal = value::bitcastFrom<int32_t>(int32Value);

        key_string::Builder kb(key_string::Version::V1, key_string::ALL_ASCENDING);
        kb.appendNumberInt(int32Value);
        value::TagValueOwned expected =
            value::TagValueOwned::fromRaw(value::makeKeyString(kb.getValueCopy()));

        runAndAssertExpression(argTag, argVal, expected.tag(), expected.value());
    }

    for (int64_t int64Value :
         {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()}) {
        auto argTag = value::TypeTags::NumberInt64;
        auto argVal = value::bitcastFrom<int64_t>(int64Value);

        key_string::Builder kb(key_string::Version::V1, key_string::ALL_ASCENDING);
        kb.appendNumberLong(int64Value);
        value::TagValueOwned expected =
            value::TagValueOwned::fromRaw(value::makeKeyString(kb.getValueCopy()));

        runAndAssertExpression(argTag, argVal, expected.tag(), expected.value());
    }

    for (double doubleValue : {-73.0, 3.14159, std::numeric_limits<double>::quiet_NaN()}) {
        auto argTag = value::TypeTags::NumberDouble;
        auto argVal = value::bitcastFrom<double>(doubleValue);

        key_string::Builder kb(key_string::Version::V1, key_string::ALL_ASCENDING);
        kb.appendNumberDouble(doubleValue);
        value::TagValueOwned expected =
            value::TagValueOwned::fromRaw(value::makeKeyString(kb.getValueCopy()));

        runAndAssertExpression(argTag, argVal, expected.tag(), expected.value());
    }

    for (Decimal128 dec128Value : {Decimal128("-73"), Decimal128("3.14159"), Decimal128("NaN")}) {
        value::TagValueOwned arg =
            value::TagValueOwned::fromRaw(value::makeCopyDecimal(dec128Value));

        key_string::Builder kb(key_string::Version::V1, key_string::ALL_ASCENDING);
        kb.appendNumberDecimal(dec128Value);
        value::TagValueOwned expected =
            value::TagValueOwned::fromRaw(value::makeKeyString(kb.getValueCopy()));

        runAndAssertExpression(arg.tag(), arg.value(), expected.tag(), expected.value());
    }
}

TEST_F(SBEBuiltinKsTest, BooleanTests) {
    for (bool boolValue : {true, false}) {
        auto argTag = value::TypeTags::Boolean;
        auto argVal = value::bitcastFrom<bool>(boolValue);

        key_string::Builder kb(key_string::Version::V1, key_string::ALL_ASCENDING);
        kb.appendBool(boolValue);
        value::TagValueOwned expected =
            value::TagValueOwned::fromRaw(value::makeKeyString(kb.getValueCopy()));

        runAndAssertExpression(argTag, argVal, expected.tag(), expected.value());
    }
}

TEST_F(SBEBuiltinKsTest, StringTests) {
    for (std::string str : {"", "hello", "world"}) {
        value::TagValueOwned arg = value::TagValueOwned::fromRaw(value::makeNewString(str));

        key_string::Builder kb(key_string::Version::V1, key_string::ALL_ASCENDING);
        kb.appendString(str);
        value::TagValueOwned expected =
            value::TagValueOwned::fromRaw(value::makeKeyString(kb.getValueCopy()));

        runAndAssertExpression(arg.tag(), arg.value(), expected.tag(), expected.value());
    }
}

TEST_F(SBEBuiltinKsTest, NullTests) {
    auto argTag = value::TypeTags::Null;
    auto argVal = value::Value{0};

    key_string::Builder kb(key_string::Version::V1, key_string::ALL_ASCENDING);
    kb.appendNull();
    value::TagValueOwned expected =
        value::TagValueOwned::fromRaw(value::makeKeyString(kb.getValueCopy()));

    runAndAssertExpression(argTag, argVal, expected.tag(), expected.value());
}

}  // namespace mongo::sbe
