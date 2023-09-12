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

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/commands/bulk_write_crud_op.h"
#include "mongo/db/commands/bulk_write_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/shard_id.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version.h"
#include "mongo/s/index_version.h"
#include "mongo/s/mock_ns_targeter.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/bulk_write_exec.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class BulkWriteMockNSTargeter : public MockNSTargeter {
public:
    using MockNSTargeter::MockNSTargeter;

    enum class LastErrorType { kCouldNotTarget, kStaleShardVersion, kStaleDbVersion };

    void noteCouldNotTarget() override {
        _lastError = LastErrorType::kCouldNotTarget;
    }

    void noteStaleShardResponse(OperationContext* opCtx,
                                const ShardEndpoint& endpoint,
                                const StaleConfigInfo& staleInfo) override {
        _lastError = LastErrorType::kStaleShardVersion;
    }

    void noteStaleDbResponse(OperationContext* opCtx,
                             const ShardEndpoint& endpoint,
                             const StaleDbRoutingVersion& staleInfo) override {
        _lastError = LastErrorType::kStaleDbVersion;
    }

    bool refreshIfNeeded(OperationContext* opCtx) override {
        if (!_lastError) {
            return false;
        }

        ON_BLOCK_EXIT([&] { _lastError = boost::none; });

        // Not changing metadata but incrementing _numRefreshes.
        _numRefreshes++;
        return false;
    }

    const boost::optional<LastErrorType>& getLastError() const {
        return _lastError;
    }

    int getNumRefreshes() const {
        return _numRefreshes;
    }

private:
    boost::optional<LastErrorType> _lastError;
    int _numRefreshes = 0;
};

auto initTargeterFullRange(const NamespaceString& nss, const ShardEndpoint& endpoint) {
    std::vector<MockRange> range{MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << MAXKEY))};
    return std::make_unique<BulkWriteMockNSTargeter>(nss, std::move(range));
}

auto initTargeterSplitRange(const NamespaceString& nss,
                            const ShardEndpoint& endpointA,
                            const ShardEndpoint& endpointB) {
    std::vector<MockRange> range{MockRange(endpointA, BSON("x" << MINKEY), BSON("x" << 0)),
                                 MockRange(endpointB, BSON("x" << 0), BSON("x" << MAXKEY))};
    return std::make_unique<BulkWriteMockNSTargeter>(nss, std::move(range));
}

auto initTargeterHalfRange(const NamespaceString& nss, const ShardEndpoint& endpoint) {
    // x >= 0 values are untargetable
    std::vector<MockRange> range{MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << 0))};
    return std::make_unique<BulkWriteMockNSTargeter>(nss, std::move(range));
}

using namespace bulk_write_exec;

class BulkWriteOpTest : public ServiceContextTest {
protected:
    BulkWriteOpTest() {
        _opCtxHolder = makeOperationContext();
        _opCtx = _opCtxHolder.get();
    }

    ServiceContext::UniqueOperationContext _opCtxHolder;
    OperationContext* _opCtx;

    // This failpoint is to skip running the useTwoPhaseWriteProtocol check which expects the Grid
    // to be initialized. With the feature flag on, the helper always returns false, which signifies
    // that we have a targetable write op.
    std::unique_ptr<FailPointEnableBlock> _skipUseTwoPhaseWriteProtocolCheck =
        std::make_unique<FailPointEnableBlock>("skipUseTwoPhaseWriteProtocolCheck");
};

// Test targeting a single op in a bulkWrite request.
TEST_F(BulkWriteOpTest, TargetSingleOp) {
    ShardId shardId("shard");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint(
        shardId, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.push_back(initTargeterFullRange(nss, endpoint));

    auto runTest = [&](const BulkWriteCommandRequest& request) {
        BulkWriteOp bulkWriteOp(_opCtx, request);

        TargetedBatchMap targeted;
        ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
        ASSERT_EQUALS(targeted.size(), 1u);
        ASSERT_EQUALS(targeted.begin()->second->getShardId(), shardId);
        ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);
        assertEndpointsEqual(targeted.begin()->second->getWrites()[0]->endpoint, endpoint);
        ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    };

    // Insert
    runTest(
        BulkWriteCommandRequest({BulkWriteInsertOp(0, BSON("x" << 1))}, {NamespaceInfoEntry(nss)}));
    // Update
    runTest(BulkWriteCommandRequest(
        {BulkWriteUpdateOp(0, BSON("x" << 1), BSON("$set" << BSON("y" << 2)))},
        {NamespaceInfoEntry(nss)}));
    // Delete
    runTest(
        BulkWriteCommandRequest({BulkWriteDeleteOp(0, BSON("x" << 1))}, {NamespaceInfoEntry(nss)}));
}

// Test targeting a single op with target error.
TEST_F(BulkWriteOpTest, TargetSingleOpError) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"),
                           ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                           boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    // Initialize the targeter so that x >= 0 values are untargetable so target call will encounter
    // an error.
    targeters.push_back(initTargeterHalfRange(nss, endpoint));

    auto runTest = [&](const BulkWriteCommandRequest& request) {
        BulkWriteOp bulkWriteOp(_opCtx, request);

        TargetedBatchMap targeted;
        // target should return target error when recordTargetErrors = false.
        ASSERT_NOT_OK(bulkWriteOp.target(targeters, false, targeted));
        ASSERT_EQUALS(targeted.size(), 0u);
        ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Ready);

        // target should transition the writeOp to an error state upon target errors when
        // recordTargetErrors = true.
        ASSERT_OK(bulkWriteOp.target(targeters, true, targeted));
        ASSERT_EQUALS(targeted.size(), 0u);
        ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Error);
    };

    // Insert
    runTest(
        BulkWriteCommandRequest({BulkWriteInsertOp(0, BSON("x" << 1))}, {NamespaceInfoEntry(nss)}));
    // Update
    runTest(BulkWriteCommandRequest(
        {BulkWriteUpdateOp(0, BSON("x" << 1), BSON("$set" << BSON("y" << 2)))},
        {NamespaceInfoEntry(nss)}));
    // Delete
    runTest(
        BulkWriteCommandRequest({BulkWriteDeleteOp(0, BSON("x" << 1))}, {NamespaceInfoEntry(nss)}));
}

// Test multiple ordered ops that target the same shard.
TEST_F(BulkWriteOpTest, TargetMultiOpsOrdered_SameShard) {
    ShardId shardId("shard");
    NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("foo.bar");
    NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("bar.foo");
    // Two different endpoints targeting the same shard for the two namespaces.
    ShardEndpoint endpoint0(
        shardId, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);
    ShardEndpoint endpoint1(
        shardId,
        ShardVersionFactory::make(ChunkVersion({OID::gen(), Timestamp(2)}, {10, 11}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.push_back(initTargeterFullRange(nss0, endpoint0));
    targeters.push_back(initTargeterFullRange(nss1, endpoint1));

    auto runTest = [&](const BulkWriteCommandRequest& request) {
        BulkWriteOp bulkWriteOp(_opCtx, request);

        // Test that both writes target the same shard under two different endpoints for their
        // namespace.
        TargetedBatchMap targeted;
        ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
        ASSERT_EQUALS(targeted.size(), 1u);
        ASSERT_EQUALS(targeted.begin()->second->getShardId(), shardId);
        ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 2u);
        assertEndpointsEqual(targeted.begin()->second->getWrites()[0]->endpoint, endpoint1);
        assertEndpointsEqual(targeted.begin()->second->getWrites()[1]->endpoint, endpoint0);
        ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
        ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    };

    // Two inserts
    runTest(BulkWriteCommandRequest(
        {BulkWriteInsertOp(1, BSON("x" << 1)), BulkWriteInsertOp(0, BSON("x" << 2))},
        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)}));
    // Two updates
    runTest(BulkWriteCommandRequest(
        {BulkWriteUpdateOp(1, BSON("x" << 1), BSON("$set" << BSON("y" << 2))),
         BulkWriteUpdateOp(0, BSON("x" << 2), BSON("$set" << BSON("y" << 2)))},
        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)}));
    // Two deletes
    runTest(BulkWriteCommandRequest(
        {BulkWriteDeleteOp(1, BSON("x" << 1)), BulkWriteDeleteOp(0, BSON("x" << 2))},
        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)}));
    // Mixed op types: update + delete
    runTest(BulkWriteCommandRequest(
        {BulkWriteUpdateOp(1, BSON("x" << 1), BSON("$set" << BSON("y" << 2))),
         BulkWriteDeleteOp(0, BSON("x" << 2))},
        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)}));
}

