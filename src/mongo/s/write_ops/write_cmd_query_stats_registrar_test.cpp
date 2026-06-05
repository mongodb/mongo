/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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


#include "mongo/s/write_ops/write_cmd_query_stats_registrar.h"

#include "mongo/bson/json.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/query_stats/mock_key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include "gtest/gtest.h"

namespace mongo::query_stats {

namespace {

class WriteCmdQueryStatsRegistrarTest : public ServiceContextTest {
public:
    const ShardId shardId1 = ShardId("shard1");
    const NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("test", "coll0");
    const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("test", "coll1");
    const std::set<NamespaceString> nssIsViewfulTimeseries{nss1};

    WriteCmdQueryStatsRegistrarTest() {
        opCtxHolder = makeOperationContext();
        opCtx = opCtxHolder.get();

        // Set write cmd query stats collection to 100% (1000/1000)
        auto& limiter =
            query_stats::QueryStatsStoreManager::getWriteCmdRateLimiter(getServiceContext());
        limiter.configureSampleBased(1000, 42);
    }

    ServiceContext::UniqueOperationContext opCtxHolder;
    OperationContext* opCtx;
    RAIIServerParameterControllerForTest deleteFlag{"featureFlagQueryStatsDelete", true};
};

/**
 * Test for WriteCmdQueryStatsRegistrar::parseAndRegisterRequest
 * Parametrized based on whether the command should be registered with the query stats store
 */
class WriteCmdQueryStatsRegistrarRegisterRequestFixture : public WriteCmdQueryStatsRegistrarTest,
                                                          public testing::WithParamInterface<bool> {
};

/**
 * Registers an update batch with multiple updates
 * Note: In the multi-update case we don't compute the QueryShapeHash for the command
 */
TEST_P(WriteCmdQueryStatsRegistrarRegisterRequestFixture, ParseAndRegisterRequestMultiUpdate) {
    bool skipRegistration = GetParam();

    // Create a batch update request
    auto update = fromjson(R"({
        update: "testColl",
        updates: [
            { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: false, upsert: false },
            { q: { x: {$eq: 3} }, u: { $set: { bar: "baz" } }, multi: false, upsert: false },
            { q: { x: {$eq: 3} }, u: [ {$set: { number: 42 }} ], multi: false, upsert: false }
        ],
        "$db": "testDB"
        })"_sd);
    auto updateCommandRequest = write_ops::UpdateCommandRequest::parse(std::move(update));
    BatchedCommandRequest batchRequest(updateCommandRequest);
    WriteCommandRef cmdRef{batchRequest};

    WriteCmdQueryStatsRegistrar::parseAndRegisterRequest(opCtx, cmdRef, skipRegistration);

    const auto& opDebug = CurOp::get(opCtx)->debug();

    // queryShapeHash shouldn't be computed in the multi-update case
    EXPECT_FALSE(opDebug.getQueryShapeHash());

    // If registration wasn't requested, checks that info isn't populated
    if (skipRegistration) {
        for (size_t opIndex = 0; opIndex < 3; opIndex++) {
            EXPECT_FALSE(opDebug.hasQueryStatsInfo(opIndex));
        }
        return;
    }

    // Asserts that all the update ops are registered.
    for (size_t opIndex = 0; opIndex < 3; opIndex++) {
        ASSERT_TRUE(opDebug.hasQueryStatsInfo(opIndex));
        ASSERT_TRUE(opDebug.getQueryStatsInfo(opIndex).key);
    }

    // Selects one of the keys and checks its query stats key.
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "testDB",
                    "coll": "testColl"
                },
                "command": "update",
                "q": {
                    "x": {
                        "$eq": "?number"
                    }
                },
                "u": {
                    "$set": {
                        "bar": "?string"
                    }
                },
                "multi": false,
                "upsert": false
            },
            "ordered": true,
            "bypassDocumentValidation": false
        })",
        opDebug.getQueryStatsInfo(1).key->toBson(
            opCtx, SerializationOptions::kDebugQueryShapeSerializeOptions, {}));
}


