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

// IWYU pragma: no_include "cxxabi.h"
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <memory>
#include <system_error>
#include <tuple>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/catalog_cache_loader_mock.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/index_version.h"
#include "mongo/s/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/sharding_mongos_test_fixture.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

class CatalogCacheTest : public ShardingTestFixture {
protected:
    void setUp() override {
        ShardingTestFixture::setUp();

        // Setup dummy config server
        configTargeter()->setFindHostReturnValue(kConfigHostAndPort);

        // Setup catalogCache with mock loader
        _catalogCacheLoader = std::make_shared<CatalogCacheLoaderMock>();
        _catalogCache = std::make_unique<CatalogCache>(getServiceContext(), _catalogCacheLoader);

        // Populate the shardRegistry with the shards from kShards vector
        std::vector<std::tuple<ShardId, HostAndPort>> shardInfos;
        for (const auto& shardId : kShards) {
            shardInfos.emplace_back(
                std::make_tuple(shardId, HostAndPort(shardId.toString(), kDummyPort)));
        }
        addRemoteShards(shardInfos);
    };

    class ScopedCollectionProvider {
    public:
        ScopedCollectionProvider(std::shared_ptr<CatalogCacheLoaderMock> catalogCacheLoader,
                                 const StatusWith<CollectionType>& swCollection)
            : _catalogCacheLoader(catalogCacheLoader) {
            _catalogCacheLoader->setCollectionRefreshReturnValue(swCollection);
        }
        ~ScopedCollectionProvider() {
            _catalogCacheLoader->clearCollectionReturnValue();
        }

    private:
        std::shared_ptr<CatalogCacheLoaderMock> _catalogCacheLoader;
    };

    ScopedCollectionProvider scopedCollectionProvider(
        const StatusWith<CollectionType>& swCollection) {
        return {_catalogCacheLoader, swCollection};
    }

    class ScopedChunksProvider {
    public:
        ScopedChunksProvider(std::shared_ptr<CatalogCacheLoaderMock> catalogCacheLoader,
                             const StatusWith<std::vector<ChunkType>>& swChunks)
            : _catalogCacheLoader(catalogCacheLoader) {
            _catalogCacheLoader->setChunkRefreshReturnValue(swChunks);
        }
        ~ScopedChunksProvider() {
            _catalogCacheLoader->clearChunksReturnValue();
        }

    private:
        std::shared_ptr<CatalogCacheLoaderMock> _catalogCacheLoader;
    };

    ScopedChunksProvider scopedChunksProvider(const StatusWith<std::vector<ChunkType>>& swChunks) {
        return {_catalogCacheLoader, swChunks};
    }

    class ScopedDatabaseProvider {
    public:
        ScopedDatabaseProvider(std::shared_ptr<CatalogCacheLoaderMock> catalogCacheLoader,
                               const StatusWith<DatabaseType>& swDatabase)
            : _catalogCacheLoader(catalogCacheLoader) {
            _catalogCacheLoader->setDatabaseRefreshReturnValue(swDatabase);
        }
        ~ScopedDatabaseProvider() {
            _catalogCacheLoader->clearDatabaseReturnValue();
        }

    private:
        std::shared_ptr<CatalogCacheLoaderMock> _catalogCacheLoader;
    };

    ScopedDatabaseProvider scopedDatabaseProvider(const StatusWith<DatabaseType>& swDatabase) {
        return {_catalogCacheLoader, swDatabase};
    }

    void loadDatabases(const std::vector<DatabaseType>& databases) {
        for (const auto& db : databases) {
            const auto scopedDbProvider = scopedDatabaseProvider(db);
            const auto swDatabase = _catalogCache->getDatabase(operationContext(), db.getDbName());
            ASSERT_OK(swDatabase.getStatus());
        }
    }

    CollectionType loadCollection(const ShardVersion& version) {
        auto coll = makeCollectionType(version);
        const auto scopedCollProv = scopedCollectionProvider(coll);
        const auto scopedChunksProv = scopedChunksProvider(makeChunks(version.placementVersion()));
        auto future = launchAsync([&] {
            onCommand([&](const executor::RemoteCommandRequest& request) {
                return makeCollectionAndIndexesAggregationResponse(coll, std::vector<BSONObj>());
            });
        });

        const auto swCri =
            _catalogCache->getCollectionRoutingInfo(operationContext(), coll.getNss());
        ASSERT_OK(swCri.getStatus());
        future.default_timed_get();
        return coll;
    }

