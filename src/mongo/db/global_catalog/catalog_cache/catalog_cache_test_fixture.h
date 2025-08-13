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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * The ServerRole-independent core of `RouterCatalogCacheTestFixture` and its
 * `ShardCatalogCacheTestFixture` counterpart.
 */
class CoreCatalogCacheTestFixture : public ShardingTestFixture {
protected:
    CoreCatalogCacheTestFixture()
        : ShardingTestFixture(false,  // No mock catalog cache
                              std::make_unique<ScopedGlobalServiceContextForTest>(shouldSetupTL)) {}

    explicit CoreCatalogCacheTestFixture(
        std::unique_ptr<ScopedGlobalServiceContextForTest> scopedServiceContext)
        : ShardingTestFixture(false,  // No mock catalog cache
                              std::move(scopedServiceContext)) {}

    void setUp() override;

    /**
     * Returns a chunk manager for the specified namespace with chunks at the specified split
     * points. Each individual chunk is placed on a separate shard with shard id being a single
     * number ranging from "0" to the number of chunks.
     */
    CollectionRoutingInfo makeCollectionRoutingInfo(
        const NamespaceString& nss,
        const ShardKeyPattern& shardKeyPattern,
        std::unique_ptr<CollatorInterface> defaultCollator,
        bool unique,
        const std::vector<BSONObj>& splitPoints,
        boost::optional<ReshardingFields> reshardingFields = boost::none,
        boost::optional<TypeCollectionTimeseriesFields> timeseriesFields = boost::none,
        boost::optional<bool> unsplittable = boost::none,
        size_t chunksPerShard = 1);

    /**
     * Returns a chunk manager representing an unsharded collection tracked on the configsvr.
     */
    CollectionRoutingInfo makeUnshardedCollectionRoutingInfo(const NamespaceString& nss);

    /**
     * Returns a chunk manager representing an unsharded collection not tracked on the configsvr,
     * or a non-existent collection.
     */
    CollectionRoutingInfo makeUntrackedCollectionRoutingInfo(const NamespaceString& nss);

    /**
     * Invalidates the catalog cache for 'kNss' and schedules a thread to invoke the blocking 'get'
     * call, returning a future which can be obtained to get the specified routing information.
     *
     * The notion of 'forced' in the function name implies that we will always indicate to the
     * catalog cache that a refresh will happen.
     *
     * NOTE: The returned value is always set. The reason to use optional is a deficiency of
     * std::future with the MSVC STL library, which requires the templated type to be default
     * constructible.
     */
    executor::NetworkTestEnv::FutureHandle<boost::optional<CollectionRoutingInfo>>
    scheduleRoutingInfoForcedRefresh(const NamespaceString& nss);

    /**
     * Gets the routing information from the catalog cache. Unforced means the refresh will only
     * happen if the cache has been invalidated elsewhere.
     *
     * NOTE: The returned value is always set. The reason to use optional is a deficiency of
     * std::future with the MSVC STL library, which requires the templated type to be default
     * constructible.
     */
    executor::NetworkTestEnv::FutureHandle<boost::optional<CollectionRoutingInfo>>
    scheduleRoutingInfoUnforcedRefresh(const NamespaceString& nss);

    /**
     * Advance the time in the cache for 'kNss' and schedules a thread to make an incremental
     * refresh.
     *
     * NOTE: The returned value is always set. The reason to use optional is a deficiency of
     * std::future with the MSVC STL library, which requires the templated type to be default
     * constructible.
     */
    executor::NetworkTestEnv::FutureHandle<boost::optional<CollectionRoutingInfo>>
    scheduleRoutingInfoIncrementalRefresh(const NamespaceString& nss);

    /**
     * Ensures that there are 'numShards' available in the shard registry. The shard ids are
     * generated as "0", "1", etc.
     *
     * Returns the mock shard descriptors that were used for the setup.
     */
    std::vector<ShardType> setupNShards(int numShards);

    /**
     * Triggers a refresh for the given namespace and mocks network calls to simulate loading
     * metadata with two chunks: [minKey, 0) and [0, maxKey) on two shards with ids: "0" and "1".
     */
    ChunkManager loadRoutingTableWithTwoChunksAndTwoShards(NamespaceString nss);

    /**
     * Same as the above method but the sharding key is hashed.
     */
    ChunkManager loadRoutingTableWithTwoChunksAndTwoShardsHash(NamespaceString nss);

    /**
     * The common implementation for any shard key.
     */
    ChunkManager loadRoutingTableWithTwoChunksAndTwoShardsImpl(
        NamespaceString nss,
        const BSONObj& shardKey,
        boost::optional<std::string> primaryShardId = boost::none,
        UUID uuid = UUID::gen(),
        OID epoch = OID::gen(),
        Timestamp timestamp = Timestamp(1));

    /**
     * Mocks network responses for loading a sharded database and collection from the config server.
     */
    void expectGetDatabase(NamespaceString nss, std::string primaryShard = "0");
    void expectGetCollection(NamespaceString nss,
                             OID epoch,
                             Timestamp timestamp,
                             UUID uuid,
                             const ShardKeyPattern& shardKeyPattern);
    void expectCollectionAndChunksAggregation(NamespaceString nss,
                                              OID epoch,
                                              Timestamp timestamp,
                                              UUID uuid,
                                              const ShardKeyPattern& shardKeyPattern,
                                              const std::vector<ChunkType>& chunks);
    const HostAndPort kConfigHostAndPort{"DummyConfig", 1234};
};

class RouterCatalogCacheTestFixture : public virtual service_context_test::RouterRoleOverride,
                                      public CoreCatalogCacheTestFixture {
    using CoreCatalogCacheTestFixture::CoreCatalogCacheTestFixture;
};

class ShardCatalogCacheTestFixture : public virtual service_context_test::ShardRoleOverride,
                                     public CoreCatalogCacheTestFixture {
    using CoreCatalogCacheTestFixture::CoreCatalogCacheTestFixture;
};

}  // namespace mongo
