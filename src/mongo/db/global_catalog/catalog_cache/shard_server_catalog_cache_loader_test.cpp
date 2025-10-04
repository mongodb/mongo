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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/catalog_cache/config_server_catalog_cache_loader_mock.h"
#include "mongo/db/global_catalog/catalog_cache/shard_server_catalog_cache_loader_impl.h"
#include "mongo/db/global_catalog/ddl/shard_metadata_util.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/global_catalog/type_shard_collection.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <type_traits>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

using std::vector;
using CollectionAndChangedChunks = CatalogCacheLoader::CollectionAndChangedChunks;

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("foo.bar");
const KeyPattern kKeyPattern = KeyPattern(BSON("a" << 1));
const KeyPattern kKeyPattern2 = KeyPattern(BSON("a" << 1 << "b" << 1));
const ShardId kShardId = ShardId("shard0");
constexpr int maxAttempts = 5;

template <typename T>
auto retryIfFailedAs(int maxAttempts,
                     std::function<bool(const DBException& e)> shouldRetry,
                     T&& f) {
    int attempt = 1;
    while (true) {
        try {
            return f();
        } catch (const DBException& ex) {
            if (attempt == maxAttempts || !shouldRetry(ex)) {
                throw;
            }
            attempt++;
        }
    }
}

bool isRetryableCatalogError(const DBException& ex) {
    return ex.isA<ErrorCategory::SnapshotError>() ||
        ex.code() == ErrorCodes::ConflictingOperationInProgress ||
        ex.code() == ErrorCodes::QueryPlanKilled;
}

CollectionAndChangedChunks retryableGetChunksSince(ShardServerCatalogCacheLoader* _shardLoader,
                                                   const NamespaceString& nss,
                                                   const ChunkVersion& version) {

    return retryIfFailedAs(maxAttempts, isRetryableCatalogError, [&]() {
        // Ensure that we resolve the future here, and capture retriable errors.
        return _shardLoader->getChunksSince(kNss, version).get();
    });
}


class ShardServerCatalogCacheLoaderTest : public ShardServerTestFixture {
public:
    /**
     * Returns five chunks using collectionPlacementVersion as a starting version, and
     * shardKeyPattern 'kKeyPattern'.
     */
    vector<ChunkType> makeFiveChunks(const ChunkVersion& collectionPlacementVersion);

    /**
     * Returns five chunks using collectionPlacementVersion as a starting version, and
     * shardKeyPattern 'kKeyPattern2'.
     */
    vector<ChunkType> makeFiveRefinedChunks(const ChunkVersion& collectionPlacementVersion);

    /**
     * Returns a chunk update diff GTE 'collectionPlacementVersion' for the chunks created by
     * makeFiveChunks above.
     */
    vector<ChunkType> makeThreeUpdatedChunksDiff(const ChunkVersion& collectionPlacementVersion);

    /**
     * Returns a routing table applying 'threeUpdatedChunks' (the result of
     * makeThreeUpdatedChunksDiff) to 'originalFiveChunks' (the result of makeFiveChunks).
     */
    vector<ChunkType> makeCombinedOriginalFiveChunksAndThreeNewChunksDiff(
        const vector<ChunkType>& originalFiveChunks, const vector<ChunkType>& threeUpdatedChunks);

    /**
     * This helper makes a CollectionType with the current _maxCollPlacementVersion.
     */
    CollectionType makeCollectionType(const ChunkVersion& collPlacementVersion,
                                      const KeyPattern& shardKeyPattern = kKeyPattern);

    /**
     * Sets up the _shardLoader with the results of makeFiveChunks().
     */
    std::pair<CollectionType, vector<ChunkType>> setUpChunkLoaderWithFiveChunks();

    void refreshCollectionEpochOnRemoteLoader();
    void refreshDatabaseOnRemoteLoader();

    ConfigServerCatalogCacheLoaderMock* _remoteLoaderMock;
    std::unique_ptr<ShardServerCatalogCacheLoader> _shardLoader;

private:
    void setUp() override;
    void tearDown() override;
};

void ShardServerCatalogCacheLoaderTest::setUp() {
    ShardServerTestFixture::setUp();

    // Create mock remote and real shard loader, retaining a pointer to the mock remote loader so
    // that unit tests can manipulate it to return certain responses.
    auto mockLoader = std::make_unique<ConfigServerCatalogCacheLoaderMock>();
    _remoteLoaderMock = mockLoader.get();
    _shardLoader = std::make_unique<ShardServerCatalogCacheLoaderImpl>(std::move(mockLoader));

    // Set the shard loader to primary mode, and set it for testing.
    _shardLoader->initializeReplicaSetRole(true);
}

