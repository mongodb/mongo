// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// IWYU pragma: no_include "cxxabi.h"
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/router_role/routing_cache/config_server_catalog_cache_loader_mock.h"
#include "mongo/db/router_role/routing_cache/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <system_error>
#include <tuple>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

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
        _catalogCacheLoader = std::make_shared<ConfigServerCatalogCacheLoaderMock>();
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
        ScopedCollectionProvider(
            std::shared_ptr<ConfigServerCatalogCacheLoaderMock> catalogCacheLoader,
            const StatusWith<CollectionType>& swCollection)
            : _catalogCacheLoader(catalogCacheLoader) {
            _catalogCacheLoader->setCollectionRefreshReturnValue(swCollection);
        }
        ~ScopedCollectionProvider() {
            _catalogCacheLoader->clearCollectionReturnValue();
        }

    private:
        std::shared_ptr<ConfigServerCatalogCacheLoaderMock> _catalogCacheLoader;
    };

    ScopedCollectionProvider scopedCollectionProvider(
        const StatusWith<CollectionType>& swCollection) {
        return {_catalogCacheLoader, swCollection};
    }

    class ScopedChunksProvider {
    public:
        ScopedChunksProvider(std::shared_ptr<ConfigServerCatalogCacheLoaderMock> catalogCacheLoader,
                             const StatusWith<std::vector<ChunkType>>& swChunks)
            : _catalogCacheLoader(catalogCacheLoader) {
            _catalogCacheLoader->setChunkRefreshReturnValue(swChunks);
        }
        ~ScopedChunksProvider() {
            _catalogCacheLoader->clearChunksReturnValue();
        }

    private:
        std::shared_ptr<ConfigServerCatalogCacheLoaderMock> _catalogCacheLoader;
    };

    ScopedChunksProvider scopedChunksProvider(const StatusWith<std::vector<ChunkType>>& swChunks) {
        return {_catalogCacheLoader, swChunks};
    }

    class ScopedDatabaseProvider {
    public:
        ScopedDatabaseProvider(
            std::shared_ptr<ConfigServerCatalogCacheLoaderMock> catalogCacheLoader,
            const StatusWith<DatabaseType>& swDatabase)
            : _catalogCacheLoader(catalogCacheLoader) {
            _catalogCacheLoader->setDatabaseRefreshReturnValue(swDatabase);
        }
        ~ScopedDatabaseProvider() {
            _catalogCacheLoader->clearDatabaseReturnValue();
        }

    private:
        std::shared_ptr<ConfigServerCatalogCacheLoaderMock> _catalogCacheLoader;
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
        const auto swCri =
            _catalogCache->getCollectionRoutingInfo(operationContext(), coll.getNss());
        ASSERT_OK(swCri.getStatus());
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
        return coll;
    }

    const NamespaceString kNss =
        NamespaceString::createNamespaceString_forTest("catalgoCacheTestDB.foo");
    const UUID kUUID = UUID::gen();
    const std::string kPattern{"_id"};
    const ShardKeyPattern kShardKeyPattern{BSON(kPattern << 1)};
    const int kDummyPort{12345};
    const HostAndPort kConfigHostAndPort{"DummyConfig", kDummyPort};
    const std::vector<ShardId> kShards{{"0"}, {"1"}};

    std::shared_ptr<ConfigServerCatalogCacheLoaderMock> _catalogCacheLoader;
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

TEST_F(CatalogCacheTest, AdvanceTimeInStoreForSingleDbOnShardRemoval) {
    // Put initial metadata in the local cache.
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    loadDatabases({DatabaseType(kDbName, kShards[0], dbVersion)});

    _catalogCache->advanceTimeInStoreForEntriesThatReferenceShard(kShards[0]);

    // Put the new metadata in the loader.
    _catalogCacheLoader->setDatabaseRefreshReturnValue(
        DatabaseType(kDbName, kShards[1], dbVersion));

    // Refresh the local cache.
    const auto swDatabase = _catalogCache->getDatabase(operationContext(), kDbName);

    ASSERT_OK(swDatabase.getStatus());
    const auto cachedDb = swDatabase.getValue();
    ASSERT_EQ(cachedDb->getPrimary(), kShards[1]);
}

