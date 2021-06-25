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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <boost/optional/optional_io.hpp>

#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/catalog_cache_loader_mock.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/type_collection_timeseries_fields_gen.h"

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
        ChunkType chunk(kNss,
                        {kShardKeyPattern.getKeyPattern().globalMin(),
                         kShardKeyPattern.getKeyPattern().globalMax()},
                        version,
                        {"0"});
        chunk.setName(OID::gen());
        return {chunk};
    }

    CollectionType makeCollectionType(const ChunkVersion& collVersion) {
        CollectionType coll(
            kNss, collVersion.epoch(), collVersion.getTimestamp(), Date_t::now(), UUID::gen());
        coll.setKeyPattern(kShardKeyPattern.getKeyPattern());
        coll.setUnique(false);
        return coll;
    }

    const NamespaceString kNss{"catalgoCacheTestDB.foo"};
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
    const auto dbVersion = DatabaseVersion(UUID::gen());
    _catalogCacheLoader->setDatabaseRefreshReturnValue(
        DatabaseType(dbName, kShards[0], true, dbVersion));

    const auto swDatabase = _catalogCache->getDatabase(operationContext(), dbName);

    ASSERT_OK(swDatabase.getStatus());
    const auto cachedDb = swDatabase.getValue();
    ASSERT_TRUE(cachedDb.shardingEnabled());
    ASSERT_EQ(cachedDb.primaryId(), kShards[0]);
    ASSERT_EQ(cachedDb.databaseVersion().getUuid(), dbVersion.getUuid());
    ASSERT_EQ(cachedDb.databaseVersion().getLastMod(), dbVersion.getLastMod());
}

TEST_F(CatalogCacheTest, GetCachedDatabase) {
    const auto dbName = "testDB";
    const auto dbVersion = DatabaseVersion(UUID::gen());
    loadDatabases({DatabaseType(dbName, kShards[0], true, dbVersion)});

    const auto swDatabase = _catalogCache->getDatabase(operationContext(), dbName);

    ASSERT_OK(swDatabase.getStatus());
    const auto cachedDb = swDatabase.getValue();
    ASSERT_TRUE(cachedDb.shardingEnabled());
    ASSERT_EQ(cachedDb.primaryId(), kShards[0]);
    ASSERT_EQ(cachedDb.databaseVersion().getUuid(), dbVersion.getUuid());
    ASSERT_EQ(cachedDb.databaseVersion().getLastMod(), dbVersion.getLastMod());
}

TEST_F(CatalogCacheTest, InvalidateSingleDbOnShardRemoval) {
    const auto dbName = "testDB";
    const auto dbVersion = DatabaseVersion(UUID::gen());
    loadDatabases({DatabaseType(dbName, kShards[0], true, dbVersion)});

    _catalogCache->invalidateEntriesThatReferenceShard(kShards[0]);
    _catalogCacheLoader->setDatabaseRefreshReturnValue(
        DatabaseType(dbName, kShards[1], true, dbVersion));
    const auto swDatabase = _catalogCache->getDatabase(operationContext(), dbName);

    ASSERT_OK(swDatabase.getStatus());
    auto cachedDb = swDatabase.getValue();
    ASSERT_EQ(cachedDb.primaryId(), kShards[1]);
}

TEST_F(CatalogCacheTest, OnStaleDatabaseVersionNoVersion) {
    // onStaleDatabaseVesrsion must invalidate the database entry if invoked with no version
    const auto dbVersion = DatabaseVersion(UUID::gen());
    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], true, dbVersion)});

    _catalogCache->onStaleDatabaseVersion(kNss.db(), boost::none);

    const auto status = _catalogCache->getDatabase(operationContext(), kNss.db()).getStatus();
    ASSERT(status == ErrorCodes::InternalError);
}

TEST_F(CatalogCacheTest, OnStaleShardVersionWithSameVersion) {
    const auto dbVersion = DatabaseVersion(UUID::gen());
    const auto cachedCollVersion = ChunkVersion(1, 0, OID::gen(), boost::none /* timestamp */);

    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], true, dbVersion)});
    loadCollection(cachedCollVersion);
    _catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
        kNss, cachedCollVersion, kShards[0]);
    ASSERT_OK(_catalogCache->getCollectionRoutingInfo(operationContext(), kNss).getStatus());
}

TEST_F(CatalogCacheTest, OnStaleShardVersionWithNoVersion) {
    const auto dbVersion = DatabaseVersion(UUID::gen());
    const auto cachedCollVersion = ChunkVersion(1, 0, OID::gen(), boost::none /* timestamp */);

    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], true, dbVersion)});
    loadCollection(cachedCollVersion);
    _catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
        kNss, boost::none, kShards[0]);
    const auto status =
        _catalogCache->getCollectionRoutingInfo(operationContext(), kNss).getStatus();
    ASSERT(status == ErrorCodes::InternalError);
}

