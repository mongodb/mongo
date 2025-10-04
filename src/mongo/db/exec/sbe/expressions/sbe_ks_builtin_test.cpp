/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
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

        auto ksExpr = makeE<EFunction>("ks",
                                       makeEs(std::move(versionExpr),
                                              std::move(orderingExpr),
                                              makeE<EConstant>(copyTag, copyVal),
                                              std::move(discriminatorExpr)));

        auto compiledExpr = compileExpression(*ksExpr);
        auto [actualTag, actualVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(actualTag, actualVal);

        auto [compareTag, compareVal] =
            value::compareValue(actualTag, actualVal, expectedTag, expectedVal);
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
        auto [expectedTag, expectedVal] = value::makeKeyString(kb.getValueCopy());
        value::ValueGuard expectedGuard(expectedTag, expectedVal);

        runAndAssertExpression(argTag, argVal, expectedTag, expectedVal);
    }

    for (int64_t int64Value :
         {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()}) {
        auto argTag = value::TypeTags::NumberInt64;
        auto argVal = value::bitcastFrom<int64_t>(int64Value);

        key_string::Builder kb(key_string::Version::V1, key_string::ALL_ASCENDING);
        kb.appendNumberLong(int64Value);
        auto [expectedTag, expectedVal] = value::makeKeyString(kb.getValueCopy());
        value::ValueGuard expectedGuard(expectedTag, expectedVal);

        runAndAssertExpression(argTag, argVal, expectedTag, expectedVal);
    }

    for (double doubleValue : {-73.0, 3.14159, std::numeric_limits<double>::quiet_NaN()}) {
        auto argTag = value::TypeTags::NumberDouble;
        auto argVal = value::bitcastFrom<double>(doubleValue);

        key_string::Builder kb(key_string::Version::V1, key_string::ALL_ASCENDING);
        kb.appendNumberDouble(doubleValue);
        auto [expectedTag, expectedVal] = value::makeKeyString(kb.getValueCopy());
        value::ValueGuard expectedGuard(expectedTag, expectedVal);

        runAndAssertExpression(argTag, argVal, expectedTag, expectedVal);
    }

    for (Decimal128 dec128Value : {Decimal128("-73"), Decimal128("3.14159"), Decimal128("NaN")}) {
        auto [argTag, argVal] = value::makeCopyDecimal(dec128Value);
        value::ValueGuard argGuard(argTag, argVal);

        key_string::Builder kb(key_string::Version::V1, key_string::ALL_ASCENDING);
        kb.appendNumberDecimal(dec128Value);
        auto [expectedTag, expectedVal] = value::makeKeyString(kb.getValueCopy());
        value::ValueGuard expectedGuard(expectedTag, expectedVal);

        runAndAssertExpression(argTag, argVal, expectedTag, expectedVal);
    }
}
}  // namespace mongo::sbe
