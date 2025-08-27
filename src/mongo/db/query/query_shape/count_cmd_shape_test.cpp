/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/query_shape/count_cmd_shape.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"


namespace mongo::query_shape {

namespace {

const auto testNss = mongo::NamespaceString::createNamespaceString_forTest("testdb.testcoll");

class CountCmdShapeTest : public ServiceContextTest {
public:
    std::unique_ptr<CountCmdShape> checkShapeBSON(const CountCommandRequest& ccr,
                                                  const BSONObj& expectedShape,
                                                  const SerializationOptions serializationOptions) {
        const auto parsedRequest = uassertStatusOK(
            parsed_find_command::parseFromCount(expCtx, ccr, extensionsCallback, testNss));
        auto shape = std::make_unique<CountCmdShape>(
            *parsedRequest, ccr.getLimit().has_value(), ccr.getSkip().has_value());
        ASSERT_BSONOBJ_EQ(expectedShape,
                          shape->toBson(expCtx->getOperationContext(), serializationOptions, {}));
        return shape;
    }

    QueryShapeHash makeShapeHash(const char* json) {
        const auto count = fromjson(json);
        const auto countCommand = std::make_unique<CountCommandRequest>(CountCommandRequest::parse(
            count,
            IDLParserContext("countCommandRequest",
                             auth::ValidatedTenancyScope::get(expCtx->getOperationContext()),
                             boost::none,
                             SerializationContext::stateDefault())));
        const auto parsedFind = uassertStatusOK(parsed_find_command::parseFromCount(
            expCtx, *countCommand, extensionsCallback, testNss));
        const auto shape = std::make_unique<CountCmdShape>(
            *parsedFind, countCommand->getLimit().has_value(), countCommand->getSkip().has_value());
        return shape->sha256Hash(expCtx->getOperationContext(), {});
    }

    std::unique_ptr<CountCmdShapeComponents> makeShapeComponentsFromQuery(const BSONObj& query) {
        const auto ccr = std::make_unique<CountCommandRequest>(testNss);
        ccr->setQuery(query);
        const auto parsedRequest = uassertStatusOK(
            parsed_find_command::parseFromCount(expCtx, *ccr, extensionsCallback, testNss));
        return std::make_unique<CountCmdShapeComponents>(
            *parsedRequest, false /* hasLimit */, false /* hasSkip */);
    }

    const SerializationOptions representativeShapeOptions =
        SerializationOptions(SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    const boost::intrusive_ptr<ExpressionContext> expCtx =
        make_intrusive<ExpressionContextForTest>();
    const ExtensionsCallbackNoop extensionsCallback{};
};

/**
 * QUERY SHAPE FIELDS
 */

// Test that a count command without any fields generates the expected shape.
TEST_F(CountCmdShapeTest, DefaultCountShape) {
    // NOTE: The namespace passed into this constructor is not used in any tests. The namespace
    // passed into parseFromCount is used.
    const auto ccr = std::make_unique<CountCommandRequest>(testNss);
    const auto expectedShape =
        fromjson(R"({ cmdNs: { db: "testdb", coll: "testcoll" }, command: "count"})");
    const auto shape = checkShapeBSON(*ccr, expectedShape, representativeShapeOptions);
    ASSERT_FALSE(shape->components.hasField.limit);
    ASSERT_FALSE(shape->components.hasField.skip);
}

// Test that the query field of the count command is included in the shape and shapified.
TEST_F(CountCmdShapeTest, CountQueryShape) {
    const auto query = BSON("a" << "y"
                                << "b" << 42);
    const auto ccr = std::make_unique<CountCommandRequest>(testNss);
    ccr->setQuery(query);
    const auto expectedShape = fromjson(
        R"({
            cmdNs: { db: "testdb", coll: "testcoll" },
            command: "count",
            query: { $and: [ { a: { $eq: "?" } }, { b: { $eq: 1 } } ] }
        })");
    checkShapeBSON(*ccr, expectedShape, representativeShapeOptions);
}

// Test that the collation field of the count command is included in the shape and not shapified.
TEST_F(CountCmdShapeTest, CountCollationShape) {
    const auto collation = BSON("locale" << "fr"
                                         << "strength" << 1);
    const auto ccr = std::make_unique<CountCommandRequest>(testNss);
    ccr->setCollation(collation);
    const auto expectedShape = fromjson(
        R"({
            cmdNs: { db: "testdb", coll: "testcoll" },
            collation: { locale: "fr", strength: 1 },
            command: "count"
        })");
    checkShapeBSON(*ccr, expectedShape, representativeShapeOptions);
}

// Test that the limit field of the count command is included in the shape and shapified.
TEST_F(CountCmdShapeTest, CountLimitShape) {
    const auto ccr = std::make_unique<CountCommandRequest>(testNss);
    ccr->setLimit(50);
    const auto expectedShape = fromjson(
        R"({
            cmdNs: { db: "testdb", coll: "testcoll" },
            command: "count",
            limit: 1
        })");
    const auto shape = checkShapeBSON(*ccr, expectedShape, representativeShapeOptions);
    ASSERT_TRUE(shape->components.hasField.limit);
}

