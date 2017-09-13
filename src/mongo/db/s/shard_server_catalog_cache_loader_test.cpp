/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/s/shard_server_catalog_cache_loader.h"

#include "mongo/db/s/catalog_cache_loader_mock.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/unittest/unittest.h"

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
    vector<ChunkType> setUpChunkLoaderWithFiveChunks();

    const KeyPattern kKeyPattern = KeyPattern(BSON(kPattern << 1));
    const stdx::function<void(OperationContext*, StatusWith<CollectionAndChangedChunks>)>
        kDoNothingCallbackFn = [](
            OperationContext * opCtx,
            StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollAndChunks) noexcept {};

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
    std::unique_ptr<CatalogCacheLoaderMock> mockLoader =
        stdx::make_unique<CatalogCacheLoaderMock>();
    _remoteLoaderMock = mockLoader.get();
    _shardLoader = stdx::make_unique<ShardServerCatalogCacheLoader>(std::move(mockLoader));

    // Set the shard loader to primary mode, and set it for testing.
    _shardLoader->initializeReplicaSetRole(true);
}

void ShardServerCatalogCacheLoaderTest::tearDown() {
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
        chunk.setNS(kNss.ns());
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
    oldChunk.setNS(kNss.ns());
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
        chunk.setNS(kNss.ns());
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
    CollectionType coll;
    coll.setNs(kNss);
    coll.setEpoch(collVersion.epoch());
    coll.setUpdatedAt(Date_t::fromMillisSinceEpoch(collVersion.toLong()));
    coll.setKeyPattern(kKeyPattern);
    coll.setUnique(false);
    return coll;
}

vector<ChunkType> ShardServerCatalogCacheLoaderTest::setUpChunkLoaderWithFiveChunks() {
    ChunkVersion collectionVersion(1, 0, OID::gen());

    CollectionType collectionType = makeCollectionType(collectionVersion);
    vector<ChunkType> chunks = makeFiveChunks(collectionVersion);
    _remoteLoaderMock->setCollectionRefreshReturnValue(collectionType);
    _remoteLoaderMock->setChunkRefreshReturnValue(chunks);

    StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> results{
        Status(ErrorCodes::InternalError, "")};
    const auto refreshCallbackFn = [&results](
        OperationContext * opCtx,
        StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollAndChunks) noexcept {
        results = std::move(swCollAndChunks);
    };

    auto notification =
        _shardLoader->getChunksSince(kNss, ChunkVersion::UNSHARDED(), refreshCallbackFn);
    notification->get();

    // Check refreshCallbackFn thread results where we can safely throw.
    ASSERT_OK(results.getStatus());
    auto collAndChunkRes = results.getValue();
    ASSERT_EQUALS(collAndChunkRes.epoch, collectionType.getEpoch());
    ASSERT_EQUALS(collAndChunkRes.changedChunks.size(), 5UL);
    for (unsigned int i = 0; i < collAndChunkRes.changedChunks.size(); ++i) {
        ASSERT_BSONOBJ_EQ(collAndChunkRes.changedChunks[i].toShardBSON(), chunks[i].toShardBSON());
    }

    return chunks;
}

TEST_F(ShardServerCatalogCacheLoaderTest, PrimaryLoadFromUnshardedToUnsharded) {
    // Return a NamespaceNotFound error that means the collection doesn't exist.

    Status errorStatus = Status(ErrorCodes::NamespaceNotFound, "collection not found");
    _remoteLoaderMock->setCollectionRefreshReturnValue(errorStatus);

    StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> results{
        Status(ErrorCodes::InternalError, "")};
    const auto refreshCallbackFn = [&results](
        OperationContext * opCtx,
        StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollAndChunks) noexcept {
        results = std::move(swCollAndChunks);
    };

    auto notification =
        _shardLoader->getChunksSince(kNss, ChunkVersion::UNSHARDED(), refreshCallbackFn);
    notification->get();

    ASSERT_EQUALS(results.getStatus(), errorStatus);
}

TEST_F(ShardServerCatalogCacheLoaderTest, PrimaryLoadFromShardedToUnsharded) {
    // First set up the shard chunk loader as sharded.

    auto chunks = setUpChunkLoaderWithFiveChunks();

    // Then return a NamespaceNotFound error, which means the collection must have been dropped,
    // clearing the chunk metadata.

    Status errorStatus = Status(ErrorCodes::NamespaceNotFound, "collection not found");
    _remoteLoaderMock->setCollectionRefreshReturnValue(errorStatus);

    StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> results{
        Status(ErrorCodes::InternalError, "")};
    const auto nextRefreshCallbackFn = [&results](
        OperationContext * opCtx,
        StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollAndChunks) noexcept {
        results = std::move(swCollAndChunks);
    };

    auto notification =
        _shardLoader->getChunksSince(kNss, chunks.back().getVersion(), nextRefreshCallbackFn);
    notification->get();

    ASSERT_EQUALS(results.getStatus(), errorStatus);
}