// Test multiple ordered ops where one of them result in a target error.
TEST_F(BulkWriteOpTest, TargetMultiOpsOrdered_RecordTargetErrors) {
    ShardId shardId("shard");
    NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("foo.bar");
    NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("bar.foo");
    ShardEndpoint endpoint0(
        shardId, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);
    ShardEndpoint endpoint1(
        shardId,
        ShardVersionFactory::make(ChunkVersion({OID::gen(), Timestamp(2)}, {10, 11}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    // Initialize the targeter so that x >= 0 values are untargetable so target call will encounter
    // an error.
    targeters.push_back(initTargeterHalfRange(nss0, endpoint0));
    targeters.push_back(initTargeterFullRange(nss1, endpoint1));

    auto runTest = [&](const BulkWriteCommandRequest& request) {
        BulkWriteOp bulkWriteOp(_opCtx, request);

        TargetedBatchMap targeted;
        ASSERT_OK(bulkWriteOp.target(targeters, true, targeted));

        // Only the first op should be targeted as the second op encounters a target error. But this
        // won't record the target error since there could be an error in the first op before
        // executing the second op.
        ASSERT_EQUALS(targeted.size(), 1u);
        ASSERT_EQUALS(targeted.begin()->second->getShardId(), shardId);
        ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);
        assertEndpointsEqual(targeted.begin()->second->getWrites()[0]->endpoint, endpoint1);
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
    };

    // Requests where only the second op would get a target error.

    // Insert gets the target error
    runTest(BulkWriteCommandRequest({BulkWriteInsertOp(1, BSON("x" << 1)),
                                     BulkWriteInsertOp(0, BSON("x" << 2)),
                                     BulkWriteInsertOp(0, BSON("x" << -1))},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)}));
    // Update gets the target error
    runTest(BulkWriteCommandRequest(
        {BulkWriteInsertOp(1, BSON("x" << 1)),
         BulkWriteUpdateOp(0, BSON("x" << 2), BSON("$set" << BSON("y" << 2))),
         BulkWriteInsertOp(0, BSON("x" << -1))},
        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)}));
    // Delete gets the target error
    runTest(BulkWriteCommandRequest({BulkWriteInsertOp(1, BSON("x" << 1)),
                                     BulkWriteDeleteOp(0, BSON("x" << 2)),
                                     BulkWriteInsertOp(0, BSON("x" << -1))},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)}));
}

// Test multiple ordered ops that target two different shards.
TEST_F(BulkWriteOpTest, TargetMultiOpsOrdered_DifferentShard) {
    ShardId shardIdA("shardA");
    ShardId shardIdB("shardB");
    NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("foo.bar");
    NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("bar.foo");
    ShardEndpoint endpointA0(
        shardIdA, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);
    ShardEndpoint endpointB0(
        shardIdB, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);
    ShardEndpoint endpointA1(
        shardIdA,
        ShardVersionFactory::make(ChunkVersion({OID::gen(), Timestamp(2)}, {10, 11}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.push_back(initTargeterSplitRange(nss0, endpointA0, endpointB0));
    targeters.push_back(initTargeterFullRange(nss1, endpointA1));

    // ops[0] -> shardA
    // ops[1] -> shardB
    // ops[2] -> shardA
    // ops[3] -> shardB
    // ops[4] -> shardA
    BulkWriteCommandRequest request(
        {
            BulkWriteInsertOp(0, BSON("x" << -1)),
            BulkWriteInsertOp(0, BSON("x" << 1)),
            BulkWriteInsertOp(1, BSON("x" << 1)),
            BulkWriteDeleteOp(0, BSON("x" << 1)),
            BulkWriteUpdateOp(0, BSON("x" << -1), BSON("$set" << BSON("y" << 2))),
        },
        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});

    BulkWriteOp bulkWriteOp(_opCtx, request);

    TargetedBatchMap targeted;

    // The resulting batch should be {shardA: [ops[0]]}.
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getShardId(), shardIdA);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);
    assertEndpointsEqual(targeted.begin()->second->getWrites()[0]->endpoint, endpointA0);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(4).getWriteState(), WriteOpState_Ready);

    targeted.clear();

    // The resulting batch should be {shardB: [ops[1]]}.
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getShardId(), shardIdB);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);
    assertEndpointsEqual(targeted.begin()->second->getWrites()[0]->endpoint, endpointB0);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(4).getWriteState(), WriteOpState_Ready);

    targeted.clear();

    // The resulting batch should be {shardA: [ops[2]]}.
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getShardId(), shardIdA);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);
    assertEndpointsEqual(targeted.begin()->second->getWrites()[0]->endpoint, endpointA1);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(4).getWriteState(), WriteOpState_Ready);

    targeted.clear();

    // The resulting batch should be {shardB: [ops[3]]}.
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getShardId(), shardIdB);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);
    assertEndpointsEqual(targeted.begin()->second->getWrites()[0]->endpoint, endpointB0);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(4).getWriteState(), WriteOpState_Ready);

    targeted.clear();

    // The resulting batch should be {shardA: [ops[4]]}.
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getShardId(), shardIdA);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);
    assertEndpointsEqual(targeted.begin()->second->getWrites()[0]->endpoint, endpointA0);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(4).getWriteState(), WriteOpState_Pending);
}

