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

#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/concurrency/locker_mongos_client_observer.h"
#include "mongo/s/mock_ns_targeter.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/bulk_write_exec.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

auto initTargeterFullRange(const NamespaceString& nss, const ShardEndpoint& endpoint) {
    std::vector<MockRange> range{MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << MAXKEY))};
    return std::make_unique<MockNSTargeter>(nss, std::move(range));
}

auto initTargeterSplitRange(const NamespaceString& nss,
                            const ShardEndpoint& endpointA,
                            const ShardEndpoint& endpointB) {
    std::vector<MockRange> range{MockRange(endpointA, BSON("x" << MINKEY), BSON("x" << 0)),
                                 MockRange(endpointB, BSON("x" << 0), BSON("x" << MAXKEY))};
    return std::make_unique<MockNSTargeter>(nss, std::move(range));
}

auto initTargeterHalfRange(const NamespaceString& nss, const ShardEndpoint& endpoint) {
    // x >= 0 values are untargetable
    std::vector<MockRange> range{MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << 0))};
    return std::make_unique<MockNSTargeter>(nss, std::move(range));
}

using namespace bulkWriteExec;

class BulkWriteOpTest : public ServiceContextTest {
protected:
    BulkWriteOpTest() {
        auto service = getServiceContext();
        service->registerClientObserver(std::make_unique<LockerMongosClientObserver>());
        _opCtxHolder = makeOperationContext();
        _opCtx = _opCtxHolder.get();
    }

    ServiceContext::UniqueOperationContext _opCtxHolder;
    OperationContext* _opCtx;
};

// Test targeting a single op in a bulkWrite request.
TEST_F(BulkWriteOpTest, TargetSingleOp) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"), ShardVersion::IGNORED(), boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.push_back(initTargeterFullRange(nss, endpoint));

    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSON("x" << 1))},
                                    {NamespaceInfoEntry(nss)});

    BulkWriteOp bulkWriteOp(_opCtx, request);

    stdx::unordered_map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.begin()->second->getEndpoint(), endpoint);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
}

// Test targeting a single op with target error.
TEST_F(BulkWriteOpTest, TargetSingleOpError) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"), ShardVersion::IGNORED(), boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    // Initialize the targeter so that x >= 0 values are untargetable so target call will encounter
    // an error.
    targeters.push_back(initTargeterHalfRange(nss, endpoint));

    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSON("x" << 1))},
                                    {NamespaceInfoEntry(nss)});

    BulkWriteOp bulkWriteOp(_opCtx, request);

    stdx::unordered_map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    // target should return target error when recordTargetErrors = false.
    ASSERT_NOT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 0u);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Ready);

    // target should transition the writeOp to an error state upon target errors when
    // recordTargetErrors = true.
    ASSERT_OK(bulkWriteOp.target(targeters, true, targeted));
    ASSERT_EQUALS(targeted.size(), 0u);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Error);
}

// Test multiple ordered ops that target the same shard.
TEST_F(BulkWriteOpTest, TargetMultiOpsOrdered_SameShard) {
    NamespaceString nss0("foo.bar");
    NamespaceString nss1("bar.foo");
    ShardEndpoint endpoint(ShardId("shard"), ShardVersion::IGNORED(), boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.push_back(initTargeterFullRange(nss0, endpoint));
    targeters.push_back(initTargeterFullRange(nss1, endpoint));

    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(1, BSON("x" << 1)), BulkWriteInsertOp(0, BSON("x" << 2))},
        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});

    BulkWriteOp bulkWriteOp(_opCtx, request);

    stdx::unordered_map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.begin()->second->getEndpoint(), endpoint);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 2u);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
}

// Test multiple ordered ops where one of them result in a target error.
TEST_F(BulkWriteOpTest, TargetMultiOpsOrdered_RecordTargetErrors) {
    NamespaceString nss0("foo.bar");
    NamespaceString nss1("bar.foo");
    ShardEndpoint endpoint(ShardId("shard"), ShardVersion::IGNORED(), boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    // Initialize the targeter so that x >= 0 values are untargetable so target call will encounter
    // an error.
    targeters.push_back(initTargeterHalfRange(nss0, endpoint));
    targeters.push_back(initTargeterFullRange(nss1, endpoint));

    // Only the second op would get a target error.
    BulkWriteCommandRequest request({BulkWriteInsertOp(1, BSON("x" << 1)),
                                     BulkWriteInsertOp(0, BSON("x" << 2)),
                                     BulkWriteInsertOp(0, BSON("x" << -1))},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});

    BulkWriteOp bulkWriteOp(_opCtx, request);

    stdx::unordered_map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(bulkWriteOp.target(targeters, true, targeted));

    // Only the first op should be targeted as the second op encounters a target error. But this
    // won't record the target error since there could be an error in the first op before executing
    // the second op.
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.begin()->second->getEndpoint(), endpoint);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Ready);

    targeted.clear();

    // Pretending the first op was done successfully, the target error should be recorded in the
    // second op.
    ASSERT_OK(bulkWriteOp.target(targeters, true, targeted));
    ASSERT_EQUALS(targeted.size(), 0u);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Error);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Ready);
}

