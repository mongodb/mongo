// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/distinct_cmd_shape.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/unittest/unittest.h"


namespace mongo::query_shape {

namespace {
const auto testNss = mongo::NamespaceString::createNamespaceString_forTest("testdb.testcoll");

BSONObj distinctJsonToShapeBSON(const char* json,
                                const query_shape::SerializationOptions& opts,
                                boost::intrusive_ptr<ExpressionContext> expCtx) {

    auto distinct = fromjson(json);
    auto distinctCommand = std::make_unique<DistinctCommandRequest>(DistinctCommandRequest::parse(
        distinct,
        IDLParserContext("distinctCommandRequest",
                         auth::ValidatedTenancyScope::get(expCtx->getOperationContext()),
                         boost::none,
                         SerializationContext::stateDefault())));
    auto pd = parsed_distinct_command::parse(
        expCtx, std::move(distinctCommand), ExtensionsCallbackNoop(), {});
    auto shape = std::make_unique<DistinctCmdShape>(*pd, expCtx);

    return shape->toBson(expCtx->getOperationContext(), opts, {});
}

QueryShapeHash distinctQueryShapeHash(const char* json,
                                      boost::intrusive_ptr<ExpressionContext> expCtx) {

    auto distinct = fromjson(json);
    auto distinctCommand = std::make_unique<DistinctCommandRequest>(DistinctCommandRequest::parse(
        distinct,
        IDLParserContext("distinctCommandRequest",
                         auth::ValidatedTenancyScope::get(expCtx->getOperationContext()),
                         boost::none,
                         SerializationContext::stateDefault())));
    auto pd = parsed_distinct_command::parse(
        expCtx, std::move(distinctCommand), ExtensionsCallbackNoop(), {});
    auto shape = std::make_unique<DistinctCmdShape>(*pd, expCtx);

    return shape->sha256Hash(expCtx->getOperationContext(), {});
}


class ExtractQueryShapeDistinctTest : public unittest::Test {
protected:
    boost::intrusive_ptr<ExpressionContext> expCtx;
    query_shape::SerializationOptions opts;

    void setUp() override {
        expCtx = make_intrusive<ExpressionContextForTest>();
        opts = query_shape::SerializationOptions(
            SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
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

// Verifies that "distinct" command shape hash value is stable (does not change between the versions
// of the server).
TEST_F(ExtractQueryShapeDistinctTest, StableQueryShapeHashValue) {
    auto assertDistinctQueryShapeHashEquals = [&](std::string expectedHashValue,
                                                  std::string distinctCommand) {
        const auto hash = distinctQueryShapeHash(distinctCommand.data(), expCtx);
        ASSERT_EQ(expectedHashValue, hash.toHexString()) << " command: " << distinctCommand;
    };

    // Verify shape hash value is equal to the expected one when the command targets a namespace.
    assertDistinctQueryShapeHashEquals(
        "344BA27EB45373D8CACF33C7341B317B4E897877D808927683805FF553A7CC42",
        R"({
            distinct: "testcoll",
            $db: "testdb",
            key: "name",
            query: { area : 5 },
            collation: { locale: "fr" }})");

    // Verify shape hash value is equal to the expected one when the command targets a collection by
    // its UUID.
    assertDistinctQueryShapeHashEquals(
        "C58B4458C460C43DE7591F90D26BCFFC44090EB091DAEEBE9ECD540608E0CCFC",
        R"({
            distinct: {"$uuid": "80EC854A-FA7D-A4B0-F533-8C0682000102"},
            $db: "testdb",
            key: "name",
            query: { area : 5 },
            collation: { locale: "fr" }})");

    // Verify shape hash value is equal to the expected one when the command does not have "query"
    // parameter.
    assertDistinctQueryShapeHashEquals(
        "34DA6D26C35E9B941AB800E728F37DE65A43EA372F55E0715C0A91365500DF38",
        R"({
            distinct: "testcoll",
            $db: "testdb",
            key: "name",
            collation: { locale: "fr" }})");

    // Verify shape hash value is equal to the expected one when the command does not have
    // "collation" parameter.
    assertDistinctQueryShapeHashEquals(
        "6885616B1ACD8B4F7FB5C1EB2A17B482EBB8788D63AC033160AA3A848F125DEA",
        R"({
            distinct: "testcoll",
            $db: "testdb",
            key: "name",
            query: { area : 5 }})");
}

// -------------------------------------------------------------------------
// rawData tests
// -------------------------------------------------------------------------

class DistinctRawDataTest : public ServiceContextTest {
protected:
    boost::intrusive_ptr<ExpressionContext> expCtx;
    void setUp() override {
        ServiceContextTest::setUp();
        expCtx = make_intrusive<ExpressionContextForTest>();
    }

