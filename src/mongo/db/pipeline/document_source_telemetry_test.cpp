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
              NamespaceString::makeCollectionlessAggregateNSS(DatabaseName(boost::none, "admin"))) {
    }
};

TEST_F(DocumentSourceTelemetryTest, ShouldFailToParseIfSpecIsNotObject) {
    const auto specObj = fromjson("{$telemetry: 1}");
    ASSERT_THROWS_CODE(DocumentSourceTelemetry::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceTelemetryTest, ShouldFailToParseIfNotRunOnAdmin) {
    const auto specObj = fromjson("{$telemetry: {}}");
    getExpCtx()->ns =
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName(boost::none, "foo"));
    ASSERT_THROWS_CODE(DocumentSourceTelemetry::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceTelemetryTest, ShouldFailToParseIfNotRunWithAggregateOne) {
    const auto specObj = fromjson("{$telemetry: {}}");
    getExpCtx()->ns = NamespaceString("admin.foo");
    ASSERT_THROWS_CODE(DocumentSourceTelemetry::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceTelemetryTest, ShouldFailToParseClearEntriesIfNotBoolean) {
    const auto specObj = fromjson("{$telemetry: {clearEntries: 1}}");
    ASSERT_THROWS_CODE(DocumentSourceTelemetry::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceTelemetryTest, ShouldFailToParseIfUnrecognisedParameterSpecified) {
    const auto specObj = fromjson("{$telemetry: {foo: true}}");
    ASSERT_THROWS_CODE(DocumentSourceTelemetry::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceTelemetryTest, ShouldParseAndSerializeNonDefaultOptionalArguments) {
    const auto elem = fromjson("{$telemetry: {clearEntries: true}}").firstElement();
    const auto parsed = DocumentSourceTelemetry::createFromBson(elem, getExpCtx());
    const auto telemetry = static_cast<DocumentSourceTelemetry*>(parsed.get());
    const auto expectedOutput = Document{{"$telemetry", Document{{"clearEntries", true}}}};
    ASSERT_DOCUMENT_EQ(telemetry->serialize().getDocument(), expectedOutput);
}

TEST_F(DocumentSourceTelemetryTest, ShouldParseAndSerializeDefaultOptionalArguments) {
    const auto elem = fromjson("{$telemetry: {clearEntries: false}}").firstElement();
    const auto parsed = DocumentSourceTelemetry::createFromBson(elem, getExpCtx());
    const auto telemetry = static_cast<DocumentSourceTelemetry*>(parsed.get());
    const auto expectedOutput = Document{{"$telemetry", Document{{"clearEntries", false}}}};
    ASSERT_DOCUMENT_EQ(telemetry->serialize().getDocument(), expectedOutput);
}

TEST_F(DocumentSourceTelemetryTest, ShouldSerializeOmittedOptionalArguments) {
    const auto elem = fromjson("{$telemetry: {}}").firstElement();
    const auto parsed = DocumentSourceTelemetry::createFromBson(elem, getExpCtx());
    const auto telemetry = static_cast<DocumentSourceTelemetry*>(parsed.get());
    const auto expectedOutput = Document{{"$telemetry", Document{{"clearEntries", false}}}};
    ASSERT_DOCUMENT_EQ(telemetry->serialize().getDocument(), expectedOutput);
}

TEST_F(DocumentSourceTelemetryTest, ShouldReturnEOFImmediatelyIfNoCurrentOps) {
    const auto elem = fromjson("{$telemetry: {}}").firstElement();
    const auto telemetry = DocumentSourceTelemetry::createFromBson(elem, getExpCtx());
    ASSERT_THROWS_CODE(telemetry->getNext(), AssertionException, ErrorCodes::NotImplemented);
}

}  // namespace
}  // namespace mongo
