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


#include "mongo/s/write_ops/unified_write_executor/write_batch_query_stats_registrar.h"

#include "mongo/bson/json.h"
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

namespace mongo::unified_write_executor {

namespace {

class WriteBatchQueryStatsRegistrarTest : public ServiceContextTest {
public:
    const ShardId shardId1 = ShardId("shard1");
    const NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("test", "coll0");
    const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("test", "coll1");
    const std::set<NamespaceString> nssIsViewfulTimeseries{nss1};

    WriteBatchQueryStatsRegistrarTest() {
        opCtxHolder = makeOperationContext();
        opCtx = opCtxHolder.get();

        // Set write cmd query stats collection to 100% (1000/1000)
        auto& limiter =
            query_stats::QueryStatsStoreManager::getWriteCmdRateLimiter(getServiceContext());
        limiter.configureSampleBased(1000, 42);
    }

    ServiceContext::UniqueOperationContext opCtxHolder;
    OperationContext* opCtx;
    // Enable collection of query stats for updates
    RAIIServerParameterControllerForTest controller{"featureFlagQueryStatsUpdateCommand", true};
};

/**
 * Test for WriteBatchQueryStatsRegistrar::parseAndRegisterRequest
 * Parametrized based on whether the command should be registered with the query stats store
 */
class WriteBatchQueryStatsRegistrarRegisterRequestFixture
    : public WriteBatchQueryStatsRegistrarTest,
      public testing::WithParamInterface<bool> {};

/**
 * Registers an update batch with multiple updates
 * Note: In the multi-update case we don't compute the QueryShapeHash for the command
 */
TEST_P(WriteBatchQueryStatsRegistrarRegisterRequestFixture, ParseAndRegisterRequestMultiUpdate) {
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

    WriteBatchQueryStatsRegistrar::parseAndRegisterRequest(opCtx, cmdRef, skipRegistration);

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


TEST_P(WriteBatchQueryStatsRegistrarRegisterRequestFixture,
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

    WriteBatchQueryStatsRegistrar::parseAndRegisterRequest(opCtx, cmdRef, skipRegistration);

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

TEST_P(WriteBatchQueryStatsRegistrarRegisterRequestFixture,
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

    WriteBatchQueryStatsRegistrar::parseAndRegisterRequest(opCtx, cmdRef, skipRegistration);

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

TEST_P(WriteBatchQueryStatsRegistrarRegisterRequestFixture,
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

    WriteBatchQueryStatsRegistrar::parseAndRegisterRequest(opCtx, cmdRef, skipRegistration);

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

TEST_P(WriteBatchQueryStatsRegistrarRegisterRequestFixture,
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

    WriteBatchQueryStatsRegistrar::parseAndRegisterRequest(opCtx, cmdRef, skipRegistration);

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

INSTANTIATE_TEST_SUITE_P(RegisterRequestSuite,
                         WriteBatchQueryStatsRegistrarRegisterRequestFixture,
                         testing::Bool());

TEST_F(WriteBatchQueryStatsRegistrarTest, SetIncludeQueryStatsMetricsIfRequestedTest) {
    WriteBatchQueryStatsRegistrar queryStatsRegistrar;

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
        queryStatsRegistrar.setIncludeQueryStatsMetricsIfRequested(opCtx, 0, updateOpEntry);

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
        queryStatsRegistrar.setIncludeQueryStatsMetricsIfRequested(opCtx, 1, updateOpEntry);

        ASSERT_TRUE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        ASSERT_EQ(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex(), 1);
    }

    // Asserts that we are allowed to request metrics for update ops up to
    // kMaxBatchOpsMetricsRequested.
    for (size_t opIndex = 2;
         opIndex < WriteBatchQueryStatsRegistrar::kMaxBatchOpsMetricsRequested + 1;
         opIndex++) {
        registerMockKey(CurOp::get(opCtx)->debug(), opIndex);

        write_ops::UpdateOpEntry updateOpEntry;
        ASSERT_FALSE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        queryStatsRegistrar.setIncludeQueryStatsMetricsIfRequested(opCtx, opIndex, updateOpEntry);

        // Asserts that the field includeQueryStatsMetricsForOpIndex has been set with 'opIndex'.
        // When a shard server receives this 'updateOpEntry', it will append cursor metrics in
        // response.
        ASSERT_TRUE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        ASSERT_EQ(*updateOpEntry.getIncludeQueryStatsMetricsForOpIndex(), opIndex);
    }

    // Asserts that after reaching the limit, we no longer request for metrics so as to prevent the
    // response growing beyond the allowed object size limit.
    for (size_t opIndex = WriteBatchQueryStatsRegistrar::kMaxBatchOpsMetricsRequested + 1;
         opIndex < WriteBatchQueryStatsRegistrar::kMaxBatchOpsMetricsRequested + 10;
         opIndex++) {
        registerMockKey(CurOp::get(opCtx)->debug(), opIndex);

        write_ops::UpdateOpEntry updateOpEntry;
        ASSERT_FALSE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        queryStatsRegistrar.setIncludeQueryStatsMetricsIfRequested(opCtx, opIndex, updateOpEntry);
        ASSERT_FALSE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
    }
}

TEST_F(WriteBatchQueryStatsRegistrarTest, PassthroughMetricsOpIndexForCoordinateMultiUpdateTest) {
    // Setting this to simulate the scenario that a primary shard receives a dispatched update
    // command (_shardsvrCoordinateMultiUpdate) from the router and it has to act like a router.
    opCtx->setCommandForwardedFromRouter();

    WriteBatchQueryStatsRegistrar queryStatsRegistrar;

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
        queryStatsRegistrar.setIncludeQueryStatsMetricsIfRequested(opCtx, 100, updateOpEntry);

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
        queryStatsRegistrar.setIncludeQueryStatsMetricsIfRequested(opCtx, 200, updateOpEntry);

        // Asserts that the index in 'updateOpEntry' remain the same and not be unset by the empty
        // QueryStatsInfo.
        ASSERT_TRUE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        ASSERT_EQ(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex(), 42);
    }
}

/**
 * Show that we don't register writes for query stats collection when the write sample rate is zero,
 * even if the read path is configured for query stats collection.
 */
TEST_F(WriteBatchQueryStatsRegistrarTest, RegisterRequestNotSampledWhenWriteRateIsZero) {
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

        WriteBatchQueryStatsRegistrar::parseAndRegisterRequest(
            opCtx, cmdRef, false /* skip request */);

        const auto& opDebug = CurOp::get(opCtx)->debug();
        ASSERT_FALSE(opDebug.hasQueryStatsInfo(0));
    };

    runTest([&] { readLimiter.configureSampleBased(1000, 42); });
    runTest([&] { readLimiter.configureWindowBased(1000); });
}

}  // namespace
}  // namespace mongo::unified_write_executor