    std::unique_ptr<DistinctCmdShape> makeShape(const char* keyStr,
                                                boost::optional<bool> rawDataVal) {
        auto json =
            std::string(R"({ distinct: "testcoll", $db: "testdb", key: ")") + keyStr + "\"}";
        auto distinct = fromjson(json);
        auto distinctCommand =
            std::make_unique<DistinctCommandRequest>(DistinctCommandRequest::parse(
                distinct,
                IDLParserContext("distinctCommandRequest",
                                 auth::ValidatedTenancyScope::get(expCtx->getOperationContext()),
                                 boost::none,
                                 SerializationContext::stateDefault())));
        if (rawDataVal.has_value()) {
            distinctCommand->setRawData(*rawDataVal);
        }
        auto pd = parsed_distinct_command::parse(
            expCtx, std::move(distinctCommand), ExtensionsCallbackNoop(), {});
        return std::make_unique<DistinctCmdShape>(*pd, expCtx);
    }
};

TEST_F(DistinctRawDataTest, RawDataTrueAppearsInShape) {
    auto shape = makeShape("field", true);
    auto shapeBson = shape->toBson(expCtx->getOperationContext(),
                                   SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                                   {});
    ASSERT_TRUE(shapeBson.hasField(DistinctCommandRequest::kRawDataFieldName));
    ASSERT_TRUE(shapeBson[DistinctCommandRequest::kRawDataFieldName].boolean());
}

TEST_F(DistinctRawDataTest, RawDataAbsentOrFalseNotInShape) {
    for (auto rawDataVal : {boost::optional<bool>{}, boost::optional<bool>{false}}) {
        auto shape = makeShape("field", rawDataVal);
        ASSERT_FALSE(shape->rawData);
        auto shapeBson =
            shape->toBson(expCtx->getOperationContext(),
                          SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                          {});
        ASSERT_FALSE(shapeBson.hasField(DistinctCommandRequest::kRawDataFieldName));
    }
}

TEST_F(DistinctRawDataTest, RawDataDifferentiatesQueryShape) {
    auto hashNone = makeShape("field", boost::none)->sha256Hash(expCtx->getOperationContext(), {});
    auto hashTrue = makeShape("field", true)->sha256Hash(expCtx->getOperationContext(), {});
    auto hashFalse = makeShape("field", false)->sha256Hash(expCtx->getOperationContext(), {});

    ASSERT_NE(hashNone.toHexString(), hashTrue.toHexString());
    // rawData=false is normalized to absent — same hash as no rawData.
    ASSERT_EQ(hashNone.toHexString(), hashFalse.toHexString());
    ASSERT_NE(hashTrue.toHexString(), hashFalse.toHexString());
}

// The shape must read rawData from the command request, not from isRawDataOperation(opCtx), so
// that query shapes built inside the setQuerySettings command still set rawData when the
// represented query has it. This pins that: with the opCtx flag left false, a request with
// rawData:true must still produce a rawData shape (and a different hash from a rawData-absent one).
TEST_F(DistinctRawDataTest, RawDataSourcedFromRequestNotOpCtx) {
    OperationContext* opCtx = expCtx->getOperationContext();
    isRawDataOperation(opCtx) = false;

    auto shapeFromRequest = makeShape("field", true);
    ASSERT_TRUE(shapeFromRequest->rawData)
        << "shape rawData must come from the request even when isRawDataOperation(opCtx) is false";

    // rawData from the request must still enter the hash despite the opCtx flag being false: the
    // rawData:true shape must differ from a rawData-absent shape. If the shape sourced rawData from
    // the (false) opCtx instead, these would collide.
    auto hashRequestTrue = shapeFromRequest->sha256Hash(opCtx, {});
    auto hashAbsent = makeShape("field", boost::none)->sha256Hash(opCtx, {});
    ASSERT_NE(hashRequestTrue.toHexString(), hashAbsent.toHexString());
}

TEST_F(DistinctShapeSizeTest, SizeOfShapeComponents) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto distinct = fromjson(R"({ distinct: "testcoll", $db: "testdb", key: "name" })");

    auto distinctCommand = std::make_unique<DistinctCommandRequest>(DistinctCommandRequest::parse(
        distinct,
        IDLParserContext("distinctCommandRequest",
                         auth::ValidatedTenancyScope::get(expCtx->getOperationContext()),
                         boost::none,
                         SerializationContext::stateDefault())));
    auto pd = parsed_distinct_command::parse(
        expCtx, std::move(distinctCommand), ExtensionsCallbackNoop(), {});
    auto components = std::make_unique<DistinctCmdShapeComponents>(*pd, expCtx);
    const auto minimumSize = sizeof(CmdSpecificShapeComponents) + sizeof(BSONObj) +
        sizeof(std::string) + components->key.size() +
        static_cast<size_t>(components->representativeQuery.objsize());

    ASSERT_GTE(components->size(), minimumSize);
    ASSERT_LTE(components->size(), minimumSize + 8 /*padding*/);
}

}  // namespace
}  // namespace mongo::query_shape
