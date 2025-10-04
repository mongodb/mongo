/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/catalog_cache/catalog_cache_test_fixture.h"

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_factory_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <system_error>
#include <utility>

namespace mongo {

void CoreCatalogCacheTestFixture::setUp() {
    ShardingTestFixture::setUp();
    configTargeter()->setFindHostReturnValue(kConfigHostAndPort);

    CollatorFactoryInterface::set(getServiceContext(), std::make_unique<CollatorFactoryMock>());
}

executor::NetworkTestEnv::FutureHandle<boost::optional<CollectionRoutingInfo>>
CoreCatalogCacheTestFixture::scheduleRoutingInfoForcedRefresh(const NamespaceString& nss) {
    return launchAsync([this, nss] {
        auto client = getServiceContext()->getService()->makeClient("Test");
        auto const catalogCache = Grid::get(getServiceContext())->catalogCache();

        catalogCache->onStaleCollectionVersion(nss, boost::none /* wantedVersion */);
        auto cri = uassertStatusOK(catalogCache->getCollectionRoutingInfo(operationContext(), nss));
        return boost::make_optional(cri);
    });
}

executor::NetworkTestEnv::FutureHandle<boost::optional<CollectionRoutingInfo>>
CoreCatalogCacheTestFixture::scheduleRoutingInfoUnforcedRefresh(const NamespaceString& nss) {
    return launchAsync([this, nss] {
        auto client = getServiceContext()->getService()->makeClient("Test");
        auto const catalogCache = Grid::get(getServiceContext())->catalogCache();

        auto cri = uassertStatusOK(catalogCache->getCollectionRoutingInfo(operationContext(), nss));
        return boost::make_optional(cri);
    });
}

executor::NetworkTestEnv::FutureHandle<boost::optional<CollectionRoutingInfo>>
CoreCatalogCacheTestFixture::scheduleRoutingInfoIncrementalRefresh(const NamespaceString& nss) {
    auto catalogCache = Grid::get(getServiceContext())->catalogCache();
    const auto cri =
        uassertStatusOK(catalogCache->getCollectionRoutingInfo(operationContext(), nss));
    ASSERT(cri.isSharded());
    ASSERT(cri.getChunkManager().isSharded());

    // Simulates the shard wanting a higher version than the one sent by the router.
    catalogCache->onStaleCollectionVersion(nss, boost::none);

    return launchAsync([this, nss] {
        auto client = getServiceContext()->getService()->makeClient("Test");
        auto const catalogCache = Grid::get(getServiceContext())->catalogCache();

        return boost::make_optional(
            uassertStatusOK(catalogCache->getCollectionRoutingInfo(operationContext(), nss)));
    });
}

std::vector<ShardType> CoreCatalogCacheTestFixture::setupNShards(int numShards) {
    std::vector<ShardType> shards;
    for (int i = 0; i < numShards; i++) {
        ShardId name(str::stream() << i);
        HostAndPort host(str::stream() << "Host" << i << ":12345");

        ShardType shard;
        shard.setName(name.toString());
        shard.setHost(host.toString());
        shards.emplace_back(std::move(shard));

        std::unique_ptr<RemoteCommandTargeterMock> targeter(
            std::make_unique<RemoteCommandTargeterMock>());
        targeter->setConnectionStringReturnValue(ConnectionString(host));
        targeter->setFindHostReturnValue(host);
        targeterFactory()->addTargeterToReturn(ConnectionString(host), std::move(targeter));
    }

    setupShards(shards);
    return shards;
}

CollectionRoutingInfo CoreCatalogCacheTestFixture::makeCollectionRoutingInfo(
    const NamespaceString& nss,
    const ShardKeyPattern& shardKeyPattern,
    std::unique_ptr<CollatorInterface> defaultCollator,
    bool unique,
    const std::vector<BSONObj>& splitPoints,
    boost::optional<ReshardingFields> reshardingFields,
    boost::optional<TypeCollectionTimeseriesFields> timeseriesFields,
    boost::optional<bool> unsplittable,
    size_t chunksPerShard) {
    ChunkVersion version({OID::gen(), Timestamp(42)}, {1, 0});

    DatabaseType db(nss.dbName(), {"0"}, DatabaseVersion(UUID::gen(), Timestamp()));

    const auto uuid = UUID::gen();
    const BSONObj collectionBSON = [&]() {
        CollectionType coll(nss,
                            version.epoch(),
                            version.getTimestamp(),
                            Date_t::now(),
                            uuid,
                            shardKeyPattern.getKeyPattern());
        coll.setUnique(unique);

        if (defaultCollator) {
            coll.setDefaultCollation(defaultCollator->getSpec().toBSON());
        }

        if (reshardingFields) {
            coll.setReshardingFields(std::move(reshardingFields));
        }

        if (timeseriesFields) {
            coll.setTimeseriesFields(std::move(timeseriesFields));
        }

        coll.setUnsplittable(unsplittable);

        return coll.toBSON();
    }();

    std::vector<BSONObj> initialChunks;

    auto splitPointsIncludingEnds(splitPoints);
    splitPointsIncludingEnds.insert(splitPointsIncludingEnds.begin(),
                                    shardKeyPattern.getKeyPattern().globalMin());
    splitPointsIncludingEnds.push_back(shardKeyPattern.getKeyPattern().globalMax());

    for (size_t i = 1; i < splitPointsIncludingEnds.size(); ++i) {
        int shardIndex = (i - 1) / chunksPerShard;
        ChunkType chunk(
            uuid,
            {shardKeyPattern.getKeyPattern().extendRangeBound(splitPointsIncludingEnds[i - 1],
                                                              false),
             shardKeyPattern.getKeyPattern().extendRangeBound(splitPointsIncludingEnds[i], false)},
            version,
            ShardId{str::stream() << shardIndex});
        chunk.setName(OID::gen());

        initialChunks.push_back(chunk.toConfigBSON());

        version.incMajor();
    }

    setupNShards(initialChunks.size());

    auto future = scheduleRoutingInfoUnforcedRefresh(nss);

    expectFindSendBSONObjVector(kConfigHostAndPort, {db.toBSON()});
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        std::vector<BSONObj> aggResult{collectionBSON};
        std::transform(initialChunks.begin(),
                       initialChunks.end(),
                       std::back_inserter(aggResult),
                       [](const auto& chunk) { return BSON("chunks" << chunk); });
        return aggResult;
    }());
    return *future.default_timed_get();
}