void ShardServerCatalogCacheLoaderTest::tearDown() {
    _shardLoader->shutDown();
    _shardLoader.reset();
    ShardServerTestFixture::tearDown();
}

vector<ChunkType> ShardServerCatalogCacheLoaderTest::makeFiveChunks(
    const ChunkVersion& collectionPlacementVersion) {
    ChunkVersion collPlacementVersion(collectionPlacementVersion);
    vector<ChunkType> chunks;
    const UUID uuid = UUID::gen();

    BSONObj mins[] = {
        BSON("a" << MINKEY), BSON("a" << 10), BSON("a" << 50), BSON("a" << 100), BSON("a" << 200)};
    BSONObj maxs[] = {
        BSON("a" << 10), BSON("a" << 50), BSON("a" << 100), BSON("a" << 200), BSON("a" << MAXKEY)};

    for (int i = 0; i < 5; ++i) {
        collPlacementVersion.incMajor();

        ChunkType chunk;
        chunk.setCollectionUUID(uuid);
        chunk.setRange({mins[i], maxs[i]});
        chunk.setShard(kShardId);
        chunk.setVersion(collPlacementVersion);

        chunks.push_back(chunk);
    }

    return chunks;
}

vector<ChunkType> ShardServerCatalogCacheLoaderTest::makeFiveRefinedChunks(
    const ChunkVersion& collectionPlacementVersion) {
    ChunkVersion collPlacementVersion(collectionPlacementVersion);
    vector<ChunkType> chunks;
    const UUID uuid = UUID::gen();

    BSONObj mins[] = {BSON("a" << MINKEY << "b" << MINKEY),
                      BSON("a" << 10 << "b" << MINKEY),
                      BSON("a" << 50 << "b" << MINKEY),
                      BSON("a" << 100 << "b" << MINKEY),
                      BSON("a" << 200 << "b" << MINKEY)};
    BSONObj maxs[] = {BSON("a" << 10 << "b" << MINKEY),
                      BSON("a" << 50 << "b" << MINKEY),
                      BSON("a" << 100 << "b" << MINKEY),
                      BSON("a" << 200 << "b" << MINKEY),
                      BSON("a" << MAXKEY << "b" << MAXKEY)};

    for (int i = 0; i < 5; ++i) {
        collPlacementVersion.incMajor();

        ChunkType chunk;
        chunk.setCollectionUUID(uuid);
        chunk.setRange({mins[i], maxs[i]});
        chunk.setShard(kShardId);
        chunk.setVersion(collPlacementVersion);

        chunks.push_back(chunk);
    }

    return chunks;
}

vector<ChunkType> ShardServerCatalogCacheLoaderTest::makeThreeUpdatedChunksDiff(
    const ChunkVersion& collectionPlacementVersion) {
    ChunkVersion collPlacementVersion(collectionPlacementVersion);
    vector<ChunkType> chunks;
    const UUID uuid = UUID::gen();

    // The diff query is for GTE a known version, so prepend the previous newest chunk, which is
    // unmodified by this change and so should be found. Note: it is important for testing that the
    // previous highest versioned chunk is unmodified. Otherwise the shard loader's results are
    // dependent on a race between persistence and retrieving data because it combines enqueued and
    // persisted results without applying modifications.
    ChunkType oldChunk;
    oldChunk.setCollectionUUID(uuid);
    oldChunk.setRange({BSON("a" << 200), BSON("a" << MAXKEY)});
    oldChunk.setShard(kShardId);
    oldChunk.setVersion(collPlacementVersion);
    chunks.push_back(oldChunk);


    // Make chunk updates
    BSONObj mins[] = {BSON("a" << MINKEY), BSON("a" << 5), BSON("a" << 10)};
    BSONObj maxs[] = {BSON("a" << 5), BSON("a" << 10), BSON("a" << 100)};

    for (int i = 0; i < 3; ++i) {
        collPlacementVersion.incMinor();

        ChunkType chunk;
        chunk.setCollectionUUID(uuid);
        chunk.setRange({mins[i], maxs[i]});
        chunk.setShard(kShardId);
        chunk.setVersion(collPlacementVersion);

        chunks.push_back(chunk);
    }

    return chunks;
}