// Test multiple ordered ops that target two different shards.
TEST_F(BulkWriteOpTest, TargetMultiOpsOrdered_DifferentShard) {
    NamespaceString nss0("foo.bar");
    NamespaceString nss1("bar.foo");
    ShardEndpoint endpointA(ShardId("shardA"), ShardVersion::IGNORED(), boost::none);
    ShardEndpoint endpointB(ShardId("shardB"), ShardVersion::IGNORED(), boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.push_back(initTargeterSplitRange(nss0, endpointA, endpointB));
    targeters.push_back(initTargeterFullRange(nss1, endpointA));

    // ops[0] -> shardA
    // ops[1] -> shardB
    // ops[2] -> shardA
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSON("x" << -1)),
                                     BulkWriteInsertOp(0, BSON("x" << 1)),
                                     BulkWriteInsertOp(1, BSON("x" << 1))},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});

    BulkWriteOp bulkWriteOp(_opCtx, request);

    stdx::unordered_map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;

    // The resulting batch should be {shardA: [ops[0]]}.
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.begin()->second->getEndpoint(), endpointA);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Ready);

    targeted.clear();

    // The resulting batch should be {shardB: [ops[1]]}.
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.begin()->second->getEndpoint(), endpointB);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Ready);

    targeted.clear();

    // The resulting batch should be {shardA: [ops[2]]}.
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.begin()->second->getEndpoint(), endpointA);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Pending);
}

// Test multiple unordered ops that target two different shards.
TEST_F(BulkWriteOpTest, TargetMultiOpsUnordered) {
    NamespaceString nss0("foo.bar");
    NamespaceString nss1("bar.foo");
    ShardEndpoint endpointA(ShardId("shardA"), ShardVersion::IGNORED(), boost::none);
    ShardEndpoint endpointB(ShardId("shardB"), ShardVersion::IGNORED(), boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.push_back(initTargeterSplitRange(nss0, endpointA, endpointB));
    targeters.push_back(initTargeterFullRange(nss1, endpointA));

    // ops[0] -> shardA
    // ops[1] -> shardB
    // ops[2] -> shardA
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSON("x" << -1)),
                                     BulkWriteInsertOp(0, BSON("x" << 1)),
                                     BulkWriteInsertOp(1, BSON("x" << 1))},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    request.setOrdered(false);

    BulkWriteOp bulkWriteOp(_opCtx, request);

    // The two resulting batches should be:
    // {shardA: [ops[0], ops[2]]}
    // {shardB: [ops[1]]}
    stdx::unordered_map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 2u);

    ASSERT_EQUALS(targeted[ShardId("shardA")]->getWrites().size(), 2u);
    ASSERT_EQUALS(targeted[ShardId("shardA")]->getWrites()[0]->writeOpRef.first, 0);
    ASSERT_EQUALS(targeted[ShardId("shardA")]->getWrites()[1]->writeOpRef.first, 2);

    ASSERT_EQUALS(targeted[ShardId("shardB")]->getWrites().size(), 1u);
    ASSERT_EQUALS(targeted[ShardId("shardB")]->getWrites()[0]->writeOpRef.first, 1);

    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Pending);
}

// Test multiple unordered ops where one of them result in a target error.
TEST_F(BulkWriteOpTest, TargetMultiOpsUnordered_RecordTargetErrors) {
    NamespaceString nss0("foo.bar");
    NamespaceString nss1("bar.foo");
    ShardEndpoint endpoint(ShardId("shard"), ShardVersion::IGNORED(), boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    // Initialize the targeter so that x >= 0 values are untargetable so target call will encounter
    // an error.
    targeters.push_back(initTargeterHalfRange(nss0, endpoint));
    targeters.push_back(initTargeterFullRange(nss1, endpoint));

    // Only the second op would get a target error.
    BulkWriteCommandRequest request({BulkWriteInsertOp(1, BSON("x" << 1)),
                                     BulkWriteInsertOp(0, BSON("x" << 2)),
                                     BulkWriteInsertOp(0, BSON("x" << -1))},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    request.setOrdered(false);

    BulkWriteOp bulkWriteOp(_opCtx, request);

    stdx::unordered_map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(bulkWriteOp.target(targeters, true, targeted));

    // In the unordered case, both the first and the third ops should be targeted successfully
    // despite targeting error on the second op.
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.begin()->second->getEndpoint(), endpoint);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 2u);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Error);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Pending);
}

}  // namespace

}  // namespace mongo
