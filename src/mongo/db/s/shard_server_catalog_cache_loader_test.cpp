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

#include "mongo/platform/basic.h"

#include <boost/optional/optional_io.hpp>

#include "mongo/db/s/shard_server_catalog_cache_loader.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache_loader_mock.h"
#include "mongo/s/type_collection_timeseries_fields_gen.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using std::vector;
using unittest::assertGet;
using CollectionAndChangedChunks = CatalogCacheLoader::CollectionAndChangedChunks;

const NamespaceString kNss = NamespaceString("foo.bar");
const std::string kPattern = "_id";
const ShardId kShardId = ShardId("shard0");

class ShardServerCatalogCacheLoaderTest : public ShardServerTestFixture {
public:
    /**
     * Returns five chunks using collVersion as a starting version.
     */
    vector<ChunkType> makeFiveChunks(const ChunkVersion& collectionVersion);

    /**
     * Returns a chunk update diff GTE 'collVersion' for the chunks created by makeFiveChunks above.
     */
    vector<ChunkType> makeThreeUpdatedChunksDiff(const ChunkVersion& collectionVersion);

    /**
     * Returns a routing table applying 'threeUpdatedChunks' (the result of
     * makeThreeUpdatedChunksDiff) to 'originalFiveChunks' (the result of makeFiveChunks).
     */
    vector<ChunkType> makeCombinedOriginalFiveChunksAndThreeNewChunksDiff(
        const vector<ChunkType>& originalFiveChunks, const vector<ChunkType>& threeUpdatedChunks);

    /**
     * This helper makes a CollectionType with the current _maxCollVersion.
     */
    CollectionType makeCollectionType(const ChunkVersion& collVersion);

    /**
     * Sets up the _shardLoader with the results of makeFiveChunks().
     */
    std::pair<CollectionType, vector<ChunkType>> setUpChunkLoaderWithFiveChunks();

    void refreshCollectionEpochOnRemoteLoader();

    const KeyPattern kKeyPattern = KeyPattern(BSON(kPattern << 1));

    CatalogCacheLoaderMock* _remoteLoaderMock;
    std::unique_ptr<ShardServerCatalogCacheLoader> _shardLoader;

private:
    void setUp() override;
    void tearDown() override;
};

void ShardServerCatalogCacheLoaderTest::setUp() {
    ShardServerTestFixture::setUp();

    // Create mock remote and real shard loader, retaining a pointer to the mock remote loader so
    // that unit tests can manipulate it to return certain responses.
    auto mockLoader = std::make_unique<CatalogCacheLoaderMock>();
    _remoteLoaderMock = mockLoader.get();
    _shardLoader = std::make_unique<ShardServerCatalogCacheLoader>(std::move(mockLoader));

    // Set the shard loader to primary mode, and set it for testing.
    _shardLoader->initializeReplicaSetRole(true);
}

void ShardServerCatalogCacheLoaderTest::tearDown() {
    _shardLoader->shutDown();
    _shardLoader.reset();
    ShardServerTestFixture::tearDown();
}

vector<ChunkType> ShardServerCatalogCacheLoaderTest::makeFiveChunks(
    const ChunkVersion& collectionVersion) {
    ChunkVersion collVersion(collectionVersion);
    vector<ChunkType> chunks;

    BSONObj mins[] = {
        BSON("a" << MINKEY), BSON("a" << 10), BSON("a" << 50), BSON("a" << 100), BSON("a" << 200)};
    BSONObj maxs[] = {
        BSON("a" << 10), BSON("a" << 50), BSON("a" << 100), BSON("a" << 200), BSON("a" << MAXKEY)};

    for (int i = 0; i < 5; ++i) {
        collVersion.incMajor();

        ChunkType chunk;
        chunk.setNS(kNss);
        chunk.setMin(mins[i]);
        chunk.setMax(maxs[i]);
        chunk.setShard(kShardId);
        chunk.setVersion(collVersion);

        chunks.push_back(chunk);
    }

    return chunks;
}