vector<ChunkType>
ShardServerCatalogCacheLoaderTest::makeCombinedOriginalFiveChunksAndThreeNewChunksDiff(
    const vector<ChunkType>& originalFiveChunks, const vector<ChunkType>& threeUpdatedChunksDiff) {
    vector<ChunkType> chunks;

    // Sorted by ascending chunk version, not range! Note, threeUpdatedChunksDiff already includes
    // the last originalFiveChunks chunk because the diff query is GTE.
    chunks.push_back(originalFiveChunks[3]);
    chunks.insert(chunks.end(), threeUpdatedChunksDiff.begin(), threeUpdatedChunksDiff.end());

    return chunks;
}

CollectionType ShardServerCatalogCacheLoaderTest::makeCollectionType(
    const ChunkVersion& collPlacementVersion, const KeyPattern& shardKeyPattern) {
    return {kNss,
            collPlacementVersion.epoch(),
            collPlacementVersion.getTimestamp(),
            Date_t::now(),
            UUID::gen(),
            shardKeyPattern};
}

std::pair<CollectionType, vector<ChunkType>>
ShardServerCatalogCacheLoaderTest::setUpChunkLoaderWithFiveChunks() {
    ChunkVersion collectionPlacementVersion({OID::gen(), Timestamp(1, 1)}, {1, 0});

    CollectionType collectionType = makeCollectionType(collectionPlacementVersion);
    vector<ChunkType> chunks = makeFiveChunks(collectionPlacementVersion);
    _remoteLoaderMock->setCollectionRefreshReturnValue(collectionType);
    _remoteLoaderMock->setChunkRefreshReturnValue(chunks);

    auto collAndChunksRes =
        retryableGetChunksSince(_shardLoader.get(), kNss, ChunkVersion::UNSHARDED());

    ASSERT_EQUALS(collAndChunksRes.epoch, collectionType.getEpoch());
    ASSERT_EQUALS(collAndChunksRes.changedChunks.size(), 5UL);
    ASSERT(!collAndChunksRes.timeseriesFields.has_value());
    for (unsigned int i = 0; i < collAndChunksRes.changedChunks.size(); ++i) {
        ASSERT_BSONOBJ_EQ(collAndChunksRes.changedChunks[i].toShardBSON(), chunks[i].toShardBSON());
    }

    return std::pair{collectionType, std::move(chunks)};
}

void ShardServerCatalogCacheLoaderTest::refreshDatabaseOnRemoteLoader() {
    DatabaseType databaseType(
        kNss.dbName(), kShardId, DatabaseVersion{UUID::gen(), Timestamp{1, 1}});
    _remoteLoaderMock->setDatabaseRefreshReturnValue(std::move(databaseType));
}

TEST_F(ShardServerCatalogCacheLoaderTest, PrimaryLoadFromUnshardedToUnsharded) {
    // Return a NamespaceNotFound error that means the collection doesn't exist.

    Status errorStatus = Status(ErrorCodes::NamespaceNotFound, "collection not found");
    _remoteLoaderMock->setCollectionRefreshReturnValue(errorStatus);

    ASSERT_THROWS_CODE_AND_WHAT(
        retryableGetChunksSince(_shardLoader.get(), kNss, ChunkVersion::UNSHARDED()),
        DBException,
        errorStatus.code(),
        errorStatus.reason());
}

TEST_F(ShardServerCatalogCacheLoaderTest, PrimaryLoadFromShardedToUnsharded) {
    // First set up the shard chunk loader as sharded.
    auto collAndChunks = setUpChunkLoaderWithFiveChunks();
    auto& chunks = collAndChunks.second;

    // Then return a NamespaceNotFound error, which means the collection must have been dropped,
    // clearing the chunk metadata.

    Status errorStatus = Status(ErrorCodes::NamespaceNotFound, "collection not found");
    _remoteLoaderMock->setCollectionRefreshReturnValue(errorStatus);

    ASSERT_THROWS_CODE_AND_WHAT(
        retryableGetChunksSince(_shardLoader.get(), kNss, chunks.back().getVersion()),
        DBException,
        errorStatus.code(),
        errorStatus.reason());
}

TEST_F(ShardServerCatalogCacheLoaderTest, PrimaryLoadFromShardedAndFindNoDiff) {
    // First set up the shard chunk loader as sharded.
    auto collAndChunks = setUpChunkLoaderWithFiveChunks();
    auto& chunks = collAndChunks.second;

    // Then set up the remote loader to return a single document we've already seen -- indicates
    // there's nothing new.

    vector<ChunkType> lastChunk;
    lastChunk.push_back(chunks.back());
    _remoteLoaderMock->setChunkRefreshReturnValue(lastChunk);

    auto collAndChunksRes =
        retryableGetChunksSince(_shardLoader.get(), kNss, chunks.back().getVersion());

    // Check that refreshing from the latest version returned a single document matching that
    // version.
    ASSERT_EQUALS(collAndChunksRes.epoch, chunks.back().getVersion().epoch());
    ASSERT_EQUALS(collAndChunksRes.changedChunks.size(), 1UL);
    ASSERT_BSONOBJ_EQ(collAndChunksRes.changedChunks.back().toShardBSON(),
                      chunks.back().toShardBSON());
}

