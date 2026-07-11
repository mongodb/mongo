// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Unit tests for ShardingWriteRouter's integration with LocalReshardingOperationsRegistry.
 *
 * Validates that ShardingWriteRouter correctly uses the registry to determine the
 * destined recipient shard during resharding, rather than relying on reshardingFields
 * in CollectionMetadata.
 */

#include "mongo/db/s/resharding/sharding_write_router.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client_impl.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/router_role/routing_cache/catalog_cache_mock.h"
#include "mongo/db/s/resharding/local_resharding_operations_registry.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state_factory_shard.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state_factory_mock.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_test_fixture_common.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test", "foo");
const ShardHandle kDonorShardHandle(ShardId("shard0"), UUID::gen());

class ShardingWriteRouterRegistryTest : public unittest::Test {
public:
    void setUp() override {
        auto service = ServiceContext::make();
        _serviceContext = service.get();
        setGlobalServiceContext(std::move(service));

        // (Generic FCV reference): Test latest FCV behavior.
        serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);

        _savedClusterRole = serverGlobalParams.clusterRole;
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;

        _sourceUUID = UUID::gen();
        _reshardingUUID = UUID::gen();
        _tempReshardingNss = NamespaceString::createNamespaceString_forTest(
            kNss.db_forSharding(),
            fmt::format("{}{}",
                        NamespaceString::kTemporaryReshardingCollectionPrefix,
                        _sourceUUID.toString()));

        const auto clusterId = OID::gen();
        const HostAndPort kConfigHostAndPort{"DummyConfig", 12345};
        ShardingState::create(_serviceContext);
        CollectionShardingStateFactory::set(
            _serviceContext,
            std::make_unique<CollectionShardingStateFactoryShard>(_serviceContext));
        DatabaseShardingStateFactory::set(_serviceContext,
                                          std::make_unique<DatabaseShardingStateFactoryMock>());

        ShardingState::get(_serviceContext)
            ->setRecoveryCompleted({clusterId,
                                    ClusterRole::ShardServer,
                                    ConnectionString(kConfigHostAndPort),
                                    kDonorShardHandle});

        auto [chunks, chunkManager] = createSourceChunkManager();
        _shardVersion = ShardVersionFactory::make(chunkManager, kDonorShardHandle.name());

        {
            const auto client = _serviceContext->getService()->makeClient("test-setup-metadata");
            const auto opCtx = client->makeOperationContext();
            OperationShardingState::setShardRole(
                opCtx.get(), kNss, _shardVersion, boost::none /* databaseVersion */);

            CollectionShardingRuntime::acquireExclusive(opCtx.get(), kNss)
                ->setCollectionMetadata(opCtx.get(),
                                        CollectionMetadata(chunkManager, kDonorShardHandle.name()));
        }

        {
            const auto client = _serviceContext->getService()->makeClient("test-setup-grid");
            const auto opCtx = client->makeOperationContext();

            auto catalogCache = CatalogCacheMock::make();
            catalogCache->setCollectionReturnValue(
                _tempReshardingNss,
                CatalogCacheMock::makeCollectionRoutingInfoSharded(
                    _tempReshardingNss,
                    kDonorShardHandle.name(),
                    DatabaseVersion(),
                    BSON("y" << 1),
                    {{ChunkRange(BSON("y" << MINKEY), BSON("y" << MAXKEY)),
                      kDonorShardHandle.name()}}));

            auto mockNetwork = std::make_unique<executor::NetworkInterfaceMock>();
            auto const grid = Grid::get(opCtx.get());
            grid->init(
                std::make_unique<ShardingCatalogClientImpl>(nullptr),
                std::move(catalogCache),
                std::make_unique<ShardRegistry>(_serviceContext, nullptr, boost::none),
                std::make_unique<ClusterCursorManager>(_serviceContext->getPreciseClockSource()),
                std::make_unique<BalancerConfiguration>(),
                std::make_unique<executor::TaskExecutorPool>(),
                mockNetwork.get());
        }
    }

    void tearDown() override {
        serverGlobalParams.clusterRole = _savedClusterRole;
        setGlobalServiceContext({});
    }

    CommonReshardingMetadata makeMetadata() {
        return CommonReshardingMetadata(
            _reshardingUUID, kNss, _sourceUUID, _tempReshardingNss, BSON("y" << 1));
    }

    ShardingWriteRouter makeRouter() {
        const auto client = _serviceContext->getService()->makeClient("test-make-router");
        const auto opCtx = client->makeOperationContext();
        OperationShardingState::setShardRole(
            opCtx.get(), kNss, _shardVersion, boost::none /* databaseVersion */);

        Lock::DBLock dbLock{opCtx.get(), kNss.dbName(), MODE_IX};
        Lock::CollectionLock collLock{opCtx.get(), kNss, MODE_IX};
        return ShardingWriteRouter(opCtx.get(), kNss);
    }

