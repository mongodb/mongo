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

#include <boost/move/utility_core.hpp>

#include <boost/none.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_query_stats.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {
namespace {

/**
 * Subclass AggregationContextFixture to set the ExpressionContext's namespace to 'admin' with
 * {aggregate: 1} by default, so that parsing tests other than those which validate the namespace do
 * not need to explicitly set it.
 */
class DocumentSourceQueryStatsTest : public AggregationContextFixture {
public:
    DocumentSourceQueryStatsTest()
        : AggregationContextFixture(
              NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin)) {}
};

TEST_F(DocumentSourceQueryStatsTest, ShouldFailToParseIfSpecIsNotObject) {
    ASSERT_THROWS_CODE(DocumentSourceQueryStats::createFromBson(
                           fromjson("{$queryStats: 1}").firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceQueryStatsTest, ShouldFailToParseIfNotRunOnAdmin) {
    getExpCtx()->setNamespaceString(NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "foo")));
    ASSERT_THROWS_CODE(DocumentSourceQueryStats::createFromBson(
                           fromjson("{$queryStats: {}}").firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceQueryStatsTest, ShouldFailToParseIfNotRunWithAggregateOne) {
    getExpCtx()->setNamespaceString(NamespaceString::createNamespaceString_forTest("admin.foo"));
    ASSERT_THROWS_CODE(DocumentSourceQueryStats::createFromBson(
                           fromjson("{$queryStats: {}}").firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceQueryStatsTest, ShouldFailToParseIfUnrecognisedParameterSpecified) {
    ASSERT_THROWS_CODE(DocumentSourceQueryStats::createFromBson(
                           fromjson("{$queryStats: {foo: true}}").firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLUnknownField);
}

TEST_F(DocumentSourceQueryStatsTest, ParseAndSerialize) {
    const auto obj = fromjson("{$queryStats: {}}");
    const auto doc = DocumentSourceQueryStats::createFromBson(obj.firstElement(), getExpCtx());
    const auto queryStatsOp = static_cast<DocumentSourceQueryStats*>(doc.get());
    const auto expected = Document{{"$queryStats", Document{}}};
    const auto serialized = queryStatsOp->serialize().getDocument();
    ASSERT_DOCUMENT_EQ(expected, serialized);

    // Also make sure that we can parse out own serialization output.

    ASSERT_DOES_NOT_THROW(
        DocumentSourceQueryStats::createFromBson(serialized.toBson().firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceQueryStatsTest, ParseAndSerializeShouldIncludeHmacKey) {
    const auto obj = fromjson(R"({
        $queryStats: {
            transformIdentifiers: {
                algorithm: "hmac-sha-256",
                hmacKey: {
                    $binary: "YW4gYXJiaXRyYXJ5IEhNQUNrZXkgZm9yIHRlc3Rpbmc=",
                    $type: "08"
                }
            }
        }
    })");
    const auto doc = DocumentSourceQueryStats::createFromBson(obj.firstElement(), getExpCtx());
    const auto queryStatsOp = static_cast<DocumentSourceQueryStats*>(doc.get());
    const auto expected =
        Document{{"$queryStats",
                  Document{{"transformIdentifiers",
                            Document{{"algorithm", "hmac-sha-256"_sd},
                                     {"hmacKey",
                                      BSONBinData("an arbitrary HMACkey for testing",
                                                  32,
                                                  BinDataType::Sensitive)}}}}}};
    const auto serialized = queryStatsOp->serialize().getDocument();
    ASSERT_DOCUMENT_EQ(serialized, expected);

    // Also make sure that we can parse out own serialization output.

    ASSERT_DOES_NOT_THROW(
        DocumentSourceQueryStats::createFromBson(serialized.toBson().firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceQueryStatsTest, ShouldFailToParseIfAlgorithmIsNotSupported) {
    auto obj = fromjson(R"({
        $queryStats: {
            transformIdentifiers: {
                algorithm: "randomalgo"
            }
        }
    })");
    ASSERT_THROWS_CODE(DocumentSourceQueryStats::createFromBson(obj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(DocumentSourceQueryStatsTest,
       ShouldFailToParseIfTransformIdentifiersSpecifiedButEmptyAlgorithm) {
    auto obj = fromjson(R"({
        $queryStats: {
            transformIdentifiers: {
                algorithm: ""
            }
        }
    })");
    ASSERT_THROWS_CODE(DocumentSourceQueryStats::createFromBson(obj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(DocumentSourceQueryStatsTest,
       ShouldFailToParseIfTransformIdentifiersSpecifiedButNoAlgorithm) {
    auto obj = fromjson(R"({
        $queryStats: {
            transformIdentifiers: {
            }
        }
    })");
    ASSERT_THROWS_CODE(DocumentSourceQueryStats::createFromBson(obj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}
}  // namespace
}  // namespace mongo
