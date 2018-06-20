/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <set>
#include <vector>

#include "mongo/s/catalog_cache_test_fixture.h"

#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/query/collation/collator_factory_mock.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

void CatalogCacheTestFixture::setUp() {
    ShardingTestFixture::setUp();
    setRemote(HostAndPort("FakeRemoteClient:34567"));
    configTargeter()->setFindHostReturnValue(kConfigHostAndPort);

    CollatorFactoryInterface::set(serviceContext(), stdx::make_unique<CollatorFactoryMock>());
}

executor::NetworkTestEnv::FutureHandle<boost::optional<CachedCollectionRoutingInfo>>
CatalogCacheTestFixture::scheduleRoutingInfoRefresh(const NamespaceString& nss) {
    return launchAsync([this, nss] {
        auto client = serviceContext()->makeClient("Test");
        auto opCtx = client->makeOperationContext();
        auto const catalogCache = Grid::get(serviceContext())->catalogCache();
        catalogCache->invalidateShardedCollection(nss);

        return boost::make_optional(
            uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx.get(), nss)));
    });
}

void CatalogCacheTestFixture::setupNShards(int numShards) {
    setupShards([&]() {
        std::vector<ShardType> shards;
        for (int i = 0; i < numShards; i++) {
            ShardId name(str::stream() << i);
            HostAndPort host(str::stream() << "Host" << i << ":12345");

            ShardType shard;
            shard.setName(name.toString());
            shard.setHost(host.toString());
            shards.emplace_back(std::move(shard));

            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                stdx::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(host));
            targeter->setFindHostReturnValue(host);
            targeterFactory()->addTargeterToReturn(ConnectionString(host), std::move(targeter));
        }

        return shards;
    }());
}

std::shared_ptr<ChunkManager> CatalogCacheTestFixture::makeChunkManager(
    const NamespaceString& nss,
    const ShardKeyPattern& shardKeyPattern,
    std::unique_ptr<CollatorInterface> defaultCollator,
    bool unique,
    const std::vector<BSONObj>& splitPoints) {
    ChunkVersion version(1, 0, OID::gen());

    const BSONObj databaseBSON = [&]() {
        DatabaseType db(nss.db().toString(), {"0"}, true, databaseVersion::makeNew());
        return db.toBSON();
    }();

    const BSONObj collectionBSON = [&]() {
        CollectionType coll;
        coll.setNs(nss);
        coll.setEpoch(version.epoch());
        coll.setKeyPattern(shardKeyPattern.getKeyPattern());
        coll.setUnique(unique);

        if (defaultCollator) {
            coll.setDefaultCollation(defaultCollator->getSpec().toBSON());
        }

        return coll.toBSON();
    }();

    std::vector<BSONObj> initialChunks;

    auto splitPointsIncludingEnds(splitPoints);
    splitPointsIncludingEnds.insert(splitPointsIncludingEnds.begin(),
                                    shardKeyPattern.getKeyPattern().globalMin());
    splitPointsIncludingEnds.push_back(shardKeyPattern.getKeyPattern().globalMax());

    for (size_t i = 1; i < splitPointsIncludingEnds.size(); ++i) {
        ChunkType chunk(
            nss,
            {shardKeyPattern.getKeyPattern().extendRangeBound(splitPointsIncludingEnds[i - 1],
                                                              false),
             shardKeyPattern.getKeyPattern().extendRangeBound(splitPointsIncludingEnds[i], false)},
            version,
            ShardId{str::stream() << (i - 1)});

        initialChunks.push_back(chunk.toConfigBSON());

        version.incMajor();
    }

    setupNShards(initialChunks.size());

    auto future = scheduleRoutingInfoRefresh(nss);

    expectFindSendBSONObjVector(kConfigHostAndPort, {databaseBSON});
    expectFindSendBSONObjVector(kConfigHostAndPort, {collectionBSON});
    expectFindSendBSONObjVector(kConfigHostAndPort, {collectionBSON});
    expectFindSendBSONObjVector(kConfigHostAndPort, initialChunks);

    auto routingInfo = future.timed_get(kFutureTimeout);
    ASSERT(routingInfo->cm());
    ASSERT(routingInfo->db().primary());

    return routingInfo->cm();
}

void CatalogCacheTestFixture::expectGetDatabase(NamespaceString nss) {
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        DatabaseType db(nss.db().toString(), {"0"}, true, databaseVersion::makeNew());
        return std::vector<BSONObj>{db.toBSON()};
    }());
}

void CatalogCacheTestFixture::expectGetCollection(NamespaceString nss,
                                                  OID epoch,
                                                  const ShardKeyPattern& shardKeyPattern) {
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        CollectionType collType;
        collType.setNs(nss);
        collType.setEpoch(epoch);
        collType.setKeyPattern(shardKeyPattern.toBSON());
        collType.setUnique(false);

        return std::vector<BSONObj>{collType.toBSON()};
    }());
}

CachedCollectionRoutingInfo CatalogCacheTestFixture::loadRoutingTableWithTwoChunksAndTwoShards(
    NamespaceString nss) {
    const OID epoch = OID::gen();
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto future = scheduleRoutingInfoRefresh(nss);

    // Mock the expected config server queries.
    expectGetDatabase(nss);
    expectGetCollection(nss, epoch, shardKeyPattern);
    expectGetCollection(nss, epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        ChunkVersion version(1, 0, epoch);

        ChunkType chunk1(
            nss, {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)}, version, {"0"});
        version.incMinor();

        ChunkType chunk2(
            nss, {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()}, version, {"1"});
        version.incMinor();

        return std::vector<BSONObj>{chunk1.toConfigBSON(), chunk2.toConfigBSON()};
    }());

    return future.timed_get(kFutureTimeout).get();
}

}  // namespace mongo