vector<ChunkType> ShardServerCatalogCacheLoaderTest::makeThreeUpdatedChunksDiff(
    const ChunkVersion& collectionVersion) {
    ChunkVersion collVersion(collectionVersion);
    vector<ChunkType> chunks;

    // The diff query is for GTE a known version, so prepend the previous newest chunk, which is
    // unmodified by this change and so should be found. Note: it is important for testing that the
    // previous highest versioned chunk is unmodified. Otherwise the shard loader's results are
    // dependent on a race between persistence and retrieving data because it combines enqueued and
    // persisted results without applying modifications.
    ChunkType oldChunk;
    oldChunk.setNS(kNss);
    oldChunk.setMin(BSON("a" << 200));
    oldChunk.setMax(BSON("a" << MAXKEY));
    oldChunk.setShard(kShardId);
    oldChunk.setVersion(collVersion);
    chunks.push_back(oldChunk);


    // Make chunk updates
    BSONObj mins[] = {BSON("a" << MINKEY), BSON("a" << 5), BSON("a" << 10)};
    BSONObj maxs[] = {BSON("a" << 5), BSON("a" << 10), BSON("a" << 100)};

    for (int i = 0; i < 3; ++i) {
        collVersion.incMinor();

        ChunkType chunk;
        chunk.setNS(kNss);
        chunk.setMin(mins[i]);
        chunk.setMax(maxs[i]);
        chunk.setShard(kShardId);
        chunk.setVersion(collVersion);

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
    const ChunkVersion& collVersion) {
    CollectionType coll(
        kNss, collVersion.epoch(), collVersion.getTimestamp(), Date_t::now(), UUID::gen());
    coll.setKeyPattern(kKeyPattern);
    coll.setUnique(false);
    return coll;
}

std::pair<CollectionType, vector<ChunkType>>
ShardServerCatalogCacheLoaderTest::setUpChunkLoaderWithFiveChunks() {
    ChunkVersion collectionVersion(1, 0, OID::gen(), boost::none /* timestamp */);

    CollectionType collectionType = makeCollectionType(collectionVersion);
    vector<ChunkType> chunks = makeFiveChunks(collectionVersion);
    _remoteLoaderMock->setCollectionRefreshReturnValue(collectionType);
    _remoteLoaderMock->setChunkRefreshReturnValue(chunks);

    auto collAndChunksRes = _shardLoader->getChunksSince(kNss, ChunkVersion::UNSHARDED()).get();

    ASSERT_EQUALS(collAndChunksRes.epoch, collectionType.getEpoch());
    ASSERT_EQUALS(collAndChunksRes.changedChunks.size(), 5UL);
    ASSERT(!collAndChunksRes.timeseriesFields.is_initialized());
    for (unsigned int i = 0; i < collAndChunksRes.changedChunks.size(); ++i) {
        ASSERT_BSONOBJ_EQ(collAndChunksRes.changedChunks[i].toShardBSON(), chunks[i].toShardBSON());
    }

    return std::pair{collectionType, std::move(chunks)};
}

TEST_F(ShardServerCatalogCacheLoaderTest, PrimaryLoadFromUnshardedToUnsharded) {
    // Return a NamespaceNotFound error that means the collection doesn't exist.

    Status errorStatus = Status(ErrorCodes::NamespaceNotFound, "collection not found");
    _remoteLoaderMock->setCollectionRefreshReturnValue(errorStatus);

    ASSERT_THROWS_CODE_AND_WHAT(_shardLoader->getChunksSince(kNss, ChunkVersion::UNSHARDED()).get(),
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
        _shardLoader->getChunksSince(kNss, chunks.back().getVersion()).get(),
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

    auto collAndChunksRes = _shardLoader->getChunksSince(kNss, chunks.back().getVersion()).get();

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

    auto collAndChunksRes = _shardLoader->getChunksSince(kNss, ChunkVersion::UNSHARDED()).get();
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

    ChunkVersion collVersion = chunks.back().getVersion();
    vector<ChunkType> updatedChunksDiff = makeThreeUpdatedChunksDiff(collVersion);
    _remoteLoaderMock->setChunkRefreshReturnValue(updatedChunksDiff);

    auto collAndChunksRes = _shardLoader->getChunksSince(kNss, chunks.back().getVersion()).get();

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

    _shardLoader->getChunksSince(kNss, chunks.back().getVersion()).get();

    // Wait for persistence of update
    _shardLoader->waitForCollectionFlush(operationContext(), kNss);

    // Set up the remote loader to return a single document we've already seen, indicating no change
    // occurred.
    vector<ChunkType> lastChunk;
    lastChunk.push_back(updatedChunksDiff.back());
    _remoteLoaderMock->setChunkRefreshReturnValue(lastChunk);

    vector<ChunkType> completeRoutingTableWithDiffApplied =
        makeCombinedOriginalFiveChunksAndThreeNewChunksDiff(chunks, updatedChunksDiff);

    auto collAndChunksRes = _shardLoader->getChunksSince(kNss, ChunkVersion::UNSHARDED()).get();
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

    ChunkVersion collVersionWithNewEpoch(1, 0, OID::gen(), boost::none /* timestamp */);
    CollectionType collectionTypeWithNewEpoch = makeCollectionType(collVersionWithNewEpoch);
    vector<ChunkType> chunksWithNewEpoch = makeFiveChunks(collVersionWithNewEpoch);
    _remoteLoaderMock->setCollectionRefreshReturnValue(collectionTypeWithNewEpoch);
    _remoteLoaderMock->setChunkRefreshReturnValue(chunksWithNewEpoch);

    auto collAndChunksRes = _shardLoader->getChunksSince(kNss, chunks.back().getVersion()).get();
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

    ChunkVersion collVersionWithNewEpoch(1, 0, OID::gen(), boost::none /* timestamp */);
    CollectionType collectionTypeWithNewEpoch = makeCollectionType(collVersionWithNewEpoch);
    vector<ChunkType> chunksWithNewEpoch = makeFiveChunks(collVersionWithNewEpoch);
    vector<ChunkType> mixedChunks;
    mixedChunks.push_back(chunks.back());
    mixedChunks.insert(mixedChunks.end(), chunksWithNewEpoch.begin(), chunksWithNewEpoch.end());
    _remoteLoaderMock->setChunkRefreshReturnValue(mixedChunks);

    ASSERT_THROWS_CODE(_shardLoader->getChunksSince(kNss, chunks.back().getVersion()).get(),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);

    // Now make sure the newly recreated collection is cleanly loaded. We cannot ensure a
    // non-variable response until the loader has remotely retrieved the new metadata and applied
    // them to the persisted store. So first do a reload and ignore the results. Then call again,
    // this time checking the results.

    _remoteLoaderMock->setCollectionRefreshReturnValue(collectionTypeWithNewEpoch);
    _remoteLoaderMock->setChunkRefreshReturnValue(chunksWithNewEpoch);

    _shardLoader->getChunksSince(kNss, chunks.back().getVersion()).get();

    // Wait for persistence of update.
    _shardLoader->waitForCollectionFlush(operationContext(), kNss);

    vector<ChunkType> lastChunkWithNewEpoch;
    lastChunkWithNewEpoch.push_back(chunksWithNewEpoch.back());
    _remoteLoaderMock->setChunkRefreshReturnValue(lastChunkWithNewEpoch);

    auto collAndChunksRes = _shardLoader->getChunksSince(kNss, ChunkVersion::UNSHARDED()).get();
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

TEST_F(ShardServerCatalogCacheLoaderTest, PrimaryLoadFromShardedWithChangeOnMetadataFormat) {
    // First set up the shard chunk loader as sharded.
    auto collAndChunks = setUpChunkLoaderWithFiveChunks();
    auto& collType = collAndChunks.first;
    auto& chunks = collAndChunks.second;

    auto changeMetadataFormat = [&](const boost::optional<Timestamp>& timestamp) {
        auto lastChunk = chunks.back();
        lastChunk.setVersion([&]() {
            const auto v = lastChunk.getVersion();
            return ChunkVersion(v.majorVersion(), v.minorVersion(), v.epoch(), timestamp);
        }());

        collType.setTimestamp(timestamp);
        _remoteLoaderMock->setCollectionRefreshReturnValue(collType);
        _remoteLoaderMock->setChunkRefreshReturnValue(std::vector{lastChunk});

        auto collAndChunksRes = _shardLoader->getChunksSince(kNss, chunks[0].getVersion()).get();
        ASSERT_EQUALS(collAndChunksRes.epoch, collType.getEpoch());
        ASSERT_EQUALS(collAndChunksRes.creationTime, timestamp);
        ASSERT_EQUALS(collAndChunksRes.changedChunks.size(), 5UL);
        for (const auto& changedChunk : collAndChunksRes.changedChunks) {
            ASSERT_EQUALS(changedChunk.getVersion().getTimestamp(), timestamp);
            ASSERT_EQUALS(changedChunk.getVersion().epoch(), collAndChunksRes.epoch);
        }
    };

    // Upgrading the metadata format to 5.0
    changeMetadataFormat(Timestamp(42));
    // Downgrading the medata format to 4.4
    changeMetadataFormat(boost::none /* timestamp */);
}

TEST_F(ShardServerCatalogCacheLoaderTest,
       PrimaryLoadFromShardedWithChangeOnMetadataFormatBecauseUpgrade) {
    const auto timestamp = Timestamp(42);
    ChunkVersion collectionVersion(1, 0, OID::gen(), boost::none /* timestamp */);
    CollectionType collectionType = makeCollectionType(collectionVersion);
    vector<ChunkType> chunks = makeFiveChunks(collectionVersion);

    // 1st refresh as if we were in 4.4: the loader discovers one new chunk without timestamp
    {
        _remoteLoaderMock->setCollectionRefreshReturnValue(collectionType);
        _remoteLoaderMock->setChunkRefreshReturnValue(std::vector{chunks[0]});
        const auto collAndChunksRes =
            _shardLoader->getChunksSince(kNss, chunks[0].getVersion()).get();
        ASSERT_EQUALS(collAndChunksRes.changedChunks.size(), 1UL);
        ASSERT_EQUALS(collAndChunksRes.creationTime, boost::none);
        ASSERT_EQUALS(collAndChunksRes.changedChunks[0].getVersion().getTimestamp(), boost::none);
    }

    // 2nd refresh as if we were in the phase 1 of the setFCV process to upgrade to 5.0: the loader
    // discovers a few new chunks with timestamp but the collection doesn't have it yet.
    {
        for (size_t i = 1; i < chunks.size() - 1; ++i) {
            chunks[i].setVersion([&]() {
                const auto v = chunks[i].getVersion();
                return ChunkVersion(v.majorVersion(), v.minorVersion(), v.epoch(), timestamp);
            }());
        }

        _remoteLoaderMock->setCollectionRefreshReturnValue(collectionType);
        _remoteLoaderMock->setChunkRefreshReturnValue(
            std::vector<ChunkType>(chunks.begin() + 1, chunks.end() - 1));
        const auto collAndChunksRes =
            _shardLoader->getChunksSince(kNss, chunks[0].getVersion()).get();
        const auto& changedChunks = collAndChunksRes.changedChunks;
        ASSERT_EQUALS(changedChunks.size(), 4UL);
        ASSERT_EQUALS(collAndChunksRes.creationTime, boost::none);
        ASSERT_EQUALS(changedChunks[0].getVersion().getTimestamp(), boost::none);
    }

    // 3rd refresh as if we were in 5.0: the loader discovers a new chunk. All chunks and the
    // collection have timestamps.
    {
        chunks.back().setVersion([&]() {
            const auto v = chunks.back().getVersion();
            return ChunkVersion(v.majorVersion(), v.minorVersion(), v.epoch(), timestamp);
        }());
        collectionType.setTimestamp(timestamp);

        _remoteLoaderMock->setCollectionRefreshReturnValue(collectionType);
        _remoteLoaderMock->setChunkRefreshReturnValue(std::vector{chunks.back()});
        const auto collAndChunksRes =
            _shardLoader->getChunksSince(kNss, chunks[0].getVersion()).get();
        const auto& changedChunks = collAndChunksRes.changedChunks;
        ASSERT_EQUALS(changedChunks.size(), 5UL);
        ASSERT_EQUALS(collAndChunksRes.creationTime, timestamp);
        for (size_t i = 0; i < chunks.size(); ++i)
            ASSERT_EQUALS(changedChunks[i].getVersion().getTimestamp(), timestamp);
    }
}

TEST_F(ShardServerCatalogCacheLoaderTest,
       PrimaryLoadFromShardedWithChangeOnMetadataFormatBecauseDowngrade) {
    const auto timestamp = Timestamp(42);
    ChunkVersion collectionVersion(1, 0, OID::gen(), timestamp);
    CollectionType collectionType = makeCollectionType(collectionVersion);
    vector<ChunkType> chunks = makeFiveChunks(collectionVersion);

    // 1st refresh as if we were in 5.0: the loader discovers one new chunk with timestamp. The
    // collection also has timestamps.
    {
        _remoteLoaderMock->setCollectionRefreshReturnValue(collectionType);
        _remoteLoaderMock->setChunkRefreshReturnValue(std::vector{chunks[0]});
        const auto collAndChunksRes =
            _shardLoader->getChunksSince(kNss, chunks[0].getVersion()).get();
        ASSERT_EQUALS(collAndChunksRes.changedChunks.size(), 1UL);
        ASSERT_EQUALS(collAndChunksRes.creationTime, timestamp);
        ASSERT_EQUALS(collAndChunksRes.changedChunks[0].getVersion().getTimestamp(), timestamp);
    }

    // 2nd refresh: the loader discovers a few new chunks without timestamp but the collection still
    // has it.
    {
        for (size_t i = 1; i < chunks.size() - 1; ++i) {
            chunks[i].setVersion([&]() {
                const auto v = chunks[i].getVersion();
                return ChunkVersion(
                    v.majorVersion(), v.minorVersion(), v.epoch(), boost::none /* timestamp */);
            }());
        }

        _remoteLoaderMock->setCollectionRefreshReturnValue(collectionType);
        _remoteLoaderMock->setChunkRefreshReturnValue(
            std::vector<ChunkType>(chunks.begin() + 1, chunks.end() - 1));
        const auto collAndChunksRes =
            _shardLoader->getChunksSince(kNss, chunks[0].getVersion()).get();
        const auto& changedChunks = collAndChunksRes.changedChunks;
        ASSERT_EQUALS(changedChunks.size(), 4UL);
        ASSERT_EQUALS(collAndChunksRes.creationTime, timestamp);
        ASSERT_EQUALS(changedChunks[0].getVersion().getTimestamp(), timestamp);
        for (size_t i = 1; i < chunks.size() - 1; ++i)
            ASSERT_EQUALS(changedChunks[i].getVersion().getTimestamp(),
                          boost::none /* timestamp */);
    }

    // 3rd refresh as if we were in 4.4: the loader discovers a new chunk. All chunks and the
    // collection don't have timestamps.
    {
        chunks.back().setVersion([&]() {
            const auto v = chunks.back().getVersion();
            return ChunkVersion(
                v.majorVersion(), v.minorVersion(), v.epoch(), boost::none /* timestamp */);
        }());
        collectionType.setTimestamp(boost::none /* timestamp */);

        _remoteLoaderMock->setCollectionRefreshReturnValue(collectionType);
        _remoteLoaderMock->setChunkRefreshReturnValue(std::vector{chunks.back()});
        const auto collAndChunksRes =
            _shardLoader->getChunksSince(kNss, chunks[0].getVersion()).get();
        const auto& changedChunks = collAndChunksRes.changedChunks;
        ASSERT_EQUALS(changedChunks.size(), 5UL);
        ASSERT_EQUALS(collAndChunksRes.creationTime, boost::none /* timestamp */);
        for (size_t i = 0; i < chunks.size(); ++i)
            ASSERT_EQUALS(changedChunks[i].getVersion().getTimestamp(),
                          boost::none /* timestamp */);
    }
}

TEST_F(ShardServerCatalogCacheLoaderTest, PrimaryLoadFromShardedAndFindDbMetadataFormatChanged) {
    const std::string dbName("dbName");
    DatabaseVersion version(UUID::gen());
    DatabaseType dbType(dbName, kShardId, true /* sharded */, version);

    _remoteLoaderMock->setDatabaseRefreshReturnValue(dbType);
    auto newDbType = _shardLoader->getDatabase(dbName).get();
    ASSERT_EQUALS(dbType.getVersion().getUuid(), newDbType.getVersion().getUuid());
    ASSERT_EQUALS(dbType.getVersion().getTimestamp(), newDbType.getVersion().getTimestamp());

    dbType.setVersion(DatabaseVersion(UUID::gen(), Timestamp(42)));
    _remoteLoaderMock->setDatabaseRefreshReturnValue(dbType);
    newDbType = _shardLoader->getDatabase(dbName).get();
    ASSERT_EQUALS(dbType.getVersion().getUuid(), newDbType.getVersion().getUuid());
    ASSERT_EQUALS(dbType.getVersion().getTimestamp(), newDbType.getVersion().getTimestamp());
}

TEST_F(ShardServerCatalogCacheLoaderTest, TimeseriesFieldsAreProperlyPropagatedOnSSCCL) {
    ChunkVersion collectionVersion(1, 0, OID::gen(), boost::none /* timestamp */);

    CollectionType collectionType = makeCollectionType(collectionVersion);
    TypeCollectionTimeseriesFields tsFields;
    tsFields.setTimeseriesOptions(TimeseriesOptions("fieldName"));
    collectionType.setTimeseriesFields(tsFields);

    vector<ChunkType> chunks = makeFiveChunks(collectionVersion);

    _remoteLoaderMock->setCollectionRefreshReturnValue(collectionType);
    _remoteLoaderMock->setChunkRefreshReturnValue(chunks);

    auto collAndChunksRes = _shardLoader->getChunksSince(kNss, ChunkVersion::UNSHARDED()).get();
    ASSERT(collAndChunksRes.timeseriesFields.is_initialized());
}

void ShardServerCatalogCacheLoaderTest::refreshCollectionEpochOnRemoteLoader() {
    ChunkVersion collectionVersion(1, 2, OID::gen(), boost::none);
    CollectionType collectionType = makeCollectionType(collectionVersion);
    vector<ChunkType> chunks = makeFiveChunks(collectionVersion);
    _remoteLoaderMock->setCollectionRefreshReturnValue(collectionType);
    _remoteLoaderMock->setChunkRefreshReturnValue(chunks);
}

TEST_F(ShardServerCatalogCacheLoaderTest, CollAndChunkTasksConsistency) {
    // Put some metadata in the persisted cache (config.cache.chunks.*)
    refreshCollectionEpochOnRemoteLoader();
    _shardLoader->getChunksSince(kNss, ChunkVersion::UNSHARDED()).get();
    _shardLoader->waitForCollectionFlush(operationContext(), kNss);

    // Pause the thread processing the pending updates on metadata
    FailPointEnableBlock failPoint("hangCollectionFlush");

    // Put a first task in the list of pending updates on metadata (in-memory)
    refreshCollectionEpochOnRemoteLoader();
    _shardLoader->getChunksSince(kNss, ChunkVersion::UNSHARDED()).get();

    // Bump the shard's term
    _shardLoader->onStepUp();

    // Putting a second task causes a verification of the contiguous versions in the list pending
    // updates on metadata
    refreshCollectionEpochOnRemoteLoader();
    _shardLoader->getChunksSince(kNss, ChunkVersion::UNSHARDED()).get();
}

}  // namespace
}  // namespace mongo
