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

#include <memory>
#include <vector>

#include "mongo/db/namespace_string.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/sharding_router_test_fixture.h"

namespace mongo {

class CatalogCacheTestFixture : public ShardingTestFixture {
protected:
    void setUp() override;

    /**
     * Returns a chunk manager for the specified namespace with chunks at the specified split
     * points. Each individual chunk is placed on a separate shard with shard id being a single
     * number ranging from "0" to the number of chunks.
     */
    ChunkManager makeChunkManager(const NamespaceString& nss,
                                  const ShardKeyPattern& shardKeyPattern,
                                  std::unique_ptr<CollatorInterface> defaultCollator,
                                  bool unique,
                                  const std::vector<BSONObj>& splitPoints,
                                  boost::optional<ReshardingFields> reshardingFields = boost::none);

    /**
     * Invalidates the catalog cache for 'kNss' and schedules a thread to invoke the blocking 'get'
     * call, returning a future which can be obtained to get the specified routing information.
     *
     * The notion of 'forced' in the function name implies that we will always indicate to the
     * catalog cache that a refresh will happen, regardless of an epoch change or a stale shard.
     *
     * NOTE: The returned value is always set. The reason to use optional is a deficiency of
     * std::future with the MSVC STL library, which requires the templated type to be default
     * constructible.
     */
    executor::NetworkTestEnv::FutureHandle<boost::optional<ChunkManager>>
    scheduleRoutingInfoForcedRefresh(const NamespaceString& nss);

    /**
     * Invalidates the catalog cache for 'kNss' and schedules a thread to invoke the blocking 'get'
     * call, returning a future which can be obtained to get the specified routing information.
     *
     * The notion of 'unforced' in the function name implies that a refresh will only happen if
     * the epoch has been changed, or if a future targetted shard has been maked as stale.
     *
     * NOTE: The returned value is always set. The reason to use optional is a deficiency of
     * std::future with the MSVC STL library, which requires the templated type to be default
     * constructible.
     */
    executor::NetworkTestEnv::FutureHandle<boost::optional<ChunkManager>>
    scheduleRoutingInfoUnforcedRefresh(const NamespaceString& nss);

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
    ChunkManager loadRoutingTableWithTwoChunksAndTwoShardsImpl(NamespaceString nss,
                                                               const BSONObj& shardKey);

    /**
     * Mocks network responses for loading a sharded database and collection from the config server.
     */
    void expectGetDatabase(NamespaceString nss, std::string primaryShard = "0");
    void expectGetCollection(NamespaceString nss,
                             OID epoch,
                             const ShardKeyPattern& shardKeyPattern);

    const HostAndPort kConfigHostAndPort{"DummyConfig", 1234};
};

}  // namespace mongo
