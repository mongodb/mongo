// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include <string_view>
#include <utility>

namespace mongo::sbe {
using namespace std::literals::string_view_literals;

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

        value::TagValueOwned actual =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_THAT(std::make_pair(actual.tag(), actual.value()),
                    ValueEq(std::make_pair(tag, val)));
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
    verifyConstantExpression(os, value::makeSmallString("abc"sv));
    verifyConstantExpression(os, value::makeBigString("abcdefghijklmnop"sv));
    verifyConstantExpression(os, value::makeNewRecordId(123));
    verifyConstantExpression(
        os, value::makeCopyObjectId(std::array<uint8_t, 12>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}));

    verifyConstantExpression(os, makeArray(BSON_ARRAY(1 << 2 << 3)));
    verifyConstantExpression(os, makeArraySet(BSON_ARRAY(2 << 1 << 3)));
    verifyConstantExpression(os, makeObject(BSON("a"sv << 1 << "b"sv << 2)));
}

}  // namespace mongo::sbe