TEST_F(CatalogCacheTest, AdvanceTimeInStoreForSingleCollectionOnShardRemoval) {
    // Put initial metadata in the local cache.
    const auto initDbVer = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const auto initDbEntry = DatabaseType(kNss.dbName(), kShards[0], initDbVer);
    loadDatabases({initDbEntry});
    const auto initCollVer =
        ShardVersionFactory::make(ChunkVersion({OID::gen(), Timestamp(1, 1)}, {1, 0}));
    loadCollection(initCollVer);

    _catalogCache->advanceTimeInStoreForEntriesThatReferenceShard(kShards[0]);

    // Put the new metadata in the loader.
    const auto newDbEntry = initDbEntry;
    auto newCollVer = initCollVer;
    newCollVer.placementVersion().incMajor();
    const auto newCollEntry = makeCollectionType(newCollVer);
    const auto newChunkEntries = makeChunks(newCollVer.placementVersion());

    _catalogCacheLoader->setDatabaseRefreshReturnValue(newDbEntry);
    _catalogCacheLoader->setCollectionRefreshReturnValue(newCollEntry);
    _catalogCacheLoader->setChunkRefreshReturnValue(newChunkEntries);

    // Refresh the local cache.
    const auto swColl = _catalogCache->getCollectionRoutingInfo(operationContext(), kNss);

    ASSERT_OK(swColl.getStatus());
    auto cachedColl = swColl.getValue();
    ASSERT_EQ(newCollVer, cachedColl.getCollectionVersion());
}

TEST_F(CatalogCacheTest, OnStaleDatabaseVersionNoVersion) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    loadDatabases({DatabaseType(kNss.dbName(), kShards[0], dbVersion)});

    _catalogCache->onStaleDatabaseVersion(kNss.dbName(), boost::none);

    const auto status = _catalogCache->getDatabase(operationContext(), kNss.dbName()).getStatus();
    ASSERT(status == ErrorCodes::InternalError);
}

TEST_F(CatalogCacheTest, OnStaleShardVersionWithSameVersion) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const CollectionGeneration gen(OID::gen(), Timestamp(1, 1));
    const auto cachedCollVersion = ShardVersionFactory::make(ChunkVersion(gen, {1, 0}));

    loadDatabases({DatabaseType(kNss.dbName(), kShards[0], dbVersion)});
    loadCollection(cachedCollVersion);
    _catalogCache->onStaleCollectionVersion(kNss, cachedCollVersion);
    ASSERT_OK(_catalogCache->getCollectionRoutingInfo(operationContext(), kNss).getStatus());
}

TEST_F(CatalogCacheTest, OnStaleShardVersionWithNoVersion) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const CollectionGeneration gen(OID::gen(), Timestamp(1, 1));
    const auto cachedCollVersion = ShardVersionFactory::make(ChunkVersion(gen, {1, 0}));

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
    const auto cachedCollVersion = ShardVersionFactory::make(ChunkVersion(gen, {1, 0}));
    const auto wantedCollVersion = ShardVersionFactory::make(ChunkVersion(gen, {2, 0}));

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
    const auto cachedCollVersion = ShardVersionFactory::make(ChunkVersion(gen, {1, 0}));

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
    const auto cachedCollVersion = ShardVersionFactory::make(ChunkVersion(gen, {1, 0}));
    const auto wantedCollVersion = ShardVersionFactory::make(ChunkVersion(gen, {2, 0}));

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
    const auto cachedCollVersion = ShardVersionFactory::make(ChunkVersion(gen, {1, 0}));
    const auto wantedCollVersion = ShardVersionFactory::make(ChunkVersion(gen, {2, 0}));

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
    const auto version = ShardVersionFactory::make(ChunkVersion(gen, {1, 0}));

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

        const auto swCri =
            _catalogCache->getCollectionRoutingInfo(operationContext(), coll.getNss());
        ASSERT_OK(swCri.getStatus());

        const auto& chunkManager = swCri.getValue().getChunkManager();
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

        _catalogCache->onStaleCollectionVersion(coll.getNss(), boost::none /* wantedVersion */);
        const auto swCri =
            _catalogCache->getCollectionRoutingInfo(operationContext(), coll.getNss());
        ASSERT_OK(swCri.getStatus());

        const auto& chunkManager = swCri.getValue().getChunkManager();
        ASSERT(chunkManager.getTimeseriesFields().has_value());
        ASSERT(chunkManager.getTimeseriesFields()->getGranularity() ==
               BucketGranularityEnum::Hours);
    }
}