// Test targeting ordered ops where a multi-target sub-batch must only contain writes for a
// single write op.
TEST_F(BulkWriteOpTest, TargetMultiTargetOpsOrdered) {
    ShardId shardIdA("shardA");
    ShardId shardIdB("shardB");
    NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpointA(
        shardIdA, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);
    ShardEndpoint endpointB(
        shardIdB, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.push_back(initTargeterSplitRange(nss0, endpointA, endpointB));

    // Ordered update and delete ops. We place multi-target ops in between single-target ops to the
    // same shards, to ensure we correctly separate the multi-target ops into their own batches.
    // Expected targets:
    // ops[0] -> shardA
    // ops[1] -> shardA and shardB
    // ops[2] -> shardB
    // ops[3] -> shardB
    // ops[4] -> shardA and shardB
    // ops[5] -> shardA
    BulkWriteCommandRequest request(
        {
            BulkWriteUpdateOp(0, BSON("x" << -1), BSON("$set" << BSON("z" << 3))),
            BulkWriteUpdateOp(
                0, BSON("x" << BSON("$gte" << -5 << "$lt" << 5)), BSON("$set" << BSON("y" << 2))),
            BulkWriteUpdateOp(0, BSON("x" << 1), BSON("$set" << BSON("z" << 3))),
            BulkWriteDeleteOp(0, BSON("x" << 1)),
            BulkWriteDeleteOp(0, BSON("x" << BSON("$gte" << -5 << "$lt" << 5))),
            BulkWriteDeleteOp(0, BSON("x" << -1)),
        },
        {NamespaceInfoEntry(nss0)});

    BulkWriteOp bulkWriteOp(_opCtx, request);

    // The resulting batches should be:
    // {shardA: [ops[0]}
    // {shardA: [ops[1]]}, {shardB: [ops[1]]}
    // {shardB: [ops[2], ops[3]]}
    // {shardA: [ops[4]]}, {shardB: [ops[4]]}
    // {shardA: [ops[5]]}

    TargetedBatchMap targeted;

    // {shardA: [ops[0]}
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites().size(), 1u);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites()[0]->writeOpRef.first, 0);
    assertEndpointsEqual(targeted[shardIdA]->getWrites()[0]->endpoint, endpointA);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(4).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(5).getWriteState(), WriteOpState_Ready);

    targeted.clear();

    // {shardA: [ops[1]]}, {shardB: [ops[1]]}
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 2u);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites().size(), 1u);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites()[0]->writeOpRef.first, 1);
    assertEndpointsEqual(targeted[shardIdA]->getWrites()[0]->endpoint, endpointA);
    ASSERT_EQUALS(targeted[shardIdB]->getWrites().size(), 1u);
    ASSERT_EQUALS(targeted[shardIdB]->getWrites()[0]->writeOpRef.first, 1);
    assertEndpointsEqual(targeted[shardIdB]->getWrites()[0]->endpoint, endpointB);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(4).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(5).getWriteState(), WriteOpState_Ready);

    targeted.clear();

    // {shardB: [ops[2], ops[3]]}
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted[shardIdB]->getWrites().size(), 2u);
    assertEndpointsEqual(targeted[shardIdB]->getWrites()[0]->endpoint, endpointB);
    ASSERT_EQUALS(targeted[shardIdB]->getWrites()[0]->writeOpRef.first, 2);
    assertEndpointsEqual(targeted[shardIdB]->getWrites()[1]->endpoint, endpointB);
    ASSERT_EQUALS(targeted[shardIdB]->getWrites()[1]->writeOpRef.first, 3);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(4).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(5).getWriteState(), WriteOpState_Ready);

    targeted.clear();

    // {shardA: [ops[4]]}, {shardB: [ops[4]]}
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 2u);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites().size(), 1u);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites()[0]->writeOpRef.first, 4);
    assertEndpointsEqual(targeted[shardIdA]->getWrites()[0]->endpoint, endpointA);
    ASSERT_EQUALS(targeted[shardIdB]->getWrites().size(), 1u);
    ASSERT_EQUALS(targeted[shardIdB]->getWrites()[0]->writeOpRef.first, 4);
    assertEndpointsEqual(targeted[shardIdB]->getWrites()[0]->endpoint, endpointB);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(4).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(5).getWriteState(), WriteOpState_Ready);


    targeted.clear();

    // {shardA: [ops[5]]}
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites().size(), 1u);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites()[0]->writeOpRef.first, 5);
    assertEndpointsEqual(targeted[shardIdA]->getWrites()[0]->endpoint, endpointA);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(4).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(5).getWriteState(), WriteOpState_Pending);
}

// Test targeting unordered ops of the same namespace that target the same shard/endpoint under two
// different shardVersions.
// As discussed in SERVER-34347, because of the way that (non-transactional) multi-target writes
// disregard the shardVersion and use ChunkVersion::IGNORED, we cannot have together in a single
// sub-batch an op for a multi-target write *and* an op for a single-target write that target
// the same endpoint, because the single target write will use the actual shardVersion.
TEST_F(BulkWriteOpTest, TargetMultiOpsUnordered_OneShard_TwoEndpoints) {
    ShardId shardIdA("shardA");
    ShardId shardIdB("shardB");
    NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("foo.bar");

    // The endpoints we'll use for our targeter.
    ShardEndpoint endpointA(
        shardIdA,
        ShardVersionFactory::make(ChunkVersion({OID::gen(), Timestamp(2)}, {10, 11}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);
    ShardEndpoint endpointB(
        shardIdB,
        ShardVersionFactory::make(ChunkVersion({OID::gen(), Timestamp(3)}, {11, 12}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.push_back(initTargeterSplitRange(nss0, endpointA, endpointB));


    // Used for assertions below; equivalent to the endpoints that multi-target ops will use (same
    // as those above but no shard version.)
    ShardEndpoint endpointANoVersion(
        shardIdA, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);
    ShardEndpoint endpointBNoVersion(
        shardIdB, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);

    // We expect the ops to target the following endpoints with/without shardVersion as indicated:
    // ops[0] -> A, shardVersion included
    // ops[1] -> A shardVersion ignored, B shardVersion ignored
    // ops[2] -> B, shardVersion included
    // ops[3] -> A shardVersion ignored, B shardVersion ignored
    // ops[4] -> A shardVersion included

    // Due to the interleaving of ops, each op should end up split into its own sub-batch, since no
    // two consecutive ops target the same endpoint with the same shardVersion.
    BulkWriteCommandRequest request(
        {
            BulkWriteUpdateOp(0, BSON("x" << -1), BSON("$set" << BSON("z" << 3))),
            BulkWriteUpdateOp(
                0, BSON("x" << BSON("$gte" << -5 << "$lt" << 5)), BSON("$set" << BSON("y" << 2))),
            BulkWriteDeleteOp(0, BSON("x" << 1)),
            BulkWriteDeleteOp(0, BSON("x" << BSON("$gte" << -5 << "$lt" << 5))),
            BulkWriteInsertOp(0, BSON("x" << -2)),
        },
        {NamespaceInfoEntry(nss0)});
    request.setOrdered(false);

    BulkWriteOp bulkWriteOp(_opCtx, request);

    TargetedBatchMap targeted;

    // batch with ops[0]
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites().size(), 1);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites()[0]->writeOpRef.first, 0);
    assertEndpointsEqual(targeted[shardIdA]->getWrites()[0]->endpoint, endpointA);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(4).getWriteState(), WriteOpState_Ready);

    targeted.clear();

    // batch with ops[1]
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 2u);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites().size(), 1);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites()[0]->writeOpRef.first, 1);
    assertEndpointsEqual(targeted[shardIdA]->getWrites()[0]->endpoint, endpointANoVersion);
    ASSERT_EQUALS(targeted[shardIdB]->getWrites().size(), 1);
    ASSERT_EQUALS(targeted[shardIdB]->getWrites()[0]->writeOpRef.first, 1);
    assertEndpointsEqual(targeted[shardIdB]->getWrites()[0]->endpoint, endpointBNoVersion);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(4).getWriteState(), WriteOpState_Ready);

    targeted.clear();

    // batch with ops[2]
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted[shardIdB]->getWrites().size(), 1);
    ASSERT_EQUALS(targeted[shardIdB]->getWrites()[0]->writeOpRef.first, 2);
    assertEndpointsEqual(targeted[shardIdB]->getWrites()[0]->endpoint, endpointB);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(4).getWriteState(), WriteOpState_Ready);

    targeted.clear();

    // batch with ops[3]
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 2u);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites().size(), 1);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites()[0]->writeOpRef.first, 3);
    assertEndpointsEqual(targeted[shardIdA]->getWrites()[0]->endpoint, endpointANoVersion);
    ASSERT_EQUALS(targeted[shardIdB]->getWrites().size(), 1);
    ASSERT_EQUALS(targeted[shardIdB]->getWrites()[0]->writeOpRef.first, 3);
    assertEndpointsEqual(targeted[shardIdB]->getWrites()[0]->endpoint, endpointBNoVersion);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(4).getWriteState(), WriteOpState_Ready);

    targeted.clear();

    // batch with ops[4]
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites().size(), 1);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites()[0]->writeOpRef.first, 4);
    assertEndpointsEqual(targeted[shardIdA]->getWrites()[0]->endpoint, endpointA);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(4).getWriteState(), WriteOpState_Pending);
}

