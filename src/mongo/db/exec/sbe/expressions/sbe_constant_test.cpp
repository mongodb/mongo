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
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

#include <array>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <utility>

namespace mongo::sbe {

class SBEConstantTest : public GoldenEExpressionTestFixture {
public:
    // Takes ownership of the value.
    void verifyConstantExpression(std::ostream& os, std::pair<value::TypeTags, value::Value> p) {
        verifyConstantExpression(os, p.first, p.second);
    }

    // Takes ownership of the value.
    void verifyConstantExpression(std::ostream& os, value::TypeTags tag, value::Value val) {
        auto expr = sbe::makeE<EConstant>(tag, val);
        printInputExpression(os, *expr);

        auto compiledExpr = compileExpression(*expr);
        printCompiledExpression(os, *compiledExpr);

        auto [tagActual, valActual] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(tagActual, valActual);

        ASSERT_THAT(std::make_pair(tagActual, valActual), ValueEq(std::make_pair(tag, val)));
    }
};

TEST_F(SBEConstantTest, SbeConstants) {
    auto& os = gctx->outStream();

    verifyConstantExpression(os, value::TypeTags::Nothing, 0);
    verifyConstantExpression(os, value::TypeTags::Null, 0);
    verifyConstantExpression(os, value::TypeTags::Boolean, value::bitcastFrom<bool>(true));
    verifyConstantExpression(os, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(123));
    verifyConstantExpression(os, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(123));
    verifyConstantExpression(os, value::TypeTags::NumberDouble, value::bitcastFrom<double>(123.0));
    verifyConstantExpression(os, value::TypeTags::MinKey, 0);
    verifyConstantExpression(os, value::TypeTags::MaxKey, 0);

    verifyConstantExpression(os, value::makeCopyDecimal(Decimal128(123.0)));
    verifyConstantExpression(os, value::makeSmallString("abc"_sd));
    verifyConstantExpression(os, value::makeBigString("abcdefghijklmnop"_sd));
    verifyConstantExpression(os, value::makeNewRecordId(123));
    verifyConstantExpression(
        os, value::makeCopyObjectId(std::array<uint8_t, 12>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}));

    verifyConstantExpression(os, makeArray(BSON_ARRAY(1 << 2 << 3)));
    verifyConstantExpression(os, makeArraySet(BSON_ARRAY(2 << 1 << 3)));
    verifyConstantExpression(os, makeObject(BSON("a"_sd << 1 << "b"_sd << 2)));
}

}  // namespace mongo::sbe