TEST_P(WriteCmdQueryStatsRegistrarRegisterRequestFixture,
       ParseAndRegisterRequestSingleUpdateReplacement) {
    bool skipRegistration = GetParam();

    // Batch update request with one update
    auto update = fromjson(R"({
        update: "testColl",
        updates: [
            { q: { x: {$eq: 3} }, u: { foo: [ "beep", "boop" ] }, multi: false, upsert: false }
        ],
        "$db": "testDB"
        })"_sd);
    auto updateCommandRequest = write_ops::UpdateCommandRequest::parse(std::move(update));
    BatchedCommandRequest batchRequest(updateCommandRequest);
    WriteCommandRef cmdRef{batchRequest};

    WriteCmdQueryStatsRegistrar::parseAndRegisterRequest(opCtx, cmdRef, skipRegistration);

    const auto& opDebug = CurOp::get(opCtx)->debug();

    // Checks that queryShapeHash is set on opDebug
    EXPECT_TRUE(opDebug.getQueryShapeHash());

    // If registration wasn't requested, checks that info isn't populated
    if (skipRegistration) {
        EXPECT_FALSE(opDebug.hasQueryStatsInfo(0));
        return;
    }

    // Asserts that the update op is registered.
    ASSERT_TRUE(opDebug.hasQueryStatsInfo(0));
    ASSERT_TRUE(opDebug.getQueryStatsInfo(0).key);

    // Check the query stats key
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "testDB",
                    "coll": "testColl"
                },
                "command": "update",
                "q": {
                    "x": {
                        "$eq": "?number"
                    }
                },
                "u": "?object",
                "multi": false,
                "upsert": false
            },
            "ordered": true,
            "bypassDocumentValidation": false
        })",
        opDebug.getQueryStatsInfo(0).key->toBson(
            opCtx, SerializationOptions::kDebugQueryShapeSerializeOptions, {}));
}

TEST_P(WriteCmdQueryStatsRegistrarRegisterRequestFixture,
       ParseAndRegisterRequestSingleUpdateModifier) {
    bool skipRegistration = GetParam();

    // Batch update request with one update
    auto update = fromjson(R"({
        update: "testColl",
        updates: [
            { q: { x: {$eq: 3} }, u: { $push: { 'foo.bar' : 12 } }, multi: false, upsert: false }
        ],
        "$db": "testDB"
        })"_sd);
    auto updateCommandRequest = write_ops::UpdateCommandRequest::parse(std::move(update));
    BatchedCommandRequest batchRequest(updateCommandRequest);
    WriteCommandRef cmdRef{batchRequest};

    WriteCmdQueryStatsRegistrar::parseAndRegisterRequest(opCtx, cmdRef, skipRegistration);

    const auto& opDebug = CurOp::get(opCtx)->debug();

    // Checks that queryShapeHash is set on opDebug
    EXPECT_TRUE(opDebug.getQueryShapeHash());

    // If registration wasn't requested, checks that info isn't populated
    if (skipRegistration) {
        EXPECT_FALSE(opDebug.hasQueryStatsInfo(0));
        return;
    }

    // Asserts that the update op is registered.
    ASSERT_TRUE(opDebug.hasQueryStatsInfo(0));
    ASSERT_TRUE(opDebug.getQueryStatsInfo(0).key);

    // Check the query stats key
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "testDB",
                    "coll": "testColl"
                },
                "command": "update",
                "q": {
                    "x": {
                        "$eq": "?number"
                    }
                },
                "u": {
                    "$push": {
                        "foo.bar": {
                            "$each": "?array<?number>"
                        }
                    }
                },
                "multi": false,
                "upsert": false
            },
            "ordered": true,
            "bypassDocumentValidation": false
        })",
        opDebug.getQueryStatsInfo(0).key->toBson(
            opCtx, SerializationOptions::kDebugQueryShapeSerializeOptions, {}));
}

