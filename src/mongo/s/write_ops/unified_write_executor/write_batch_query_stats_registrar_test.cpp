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
    }

    ServiceContext::UniqueOperationContext opCtxHolder;
    OperationContext* opCtx;
};

TEST_F(WriteBatchQueryStatsRegistrarTest, RegisterRequestTest) {
    const DatabaseVersion nss1DbVersion(UUID::gen(), Timestamp(1, 0));
    const ShardEndpoint nss1Shard1(shardId1, ShardVersion::UNTRACKED(), nss1DbVersion);

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

    // Enable query stats collection and configure rate limiting
    RAIIServerParameterControllerForTest controller("featureFlagQueryStatsUpdateCommand", true);
    auto& limiter = query_stats::QueryStatsStoreManager::getRateLimiter(getServiceContext());
    limiter.configureWindowBased(-1);
    WriteBatchQueryStatsRegistrar::registerRequest(opCtx, cmdRef);

    // Asserts that all the update ops are registered.
    const auto& opDebug = CurOp::get(opCtx)->debug();
    for (size_t opIndex = 0; opIndex < 3; opIndex++) {
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

TEST_F(WriteBatchQueryStatsRegistrarTest, SetIncludeQueryStatsMetricsIfRequestedTest) {
    WriteBatchQueryStatsRegistrar queryStatsRegisterer;

    auto registerMockKey = [&](OpDebug& opDebug, size_t opIndex) {
        OpDebug::QueryStatsInfo qsi;
        qsi.key = std::make_unique<query_stats::MockKey>(opCtx);
        qsi.keyHash = 42;
        opDebug.setQueryStatsInfoAtOpIndex(opIndex, std::move(qsi));
    };

    // Asserts that we are allowed to request metrics for update ops up to
    // kMaxBatchOpsMetricsRequested.
    for (size_t opIndex = 0; opIndex < WriteBatchQueryStatsRegistrar::kMaxBatchOpsMetricsRequested;
         opIndex++) {
        registerMockKey(CurOp::get(opCtx)->debug(), opIndex);

        write_ops::UpdateOpEntry updateOpEntry;
        ASSERT_FALSE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        queryStatsRegisterer.setIncludeQueryStatsMetricsIfRequested(
            CurOp::get(opCtx), opIndex, updateOpEntry);

        // Asserts that the field includeQueryStatsMetricsForOpIndex has been set with 'opIndex'.
        // When a shard server receives this 'updateOpEntry', it will append cursor metrics in
        // response.
        ASSERT_TRUE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        ASSERT_EQ(*updateOpEntry.getIncludeQueryStatsMetricsForOpIndex(), opIndex);
    }

    // Asserts that after reaching the limit, we no longer request for metrics so as to prevent the
    // response growing beyond the allowed object size limit.
    for (size_t opIndex = WriteBatchQueryStatsRegistrar::kMaxBatchOpsMetricsRequested;
         opIndex < WriteBatchQueryStatsRegistrar::kMaxBatchOpsMetricsRequested + 10;
         opIndex++) {
        registerMockKey(CurOp::get(opCtx)->debug(), opIndex);

        write_ops::UpdateOpEntry updateOpEntry;
        ASSERT_FALSE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
        queryStatsRegisterer.setIncludeQueryStatsMetricsIfRequested(
            CurOp::get(opCtx), opIndex, updateOpEntry);
        ASSERT_FALSE(updateOpEntry.getIncludeQueryStatsMetricsForOpIndex());
    }
}

}  // namespace
}  // namespace mongo::unified_write_executor