    void loadUnshardedCollection(const NamespaceString& nss) {
        const auto scopedCollProvider =
            scopedCollectionProvider(Status(ErrorCodes::NamespaceNotFound, "collection not found"));

        const auto swCri = _catalogCache->getCollectionRoutingInfo(operationContext(), nss);
        ASSERT_OK(swCri.getStatus());
    }

    std::vector<ChunkType> makeChunks(ChunkVersion version) {
        ChunkType chunk(kUUID,
                        {kShardKeyPattern.getKeyPattern().globalMin(),
                         kShardKeyPattern.getKeyPattern().globalMax()},
                        version,
                        {"0"});
        chunk.setName(OID::gen());
        return {chunk};
    }

    CollectionType makeCollectionType(const ShardVersion& collPlacementVersion) {
        CollectionType coll{kNss,
                            collPlacementVersion.placementVersion().epoch(),
                            collPlacementVersion.placementVersion().getTimestamp(),
                            Date_t::now(),
                            kUUID,
                            kShardKeyPattern.getKeyPattern()};
        if (collPlacementVersion.indexVersion()) {
            coll.setIndexVersion(CollectionIndexes(kUUID, *collPlacementVersion.indexVersion()));
        }
        return coll;
    }

    BSONObj makeCollectionAndIndexesAggregationResponse(const CollectionType& coll,
                                                        const std::vector<BSONObj>& indexes) {
        BSONObj obj = coll.toBSON();
        BSONObjBuilder indexField;
        indexField.append("indexes", indexes);
        BSONObj newObj = obj.addField(indexField.obj().firstElement());
        return CursorResponse(CollectionType::ConfigNS, CursorId{0}, {newObj})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    }

    const NamespaceString kNss =
        NamespaceString::createNamespaceString_forTest("catalgoCacheTestDB.foo");
    const UUID kUUID = UUID::gen();
    const std::string kPattern{"_id"};
    const ShardKeyPattern kShardKeyPattern{BSON(kPattern << 1)};
    const int kDummyPort{12345};
    const HostAndPort kConfigHostAndPort{"DummyConfig", kDummyPort};
    const std::vector<ShardId> kShards{{"0"}, {"1"}};
    RAIIServerParameterControllerForTest featureFlagController{
        "featureFlagGlobalIndexesShardingCatalog", true};

    std::shared_ptr<CatalogCacheLoaderMock> _catalogCacheLoader;
    std::unique_ptr<CatalogCache> _catalogCache;
};

const DatabaseName kDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testDB");

TEST_F(CatalogCacheTest, GetDatabase) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    _catalogCacheLoader->setDatabaseRefreshReturnValue(
        DatabaseType(kDbName, kShards[0], dbVersion));

    const auto swDatabase = _catalogCache->getDatabase(operationContext(), kDbName);

    ASSERT_OK(swDatabase.getStatus());
    const auto cachedDb = swDatabase.getValue();
    ASSERT_EQ(cachedDb->getPrimary(), kShards[0]);
    ASSERT_EQ(cachedDb->getVersion().getUuid(), dbVersion.getUuid());
    ASSERT_EQ(cachedDb->getVersion().getLastMod(), dbVersion.getLastMod());
}

TEST_F(CatalogCacheTest, GetCachedDatabase) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    loadDatabases({DatabaseType(kDbName, kShards[0], dbVersion)});

    const auto swDatabase = _catalogCache->getDatabase(operationContext(), kDbName);

    ASSERT_OK(swDatabase.getStatus());
    const auto cachedDb = swDatabase.getValue();
    ASSERT_EQ(cachedDb->getPrimary(), kShards[0]);
    ASSERT_EQ(cachedDb->getVersion().getUuid(), dbVersion.getUuid());
    ASSERT_EQ(cachedDb->getVersion().getLastMod(), dbVersion.getLastMod());
}

