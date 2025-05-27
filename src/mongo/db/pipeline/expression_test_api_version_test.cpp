/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/pipeline/expression_test_api_version.h"

#include "mongo/base/string_data.h"
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
