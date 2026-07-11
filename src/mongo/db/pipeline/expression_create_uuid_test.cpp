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

using ExpressionCreateUUIDTest = AggregationContextFixture;

TEST_F(ExpressionCreateUUIDTest, Basic) {
    auto expCtx = getExpCtx();
    BSONObj spec = BSON("$createUUID" << BSONObj());
    auto exp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    auto validateIsBindata = [](const Value& value) {
        ASSERT_EQ(BSONType::binData, value.getType());
        ASSERT_EQ(value.getBinData().type, newUUID);
    };

    // Validate a UUID is returned.
    auto result1 = exp->evaluate(Document{}, &expCtx->variables);
    validateIsBindata(result1);
    // Validate a different UUID is returned.
    auto result2 = exp->evaluate(Document{}, &expCtx->variables);
    validateIsBindata(result2);
    ASSERT_VALUE_NE(result1, result2);

    // Serialize, re-parse, and validate a different UUID is returned.
    auto serialized = exp->serialize(query_shape::SerializationOptions{
        .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)});
    auto nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "unittests", "pipeline_test");
    expCtx = make_intrusive<ExpressionContextForTest>(getOpCtx(), nss);
    exp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    auto result3 = exp->evaluate(Document{}, &expCtx->variables);
    validateIsBindata(result3);
    ASSERT_VALUE_NE(result1, result3);
}

TEST_F(ExpressionCreateUUIDTest, ParseError) {
    auto expCtx = getExpCtx();
    BSONObj spec = BSON("$createUUID" << BSON("some" << "argument"));
    ASSERT_THROWS_CODE_AND_WHAT(
        Expression::parseExpression(getExpCtxRaw(), spec, expCtx->variablesParseState),
        AssertionException,
        10081901,
        "$createUUID does not accept arguments");
}

}  // namespace mongo