protected:
    std::pair<std::vector<ChunkType>, CurrentChunkManager> createSourceChunkManager() {
        const auto shardKeyPattern = KeyPattern(BSON("_id" << 1));
        const auto collEpoch = OID::gen();
        const auto collTimestamp = Timestamp(100, 5);

        std::vector<ChunkType> chunks;
        chunks.emplace_back(_sourceUUID,
                            ChunkRange(BSON("_id" << MINKEY), BSON("_id" << MAXKEY)),
                            ChunkVersion({collEpoch, collTimestamp}, {1, 0}),
                            kDonorShardHandle.name());

        CurrentChunkManager cm(ShardingTestFixtureCommon::makeStandaloneRoutingTableHistory(
            RoutingTableHistory::makeNew(kNss,
                                         _sourceUUID,
                                         shardKeyPattern,
                                         false, /* unsplittable */
                                         nullptr,
                                         false,
                                         collEpoch,
                                         collTimestamp,
                                         boost::none /* timeseriesFields */,
                                         boost::none /* reshardingFields */,
                                         true,
                                         chunks)));

        return std::make_pair(chunks, cm);
    }

    ServiceContext* _serviceContext;
    UUID _sourceUUID = UUID::gen();
    UUID _reshardingUUID = UUID::gen();
    NamespaceString _tempReshardingNss;
    boost::optional<ShardVersion> _shardVersion;
    ClusterRole _savedClusterRole{ClusterRole::None};
};

TEST_F(ShardingWriteRouterRegistryTest, NoReshardingOperationReturnsNone) {
    auto router = makeRouter();
    auto result = router.getReshardingDestinedRecipient(BSON("_id" << 0 << "y" << 5));
    ASSERT_FALSE(result.has_value());
}

TEST_F(ShardingWriteRouterRegistryTest, OnlyRecipientRoleReturnsNone) {
    auto& registry = LocalReshardingOperationsRegistry::get();
    auto metadata = makeMetadata();
    registry.registerOperation(LocalReshardingOperationsRegistry::Role::kRecipient, metadata);

    auto router = makeRouter();
    auto result = router.getReshardingDestinedRecipient(BSON("_id" << 0 << "y" << 5));
    ASSERT_FALSE(result.has_value());
}

TEST_F(ShardingWriteRouterRegistryTest, OnlyCoordinatorRoleReturnsNone) {
    auto& registry = LocalReshardingOperationsRegistry::get();
    auto metadata = makeMetadata();
    registry.registerOperation(LocalReshardingOperationsRegistry::Role::kCoordinator, metadata);

    auto router = makeRouter();
    auto result = router.getReshardingDestinedRecipient(BSON("_id" << 0 << "y" << 5));
    ASSERT_FALSE(result.has_value());
}

TEST_F(ShardingWriteRouterRegistryTest, DonorRoleRegisteredReturnsShardId) {
    auto& registry = LocalReshardingOperationsRegistry::get();
    auto metadata = makeMetadata();
    registry.registerOperation(LocalReshardingOperationsRegistry::Role::kDonor, metadata);

    auto router = makeRouter();
    auto result = router.getReshardingDestinedRecipient(BSON("_id" << 0 << "y" << 5));
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(*result, kDonorShardHandle.name());
}