TEST_F(CatalogCacheTest, GetDatabaseDrop) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));

    _catalogCacheLoader->setDatabaseRefreshReturnValue(
        DatabaseType(kDbName, kShards[0], dbVersion));

    // The CatalogCache doesn't have any valid info about this DB and finds a new DatabaseType
    auto swDatabase = _catalogCache->getDatabase(operationContext(), kDbName);
    ASSERT_OK(swDatabase.getStatus());
    const auto cachedDb = swDatabase.getValue();
    ASSERT_EQ(cachedDb->getVersion().getUuid(), dbVersion.getUuid());
    ASSERT_EQ(cachedDb->getVersion().getLastMod(), dbVersion.getLastMod());

    // Advancing the timeInStore, e.g. because of a movePrimary
    _catalogCache->onStaleDatabaseVersion(kDbName, dbVersion.makeUpdated());

    // However, when this CatalogCache asks to the loader for the new info associated to dbName it
    // didn't find any (i.e. the database was dropped)
    _catalogCacheLoader->setDatabaseRefreshReturnValue(
        Status(ErrorCodes::NamespaceNotFound, "dummy errmsg"));

    // Finally, the CatalogCache shouldn't find the Database
    swDatabase = _catalogCache->getDatabase(operationContext(), kDbName);
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, swDatabase.getStatus());
}

TEST_F(CatalogCacheTest, InvalidateSingleDbOnShardRemoval) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    loadDatabases({DatabaseType(kDbName, kShards[0], dbVersion)});

    _catalogCache->invalidateEntriesThatReferenceShard(kShards[0]);
    _catalogCacheLoader->setDatabaseRefreshReturnValue(
        DatabaseType(kDbName, kShards[1], dbVersion));
    const auto swDatabase = _catalogCache->getDatabase(operationContext(), kDbName);

    ASSERT_OK(swDatabase.getStatus());
    auto cachedDb = swDatabase.getValue();
    ASSERT_EQ(cachedDb->getPrimary(), kShards[1]);
}

TEST_F(CatalogCacheTest, OnStaleDatabaseVersionNoVersion) {
    // onStaleDatabaseVesrsion must invalidate the database entry if invoked with no version
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    loadDatabases({DatabaseType(kNss.dbName(), kShards[0], dbVersion)});

    _catalogCache->onStaleDatabaseVersion(kNss.dbName(), boost::none);

    const auto status = _catalogCache->getDatabase(operationContext(), kNss.dbName()).getStatus();
    ASSERT(status == ErrorCodes::InternalError);
}

TEST_F(CatalogCacheTest, OnStaleShardVersionWithSameVersion) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const CollectionGeneration gen(OID::gen(), Timestamp(1, 1));
    const auto cachedCollVersion = ShardVersionFactory::make(
        ChunkVersion(gen, {1, 0}), boost::optional<CollectionIndexes>(boost::none));

    loadDatabases({DatabaseType(kNss.dbName(), kShards[0], dbVersion)});
    loadCollection(cachedCollVersion);
    _catalogCache->onStaleCollectionVersion(kNss, cachedCollVersion);
    ASSERT_OK(_catalogCache->getCollectionRoutingInfo(operationContext(), kNss).getStatus());
}

TEST_F(CatalogCacheTest, OnStaleShardVersionWithNoVersion) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const CollectionGeneration gen(OID::gen(), Timestamp(1, 1));
    const auto cachedCollVersion = ShardVersionFactory::make(
        ChunkVersion(gen, {1, 0}), boost::optional<CollectionIndexes>(boost::none));

    loadDatabases({DatabaseType(kNss.dbName(), kShards[0], dbVersion)});
    loadCollection(cachedCollVersion);
    _catalogCache->onStaleCollectionVersion(kNss, boost::none);
    const auto status =
        _catalogCache->getCollectionRoutingInfo(operationContext(), kNss).getStatus();
    ASSERT(status == ErrorCodes::InternalError);
}

TEST_F(CatalogCacheTest, OnStaleShardVersionWithGreaterPlacementVersion) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const CollectionGeneration gen(OID::gen(), Timestamp(1, 1));
    const auto cachedCollVersion = ShardVersionFactory::make(
        ChunkVersion(gen, {1, 0}), boost::optional<CollectionIndexes>(boost::none));
    const auto wantedCollVersion = ShardVersionFactory::make(
        ChunkVersion(gen, {2, 0}), boost::optional<CollectionIndexes>(boost::none));

    loadDatabases({DatabaseType(kNss.dbName(), kShards[0], dbVersion)});
    loadCollection(cachedCollVersion);
    _catalogCache->onStaleCollectionVersion(kNss, wantedCollVersion);
    const auto status =
        _catalogCache->getCollectionRoutingInfo(operationContext(), kNss).getStatus();
    ASSERT(status == ErrorCodes::InternalError);
}