// Test multiple unordered ops that target two different shards.
TEST_F(BulkWriteOpTest, TargetMultiOpsUnordered) {
    ShardId shardIdA("shardA");
    ShardId shardIdB("shardB");
    NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("foo.bar");
    NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("bar.foo");
    ShardEndpoint endpointA0(
        shardIdA, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);
    ShardEndpoint endpointB0(
        shardIdB, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);
    ShardEndpoint endpointA1(
        shardIdA,
        ShardVersionFactory::make(ChunkVersion({OID::gen(), Timestamp(2)}, {10, 11}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.push_back(initTargeterSplitRange(nss0, endpointA0, endpointB0));
    targeters.push_back(initTargeterFullRange(nss1, endpointA1));

    // ops[0] -> shardA
    // ops[1] -> shardB
    // ops[2] -> shardA
    // ops[3] -> shardB
    // ops[4] -> shardA
    BulkWriteCommandRequest request(
        {
            BulkWriteInsertOp(0, BSON("x" << -1)),
            BulkWriteInsertOp(0, BSON("x" << 1)),
            BulkWriteInsertOp(1, BSON("x" << 1)),
            BulkWriteDeleteOp(0, BSON("x" << 1)),
            BulkWriteUpdateOp(0, BSON("x" << -1), BSON("$set" << BSON("y" << 2))),
        },
        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    request.setOrdered(false);

    BulkWriteOp bulkWriteOp(_opCtx, request);

    // The two resulting batches should be:
    // {shardA: [ops[0], ops[2], ops[4]]}
    // {shardB: [ops[1], ops[3]]}
    TargetedBatchMap targeted;
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 2u);

    ASSERT_EQUALS(targeted[shardIdA]->getWrites().size(), 3u);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites()[0]->writeOpRef.first, 0);
    assertEndpointsEqual(targeted[shardIdA]->getWrites()[0]->endpoint, endpointA0);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites()[1]->writeOpRef.first, 2);
    assertEndpointsEqual(targeted[shardIdA]->getWrites()[1]->endpoint, endpointA1);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites()[2]->writeOpRef.first, 4);
    assertEndpointsEqual(targeted[shardIdA]->getWrites()[2]->endpoint, endpointA0);

    ASSERT_EQUALS(targeted[shardIdB]->getWrites().size(), 2u);
    ASSERT_EQUALS(targeted[shardIdB]->getWrites()[0]->writeOpRef.first, 1);
    assertEndpointsEqual(targeted[shardIdB]->getWrites()[0]->endpoint, endpointB0);
    ASSERT_EQUALS(targeted[shardIdB]->getWrites()[1]->writeOpRef.first, 3);
    assertEndpointsEqual(targeted[shardIdB]->getWrites()[1]->endpoint, endpointB0);

    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(4).getWriteState(), WriteOpState_Pending);
}

// Test multiple unordered ops where one of them result in a target error.
TEST_F(BulkWriteOpTest, TargetMultiOpsUnordered_RecordTargetErrors) {
    ShardId shardId("shard");
    NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("foo.bar");
    NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("bar.foo");
    ShardEndpoint endpoint0(
        shardId, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);
    ShardEndpoint endpoint1(
        shardId,
        ShardVersionFactory::make(ChunkVersion({OID::gen(), Timestamp(2)}, {10, 11}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    // Initialize the targeter so that x >= 0 values are untargetable so target call will encounter
    // an error.
    targeters.push_back(initTargeterHalfRange(nss0, endpoint0));
    targeters.push_back(initTargeterFullRange(nss1, endpoint1));

    // Only the second op would get a target error.
    BulkWriteCommandRequest request({BulkWriteInsertOp(1, BSON("x" << 1)),
                                     BulkWriteInsertOp(0, BSON("x" << 2)),
                                     BulkWriteInsertOp(0, BSON("x" << -1))},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    request.setOrdered(false);

    BulkWriteOp bulkWriteOp(_opCtx, request);

    TargetedBatchMap targeted;
    ASSERT_OK(bulkWriteOp.target(targeters, true, targeted));

    // In the unordered case, both the first and the third ops should be targeted successfully
    // despite targeting error on the second op.
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getShardId(), shardId);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 2u);
    assertEndpointsEqual(targeted.begin()->second->getWrites()[0]->endpoint, endpoint1);
    assertEndpointsEqual(targeted.begin()->second->getWrites()[1]->endpoint, endpoint0);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Error);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Pending);
}

// Tests targeting retryable timeseries update op.
TEST_F(BulkWriteOpTest, TargetRetryableTimeseriesUpdate) {
    ShardId shardId("shard");
    NamespaceString nonTsNs = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint0(
        shardId, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);


    NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo.t");
    NamespaceString bucketNs =
        NamespaceString::createNamespaceString_forTest("foo.system.buckets.t");
    ShardEndpoint endpoint1(
        shardId,
        ShardVersionFactory::make(ChunkVersion({OID::gen(), Timestamp(2)}, {10, 11}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);

    // Set up targeters for both the bucket collection and the non-timeseries collection.
    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.push_back(initTargeterFullRange(nonTsNs, endpoint0));
    targeters.push_back(initTargeterFullRange(bucketNs, endpoint1));

    auto bucketTargeter = static_cast<BulkWriteMockNSTargeter*>(targeters[1].get());
    bucketTargeter->setIsShardedTimeSeriesBucketsNamespace(true);

    BulkWriteCommandRequest request(
        {BulkWriteUpdateOp(0, BSON("x" << 1), BSON("$set" << BSON("y" << 1))),
         BulkWriteUpdateOp(0, BSON("x" << 2), BSON("$set" << BSON("y" << 2))),
         BulkWriteUpdateOp(1, BSON("x" << 1), BSON("$set" << BSON("y" << 1))),
         BulkWriteUpdateOp(1, BSON("x" << 2), BSON("$set" << BSON("y" << 2)))},
        {NamespaceInfoEntry(nonTsNs), NamespaceInfoEntry(ns)});
    request.setOrdered(false);

    // Set up the opCtx for retryable writes.
    _opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
    _opCtx->setTxnNumber(5);

    BulkWriteOp bulkWriteOp(_opCtx, request);

    TargetedBatchMap targeted;

    // The first two retryable updates on nonTsNs should be batched together.
    auto swWriteType = bulkWriteOp.target(targeters, false, targeted);
    ASSERT_OK(swWriteType);
    ASSERT_EQUALS(swWriteType.getValue(), WriteType::Ordinary);
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted[shardId]->getWrites().size(), 2u);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Ready);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Ready);

    targeted.clear();

    // Each of the retryable timeseries updates should be in its own batch.
    swWriteType = bulkWriteOp.target(targeters, false, targeted);
    ASSERT_OK(swWriteType);
    ASSERT_EQUALS(swWriteType.getValue(), WriteType::TimeseriesRetryableUpdate);
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted[shardId]->getWrites().size(), 1u);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Ready);

    targeted.clear();

    swWriteType = bulkWriteOp.target(targeters, false, targeted);
    ASSERT_OK(swWriteType);
    ASSERT_EQUALS(swWriteType.getValue(), WriteType::TimeseriesRetryableUpdate);
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted[shardId]->getWrites().size(), 1u);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(0).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(1).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(2).getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(bulkWriteOp.getWriteOp_forTest(3).getWriteState(), WriteOpState_Pending);
}