TEST_P(WriteCmdQueryStatsRegistrarRegisterRequestFixture,
       ParseAndRegisterRequestSingleUpdatePipeline) {
    bool skipRegistration = GetParam();

    // Batch update request with one update
    auto update = fromjson(R"({
        update: "testColl",
        updates: [
            { q: { x: {$eq: 3} }, u: [ { $set: { baz : 15 } } ], multi: false, upsert: false }
        ],
        "$db": "testDB"
        })"_sd);
    auto updateCommandRequest = write_ops::UpdateCommandRequest::parse(std::move(update));
    BatchedCommandRequest batchRequest(updateCommandRequest);
    WriteCommandRef cmdRef{batchRequest};

    WriteCmdQueryStatsRegistrar::parseAndRegisterRequest(opCtx, cmdRef, skipRegistration);

    const auto& opDebug = CurOp::get(opCtx)->debug();

    // Checks that queryShapeHash is set on opDebug
    EXPECT_TRUE(opDebug.getQueryShapeHash());

    // If registration wasn't requested, checks that info isn't populated
    if (skipRegistration) {
        EXPECT_FALSE(opDebug.hasQueryStatsInfo(0));
        return;
    }

    // Asserts that the update op is registered.
    ASSERT_TRUE(opDebug.hasQueryStatsInfo(0));
    ASSERT_TRUE(opDebug.getQueryStatsInfo(0).key);

    // Check the query stats key
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "testDB",
                    "coll": "testColl"
                },
                "command": "update",
                "q": {
                    "x": {
                        "$eq": "?number"
                    }
                },
                "u": [
                    {
                        "$set": {
                            "baz": "?number"
                        }
                    }
                ],
                "multi": false,
                "upsert": false
            },
            "ordered": true,
            "bypassDocumentValidation": false
        })",
        opDebug.getQueryStatsInfo(0).key->toBson(
            opCtx, SerializationOptions::kDebugQueryShapeSerializeOptions, {}));
}

TEST_P(WriteCmdQueryStatsRegistrarRegisterRequestFixture,
       RegisterRequestForShardsvrCoordinateMultiUpdateTest) {
    bool skipRegistration = GetParam();

    // Setting this to simulate the scenario that a primary shard receives a dispatched update
    // command (_shardsvrCoordinateMultiUpdate) from the router and it has to act like a router.
    opCtx->setCommandForwardedFromRouter();

    const DatabaseVersion nss1DbVersion(UUID::gen(), Timestamp(1, 0));
    const ShardEndpoint nss1Shard1(shardId1, ShardVersion::UNTRACKED(), nss1DbVersion);

    // Create a batch update request
    auto update = fromjson(R"({
        update: "testColl",
        updates: [
            { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: false, upsert: false },
            { q: { x: {$eq: 3} }, u: { $set: { bar: "baz" } }, multi: false, upsert: false, includeQueryStatsMetricsForOpIndex: 42 },
            { q: { x: {$eq: 3} }, u: [ {$set: { number: 42 }} ], multi: false, upsert: false }
        ],
        "$db": "testDB"
        })"_sd);
    auto updateCommandRequest = write_ops::UpdateCommandRequest::parse(std::move(update));
    BatchedCommandRequest batchRequest(updateCommandRequest);
    WriteCommandRef cmdRef{batchRequest};

    WriteCmdQueryStatsRegistrar::parseAndRegisterRequest(opCtx, cmdRef, skipRegistration);

    // Asserts that none of the update ops is registered.
    const auto& opDebug = CurOp::get(opCtx)->debug();
    for (size_t opIndex = 0; opIndex < 3; opIndex++) {
        ASSERT_FALSE(opDebug.hasQueryStatsInfo(opIndex));
    }

    // Asserts that the entry for index 42 is created. Since we are on a primary shard and it
    // creates QueryStatsInfo only for returning the metrics back to the mongos, the entry does not
    // have key and key hash.
    ASSERT_TRUE(opDebug.hasQueryStatsInfo(42));
    ASSERT_FALSE(opDebug.getQueryStatsInfo(42).key);
    ASSERT_FALSE(opDebug.getQueryStatsInfo(42).keyHash);
}

TEST_P(WriteCmdQueryStatsRegistrarRegisterRequestFixture, ParseAndRegisterRequestMultiDelete) {
    bool skipRegistration = GetParam();

    auto deleteCmd = fromjson(R"({
        delete: "testColl",
        deletes: [
            { q: { x: {$eq: 3} }, limit: 0 },
            { q: { y: {$gt: 10} }, limit: 1 }
        ],
        "$db": "testDB"
    })"_sd);
    auto deleteCommandRequest = write_ops::DeleteCommandRequest::parse(std::move(deleteCmd));
    BatchedCommandRequest batchRequest(deleteCommandRequest);
    WriteCommandRef cmdRef{batchRequest};

    WriteCmdQueryStatsRegistrar::parseAndRegisterRequest(opCtx, cmdRef, skipRegistration);

    const auto& opDebug = CurOp::get(opCtx)->debug();

    // Multi-delete batch: no queryShapeHash on CurOp.
    EXPECT_FALSE(opDebug.getQueryShapeHash());

    if (skipRegistration) {
        for (size_t opIndex = 0; opIndex < 2; opIndex++) {
            EXPECT_FALSE(opDebug.hasQueryStatsInfo(opIndex));
        }
        return;
    }

    for (size_t opIndex = 0; opIndex < 2; opIndex++) {
        ASSERT_TRUE(opDebug.hasQueryStatsInfo(opIndex));
        ASSERT_TRUE(opDebug.getQueryStatsInfo(opIndex).key);
    }
}

