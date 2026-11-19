// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_shape/write_cmd_shape_test_util.h"
#include "mongo/db/query/query_stats/write_key.h"
#include "mongo/db/query/query_stats/write_key_test_util.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/ensure_fcv.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/tassert_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/version/releases.h"

#include <string_view>

namespace mongo::query_stats {
namespace {

using namespace std::literals::string_view_literals;

static constexpr auto kCollType = query_shape::CollectionType::kCollection;

BSONObj appendComponents(const BulkWriteCmdComponents& components,
                         const query_shape::SerializationOptions& opts) {
    BSONObjBuilder bob;
    components.appendTo(bob, opts);
    return bob.obj();
}

BulkWriteCommandRequest parseBulkWrite(std::string_view cmd) {
    return BulkWriteCommandRequest::parseOwned(fromjson(cmd));
}

class BulkWriteKeyTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _opCtx = makeOperationContext();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    std::unique_ptr<BulkWriteKey> makeBulkWriteTestKey(
        const BulkWriteCommandRequest& request,
        size_t opIdx = 0,
        query_shape::CollectionType collType = kCollType) {
        return makeBulkWriteKey(opCtx(), request, opIdx, collType);
    }

    std::unique_ptr<BulkWriteKey> makeBulkWriteTestKey(std::string_view cmd, size_t opIdx = 0) {
        return makeBulkWriteTestKey(parseBulkWrite(cmd), opIdx);
    }

    BSONObj toDebugBson(const Key& key) {
        return key.toBson(opCtx(),
                          query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions,
                          SerializationContext::stateDefault());
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

constexpr auto kMixedOpsCmd = R"({
    bulkWrite: 1,
    ops: [
        { insert: 0, document: { a: 1 } },
        { update: 0, filter: { x: 3 }, updateMods: { $set: { y: 1 } } },
        { delete: 0, filter: { x: 3 }, multi: false }
    ],
    nsInfo: [ { ns: "testDB.testColl" } ],
    "$db": "admin"
})"sv;

TEST_F(BulkWriteKeyTest, ComponentsAreNotAbstracted) {
    constexpr auto explicitCmd = R"({
        bulkWrite: 1,
        ops: [ { insert: 0, document: { a: 1 } } ],
        nsInfo: [ { ns: "testDB.testColl" } ],
        ordered: false,
        bypassDocumentValidation: true,
        errorsOnly: true,
        "$db": "admin"
    })"sv;
    BulkWriteCmdComponents defaults(parseBulkWrite(kMixedOpsCmd));
    BulkWriteCmdComponents explicitValues(parseBulkWrite(explicitCmd));

    ASSERT_BSONOBJ_EQ(
        fromjson(R"({ ordered: true, bypassDocumentValidation: false, errorsOnly: false })"),
        appendComponents(defaults,
                         query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions));
    ASSERT_BSONOBJ_EQ(
        fromjson(R"({ ordered: false, bypassDocumentValidation: true, errorsOnly: true })"),
        appendComponents(explicitValues,
                         query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions));
    ASSERT_NE(absl::HashOf(defaults), absl::HashOf(explicitValues));
    ASSERT_NE(absl::HashOf(*makeBulkWriteTestKey(explicitCmd)),
              absl::HashOf(*makeBulkWriteTestKey(kMixedOpsCmd)));
}