// Tests that a targeted write batch to be sent to a shard is correctly converted to a
// bulk command request.
TEST_F(BulkWriteOpTest, BuildChildRequestFromTargetedWriteBatch) {
    ShardId shardId("shard");
    NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("foster.the.people");
    NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("sonate.pacifique");

    // Two different endpoints targeting the same shard for the two namespaces.
    ShardEndpoint endpoint0(
        shardId, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);
    ShardEndpoint endpoint1(
        shardId,
        ShardVersionFactory::make(ChunkVersion({OID::gen(), Timestamp(2)}, {10, 11}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.push_back(initTargeterFullRange(nss0, endpoint0));
    targeters.push_back(initTargeterFullRange(nss1, endpoint1));

    BulkWriteCommandRequest request(
        {
            BulkWriteInsertOp(0, BSON("x" << 1)),                                  // to nss 0
            BulkWriteInsertOp(1, BSON("x" << 2)),                                  // to nss 1
            BulkWriteInsertOp(0, BSON("x" << 3)),                                  // to nss 0
            BulkWriteUpdateOp(0, BSON("x" << 1), BSON("$set" << BSON("y" << 2))),  // to nss 0
            BulkWriteDeleteOp(1, BSON("x" << 1)),
        },
        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});

    // Randomly set ordered and bypass document validation.
    request.setOrdered(time(nullptr) % 2 == 0);
    request.setBypassDocumentValidation(time(nullptr) % 2 == 0);
    LOGV2(7278800,
          "Ordered and bypass document validation set randomly",
          "ordered"_attr = request.getOrdered(),
          "bypassDocumentValidation"_attr = request.getBypassDocumentValidation());

    BulkWriteOp bulkWriteOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));


    auto& batch = targeted.begin()->second;

    auto childRequest = bulkWriteOp.buildBulkCommandRequest(targeters, *batch);

    ASSERT_EQUALS(childRequest.getOrdered(), request.getOrdered());
    ASSERT_EQUALS(childRequest.getBypassDocumentValidation(),
                  request.getBypassDocumentValidation());


    ASSERT_EQUALS(childRequest.getOps().size(), 5u);
    for (size_t i = 0; i < 5u; i++) {
        const auto& childOp = BulkWriteCRUDOp(childRequest.getOps()[i]);
        const auto& origOp = BulkWriteCRUDOp(request.getOps()[i]);
        ASSERT_BSONOBJ_EQ(childOp.toBSON(), origOp.toBSON());
    }

    ASSERT_EQUALS(childRequest.getNsInfo().size(), 2u);
    ASSERT_EQUALS(childRequest.getNsInfo()[0].getShardVersion(), endpoint0.shardVersion);
    ASSERT_EQUALS(childRequest.getNsInfo()[0].getNs(), request.getNsInfo()[0].getNs());
    ASSERT_EQUALS(childRequest.getNsInfo()[1].getShardVersion(), endpoint1.shardVersion);
    ASSERT_EQUALS(childRequest.getNsInfo()[1].getNs(), request.getNsInfo()[1].getNs());
}

// Tests that stmtIds are correctly attached to bulkWrite requests when the operations
// are ordered.
TEST_F(BulkWriteOpTest, TestOrderedOpsNoExistingStmtIds) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("mgmt.kids");

    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.push_back(initTargeterSplitRange(nss, endpointA, endpointB));

    // Because the operations are ordered, the bulkWrite operations is broken up by shard
    // endpoint. In other words, targeting this request will result in two batches:
    // 1) to shard A, and then 2) another to shard B after the first batch is complete.
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSON("x" << -1)),  // stmtId 0, shard A
                                     BulkWriteInsertOp(0, BSON("x" << -2)),  // stmtId 1, shard A
                                     BulkWriteInsertOp(0, BSON("x" << 1)),   // stmtId 2, shard B
                                     BulkWriteInsertOp(0, BSON("x" << 2))},  // stmtId 3, shard B
                                    {NamespaceInfoEntry(nss)});
    request.setOrdered(true);

    // Setting the txnNumber makes it a retryable write.
    _opCtx->setLogicalSessionId(LogicalSessionId(UUID::gen(), SHA256Block()));
    _opCtx->setTxnNumber(TxnNumber(0));
    BulkWriteOp bulkWriteOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));

    auto* batch = targeted.begin()->second.get();
    auto childRequest = bulkWriteOp.buildBulkCommandRequest(targeters, *batch);
    auto childStmtIds = childRequest.getStmtIds();
    ASSERT_EQUALS(childStmtIds->size(), 2u);
    ASSERT_EQUALS(childStmtIds->at(0), 0);
    ASSERT_EQUALS(childStmtIds->at(1), 1);

    // Target again to get a batch for the operations to shard B.
    targeted.clear();
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));

    batch = targeted.begin()->second.get();
    childRequest = bulkWriteOp.buildBulkCommandRequest(targeters, *batch);
    childStmtIds = childRequest.getStmtIds();
    ASSERT_EQUALS(childStmtIds->size(), 2u);
    ASSERT_EQUALS(childStmtIds->at(0), 2);
    ASSERT_EQUALS(childStmtIds->at(1), 3);
}

// Tests that stmtIds are correctly attached to bulkWrite requests when the operations
// are unordered.
TEST_F(BulkWriteOpTest, TestUnorderedOpsNoExistingStmtIds) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("zero7.spinning");

    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.push_back(initTargeterSplitRange(nss, endpointA, endpointB));

    // Since the ops aren't ordered, two batches are produced on a single targeting call:
    // 1) the ops to shard A (op 0 and op 2) are a batch and 2) the ops to shard B (op 1
    // and op 3) are a batch. Therefore the stmtIds in the bulkWrite request sent to the shards
    // will be interleaving.
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSON("x" << -1)),  // stmtId 0, shard A
                                     BulkWriteInsertOp(0, BSON("x" << 1)),   // stmtId 1, shard B
                                     BulkWriteInsertOp(0, BSON("x" << -1)),  // stmtId 2, shard A
                                     BulkWriteInsertOp(0, BSON("x" << 2))},  // stmtId 3, shard B
                                    {NamespaceInfoEntry(nss)});
    request.setOrdered(false);

    // Setting the txnNumber makes it a retryable write.
    _opCtx->setLogicalSessionId(LogicalSessionId(UUID::gen(), SHA256Block()));
    _opCtx->setTxnNumber(TxnNumber(0));
    BulkWriteOp bulkWriteOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));

    // The batch to shard A contains op 0 and op 2.
    auto* batch = targeted[ShardId("shardA")].get();
    auto childRequest = bulkWriteOp.buildBulkCommandRequest(targeters, *batch);
    auto childStmtIds = childRequest.getStmtIds();
    ASSERT_EQUALS(childStmtIds->size(), 2u);
    ASSERT_EQUALS(childStmtIds->at(0), 0);
    ASSERT_EQUALS(childStmtIds->at(1), 2);

    // The batch to shard B contains op 1 and op 3.
    batch = targeted[ShardId("shardB")].get();
    childRequest = bulkWriteOp.buildBulkCommandRequest(targeters, *batch);
    childStmtIds = childRequest.getStmtIds();
    ASSERT_EQUALS(childStmtIds->size(), 2u);
    ASSERT_EQUALS(childStmtIds->at(0), 1);
    ASSERT_EQUALS(childStmtIds->at(1), 3);
}