TEST_P(WriteCmdQueryStatsRegistrarRegisterRequestFixture, ParseAndRegisterRequestSingleDelete) {
    bool skipRegistration = GetParam();

    auto deleteCmd = fromjson(R"({
        delete: "testColl",
        deletes: [
            { q: { x: {$eq: 3} }, limit: 0 }
        ],
        "$db": "testDB"
    })"_sd);
    auto deleteCommandRequest = write_ops::DeleteCommandRequest::parse(std::move(deleteCmd));
    BatchedCommandRequest batchRequest(deleteCommandRequest);
    WriteCommandRef cmdRef{batchRequest};

    WriteCmdQueryStatsRegistrar::parseAndRegisterRequest(opCtx, cmdRef, skipRegistration);

    const auto& opDebug = CurOp::get(opCtx)->debug();

    // Single delete: queryShapeHash should be set.
    EXPECT_TRUE(opDebug.getQueryShapeHash());

    if (skipRegistration) {
        EXPECT_FALSE(opDebug.hasQueryStatsInfo(0));
        return;
    }

    ASSERT_TRUE(opDebug.hasQueryStatsInfo(0));
    ASSERT_TRUE(opDebug.getQueryStatsInfo(0).key);

    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "queryShape": {
                "cmdNs": {
                    "db": "testDB",
                    "coll": "testColl"
                },
                "command": "delete",
                "q": {
                    "x": {
                        "$eq": "?number"
                    }
                },
                "limit": 0
            },
            "ordered": true,
            "bypassDocumentValidation": false
        })",
        opDebug.getQueryStatsInfo(0).key->toBson(
            opCtx, SerializationOptions::kDebugQueryShapeSerializeOptions, {}));
}
TEST_P(WriteCmdQueryStatsRegistrarRegisterRequestFixture,
       RegisterRequestForShardsvrCoordinateDeleteForwardedFromRouter) {
    bool skipRegistration = GetParam();

    opCtx->setCommandForwardedFromRouter();

    auto deleteCmd = fromjson(R"({
        delete: "testColl",
        deletes: [
            { q: { x: {$eq: 3} }, limit: 0 },
            { q: { y: {$gt: 5} }, limit: 1, includeQueryStatsMetricsForOpIndex: 42 },
            { q: { z: {$lt: 0} }, limit: 0 }
        ],
        "$db": "testDB"
    })"_sd);
    auto deleteCommandRequest = write_ops::DeleteCommandRequest::parse(std::move(deleteCmd));
    BatchedCommandRequest batchRequest(deleteCommandRequest);
    WriteCommandRef cmdRef{batchRequest};

    WriteCmdQueryStatsRegistrar::parseAndRegisterRequest(opCtx, cmdRef, skipRegistration);

    const auto& opDebug = CurOp::get(opCtx)->debug();
    // No ops registered when forwarded from router.
    for (size_t opIndex = 0; opIndex < 3; opIndex++) {
        ASSERT_FALSE(opDebug.hasQueryStatsInfo(opIndex));
    }
    // Entry at index 42 is created (empty, for returning metrics to mongos).
    ASSERT_TRUE(opDebug.hasQueryStatsInfo(42));
    ASSERT_FALSE(opDebug.getQueryStatsInfo(42).key);
    ASSERT_FALSE(opDebug.getQueryStatsInfo(42).keyHash);
}

INSTANTIATE_TEST_SUITE_P(RegisterRequestSuite,
                         WriteCmdQueryStatsRegistrarRegisterRequestFixture,
                         testing::Bool());

TEST_F(WriteCmdQueryStatsRegistrarTest, ParseAndRegisterRequestDeleteSkipsWhenFlagDisabled) {
    // Disable the delete feature flag for this test.
    RAIIServerParameterControllerForTest disabledFlag{"featureFlagQueryStatsDelete", false};

    auto deleteCmd = fromjson(R"({
        delete: "testColl",
        deletes: [ { q: { x: {$eq: 3} }, limit: 0 } ],
        "$db": "testDB"
    })"_sd);
    auto deleteCommandRequest = write_ops::DeleteCommandRequest::parse(std::move(deleteCmd));
    BatchedCommandRequest batchRequest(deleteCommandRequest);
    WriteCommandRef cmdRef{batchRequest};

    WriteCmdQueryStatsRegistrar::parseAndRegisterRequest(opCtx, cmdRef);

    const auto& opDebug = CurOp::get(opCtx)->debug();
    EXPECT_FALSE(opDebug.hasQueryStatsInfo(0));
}

