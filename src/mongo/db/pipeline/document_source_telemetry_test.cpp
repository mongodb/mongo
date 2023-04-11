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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_telemetry.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

/**
 * Subclass AggregationContextFixture to set the ExpressionContext's namespace to 'admin' with
 * {aggregate: 1} by default, so that parsing tests other than those which validate the namespace do
 * not need to explicitly set it.
 */
class DocumentSourceTelemetryTest : public AggregationContextFixture {
public:
    DocumentSourceTelemetryTest()
        : AggregationContextFixture(
              NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin)) {}
};

TEST_F(DocumentSourceTelemetryTest, ShouldFailToParseIfSpecIsNotObject) {
    ASSERT_THROWS_CODE(DocumentSourceTelemetry::createFromBson(
                           fromjson("{$telemetry: 1}").firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceTelemetryTest, ShouldFailToParseIfNotRunOnAdmin) {
    getExpCtx()->ns = NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "foo"));
    ASSERT_THROWS_CODE(DocumentSourceTelemetry::createFromBson(
                           fromjson("{$telemetry: {}}").firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceTelemetryTest, ShouldFailToParseIfNotRunWithAggregateOne) {
    getExpCtx()->ns = NamespaceString::createNamespaceString_forTest("admin.foo");
    ASSERT_THROWS_CODE(DocumentSourceTelemetry::createFromBson(
                           fromjson("{$telemetry: {}}").firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceTelemetryTest, ShouldFailToParseIfUnrecognisedParameterSpecified) {
    ASSERT_THROWS_CODE(DocumentSourceTelemetry::createFromBson(
                           fromjson("{$telemetry: {foo: true}}").firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceTelemetryTest, ParseAndSerialize) {
    auto obj = fromjson("{$telemetry: {}}");
    auto doc = DocumentSourceTelemetry::createFromBson(obj.firstElement(), getExpCtx());
    auto telemetryOp = static_cast<DocumentSourceTelemetry*>(doc.get());
    auto expected = Document{{"$telemetry", Document{}}};
    ASSERT_DOCUMENT_EQ(telemetryOp->serialize().getDocument(), expected);
}

}  // namespace
}  // namespace mongo