TEST_F(BulkWriteKeyTest, CursorBatchSizeAbstracted) {
    auto makeCmd = [](std::string_view cursor) {
        return std::string{R"({
            bulkWrite: 1,
            ops: [ { insert: 0, document: { a: 1 } } ],
            nsInfo: [ { ns: "testDB.testColl" } ],)"} +
            std::string{cursor} + R"( "$db": "admin" })";
    };
    BulkWriteCmdComponents batchSize5(parseBulkWrite(makeCmd("cursor: { batchSize: 5 },")));
    BulkWriteCmdComponents batchSize500(parseBulkWrite(makeCmd("cursor: { batchSize: 500 },")));
    // An explicit batchSize of 0 (an empty first batch) is present, unlike an absent batchSize.
    BulkWriteCmdComponents batchSize0(parseBulkWrite(makeCmd("cursor: { batchSize: 0 },")));
    BulkWriteCmdComponents emptyCursor(parseBulkWrite(makeCmd("cursor: {},")));
    BulkWriteCmdComponents noCursor(parseBulkWrite(makeCmd("")));

    ASSERT_BSONOBJ_EQ(
        fromjson(R"({ ordered: true, bypassDocumentValidation: false, errorsOnly: false,
                      cursor: { batchSize: "?number" } })"),
        appendComponents(batchSize5,
                         query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions));
    ASSERT_BSONOBJ_EQ(
        fromjson(R"({ ordered: true, bypassDocumentValidation: false, errorsOnly: false,
                      cursor: { batchSize: 1 } })"),
        appendComponents(
            batchSize5,
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions));
    ASSERT_BSONOBJ_EQ(
        fromjson(R"({ ordered: true, bypassDocumentValidation: false, errorsOnly: false })"),
        appendComponents(emptyCursor,
                         query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions));

    ASSERT_BSONOBJ_EQ(
        fromjson(R"({ ordered: true, bypassDocumentValidation: false, errorsOnly: false,
                      cursor: { batchSize: "?number" } })"),
        appendComponents(batchSize0,
                         query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions));

    ASSERT_EQ(absl::HashOf(batchSize5), absl::HashOf(batchSize500));
    ASSERT_EQ(absl::HashOf(batchSize5), absl::HashOf(batchSize0));
    ASSERT_EQ(absl::HashOf(emptyCursor), absl::HashOf(noCursor));
    ASSERT_NE(absl::HashOf(batchSize5), absl::HashOf(noCursor));
}

TEST_F(BulkWriteKeyTest, PerOpHintAndCommandReadConcern) {
    auto request = parseBulkWrite(R"({
        bulkWrite: 1,
        ops: [
            { insert: 0, document: { a: 1 } },
            { update: 0, filter: { x: 3 }, updateMods: { $set: { y: 1 } }, hint: { x: 1 } },
            { delete: 0, filter: { x: 3 }, multi: false, hint: "x_1" },
            { update: 0, filter: { x: 3 }, updateMods: { $set: { y: 1 } } }
        ],
        nsInfo: [ { ns: "testDB.testColl" } ],
        readConcern: { level: "majority" },
        maxTimeMS: 500,
        "$db": "admin"
    })"sv);
    auto insertBson = toDebugBson(*makeBulkWriteTestKey(request, 0));
    ASSERT_FALSE(insertBson.hasField("hint")) << insertBson;
    ASSERT_FALSE(insertBson.hasField("readConcern")) << insertBson;
    ASSERT_EQ(insertBson["maxTimeMS"].str(), "?number") << insertBson;

    ASSERT_BSONOBJ_EQ(fromjson("{x: 1}"),
                      toDebugBson(*makeBulkWriteTestKey(request, 1))["hint"].Obj());
    ASSERT_BSONOBJ_EQ(fromjson("{$hint: 'x_1'}"),
                      toDebugBson(*makeBulkWriteTestKey(request, 2))["hint"].Obj());
    ASSERT_FALSE(toDebugBson(*makeBulkWriteTestKey(request, 3)).hasField("hint"));
    ASSERT_NE(absl::HashOf(*makeBulkWriteTestKey(request, 1)),
              absl::HashOf(*makeBulkWriteTestKey(request, 3)));

    for (size_t opIdx : {1, 2, 3}) {
        auto keyBson = toDebugBson(*makeBulkWriteTestKey(request, opIdx));
        ASSERT_BSONOBJ_EQ(fromjson("{level: 'majority'}"), keyBson["readConcern"].Obj());
        ASSERT_EQ(keyBson["maxTimeMS"].str(), "?number") << keyBson;
    }
}