TEST_F(CatalogCacheTest, LookupCollectionWithInvalidOptions) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const auto gen = CollectionGeneration(OID::gen(), Timestamp(42));
    const auto version = ShardVersionFactory::make(ChunkVersion(gen, {1, 0}));

    loadDatabases({DatabaseType(kNss.dbName(), kShards[0], dbVersion)});

    auto coll = makeCollectionType(version);

    const auto scopedCollProv = scopedCollectionProvider(coll);
    const auto scopedChunksProv = scopedChunksProvider(StatusWith<std::vector<ChunkType>>(
        ErrorCodes::InvalidOptions, "Testing error with invalid options"));

    const auto swCri = _catalogCache->getCollectionRoutingInfo(operationContext(), coll.getNss());

    ASSERT_EQUALS(swCri.getStatus(), ErrorCodes::InvalidOptions);
}


TEST_F(CatalogCacheTest, PeekCollectionCacheVersion) {
    // Nothing cached, then peek returns boost::none. Does not attempt to refresh.
    ASSERT_EQ(boost::none, _catalogCache->peekCollectionCacheVersion(kNss));

    // Now force the cache to refresh. Now peek returns the version of whatever has been cached.
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const CollectionGeneration gen(OID::gen(), Timestamp(1, 1));
    const auto cachedCollVersion = ShardVersionFactory::make(ChunkVersion(gen, {1, 0}));
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
    const auto cachedCollVersion = ShardVersionFactory::make(ChunkVersion(gen, {1, 0}));
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

TEST_F(CatalogCacheTest,
       LookupDatabaseCalledOnceOnSeveralGetDatabaseWithStaleDatabaseVersionException) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    loadDatabases({DatabaseType(kNss.dbName(), kShards[0], dbVersion)});

    // Receiving StaleDbVersion on catalog cache
    _catalogCache->onStaleDatabaseVersion(kNss.dbName(), boost::none);

    _catalogCacheLoader->setDatabaseRefreshReturnValue(
        DatabaseType(kNss.dbName(), kShards[0], dbVersion));

    // Capture baseline for stats (loadDatabases should have increased them already)
    BSONObjBuilder baselineBuilder;
    _catalogCache->report(&baselineBuilder);
    const auto countBefore = baselineBuilder.obj()["catalogCache"]
                                 .Obj()["countDatabaseFullRefreshesStarted"]
                                 .numberLong();

    // Spawn 10 threads each of which is trying to get database (leading to refresh)
    constexpr size_t kNumThreads = 10;
    std::vector<StatusWith<CachedDatabaseInfo>> results(
        kNumThreads, Status(ErrorCodes::InternalError, "Unknown"));
    std::vector<stdx::thread> threads;
    threads.reserve(kNumThreads);

    {
        FailPointEnableBlock failPoint("blockDatabaseCacheLookup");

        for (size_t i = 0; i < kNumThreads; ++i) {
            threads.emplace_back([this, &results, i] {
                ThreadClient tc("getDatabase-" + std::to_string(i),
                                getGlobalServiceContext()->getService());
                auto opCtx = tc->makeOperationContext();
                results[i] = _catalogCache->getDatabase(opCtx.get(), kNss.dbName());
            });
        }
    }

    for (auto& t : threads) {
        t.join();
    }

    // All 10 getDatabase calls must have succeeded with the correct database info
    for (size_t i = 0; i < kNumThreads; ++i) {
        ASSERT_OK(results[i].getStatus());
        ASSERT_EQ(results[i].getValue()->getPrimary(), kShards[0]);
        ASSERT_EQ(results[i].getValue()->getVersion().getUuid(), dbVersion.getUuid());
        ASSERT_EQ(results[i].getValue()->getVersion().getLastMod(), dbVersion.getLastMod());
    }

    // Despite 10 concurrent callers all seeing a stale cache, only one full refresh
    // was made to the config server
    BSONObjBuilder builder;
    _catalogCache->report(&builder);
    const auto report = builder.obj();
    const auto stats = report["catalogCache"].Obj();
    ASSERT_EQ(1, stats["countDatabaseFullRefreshesStarted"].numberLong() - countBefore);
}