// Same as the above unit test, PrimaryLoadFromShardedAndFindNoDiff, but caller requests complete
// routing table, rather than diff from a known version.
TEST_F(ShardServerCatalogCacheLoaderTest, PrimaryLoadFromShardedAndFindNoDiffRequestAll) {
    // First set up the shard chunk loader as sharded.
    auto collAndChunks = setUpChunkLoaderWithFiveChunks();
    auto& chunks = collAndChunks.second;

    // Then set up the remote loader to return a single document we've already seen -- indicates
    // there's nothing new.

    vector<ChunkType> lastChunk;
    lastChunk.push_back(chunks.back());
    _remoteLoaderMock->setChunkRefreshReturnValue(lastChunk);

    auto collAndChunksRes =
        retryableGetChunksSince(_shardLoader.get(), kNss, ChunkVersion::UNSHARDED());
    ASSERT_EQUALS(collAndChunksRes.epoch, chunks.back().getVersion().epoch());
    ASSERT_EQUALS(collAndChunksRes.changedChunks.size(), 5UL);
    for (unsigned int i = 0; i < collAndChunksRes.changedChunks.size(); ++i) {
        ASSERT_BSONOBJ_EQ(collAndChunksRes.changedChunks[i].getMin(), chunks[i].getMin());
        ASSERT_BSONOBJ_EQ(collAndChunksRes.changedChunks[i].getMax(), chunks[i].getMax());
        ASSERT_EQUALS(collAndChunksRes.changedChunks[i].getVersion(), chunks[i].getVersion());
    }
}

TEST_F(ShardServerCatalogCacheLoaderTest, PrimaryLoadFromShardedAndFindDiff) {
    // First set up the shard chunk loader as sharded.
    auto collAndChunks = setUpChunkLoaderWithFiveChunks();
    auto& chunks = collAndChunks.second;

    // Then refresh again and find updated chunks.

    ChunkVersion collPlacementVersion = chunks.back().getVersion();
    vector<ChunkType> updatedChunksDiff = makeThreeUpdatedChunksDiff(collPlacementVersion);
    _remoteLoaderMock->setChunkRefreshReturnValue(updatedChunksDiff);

    auto collAndChunksRes =
        retryableGetChunksSince(_shardLoader.get(), kNss, chunks.back().getVersion());

    // Check that the diff was returned successfull.
    ASSERT_EQUALS(collAndChunksRes.epoch, updatedChunksDiff.front().getVersion().epoch());
    ASSERT_EQUALS(collAndChunksRes.changedChunks.size(), 4UL);
    for (unsigned int i = 0; i < collAndChunksRes.changedChunks.size(); ++i) {
        ASSERT_BSONOBJ_EQ(collAndChunksRes.changedChunks[i].getMin(),
                          updatedChunksDiff[i].getMin());
        ASSERT_BSONOBJ_EQ(collAndChunksRes.changedChunks[i].getMax(),
                          updatedChunksDiff[i].getMax());
        ASSERT_EQUALS(collAndChunksRes.changedChunks[i].getVersion(),
                      updatedChunksDiff[i].getVersion());
    }
}

