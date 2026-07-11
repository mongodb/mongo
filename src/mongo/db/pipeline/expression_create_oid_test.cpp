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

using ExpressionCreateObjectIdTest = AggregationContextFixture;

TEST_F(ExpressionCreateObjectIdTest, Basic) {
    auto expCtx = getExpCtx();
    BSONObj spec = BSON("$createObjectId" << BSONObj());
    auto exp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    // Validate an ObjectId is returned.
    auto result1 = exp->evaluate(Document{}, &expCtx->variables);
    ASSERT_EQ(BSONType::oid, result1.getType());
    // Validate a different ObjectId is returned.
    auto result2 = exp->evaluate(Document{}, &expCtx->variables);
    ASSERT_EQ(BSONType::oid, result2.getType());
    ASSERT_VALUE_NE(result1, result2);

    // Serialize, re-parse, and validate a different ObjectId is returned.
    auto serialized =
        exp
            ->serialize(query_shape::SerializationOptions{
                .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)})
            .getDocument()
            .toBson();
    ASSERT_BSONOBJ_EQ(fromjson(R"({$createObjectId: {}})"), serialized);
    auto nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "unittests", "pipeline_test");
    expCtx = make_intrusive<ExpressionContextForTest>(getOpCtx(), nss);
    exp = Expression::parseExpression(expCtx.get(), serialized, expCtx->variablesParseState);

    auto result3 = exp->evaluate(Document{}, &expCtx->variables);
    ASSERT_EQ(BSONType::oid, result3.getType());
    ASSERT_VALUE_NE(result1, result3);
}

TEST_F(ExpressionCreateObjectIdTest, ParseErrorObjectWithArg) {
    auto expCtx = getExpCtx();
    BSONObj spec = BSON("$createObjectId" << BSON("some" << "argument"));
    ASSERT_THROWS_CODE_AND_WHAT(
        Expression::parseExpression(getExpCtxRaw(), spec, expCtx->variablesParseState),
        AssertionException,
        ErrorCodes::FailedToParse,
        "$createObjectId only accepts the empty object as argument. To convert a value to an "
        "ObjectId, use $toObjectId.");
}

TEST_F(ExpressionCreateObjectIdTest, ParseErrorArrayWithArg) {
    auto expCtx = getExpCtx();
    BSONObj spec = BSON("$createObjectId" << BSON_ARRAY("argument"));
    ASSERT_THROWS_CODE_AND_WHAT(
        Expression::parseExpression(getExpCtxRaw(), spec, expCtx->variablesParseState),
        AssertionException,
        ErrorCodes::FailedToParse,
        "$createObjectId only accepts the empty object as argument. To convert a value to an "
        "ObjectId, use $toObjectId.");
}

TEST_F(ExpressionCreateObjectIdTest, ParseErrorStringArg) {
    auto expCtx = getExpCtx();
    BSONObj spec = BSON("$createObjectId" << "argument");
    ASSERT_THROWS_CODE_AND_WHAT(
        Expression::parseExpression(getExpCtxRaw(), spec, expCtx->variablesParseState),
        AssertionException,
        ErrorCodes::FailedToParse,
        "$createObjectId only accepts the empty object as argument. To convert a value to an "
        "ObjectId, use $toObjectId.");
}

}  // namespace mongo