TEST_F(WriteCmdQueryStatsRegistrarTest, ParseAndRegisterRequestDeleteSkipsWhenEncrypted) {
    auto deleteCmd = fromjson(R"({
        delete: "testColl",
        deletes: [ { q: { x: {$eq: 3} }, limit: 0 } ],
        encryptionInformation: { type: 1, schema: {} },
        "$db": "testDB"
    })"_sd);
    auto deleteCommandRequest = write_ops::DeleteCommandRequest::parse(std::move(deleteCmd));
    BatchedCommandRequest batchRequest(deleteCommandRequest);
    WriteCommandRef cmdRef{batchRequest};

    WriteCmdQueryStatsRegistrar::parseAndRegisterRequest(opCtx, cmdRef);

    const auto& opDebug = CurOp::get(opCtx)->debug();
    EXPECT_FALSE(opDebug.hasQueryStatsInfo(0));
}

TEST_F(WriteCmdQueryStatsRegistrarRegisterRequestFixture,
       SetIncludeQueryStatsMetricsForUpdateIfRequestedTest) {
    WriteCmdQueryStatsRegistrar queryStatsRegistrar;

    auto registerMockKey = [&](OpDebug& opDebug, size_t opIndex) {
        OpDebug::QueryStatsInfo qsi;
        qsi.key = std::make_unique<query_stats::MockKey>(opCtx);
        qsi.keyHash = 42;
        opDebug.setQueryStatsInfoAtOpIndex(opIndex, std::move(qsi));
    };

    // MongoS ignores and resets the op index field passed from client. If we decide not to record
    // this op (as we do not register a query stats key for it), we assert that the op index field
    // is cleared before sending to MongoD shards.
    {
        write_ops::UpdateOpEntry updateOpEntry;
        updateOpEntry.setIncludeQueryStatsMetricsForOpIndex(42);
        queryStatsRegistrar
            .setIncludeQueryStatsMetricsForOpIndexIfRequested<write_ops::UpdateOpEntry>(
                opCtx, 0, updateOpEntry);

        ASSERT_FALSE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
    }

    // Similar to the above case, MongoS ignores and overwrites the op index passed from client if
    // we decide to record this op. Asserts that the op index field is based on the position (1)
    // rather than the op index passed from the client.
    {
        // Registering a mock query stats key to indicate that we decide to record this update op.
        registerMockKey(CurOp::get(opCtx)->debug(), 1);

        write_ops::UpdateOpEntry updateOpEntry;
        updateOpEntry.setIncludeQueryStatsMetricsForOpIndex(42);
        queryStatsRegistrar
            .setIncludeQueryStatsMetricsForOpIndexIfRequested<write_ops::UpdateOpEntry>(
                opCtx, 1, updateOpEntry);

        ASSERT_TRUE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        ASSERT_EQ(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex(), 1);
    }

    // Asserts that we are allowed to request metrics for update ops up to
    // kMaxBatchOpsMetricsRequested.
    for (size_t opIndex = 2;
         opIndex < WriteCmdQueryStatsRegistrar::kMaxBatchOpsMetricsRequested + 1;
         opIndex++) {
        registerMockKey(CurOp::get(opCtx)->debug(), opIndex);

        write_ops::UpdateOpEntry updateOpEntry;
        ASSERT_FALSE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        queryStatsRegistrar
            .setIncludeQueryStatsMetricsForOpIndexIfRequested<write_ops::UpdateOpEntry>(
                opCtx, opIndex, updateOpEntry);

        // Asserts that the field includeQueryStatsMetricsForOpIndex has been set with 'opIndex'.
        // When a shard server receives this 'updateOpEntry', it will append cursor metrics in
        // response.
        ASSERT_TRUE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        ASSERT_EQ(*updateOpEntry.getIncludeQueryStatsMetricsForOpIndex(), opIndex);
    }

    // Asserts that after reaching the limit, we no longer request for metrics so as to prevent the
    // response growing beyond the allowed object size limit.
    for (size_t opIndex = WriteCmdQueryStatsRegistrar::kMaxBatchOpsMetricsRequested + 1;
         opIndex < WriteCmdQueryStatsRegistrar::kMaxBatchOpsMetricsRequested + 10;
         opIndex++) {
        registerMockKey(CurOp::get(opCtx)->debug(), opIndex);

        write_ops::UpdateOpEntry updateOpEntry;
        ASSERT_FALSE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        queryStatsRegistrar
            .setIncludeQueryStatsMetricsForOpIndexIfRequested<write_ops::UpdateOpEntry>(
                opCtx, opIndex, updateOpEntry);
        ASSERT_FALSE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
    }
}