TEST_F(CatalogCacheTest, GetCollectionRoutingInfoAllowLocksReturnsImmediately) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const CollectionGeneration gen(OID::gen(), Timestamp(1, 1));
    const auto cachedCollVersion = ShardVersionFactory::make(
        ChunkVersion(gen, {1, 0}), boost::optional<CollectionIndexes>(boost::none));

    loadDatabases({DatabaseType(kNss.dbName(), kShards[0], dbVersion)});
    auto coll = loadCollection(cachedCollVersion);

    const auto swCri = _catalogCache->getCollectionRoutingInfo(
        operationContext(), coll.getNss(), true /* allowLocks */);
    ASSERT_OK(swCri.getStatus());

    ASSERT(swCri.getValue().getCollectionVersion() == cachedCollVersion);
}

TEST_F(CatalogCacheTest, GetCollectionRoutingInfoAllowLocksNeedsToFetchNewCollInfo) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const CollectionGeneration gen(OID::gen(), Timestamp(1, 1));
    const auto cachedCollVersion = ShardVersionFactory::make(
        ChunkVersion(gen, {1, 0}), boost::optional<CollectionIndexes>(boost::none));
    const auto wantedCollVersion = ShardVersionFactory::make(
        ChunkVersion(gen, {2, 0}), boost::optional<CollectionIndexes>(boost::none));

    loadDatabases({DatabaseType(kNss.dbName(), kShards[0], dbVersion)});
    loadCollection(cachedCollVersion);
    _catalogCache->onStaleCollectionVersion(kNss, wantedCollVersion);

    {
        FailPointEnableBlock failPoint("blockCollectionCacheLookup");

        const auto status =
            _catalogCache->getCollectionRoutingInfo(operationContext(), kNss, true /* allowLocks */)
                .getStatus();

        ASSERT(status == ErrorCodes::ShardCannotRefreshDueToLocksHeld);
        auto refreshInfo = status.extraInfo<ShardCannotRefreshDueToLocksHeldInfo>();
        ASSERT(refreshInfo);
    }
    // Cancel ongoing refresh
    _catalogCache->invalidateCollectionEntry_LINEARIZABLE(kNss);
}

TEST_F(CatalogCacheTest, GetCollectionRoutingInfoAllowLocksNeedsToFetchNewDBInfo) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const CollectionGeneration gen(OID::gen(), Timestamp(1, 1));
    const auto cachedCollVersion = ShardVersionFactory::make(
        ChunkVersion(gen, {1, 0}), boost::optional<CollectionIndexes>(boost::none));
    const auto wantedCollVersion = ShardVersionFactory::make(
        ChunkVersion(gen, {2, 0}), boost::optional<CollectionIndexes>(boost::none));

    loadDatabases({DatabaseType(kNss.dbName(), kShards[0], dbVersion)});
    loadCollection(cachedCollVersion);
    _catalogCache->invalidateDatabaseEntry_LINEARIZABLE(kNss.dbName());

    {
        FailPointEnableBlock failPoint("blockDatabaseCacheLookup");

        const auto status =
            _catalogCache->getCollectionRoutingInfo(operationContext(), kNss, true /* allowLocks */)
                .getStatus();

        ASSERT(status == ErrorCodes::ShardCannotRefreshDueToLocksHeld);
        auto refreshInfo = status.extraInfo<ShardCannotRefreshDueToLocksHeldInfo>();
        ASSERT(refreshInfo);
    }
    // Cancel ongoing refresh
    _catalogCache->invalidateDatabaseEntry_LINEARIZABLE(kNss.dbName());
}