TEST_F(ShardServerCatalogCacheLoaderTest, PrimaryLoadFromShardedAndFindDiffRequestAll) {
    // First set up the shard chunk loader as sharded.
    auto collAndChunks = setUpChunkLoaderWithFiveChunks();
    auto& chunks = collAndChunks.second;

    // First cause a remote refresh to find the updated chunks. Then wait for persistence, so that
    // we ensure that nothing is enqueued and the next getChunksSince call will return a predictable
    // number of chunk documents: the result of applying the enqueued update diff.

    vector<ChunkType> updatedChunksDiff = makeThreeUpdatedChunksDiff(chunks.back().getVersion());
    _remoteLoaderMock->setChunkRefreshReturnValue(updatedChunksDiff);

    retryableGetChunksSince(_shardLoader.get(), kNss, chunks.back().getVersion());

    // Wait for persistence of update
    _shardLoader->waitForCollectionFlush(operationContext(), kNss);

    // Set up the remote loader to return a single document we've already seen, indicating no change
    // occurred.
    vector<ChunkType> lastChunk;
    lastChunk.push_back(updatedChunksDiff.back());
    _remoteLoaderMock->setChunkRefreshReturnValue(lastChunk);

    vector<ChunkType> completeRoutingTableWithDiffApplied =
        makeCombinedOriginalFiveChunksAndThreeNewChunksDiff(chunks, updatedChunksDiff);

    auto collAndChunksRes =
        retryableGetChunksSince(_shardLoader.get(), kNss, ChunkVersion::UNSHARDED());
    ASSERT_EQUALS(collAndChunksRes.epoch,
                  completeRoutingTableWithDiffApplied.front().getVersion().epoch());
    ASSERT_EQUALS(collAndChunksRes.changedChunks.size(), 5UL);
    for (unsigned int i = 0; i < collAndChunksRes.changedChunks.size(); ++i) {
        ASSERT_BSONOBJ_EQ(collAndChunksRes.changedChunks[i].getMin(),
                          completeRoutingTableWithDiffApplied[i].getMin());
        ASSERT_BSONOBJ_EQ(collAndChunksRes.changedChunks[i].getMax(),
                          completeRoutingTableWithDiffApplied[i].getMax());
        ASSERT_EQUALS(collAndChunksRes.changedChunks[i].getVersion(),
                      completeRoutingTableWithDiffApplied[i].getVersion());
    }
}

TEST_F(ShardServerCatalogCacheLoaderTest, PrimaryLoadFromShardedAndFindNewEpoch) {
    // First set up the shard chunk loader as sharded.
    auto collAndChunks = setUpChunkLoaderWithFiveChunks();
    auto& chunks = collAndChunks.second;

    // Then refresh again and find that the collection has been dropped and recreated.

    ChunkVersion collPlacementVersionWithNewEpoch({OID::gen(), Timestamp(2, 0)}, {1, 0});
    CollectionType collectionTypeWithNewEpoch =
        makeCollectionType(collPlacementVersionWithNewEpoch);
    vector<ChunkType> chunksWithNewEpoch = makeFiveChunks(collPlacementVersionWithNewEpoch);
    _remoteLoaderMock->setCollectionRefreshReturnValue(collectionTypeWithNewEpoch);
    _remoteLoaderMock->setChunkRefreshReturnValue(chunksWithNewEpoch);

    auto collAndChunksRes =
        retryableGetChunksSince(_shardLoader.get(), kNss, chunks.back().getVersion());
    ASSERT_EQUALS(collAndChunksRes.epoch, collectionTypeWithNewEpoch.getEpoch());
    ASSERT_EQUALS(collAndChunksRes.changedChunks.size(), 5UL);
    for (unsigned int i = 0; i < collAndChunksRes.changedChunks.size(); ++i) {
        ASSERT_BSONOBJ_EQ(collAndChunksRes.changedChunks[i].getMin(),
                          chunksWithNewEpoch[i].getMin());
        ASSERT_BSONOBJ_EQ(collAndChunksRes.changedChunks[i].getMax(),
                          chunksWithNewEpoch[i].getMax());
        ASSERT_EQUALS(collAndChunksRes.changedChunks[i].getVersion(),
                      chunksWithNewEpoch[i].getVersion());
    }
}