TEST_F(WriteCmdQueryStatsRegistrarRegisterRequestFixture,
       SetIncludeQueryStatsMetricsIfRequestedForInsertTest) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");

    auto makeInsertRequest = [&]() {
        write_ops::InsertCommandRequest insertReq(nss);
        insertReq.setDocuments({BSON("_id" << 1)});
        return insertReq;
    };

    auto registerMockKey = [&](OperationContext* ctx) {
        OpDebug::QueryStatsInfo qsi;
        qsi.key = std::make_unique<query_stats::MockKey>(ctx);
        qsi.keyHash = 42;
        CurOp::get(ctx)->debug().setQueryStatsInfoAtOpIndex(0 /* kInsertOpIndex */, std::move(qsi));
    };

    // Runs 'fn' with a fresh opCtx on a dedicated client. AlternativeClientRegion stays alive
    // for the duration of the call, ensuring the client swap is active throughout 'fn'.
    auto withFreshOpCtx = [&](std::string clientName, auto fn) {
        auto client = getServiceContext()->getService()->makeClient(clientName);
        AlternativeClientRegion acr(client);
        auto opCtxHolder = cc().makeOperationContext();
        fn(opCtxHolder.get());
    };

    // No query stats entry registered: the flag is explicitly cleared, even if the client pre-set
    // it.
    withFreshOpCtx("no-key-client", [&](OperationContext* localOpCtx) {
        WriteCmdQueryStatsRegistrar queryStatsRegistrar;
        auto insertReq = makeInsertRequest();
        insertReq.setIncludeQueryStatsMetrics(true);
        queryStatsRegistrar.setIncludeQueryStatsMetricsIfRequested(localOpCtx, insertReq);
        ASSERT_FALSE(insertReq.getIncludeQueryStatsMetrics());
    });

    // Query stats entry registered: the flag is set to true.
    withFreshOpCtx("key-registered-client", [&](OperationContext* localOpCtx) {
        registerMockKey(localOpCtx);
        WriteCmdQueryStatsRegistrar queryStatsRegistrar;
        auto insertReq = makeInsertRequest();
        ASSERT_FALSE(insertReq.getIncludeQueryStatsMetrics());
        queryStatsRegistrar.setIncludeQueryStatsMetricsIfRequested(localOpCtx, insertReq);
        ASSERT_TRUE(insertReq.getIncludeQueryStatsMetrics());
    });

    // Command forwarded from router: function returns early without modifying the request.
    withFreshOpCtx("forwarded-from-router-client", [&](OperationContext* localOpCtx) {
        localOpCtx->setCommandForwardedFromRouter();
        registerMockKey(localOpCtx);
        WriteCmdQueryStatsRegistrar queryStatsRegistrar;
        auto insertReq = makeInsertRequest();
        insertReq.setIncludeQueryStatsMetrics(true);
        queryStatsRegistrar.setIncludeQueryStatsMetricsIfRequested(localOpCtx, insertReq);
        // Pre-set value is left untouched since the function returns early.
        ASSERT_TRUE(insertReq.getIncludeQueryStatsMetrics());
    });
}

