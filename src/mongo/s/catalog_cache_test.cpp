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

#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/catalog_cache_loader_mock.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/type_collection_common_types_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

class CatalogCacheTest : public ShardingTestFixture {
protected:
    void setUp() override {
        ShardingTestFixture::setUp();

        // Setup dummy config server
        setRemote(kConfigHostAndPort);
        configTargeter()->setFindHostReturnValue(kConfigHostAndPort);

        // Setup catalogCache with mock loader
        _catalogCacheLoader = std::make_shared<CatalogCacheLoaderMock>();
        _catalogCache = std::make_unique<CatalogCache>(getServiceContext(), *_catalogCacheLoader);

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
            const auto swDatabase = _catalogCache->getDatabase(operationContext(), db.getName());
            ASSERT_OK(swDatabase.getStatus());
        }
    }

    void loadCollection(const ChunkVersion& version) {
        const auto coll = makeCollectionType(version);
        const auto scopedCollProv = scopedCollectionProvider(coll);
        const auto scopedChunksProv = scopedChunksProvider(makeChunks(version));

        const auto swChunkManager =
            _catalogCache->getCollectionRoutingInfo(operationContext(), coll.getNss());
        ASSERT_OK(swChunkManager.getStatus());
    }

    void loadUnshardedCollection(const NamespaceString& nss) {
        const auto scopedCollProvider =
            scopedCollectionProvider(Status(ErrorCodes::NamespaceNotFound, "collection not found"));

        const auto swChunkManager =
            _catalogCache->getCollectionRoutingInfo(operationContext(), nss);
        ASSERT_OK(swChunkManager.getStatus());
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

    CollectionType makeCollectionType(const ChunkVersion& collVersion) {
        return {kNss,
                collVersion.epoch(),
                collVersion.getTimestamp(),
                Date_t::now(),
                kUUID,
                kShardKeyPattern.getKeyPattern()};
    }

    const NamespaceString kNss{"catalgoCacheTestDB.foo"};
    const UUID kUUID = UUID::gen();
    const std::string kPattern{"_id"};
    const ShardKeyPattern kShardKeyPattern{BSON(kPattern << 1)};
    const int kDummyPort{12345};
    const HostAndPort kConfigHostAndPort{"DummyConfig", kDummyPort};
    const std::vector<ShardId> kShards{{"0"}, {"1"}};

    std::shared_ptr<CatalogCacheLoaderMock> _catalogCacheLoader;
    std::unique_ptr<CatalogCache> _catalogCache;
};

TEST_F(CatalogCacheTest, GetDatabase) {
    const auto dbName = "testDB";
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    _catalogCacheLoader->setDatabaseRefreshReturnValue(DatabaseType(dbName, kShards[0], dbVersion));

    const auto swDatabase = _catalogCache->getDatabase(operationContext(), dbName);

    ASSERT_OK(swDatabase.getStatus());
    const auto cachedDb = swDatabase.getValue();
    ASSERT_EQ(cachedDb->getPrimary(), kShards[0]);
    ASSERT_EQ(cachedDb->getVersion().getUuid(), dbVersion.getUuid());
    ASSERT_EQ(cachedDb->getVersion().getLastMod(), dbVersion.getLastMod());
}

TEST_F(CatalogCacheTest, GetCachedDatabase) {
    const auto dbName = "testDB";
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    loadDatabases({DatabaseType(dbName, kShards[0], dbVersion)});

    const auto swDatabase = _catalogCache->getDatabase(operationContext(), dbName);

    ASSERT_OK(swDatabase.getStatus());
    const auto cachedDb = swDatabase.getValue();
    ASSERT_EQ(cachedDb->getPrimary(), kShards[0]);
    ASSERT_EQ(cachedDb->getVersion().getUuid(), dbVersion.getUuid());
    ASSERT_EQ(cachedDb->getVersion().getLastMod(), dbVersion.getLastMod());
}

TEST_F(CatalogCacheTest, GetDatabaseDrop) {
    const auto dbName = "testDB";
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));

    _catalogCacheLoader->setDatabaseRefreshReturnValue(DatabaseType(dbName, kShards[0], dbVersion));

    // The CatalogCache doesn't have any valid info about this DB and finds a new DatabaseType
    auto swDatabase = _catalogCache->getDatabase(operationContext(), dbName);
    ASSERT_OK(swDatabase.getStatus());
    const auto cachedDb = swDatabase.getValue();
    ASSERT_EQ(cachedDb->getVersion().getUuid(), dbVersion.getUuid());
    ASSERT_EQ(cachedDb->getVersion().getLastMod(), dbVersion.getLastMod());

    // Advancing the timeInStore, e.g. because of a movePrimary
    _catalogCache->onStaleDatabaseVersion(dbName, dbVersion.makeUpdated());

    // However, when this CatalogCache asks to the loader for the new info associated to dbName it
    // didn't find any (i.e. the database was dropped)
    _catalogCacheLoader->setDatabaseRefreshReturnValue(
        Status(ErrorCodes::NamespaceNotFound, "dummy errmsg"));

    // Finally, the CatalogCache shouldn't find the Database
    swDatabase = _catalogCache->getDatabase(operationContext(), dbName);
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, swDatabase.getStatus());
}