TEST_F(CatalogCacheTest, OnStaleShardVersionWithGraterVersion) {
    const auto dbVersion = DatabaseVersion(UUID::gen());
    const auto cachedCollVersion = ChunkVersion(1, 0, OID::gen(), boost::none /* timestamp */);
    const auto wantedCollVersion =
        ChunkVersion(2, 0, cachedCollVersion.epoch(), cachedCollVersion.getTimestamp());

    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], true, dbVersion)});
    loadCollection(cachedCollVersion);
    _catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
        kNss, wantedCollVersion, kShards[0]);
    const auto status =
        _catalogCache->getCollectionRoutingInfo(operationContext(), kNss).getStatus();
    ASSERT(status == ErrorCodes::InternalError);
}

TEST_F(CatalogCacheTest, GetDatabaseWithMetadataFormatChange) {
    const auto dbName = "testDB";
    const auto uuid = UUID::gen();
    const DatabaseVersion versionWithoutTimestamp(uuid);
    const DatabaseVersion versionWithTimestamp(uuid, Timestamp(42));

    auto getDatabaseWithRefreshAndCheckResults = [&](const DatabaseVersion& version) {
        _catalogCacheLoader->setDatabaseRefreshReturnValue(
            DatabaseType(dbName, kShards[0], true, version));
        const auto cachedDb =
            _catalogCache->getDatabaseWithRefresh(operationContext(), dbName).getValue();
        const auto cachedDbVersion = cachedDb.databaseVersion();
        ASSERT_EQ(cachedDbVersion.getTimestamp(), version.getTimestamp());
    };

    // The CatalogCache is refreshed and it finds a DatabaseType using uuids.
    getDatabaseWithRefreshAndCheckResults(versionWithoutTimestamp);
    // The CatalogCache is forced to refresh and it finds a metadata format missmatch: we are using
    // uuids locally but the loader returns a version with uuid and timestamp. The catalog cache
    // returns a new DatabaseType with the new format.
    getDatabaseWithRefreshAndCheckResults(versionWithTimestamp);
    // The CatalogCache is forced to refresh and it finds a metadata format missmatch: we are using
    // uuids and timestamps locally but the loader returns a version with only uuid. The catalog
    // cache returns a new DatabaseType with the new format.
    getDatabaseWithRefreshAndCheckResults(versionWithoutTimestamp);
}

TEST_F(CatalogCacheTest, GetCollectionWithMetadataFormatChange) {
    const auto dbVersion = DatabaseVersion(UUID::gen());
    const auto epoch = OID::gen();
    const auto collVersionWithoutTimestamp = ChunkVersion(1, 0, epoch, boost::none /* timestamp */);
    const auto collVersionWithTimestamp = ChunkVersion(1, 0, epoch, Timestamp(42));

    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], true, dbVersion)});

    auto getCollectionWithRefreshAndCheckResults = [this](const ChunkVersion& version) {
        const auto coll = makeCollectionType(version);
        const auto scopedCollProv = scopedCollectionProvider(coll);
        const auto scopedChunksProv = scopedChunksProvider(makeChunks(version));

        const auto swChunkManager =
            _catalogCache->getCollectionRoutingInfoWithRefresh(operationContext(), coll.getNss());
        ASSERT_OK(swChunkManager.getStatus());

        const auto& chunkManager = swChunkManager.getValue();
        const auto collectionVersion = chunkManager.getVersion();

        ASSERT_EQ(collectionVersion.getTimestamp(), version.getTimestamp());
        chunkManager.forEachChunk([&](const Chunk& chunk) {
            ASSERT_EQ(chunk.getLastmod().getTimestamp(), version.getTimestamp());
            return true;
        });
    };
    // The CatalogCache is refreshed and it finds a Collection using epochs.
    getCollectionWithRefreshAndCheckResults(collVersionWithoutTimestamp);
    // The CatalogCache is forced to refresh and it finds a metadata format mismatch: we are using
    // epochs locally but the loader returns a version with uuid and timestamp. The catalog cache
    // returns a new ChunkManager with the new format.
    getCollectionWithRefreshAndCheckResults(collVersionWithTimestamp);
    // The CatalogCache is forced to refresh and it finds a metadata format mismatch: we are using
    // epochs and timestamps locally but the loader returns a version with just epochs. The catalog
    // cache returns a new ChunkManager with the new format.
    getCollectionWithRefreshAndCheckResults(collVersionWithoutTimestamp);
}