TEST_F(WriteCmdQueryStatsRegistrarRegisterRequestFixture,
       PassthroughMetricsOpIndexForCoordinateMultiUpdateTest) {
    // Setting this to simulate the scenario that a primary shard receives a dispatched update
    // command (_shardsvrCoordinateMultiUpdate) from the router and it has to act like a router.
    opCtx->setCommandForwardedFromRouter();

    WriteCmdQueryStatsRegistrar queryStatsRegistrar;

    auto registerMockKey = [&](OpDebug& opDebug, size_t opIndex) {
        OpDebug::QueryStatsInfo qsi;
        qsi.key = std::make_unique<query_stats::MockKey>(opCtx);
        qsi.keyHash = 42;
        opDebug.setQueryStatsInfoAtOpIndex(opIndex, std::move(qsi));
    };


    // Primary shard simply lets the provided opIndex to pass through.
    {
        registerMockKey(CurOp::get(opCtx)->debug(), 100);

        write_ops::UpdateOpEntry updateOpEntry;
        updateOpEntry.setIncludeQueryStatsMetricsForOpIndex(42);
        ASSERT_TRUE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        queryStatsRegistrar
            .setIncludeQueryStatsMetricsForOpIndexIfRequested<write_ops::UpdateOpEntry>(
                opCtx, 100, updateOpEntry);

        // Asserts that the index in 'updateOpEntry' remain the same (42) and not be overwritten by
        // provided opIndex (100).
        ASSERT_TRUE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        ASSERT_EQ(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex(), 42);
    }

    // Primary shard simply lets the provided opIndex to pass through even when QueryStatsInfo is
    // empty.
    {
        write_ops::UpdateOpEntry updateOpEntry;
        updateOpEntry.setIncludeQueryStatsMetricsForOpIndex(42);
        ASSERT_TRUE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        queryStatsRegistrar
            .setIncludeQueryStatsMetricsForOpIndexIfRequested<write_ops::UpdateOpEntry>(
                opCtx, 200, updateOpEntry);

        // Asserts that the index in 'updateOpEntry' remain the same and not be unset by the empty
        // QueryStatsInfo.
        ASSERT_TRUE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        ASSERT_EQ(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex(), 42);
    }
}

