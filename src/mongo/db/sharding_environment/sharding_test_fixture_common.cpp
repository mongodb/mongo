// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/sharding_environment/sharding_test_fixture_common.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/shard_role/resource_yielders.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_shared_state_cache.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <chrono>
#include <cstddef>

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

using executor::NetworkTestEnv;

ShardingTestFixtureCommon::ShardingTestFixtureCommon(
    std::unique_ptr<ScopedGlobalServiceContextForTest> scopedGlobalContext)
    : ServiceContextTest(std::move(scopedGlobalContext)) {}

ShardingTestFixtureCommon::~ShardingTestFixtureCommon() {
    invariant(!_opCtxHolder,
              "ShardingTestFixtureCommon::tearDown() must have been called before destruction");
}

void ShardingTestFixtureCommon::setUp() {
    _opCtxHolder = makeOperationContext();

    ResourceYielderFactory::initialize(getServiceContext());
}

void ShardingTestFixtureCommon::tearDown() {
    _opCtxHolder.reset();
}

void ShardingTestFixtureCommon::shutdownExecutorPool() {
    auto grid = Grid::get(getServiceContext());

    if (!grid || !grid->isInitialized() || !grid->getExecutorPool() || _executorPoolShutDown) {
        return;
    }
    _executorPoolShutDown = true;

    grid->getExecutorPool()->shutdown_forTest();
    for (auto mockNet : {&_mockNetwork, &_mockNetworkForPool}) {
        executor::NetworkInterfaceMock::InNetworkGuard(*mockNet)
            ->drainUnfinishedNetworkOperations();
    }
    grid->getExecutorPool()->join_forTest();
}

OperationContext* ShardingTestFixtureCommon::operationContext() const {
    invariant(_opCtxHolder,
              "ShardingTestFixtureCommon::setUp() must have been called before this method");
    return _opCtxHolder.get();
}

ShardRegistry* ShardingTestFixtureCommon::shardRegistry() const {
    return Grid::get(operationContext())->shardRegistry();
}

std::shared_ptr<ShardSharedStateCache::State> ShardingTestFixtureCommon::getShardState(
    const ShardId& shardId) const {
    return ShardSharedStateCache::get(operationContext()).getShardState(shardId);
}

RoutingTableHistoryValueHandle ShardingTestFixtureCommon::makeStandaloneRoutingTableHistory(
    RoutingTableHistory rt) {
    const auto version = rt.getVersion();
    return RoutingTableHistoryValueHandle(
        std::make_shared<RoutingTableHistory>(std::move(rt)),
        ComparableChunkVersion::makeComparableChunkVersion(version));
}

void ShardingTestFixtureCommon::onCommand(NetworkTestEnv::OnCommandFunction func) {
    _networkTestEnv->onCommand(func);
}

void ShardingTestFixtureCommon::onCommands(
    std::vector<executor::NetworkTestEnv::OnCommandFunction> funcs) {
    _networkTestEnv->onCommands(std::move(funcs));
}

void ShardingTestFixtureCommon::onCommandWithMetadata(
    NetworkTestEnv::OnCommandWithMetadataFunction func) {
    _networkTestEnv->onCommandWithMetadata(func);
}

void ShardingTestFixtureCommon::onFindCommand(NetworkTestEnv::OnFindCommandFunction func) {
    _networkTestEnv->onFindCommand(func);
}

void ShardingTestFixtureCommon::onFindWithMetadataCommand(
    NetworkTestEnv::OnFindCommandWithMetadataFunction func) {
    _networkTestEnv->onFindWithMetadataCommand(func);
}


ClockSourceMock* ShardingTestFixtureCommon::clockSource() const {
    const auto clockSource =
        dynamic_cast<ClockSourceMock*>(getServiceContext()->getPreciseClockSource());
    ASSERT_NE(clockSource, nullptr);
    return clockSource;
}

Milliseconds ShardingTestFixtureCommon::advanceUntilReadyRequest() const {
    using namespace std::literals;
    const auto opCtx = operationContext();

    std::this_thread::sleep_for(1ms);
    auto totalWaited = Milliseconds{0};

    // We must release the InNetworkGuard on each iteration to allow the executor
    // thread to run callbacks. The NetworkInterfaceMock uses a cooperative multitasking model
    // where only one thread (network OR executor) can be running at a time.
    while (true) {
        {
            auto guard = executor::NetworkInterfaceMock::InNetworkGuard{_mockNetwork};
            if (_mockNetwork->hasReadyRequests()) {
                return totalWaited;
            }
            opCtx->checkForInterrupt();
            auto advance = Milliseconds{10};
            clockSource()->advance(advance);
            totalWaited += advance;
        }  // Guard released here, allowing executor thread to run callbacks

        std::this_thread::sleep_for(100us);
    }
}

void ShardingTestFixtureCommon::addRemoteShards(
    const std::vector<std::tuple<ShardId, HostAndPort>>& shardInfos) {
    std::vector<ShardType> shards;

    for (auto shard : shardInfos) {
        ShardType shardType;
        shardType.setHandle(ShardHandle(ShardId(std::get<0>(shard).toString()), boost::none));
        shardType.setHost(std::get<1>(shard).toString());
        shards.push_back(shardType);

        std::unique_ptr<RemoteCommandTargeterMock> targeter(
            std::make_unique<RemoteCommandTargeterMock>());
        targeter->setConnectionStringReturnValue(ConnectionString(std::get<1>(shard)));
        targeter->setFindHostReturnValue(std::get<1>(shard));

        targeterFactory()->addTargeterToReturn(ConnectionString(std::get<1>(shard)),
                                               std::move(targeter));
    }

    auto future = launchAsync([this] { shardRegistry()->reload(operationContext()); });

    onFindWithMetadataCommand([this, &shards](const executor::RemoteCommandRequest& request) {
        const NamespaceString nss = NamespaceString::createNamespaceString_forTest(
            request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss, NamespaceString::kConfigsvrShardsNamespace);

        // If there is no '$db', append it.
        auto cmd = static_cast<OpMsgRequest>(request).body;
        auto query = query_request_helper::makeFromFindCommandForTests(cmd, nss);

        ASSERT_EQ(query->getNamespaceOrUUID().nss(), NamespaceString::kConfigsvrShardsNamespace);
        ASSERT_BSONOBJ_EQ(query->getFilter(), BSONObj());
        ASSERT_BSONOBJ_EQ(query->getSort(), BSONObj());
        ASSERT_FALSE(query->getLimit().has_value());

        std::vector<BSONObj> shardsToReturn;

        Timestamp maxTopologyTime;
        std::transform(shards.begin(),
                       shards.end(),
                       std::back_inserter(shardsToReturn),
                       [&maxTopologyTime](const ShardType& shard) {
                           maxTopologyTime = std::max(shard.getTopologyTime(), maxTopologyTime);
                           return shard.toBSON();
                       });

        BSONObjBuilder bob;
        bob.append(VectorClock::kTopologyTimeFieldName, maxTopologyTime);

        return std::make_tuple(shardsToReturn, bob.obj());
    });

    future.default_timed_get();
}

}  // namespace mongo