TEST_F(CatalogCacheTest,
       GetCollectionWithRefreshDuringUpgradeWithMetadataFormatChangeChunksDontMatchCollection) {
    const auto dbVersion = DatabaseVersion(UUID::gen());
    const auto epoch = OID::gen();
    const auto timestamp = Timestamp(42);

    const auto collVersionWithoutTimestamp = ChunkVersion(1, 0, epoch, boost::none /* timestamp */);
    const auto collVersionWithTimestamp = ChunkVersion(1, 0, epoch, timestamp);

    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], true, dbVersion)});

    const auto coll = makeCollectionType(collVersionWithoutTimestamp);
    const auto scopedCollProv = scopedCollectionProvider(coll);
    const auto scopedChunksProv = scopedChunksProvider(makeChunks(collVersionWithTimestamp));

    const auto swChunkManager =
        _catalogCache->getCollectionRoutingInfoWithRefresh(operationContext(), coll.getNss());
    ASSERT_OK(swChunkManager.getStatus());

    const auto& chunkManager = swChunkManager.getValue();
    const auto collectionVersion = chunkManager.getVersion();

    ASSERT_EQ(collectionVersion.getTimestamp(), boost::none);

    chunkManager.forEachChunk([&](const Chunk& chunk) {
        ASSERT_EQ(chunk.getLastmod().getTimestamp(), timestamp);
        return true;
    });
}

TEST_F(CatalogCacheTest,
       GetCollectionWithRefreshDuringUpgradeWithMetadataFormatChangeSomeChunksMatchCollection) {
    const auto dbVersion = DatabaseVersion(UUID::gen());
    const auto epoch = OID::gen();
    const auto timestamp = Timestamp(42);

    const auto collVersionWithoutTimestamp = ChunkVersion(1, 0, epoch, boost::none /* timestamp */);
    const auto collVersionWithTimestamp = ChunkVersion(1, 1, epoch, timestamp);

    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], true, dbVersion)});

    const auto coll = makeCollectionType(collVersionWithoutTimestamp);
    const auto scopedCollProv = scopedCollectionProvider(coll);

    ChunkType chunk1(kNss,
                     {kShardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 100)},
                     collVersionWithTimestamp,
                     {"0"});
    chunk1.setName(OID::gen());

    ChunkType chunk2(kNss,
                     {BSON("_id" << 100), kShardKeyPattern.getKeyPattern().globalMax()},
                     collVersionWithoutTimestamp,
                     {"0"});
    chunk2.setName(OID::gen());

    const auto scopedChunksProv = scopedChunksProvider(std::vector{chunk1, chunk2});

    const auto swChunkManager =
        _catalogCache->getCollectionRoutingInfoWithRefresh(operationContext(), coll.getNss());
    ASSERT_OK(swChunkManager.getStatus());

    const auto& chunkManager = swChunkManager.getValue();
    const auto collectionVersion = chunkManager.getVersion();

    ASSERT_EQ(collectionVersion.getTimestamp(), boost::none);
}

TEST_F(CatalogCacheTest, GetCollectionWithRefreshDuringDowngradeWithMetadataFormatChange) {
    const auto dbVersion = DatabaseVersion(UUID::gen());
    const auto epoch = OID::gen();
    const auto timestamp = Timestamp(42);

    const auto collVersionWithoutTimestamp = ChunkVersion(1, 0, epoch, boost::none /* timestamp */);
    const auto collVersionWithTimestamp = ChunkVersion(1, 0, epoch, timestamp);

    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], true, dbVersion)});

    const auto coll = makeCollectionType(collVersionWithTimestamp);
    const auto scopedCollProv = scopedCollectionProvider(coll);
    const auto scopedChunksProv = scopedChunksProvider(makeChunks(collVersionWithoutTimestamp));

    const auto swChunkManager =
        _catalogCache->getCollectionRoutingInfoWithRefresh(operationContext(), coll.getNss());
    ASSERT_OK(swChunkManager.getStatus());

    const auto& chunkManager = swChunkManager.getValue();
    const auto collectionVersion = chunkManager.getVersion();

    ASSERT_EQ(collectionVersion.getTimestamp(), timestamp);

    chunkManager.forEachChunk([&](const Chunk& chunk) {
        ASSERT_EQ(chunk.getLastmod().getTimestamp(), boost::none);
        return true;
    });
}

TEST_F(CatalogCacheTest, TimeseriesFieldsAreProperlyPropagatedOnCC) {
    const auto dbVersion = DatabaseVersion(UUID::gen());
    const auto epoch = OID::gen();
    const auto version = ChunkVersion(1, 0, epoch, Timestamp(42));

    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], true, dbVersion)});

    auto coll = makeCollectionType(version);
    TypeCollectionTimeseriesFields tsFields;
    tsFields.setTimeseriesOptions(TimeseriesOptions("fieldName"));
    coll.setTimeseriesFields(tsFields);

    const auto scopedCollProv = scopedCollectionProvider(coll);
    const auto scopedChunksProv = scopedChunksProvider(makeChunks(version));

    const auto swChunkManager =
        _catalogCache->getCollectionRoutingInfoWithRefresh(operationContext(), coll.getNss());
    ASSERT_OK(swChunkManager.getStatus());

    const auto& chunkManager = swChunkManager.getValue();
    ASSERT(chunkManager.getTimeseriesFields().is_initialized());
}

TEST_F(CatalogCacheTest, LookupCollectionWithInvalidOptions) {
    const auto dbVersion = DatabaseVersion(UUID::gen());
    const auto epoch = OID::gen();
    const auto version = ChunkVersion(1, 0, epoch, Timestamp(42));

    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], true, dbVersion)});

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