TEST_F(CatalogCacheTest, TimeseriesFieldsAreProperlyPropagatedOnCC) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const auto gen = CollectionGeneration(OID::gen(), Timestamp(42));
    const auto version = ShardVersionFactory::make(ChunkVersion(gen, {1, 0}),
                                                   boost::optional<CollectionIndexes>(boost::none));

    loadDatabases({DatabaseType(kNss.dbName(), kShards[0], dbVersion)});

    auto coll = makeCollectionType(version);
    auto chunks = makeChunks(version.placementVersion());
    auto timeseriesOptions = TimeseriesOptions("fieldName");

    // 1st refresh: we should find a bucket granularity of seconds
    {
        TypeCollectionTimeseriesFields tsFields;
        timeseriesOptions.setGranularity(BucketGranularityEnum::Seconds);
        tsFields.setTimeseriesOptions(timeseriesOptions);
        coll.setTimeseriesFields(tsFields);

        const auto scopedCollProv = scopedCollectionProvider(coll);
        const auto scopedChunksProv = scopedChunksProvider(chunks);
        auto future = launchAsync([&] {
            onCommand([&](const executor::RemoteCommandRequest& request) {
                return makeCollectionAndIndexesAggregationResponse(coll, std::vector<BSONObj>());
            });
        });

        const auto swCri =
            _catalogCache->getCollectionRoutingInfo(operationContext(), coll.getNss());
        ASSERT_OK(swCri.getStatus());
        future.default_timed_get();

        const auto& chunkManager = swCri.getValue().cm;
        ASSERT(chunkManager.getTimeseriesFields().has_value());
        ASSERT(chunkManager.getTimeseriesFields()->getGranularity() ==
               BucketGranularityEnum::Seconds);
    }

    // 2nd refresh: we should find a bucket granularity of hours
    {
        TypeCollectionTimeseriesFields tsFields;
        timeseriesOptions.setGranularity(BucketGranularityEnum::Hours);
        tsFields.setTimeseriesOptions(timeseriesOptions);
        coll.setTimeseriesFields(tsFields);

        auto& lastChunk = chunks.back();
        ChunkVersion newCollectionPlacementVersion = lastChunk.getVersion();
        newCollectionPlacementVersion.incMinor();
        lastChunk.setVersion(newCollectionPlacementVersion);

        const auto scopedCollProv = scopedCollectionProvider(coll);
        const auto scopedChunksProv = scopedChunksProvider(std::vector{lastChunk});
        auto future = launchAsync([&] {
            onCommand([&](const executor::RemoteCommandRequest& request) {
                return makeCollectionAndIndexesAggregationResponse(coll, std::vector<BSONObj>());
            });
        });

        _catalogCache->onStaleCollectionVersion(coll.getNss(), boost::none /* wantedVersion */);
        const auto swCri =
            _catalogCache->getCollectionRoutingInfo(operationContext(), coll.getNss());
        ASSERT_OK(swCri.getStatus());
        future.default_timed_get();

        const auto& chunkManager = swCri.getValue().cm;
        ASSERT(chunkManager.getTimeseriesFields().has_value());
        ASSERT(chunkManager.getTimeseriesFields()->getGranularity() ==
               BucketGranularityEnum::Hours);
    }
}

TEST_F(CatalogCacheTest, LookupCollectionWithInvalidOptions) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const auto gen = CollectionGeneration(OID::gen(), Timestamp(42));
    const auto version = ShardVersionFactory::make(ChunkVersion(gen, {1, 0}),
                                                   boost::optional<CollectionIndexes>(boost::none));

    loadDatabases({DatabaseType(kNss.dbName(), kShards[0], dbVersion)});

    auto coll = makeCollectionType(version);

    const auto scopedCollProv = scopedCollectionProvider(coll);
    const auto scopedChunksProv = scopedChunksProvider(StatusWith<std::vector<ChunkType>>(
        ErrorCodes::InvalidOptions, "Testing error with invalid options"));

    const auto swCri = _catalogCache->getCollectionRoutingInfo(operationContext(), coll.getNss());

    ASSERT_EQUALS(swCri.getStatus(), ErrorCodes::InvalidOptions);
}


TEST_F(CatalogCacheTest, OnStaleShardVersionWithGreaterIndexVersion) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const CollectionGeneration gen(OID::gen(), Timestamp(1, 1));
    const auto cachedCollVersion = ShardVersionFactory::make(
        ChunkVersion(gen, {1, 0}), boost::optional<CollectionIndexes>(boost::none));
    const auto wantedCollVersion = ShardVersionFactory::make(
        ChunkVersion(gen, {1, 0}), CollectionIndexes(kUUID, Timestamp(1, 0)));

    loadDatabases({DatabaseType(kNss.dbName(), kShards[0], dbVersion)});
    CollectionType coll = loadCollection(cachedCollVersion);
    _catalogCache->onStaleCollectionVersion(kNss, wantedCollVersion);

    auto future = launchAsync([&] {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            coll.setIndexVersion({coll.getUuid(), Timestamp(1, 0)});
            return makeCollectionAndIndexesAggregationResponse(
                coll,
                {IndexCatalogType("x_1", BSON("x" << 1), BSONObj(), Timestamp(1, 0), coll.getUuid())
                     .toBSON()});
        });
    });

    const auto routingInfo = _catalogCache->getCollectionRoutingInfo(operationContext(), kNss);
    future.default_timed_get();
}

