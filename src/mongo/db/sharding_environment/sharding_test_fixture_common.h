// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace mongo {

/**
 * Contains common functionality and tools, which apply to both mongos and mongod unit-tests.
 */
class [[MONGO_MOD_OPEN]] ShardingTestFixtureCommon : public ServiceContextTest {
public:
    /**
     * Constructs a standalone RoutingTableHistory object (i.e., not linked to any CatalogCache),
     * which can be used to pass to ChunkManager for tests, which specifically target the behaviour
     * of the ChunkManager.
     */
    static RoutingTableHistoryValueHandle makeStandaloneRoutingTableHistory(RoutingTableHistory rt);

protected:
    explicit ShardingTestFixtureCommon(
        std::unique_ptr<ScopedGlobalServiceContextForTest> scopedGlobalContext);
    ~ShardingTestFixtureCommon() override;

    void setUp() override;
    void tearDown() override;

    /**
     * Shuts down the TaskExecutorPool and remembers that it has been shut down, so that it is not
     * shut down again on tearDown.
     *
     * Not safe to call from multiple threads.
     */
    void shutdownExecutorPool();

    /**
     * Waits until there's a ready request in the mock network. While waiting, advances the mock
     * clock.
     */
    Milliseconds advanceUntilReadyRequest() const;

    ClockSourceMock* clockSource() const;

    OperationContext* operationContext() const;

    ShardRegistry* shardRegistry() const;

    std::shared_ptr<ShardSharedStateCache::State> getShardState(const ShardId& shardId) const;

    /**
     * Returns the NetworkInterfaceMock associated with the fixed TaskExecutor.
     */
    executor::NetworkInterfaceMock* network() const {
        invariant(_mockNetwork);
        return _mockNetwork;
    }

    /**
     * Returns the NetworkInterfaceMock associated with the TaskExecutorPool's executors.
     */
    executor::NetworkInterfaceMock* networkForPool() const {
        invariant(_mockNetworkForPool);
        return _mockNetworkForPool;
    }

    RemoteCommandTargeterFactoryMock* targeterFactory() const {
        invariant(_targeterFactory);
        return _targeterFactory;
    }

    template <typename Lambda>
    executor::NetworkTestEnv::FutureHandle<typename std::invoke_result<Lambda>::type> launchAsync(
        Lambda&& func) const {
        return _networkTestEnv->launchAsync(std::forward<Lambda>(func));
    }

    /**
     * Blocking methods, which receive one message from the network and respond using the responses
     * returned from the input function. This is a syntactic sugar for simple, single request +
     * response or find tests.
     */
    void onCommand(executor::NetworkTestEnv::OnCommandFunction func);
    void onCommands(std::vector<executor::NetworkTestEnv::OnCommandFunction> funcs);
    void onCommandWithMetadata(executor::NetworkTestEnv::OnCommandWithMetadataFunction func);
    void onFindCommand(executor::NetworkTestEnv::OnFindCommandFunction func);
    void onFindWithMetadataCommand(
        executor::NetworkTestEnv::OnFindCommandWithMetadataFunction func);

    virtual std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() {
        return nullptr;
    }

    /**
     * Adds ShardRemote shards to the shard registry.
     */
    void addRemoteShards(const std::vector<std::tuple<ShardId, HostAndPort>>& shards);

    // Since a NetworkInterface is a private member of a TaskExecutor, we store a raw pointer to the
    // fixed and arbitrary TaskExecutors NetworkInterfaces here.
    //
    // TODO(Esha): Currently, some fine-grained synchronization of the network and task executor is
    // outside of NetworkTestEnv's capabilities. If all control of the network is done through
    // _networkTestEnv, storing these raw pointers is not necessary.
    executor::NetworkInterfaceMock* _mockNetwork{nullptr};
    executor::NetworkInterfaceMock* _mockNetworkForPool{nullptr};

    // Allows for processing tasks through the NetworkInterfaceMock/ThreadPoolMock subsystem
    std::unique_ptr<executor::NetworkTestEnv> _networkTestEnv;

    // Records if a component has been shut down, so that it is only shut down once.
    bool _executorPoolShutDown = false;

    // Since the RemoteCommandTargeterFactory is currently a private member of ShardFactory, we
    // store a raw pointer to it here.
    RemoteCommandTargeterFactoryMock* _targeterFactory = nullptr;

private:
    // Keeps the lifetime of the operation context
    ServiceContext::UniqueOperationContext _opCtxHolder;
};

}  // namespace mongo