// Test that the skip field of the count command is included in the shape and shapified.
TEST_F(CountCmdShapeTest, CountSkipShape) {
    const auto ccr = std::make_unique<CountCommandRequest>(testNss);
    ccr->setSkip(50);
    const auto expectedShape = fromjson(
        R"({
            cmdNs: { db: "testdb", coll: "testcoll" },
            command: "count",
            skip: 1
        })");
    const auto shape = checkShapeBSON(*ccr, expectedShape, representativeShapeOptions);
    ASSERT_TRUE(shape->components.hasField.skip);
}

// Test that the query, limit, and skip fields are properly serialized when using the debug format.
TEST_F(CountCmdShapeTest, CountShapeDebugFormat) {
    const auto ccr = std::make_unique<CountCommandRequest>(testNss);
    const auto query = BSON("a" << "y"
                                << "b" << 42);
    const auto collation = BSON("locale" << "fr"
                                         << "strength" << 1);
    ccr->setQuery(query);
    ccr->setCollation(collation);
    ccr->setLimit(50);
    ccr->setSkip(50);
    const auto expectedShape = fromjson(
        R"({
            cmdNs: { db: "testdb", coll: "testcoll" },
            collation: { locale: "fr", strength: 1 },
            command: "count",
            query: { $and: [ { a: { $eq: "?string" } }, { b: { $eq: "?number" } } ] },
            limit: "?number",
            skip: "?number"
        })");
    checkShapeBSON(*ccr,
                   expectedShape,
                   SerializationOptions(SerializationOptions::kDebugQueryShapeSerializeOptions));
}

/**
 * QUERY SHAPE HASH
 */

// Test that the shapified query field of the count command hashes to the same value when equal, and
// a different hash value otherwise.
TEST_F(CountCmdShapeTest, CompareQueryShapeHashes) {
    const auto count1 =
        R"({
            count: "testcoll",
            $db: "testdb",
            query: { area : 5 }
        })";

    const auto count2 =
        R"({
            count: "testcoll",
            $db: "testdb",
            query: { area : 10 }
        })";

    const auto count3 =
        R"({
            count: "testcoll",
            $db: "testdb",
            query: { volume : 15 }
        })";

    const auto hash1 = makeShapeHash(count1);
    const auto hash2 = makeShapeHash(count2);
    const auto hash3 = makeShapeHash(count3);

    ASSERT_EQ(hash1, hash2);
    ASSERT_NOT_EQUALS(hash1, hash3);
}

// Test that the collation field of the count command hashes to the same value only when strictly
// equal, and a different hash value otherwise.
TEST_F(CountCmdShapeTest, CompareCollationHashes) {
    const auto count1 =
        R"({
            count: "testcoll",
            $db: "testdb",
            collation: { locale: "fr", strength: 1 }
        })";

    const auto count2 =
        R"({
            count: "testcoll",
            $db: "testdb",
            collation: { locale: "fr", strength: 5 }
        })";

    const auto count3 =
        R"({
            count: "testcoll",
            $db: "testdb",
            collation: { locale: "simple", caseLevel: true }
        })";

    // Hash the same command twice to ensure strict equality exists.
    const auto hash1 = makeShapeHash(count1);
    const auto hash2 = makeShapeHash(count1);
    const auto hash3 = makeShapeHash(count2);
    const auto hash4 = makeShapeHash(count3);

    ASSERT_EQ(hash1, hash2);
    ASSERT_NOT_EQUALS(hash1, hash3);
    ASSERT_NOT_EQUALS(hash3, hash4);
}

// Test that the shapified limit field of the count command hashes to the same value when equal, and
// a different hash value otherwise.
TEST_F(CountCmdShapeTest, CompareLimitHashes) {
    const auto count1 =
        R"({
            count: "testcoll",
            $db: "testdb",
            limit: 10
        })";

    const auto count2 =
        R"({
            count: "testcoll",
            $db: "testdb",
            limit: 50
        })";

    const auto hash1 = makeShapeHash(count1);
    const auto hash2 = makeShapeHash(count2);

    ASSERT_EQ(hash1, hash2);
}

