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
    const std::vector<BinDataType> binDataTypes{BinDataGeneral,
                                                Function,
                                                ByteArrayDeprecated,
                                                bdtUUID,
                                                newUUID,
                                                MD5Type,
                                                Encrypt,
                                                Column,
                                                Sensitive,
                                                Vector,
                                                bdtCustom};
    for (const BinDataType& subtype : binDataTypes) {
        BSONBinData binData{"gf1UcxdHTJ2HQ/EGQrO7mQ==", 16, subtype};
        assertEvaluateSubtype(Value(binData), Value(static_cast<int>(subtype)));
    }
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