TEST_F(ShardServerCatalogCacheLoaderTest, PrimaryLoadFromShardedAndFindNoDiff) {
    // First set up the shard chunk loader as sharded.

    vector<ChunkType> chunks = setUpChunkLoaderWithFiveChunks();

    // Then set up the remote loader to return a single document we've already seen -- indicates
    // there's nothing new.

    vector<ChunkType> lastChunk;
    lastChunk.push_back(chunks.back());
    _remoteLoaderMock->setChunkRefreshReturnValue(lastChunk);

    StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> results{
        Status(ErrorCodes::InternalError, "")};
    const auto refreshCallbackFn = [&results](
        OperationContext * opCtx,
        StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollAndChunks) noexcept {
        results = std::move(swCollAndChunks);
    };

    auto notification =
        _shardLoader->getChunksSince(kNss, chunks.back().getVersion(), refreshCallbackFn);
    notification->get();

    // Check that refreshing from the latest version returned a single document matching that
    // version.
    ASSERT_OK(results.getStatus());
    auto collAndChunksRes = results.getValue();
    ASSERT_EQUALS(collAndChunksRes.epoch, chunks.back().getVersion().epoch());
    ASSERT_EQUALS(collAndChunksRes.changedChunks.size(), 1UL);
    ASSERT_BSONOBJ_EQ(collAndChunksRes.changedChunks.back().toShardBSON(),
                      chunks.back().toShardBSON());
}