// Insert ops drop readConcern, so the same insert op gets the same key whatever the readConcern.
TEST_F(BulkWriteKeyTest, InsertOpKeyIgnoresReadConcern) {
    auto withReadConcern = makeBulkWriteTestKey(R"({
        bulkWrite: 1,
        ops: [ { insert: 0, document: { a: 1 } } ],
        nsInfo: [ { ns: "testDB.testColl" } ],
        readConcern: { level: "majority" },
        "$db": "admin"
    })"sv);
    auto withoutReadConcern = makeBulkWriteTestKey(R"({
        bulkWrite: 1,
        ops: [ { insert: 0, document: { a: 1 } } ],
        nsInfo: [ { ns: "testDB.testColl" } ],
        "$db": "admin"
    })"sv);
    ASSERT_BSONOBJ_EQ(toDebugBson(*withoutReadConcern), toDebugBson(*withReadConcern));
    ASSERT_EQ(absl::HashOf(*withoutReadConcern), absl::HashOf(*withReadConcern));
}

TEST_F(BulkWriteKeyTest, OpIndexOutOfRange) {
    auto request = parseBulkWrite(kMixedOpsCmd);
    ASSERT_TASSERT_CODE(
        BulkWriteKey(
            opCtx(), request, 3, query_shape::makeBulkWriteShape(opCtx(), request, 0), kCollType),
        13743401);
}

TEST_F(BulkWriteKeyTest, FullKeyToBson) {
    auto bulkKey = makeBulkWriteTestKey(R"({
        bulkWrite: 1,
        ops: [ { update: 0, filter: { x: 3 }, updateMods: { $set: { y: 1 } }, hint: { x: 1 } } ],
        nsInfo: [ { ns: "testDB.testColl" } ],
        cursor: { batchSize: 10 },
        "$db": "admin"
    })"sv);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "testDB",
                    "coll": "testColl"
                },
                "command": "bulkWrite",
                "op": "update",
                "filter": {
                    "x": {
                        "$eq": "?number"
                    }
                },
                "updateMods": {
                    "$set": {
                        "y": "?number"
                    }
                },
                "multi": false,
                "upsert": false
            },
            "collectionType": "collection",
            "hint": {
                "x": 1
            },
            "ordered": true,
            "bypassDocumentValidation": false,
            "errorsOnly": false,
            "cursor": {
                "batchSize": "?number"
            }
        })",
        toDebugBson(*bulkKey));

    auto standaloneKey = makeUpdateKey(opCtx(), R"({
        update: "testColl",
        updates: [ { q: { x: 3 }, u: { $set: { y: 1 } }, hint: { x: 1 } } ],
        "$db": "testDB"
    })"sv);
    ASSERT_BSONOBJ_NE(toDebugBson(*standaloneKey)["queryShape"].Obj(),
                      toDebugBson(*bulkKey)["queryShape"].Obj());
}

// The bulkWrite op shape is distinguished from the standalone write command shape by its 'command'
// ("bulkWrite") and 'op' fields, so both the shape hash and the key hash differ.
TEST_F(BulkWriteKeyTest, QueryShapeHashAndKeyHashDifferFromStandalone) {
    auto request = parseBulkWrite(kMixedOpsCmd);
    std::vector<std::unique_ptr<Key>> standaloneKeys;
    standaloneKeys.push_back(makeInsertKey(opCtx(), R"({
        insert: "testColl", documents: [ { a: 1 } ], "$db": "testDB"
    })"sv));
    standaloneKeys.push_back(makeUpdateKey(opCtx(), R"({
        update: "testColl", updates: [ { q: { x: 3 }, u: { $set: { y: 1 } } } ], "$db": "testDB"
    })"sv));
    standaloneKeys.push_back(makeDeleteKey(opCtx(), R"({
        delete: "testColl", deletes: [ { q: { x: 3 }, limit: 1 } ], "$db": "testDB"
    })"sv));

    for (size_t opIdx = 0; opIdx < request.getOps().size(); ++opIdx) {
        auto bulkKey = makeBulkWriteTestKey(request, opIdx);
        const auto& standaloneKey = *standaloneKeys[opIdx];
        ASSERT_NE(standaloneKey.getQueryShapeHash(opCtx(), SerializationContext::stateDefault()),
                  bulkKey->getQueryShapeHash(opCtx(), SerializationContext::stateDefault()));
        ASSERT_NE(absl::HashOf(standaloneKey), absl::HashOf(*bulkKey));
    }
}