TEST_F(WriteCmdQueryStatsRegistrarRegisterRequestFixture,
       PassthroughMetricsOpIndexForCoordinateMultiDeleteTest) {
    // Setting this to simulate the scenario that a primary shard receives a dispatched delete
    // command (_shardsvrCoordinateMultiUpdate) from the router and it has to act like a router.
    opCtx->setCommandForwardedFromRouter();

    WriteCmdQueryStatsRegistrar queryStatsRegistrar;

    auto registerMockKey = [&](OpDebug& opDebug, size_t opIndex) {
        OpDebug::QueryStatsInfo qsi;
        qsi.key = std::make_unique<query_stats::MockKey>(opCtx);
        qsi.keyHash = 42;
        opDebug.setQueryStatsInfoAtOpIndex(opIndex, std::move(qsi));
    };

    // Primary shard simply lets the provided opIndex pass through.
    {
        registerMockKey(CurOp::get(opCtx)->debug(), 100);

        write_ops::DeleteOpEntry deleteOpEntry;
        deleteOpEntry.setIncludeQueryStatsMetricsForOpIndex(42);
        ASSERT_TRUE(deleteOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        queryStatsRegistrar
            .setIncludeQueryStatsMetricsForOpIndexIfRequested<write_ops::DeleteOpEntry>(
                opCtx, 100, deleteOpEntry);

        // The index in 'deleteOpEntry' should remain 42 and not be overwritten by opIndex (100).
        ASSERT_TRUE(deleteOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        ASSERT_EQ(deleteOpEntry.getIncludeQueryStatsMetricsForOpIndex(), 42);
    }

    // Primary shard lets the provided opIndex pass through even when QueryStatsInfo is empty.
    {
        write_ops::DeleteOpEntry deleteOpEntry;
        deleteOpEntry.setIncludeQueryStatsMetricsForOpIndex(42);
        ASSERT_TRUE(deleteOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        queryStatsRegistrar
            .setIncludeQueryStatsMetricsForOpIndexIfRequested<write_ops::DeleteOpEntry>(
                opCtx, 200, deleteOpEntry);

        // The index should remain 42 and not be unset by the empty QueryStatsInfo.
        ASSERT_TRUE(deleteOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        ASSERT_EQ(deleteOpEntry.getIncludeQueryStatsMetricsForOpIndex(), 42);
    }
}

/**
 * Show that we don't register writes for query stats collection when the write sample rate is zero,
 * even if the read path is configured for query stats collection.
 */
TEST_F(WriteCmdQueryStatsRegistrarRegisterRequestFixture,
       RegisterRequestNotSampledWhenWriteRateIsZero) {
    auto& writeLimiter =
        query_stats::QueryStatsStoreManager::getWriteCmdRateLimiter(getServiceContext());
    writeLimiter.configureSampleBased(0, 42);

    auto& readLimiter = query_stats::QueryStatsStoreManager::getRateLimiter(getServiceContext());

    auto runTest = [&](auto configureReadLimiter) {
        auto update = fromjson(R"({
            update: "testColl",
            updates: [
                { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: false, upsert: false }
            ],
            "$db": "testDB"
            })"_sd);
        auto updateCommandRequest = write_ops::UpdateCommandRequest::parse(std::move(update));
        BatchedCommandRequest batchRequest(updateCommandRequest);
        WriteCommandRef cmdRef{batchRequest};

        configureReadLimiter();

        WriteCmdQueryStatsRegistrar::parseAndRegisterRequest(
            opCtx, cmdRef, false /* skip request */);

        const auto& opDebug = CurOp::get(opCtx)->debug();
        ASSERT_FALSE(opDebug.hasQueryStatsInfo(0));
    };

    runTest([&] { readLimiter.configureSampleBased(1000, 42); });
    runTest([&] { readLimiter.configureWindowBased(1000); });
}

TEST_F(WriteCmdQueryStatsRegistrarTest, SetIncludeQueryStatsMetricsForOpIndexIfRequestedDelete) {
    WriteCmdQueryStatsRegistrar queryStatsRegistrar;

    auto registerMockKey = [&](OpDebug& opDebug, size_t opIndex) {
        OpDebug::QueryStatsInfo qsi;
        qsi.key = std::make_unique<query_stats::MockKey>(opCtx);
        qsi.keyHash = 42;
        opDebug.setQueryStatsInfoAtOpIndex(opIndex, std::move(qsi));
    };

    // When no query stats key is registered for the op, the field is explicitly unset.
    {
        write_ops::DeleteOpEntry deleteOpEntry;
        deleteOpEntry.setIncludeQueryStatsMetricsForOpIndex(42);
        queryStatsRegistrar
            .setIncludeQueryStatsMetricsForOpIndexIfRequested<write_ops::DeleteOpEntry>(
                opCtx, 0, deleteOpEntry);

        ASSERT_FALSE(deleteOpEntry.getIncludeQueryStatsMetricsForOpIndex());
    }

    // When a query stats key is registered, the field is set to the actual op index.
    {
        registerMockKey(CurOp::get(opCtx)->debug(), 1);

        write_ops::DeleteOpEntry deleteOpEntry;
        deleteOpEntry.setIncludeQueryStatsMetricsForOpIndex(42);
        queryStatsRegistrar
            .setIncludeQueryStatsMetricsForOpIndexIfRequested<write_ops::DeleteOpEntry>(
                opCtx, 1, deleteOpEntry);

        ASSERT_TRUE(deleteOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        ASSERT_EQ(*deleteOpEntry.getIncludeQueryStatsMetricsForOpIndex(), 1);
    }

    // Asserts that we are allowed to request metrics for delete ops up to
    // kMaxBatchOpsMetricsRequested.
    for (size_t opIndex = 2;
         opIndex < WriteCmdQueryStatsRegistrar::kMaxBatchOpsMetricsRequested + 1;
         opIndex++) {
        registerMockKey(CurOp::get(opCtx)->debug(), opIndex);

        write_ops::DeleteOpEntry deleteOpEntry;
        queryStatsRegistrar
            .setIncludeQueryStatsMetricsForOpIndexIfRequested<write_ops::DeleteOpEntry>(
                opCtx, opIndex, deleteOpEntry);
        ASSERT_TRUE(deleteOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        ASSERT_EQ(*deleteOpEntry.getIncludeQueryStatsMetricsForOpIndex(), opIndex);
    }

    // Asserts that after reaching the limit, we no longer request for metrics so as to prevent the
    // response growing beyond the allowed object size limit.
    for (size_t opIndex = WriteCmdQueryStatsRegistrar::kMaxBatchOpsMetricsRequested + 1;
         opIndex < WriteCmdQueryStatsRegistrar::kMaxBatchOpsMetricsRequested + 10;
         opIndex++) {
        registerMockKey(CurOp::get(opCtx)->debug(), opIndex);

        write_ops::DeleteOpEntry deleteOpEntry;
        queryStatsRegistrar
            .setIncludeQueryStatsMetricsForOpIndexIfRequested<write_ops::DeleteOpEntry>(
                opCtx, opIndex, deleteOpEntry);
        ASSERT_FALSE(deleteOpEntry.getIncludeQueryStatsMetricsForOpIndex());
    }
}

}  // namespace
}  // namespace mongo::query_stats