// Tests that stmtIds are correctly attached to bulkWrite requests when the operations
// are unordered and stmtIds are attached to the request already.
TEST_F(BulkWriteOpTest, TestUnorderedOpsStmtIdsExist) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("zero7.spinning");

    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.push_back(initTargeterSplitRange(nss, endpointA, endpointB));

    // Since the ops aren't ordered, two batches are produced on a single targeting call:
    // 1) the ops to shard A (op 0 and op 2) are a batch and 2) the ops to shard B (op 1
    // and op 3) are a batch. Therefore the stmtIds in the bulkWrite request sent to the shards
    // will be interleaving.
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSON("x" << -1)),  // stmtId 6, shard A
                                     BulkWriteInsertOp(0, BSON("x" << 1)),   // stmtId 7, shard B
                                     BulkWriteInsertOp(0, BSON("x" << -1)),  // stmtId 8, shard A
                                     BulkWriteInsertOp(0, BSON("x" << 2))},  // stmtId 9, shard B
                                    {NamespaceInfoEntry(nss)});
    request.setOrdered(false);
    request.setStmtIds(std::vector<int>{6, 7, 8, 9});

    // Setting the txnNumber makes it a retryable write.
    _opCtx->setLogicalSessionId(LogicalSessionId(UUID::gen(), SHA256Block()));
    _opCtx->setTxnNumber(TxnNumber(0));
    BulkWriteOp bulkWriteOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));

    // The batch to shard A contains op 0 and op 2.
    auto* batch = targeted[ShardId("shardA")].get();
    auto childRequest = bulkWriteOp.buildBulkCommandRequest(targeters, *batch);
    auto childStmtIds = childRequest.getStmtIds();
    ASSERT_EQUALS(childStmtIds->size(), 2u);
    ASSERT_EQUALS(childStmtIds->at(0), 6);
    ASSERT_EQUALS(childStmtIds->at(1), 8);

    // The batch to shard B contains op 1 and op 3.
    batch = targeted[ShardId("shardB")].get();
    childRequest = bulkWriteOp.buildBulkCommandRequest(targeters, *batch);
    childStmtIds = childRequest.getStmtIds();
    ASSERT_EQUALS(childStmtIds->size(), 2u);
    ASSERT_EQUALS(childStmtIds->at(0), 7);
    ASSERT_EQUALS(childStmtIds->at(1), 9);
}

// Tests that stmtIds are correctly attached to bulkWrite requests when the operations
// are unordered and the stmtId field exists.
TEST_F(BulkWriteOpTest, TestUnorderedOpsStmtIdFieldExists) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("zero7.spinning");

    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.push_back(initTargeterSplitRange(nss, endpointA, endpointB));

    // Since the ops aren't ordered, two batches are produced on a single targeting call:
    // 1) the ops to shard A (op 0 and op 2) are a batch and 2) the ops to shard B (op 1
    // and op 3) are a batch. Therefore the stmtIds in the bulkWrite request sent to the shards
    // will be interleaving.
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSON("x" << -1)),  // stmtId 6, shard A
                                     BulkWriteInsertOp(0, BSON("x" << 1)),   // stmtId 7, shard B
                                     BulkWriteInsertOp(0, BSON("x" << -1)),  // stmtId 8, shard A
                                     BulkWriteInsertOp(0, BSON("x" << 2))},  // stmtId 9, shard B
                                    {NamespaceInfoEntry(nss)});
    request.setOrdered(false);
    request.setStmtId(6);  // Produces stmtIds 6, 7, 8, 9

    // Setting the txnNumber makes it a retryable write.
    _opCtx->setLogicalSessionId(LogicalSessionId(UUID::gen(), SHA256Block()));
    _opCtx->setTxnNumber(TxnNumber(0));
    BulkWriteOp bulkWriteOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));

    // The batch to shard A contains op 0 and op 2.
    auto* batch = targeted[ShardId("shardA")].get();
    auto childRequest = bulkWriteOp.buildBulkCommandRequest(targeters, *batch);
    auto childStmtIds = childRequest.getStmtIds();
    ASSERT_EQUALS(childStmtIds->size(), 2u);
    ASSERT_EQUALS(childStmtIds->at(0), 6);
    ASSERT_EQUALS(childStmtIds->at(1), 8);

    // The batch to shard B contains op 1 and op 3.
    batch = targeted[ShardId("shardB")].get();
    childRequest = bulkWriteOp.buildBulkCommandRequest(targeters, *batch);
    childStmtIds = childRequest.getStmtIds();
    ASSERT_EQUALS(childStmtIds->size(), 2u);
    ASSERT_EQUALS(childStmtIds->at(0), 7);
    ASSERT_EQUALS(childStmtIds->at(1), 9);
}

// Test building a child bulkWrite request to send to shards involving timeseries collections.
TEST_F(BulkWriteOpTest, BuildTimeseriesChildRequest) {
    ShardId shardId("shard");
    ShardEndpoint endpoint(
        shardId, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);

    NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo.t");
    NamespaceString bucketNs =
        NamespaceString::createNamespaceString_forTest("foo.system.buckets.t");

    // Set up targeter for the bucket collection.
    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.push_back(initTargeterFullRange(bucketNs, endpoint));
    auto bucketTargeter = static_cast<BulkWriteMockNSTargeter*>(targeters[0].get());
    bucketTargeter->setIsShardedTimeSeriesBucketsNamespace(true);

    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSON("x" << -1))},
                                    {NamespaceInfoEntry(ns)});

    BulkWriteOp bulkWriteOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    auto* batch = targeted[ShardId("shard")].get();
    auto childRequest = bulkWriteOp.buildBulkCommandRequest(targeters, *batch);

    // Test that we translate to bucket namespace and set the isTimeseriesNamespace flag.
    auto& nsInfoEntry = childRequest.getNsInfo()[0];
    ASSERT_EQUALS(nsInfoEntry.getIsTimeseriesNamespace(), true);
    ASSERT_EQUALS(nsInfoEntry.getNs(), bucketNs);
}


// Test BatchItemRef.getLet().
TEST_F(BulkWriteOpTest, BatchItemRefGetLet) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");

    // The content of the request (updateOp and Let) do not matter here,
    // only that BatchItemRef.getLet() matches BulkWriteCommandRequest.setLet().
    BulkWriteCommandRequest request({BulkWriteUpdateOp(0, BSON("x" << 1), BSON("x" << 2))},
                                    {NamespaceInfoEntry(nss)});

    BSONObj expected{BSON("key"
                          << "value")};
    request.setLet(expected);

    BulkWriteOp bulkWriteOp(_opCtx, request);
    const auto& letOption = bulkWriteOp.getWriteOp_forTest(0).getWriteItem().getLet();
    ASSERT(letOption.has_value());
    ASSERT_BSONOBJ_EQ(letOption.value(), expected);
}

using BulkOp =
    stdx::variant<mongo::BulkWriteInsertOp, mongo::BulkWriteUpdateOp, mongo::BulkWriteDeleteOp>;

BulkOp makeTestInsertOp(BSONObj document) {
    BulkWriteInsertOp op;
    op.setInsert(0);
    op.setDocument(document);
    return op;
}