TEST_F(ShardServerCatalogCacheLoaderTest, PrimaryLoadFromShardedAndFindMixedChunkVersions) {
    // First set up the shard chunk loader as sharded.
    auto collAndChunks = setUpChunkLoaderWithFiveChunks();
    auto& chunks = collAndChunks.second;

    // Then refresh again and retrieve chunks from the config server that have mixed epoches, like
    // as if the chunks read yielded around a drop and recreate of the collection.

    ChunkVersion collPlacementVersionWithNewEpoch({OID::gen(), Timestamp(2, 0)}, {1, 0});
    CollectionType collectionTypeWithNewEpoch =
        makeCollectionType(collPlacementVersionWithNewEpoch);
    vector<ChunkType> chunksWithNewEpoch = makeFiveChunks(collPlacementVersionWithNewEpoch);
    vector<ChunkType> mixedChunks;
    mixedChunks.push_back(chunks.back());
    mixedChunks.insert(mixedChunks.end(), chunksWithNewEpoch.begin(), chunksWithNewEpoch.end());
    _remoteLoaderMock->setChunkRefreshReturnValue(mixedChunks);

    // This test forces a ConflictingOperationInProgress, which is a transient error. We are not
    // assuming any implementation of getChunksSince in the test, and it could be that other
    // transient errors have higher priority. That's why we are asking for retries in the test.
    ASSERT_THROWS_CODE(
        retryIfFailedAs(
            maxAttempts,
            [&](const DBException& ex) {
                return ex.isA<ErrorCategory::SnapshotError>() ||
                    ex.code() == ErrorCodes::QueryPlanKilled;
            },
            [&] { _shardLoader->getChunksSince(kNss, chunks.back().getVersion()).get(); }),
        DBException,
        ErrorCodes::ConflictingOperationInProgress);

    // Now make sure the newly recreated collection is cleanly loaded. We cannot ensure a
    // non-variable response until the loader has remotely retrieved the new metadata and applied
    // them to the persisted store. So first do a reload and ignore the results. Then call again,
    // this time checking the results.

    _remoteLoaderMock->setCollectionRefreshReturnValue(collectionTypeWithNewEpoch);
    _remoteLoaderMock->setChunkRefreshReturnValue(chunksWithNewEpoch);

    retryableGetChunksSince(_shardLoader.get(), kNss, chunks.back().getVersion());

    // Wait for persistence of update.
    _shardLoader->waitForCollectionFlush(operationContext(), kNss);

    vector<ChunkType> lastChunkWithNewEpoch;
    lastChunkWithNewEpoch.push_back(chunksWithNewEpoch.back());
    _remoteLoaderMock->setChunkRefreshReturnValue(lastChunkWithNewEpoch);

    auto collAndChunksRes =
        retryableGetChunksSince(_shardLoader.get(), kNss, ChunkVersion::UNSHARDED());
    ASSERT_EQUALS(collAndChunksRes.epoch, collectionTypeWithNewEpoch.getEpoch());
    ASSERT_EQUALS(collAndChunksRes.changedChunks.size(), 5UL);
    for (unsigned int i = 0; i < collAndChunksRes.changedChunks.size(); ++i) {
        ASSERT_BSONOBJ_EQ(collAndChunksRes.changedChunks[i].getMin(),
                          chunksWithNewEpoch[i].getMin());
        ASSERT_BSONOBJ_EQ(collAndChunksRes.changedChunks[i].getMax(),
                          chunksWithNewEpoch[i].getMax());
        ASSERT_EQUALS(collAndChunksRes.changedChunks[i].getVersion(),
                      chunksWithNewEpoch[i].getVersion());
    }
}

TEST_F(ShardServerCatalogCacheLoaderTest, PersistedCachedDataIsDroppedWhenCorrupted) {
    //  Required fields for documents in config.cache.collections
    std::vector<StringData> requiredFieldNames = {ShardCollectionType::kEpochFieldName,
                                                  ShardCollectionType::kTimestampFieldName,
                                                  ShardCollectionType::kUuidFieldName,
                                                  ShardCollectionType::kKeyPatternFieldName,
                                                  ShardCollectionType::kUniqueFieldName};

    const auto collAndChunks = setUpChunkLoaderWithFiveChunks();
    _shardLoader->waitForCollectionFlush(operationContext(), kNss);
    const auto persistedCollection =
        uassertStatusOK(shardmetadatautil::readShardCollectionsEntry(operationContext(), kNss));

    for (const auto& fieldName : requiredFieldNames) {
        // Simulate config.cache.collections getting corrupted by missing a required field
        ASSERT_OK(shardmetadatautil::updateShardCollectionsEntry(
            operationContext(),
            BSON(ShardCollectionType::kNssFieldName
                 << NamespaceStringUtil::serialize(kNss, SerializationContext::stateDefault())),
            BSON("$unset" << BSON(fieldName << 1)),
            false /*upsert*/));

        // Assert that data in config.cache.collections is corrupted
        const auto status =
            shardmetadatautil::readShardCollectionsEntry(operationContext(), kNss).getStatus();
        ASSERT_FALSE(status.isOK());
        ASSERT_EQUALS(status.code(), ErrorCodes::IDLFailedToParse);

        // Ensure that the corrupted cache gets dropped and the persisted metadata
        // is intact after a refresh.
        _remoteLoaderMock->setCollectionRefreshReturnValue(collAndChunks.first);
        _remoteLoaderMock->setChunkRefreshReturnValue(collAndChunks.second);
        const auto newChunks = retryableGetChunksSince(
            _shardLoader.get(), kNss, collAndChunks.second.back().getVersion());
        _shardLoader->waitForCollectionFlush(operationContext(), kNss);
        // Assert that refreshing from the latest version returned a single document
        // matching that version.
        ASSERT_EQUALS(newChunks.changedChunks.size(), 1UL);
        ASSERT_EQUALS(newChunks.epoch, collAndChunks.second.back().getVersion().epoch());
        // Assert that the persisted metadata is intact
        const auto newPersistedCollection =
            uassertStatusOK(shardmetadatautil::readShardCollectionsEntry(operationContext(), kNss));
        ASSERT_BSONOBJ_EQ(persistedCollection.toBSON(), newPersistedCollection.toBSON());
    }
}

