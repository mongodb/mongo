// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/count_key.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape/query_shape_hash.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

#include <absl/hash/hash.h>

namespace mongo::query_stats {

namespace {

const auto testNss = NamespaceString::createNamespaceString_forTest("testdb.testcoll");
const auto collectionType = query_shape::CollectionType::kCollection;

class CountKeyTest : public ServiceContextTest {
public:
    const boost::intrusive_ptr<ExpressionContext> expCtx =
        make_intrusive<ExpressionContextForTest>();
    const query_shape::SerializationOptions opts = query_shape::SerializationOptions(
        query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);

    std::unique_ptr<query_shape::CountCmdShape> makeCountShapeFromRequest(CountCommandRequest req) {
        const std::unique_ptr<ParsedFindCommand> parsedRequest = uassertStatusOK(
            parsed_find_command::parseFromCount(expCtx, req, ExtensionsCallbackNoop(), testNss));
        return std::make_unique<query_shape::CountCmdShape>(
            *parsedRequest, req.getLimit().has_value(), req.getSkip().has_value());
    }
};

/**
 * KEY FIELDS
 */

// Test that a count command without any fields generates the expected key.
TEST_F(CountKeyTest, DefaultCountKey) {
    const CountCommandRequest& countReq = CountCommandRequest(testNss);
    const auto key = std::make_unique<CountKey>(
        expCtx, countReq, makeCountShapeFromRequest(countReq), collectionType);

    const auto expectedKey = fromjson(
        R"({
            queryShape: {
                cmdNs: { db: "testdb", coll: "testcoll" }, 
                command: "count" 
            }, 
            "collectionType": "collection"
        })");

    ASSERT_BSONOBJ_EQ(expectedKey, key->toBson(expCtx->getOperationContext(), opts, {}));
}

// Test that the hint parameter is included in the key.
TEST_F(CountKeyTest, CountHintKey) {
    CountCommandRequest request = CountCommandRequest(testNss);
    request.setHint(BSON("a" << 1));
    const auto key = std::make_unique<CountKey>(
        expCtx, request, makeCountShapeFromRequest(request), collectionType);

    const auto expectedKey = fromjson(
        R"({
            queryShape: {
                cmdNs: { db: "testdb", coll: "testcoll" }, 
                command: "count" 
            }, 
            "collectionType": "collection",
            "hint": { a: 1 }
        })");

    ASSERT_BSONOBJ_EQ(expectedKey, key->toBson(expCtx->getOperationContext(), opts, {}));
}

// Test that the readConcern parameter is included in the key.
TEST_F(CountKeyTest, CountReadConcernKey) {
    CountCommandRequest request = CountCommandRequest(testNss);
    request.setReadConcern(repl::ReadConcernArgs::kLocal);
    const auto key = std::make_unique<CountKey>(
        expCtx, request, makeCountShapeFromRequest(request), collectionType);

    const auto expectedKey = fromjson(
        R"({
            queryShape: {
                cmdNs: { db: "testdb", coll: "testcoll" }, 
                command: "count" 
            }, 
            "readConcern": { level: "local" },
            "collectionType": "collection"
        })");

    ASSERT_BSONOBJ_EQ(expectedKey, key->toBson(expCtx->getOperationContext(), opts, {}));
}

// Test that the maxTimeMS parameter is included in the key.
TEST_F(CountKeyTest, CountMaxTimeMSKey) {
    CountCommandRequest request = CountCommandRequest(testNss);
    request.setMaxTimeMS(1000);
    const auto key = std::make_unique<CountKey>(
        expCtx, request, makeCountShapeFromRequest(request), collectionType);

    const auto expectedKey = fromjson(
        R"({
            queryShape: {
                cmdNs: { db: "testdb", coll: "testcoll" }, 
                command: "count" 
            }, 
            "collectionType": "collection",
            "maxTimeMS": 1
        })");

    ASSERT_BSONOBJ_EQ(expectedKey, key->toBson(expCtx->getOperationContext(), opts, {}));
}

// Test that the comment parameter is included in the key.
TEST_F(CountKeyTest, CountCommentKey) {
    CountCommandRequest request = CountCommandRequest(testNss);
    const auto comment = BSON("comment" << "hello");
    expCtx->getOperationContext()->setComment(comment);
    const auto key = std::make_unique<CountKey>(
        expCtx, request, makeCountShapeFromRequest(request), collectionType);

    const auto expectedKey = fromjson(
        R"({
            queryShape: {
                cmdNs: { db: "testdb", coll: "testcoll" }, 
                command: "count" 
            },
            "comment": "?", 
            "collectionType": "collection"
        })");

    ASSERT_BSONOBJ_EQ(expectedKey, key->toBson(expCtx->getOperationContext(), opts, {}));
}

TEST_F(CountKeyTest, OriginalQueryShapeHashAppearsInKey) {
    // Build the shape from a plain request. parseFromCount rejects originalQueryShapeHash
    // unless the client is internal (it's an internal-only field set by routers on shards).
    // The shape only cares about filter/collation/hint, so using a separate plain request is fine.
    CountCommandRequest shapeRequest = CountCommandRequest(testNss);
    auto countShape = makeCountShapeFromRequest(shapeRequest);

    CountCommandRequest keyRequest = CountCommandRequest(testNss);
    const auto hash = query_shape::QueryShapeHash::fromHexString(
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    keyRequest.setOriginalQueryShapeHash(hash);
    const auto key =
        std::make_unique<CountKey>(expCtx, keyRequest, std::move(countShape), collectionType);

    const auto keyBson = key->toBson(expCtx->getOperationContext(), opts, {});
    ASSERT_EQ(keyBson["originalQueryShapeHash"].str(),
              "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
}

TEST_F(CountKeyTest, OriginalQueryShapeHashAbsentWhenNotSet) {
    const CountCommandRequest request = CountCommandRequest(testNss);
    const auto key = std::make_unique<CountKey>(
        expCtx, request, makeCountShapeFromRequest(request), collectionType);

    const auto keyBson = key->toBson(expCtx->getOperationContext(), opts, {});
    ASSERT_TRUE(keyBson["originalQueryShapeHash"].eoo());
}

TEST_F(CountKeyTest, DifferentOriginalQueryShapeHashesProduceDifferentKeys) {
    // Build shape once; only the key request needs the hash.
    CountCommandRequest shapeRequest = CountCommandRequest(testNss);

    auto makeKeyWithHash = [&](std::string_view hexHash) {
        auto countShape = makeCountShapeFromRequest(shapeRequest);
        CountCommandRequest keyRequest = CountCommandRequest(testNss);
        keyRequest.setOriginalQueryShapeHash(query_shape::QueryShapeHash::fromHexString(hexHash));
        return std::make_unique<CountKey>(
            expCtx, keyRequest, std::move(countShape), collectionType);
    };

    auto keyA = makeKeyWithHash("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    auto keyB = makeKeyWithHash("BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");

    ASSERT_NE(absl::HashOf(*keyA), absl::HashOf(*keyB));
}

}  // namespace
}  // namespace mongo::query_stats
