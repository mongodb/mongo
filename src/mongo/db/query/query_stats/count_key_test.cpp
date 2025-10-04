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

#include "mongo/db/query/query_stats/count_key.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo::query_stats {

namespace {

const auto testNss = NamespaceString::createNamespaceString_forTest("testdb.testcoll");
const auto collectionType = query_shape::CollectionType::kCollection;

class CountKeyTest : public ServiceContextTest {
public:
    const boost::intrusive_ptr<ExpressionContext> expCtx =
        make_intrusive<ExpressionContextForTest>();
    const SerializationOptions opts =
        SerializationOptions(SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    const std::unique_ptr<ParsedFindCommand> parsedRequest =
        uassertStatusOK(parsed_find_command::parseFromCount(
            expCtx, CountCommandRequest(testNss), ExtensionsCallbackNoop(), testNss));
};

/**
 * KEY FIELDS
 */

// Test that a count command without any fields generates the expected key.
TEST_F(CountKeyTest, DefaultCountKey) {
    const auto key = std::make_unique<CountKey>(expCtx,
                                                *parsedRequest,
                                                false /* hasLimit */,
                                                false /* hasSkip */,
                                                boost::none /* readConcern */,
                                                false /* maxTimeMS */,
                                                collectionType);

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
    parsedRequest->findCommandRequest->setHint(BSON("a" << 1));
    const auto key = std::make_unique<CountKey>(expCtx,
                                                *parsedRequest,
                                                false /* hasLimit */,
                                                false /* hasSkip */,
                                                boost::none /* readConcern */,
                                                false /* maxTimeMS */,
                                                collectionType);

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
    const auto key = std::make_unique<CountKey>(expCtx,
                                                *parsedRequest,
                                                false /* hasLimit */,
                                                false /* hasSkip */,
                                                repl::ReadConcernArgs::kLocal,
                                                false /* maxTimeMS */,
                                                collectionType);

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
    const auto key = std::make_unique<CountKey>(expCtx,
                                                *parsedRequest,
                                                false /* hasLimit */,
                                                false /* hasSkip */,
                                                boost::none /* readConcern */,
                                                true /* maxTimeMS */,
                                                collectionType);

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
    const auto comment = BSON("comment" << "hello");
    expCtx->getOperationContext()->setComment(comment);
    const auto key = std::make_unique<CountKey>(expCtx,
                                                *parsedRequest,
                                                false /* hasLimit */,
                                                false /* hasSkip */,
                                                boost::none /* readConcern */,
                                                false /* maxTimeMS */,
                                                collectionType);

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
}  // namespace
}  // namespace mongo::query_stats