TEST_F(ShardServerCatalogCacheLoaderTest, TimeseriesFieldsAreProperlyPropagatedOnSSCCL) {
    ChunkVersion collectionPlacementVersion({OID::gen(), Timestamp(1, 1)}, {1, 0});

    CollectionType collectionType = makeCollectionType(collectionPlacementVersion);
    vector<ChunkType> chunks = makeFiveChunks(collectionPlacementVersion);
    auto timeseriesOptions = TimeseriesOptions("fieldName");

    {
        TypeCollectionTimeseriesFields tsFields;
        timeseriesOptions.setGranularity(BucketGranularityEnum::Seconds);
        tsFields.setTimeseriesOptions(timeseriesOptions);
        collectionType.setTimeseriesFields(tsFields);

        _remoteLoaderMock->setCollectionRefreshReturnValue(collectionType);
        _remoteLoaderMock->setChunkRefreshReturnValue(chunks);

        auto collAndChunksRes =
            retryableGetChunksSince(_shardLoader.get(), kNss, ChunkVersion::UNSHARDED());
        ASSERT(collAndChunksRes.timeseriesFields.has_value());
        ASSERT(collAndChunksRes.timeseriesFields->getGranularity() ==
               BucketGranularityEnum::Seconds);
    }

    {
        auto& lastChunk = chunks.back();
        const auto maxLoaderVersion = lastChunk.getVersion();
        ChunkVersion newCollectionPlacementVersion = maxLoaderVersion;
        newCollectionPlacementVersion.incMinor();
        lastChunk.setVersion(newCollectionPlacementVersion);

        TypeCollectionTimeseriesFields tsFields;
        timeseriesOptions.setGranularity(BucketGranularityEnum::Hours);
        tsFields.setTimeseriesOptions(timeseriesOptions);
        collectionType.setTimeseriesFields(tsFields);

        _remoteLoaderMock->setCollectionRefreshReturnValue(collectionType);
        _remoteLoaderMock->setChunkRefreshReturnValue(std::vector{lastChunk});

        auto collAndChunksRes = retryableGetChunksSince(_shardLoader.get(), kNss, maxLoaderVersion);
        ASSERT(collAndChunksRes.timeseriesFields.has_value());
        ASSERT(collAndChunksRes.timeseriesFields->getGranularity() == BucketGranularityEnum::Hours);
    }
}

void ShardServerCatalogCacheLoaderTest::refreshCollectionEpochOnRemoteLoader() {
    ChunkVersion collectionPlacementVersion({OID::gen(), Timestamp(1, 1)}, {1, 2});
    CollectionType collectionType = makeCollectionType(collectionPlacementVersion);
    vector<ChunkType> chunks = makeFiveChunks(collectionPlacementVersion);
    _remoteLoaderMock->setCollectionRefreshReturnValue(collectionType);
    _remoteLoaderMock->setChunkRefreshReturnValue(chunks);
}

TEST_F(ShardServerCatalogCacheLoaderTest, CollAndChunkTasksConsistency) {
    // Put some metadata in the persisted cache (config.cache.chunks.*)
    refreshCollectionEpochOnRemoteLoader();
    retryableGetChunksSince(_shardLoader.get(), kNss, ChunkVersion::UNSHARDED());
    _shardLoader->waitForCollectionFlush(operationContext(), kNss);

    // Pause the thread processing the pending updates on metadata
    FailPointEnableBlock failPoint("hangCollectionFlush");

    // Put a first task in the list of pending updates on metadata (in-memory)
    refreshCollectionEpochOnRemoteLoader();
    retryableGetChunksSince(_shardLoader.get(), kNss, ChunkVersion::UNSHARDED());

    // Bump the shard's term
    _shardLoader->onStepUp();

    // Putting a second task causes a verification of the contiguous versions in the list pending
    // updates on metadata
    refreshCollectionEpochOnRemoteLoader();
    retryableGetChunksSince(_shardLoader.get(), kNss, ChunkVersion::UNSHARDED());
}

/**
 * Refine shard key currently only changes the epoch and does not change the major/minor versions
 * in the chunks. This test simulates a stepDown in the middle of making changes to the
 * config.cache collections by the ShardServerCatalogCacheLoader after a refine shard key and makes
 * sure that the shard will be able to reach the valid state on config.cache on the next refresh.
 */
