// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/distinct_key.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_distinct.h"
#include "mongo/db/query/distinct_command_gen.h"
#include "mongo/db/query/query_shape/query_shape_hash.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string_view>

#include <absl/hash/hash.h>

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
    query_shape::SerializationOptions opts = query_shape::SerializationOptions(
        query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);

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
    query_shape::SerializationOptions opts = query_shape::SerializationOptions(
        query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);

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
    query_shape::SerializationOptions opts = query_shape::SerializationOptions(
        query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);

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
    query_shape::SerializationOptions opts = query_shape::SerializationOptions(
        query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    opts.literalPolicy = query_shape::LiteralSerializationPolicy::kToDebugTypeString;

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

TEST_F(DistinctKeyTest, OriginalQueryShapeHashAppearsInKey) {
    boost::intrusive_ptr<ExpressionContext> expCtx = make_intrusive<ExpressionContextForTest>();
    query_shape::SerializationOptions opts = query_shape::SerializationOptions(
        query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    const auto hash = query_shape::QueryShapeHash::fromHexString(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

    auto dcr = std::make_unique<DistinctCommandRequest>(kDefaultTestNss);
    dcr->setKey("name");
    dcr->setOriginalQueryShapeHash(hash);
    auto parsedDistinct =
        parsed_distinct_command::parse(expCtx, std::move(dcr), ExtensionsCallbackNoop(), {});
    auto distinctShape = std::make_unique<query_shape::DistinctCmdShape>(*parsedDistinct, expCtx);
    auto key = std::make_unique<DistinctKey>(
        expCtx, *parsedDistinct->distinctCommandRequest, std::move(distinctShape), collectionType);

    const auto keyBson = key->toBson(expCtx->getOperationContext(), opts, {});
    ASSERT_EQ(keyBson["originalQueryShapeHash"].str(),
              "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
}

TEST_F(DistinctKeyTest, DifferentOriginalQueryShapeHashesProduceDifferentKeys) {
    auto makeKeyWithHash = [](std::string_view hexHash) {
        auto expCtx = make_intrusive<ExpressionContextForTest>();
        auto dcr = std::make_unique<DistinctCommandRequest>(kDefaultTestNss);
        dcr->setKey("name");
        dcr->setOriginalQueryShapeHash(query_shape::QueryShapeHash::fromHexString(hexHash));
        auto parsedDistinct =
            parsed_distinct_command::parse(expCtx, std::move(dcr), ExtensionsCallbackNoop(), {});
        auto distinctShape =
            std::make_unique<query_shape::DistinctCmdShape>(*parsedDistinct, expCtx);
        return std::make_unique<DistinctKey>(expCtx,
                                             *parsedDistinct->distinctCommandRequest,
                                             std::move(distinctShape),
                                             collectionType);
    };

    auto keyA = makeKeyWithHash("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    auto keyB = makeKeyWithHash("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

    ASSERT_NE(absl::HashOf(*keyA), absl::HashOf(*keyB));
}

}  // namespace
}  // namespace mongo::query_stats
