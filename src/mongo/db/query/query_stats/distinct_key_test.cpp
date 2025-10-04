/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/query_stats/distinct_key.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_distinct.h"
#include "mongo/db/query/distinct_command_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo::query_stats {

namespace {
static const NamespaceString kDefaultTestNss =
    NamespaceString::createNamespaceString_forTest("testdb.testcoll");

static constexpr auto collectionType = query_shape::CollectionType::kCollection;


class DistinctKeyTest : public ServiceContextTest {
public:
    static std::unique_ptr<const Key> makeDistinctKeyFromQuery(
        const BSONObj& distinct, boost::intrusive_ptr<ExpressionContext> expCtx) {

        auto dcr = std::make_unique<DistinctCommandRequest>(DistinctCommandRequest::parse(
            distinct,
            IDLParserContext("distinctCommandRequest",
                             auth::ValidatedTenancyScope::get(expCtx->getOperationContext()),
                             boost::none,
                             SerializationContext::stateDefault())));

        auto parsedDistinct =
            parsed_distinct_command::parse(expCtx, std::move(dcr), ExtensionsCallbackNoop(), {});
        auto distinctShape =
            std::make_unique<query_shape::DistinctCmdShape>(*parsedDistinct, expCtx);
        return std::make_unique<DistinctKey>(expCtx,
                                             *parsedDistinct->distinctCommandRequest,
                                             std::move(distinctShape),
                                             collectionType);
    }
};

TEST_F(DistinctKeyTest, ExtractKeyFromDistinctRequiredFields) {
    boost::intrusive_ptr<ExpressionContext> expCtx = make_intrusive<ExpressionContextForTest>();
    SerializationOptions opts =
        SerializationOptions(SerializationOptions::kRepresentativeQueryShapeSerializeOptions);

    auto expectedKey = fromjson(
        R"({
            "queryShape" : {
 		        "cmdNs" : { "db" : "testdb", "coll" : "testcoll" },
                "command" : "distinct",
                "key" : "name" },
            "collectionType" : "collection"
        })");

    auto distinct = fromjson(
        R"({
            distinct: "testcoll",
            $db: "testdb",
            key: "name"
        })");

    auto key = makeDistinctKeyFromQuery(distinct, expCtx);
    auto keyBSON =
        key->toBson(expCtx->getOperationContext(), opts, SerializationContext::stateDefault());

    ASSERT_BSONOBJ_EQ(keyBSON, expectedKey);
}

TEST_F(DistinctKeyTest, ExtractKeyFromDistinctQuery) {
    boost::intrusive_ptr<ExpressionContext> expCtx = make_intrusive<ExpressionContextForTest>();
    SerializationOptions opts =
        SerializationOptions(SerializationOptions::kRepresentativeQueryShapeSerializeOptions);

    auto expectedKey = fromjson(
        R"({
            "queryShape" : {
 		        "cmdNs" : { "db" : "testdb", "coll" : "testcoll" },
                "command" : "distinct",
                "key" : "name",
                "query" : { "$and" : [ { "e1" : { "$eq" : "?" } }, { "e2" : { "$eq" : 1 } } ] } },
            "collectionType" : "collection"
        })");

    auto distinct = fromjson(
        R"({
            distinct: "testcoll",
            $db: "testdb",
            key: "name",
            query: { e1 : "y", e2 : 5 }
        })");

    auto key = makeDistinctKeyFromQuery(distinct, expCtx);
    auto keyBSON =
        key->toBson(expCtx->getOperationContext(), opts, SerializationContext::stateDefault());

    ASSERT_BSONOBJ_EQ(keyBSON, expectedKey);
}

TEST_F(DistinctKeyTest, ExtractKeyFromDistinctComplex) {
    boost::intrusive_ptr<ExpressionContext> expCtx = make_intrusive<ExpressionContextForTest>();
    SerializationOptions opts =
        SerializationOptions(SerializationOptions::kRepresentativeQueryShapeSerializeOptions);

    auto expectedKey = fromjson(
        R"({
            "queryShape" : {
 		        "cmdNs" : { "db" : "testdb", "coll" : "testcoll" },
                "collation" : { locale: "fr", strength: 1 },
                "command" : "distinct",
                "key" : "name",
                "query" : { "$and" : [ { "e1" : { "$eq" : "?" } }, { "e2" : { "$eq" : 1 } } ] } },
            "comment" : "?",
            "readConcern" : { level : "local" },
            "collectionType" : "collection",
            "hint" : { x : 1 }
        })");

    auto distinct = fromjson(
        R"({
            distinct: "testcoll",
            $db: "testdb",
            key: "name",
            query: { e1 : "y", e2 : 5 },
            hint: { x : 1 },
            readConcern: { level : "local" },
            collation: { locale: "fr", strength: 1 }
        })");
    // For this test, the comment must be set individually outside of the original command BSON
    // because it is not set when being parsed into a DistinctCommandRequest.
    expCtx->getOperationContext()->setComment(BSON("comment" << "hello"));

    auto key = makeDistinctKeyFromQuery(distinct, expCtx);
    auto keyBSON =
        key->toBson(expCtx->getOperationContext(), opts, SerializationContext::stateDefault());

    ASSERT_BSONOBJ_EQ(keyBSON, expectedKey);
}

TEST_F(DistinctKeyTest, ExtractKeyFromDistinctQueryDebugString) {
    boost::intrusive_ptr<ExpressionContext> expCtx = make_intrusive<ExpressionContextForTest>();
    SerializationOptions opts =
        SerializationOptions(SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    opts.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;

    auto expectedKey = fromjson(
        R"({
            "queryShape" : {
 		        "cmdNs" : { "db" : "testdb", "coll" : "testcoll" },
                "command" : "distinct",
                "key" : "name",
                "query" : { "$and" : [ { "e1" : { "$eq" : "?string" } }, {"e2" : { "$eq" : "?number" } } ] } },
            "collectionType" : "collection"
        })");

    auto distinct = fromjson(
        R"({
            distinct: "testcoll",
            $db: "testdb",
            key: "name",
            query: { e1 : "y", e2 : 5 }
        })");

    auto key = makeDistinctKeyFromQuery(distinct, expCtx);
    auto keyBSON =
        key->toBson(expCtx->getOperationContext(), opts, SerializationContext::stateDefault());

    ASSERT_BSONOBJ_EQ(keyBSON, expectedKey);
}

}  // namespace
}  // namespace mongo::query_stats