TEST_F(BulkWriteKeyTest, Size) {
    BulkWriteCmdComponents components(parseBulkWrite(kMixedOpsCmd));
    ASSERT_EQ(components.size(), sizeof(BulkWriteCmdComponents));

    auto withCursor = makeBulkWriteTestKey(R"({
        bulkWrite: 1,
        ops: [ { insert: 0, document: { a: 1 } } ],
        nsInfo: [ { ns: "testDB.testColl" } ],
        cursor: { batchSize: 1000 },
        errorsOnly: true,
        ordered: false,
        "$db": "admin"
    })"sv);
    auto withoutCursor = makeBulkWriteTestKey(R"({
        bulkWrite: 1,
        ops: [ { insert: 0, document: { b: 1 } } ],
        nsInfo: [ { ns: "testDB.testColl" } ],
        "$db": "admin"
    })"sv);
    ASSERT_EQ(withCursor->size(), withoutCursor->size());

    auto keyWithoutComment = makeBulkWriteTestKey(kMixedOpsCmd, 1);
    opCtx()->setComment(BSON("comment" << "foo"));
    auto keyWithComment = makeBulkWriteTestKey(kMixedOpsCmd, 1);
    ASSERT_LT(keyWithoutComment->size(), keyWithComment->size());
}

class BulkWriteGateTest : public BulkWriteKeyTest {
public:
    bool shouldCompute(const BulkWriteCommandRequest& request,
                       size_t opIdx = 0,
                       query_shape::CollectionType collType = kCollType) {
        return shouldComputeBulkWriteQueryShape(opCtx(), request, opIdx, collType);
    }

    // (Generic FCV reference): The feature flag check requires an initialized FCV.
    unittest::EnsureFCV _fcv{multiversion::GenericFCV::kLatest};
};

TEST_F(BulkWriteGateTest, UnsupportedNamespaces) {
    unittest::ServerParameterGuard flag("featureFlagQueryStatsAdditionalWriteCommands", true);
    auto request = parseBulkWrite(R"({
        bulkWrite: 1,
        ops: [
            { insert: 0, document: { a: 1 } },
            { insert: 1, document: { a: 1 } },
            { insert: 2, document: { a: 1 } }
        ],
        nsInfo: [
            { ns: "testDB.plain" },
            { ns: "testDB.encrypted" },
            { ns: "testDB.ts", isTimeseriesNamespace: true }
        ],
        "$db": "admin"
    })"sv);
    auto nsInfo = request.getNsInfo();
    nsInfo[1].setEncryptionInformation(EncryptionInformation(fromjson("{schema: {}}")));
    request.setNsInfo(nsInfo);
    ASSERT_TRUE(shouldCompute(request, 0));
    ASSERT_FALSE(shouldCompute(request, 1));
    ASSERT_FALSE(shouldCompute(request, 2));
    ASSERT_FALSE(shouldCompute(request, 0, query_shape::CollectionType::kTimeseries));
}

TEST_F(BulkWriteGateTest, DeltaUpdate) {
    unittest::ServerParameterGuard flag("featureFlagQueryStatsAdditionalWriteCommands", true);
    auto request = parseBulkWrite(kMixedOpsCmd);
    auto ops = request.getOps();
    get<BulkWriteUpdateOp>(ops[1]).setUpdateMods(
        write_ops::UpdateModification(fromjson("{i: {y: 2}}"),
                                      write_ops::UpdateModification::DeltaTag{},
                                      write_ops::UpdateModification::DiffOptions{}));
    request.setOps(ops);
    ASSERT_TRUE(shouldCompute(request, 0));
    ASSERT_FALSE(shouldCompute(request, 1));
    ASSERT_TRUE(shouldCompute(request, 2));
}

}  // namespace
}  // namespace mongo::query_stats
