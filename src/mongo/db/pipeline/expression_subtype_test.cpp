/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/data_view.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

#include <array>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class ExpressionSubtypeTest : public AggregationContextFixture {
public:
    void assertEvaluateSubtype(Value operand, Value expectedSubtype) {
        auto expCtx = getExpCtx();
        BSONObj spec = BSON("$subtype" << operand);
        auto exp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

        auto result = exp->evaluate(Document{}, &expCtx->variables);
        ASSERT_VALUE_EQ(result, expectedSubtype);
    }
};

TEST_F(ExpressionSubtypeTest, WithDefinedBinDataSubtype) {
    // Column (7) cannot be used as a literal.
    // ByteArrayDeprecated (2) requires a valid inner length prefix; build a 16-byte buffer
    // where bytes[0..3] hold 12 (= 16 - 4) as a little-endian int32.
    std::array<char, 16> subtype2Buf{};
    DataView(subtype2Buf.data()).write<LittleEndian<int32_t>>(12);

    const std::vector<std::pair<BinDataType, BSONBinData>> cases{
        {BinDataGeneral, {"gf1UcxdHTJ2HQ/EGQrO7mQ==", 16, BinDataGeneral}},
        {Function, {"gf1UcxdHTJ2HQ/EGQrO7mQ==", 16, Function}},
        {ByteArrayDeprecated, {subtype2Buf.data(), 16, ByteArrayDeprecated}},
        {bdtUUID, {"gf1UcxdHTJ2HQ/EGQrO7mQ==", 16, bdtUUID}},
        {newUUID, {"gf1UcxdHTJ2HQ/EGQrO7mQ==", 16, newUUID}},
        {MD5Type, {"gf1UcxdHTJ2HQ/EGQrO7mQ==", 16, MD5Type}},
        {Encrypt, {"gf1UcxdHTJ2HQ/EGQrO7mQ==", 16, Encrypt}},
        {Sensitive, {"gf1UcxdHTJ2HQ/EGQrO7mQ==", 16, Sensitive}},
        {Vector, {"gf1UcxdHTJ2HQ/EGQrO7mQ==", 16, Vector}},
        {bdtCustom, {"gf1UcxdHTJ2HQ/EGQrO7mQ==", 16, bdtCustom}},
    };
    for (const auto& [subtype, binData] : cases) {
        assertEvaluateSubtype(Value(binData), Value(static_cast<int>(subtype)));
    }
}

TEST_F(ExpressionSubtypeTest, BsonColumnThrowsWhenUsedAsLiteral) {
    // Assert that we throw when the Column type is used as a literal.
    auto expCtx = getExpCtx();
    BSONBinData columnBinData{"gf1UcxdHTJ2HQ/EGQrO7mQ==", 16, Column};
    BSONObj spec = BSON("$subtype" << Value(columnBinData));
    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(ExpressionSubtypeTest, ByteArrayDeprecatedThrowsWithBadInnerPrefix) {
    auto expCtx = getExpCtx();
    // A bare 4-byte buffer with no inner length prefix (all zeros → inner length = 0, but
    // total - 4 = 0, so this actually passes). Use a deliberately wrong prefix instead.
    std::array<char, 8> badBuf{};
    DataView(badBuf.data()).write<LittleEndian<int32_t>>(99);  // 99 != 8 - 4 = 4
    BSONBinData badSubtype2{badBuf.data(), 8, ByteArrayDeprecated};
    BSONObj spec = BSON("$subtype" << Value(badSubtype2));
    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(ExpressionSubtypeTest, BdtUUIDThrowsWithWrongSize) {
    auto expCtx = getExpCtx();
    BSONBinData shortUUID{"AAAA", 4, bdtUUID};
    BSONObj spec = BSON("$subtype" << Value(shortUUID));
    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(ExpressionSubtypeTest, MD5TypeThrowsWithWrongSize) {
    auto expCtx = getExpCtx();
    BSONBinData shortMD5{"AAAA", 4, MD5Type};
    BSONObj spec = BSON("$subtype" << Value(shortMD5));
    ASSERT_THROWS_CODE(Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(ExpressionSubtypeTest, WithUndefinedBinDataSubtype) {
    // Use a subtype value (200) that doesn't exist as a named constant in the BinDataType enum
    BSONBinData binData{"gf1UcxdHTJ2HQ/EGQrO7mQ==", 16, static_cast<BinDataType>(200)};
    assertEvaluateSubtype(Value(binData), Value(200));
}

TEST_F(ExpressionSubtypeTest, WithNull) {
    assertEvaluateSubtype(Value(BSONNULL), Value(BSONNULL));
}

TEST_F(ExpressionSubtypeTest, SubtypeWithMoreThanOneInputFailsToParse) {
    BSONBinData binData{"gf1UcxdHTJ2HQ/EGQrO7mQ==", 16, BinDataGeneral};
    auto expCtx = getExpCtx();

    auto spec = BSON("$subtype" << BSON_ARRAY(Value(binData) << Value(binData)));
    ASSERT_THROWS_WITH_CHECK(
        Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), 16020);
            ASSERT_STRING_CONTAINS(
                exception.reason(),
                "Expression $subtype takes exactly 1 arguments. 2 were passed in.");
        });
}

TEST_F(ExpressionSubtypeTest, ParseAndSerialize) {
    BSONBinData binData{"gf1UcxdHTJ2HQ/EGQrO7mQ==", 16, BinDataGeneral};
    auto expCtx = getExpCtx();

    auto spec = BSON("$subtype" << Value(binData));
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    ASSERT_VALUE_EQ_AUTO("{$subtype: [{$const: BinData(0, \"6766315563786448544A3248512F4547\")}]}",
                         convertExp->serialize());

    ASSERT_VALUE_EQ_AUTO(
        "{$subtype: [{$const: BinData(0, \"6766315563786448544A3248512F4547\")}]}",
        convertExp->serialize(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}));
}

TEST_F(ExpressionSubtypeTest, ParseAndSerializeWithPathInput) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$subtype" << "$path1");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    ASSERT_VALUE_EQ_AUTO("{$subtype: [\"$path1\"]}", convertExp->serialize());

    ASSERT_VALUE_EQ_AUTO(
        "{$subtype: [\"$path1\"]}",
        convertExp->serialize(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}));
}

}  // namespace mongo