BulkOp makeTestUpdateOp(BSONObj filter,
                        mongo::write_ops::UpdateModification updateMods,
                        mongo::OptionalBool upsertSupplied,
                        mongo::BSONObj hint,
                        boost::optional<std::vector<mongo::BSONObj>> arrayFilters,
                        boost::optional<mongo::BSONObj> constants,
                        boost::optional<mongo::BSONObj> collation) {
    BulkWriteUpdateOp op;
    op.setUpdate(0);
    op.setFilter(filter);
    op.setUpdateMods(updateMods);
    if (upsertSupplied.has_value()) {
        op.setUpsert(true);
        op.setUpsertSupplied(upsertSupplied);
    }
    op.setArrayFilters(arrayFilters);
    op.setHint(hint);
    op.setConstants(constants);
    op.setCollation(collation);
    return op;
}

BulkOp makeTestDeleteOp(BSONObj filter,
                        mongo::BSONObj hint,
                        boost::optional<mongo::BSONObj> collation) {
    BulkWriteDeleteOp op;
    op.setDeleteCommand(0);
    op.setFilter(filter);
    op.setHint(hint);
    op.setCollation(collation);
    return op;
}

int getSizeEstimate(BulkOp op) {
    // BatchItemRef can only be created from an underlying request, but the only field we care
    // about on the request is the ops. The other fields are necessary to satisfy invariants.
    BulkWriteCommandRequest dummyBulkRequest;
    dummyBulkRequest.setOps({op});
    dummyBulkRequest.setDbName(DatabaseName::kAdmin);
    dummyBulkRequest.setNsInfo({});
    return BatchItemRef(&dummyBulkRequest, 0).getSizeForBulkWriteBytes();
}

int getActualSize(BulkOp op) {
    return BulkWriteCRUDOp(op).toBSON().objsize();
}

// Test that we calculate accurate estimates for bulkWrite insert ops.
TEST_F(BulkWriteOpTest, TestBulkWriteInsertSizeEstimation) {
    auto basicInsert = makeTestInsertOp(fromjson("{x: 1}"));
    ASSERT_EQ(getSizeEstimate(basicInsert), getActualSize(basicInsert));

    auto largerInsert = makeTestInsertOp(fromjson("{x: 1, y: 'hello', z: {a: 1}}"));
    ASSERT_EQ(getSizeEstimate(largerInsert), getActualSize(largerInsert));
}

// Test that we calculate accurate estimates for bulkWrite update ops.
TEST_F(BulkWriteOpTest, TestBulkWriteUpdateSizeEstimation) {
    auto basicUpdate = makeTestUpdateOp(fromjson("{x: 1}") /* filter */,
                                        write_ops::UpdateModification(fromjson("{$set: {y: 1}}")),
                                        mongo::OptionalBool() /* upsertSupplied */,
                                        BSONObj() /* hint */,
                                        boost::none,
                                        boost::none,
                                        boost::none);
    ASSERT_EQ(getSizeEstimate(basicUpdate), getActualSize(basicUpdate));

    auto updateAllFieldsSetBesidesArrayFilters =
        makeTestUpdateOp(fromjson("{x: 1}") /* filter */,
                         write_ops::UpdateModification(fromjson("{$set: {y: 1}}")),
                         mongo::OptionalBool(true) /* upsertSupplied */,
                         fromjson("{a: 1}") /* hint */,
                         boost::none,
                         fromjson("{z: 1}") /* constants */,
                         fromjson("{locale: 'simple'}") /* collation */);
    ASSERT_EQ(getSizeEstimate(updateAllFieldsSetBesidesArrayFilters),
              getActualSize(updateAllFieldsSetBesidesArrayFilters));

    std::vector<BSONObj> arrayFilters = {fromjson("{j: 1}"), fromjson("{k: 1}")};
    auto updateAllFieldsSet =
        makeTestUpdateOp(fromjson("{x: 1}") /* filter */,
                         write_ops::UpdateModification(fromjson("{$set: {y: 1}}")),
                         mongo::OptionalBool(true) /* upsertSupplied */,
                         fromjson("{a: 1}") /* hint */,
                         arrayFilters,
                         fromjson("{z: 1}") /* constants */,
                         fromjson("{locale: 'simple'}") /* collation */);
    // We can't make an exact assertion when arrayFilters is set, because the way we estimate BSON
    // array index size overcounts for simplicity.
    ASSERT(getSizeEstimate(updateAllFieldsSet) > getActualSize(updateAllFieldsSet));

    std::vector<BSONObj> pipeline = {fromjson("{$set: {y: 1}}")};
    auto updateWithPipeline = makeTestUpdateOp(fromjson("{x: 1}") /* filter */,
                                               write_ops::UpdateModification(pipeline),
                                               mongo::OptionalBool() /* upsertSupplied */,
                                               BSONObj() /* hint */,
                                               boost::none,
                                               boost::none,
                                               boost::none);
    // We can't make an exact assertion when an update pipeline is used, because the way we estimate
    // BSON array index size overcounts for simplicity.
    ASSERT(getSizeEstimate(updateWithPipeline) > getActualSize(updateWithPipeline));
}

// Test that we calculate accurate estimates for bulkWrite delete ops.
TEST_F(BulkWriteOpTest, TestBulkWriteDeleteSizeEstimation) {
    auto basicDelete = makeTestDeleteOp(fromjson("{x: 1}"), BSONObj() /* hint */, boost::none);
    ASSERT_EQ(getSizeEstimate(basicDelete), getActualSize(basicDelete));

    auto deleteAllFieldsSet = makeTestDeleteOp(fromjson("{x: 1}") /* filter */,
                                               fromjson("{y: 1}") /* hint */,
                                               fromjson("{locale: 'simple'}") /* collation */);
    ASSERT_EQ(getSizeEstimate(deleteAllFieldsSet), getActualSize(deleteAllFieldsSet));
}

