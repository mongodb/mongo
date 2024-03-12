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

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape/distinct_cmd_shape.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"


namespace mongo::query_shape {

namespace {
const auto testNss = mongo::NamespaceString::createNamespaceString_forTest("testdb.testcoll");

BSONObj distinctJsonToShapeBSON(const char* json,
                                const SerializationOptions& opts,
                                boost::intrusive_ptr<ExpressionContext> expCtx) {

    auto distinct = fromjson(json);
    auto distinctCommand = std::make_unique<DistinctCommandRequest>(DistinctCommandRequest::parse(
        IDLParserContext("distinctCommandRequest",
                         false /* apiStrict */,
                         auth::ValidatedTenancyScope::get(expCtx->opCtx),
                         boost::none,
                         SerializationContext::stateDefault()),
        distinct));
    auto pd = parsed_distinct_command::parse(
        expCtx, std::move(distinct), std::move(distinctCommand), ExtensionsCallbackNoop(), {});
    auto shape = std::make_unique<DistinctCmdShape>(*pd, expCtx);

    return shape->toBson(expCtx->opCtx, opts, {});
}

QueryShapeHash distinctQueryShapeHash(const char* json,
                                      boost::intrusive_ptr<ExpressionContext> expCtx) {

    auto distinct = fromjson(json);
    auto distinctCommand = std::make_unique<DistinctCommandRequest>(DistinctCommandRequest::parse(
        IDLParserContext("distinctCommandRequest",
                         false /* apiStrict */,
                         auth::ValidatedTenancyScope::get(expCtx->opCtx),
                         boost::none,
                         SerializationContext::stateDefault()),
        distinct));
    auto pd = parsed_distinct_command::parse(
        expCtx, std::move(distinct), std::move(distinctCommand), ExtensionsCallbackNoop(), {});
    auto shape = std::make_unique<DistinctCmdShape>(*pd, expCtx);

    return shape->sha256Hash(expCtx->opCtx, {});
}


class ExtractQueryShapeDistinctTest : public unittest::Test {
protected:
    boost::intrusive_ptr<ExpressionContext> expCtx;
    SerializationOptions opts;

    void setUp() {
        expCtx = make_intrusive<ExpressionContextForTest>();
        opts =
            SerializationOptions(SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    }
};

class DistinctShapeSizeTest : public ServiceContextTest {};

TEST_F(ExtractQueryShapeDistinctTest, ExtractFromDistinct) {
    auto expectedShape = fromjson(
        R"({ cmdNs: { db: "testdb", coll: "testcoll" }, command: "distinct", key: "name" })");
    auto distinct = R"({ distinct: "testcoll", $db: "testdb", key: "name" })";

    auto shape = distinctJsonToShapeBSON(distinct, opts, expCtx);

    ASSERT_BSONOBJ_EQ(shape, expectedShape);
}

TEST_F(ExtractQueryShapeDistinctTest, ExtractFromDistinctQuery) {
    auto expectedShape = fromjson(
        R"({
            cmdNs: { db: "testdb", coll: "testcoll" },
            command: "distinct",
            key: "name",
            query: { $and: [ { e1: { $eq: "?" } }, { e2: { $eq: 1 } } ] }
        })");

    auto distinct =
        R"({
            distinct: "testcoll",
            $db: "testdb",
            key: "name",
            query: { e1 : "y", e2: 5 }
        })";

    auto shape = distinctJsonToShapeBSON(distinct, opts, expCtx);

    ASSERT_BSONOBJ_EQ(shape, expectedShape);
}

TEST_F(ExtractQueryShapeDistinctTest, ExtractFromDistinctCollation) {
    auto expectedShape = fromjson(
        R"({
            cmdNs: { db: "testdb", coll: "testcoll" },
            collation: { locale: "fr", strength: 1 },
            command: "distinct",
            key: "name"
        })");

    auto distinct =
        R"({
            distinct: "testcoll",
            $db: "testdb",
            key: "name",
            collation: { locale: "fr", strength: 1 }
        })";

    auto shape = distinctJsonToShapeBSON(distinct, opts, expCtx);

    ASSERT_BSONOBJ_EQ(shape, expectedShape);
}

TEST_F(ExtractQueryShapeDistinctTest, ExtractFromDistinctHint) {
    auto expectedShape = fromjson(
        R"({
            cmdNs: { db: "testdb", coll: "testcoll" },
            command: "distinct",
            key: "name"
        })");

    auto distinct =
        R"({
            distinct: "testcoll",
            $db: "testdb",
            key: "name",
            hint: { x: 1 }
        })";

    auto shape = distinctJsonToShapeBSON(distinct, opts, expCtx);

    ASSERT_BSONOBJ_EQ(shape, expectedShape);
}

TEST_F(ExtractQueryShapeDistinctTest, CompareShapeHashes) {
    auto distinct1 =
        R"({
            distinct: "testcoll",
            $db: "testdb",
            key: "name",
            query: { area : 5 }
        })";

    auto distinct2 =
        R"({
            distinct: "testcoll",
            $db: "testdb",
            key: "name",
            query: { area : 10 }
        })";

    auto distinct3 =
        R"({
            distinct: "testcoll",
            $db: "testdb",
            key: "name",
            query: { volume : 15 }
        })";

    auto hash1 = distinctQueryShapeHash(distinct1, expCtx);
    auto hash2 = distinctQueryShapeHash(distinct2, expCtx);
    auto hash3 = distinctQueryShapeHash(distinct3, expCtx);

    ASSERT_EQ(hash1, hash2);
    ASSERT_NOT_EQUALS(hash1, hash3);
}

TEST_F(DistinctShapeSizeTest, SizeOfShapeComponents) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto distinct = fromjson(R"({ distinct: "testcoll", $db: "testdb", key: "name" })");

    auto distinctCommand = std::make_unique<DistinctCommandRequest>(DistinctCommandRequest::parse(
        IDLParserContext("distinctCommandRequest",
                         false /* apiStrict */,
                         auth::ValidatedTenancyScope::get(expCtx->opCtx),
                         boost::none,
                         SerializationContext::stateDefault()),
        distinct));
    auto pd = parsed_distinct_command::parse(
        expCtx, std::move(distinct), std::move(distinctCommand), ExtensionsCallbackNoop(), {});
    auto components = std::make_unique<DistinctCmdShapeComponents>(*pd, expCtx);
    const auto minimumSize = sizeof(CmdSpecificShapeComponents) + sizeof(BSONObj) +
        sizeof(std::string) + components->key.size() +
        static_cast<size_t>(components->representativeQuery.objsize());

    ASSERT_GTE(components->size(), minimumSize);
    ASSERT_LTE(components->size(), minimumSize + 8 /*padding*/);
}

}  // namespace
}  // namespace mongo::query_shape
