// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
        convertExp->serialize(query_shape::SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}));
}

TEST_F(ExpressionSubtypeTest, ParseAndSerializeWithPathInput) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$subtype" << "$path1");
    auto convertExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    ASSERT_VALUE_EQ_AUTO("{$subtype: [\"$path1\"]}", convertExp->serialize());

    ASSERT_VALUE_EQ_AUTO(
        "{$subtype: [\"$path1\"]}",
        convertExp->serialize(query_shape::SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}));
}

}  // namespace mongo