TEST_F(ShardServerCatalogCacheLoaderTest, RecoverAfterPartiallyFlushedMetadata) {
    // Initialize the cache and wait for it to be flushed to disk.
    const auto initialCollAndChunks = setUpChunkLoaderWithFiveChunks();
    _shardLoader->waitForCollectionFlush(operationContext(), kNss);

    // Do a refresh, that simulates the metadata after a refineCollectionShardKey (new
    // epoch/timestamp and chunks with refined bounds). Wait for it to be flushed.
    ChunkVersion collPlacementVersionWithNewEpoch({OID::gen(), Timestamp(2, 0)}, {1, 0});
    CollectionType collectionTypeWithNewEpoch =
        makeCollectionType(collPlacementVersionWithNewEpoch, kKeyPattern2);
    vector<ChunkType> chunksWithNewEpoch = makeFiveRefinedChunks(collPlacementVersionWithNewEpoch);
    _remoteLoaderMock->setCollectionRefreshReturnValue(collectionTypeWithNewEpoch);
    _remoteLoaderMock->setChunkRefreshReturnValue(chunksWithNewEpoch);

    retryableGetChunksSince(
        _shardLoader.get(), kNss, initialCollAndChunks.second.back().getVersion());
    _shardLoader->waitForCollectionFlush(operationContext(), kNss);

    // "Rollback" the persisted metadata as if only the update to config.cache.collections had
    // happened, but not the update on config.cache.chunks.<nss>
    ASSERT_OK(shardmetadatautil::updateShardChunks(operationContext(),
                                                   kNss,
                                                   initialCollAndChunks.second,
                                                   initialCollAndChunks.first.getEpoch()));

    ASSERT_OK(shardmetadatautil::updateShardCollectionsEntry(
        operationContext(),
        BSON(ShardCollectionType::kNssFieldName
             << NamespaceStringUtil::serialize(kNss, SerializationContext::stateDefault())),
        BSON("$set" << BSON(ShardCollectionType::kRefreshingFieldName << true)),
        false));

    // Refresh again and expect the SSCCL to be able to recover and return the correct collection
    // and chunks metadata (i.e. post-refine).
    _remoteLoaderMock->setCollectionRefreshReturnValue(collectionTypeWithNewEpoch);
    _remoteLoaderMock->setChunkRefreshReturnValue(chunksWithNewEpoch);
    const auto newChunks = retryableGetChunksSince(
        _shardLoader.get(), kNss, initialCollAndChunks.second.back().getVersion());

    ASSERT_BSONOBJ_EQ(kKeyPattern2.toBSON(), newChunks.shardKeyPattern);
    ASSERT_EQ(5, newChunks.changedChunks.size());
    ASSERT_BSONOBJ_EQ(chunksWithNewEpoch.front().getMin(),
                      newChunks.changedChunks.front().getMin());

    _shardLoader->waitForCollectionFlush(operationContext(), kNss);

    const auto newPersistedChunks = uassertStatusOK(
        shardmetadatautil::readShardChunks(operationContext(),
                                           kNss,
                                           BSONObj(),
                                           BSONObj(),
                                           boost::none,
                                           collPlacementVersionWithNewEpoch.epoch(),
                                           collPlacementVersionWithNewEpoch.getTimestamp()));
    ASSERT_EQ(5, newPersistedChunks.size());
    ASSERT_BSONOBJ_EQ(chunksWithNewEpoch.front().getMin(), newPersistedChunks.front().getMin());

    const auto persistedCollection =
        uassertStatusOK(shardmetadatautil::readShardCollectionsEntry(operationContext(), kNss));
    ASSERT_FALSE(persistedCollection.getRefreshing().value_or(false));
}

TEST_F(ShardServerCatalogCacheLoaderTest, UnsplittableFieldIsPropagatedOnSSCCL) {
    ChunkVersion collectionPlacementVersion({OID::gen(), Timestamp(1, 1)}, {1, 0});

    CollectionType collectionType = makeCollectionType(collectionPlacementVersion);
    std::vector<ChunkType> chunk = {
        ChunkType{UUID::gen(),
                  ChunkRange{BSON("_id" << MINKEY), BSON("_id" << MAXKEY)},
                  collectionPlacementVersion,
                  kShardId},
    };
    collectionType.setUnsplittable(true);

    _remoteLoaderMock->setCollectionRefreshReturnValue(collectionType);
    _remoteLoaderMock->setChunkRefreshReturnValue(chunk);

    auto collAndChunksRes =
        retryableGetChunksSince(_shardLoader.get(), kNss, ChunkVersion::UNSHARDED());
    ASSERT(collAndChunksRes.unsplittable);
}
}  // namespace
}  // namespace mongo