// Simulates a situation where we receive a bulkWrite request with large top-level fields (in this
// case, 'let') that is very close to MaxBSONObjInternalSize. Confirms that we factor in top-
// level fields when deciding when to split batches.
TEST_F(BulkWriteOpTest, TestBulkWriteBatchSplittingLargeBaseCommandSize) {
    ShardId shardId("shard");
    NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("foo.bar");
    NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("bar.foo");
    // Two different endpoints targeting the same shard for the two namespaces.
    ShardEndpoint endpoint0(
        shardId, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);
    ShardEndpoint endpoint1(
        shardId,
        ShardVersionFactory::make(ChunkVersion({OID::gen(), Timestamp(2)}, {10, 11}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.push_back(initTargeterFullRange(nss0, endpoint0));
    targeters.push_back(initTargeterFullRange(nss1, endpoint1));

    BulkWriteCommandRequest bigReq;

    // Create a ~15 MB let.
    auto giantLet = BSON("a" << std::string(15077000, 'a'));
    bigReq.setLet(giantLet);

    // Create a ~.1 MB document to insert.
    auto insertDoc = BSON("x" << 1 << "b" << std::string(100000, 'b'));
    std::vector<BulkOp> ops;
    for (auto i = 0; i < 17; i++) {
        auto op = BulkWriteInsertOp(i % 2, insertDoc);
        ops.push_back(op);
    }

    bigReq.setLet(giantLet);
    bigReq.setOps(ops);
    bigReq.setNsInfo({NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    bigReq.setDbName(DatabaseName::kAdmin);

    // Ensure we've built a request that's actual serialized size is slightly bigger than
    // BSONObjMaxUserSize,  which is the threshold we use to split batches. This should guarantee
    // that the estimated size we calculate for a sub-batch containing all of these writes
    // would also be bigger than BSONMaxUserObjSize and that we will split into multiple batches.
    ASSERT(bigReq.toBSON(BSONObj()).objsize() > BSONObjMaxUserSize);

    BulkWriteOp bulkWriteOp(_opCtx, bigReq);

    TargetedBatchMap targeted;
    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    // We shouldn't have targeted all of the writes yet.
    auto targetedSoFar = targeted.begin()->second->getWrites().size();
    ASSERT(targetedSoFar < bigReq.getOps().size());
    targeted.clear();

    ASSERT_OK(bulkWriteOp.target(targeters, false, targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    auto remainingTargeted = targeted.begin()->second->getWrites().size();
    // We should have been able to target all the remaining writes in a second batch.
    ASSERT_EQ(targetedSoFar + remainingTargeted, bigReq.getOps().size());
}

/**
 * Mimics a sharding backend to test BulkWriteExec.
 */
class BulkWriteExecTest : public ShardingTestFixture {
public:
    BulkWriteExecTest() = default;
    ~BulkWriteExecTest() = default;

    const ShardId kShardIdA = ShardId("shardA");
    const ShardId kShardIdB = ShardId("shardB");

    void setUp() override {
        ShardingTestFixture::setUp();
        configTargeter()->setFindHostReturnValue(HostAndPort("FakeConfigHost", 12345));

        std::vector<std::tuple<ShardId, HostAndPort>> remoteShards{
            {kShardIdA, HostAndPort(str::stream() << kShardIdA << ":123")},
            {kShardIdB, HostAndPort(str::stream() << kShardIdB << ":123")},
        };

        std::vector<ShardType> shards;

        for (size_t i = 0; i < remoteShards.size(); i++) {
            ShardType shardType;
            shardType.setName(std::get<0>(remoteShards[i]).toString());
            shardType.setHost(std::get<1>(remoteShards[i]).toString());

            shards.push_back(shardType);

            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(
                ConnectionString(std::get<1>(remoteShards[i])));
            targeter->setFindHostReturnValue(std::get<1>(remoteShards[i]));

            targeterFactory()->addTargeterToReturn(ConnectionString(std::get<1>(remoteShards[i])),
                                                   std::move(targeter));
        }

        setupShards(shards);
    }
};

TEST_F(BulkWriteExecTest, RefreshTargetersOnTargetErrors) {
    NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("foo.bar");
    NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("bar.foo");
    ShardEndpoint endpoint0(
        kShardIdA, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);
    ShardEndpoint endpoint1(
        kShardIdB,
        ShardVersionFactory::make(ChunkVersion({OID::gen(), Timestamp(2)}, {10, 11}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);

    std::vector<std::unique_ptr<NSTargeter>> targeters;
    // Initialize the targeter so that x >= 0 values are untargetable so target call will encounter
    // an error.
    targeters.push_back(initTargeterHalfRange(nss0, endpoint0));
    targeters.push_back(initTargeterFullRange(nss1, endpoint1));

    auto targeter0 = static_cast<BulkWriteMockNSTargeter*>(targeters[0].get());
    auto targeter1 = static_cast<BulkWriteMockNSTargeter*>(targeters[1].get());

    // Only the first op would get a target error.
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(0, BSON("x" << 1)), BulkWriteInsertOp(1, BSON("x" << 1))},
        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});

    LOGV2(7695300, "Sending an unordered request with untargetable first op and valid second op.");
    auto future = launchAsync([&] {
        // Test unordered operations. Since only the first op is untargetable, the second op will
        // succeed without errors. But bulk_write_exec::execute would retry on targeting errors and
        // try to refresh the targeters upon targeting errors.
        request.setOrdered(false);
        auto [replyItems, numErrors] =
            bulk_write_exec::execute(operationContext(), targeters, request);
        ASSERT_EQUALS(replyItems.size(), 2u);
        ASSERT_NOT_OK(replyItems[0].getStatus());
        ASSERT_OK(replyItems[1].getStatus());
        ASSERT_EQUALS(targeter0->getNumRefreshes(), 1);
        ASSERT_EQUALS(targeter1->getNumRefreshes(), 1);
        ASSERT_EQUALS(numErrors, 1);
    });

    // Mock a bulkWrite response to respond to the second op, which is valid.
    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        LOGV2(7695301,
              "Shard received a request, sending mock response.",
              "request"_attr = request.toString());
        BulkWriteCommandReply reply;
        reply.setCursor(BulkWriteCommandResponseCursor(
            0,  // cursorId
            std::vector<mongo::BulkWriteReplyItem>{BulkWriteReplyItem(0)}));
        reply.setNumErrors(0);
        return reply.toBSON();
    });
    future.default_timed_get();

    LOGV2(7695302, "Sending an ordered request with untargetable first op and valid second op.");
    // This time there is no need to mock a response because when the first op's targeting fails,
    // the entire operation is halted and so nothing is sent to the shards.
    future = launchAsync([&] {
        // Test ordered operations. This is mostly the same as the test case above except that we
        // should only return the first error for ordered operations.
        request.setOrdered(true);
        auto [replyItems, numErrors] =
            bulk_write_exec::execute(operationContext(), targeters, request);
        ASSERT_EQUALS(replyItems.size(), 1u);
        ASSERT_NOT_OK(replyItems[0].getStatus());
        // We should have another refresh attempt.
        ASSERT_EQUALS(targeter0->getNumRefreshes(), 2);
        ASSERT_EQUALS(targeter1->getNumRefreshes(), 2);
        ASSERT_EQUALS(numErrors, 1);
    });

    future.default_timed_get();
}

TEST_F(BulkWriteExecTest, CollectionDroppedBeforeRefreshingTargeters) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint(
        kShardIdA, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);

    // Mock targeter that throws StaleEpoch on refresh to mimic the collection being dropped.
    class StaleEpochMockNSTargeter : public MockNSTargeter {
    public:
        using MockNSTargeter::MockNSTargeter;

        bool refreshIfNeeded(OperationContext* opCtx) override {
            uasserted(ErrorCodes::StaleEpoch, "Mock StaleEpoch error");
        }
    };

    std::vector<std::unique_ptr<NSTargeter>> targeters;

    // Initialize the targeter so that x >= 0 values are untargetable so target call will encounter
    // an error.
    std::vector<MockRange> range{MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << 0))};
    targeters.push_back(std::make_unique<StaleEpochMockNSTargeter>(nss, std::move(range)));

    // The first op would get a target error.
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(0, BSON("x" << 1)), BulkWriteInsertOp(0, BSON("x" << -1))},
        {NamespaceInfoEntry(nss)});
    request.setOrdered(false);

    // After the targeting error from the first op, targeter refresh will throw a StaleEpoch
    // exception which should abort the entire bulkWrite.
    auto [replyItems, numErrors] = bulk_write_exec::execute(operationContext(), targeters, request);
    ASSERT_EQUALS(replyItems.size(), 2u);
    ASSERT_EQUALS(replyItems[0].getStatus().code(), ErrorCodes::StaleEpoch);
    ASSERT_EQUALS(replyItems[1].getStatus().code(), ErrorCodes::StaleEpoch);
    ASSERT_EQUALS(numErrors, 2);
}

// TODO(SERVER-72790): Test refreshing targeters on stale config errors, including the case where
// NoProgressMade is returned if stale config retry doesn't make any progress after
// kMaxRoundsWithoutProgress.

}  // namespace

}  // namespace mongo