TEST_F(CatalogCacheTest,
       LookupCollectionCalledOnceOnSeveralGetCollectionWithStaleDatabaseVersionException) {
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
    const auto gen = CollectionGeneration(OID::gen(), Timestamp(1, 1));
    const auto collVersion = ShardVersionFactory::make(ChunkVersion(gen, {1, 0}));
    loadDatabases({DatabaseType(kNss.dbName(), kShards[0], dbVersion)});
    loadCollection(collVersion);

    // Receiving StaleCollectionVersion on catalog cache
    _catalogCache->onStaleCollectionVersion(kNss, boost::none);

    _catalogCacheLoader->setCollectionRefreshReturnValue(makeCollectionType(collVersion));
    _catalogCacheLoader->setChunkRefreshReturnValue(makeChunks(collVersion.placementVersion()));

    // Capture baseline for stats (loadCollection should have increased them already)
    BSONObjBuilder baselineBuilder;
    _catalogCache->report(&baselineBuilder);
    const auto countBefore = baselineBuilder.obj()["catalogCache"]
                                 .Obj()["countIncrementalRefreshesStarted"]
                                 .numberLong();

    // Spawn 10 threads each of which is trying to get collection routing info (leading to refresh)
    constexpr size_t kNumThreads = 10;
    std::vector<StatusWith<CollectionRoutingInfo>> results(
        kNumThreads, Status(ErrorCodes::InternalError, "Unknown"));
    std::vector<stdx::thread> threads;
    threads.reserve(kNumThreads);

    {
        FailPointEnableBlock failPoint("blockCollectionCacheLookup");

        for (size_t i = 0; i < kNumThreads; ++i) {
            threads.emplace_back([this, &results, i] {
                ThreadClient tc("getCollection-" + std::to_string(i),
                                getGlobalServiceContext()->getService());
                auto opCtx = tc->makeOperationContext();
                results[i] = _catalogCache->getCollectionRoutingInfo(opCtx.get(), kNss);
            });
        }
    }

    for (auto& t : threads) {
        t.join();
    }

    // All 10 getCollectionRoutingInfo calls must have succeeded with the correct collection info
    for (size_t i = 0; i < kNumThreads; ++i) {
        ASSERT_OK(results[i].getStatus());
        ASSERT_EQ(results[i].getValue().getCollectionVersion(), collVersion);
    }

    // Despite 10 concurrent callers all seeing a stale cache, only one full refresh
    // was made to the config server
    BSONObjBuilder builder;
    _catalogCache->report(&builder);
    const auto report = builder.obj();
    const auto stats = report["catalogCache"].Obj();
    ASSERT_EQ(1, stats["countIncrementalRefreshesStarted"].numberLong() - countBefore);
}
}  // namespace
}  // namespace mongo
