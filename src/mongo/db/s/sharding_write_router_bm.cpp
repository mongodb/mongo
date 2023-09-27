/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <benchmark/benchmark.h>
#include <boost/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/collection_sharding_state_factory_shard.h"
#include "mongo/db/s/collection_sharding_state_factory_standalone.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_write_router.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/catalog_cache_mock.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/index_version.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test", "foo");

class WriteRouterTestFixture : public benchmark::Fixture {
public:
    void firstSetup() {
        auto service = ServiceContext::make();
        auto serviceContext = service.get();
        setGlobalServiceContext(std::move(service));

        setupCatalogCacheMock(serviceContext, _withShardedCollection);
    }

    void lastTearDown() {
        setGlobalServiceContext({});
    }

    ServiceContext* serviceContext() {
        return getGlobalServiceContext();
    }

protected:
    void SetUp(benchmark::State& state) override {
        if (state.thread_index == 0) {
            firstSetup();
        }
    }

    void TearDown(benchmark::State& state) override {
        if (state.thread_index == 0) {
            lastTearDown();
        }
    }

    void setupCatalogCacheMock(ServiceContext* serviceContext, bool withShardedCollection) {
        const auto client = serviceContext->makeClient("test-setup");
        const auto opCtxHolder = client->makeOperationContext();
        OperationContext* opCtx = opCtxHolder.get();

        auto catalogCache = CatalogCacheMock::make();

        if (withShardedCollection) {
            const size_t nShards = 1;
            const uint32_t nChunks = 60;
            const auto clusterId = OID::gen();
            const auto shards = std::vector<ShardId>{ShardId("shard0")};
            const auto originatorShard = shards[0];

            const auto [chunks, chunkManager] = createChunks(nShards, nChunks, shards);

            ShardingState::get(serviceContext)->setInitialized(originatorShard, clusterId);

            CollectionShardingStateFactory::set(
                serviceContext,
                std::make_unique<CollectionShardingStateFactoryShard>(serviceContext));

            _shardVersion.emplace(ShardVersionFactory::make(
                chunkManager, originatorShard, boost::optional<CollectionIndexes>(boost::none)));

            OperationShardingState::setShardRole(
                opCtx, kNss, _shardVersion, boost::none /* databaseVersion */);

            // Configuring the filtering metadata such that calls to getCollectionDescription
            // what we want. Specifically the reshardingFields are what we use. Its specified by
            // the chunkManager.
            Lock::DBLock dbLock{opCtx, kNss.dbName(), MODE_IX};
            Lock::CollectionLock collLock{opCtx, kNss, MODE_IX};
            CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, kNss)
                ->setFilteringMetadata(opCtx, CollectionMetadata(chunkManager, originatorShard));

            catalogCache->setChunkManagerReturnValue(chunkManager);
        }

        auto mockNetwork = std::make_unique<executor::NetworkInterfaceMock>();

        auto const grid = Grid::get(opCtx);
        grid->init(std::make_unique<ShardingCatalogClientImpl>(nullptr),
                   std::move(catalogCache),
                   std::make_unique<ShardRegistry>(serviceContext, nullptr, boost::none),
                   std::make_unique<ClusterCursorManager>(serviceContext->getPreciseClockSource()),
                   std::make_unique<BalancerConfiguration>(),
                   std::make_unique<executor::TaskExecutorPool>(),
                   mockNetwork.get());

        // Note: mockNetwork in Grid will become a dangling pointer after this function, this
        // is fine since the test shouldn't be using it.
    }

    ShardId pessimalShardSelector(int i, int nShards, int nChunks) {
        return ShardId(str::stream() << "shard" << (i % nShards));
    }

    ChunkRange getRangeForChunk(int i, int nChunks) {
        invariant(i >= 0);
        invariant(nChunks > 0);
        invariant(i < nChunks);
        if (i == 0) {
            return {BSON("_id" << MINKEY), BSON("_id" << 0)};
        }
        if (i + 1 == nChunks) {
            return {BSON("_id" << (i - 1) * 100), BSON("_id" << MAXKEY)};
        }
        return {BSON("_id" << (i - 1) * 100), BSON("_id" << i * 100)};
    }

    RoutingTableHistoryValueHandle makeStandaloneRoutingTableHistory(RoutingTableHistory rt) {
        const auto version = rt.getVersion();
        return RoutingTableHistoryValueHandle(
            std::make_shared<RoutingTableHistory>(std::move(rt)),
            ComparableChunkVersion::makeComparableChunkVersion(version));
    }

    std::pair<std::vector<mongo::ChunkType>, mongo::ChunkManager> createChunks(
        size_t nShards, uint32_t nChunks, std::vector<ShardId> shards) {
        invariant(shards.size() == nShards);

        const auto collIdentifier = UUID::gen();
        const auto shardKeyPattern = KeyPattern(BSON("_id" << 1));
        const auto reshardKeyPattern = KeyPattern(BSON("y" << 1));
        const auto collEpoch = OID::gen();
        const auto collTimestamp = Timestamp(100, 5);
        const auto tempNss = NamespaceString::createNamespaceString_forTest(
            kNss.db_forSharding(),
            fmt::format("{}{}",
                        NamespaceString::kTemporaryReshardingCollectionPrefix,
                        collIdentifier.toString()));

        std::vector<ChunkType> chunks;
        chunks.reserve(nChunks);

        for (uint32_t i = 0; i < nChunks; ++i) {
            chunks.emplace_back(collIdentifier,
                                getRangeForChunk(i, nChunks),
                                ChunkVersion({collEpoch, collTimestamp}, {i + 1, 0}),
                                pessimalShardSelector(i, nShards, nChunks));
        }

        TypeCollectionReshardingFields reshardingFields{UUID::gen()};
        reshardingFields.setState(CoordinatorStateEnum::kPreparingToDonate);
        // ShardingWriteRouter is only meant to be used by the donor.
        reshardingFields.setDonorFields(
            TypeCollectionDonorFields{tempNss, reshardKeyPattern, shards});

        ChunkManager cm(shards[0],
                        DatabaseVersion(UUID::gen(), Timestamp(1, 0)),
                        makeStandaloneRoutingTableHistory(
                            RoutingTableHistory::makeNew(kNss,
                                                         collIdentifier,
                                                         shardKeyPattern,
                                                         false, /* unsplittable */
                                                         nullptr,
                                                         false,
                                                         collEpoch,
                                                         collTimestamp,
                                                         boost::none /* timeseriesFields */,
                                                         reshardingFields, /* reshardingFields */
                                                         true,
                                                         chunks)),
                        boost::none);

        return std::make_pair(chunks, cm);
    }

