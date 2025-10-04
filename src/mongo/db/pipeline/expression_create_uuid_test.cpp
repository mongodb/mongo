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

class ExpressionCreateUUIDTest : public AggregationContextFixture {
public:
    ExpressionCreateUUIDTest() {
        // TODO(SERVER-101162): Delete this once the feature flag defaults to true.
        // Use logic similar to registerSigmoidExpression to register the $createUUID expression
        // even though the feature falg defaults to off.
        // $createUUID is gated behind a feature flag and does
        // not get put into the map as the flag is off by default. Changing the value of the feature
        // flag with RAIIServerParameterControllerForTest() does not solve the issue because the
        // registration logic is not re-hit.
        try {
            Expression::registerExpression("$createUUID",
                                           ExpressionCreateUUID::parse,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           AllowedWithClientType::kAny,
                                           nullptr /* featureFlag */);
        } catch (const DBException& e) {
            // Allow this exception, to allow multiple ExpressionCreateUUIDTest instances
            // to be created in this process.
            ASSERT(e.reason() == "Duplicate expression ($createUUID) registered.");
        }
    }
};

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
    auto serialized = exp->serialize(SerializationOptions{
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