TEST_F(CatalogCacheTest, OnStaleShardVersionIndexVersionBumpNotNone) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const CollectionGeneration gen(OID::gen(), Timestamp(1, 1));
    const auto cachedCollVersion = ShardVersionFactory::make(
        ChunkVersion(gen, {1, 0}), CollectionIndexes(kUUID, Timestamp(1, 0)));
    const auto wantedCollVersion = ShardVersionFactory::make(
        ChunkVersion(gen, {1, 0}), CollectionIndexes(kUUID, Timestamp(2, 0)));

    loadDatabases({DatabaseType(kNss.dbName(), kShards[0], dbVersion)});
    CollectionType coll = loadCollection(cachedCollVersion);
    _catalogCache->onStaleCollectionVersion(kNss, wantedCollVersion);

    auto future = launchAsync([&] {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            coll.setIndexVersion({coll.getUuid(), Timestamp(2, 0)});
            return makeCollectionAndIndexesAggregationResponse(
                coll,
                {IndexCatalogType("x_1", BSON("x" << 1), BSONObj(), Timestamp(2, 0), coll.getUuid())
                     .toBSON()});
        });
    });

    const auto routingInfo = _catalogCache->getCollectionRoutingInfo(operationContext(), kNss);
    future.default_timed_get();
}

TEST_F(CatalogCacheTest, PeekCollectionCacheVersion) {
    // Nothing cached, then peek returns boost::none. Does not attempt to refresh.
    ASSERT_EQ(boost::none, _catalogCache->peekCollectionCacheVersion(kNss));

    // Now force the cache to refresh. Now peek returns the version of whatever has been cached.
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const CollectionGeneration gen(OID::gen(), Timestamp(1, 1));
    const auto cachedCollVersion = ShardVersionFactory::make(
        ChunkVersion(gen, {1, 0}), boost::optional<CollectionIndexes>(boost::none));
    loadDatabases({DatabaseType(kNss.dbName(), kShards[0], dbVersion)});
    loadCollection(cachedCollVersion);

    ASSERT_EQ(cachedCollVersion.placementVersion(),
              _catalogCache->peekCollectionCacheVersion(kNss));

    // If we have cached an untracked collection, then peek returns boost::none.
    _catalogCache->onStaleCollectionVersion(kNss, boost::none);
    loadUnshardedCollection(kNss);

    ASSERT_EQ(boost::none, _catalogCache->peekCollectionCacheVersion(kNss));
}

TEST_F(CatalogCacheTest, AdvanceCollectionTimeInStore) {
    // Cache something.
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const CollectionGeneration gen(OID::gen(), Timestamp(1, 1));
    const auto cachedCollVersion = ShardVersionFactory::make(
        ChunkVersion(gen, {1, 0}), boost::optional<CollectionIndexes>(boost::none));
    loadDatabases({DatabaseType(kNss.dbName(), kShards[0], dbVersion)});
    const auto coll = loadCollection(cachedCollVersion);

    ASSERT_EQ(cachedCollVersion,
              uassertStatusOK(_catalogCache->getCollectionRoutingInfo(operationContext(), kNss))
                  .getCollectionVersion());

    // Advance the time-in-store of the cache.
    auto newCollectionVersion = cachedCollVersion;
    newCollectionVersion.placementVersion().incMajor();
    _catalogCache->advanceCollectionTimeInStore(kNss, newCollectionVersion.placementVersion());

    // getCollectionRoutingInfo should return the new version, because the cache has been notified
    // that it is stale.
    {
        // Put the new version on the loader.
        const auto scopedCollProv = scopedCollectionProvider(coll);
        const auto scopedChunksProv =
            scopedChunksProvider(makeChunks(newCollectionVersion.placementVersion()));

        // getCollectionRoutingInfo will trigger a refresh and see the new version
        ASSERT_EQ(newCollectionVersion,
                  uassertStatusOK(_catalogCache->getCollectionRoutingInfo(operationContext(), kNss))
                      .getCollectionVersion());
    }

    // Call 'advanceCollectionTimeInStore' but with an older version. This should be a no-op.
    _catalogCache->advanceCollectionTimeInStore(kNss, cachedCollVersion.placementVersion());
    ASSERT_EQ(newCollectionVersion,
              uassertStatusOK(_catalogCache->getCollectionRoutingInfo(operationContext(), kNss))
                  .getCollectionVersion());
}

}  // namespace
}  // namespace mongo