TEST_F(CatalogCacheTest, InvalidateSingleDbOnShardRemoval) {
    const auto dbName = "testDB";
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    loadDatabases({DatabaseType(dbName, kShards[0], dbVersion)});

    _catalogCache->invalidateEntriesThatReferenceShard(kShards[0]);
    _catalogCacheLoader->setDatabaseRefreshReturnValue(DatabaseType(dbName, kShards[1], dbVersion));
    const auto swDatabase = _catalogCache->getDatabase(operationContext(), dbName);

    ASSERT_OK(swDatabase.getStatus());
    auto cachedDb = swDatabase.getValue();
    ASSERT_EQ(cachedDb->getPrimary(), kShards[1]);
}

TEST_F(CatalogCacheTest, OnStaleDatabaseVersionNoVersion) {
    // onStaleDatabaseVesrsion must invalidate the database entry if invoked with no version
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], dbVersion)});

    _catalogCache->onStaleDatabaseVersion(kNss.db(), boost::none);

    const auto status = _catalogCache->getDatabase(operationContext(), kNss.db()).getStatus();
    ASSERT(status == ErrorCodes::InternalError);
}

TEST_F(CatalogCacheTest, OnStaleShardVersionWithSameVersion) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const CollectionGeneration gen(OID::gen(), Timestamp(1, 1));
    const auto cachedCollVersion =
        ShardVersion(ChunkVersion(gen, {1, 0}), CollectionIndexes(gen, boost::none));

    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], dbVersion)});
    loadCollection(cachedCollVersion);
    _catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
        kNss, cachedCollVersion, kShards[0]);
    ASSERT_OK(_catalogCache->getCollectionRoutingInfo(operationContext(), kNss).getStatus());
}

TEST_F(CatalogCacheTest, OnStaleShardVersionWithNoVersion) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const auto cachedCollVersion = ChunkVersion({OID::gen(), Timestamp(1, 1)}, {1, 0});

    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], dbVersion)});
    loadCollection(cachedCollVersion);
    _catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
        kNss, boost::none, kShards[0]);
    const auto status =
        _catalogCache->getCollectionRoutingInfo(operationContext(), kNss).getStatus();
    ASSERT(status == ErrorCodes::InternalError);
}

TEST_F(CatalogCacheTest, OnStaleShardVersionWithGraterVersion) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const auto cachedCollVersion = ChunkVersion({OID::gen(), Timestamp(1, 1)}, {1, 0});
    const auto wantedCollVersion = ShardVersion(ChunkVersion(cachedCollVersion, {2, 0}),
                                                CollectionIndexes(cachedCollVersion, boost::none));

    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], dbVersion)});
    loadCollection(cachedCollVersion);
    _catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
        kNss, wantedCollVersion, kShards[0]);
    const auto status =
        _catalogCache->getCollectionRoutingInfo(operationContext(), kNss).getStatus();
    ASSERT(status == ErrorCodes::InternalError);
}

TEST_F(CatalogCacheTest, TimeseriesFieldsAreProperlyPropagatedOnCC) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const auto epoch = OID::gen();
    const auto version = ChunkVersion({epoch, Timestamp(42)}, {1, 0});

    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], dbVersion)});

    auto coll = makeCollectionType(version);
    auto chunks = makeChunks(version);
    auto timeseriesOptions = TimeseriesOptions("fieldName");

    // 1st refresh: we should find a bucket granularity of seconds (default)
    {
        TypeCollectionTimeseriesFields tsFields;
        tsFields.setTimeseriesOptions(timeseriesOptions);
        coll.setTimeseriesFields(tsFields);

        const auto scopedCollProv = scopedCollectionProvider(coll);
        const auto scopedChunksProv = scopedChunksProvider(chunks);

        const auto swChunkManager =
            _catalogCache->getCollectionRoutingInfoWithRefresh(operationContext(), coll.getNss());
        ASSERT_OK(swChunkManager.getStatus());

        const auto& chunkManager = swChunkManager.getValue();
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
        ChunkVersion newCollectionVersion = lastChunk.getVersion();
        newCollectionVersion.incMinor();
        lastChunk.setVersion(newCollectionVersion);

        const auto scopedCollProv = scopedCollectionProvider(coll);
        const auto scopedChunksProv = scopedChunksProvider(std::vector{lastChunk});

        const auto swChunkManager =
            _catalogCache->getCollectionRoutingInfoWithRefresh(operationContext(), coll.getNss());
        ASSERT_OK(swChunkManager.getStatus());

        const auto& chunkManager = swChunkManager.getValue();
        ASSERT(chunkManager.getTimeseriesFields().has_value());
        ASSERT(chunkManager.getTimeseriesFields()->getGranularity() ==
               BucketGranularityEnum::Hours);
    }
}

TEST_F(CatalogCacheTest, LookupCollectionWithInvalidOptions) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const auto epoch = OID::gen();
    const auto version = ChunkVersion({epoch, Timestamp(42)}, {1, 0});

    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], dbVersion)});

    auto coll = makeCollectionType(version);

    const auto scopedCollProv = scopedCollectionProvider(coll);
    const auto scopedChunksProv = scopedChunksProvider(StatusWith<std::vector<ChunkType>>(
        ErrorCodes::InvalidOptions, "Testing error with invalid options"));

    const auto swChunkManager =
        _catalogCache->getCollectionRoutingInfoWithRefresh(operationContext(), coll.getNss());

    ASSERT_EQUALS(swChunkManager.getStatus(), ErrorCodes::InvalidOptions);
}

}  // namespace
}  // namespace mongo