// Same as the above unit test, PrimaryLoadFromShardedAndFindNoDiff, but caller requests complete
// routing table, rather than diff from a known version.
TEST_F(ShardServerCatalogCacheLoaderTest, PrimaryLoadFromShardedAndFindNoDiffRequestAll) {
    // First set up the shard chunk loader as sharded.

    vector<ChunkType> chunks = setUpChunkLoaderWithFiveChunks();

    // Then set up the remote loader to return a single document we've already seen -- indicates
    // there's nothing new.

    vector<ChunkType> lastChunk;
    lastChunk.push_back(chunks.back());
    _remoteLoaderMock->setChunkRefreshReturnValue(lastChunk);

    StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> results{
        Status(ErrorCodes::InternalError, "")};
    const auto completeRefreshCallbackFn = [&results](
        OperationContext * opCtx,
        StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollAndChunks) noexcept {
        results = std::move(swCollAndChunks);
    };

    auto notification =
        _shardLoader->getChunksSince(kNss, ChunkVersion::UNSHARDED(), completeRefreshCallbackFn);
    notification->get();

    // Check that the complete routing table was returned successfully.
    ASSERT_OK(results.getStatus());
    auto collAndChunksRes = results.getValue();
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

    vector<ChunkType> chunks = setUpChunkLoaderWithFiveChunks();

    // Then refresh again and find updated chunks.

    ChunkVersion collVersion = chunks.back().getVersion();
    vector<ChunkType> updatedChunksDiff = makeThreeUpdatedChunksDiff(collVersion);
    _remoteLoaderMock->setChunkRefreshReturnValue(updatedChunksDiff);

    StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> results{
        Status(ErrorCodes::InternalError, "")};
    const auto refreshCallbackFn = [&results](
        OperationContext * opCtx,
        StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollAndChunks) noexcept {
        results = std::move(swCollAndChunks);
    };

    auto notification =
        _shardLoader->getChunksSince(kNss, chunks.back().getVersion(), refreshCallbackFn);
    notification->get();

    // Check that the diff was returned successfull.
    ASSERT_OK(results.getStatus());
    auto collAndChunksRes = results.getValue();
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

    vector<ChunkType> chunks = setUpChunkLoaderWithFiveChunks();

    // First cause a remote refresh to find the updated chunks. Then wait for persistence, so that
    // we ensure that nothing is enqueued and the next getChunksSince call will return a predictable
    // number of chunk documents: the result of applying the enqueued update diff.

    vector<ChunkType> updatedChunksDiff = makeThreeUpdatedChunksDiff(chunks.back().getVersion());
    _remoteLoaderMock->setChunkRefreshReturnValue(updatedChunksDiff);

    auto notification =
        _shardLoader->getChunksSince(kNss, chunks.back().getVersion(), kDoNothingCallbackFn);
    notification->get();

    // Wait for persistence of update
    _shardLoader->waitForCollectionFlush(operationContext(), kNss);

    // Set up the remote loader to return a single document we've already seen, indicating no change
    // occurred.
    vector<ChunkType> lastChunk;
    lastChunk.push_back(updatedChunksDiff.back());
    _remoteLoaderMock->setChunkRefreshReturnValue(lastChunk);

    vector<ChunkType> completeRoutingTableWithDiffApplied =
        makeCombinedOriginalFiveChunksAndThreeNewChunksDiff(chunks, updatedChunksDiff);

    StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> results{
        Status(ErrorCodes::InternalError, "")};
    const auto refreshCallbackFn = [&results](
        OperationContext * opCtx,
        StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollAndChunks) noexcept {
        results = std::move(swCollAndChunks);
    };

    auto nextNotification =
        _shardLoader->getChunksSince(kNss, ChunkVersion::UNSHARDED(), refreshCallbackFn);
    nextNotification->get();

    // Check that the complete routing table, with diff applied, was returned.
    ASSERT_OK(results.getStatus());
    auto collAndChunksRes = results.getValue();
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

    vector<ChunkType> chunks = setUpChunkLoaderWithFiveChunks();

    // Then refresh again and find that the collection has been dropped and recreated.

    ChunkVersion collVersionWithNewEpoch(1, 0, OID::gen());
    CollectionType collectionTypeWithNewEpoch = makeCollectionType(collVersionWithNewEpoch);
    vector<ChunkType> chunksWithNewEpoch = makeFiveChunks(collVersionWithNewEpoch);
    _remoteLoaderMock->setCollectionRefreshReturnValue(collectionTypeWithNewEpoch);
    _remoteLoaderMock->setChunkRefreshReturnValue(chunksWithNewEpoch);

    StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> results{
        Status(ErrorCodes::InternalError, "")};
    const auto refreshCallbackFn = [&results](
        OperationContext * opCtx,
        StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollAndChunks) noexcept {
        results = std::move(swCollAndChunks);
    };

    auto notification =
        _shardLoader->getChunksSince(kNss, chunks.back().getVersion(), refreshCallbackFn);
    notification->get();

    // Check that the complete routing table for the new epoch was returned.
    ASSERT_OK(results.getStatus());
    auto collAndChunksRes = results.getValue();
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

    vector<ChunkType> chunks = setUpChunkLoaderWithFiveChunks();

    // Then refresh again and retrieve chunks from the config server that have mixed epoches, like
    // as if the chunks read yielded around a drop and recreate of the collection.

    CollectionType originalCollectionType = makeCollectionType(chunks.back().getVersion());

    ChunkVersion collVersionWithNewEpoch(1, 0, OID::gen());
    CollectionType collectionTypeWithNewEpoch = makeCollectionType(collVersionWithNewEpoch);
    vector<ChunkType> chunksWithNewEpoch = makeFiveChunks(collVersionWithNewEpoch);
    vector<ChunkType> mixedChunks;
    mixedChunks.push_back(chunks.back());
    mixedChunks.insert(mixedChunks.end(), chunksWithNewEpoch.begin(), chunksWithNewEpoch.end());
    _remoteLoaderMock->setChunkRefreshReturnValue(mixedChunks);

    StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> mixedResults{
        Status(ErrorCodes::InternalError, "")};
    const auto mixedRefreshCallbackFn = [&mixedResults](
        OperationContext * opCtx,
        StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollAndChunks) noexcept {
        mixedResults = std::move(swCollAndChunks);
    };

    auto mixedNotification =
        _shardLoader->getChunksSince(kNss, chunks.back().getVersion(), mixedRefreshCallbackFn);
    mixedNotification->get();

    ASSERT_EQUALS(mixedResults.getStatus().code(), ErrorCodes::ConflictingOperationInProgress);

    // Now make sure the newly recreated collection is cleanly loaded. We cannot ensure a
    // non-variable response until the loader has remotely retrieved the new metadata and applied
    // them to the persisted store. So first do a reload and ignore the results. Then call again,
    // this time checking the results.

    _remoteLoaderMock->setCollectionRefreshReturnValue(collectionTypeWithNewEpoch);
    _remoteLoaderMock->setChunkRefreshReturnValue(chunksWithNewEpoch);

    auto cleanNotification =
        _shardLoader->getChunksSince(kNss, chunks.back().getVersion(), kDoNothingCallbackFn);
    cleanNotification->get();

    // Wait for persistence of update.
    _shardLoader->waitForCollectionFlush(operationContext(), kNss);

    vector<ChunkType> lastChunkWithNewEpoch;
    lastChunkWithNewEpoch.push_back(chunksWithNewEpoch.back());
    _remoteLoaderMock->setChunkRefreshReturnValue(lastChunkWithNewEpoch);

    StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> results{
        Status(ErrorCodes::InternalError, "")};
    const auto refreshCallbackFn = [&results](
        OperationContext * opCtx,
        StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollAndChunks) noexcept {
        results = std::move(swCollAndChunks);
    };

    auto notification =
        _shardLoader->getChunksSince(kNss, ChunkVersion::UNSHARDED(), refreshCallbackFn);
    notification->get();

    ASSERT_OK(results.getStatus());
    auto collAndChunksRes = results.getValue();
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

}  // namespace
}  // namespace mongo
