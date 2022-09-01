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
#include <cstdint>
#include <utility>
#include <vector>

#include "mongo/db/concurrency/locker_noop_client_observer.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/collection_sharding_state_factory_shard.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_write_router.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/random.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/catalog_cache_loader_mock.h"
#include "mongo/s/catalog_cache_mock.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

const NamespaceString kNss("test", "foo");

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
    const auto tempNss =
        NamespaceString(kNss.db(),
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
    reshardingFields.setDonorFields(TypeCollectionDonorFields{tempNss, reshardKeyPattern, shards});

    ChunkManager cm(shards[0],
                    DatabaseVersion(UUID::gen(), Timestamp(1, 0)),
                    makeStandaloneRoutingTableHistory(
                        RoutingTableHistory::makeNew(kNss,
                                                     collIdentifier,
                                                     shardKeyPattern,
                                                     nullptr,
                                                     false,
                                                     collEpoch,
                                                     collTimestamp,
                                                     boost::none /* timeseriesFields */,
                                                     reshardingFields, /* reshardingFields */
                                                     boost::none /* chunkSizeBytes */,
                                                     true,
                                                     chunks)),
                    boost::none);

    return std::make_pair(chunks, cm);
}

std::unique_ptr<CatalogCacheMock> createCatalogCacheMock(OperationContext* opCtx) {
    const size_t nShards = 1;
    const uint32_t nChunks = 60;
    const auto clusterId = OID::gen();
    const auto shards = std::vector<ShardId>{ShardId("shard0")};
    const auto originatorShard = shards[0];

    const auto [chunks, chunkManager] = createChunks(nShards, nChunks, shards);

    ShardingState::get(opCtx->getServiceContext())->setInitialized(originatorShard, clusterId);

    CollectionShardingStateFactory::set(
        opCtx->getServiceContext(),
        std::make_unique<CollectionShardingStateFactoryShard>(opCtx->getServiceContext()));

    const ChunkVersion placementVersion = chunkManager.getVersion(originatorShard);
    OperationShardingState::setShardRole(
        opCtx,
        kNss,
        ShardVersion(placementVersion,
                     CollectionIndexes(placementVersion, boost::none)) /* shardVersion */,
        boost::none /* databaseVersion */);

    // Configuring the filtering metadata such that calls to getCollectionDescription return what we
    // want. Specifically the reshardingFields are what we use. Its specified by the chunkManager.
    CollectionShardingRuntime::get(opCtx, kNss)
        ->setFilteringMetadata(opCtx, CollectionMetadata(chunkManager, originatorShard));

    auto catalogCache = CatalogCacheMock::make();
    catalogCache->setChunkManagerReturnValue(chunkManager);

    return catalogCache;
}

void BM_InsertGetDestinedRecipient(benchmark::State& state) {
    // ShardingWriteRouter currently requires the ShardServer cluster role.
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;

    auto serviceContext = ServiceContext::make();
    const auto client = serviceContext->makeClient("test");
    serviceContext->registerClientObserver(std::make_unique<LockerNoopClientObserver>());
    const auto opCtx = client->makeOperationContext();

    const auto catalogCache = createCatalogCacheMock(opCtx.get());

    ShardingWriteRouter writeRouter(opCtx.get(), kNss, catalogCache.get());

    for (auto keepRunning : state) {
        benchmark::ClobberMemory();
        auto shardId = writeRouter.getReshardingDestinedRecipient(BSON("_id" << 0));
        ASSERT(shardId != boost::none);
    }
}

void BM_UpdateGetDestinedRecipient(benchmark::State& state) {
    // ShardingWriteRouter currently requires the ShardServer cluster role.
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;

    auto serviceContext = ServiceContext::make();
    const auto client = serviceContext->makeClient("test");
    serviceContext->registerClientObserver(std::make_unique<LockerNoopClientObserver>());
    const auto opCtx = client->makeOperationContext();

    const auto catalogCache = createCatalogCacheMock(opCtx.get());

    for (auto keepRunning : state) {
        benchmark::ClobberMemory();
        ShardingWriteRouter writeRouter(opCtx.get(), kNss, catalogCache.get());
        auto shardId = writeRouter.getReshardingDestinedRecipient(BSON("_id" << 0));
        ASSERT(shardId != boost::none);
    }
}

void BM_UnshardedDestinedRecipient(benchmark::State& state) {
    serverGlobalParams.clusterRole = ClusterRole::None;

    auto serviceContext = ServiceContext::make();
    const auto client = serviceContext->makeClient("test");
    serviceContext->registerClientObserver(std::make_unique<LockerNoopClientObserver>());
    const auto opCtx = client->makeOperationContext();

    const auto catalogCache = CatalogCacheMock::make();

    for (auto keepRunning : state) {
        benchmark::ClobberMemory();
        ShardingWriteRouter writeRouter(opCtx.get(), kNss, catalogCache.get());
        auto shardId = writeRouter.getReshardingDestinedRecipient(BSON("_id" << 0));
        ASSERT(shardId == boost::none);
    }
}

BENCHMARK(BM_InsertGetDestinedRecipient)
    ->Range(1, 1 << 4)
    ->ThreadRange(1, ProcessInfo::getNumAvailableCores());

BENCHMARK(BM_UpdateGetDestinedRecipient)
    ->Range(1, 1 << 4)
    ->ThreadRange(1, ProcessInfo::getNumAvailableCores());

BENCHMARK(BM_UnshardedDestinedRecipient)
    ->Range(1, 1 << 4)
    ->ThreadRange(1, ProcessInfo::getNumAvailableCores());

}  // namespace
}  // namespace mongo