TEST_F(ShardingWriteRouterRegistryTest, DonorAndRecipientRolesReturnsShardId) {
    auto& registry = LocalReshardingOperationsRegistry::get();
    auto metadata = makeMetadata();
    registry.registerOperation(LocalReshardingOperationsRegistry::Role::kDonor, metadata);
    registry.registerOperation(LocalReshardingOperationsRegistry::Role::kRecipient, metadata);

    auto router = makeRouter();
    auto result = router.getReshardingDestinedRecipient(BSON("_id" << 0 << "y" << 5));
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(*result, kDonorShardHandle.name());
}

TEST_F(ShardingWriteRouterRegistryTest, OperationUnregisteredReturnsNone) {
    auto& registry = LocalReshardingOperationsRegistry::get();
    auto metadata = makeMetadata();
    registry.registerOperation(LocalReshardingOperationsRegistry::Role::kDonor, metadata);
    registry.unregisterOperation(LocalReshardingOperationsRegistry::Role::kDonor, metadata);

    auto router = makeRouter();
    auto result = router.getReshardingDestinedRecipient(BSON("_id" << 0 << "y" << 5));
    ASSERT_FALSE(result.has_value());
}

TEST_F(ShardingWriteRouterRegistryTest, DifferentNamespaceReturnsNone) {
    auto& registry = LocalReshardingOperationsRegistry::get();
    auto metadata = makeMetadata();
    registry.registerOperation(LocalReshardingOperationsRegistry::Role::kDonor, metadata);

    {
        auto router = makeRouter();
        auto result = router.getReshardingDestinedRecipient(BSON("_id" << 0 << "y" << 5));
        ASSERT_TRUE(result.has_value());
    }

    {
        const auto otherNss =
            NamespaceString::createNamespaceString_forTest("test", "other_collection");
        const auto client = _serviceContext->getService()->makeClient("test-other-ns");
        const auto opCtx = client->makeOperationContext();
        Lock::DBLock dbLock{opCtx.get(), otherNss.dbName(), MODE_IX};
        Lock::CollectionLock collLock{opCtx.get(), otherNss, MODE_IX};
        ShardingWriteRouter router(opCtx.get(), otherNss);

        auto result = router.getReshardingDestinedRecipient(BSON("_id" << 0 << "y" << 5));
        ASSERT_FALSE(result.has_value());
    }
}

TEST_F(ShardingWriteRouterRegistryTest, NotShardServerReturnsNone) {
    serverGlobalParams.clusterRole = ClusterRole::None;

    const auto client = _serviceContext->getService()->makeClient("test-non-shard");
    const auto opCtx = client->makeOperationContext();
    ShardingWriteRouter router(opCtx.get(), kNss);

    auto result = router.getReshardingDestinedRecipient(BSON("_id" << 0 << "y" << 5));
    ASSERT_FALSE(result.has_value());

    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
}

TEST_F(ShardingWriteRouterRegistryTest, CollDescHasNoRoutingTableReturnsNone) {
    const auto untrackedNss =
        NamespaceString::createNamespaceString_forTest("test", "unsharded_coll");

    // Set up CSR for the untracked namespace with empty metadata i.e. no routing table.
    {
        const auto client = _serviceContext->getService()->makeClient("test-setup-unsharded");
        const auto opCtx = client->makeOperationContext();
        CollectionShardingRuntime::acquireExclusive(opCtx.get(), untrackedNss)
            ->setCollectionMetadata(opCtx.get(), CollectionMetadata());
    }

    const auto client = _serviceContext->getService()->makeClient("test-unsharded-router");
    const auto opCtx = client->makeOperationContext();
    Lock::DBLock dbLock{opCtx.get(), untrackedNss.dbName(), MODE_IX};
    Lock::CollectionLock collLock{opCtx.get(), untrackedNss, MODE_IX};
    ShardingWriteRouter router(opCtx.get(), untrackedNss);

    auto result = router.getReshardingDestinedRecipient(BSON("_id" << 0 << "y" << 5));
    ASSERT_FALSE(result.has_value());
}

}  // namespace
}  // namespace mongo