protected:
    bool _withShardedCollection{false};
    boost::optional<ShardVersion> _shardVersion;
};

class ShardingWriteRouterTestFixture : public WriteRouterTestFixture {
public:
    ShardingWriteRouterTestFixture() {
        _withShardedCollection = true;
    }
};

BENCHMARK_DEFINE_F(ShardingWriteRouterTestFixture, BM_InsertGetDestinedRecipient)
(benchmark::State& state) {
    // ShardingWriteRouter currently requires the ShardServer cluster role.
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;

    for (auto keepRunning : state) {
        benchmark::ClobberMemory();

        const auto client = serviceContext()->makeClient("test");
        const auto opCtx = client->makeOperationContext();

        OperationShardingState::setShardRole(
            opCtx.get(), kNss, _shardVersion, boost::none /* databaseVersion */);

        Lock::DBLock dbLock{opCtx.get(), kNss.dbName(), MODE_IX};
        Lock::CollectionLock collLock{opCtx.get(), kNss, MODE_IX};

        ShardingWriteRouter writeRouter(opCtx.get(), kNss);
        auto shardId = writeRouter.getReshardingDestinedRecipient(BSON("_id" << 0));
        ASSERT(shardId != boost::none);
    }
}

BENCHMARK_DEFINE_F(ShardingWriteRouterTestFixture, BM_UpdateGetDestinedRecipient)
(benchmark::State& state) {
    // ShardingWriteRouter currently requires the ShardServer cluster role.
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;

    for (auto keepRunning : state) {
        benchmark::ClobberMemory();

        const auto client = serviceContext()->makeClient("test");
        const auto opCtx = client->makeOperationContext();

        OperationShardingState::setShardRole(
            opCtx.get(), kNss, _shardVersion, boost::none /* databaseVersion */);

        Lock::DBLock dbLock{opCtx.get(), kNss.dbName(), MODE_IX};
        Lock::CollectionLock collLock{opCtx.get(), kNss, MODE_IX};

        ShardingWriteRouter writeRouter(opCtx.get(), kNss);
        auto shardId = writeRouter.getReshardingDestinedRecipient(BSON("_id" << 0));
        ASSERT(shardId != boost::none);
    }
}

BENCHMARK_DEFINE_F(WriteRouterTestFixture, BM_UnshardedDestinedRecipient)
(benchmark::State& state) {
    serverGlobalParams.clusterRole = ClusterRole::None;

    for (auto keepRunning : state) {
        benchmark::ClobberMemory();

        const auto client = serviceContext()->makeClient("test");
        const auto opCtx = client->makeOperationContext();

        Lock::DBLock dbLock{opCtx.get(), kNss.dbName(), MODE_IX};
        Lock::CollectionLock collLock{opCtx.get(), kNss, MODE_IX};

        ShardingWriteRouter writeRouter(opCtx.get(), kNss);
        auto shardId = writeRouter.getReshardingDestinedRecipient(BSON("_id" << 0));
        ASSERT(shardId == boost::none);
    }
}

BENCHMARK_REGISTER_F(ShardingWriteRouterTestFixture, BM_InsertGetDestinedRecipient)
    ->Range(1, 1 << 4)
    ->ThreadRange(1, ProcessInfo::getNumAvailableCores());

BENCHMARK_REGISTER_F(ShardingWriteRouterTestFixture, BM_UpdateGetDestinedRecipient)
    ->Range(1, 1 << 4)
    ->ThreadRange(1, ProcessInfo::getNumAvailableCores());

BENCHMARK_REGISTER_F(WriteRouterTestFixture, BM_UnshardedDestinedRecipient)
    ->Range(1, 1 << 4)
    ->ThreadRange(1, ProcessInfo::getNumAvailableCores());

}  // namespace
}  // namespace mongo
