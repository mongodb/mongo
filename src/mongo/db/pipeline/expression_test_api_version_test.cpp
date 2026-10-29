// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/expression_test_api_version.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using TestApiVersion = AggregationContextFixture;

TEST_F(TestApiVersion, UnstableAcceptsBooleanValue) {
    auto expCtx = getExpCtx();
    boost::intrusive_ptr<Expression> expression = ExpressionTestApiVersion::parse(
        expCtx.get(),
        BSON("$_testApiVersion" << BSON("unstable" << true)).firstElement(),
        expCtx->variablesParseState);

    ASSERT_VALUE_EQ(Value(DOC("$_testApiVersion" << DOC("unstable" << true))),
                    expression->serialize());
}

TEST_F(TestApiVersion, UnstableDoesNotAcceptNumericValue) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(ExpressionTestApiVersion::parse(
                           expCtx.get(),
                           BSON("$_testApiVersion" << BSON("unstable" << 1)).firstElement(),
                           expCtx->variablesParseState),
                       AssertionException,
                       5161702);
}

TEST_F(TestApiVersion, DeprecatedAcceptsBooleanValue) {
    auto expCtx = getExpCtx();
    boost::intrusive_ptr<Expression> expression = ExpressionTestApiVersion::parse(
        expCtx.get(),
        BSON("$_testApiVersion" << BSON("deprecated" << true)).firstElement(),
        expCtx->variablesParseState);

    ASSERT_VALUE_EQ(Value(DOC("$_testApiVersion" << DOC("deprecated" << true))),
                    expression->serialize());
}

TEST_F(TestApiVersion, DeprecatedDoesNotAcceptNumericValue) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(ExpressionTestApiVersion::parse(
                           expCtx.get(),
                           BSON("$_testApiVersion" << BSON("deprecated" << 1)).firstElement(),
                           expCtx->variablesParseState),
                       AssertionException,
                       5161703);
}

TEST_F(TestApiVersion, DoesNotAcceptInvalidParameter) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(
        ExpressionTestApiVersion::parse(
            expCtx.get(),
            BSON("$_testApiVersion" << BSON("invalidParameter" << true)).firstElement(),
            expCtx->variablesParseState),
        AssertionException,
        5161704);
}

TEST_F(TestApiVersion, OnlyTakesOneParameter) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(
        ExpressionTestApiVersion::parse(
            expCtx.get(),
            BSON("$_testApiVersion" << BSON("deprecated" << true << "unstable" << true))
                .firstElement(),
            expCtx->variablesParseState),
        AssertionException,
        5161701);
}

TEST_F(TestApiVersion, DoesNotAcceptEmptyDocument) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(
        ExpressionTestApiVersion::parse(expCtx.get(),
                                        BSON("$_testApiVersion" << BSONObj()).firstElement(),
                                        expCtx->variablesParseState),
        AssertionException,
        5161701);
}
}  // namespace
}  // namespace mongo