CollectionRoutingInfo CoreCatalogCacheTestFixture::makeUnshardedCollectionRoutingInfo(
    const NamespaceString& nss) {
    return makeCollectionRoutingInfo(nss,
                                     ShardKeyPattern(BSON("_id" << 1)),
                                     nullptr /* defaultCollator */,
                                     false /* unique */,
                                     {} /* splitPoints */,
                                     boost::none /* reshardingFields */,
                                     boost::none /* timeseriesFields */,
                                     true /* unsplittable */);
}

CollectionRoutingInfo CoreCatalogCacheTestFixture::makeUntrackedCollectionRoutingInfo(
    const NamespaceString& nss) {
    setupNShards(1);
    DatabaseType db(nss.dbName(), {"0"}, DatabaseVersion(UUID::gen(), Timestamp()));

    auto future = scheduleRoutingInfoUnforcedRefresh(nss);
    expectFindSendBSONObjVector(kConfigHostAndPort, {db.toBSON()});
    expectFindSendBSONObjVector(kConfigHostAndPort, std::vector<BSONObj>{});
    return *future.default_timed_get();
}

void CoreCatalogCacheTestFixture::expectGetDatabase(NamespaceString nss, std::string shardId) {
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        DatabaseType db(nss.dbName(), {shardId}, DatabaseVersion(UUID::gen(), Timestamp()));
        return std::vector<BSONObj>{db.toBSON()};
    }());
}

void CoreCatalogCacheTestFixture::expectGetCollection(NamespaceString nss,
                                                      OID epoch,
                                                      Timestamp timestamp,
                                                      UUID uuid,
                                                      const ShardKeyPattern& shardKeyPattern) {
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        CollectionType collType(
            nss, epoch, timestamp, Date_t::now(), uuid, shardKeyPattern.toBSON());
        return std::vector<BSONObj>{collType.toBSON()};
    }());
}

void CoreCatalogCacheTestFixture::expectCollectionAndChunksAggregation(
    NamespaceString nss,
    OID epoch,
    Timestamp timestamp,
    UUID uuid,
    const ShardKeyPattern& shardKeyPattern,
    const std::vector<ChunkType>& chunks) {
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        CollectionType collType(
            nss, epoch, timestamp, Date_t::now(), uuid, shardKeyPattern.toBSON());

        std::vector<BSONObj> aggResult{collType.toBSON()};
        std::transform(chunks.begin(),
                       chunks.end(),
                       std::back_inserter(aggResult),
                       [](const auto& chunk) { return BSON("chunks" << chunk.toConfigBSON()); });
        return aggResult;
    }());
}

ChunkManager CoreCatalogCacheTestFixture::loadRoutingTableWithTwoChunksAndTwoShards(
    NamespaceString nss) {

    return loadRoutingTableWithTwoChunksAndTwoShardsImpl(nss, BSON("_id" << 1));
}

ChunkManager CoreCatalogCacheTestFixture::loadRoutingTableWithTwoChunksAndTwoShardsHash(
    NamespaceString nss) {

    return loadRoutingTableWithTwoChunksAndTwoShardsImpl(nss, BSON("_id" << "hashed"));
}

ChunkManager CoreCatalogCacheTestFixture::loadRoutingTableWithTwoChunksAndTwoShardsImpl(
    NamespaceString nss,
    const BSONObj& shardKey,
    boost::optional<std::string> primaryShardId,
    UUID uuid,
    OID epoch,
    Timestamp timestamp) {
    const ShardKeyPattern shardKeyPattern(shardKey);

    auto future = scheduleRoutingInfoForcedRefresh(nss);

    // Mock the expected config server queries.
    if (!nss.isAdminDB() && !nss.isConfigDB()) {
        if (primaryShardId) {
            expectGetDatabase(nss, *primaryShardId);
        } else {
            expectGetDatabase(nss);
        }
    }
    CollectionType collType(nss, epoch, timestamp, Date_t::now(), uuid, shardKeyPattern.toBSON());
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        ChunkVersion version({epoch, timestamp}, {1, 0});

        ChunkType chunk1(
            uuid, {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)}, version, {"0"});
        chunk1.setName(OID::gen());
        version.incMinor();

        ChunkType chunk2(
            uuid, {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()}, version, {"1"});
        chunk2.setName(OID::gen());
        version.incMinor();

        const auto chunk1Obj = BSON("chunks" << chunk1.toConfigBSON());
        const auto chunk2Obj = BSON("chunks" << chunk2.toConfigBSON());
        return std::vector<BSONObj>{collType.toBSON(), chunk1Obj, chunk2Obj};
    }());
    return future.default_timed_get()->getChunkManager();
}

}  // namespace mongo