// Test that the shapified skip field of the count command hashes to the same value when equal, and
// a different hash value otherwise.
TEST_F(CountCmdShapeTest, CompareSkipHashes) {
    const auto count1 =
        R"({
            count: "testcoll",
            $db: "testdb",
            skip: 10
        })";

    const auto count2 =
        R"({
            count: "testcoll",
            $db: "testdb",
            skip: 50
        })";

    const auto hash1 = makeShapeHash(count1);
    const auto hash2 = makeShapeHash(count2);

    ASSERT_EQ(hash1, hash2);
}

// Verifies that count command shape hash value is stable (does not change between the versions
// of the server). Unlike the CountCmdShapeComponents::HashValue function, the
// CountCmdShape::sha256Hash function should always return the same value for the same input.
TEST_F(CountCmdShapeTest, StableQueryShapeHashValue) {
    auto assertCountQueryShapeHashEquals = [&](std::string expectedHashValue,
                                               std::string countCommand) {
        const auto hash = makeShapeHash(countCommand.data());
        ASSERT_EQ(expectedHashValue, hash.toHexString()) << " command: " << countCommand;
    };

    // Verify shape hash value is equal to the expected one when the command targets a namespace.
    assertCountQueryShapeHashEquals(
        "73F5A83DE5793C686AEB35CA93FE96D0A75ACC5138504BE5C69615F59F36BF8E",
        R"({
            count: "testcoll",
            $db: "testdb",
            query: { area : 5 },
            collation: { locale: "fr" },
            limit: 10,
            skip: 5})");

    // Verify shape hash value is equal to the expected one when the command does not have "query"
    // parameter.
    assertCountQueryShapeHashEquals(
        "5784B07A6169DD64F7083669918BA54279D73B6656DBD13954BC1381F2FBE639",
        R"({
            count: "testcoll",
            $db: "testdb",
            collation: { locale: "fr" },
            limit: 10,
            skip: 5})");

    // Verify shape hash value is equal to the expected one when the command does not have
    // "collation" parameter.
    assertCountQueryShapeHashEquals(
        "81611869E1CB6B76C8A413BE81D2D72A7E0492118B5E69D9788B2C47185E1EA9",
        R"({
            count: "testcoll",
            $db: "testdb",
            query: { area : 5 },
            limit: 10,
            skip: 5})");

    // Verify shape hash value is equal to the expected one when the command does not have
    // "limit" parameter.
    assertCountQueryShapeHashEquals(
        "F6047E44B153AC61A241C2C825E8E52C60A6B6DA4A627724E242E037A3D8FD27",
        R"({
            count: "testcoll",
            $db: "testdb",
            query: { area : 5 },
            collation: { locale: "fr" },
            skip: 5})");

    // Verify shape hash value is equal to the expected one when the command does not have
    // "skip" parameter.
    assertCountQueryShapeHashEquals(
        "0B33FAD0DECE534A7FE6F9B1D640C2815DB7F701F73E4B29849D426DC62C5A62",
        R"({
            count: "testcoll",
            $db: "testdb",
            query: { area : 5 },
            collation: { locale: "fr" },
            limit: 10})");
}

/**
 * QUERY SHAPE SIZE
 */

// Test that the size of the query shape included the query and the command specific components.
TEST_F(CountCmdShapeTest, SizeOfShapeComponents) {
    const auto query = BSON("a" << "y"
                                << "b" << 42);
    const auto countCmdComponent = makeShapeComponentsFromQuery(query);
    const auto querySize = countCmdComponent->representativeQuery.objsize();

    ASSERT_EQ(countCmdComponent->size(), sizeof(CountCmdShapeComponents) + querySize);
}

// Test that the size of the query shape changes based on the size of the query.
TEST_F(CountCmdShapeTest, DifferentShapeComponentsSizes) {
    const auto smallQuery = BSON("a" << BSONObj());
    const auto smallFindCmdComponent = makeShapeComponentsFromQuery(smallQuery);

    const auto largeQuery = BSON("a" << 1 << "b" << 42);
    const auto largeFindCmdComponent = makeShapeComponentsFromQuery(largeQuery);

    ASSERT_LT(smallQuery.objsize(), largeQuery.objsize());
    ASSERT_LT(smallFindCmdComponent->size(), largeFindCmdComponent->size());
}

}  // namespace
}  // namespace mongo::query_shape
