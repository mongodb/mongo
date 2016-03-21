/**
 *    Copyright (C) 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/base/status.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/metadata_loader.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set_test_fixture.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/write_ops/batched_command_response.h"

namespace mongo {
namespace {

using std::string;
using std::unique_ptr;
using std::vector;

using executor::RemoteCommandResponse;

class NoChunkFixture : public CatalogManagerReplSetTestFixture {
protected:
    void setUp() {
        CatalogManagerReplSetTestFixture::setUp();
        getMessagingPort()->setRemote(HostAndPort("FakeRemoteClient:34567"));
        configTargeter()->setFindHostReturnValue(configHost);

        OID epoch = OID::gen();

        CollectionType collType;
        collType.setNs(NamespaceString{"test.foo"});
        collType.setKeyPattern(BSON("a" << 1));
        collType.setUnique(false);
        collType.setUpdatedAt(Date_t::fromMillisSinceEpoch(1));
        collType.setEpoch(epoch);
        ASSERT_OK(collType.validate());

        // Need a chunk on another shard, otherwise the chunks are invalid in general and we
        // can't load metadata
        ChunkType chunkType;
        chunkType.setNS(NamespaceString{"test.foo"}.ns());
        chunkType.setShard("shard0001");
        chunkType.setMin(BSON("a" << MINKEY));
        chunkType.setMax(BSON("a" << MAXKEY));
        chunkType.setVersion(ChunkVersion(1, 0, epoch));
        chunkType.setName(OID::gen().toString());
        ASSERT_OK(chunkType.validate());
        std::vector<BSONObj> chunksToSend{chunkType.toBSON()};

        auto future = launchAsync([this] {
            MetadataLoader loader;
            auto status = loader.makeCollectionMetadata(operationContext(),
                                                        catalogManager(),
                                                        "test.foo",
                                                        "shard0000",
                                                        NULL, /* no old metadata */
                                                        &_metadata);
            ASSERT_OK(status);
            ASSERT_EQUALS(0u, _metadata.getNumChunks());
        });

        expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{collType.toBSON()});
        expectFindOnConfigSendBSONObjVector(chunksToSend);

        future.timed_get(kFutureTimeout);
    }

    const CollectionMetadata& getCollMetadata() const {
        return _metadata;
    }

private:
    CollectionMetadata _metadata;
    const HostAndPort configHost{HostAndPort(CONFIG_HOST_PORT)};
};

TEST_F(NoChunkFixture, BasicBelongsToMe) {
    ASSERT_FALSE(getCollMetadata().keyBelongsToMe(BSON("a" << MINKEY)));
    ASSERT_FALSE(getCollMetadata().keyBelongsToMe(BSON("a" << 10)));
}

TEST_F(NoChunkFixture, CompoundKeyBelongsToMe) {
    ASSERT_FALSE(getCollMetadata().keyBelongsToMe(BSON("a" << 1 << "b" << 2)));
}

TEST_F(NoChunkFixture, IsKeyValid) {
    ASSERT_TRUE(getCollMetadata().isValidKey(BSON("a"
                                                  << "abcde")));
    ASSERT_TRUE(getCollMetadata().isValidKey(BSON("a" << 3)));
    ASSERT_FALSE(getCollMetadata().isValidKey(BSON("a"
                                                   << "abcde"
                                                   << "b" << 1)));
    ASSERT_FALSE(getCollMetadata().isValidKey(BSON("c"
                                                   << "abcde")));
}

TEST_F(NoChunkFixture, getNextFromEmpty) {
    ChunkType nextChunk;
    ASSERT(!getCollMetadata().getNextChunk(getCollMetadata().getMinKey(), &nextChunk));
}

TEST_F(NoChunkFixture, FirstChunkClonePlus) {
    ChunkType chunk;
    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 20));

    string errMsg;
    const ChunkVersion version(99, 0, OID());
    unique_ptr<CollectionMetadata> cloned(
        getCollMetadata().clonePlusChunk(chunk, version, &errMsg));

    ASSERT(errMsg.empty());
    ASSERT_EQUALS(1u, cloned->getNumChunks());
    ASSERT_EQUALS(cloned->getShardVersion().toLong(), version.toLong());
    ASSERT_EQUALS(cloned->getCollVersion().toLong(), version.toLong());
    ASSERT(cloned->keyBelongsToMe(BSON("a" << 15)));
}

TEST_F(NoChunkFixture, MustHaveVersionForFirstChunk) {
    ChunkType chunk;
    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 20));

    string errMsg;
    unique_ptr<CollectionMetadata> cloned(
        getCollMetadata()  // br
            .clonePlusChunk(chunk, ChunkVersion(0, 0, OID()), &errMsg));

    ASSERT(cloned == NULL);
    ASSERT_FALSE(errMsg.empty());
}

TEST_F(NoChunkFixture, NoPendingChunks) {
    ASSERT(!getCollMetadata().keyIsPending(BSON("a" << 15)));
    ASSERT(!getCollMetadata().keyIsPending(BSON("a" << 25)));
}

TEST_F(NoChunkFixture, FirstPendingChunk) {
    string errMsg;
    ChunkType chunk;
    unique_ptr<CollectionMetadata> cloned;

    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 20));

    cloned.reset(getCollMetadata().clonePlusPending(chunk, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);

    ASSERT(cloned->keyIsPending(BSON("a" << 15)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 25)));
    ASSERT(cloned->keyIsPending(BSON("a" << 10)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 20)));
}

TEST_F(NoChunkFixture, EmptyMultiPendingChunk) {
    string errMsg;
    ChunkType chunk;
    unique_ptr<CollectionMetadata> cloned;

    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 20));

    cloned.reset(getCollMetadata().clonePlusPending(chunk, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);

    chunk.setMin(BSON("a" << 40));
    chunk.setMax(BSON("a" << 50));

    cloned.reset(cloned->clonePlusPending(chunk, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);

    ASSERT(cloned->keyIsPending(BSON("a" << 15)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 25)));
    ASSERT(cloned->keyIsPending(BSON("a" << 45)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 55)));
}

TEST_F(NoChunkFixture, MinusPendingChunk) {
    string errMsg;
    ChunkType chunk;
    unique_ptr<CollectionMetadata> cloned;

    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 20));

    cloned.reset(getCollMetadata().clonePlusPending(chunk, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);

    cloned.reset(cloned->cloneMinusPending(chunk, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);

    ASSERT(!cloned->keyIsPending(BSON("a" << 15)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 25)));
}

TEST_F(NoChunkFixture, OverlappingPendingChunk) {
    string errMsg;
    ChunkType chunk;
    unique_ptr<CollectionMetadata> cloned;

    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 30));

    cloned.reset(getCollMetadata().clonePlusPending(chunk, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);

    chunk.setMin(BSON("a" << 20));
    chunk.setMax(BSON("a" << 40));

    cloned.reset(cloned->clonePlusPending(chunk, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);

    ASSERT(!cloned->keyIsPending(BSON("a" << 15)));
    ASSERT(cloned->keyIsPending(BSON("a" << 25)));
    ASSERT(cloned->keyIsPending(BSON("a" << 35)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 45)));
}

TEST_F(NoChunkFixture, OverlappingPendingChunks) {
    string errMsg;
    ChunkType chunk;
    unique_ptr<CollectionMetadata> cloned;

    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 30));

    cloned.reset(getCollMetadata().clonePlusPending(chunk, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);

    chunk.setMin(BSON("a" << 30));
    chunk.setMax(BSON("a" << 50));

    cloned.reset(cloned->clonePlusPending(chunk, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);

    chunk.setMin(BSON("a" << 20));
    chunk.setMax(BSON("a" << 40));

    cloned.reset(cloned->clonePlusPending(chunk, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);

    ASSERT(!cloned->keyIsPending(BSON("a" << 15)));
    ASSERT(cloned->keyIsPending(BSON("a" << 25)));
    ASSERT(cloned->keyIsPending(BSON("a" << 35)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 45)));
}

TEST_F(NoChunkFixture, MinusInvalidPendingChunk) {
    string errMsg;
    ChunkType chunk;
    unique_ptr<CollectionMetadata> cloned;

    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 30));

    cloned.reset(getCollMetadata().cloneMinusPending(chunk, &errMsg));

    ASSERT_NOT_EQUALS(errMsg, "");
    ASSERT(cloned == NULL);
}

TEST_F(NoChunkFixture, MinusOverlappingPendingChunk) {
    string errMsg;
    ChunkType chunk;
    unique_ptr<CollectionMetadata> cloned;

    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 30));

    cloned.reset(getCollMetadata().clonePlusPending(chunk, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);

    chunk.setMin(BSON("a" << 15));
    chunk.setMax(BSON("a" << 35));

    cloned.reset(cloned->cloneMinusPending(chunk, &errMsg));

    ASSERT_NOT_EQUALS(errMsg, "");
    ASSERT(cloned == NULL);
}

TEST_F(NoChunkFixture, PlusChunkWithPending) {
    string errMsg;
    ChunkType chunk;
    unique_ptr<CollectionMetadata> cloned;

    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 20));

    cloned.reset(getCollMetadata().clonePlusPending(chunk, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);

    ASSERT(cloned->keyIsPending(BSON("a" << 15)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 25)));

    chunk.setMin(BSON("a" << 20));
    chunk.setMax(BSON("a" << 30));

    cloned.reset(cloned->clonePlusChunk(
        chunk, ChunkVersion(1, 0, cloned->getCollVersion().epoch()), &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);

    ASSERT(cloned->keyIsPending(BSON("a" << 15)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 25)));
}

TEST_F(NoChunkFixture, MergeChunkEmpty) {
    string errMsg;
    unique_ptr<CollectionMetadata> cloned;

    cloned.reset(getCollMetadata().cloneMerge(
        BSON("a" << 15), BSON("a" << 25), ChunkVersion(1, 0, OID::gen()), &errMsg));

    ASSERT_NOT_EQUALS(errMsg, "");
    ASSERT(cloned == NULL);
}

TEST_F(NoChunkFixture, OrphanedDataRangeBegin) {
    const CollectionMetadata& metadata = getCollMetadata();

    KeyRange keyRange;
    BSONObj lookupKey = metadata.getMinKey();
    ASSERT(metadata.getNextOrphanRange(lookupKey, &keyRange));

    ASSERT(keyRange.minKey.woCompare(metadata.getMinKey()) == 0);
    ASSERT(keyRange.maxKey.woCompare(metadata.getMaxKey()) == 0);

    // Make sure we don't have any more ranges
    ASSERT(!metadata.getNextOrphanRange(keyRange.maxKey, &keyRange));
}

TEST_F(NoChunkFixture, OrphanedDataRangeMiddle) {
    const CollectionMetadata& metadata = getCollMetadata();

    KeyRange keyRange;
    BSONObj lookupKey = BSON("a" << 20);
    ASSERT(metadata.getNextOrphanRange(lookupKey, &keyRange));

    ASSERT(keyRange.minKey.woCompare(metadata.getMinKey()) == 0);
    ASSERT(keyRange.maxKey.woCompare(metadata.getMaxKey()) == 0);
    ASSERT(keyRange.keyPattern.woCompare(metadata.getKeyPattern()) == 0);

    // Make sure we don't have any more ranges
    ASSERT(!metadata.getNextOrphanRange(keyRange.maxKey, &keyRange));
}

TEST_F(NoChunkFixture, OrphanedDataRangeEnd) {
    const CollectionMetadata& metadata = getCollMetadata();

    KeyRange keyRange;
    ASSERT(!metadata.getNextOrphanRange(metadata.getMaxKey(), &keyRange));
}

TEST_F(NoChunkFixture, PendingOrphanedDataRanges) {
    string errMsg;
    ChunkType chunk;
    unique_ptr<CollectionMetadata> cloned;

    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 20));

    cloned.reset(getCollMetadata().clonePlusPending(chunk, &errMsg));
    ASSERT_EQUALS(errMsg, string(""));
    ASSERT(cloned != NULL);

    KeyRange keyRange;
    ASSERT(cloned->getNextOrphanRange(cloned->getMinKey(), &keyRange));
    ASSERT(keyRange.minKey.woCompare(cloned->getMinKey()) == 0);
    ASSERT(keyRange.maxKey.woCompare(BSON("a" << 10)) == 0);
    ASSERT(keyRange.keyPattern.woCompare(cloned->getKeyPattern()) == 0);

    ASSERT(cloned->getNextOrphanRange(keyRange.maxKey, &keyRange));
    ASSERT(keyRange.minKey.woCompare(BSON("a" << 20)) == 0);
    ASSERT(keyRange.maxKey.woCompare(cloned->getMaxKey()) == 0);
    ASSERT(keyRange.keyPattern.woCompare(cloned->getKeyPattern()) == 0);

    ASSERT(!cloned->getNextOrphanRange(keyRange.maxKey, &keyRange));
}

/**
 * Fixture with single chunk containing:
 * [10->20)
 */
class SingleChunkFixture : public CatalogManagerReplSetTestFixture {
protected:
    void setUp() {
        CatalogManagerReplSetTestFixture::setUp();
        getMessagingPort()->setRemote(HostAndPort("FakeRemoteClient:34567"));
        configTargeter()->setFindHostReturnValue(configHost);

        OID epoch = OID::gen();

        ChunkVersion chunkVersion = ChunkVersion(1, 0, epoch);

        CollectionType collType;
        collType.setNs(NamespaceString{"test.foo"});
        collType.setKeyPattern(BSON("a" << 1));
        collType.setUnique(false);
        collType.setUpdatedAt(Date_t::fromMillisSinceEpoch(1));
        collType.setEpoch(epoch);

        BSONObj fooSingle = BSON(
            ChunkType::name("test.foo-a_10")
            << ChunkType::ns("test.foo") << ChunkType::min(BSON("a" << 10))
            << ChunkType::max(BSON("a" << 20))
            << ChunkType::DEPRECATED_lastmod(Date_t::fromMillisSinceEpoch(chunkVersion.toLong()))
            << ChunkType::DEPRECATED_epoch(epoch) << ChunkType::shard("shard0000"));
        std::vector<BSONObj> chunksToSend{fooSingle};

        auto future = launchAsync([this] {
            MetadataLoader loader;
            auto status = loader.makeCollectionMetadata(operationContext(),
                                                        catalogManager(),
                                                        "test.foo",
                                                        "shard0000",
                                                        NULL, /* no old metadata */
                                                        &_metadata);
            ASSERT_OK(status);
        });

        expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{collType.toBSON()});
        expectFindOnConfigSendBSONObjVector(chunksToSend);

        future.timed_get(kFutureTimeout);
    }

    const CollectionMetadata& getCollMetadata() const {
        return _metadata;
    }

private:
    CollectionMetadata _metadata;
    const HostAndPort configHost{HostAndPort(CONFIG_HOST_PORT)};
};

TEST_F(SingleChunkFixture, BasicBelongsToMe) {
    ASSERT(getCollMetadata().keyBelongsToMe(BSON("a" << 10)));
    ASSERT(getCollMetadata().keyBelongsToMe(BSON("a" << 15)));
    ASSERT(getCollMetadata().keyBelongsToMe(BSON("a" << 19)));
}

TEST_F(SingleChunkFixture, DoesntBelongsToMe) {
    ASSERT_FALSE(getCollMetadata().keyBelongsToMe(BSON("a" << 0)));
    ASSERT_FALSE(getCollMetadata().keyBelongsToMe(BSON("a" << 9)));
    ASSERT_FALSE(getCollMetadata().keyBelongsToMe(BSON("a" << 20)));
    ASSERT_FALSE(getCollMetadata().keyBelongsToMe(BSON("a" << 1234)));
    ASSERT_FALSE(getCollMetadata().keyBelongsToMe(BSON("a" << MINKEY)));
    ASSERT_FALSE(getCollMetadata().keyBelongsToMe(BSON("a" << MAXKEY)));
}

TEST_F(SingleChunkFixture, CompoudKeyBelongsToMe) {
    ASSERT(getCollMetadata().keyBelongsToMe(BSON("a" << 15 << "a" << 14)));
}

TEST_F(SingleChunkFixture, getNextFromEmpty) {
    ChunkType nextChunk;
    ASSERT(getCollMetadata().getNextChunk(getCollMetadata().getMinKey(), &nextChunk));
    ASSERT_EQUALS(0, nextChunk.getMin().woCompare(BSON("a" << 10)));
    ASSERT_EQUALS(0, nextChunk.getMax().woCompare(BSON("a" << 20)));
}

TEST_F(SingleChunkFixture, GetLastChunkIsFalse) {
    ChunkType nextChunk;
    ASSERT(!getCollMetadata().getNextChunk(getCollMetadata().getMaxKey(), &nextChunk));
}

TEST_F(SingleChunkFixture, DonateLastChunk) {
    ChunkType chunk;
    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 20));

    ChunkVersion newCollectionVersion(getCollMetadata().getCollVersion());
    newCollectionVersion.incMajor();

    unique_ptr<CollectionMetadata> cloned(
        getCollMetadata().cloneMigrate(chunk, newCollectionVersion));

    ASSERT_EQUALS(0u, cloned->getNumChunks());
    ASSERT_EQUALS(cloned->getShardVersion(), ChunkVersion(0, 0, newCollectionVersion.epoch()));
    ASSERT_EQUALS(cloned->getCollVersion(), newCollectionVersion);
    ASSERT(!cloned->keyBelongsToMe(BSON("a" << 15)));
}

TEST_F(SingleChunkFixture, PlusPendingChunk) {
    string errMsg;
    ChunkType chunk;
    unique_ptr<CollectionMetadata> cloned;

    chunk.setMin(BSON("a" << 20));
    chunk.setMax(BSON("a" << 30));

    cloned.reset(getCollMetadata().clonePlusPending(chunk, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);

    ASSERT(cloned->keyBelongsToMe(BSON("a" << 15)));
    ASSERT(!cloned->keyBelongsToMe(BSON("a" << 25)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 15)));
    ASSERT(cloned->keyIsPending(BSON("a" << 25)));
}

TEST_F(SingleChunkFixture, PlusOverlapPendingChunk) {
    string errMsg;
    ChunkType chunk;
    unique_ptr<CollectionMetadata> cloned;

    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 20));

    cloned.reset(getCollMetadata().clonePlusPending(chunk, &errMsg));

    ASSERT_NOT_EQUALS(errMsg, "");
    ASSERT(cloned == NULL);
}

TEST_F(SingleChunkFixture, MinusChunkWithPending) {
    ChunkType chunk;
    chunk.setMin(BSON("a" << 20));
    chunk.setMax(BSON("a" << 30));

    string errMsg;
    unique_ptr<CollectionMetadata> cloned(getCollMetadata().clonePlusPending(chunk, &errMsg));
    ASSERT(cloned->keyIsPending(BSON("a" << 25)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 35)));

    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 20));

    ChunkVersion newCollectionVersion(getCollMetadata().getCollVersion());
    newCollectionVersion.incMajor();

    cloned = cloned->cloneMigrate(chunk, newCollectionVersion);
    ASSERT(cloned);
    ASSERT(cloned->keyIsPending(BSON("a" << 25)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 35)));
}

TEST_F(SingleChunkFixture, SingleSplit) {
    ChunkVersion version = getCollMetadata().getCollVersion();
    version.incMinor();

    ChunkType chunk;
    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 20));

    vector<BSONObj> splitPoints;
    splitPoints.push_back(BSON("a" << 14));

    string errMsg;
    unique_ptr<CollectionMetadata> cloned(
        getCollMetadata().cloneSplit(chunk, splitPoints, version, &errMsg));
    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);

    ChunkVersion newVersion(cloned->getCollVersion());
    ASSERT_EQUALS(version.epoch(), newVersion.epoch());
    ASSERT_EQUALS(version.majorVersion(), newVersion.majorVersion());
    ASSERT_EQUALS(version.minorVersion() + 1, newVersion.minorVersion());

    ASSERT(cloned->getNextChunk(BSON("a" << MINKEY), &chunk));
    ASSERT(chunk.getMin().woCompare(BSON("a" << 10)) == 0);
    ASSERT(chunk.getMax().woCompare(BSON("a" << 14)) == 0);

    ASSERT(cloned->getNextChunk(BSON("a" << 14), &chunk));
    ASSERT(chunk.getMin().woCompare(BSON("a" << 14)) == 0);
    ASSERT(chunk.getMax().woCompare(BSON("a" << 20)) == 0);

    ASSERT_FALSE(cloned->getNextChunk(BSON("a" << 20), &chunk));
}

TEST_F(SingleChunkFixture, MultiSplit) {
    string errMsg;
    ChunkType chunk;
    unique_ptr<CollectionMetadata> cloned;

    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 20));

    vector<BSONObj> splitPoints;
    splitPoints.push_back(BSON("a" << 14));
    splitPoints.push_back(BSON("a" << 16));

    ChunkVersion version = getCollMetadata().getCollVersion();
    version.incMinor();

    cloned.reset(getCollMetadata().cloneSplit(chunk, splitPoints, version, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);

    ChunkVersion newVersion(cloned->getCollVersion());
    ASSERT_EQUALS(version.epoch(), newVersion.epoch());
    ASSERT_EQUALS(version.majorVersion(), newVersion.majorVersion());
    ASSERT_EQUALS(version.minorVersion() + 2, newVersion.minorVersion());

    ASSERT(cloned->getNextChunk(BSON("a" << MINKEY), &chunk));
    ASSERT(chunk.getMin().woCompare(BSON("a" << 10)) == 0);
    ASSERT(chunk.getMax().woCompare(BSON("a" << 14)) == 0);

    ASSERT(cloned->getNextChunk(BSON("a" << 14), &chunk));
    ASSERT(chunk.getMin().woCompare(BSON("a" << 14)) == 0);
    ASSERT(chunk.getMax().woCompare(BSON("a" << 16)) == 0);

    ASSERT(cloned->getNextChunk(BSON("a" << 16), &chunk));
    ASSERT(chunk.getMin().woCompare(BSON("a" << 16)) == 0);
    ASSERT(chunk.getMax().woCompare(BSON("a" << 20)) == 0);

    ASSERT_FALSE(cloned->getNextChunk(BSON("a" << 20), &chunk));
}

TEST_F(SingleChunkFixture, SplitChunkWithPending) {
    string errMsg;
    ChunkType chunk;
    unique_ptr<CollectionMetadata> cloned;

    chunk.setMin(BSON("a" << 20));
    chunk.setMax(BSON("a" << 30));

    cloned.reset(getCollMetadata().clonePlusPending(chunk, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);

    ASSERT(cloned->keyIsPending(BSON("a" << 25)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 35)));

    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 20));

    vector<BSONObj> splitPoints;
    splitPoints.push_back(BSON("a" << 14));
    splitPoints.push_back(BSON("a" << 16));

    cloned.reset(cloned->cloneSplit(chunk,
                                    splitPoints,
                                    ChunkVersion(cloned->getCollVersion().majorVersion() + 1,
                                                 0,
                                                 cloned->getCollVersion().epoch()),
                                    &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);

    ASSERT(cloned->keyIsPending(BSON("a" << 25)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 35)));
}


TEST_F(SingleChunkFixture, MergeChunkSingle) {
    string errMsg;
    unique_ptr<CollectionMetadata> cloned;

    cloned.reset(getCollMetadata().cloneMerge(
        BSON("a" << 10), BSON("a" << 20), ChunkVersion(2, 0, OID::gen()), &errMsg));

    ASSERT_NOT_EQUALS(errMsg, "");
    ASSERT(cloned == NULL);
}

TEST_F(SingleChunkFixture, ChunkOrphanedDataRanges) {
    KeyRange keyRange;
    ASSERT(getCollMetadata().getNextOrphanRange(getCollMetadata().getMinKey(), &keyRange));
    ASSERT(keyRange.minKey.woCompare(getCollMetadata().getMinKey()) == 0);
    ASSERT(keyRange.maxKey.woCompare(BSON("a" << 10)) == 0);
    ASSERT(keyRange.keyPattern.woCompare(getCollMetadata().getKeyPattern()) == 0);

    ASSERT(getCollMetadata().getNextOrphanRange(keyRange.maxKey, &keyRange));
    ASSERT(keyRange.minKey.woCompare(BSON("a" << 20)) == 0);
    ASSERT(keyRange.maxKey.woCompare(getCollMetadata().getMaxKey()) == 0);
    ASSERT(keyRange.keyPattern.woCompare(getCollMetadata().getKeyPattern()) == 0);

    ASSERT(!getCollMetadata().getNextOrphanRange(keyRange.maxKey, &keyRange));
}

/**
 * Fixture with single chunk containing:
 * [(min, min)->(max, max))
 */
class SingleChunkMinMaxCompoundKeyFixture : public CatalogManagerReplSetTestFixture {
protected:
    void setUp() {
        CatalogManagerReplSetTestFixture::setUp();
        getMessagingPort()->setRemote(HostAndPort("FakeRemoteClient:34567"));
        configTargeter()->setFindHostReturnValue(configHost);

        OID epoch = OID::gen();

        ChunkVersion chunkVersion = ChunkVersion(1, 0, epoch);

        CollectionType collType;
        collType.setNs(NamespaceString{"test.foo"});
        collType.setKeyPattern(BSON("a" << 1));
        collType.setUnique(false);
        collType.setUpdatedAt(Date_t::fromMillisSinceEpoch(1));
        collType.setEpoch(epoch);

        BSONObj fooSingle = BSON(
            ChunkType::name("test.foo-a_MinKey")
            << ChunkType::ns("test.foo") << ChunkType::min(BSON("a" << MINKEY << "b" << MINKEY))
            << ChunkType::max(BSON("a" << MAXKEY << "b" << MAXKEY))
            << ChunkType::DEPRECATED_lastmod(Date_t::fromMillisSinceEpoch(chunkVersion.toLong()))
            << ChunkType::DEPRECATED_epoch(epoch) << ChunkType::shard("shard0000"));
        std::vector<BSONObj> chunksToSend{fooSingle};

        auto future = launchAsync([this] {
            MetadataLoader loader;
            auto status = loader.makeCollectionMetadata(operationContext(),
                                                        catalogManager(),
                                                        "test.foo",
                                                        "shard0000",
                                                        NULL, /* no old metadata */
                                                        &_metadata);
            ASSERT_OK(status);
        });

        expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{collType.toBSON()});
        expectFindOnConfigSendBSONObjVector(chunksToSend);

        future.timed_get(kFutureTimeout);
    }

    const CollectionMetadata& getCollMetadata() const {
        return _metadata;
    }

private:
    CollectionMetadata _metadata;
    const HostAndPort configHost{HostAndPort(CONFIG_HOST_PORT)};
};

// Note: no tests for single key belongsToMe because they are not allowed
// if shard key is compound.

TEST_F(SingleChunkMinMaxCompoundKeyFixture, CompoudKeyBelongsToMe) {
    ASSERT(getCollMetadata().keyBelongsToMe(BSON("a" << MINKEY << "b" << MINKEY)));
    ASSERT_FALSE(getCollMetadata().keyBelongsToMe(BSON("a" << MAXKEY << "b" << MAXKEY)));
    ASSERT(getCollMetadata().keyBelongsToMe(BSON("a" << MINKEY << "b" << 10)));
    ASSERT(getCollMetadata().keyBelongsToMe(BSON("a" << 10 << "b" << 20)));
}

/**
 * Fixture with chunks:
 * [(10, 0)->(20, 0)), [(30, 0)->(40, 0))
 */
class TwoChunksWithGapCompoundKeyFixture : public CatalogManagerReplSetTestFixture {
protected:
    void setUp() {
        CatalogManagerReplSetTestFixture::setUp();
        getMessagingPort()->setRemote(HostAndPort("FakeRemoteClient:34567"));
        configTargeter()->setFindHostReturnValue(configHost);

        OID epoch = OID::gen();

        ChunkVersion chunkVersion = ChunkVersion(1, 0, epoch);

        CollectionType collType;
        collType.setNs(NamespaceString{"test.foo"});
        collType.setKeyPattern(BSON("a" << 1));
        collType.setUnique(false);
        collType.setUpdatedAt(Date_t::fromMillisSinceEpoch(1));
        collType.setEpoch(epoch);

        std::vector<BSONObj> chunksToSend;
        chunksToSend.push_back(BSON(
            ChunkType::name("test.foo-a_10")
            << ChunkType::ns("test.foo") << ChunkType::min(BSON("a" << 10 << "b" << 0))
            << ChunkType::max(BSON("a" << 20 << "b" << 0))
            << ChunkType::DEPRECATED_lastmod(Date_t::fromMillisSinceEpoch(chunkVersion.toLong()))
            << ChunkType::DEPRECATED_epoch(epoch) << ChunkType::shard("shard0000")));
        chunksToSend.push_back(BSON(
            ChunkType::name("test.foo-a_10")
            << ChunkType::ns("test.foo") << ChunkType::min(BSON("a" << 30 << "b" << 0))
            << ChunkType::max(BSON("a" << 40 << "b" << 0))
            << ChunkType::DEPRECATED_lastmod(Date_t::fromMillisSinceEpoch(chunkVersion.toLong()))
            << ChunkType::DEPRECATED_epoch(epoch) << ChunkType::shard("shard0000")));

        auto future = launchAsync([this] {
            MetadataLoader loader;
            auto status = loader.makeCollectionMetadata(operationContext(),
                                                        catalogManager(),
                                                        "test.foo",
                                                        "shard0000",
                                                        NULL, /* no old metadata */
                                                        &_metadata);
            ASSERT_OK(status);
        });

        expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{collType.toBSON()});
        expectFindOnConfigSendBSONObjVector(chunksToSend);

        future.timed_get(kFutureTimeout);
    }

    const CollectionMetadata& getCollMetadata() const {
        return _metadata;
    }

private:
    CollectionMetadata _metadata;
    const HostAndPort configHost{HostAndPort(CONFIG_HOST_PORT)};
};

TEST_F(TwoChunksWithGapCompoundKeyFixture, ClonePlusBasic) {
    ChunkType chunk;
    chunk.setMin(BSON("a" << 40 << "b" << 0));
    chunk.setMax(BSON("a" << 50 << "b" << 0));

    string errMsg;
    ChunkVersion version(1, 0, getCollMetadata().getShardVersion().epoch());
    unique_ptr<CollectionMetadata> cloned(
        getCollMetadata().clonePlusChunk(chunk, version, &errMsg));

    ASSERT(errMsg.empty());
    ASSERT_EQUALS(2u, getCollMetadata().getNumChunks());
    ASSERT_EQUALS(3u, cloned->getNumChunks());

    // TODO: test maxShardVersion, maxCollVersion

    ASSERT_FALSE(cloned->keyBelongsToMe(BSON("a" << 25 << "b" << 0)));
    ASSERT_FALSE(cloned->keyBelongsToMe(BSON("a" << 29 << "b" << 0)));
    ASSERT(cloned->keyBelongsToMe(BSON("a" << 30 << "b" << 0)));
    ASSERT(cloned->keyBelongsToMe(BSON("a" << 45 << "b" << 0)));
    ASSERT(cloned->keyBelongsToMe(BSON("a" << 49 << "b" << 0)));
    ASSERT_FALSE(cloned->keyBelongsToMe(BSON("a" << 50 << "b" << 0)));
}

TEST_F(TwoChunksWithGapCompoundKeyFixture, ClonePlusOverlappingRange) {
    ChunkType chunk;
    chunk.setMin(BSON("a" << 15 << "b" << 0));
    chunk.setMax(BSON("a" << 25 << "b" << 0));

    string errMsg;
    unique_ptr<CollectionMetadata> cloned(
        getCollMetadata().clonePlusChunk(chunk, ChunkVersion(1, 0, OID()), &errMsg));
    ASSERT(cloned == NULL);
    ASSERT_FALSE(errMsg.empty());
    ASSERT_EQUALS(2u, getCollMetadata().getNumChunks());
}

TEST_F(TwoChunksWithGapCompoundKeyFixture, CloneMinusBasic) {
    ChunkType chunk;
    chunk.setMin(BSON("a" << 10 << "b" << 0));
    chunk.setMax(BSON("a" << 20 << "b" << 0));

    ChunkVersion newCollectionVersion(getCollMetadata().getCollVersion());
    newCollectionVersion.incMajor();

    unique_ptr<CollectionMetadata> cloned(
        getCollMetadata().cloneMigrate(chunk, newCollectionVersion));
    ASSERT_EQUALS(2u, getCollMetadata().getNumChunks());
    ASSERT_EQUALS(1u, cloned->getNumChunks());
    ASSERT_FALSE(cloned->keyBelongsToMe(BSON("a" << 5 << "b" << 0)));
    ASSERT_FALSE(cloned->keyBelongsToMe(BSON("a" << 15 << "b" << 0)));
    ASSERT(cloned->keyBelongsToMe(BSON("a" << 30 << "b" << 0)));
    ASSERT(cloned->keyBelongsToMe(BSON("a" << 35 << "b" << 0)));
    ASSERT_FALSE(cloned->keyBelongsToMe(BSON("a" << 40 << "b" << 0)));
}

TEST_F(TwoChunksWithGapCompoundKeyFixture, CloneSplitBasic) {
    const BSONObj min(BSON("a" << 10 << "b" << 0));
    const BSONObj max(BSON("a" << 20 << "b" << 0));

    ChunkType chunk;
    chunk.setMin(min);
    chunk.setMax(max);

    const BSONObj split1(BSON("a" << 15 << "b" << 0));
    const BSONObj split2(BSON("a" << 18 << "b" << 0));
    vector<BSONObj> splitKeys;
    splitKeys.push_back(split1);
    splitKeys.push_back(split2);
    ChunkVersion version(1, 99, OID());  // first chunk 1|99 , second 1|100

    string errMsg;
    unique_ptr<CollectionMetadata> cloned(
        getCollMetadata().cloneSplit(chunk, splitKeys, version, &errMsg));

    version.incMinor(); /* second chunk 1|100, first split point */
    version.incMinor(); /* third chunk 1|101, second split point */
    ASSERT_EQUALS(cloned->getShardVersion().toLong(), version.toLong() /* 1|101 */);
    ASSERT_EQUALS(cloned->getCollVersion().toLong(), version.toLong());
    ASSERT_EQUALS(getCollMetadata().getNumChunks(), 2u);
    ASSERT_EQUALS(cloned->getNumChunks(), 4u);
    ASSERT(cloned->keyBelongsToMe(min));
    ASSERT(cloned->keyBelongsToMe(split1));
    ASSERT(cloned->keyBelongsToMe(split2));
    ASSERT(!cloned->keyBelongsToMe(max));
}

TEST_F(TwoChunksWithGapCompoundKeyFixture, CloneSplitOutOfRangeSplitPoint) {
    ChunkType chunk;
    chunk.setMin(BSON("a" << 10 << "b" << 0));
    chunk.setMax(BSON("a" << 20 << "b" << 0));

    vector<BSONObj> splitKeys;
    splitKeys.push_back(BSON("a" << 5 << "b" << 0));

    string errMsg;
    unique_ptr<CollectionMetadata> cloned(
        getCollMetadata().cloneSplit(chunk, splitKeys, ChunkVersion(1, 0, OID()), &errMsg));

    ASSERT(cloned == NULL);
    ASSERT_FALSE(errMsg.empty());
    ASSERT_EQUALS(2u, getCollMetadata().getNumChunks());
}

TEST_F(TwoChunksWithGapCompoundKeyFixture, CloneSplitBadChunkRange) {
    const BSONObj min(BSON("a" << 10 << "b" << 0));
    const BSONObj max(BSON("a" << 25 << "b" << 0));

    ChunkType chunk;
    chunk.setMin(BSON("a" << 10 << "b" << 0));
    chunk.setMax(BSON("a" << 25 << "b" << 0));

    vector<BSONObj> splitKeys;
    splitKeys.push_back(BSON("a" << 15 << "b" << 0));

    string errMsg;
    unique_ptr<CollectionMetadata> cloned(
        getCollMetadata().cloneSplit(chunk, splitKeys, ChunkVersion(1, 0, OID()), &errMsg));

    ASSERT(cloned == NULL);
    ASSERT_FALSE(errMsg.empty());
    ASSERT_EQUALS(2u, getCollMetadata().getNumChunks());
}

TEST_F(TwoChunksWithGapCompoundKeyFixture, ChunkGapOrphanedDataRanges) {
    KeyRange keyRange;
    ASSERT(getCollMetadata().getNextOrphanRange(getCollMetadata().getMinKey(), &keyRange));
    ASSERT(keyRange.minKey.woCompare(getCollMetadata().getMinKey()) == 0);
    ASSERT(keyRange.maxKey.woCompare(BSON("a" << 10 << "b" << 0)) == 0);
    ASSERT(keyRange.keyPattern.woCompare(getCollMetadata().getKeyPattern()) == 0);

    ASSERT(getCollMetadata().getNextOrphanRange(keyRange.maxKey, &keyRange));
    ASSERT(keyRange.minKey.woCompare(BSON("a" << 20 << "b" << 0)) == 0);
    ASSERT(keyRange.maxKey.woCompare(BSON("a" << 30 << "b" << 0)) == 0);
    ASSERT(keyRange.keyPattern.woCompare(getCollMetadata().getKeyPattern()) == 0);

    ASSERT(getCollMetadata().getNextOrphanRange(keyRange.maxKey, &keyRange));
    ASSERT(keyRange.minKey.woCompare(BSON("a" << 40 << "b" << 0)) == 0);
    ASSERT(keyRange.maxKey.woCompare(getCollMetadata().getMaxKey()) == 0);
    ASSERT(keyRange.keyPattern.woCompare(getCollMetadata().getKeyPattern()) == 0);

    ASSERT(!getCollMetadata().getNextOrphanRange(keyRange.maxKey, &keyRange));
}

TEST_F(TwoChunksWithGapCompoundKeyFixture, ChunkGapAndPendingOrphanedDataRanges) {
    string errMsg;
    ChunkType chunk;
    unique_ptr<CollectionMetadata> cloned;

    chunk.setMin(BSON("a" << 20 << "b" << 0));
    chunk.setMax(BSON("a" << 30 << "b" << 0));

    cloned.reset(getCollMetadata().clonePlusPending(chunk, &errMsg));
    ASSERT_EQUALS(errMsg, string(""));
    ASSERT(cloned != NULL);

    KeyRange keyRange;
    ASSERT(cloned->getNextOrphanRange(cloned->getMinKey(), &keyRange));
    ASSERT(keyRange.minKey.woCompare(cloned->getMinKey()) == 0);
    ASSERT(keyRange.maxKey.woCompare(BSON("a" << 10 << "b" << 0)) == 0);
    ASSERT(keyRange.keyPattern.woCompare(cloned->getKeyPattern()) == 0);

    ASSERT(cloned->getNextOrphanRange(keyRange.maxKey, &keyRange));
    ASSERT(keyRange.minKey.woCompare(BSON("a" << 40 << "b" << 0)) == 0);
    ASSERT(keyRange.maxKey.woCompare(cloned->getMaxKey()) == 0);
    ASSERT(keyRange.keyPattern.woCompare(cloned->getKeyPattern()) == 0);

    ASSERT(!cloned->getNextOrphanRange(keyRange.maxKey, &keyRange));
}

/**
 * Fixture with chunk containing:
 * [min->10) , [10->20) , <gap> , [30->max)
 */
class ThreeChunkWithRangeGapFixture : public CatalogManagerReplSetTestFixture {
protected:
    void setUp() {
        CatalogManagerReplSetTestFixture::setUp();
        getMessagingPort()->setRemote(HostAndPort("FakeRemoteClient:34567"));
        configTargeter()->setFindHostReturnValue(configHost);

        OID epoch = OID::gen();

        CollectionType collType;
        collType.setNs(NamespaceString{"x.y"});
        collType.setKeyPattern(BSON("a" << 1));
        collType.setUnique(false);
        collType.setUpdatedAt(Date_t::fromMillisSinceEpoch(1));
        collType.setEpoch(epoch);

        std::vector<BSONObj> chunksToSend;
        {
            ChunkVersion version(1, 1, epoch);
            chunksToSend.push_back(BSON(
                ChunkType::name("x.y-a_MinKey")
                << ChunkType::ns("x.y") << ChunkType::min(BSON("a" << MINKEY))
                << ChunkType::max(BSON("a" << 10))
                << ChunkType::DEPRECATED_lastmod(Date_t::fromMillisSinceEpoch(version.toLong()))
                << ChunkType::DEPRECATED_epoch(version.epoch()) << ChunkType::shard("shard0000")));
        }

        {
            ChunkVersion version(1, 3, epoch);
            chunksToSend.push_back(BSON(
                ChunkType::name("x.y-a_10")
                << ChunkType::ns("x.y") << ChunkType::min(BSON("a" << 10))
                << ChunkType::max(BSON("a" << 20))
                << ChunkType::DEPRECATED_lastmod(Date_t::fromMillisSinceEpoch(version.toLong()))
                << ChunkType::DEPRECATED_epoch(version.epoch()) << ChunkType::shard("shard0000")));
        }

        {
            ChunkVersion version(1, 2, epoch);
            chunksToSend.push_back(BSON(
                ChunkType::name("x.y-a_30")
                << ChunkType::ns("x.y") << ChunkType::min(BSON("a" << 30))
                << ChunkType::max(BSON("a" << MAXKEY))
                << ChunkType::DEPRECATED_lastmod(Date_t::fromMillisSinceEpoch(version.toLong()))
                << ChunkType::DEPRECATED_epoch(version.epoch()) << ChunkType::shard("shard0000")));
        }

        auto future = launchAsync([this] {
            MetadataLoader loader;
            auto status = loader.makeCollectionMetadata(operationContext(),
                                                        catalogManager(),
                                                        "test.foo",
                                                        "shard0000",
                                                        NULL, /* no old metadata */
                                                        &_metadata);
            ASSERT_OK(status);
        });

        expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{collType.toBSON()});
        expectFindOnConfigSendBSONObjVector(chunksToSend);

        future.timed_get(kFutureTimeout);
    }

    const CollectionMetadata& getCollMetadata() const {
        return _metadata;
    }

private:
    CollectionMetadata _metadata;
    const HostAndPort configHost{HostAndPort(CONFIG_HOST_PORT)};
};

TEST_F(ThreeChunkWithRangeGapFixture, ShardOwnsDoc) {
    ASSERT(getCollMetadata().keyBelongsToMe(BSON("a" << 5)));
    ASSERT(getCollMetadata().keyBelongsToMe(BSON("a" << 10)));
    ASSERT(getCollMetadata().keyBelongsToMe(BSON("a" << 30)));
    ASSERT(getCollMetadata().keyBelongsToMe(BSON("a" << 40)));
}

TEST_F(ThreeChunkWithRangeGapFixture, ShardDoesntOwnDoc) {
    ASSERT_FALSE(getCollMetadata().keyBelongsToMe(BSON("a" << 25)));
    ASSERT_FALSE(getCollMetadata().keyBelongsToMe(BSON("a" << MAXKEY)));
}

TEST_F(ThreeChunkWithRangeGapFixture, GetNextFromEmpty) {
    ChunkType nextChunk;
    ASSERT(getCollMetadata().getNextChunk(getCollMetadata().getMinKey(), &nextChunk));
    ASSERT_EQUALS(0, nextChunk.getMin().woCompare(BSON("a" << MINKEY)));
    ASSERT_EQUALS(0, nextChunk.getMax().woCompare(BSON("a" << 10)));
}

TEST_F(ThreeChunkWithRangeGapFixture, GetNextFromMiddle) {
    ChunkType nextChunk;
    ASSERT(getCollMetadata().getNextChunk(BSON("a" << 20), &nextChunk));
    ASSERT_EQUALS(0, nextChunk.getMin().woCompare(BSON("a" << 30)));
    ASSERT_EQUALS(0, nextChunk.getMax().woCompare(BSON("a" << MAXKEY)));
}

TEST_F(ThreeChunkWithRangeGapFixture, GetNextFromLast) {
    ChunkType nextChunk;
    ASSERT(getCollMetadata().getNextChunk(BSON("a" << 30), &nextChunk));
}

TEST_F(ThreeChunkWithRangeGapFixture, MergeChunkHoleInRange) {
    string errMsg;
    unique_ptr<CollectionMetadata> cloned;

    // Try to merge with hole in range
    ChunkVersion newShardVersion(5, 0, getCollMetadata().getShardVersion().epoch());
    cloned.reset(getCollMetadata().cloneMerge(
        BSON("a" << 10), BSON("a" << MAXKEY), newShardVersion, &errMsg));

    ASSERT_NOT_EQUALS(errMsg, "");
    ASSERT(cloned == NULL);
}

TEST_F(ThreeChunkWithRangeGapFixture, MergeChunkDiffEndKey) {
    string errMsg;
    unique_ptr<CollectionMetadata> cloned;

    // Try to merge with different end key
    ChunkVersion newShardVersion(5, 0, getCollMetadata().getShardVersion().epoch());
    cloned.reset(getCollMetadata().cloneMerge(
        BSON("a" << MINKEY), BSON("a" << 19), newShardVersion, &errMsg));

    ASSERT_NOT_EQUALS(errMsg, "");
    ASSERT(cloned == NULL);
}

TEST_F(ThreeChunkWithRangeGapFixture, MergeChunkMinKey) {
    string errMsg;
    unique_ptr<CollectionMetadata> cloned;

    ASSERT_EQUALS(getCollMetadata().getNumChunks(), 3u);

    // Try to merge lowest chunks together
    ChunkVersion newShardVersion(5, 0, getCollMetadata().getShardVersion().epoch());
    cloned.reset(getCollMetadata().cloneMerge(
        BSON("a" << MINKEY), BSON("a" << 20), newShardVersion, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);
    ASSERT(cloned->keyBelongsToMe(BSON("a" << 10)));
    ASSERT_EQUALS(cloned->getNumChunks(), 2u);
    ASSERT_EQUALS(cloned->getShardVersion().majorVersion(), 5);
}

TEST_F(ThreeChunkWithRangeGapFixture, MergeChunkMaxKey) {
    string errMsg;
    unique_ptr<CollectionMetadata> cloned;
    ChunkVersion newShardVersion(5, 0, getCollMetadata().getShardVersion().epoch());

    // Add one chunk to complete the range
    ChunkType chunk;
    chunk.setMin(BSON("a" << 20));
    chunk.setMax(BSON("a" << 30));
    cloned.reset(getCollMetadata().clonePlusChunk(chunk, newShardVersion, &errMsg));
    ASSERT_EQUALS(errMsg, "");
    ASSERT_EQUALS(cloned->getNumChunks(), 4u);
    ASSERT(cloned != NULL);

    // Try to merge highest chunks together
    newShardVersion.incMajor();
    cloned.reset(
        cloned->cloneMerge(BSON("a" << 20), BSON("a" << MAXKEY), newShardVersion, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);
    ASSERT(cloned->keyBelongsToMe(BSON("a" << 30)));
    ASSERT_EQUALS(cloned->getNumChunks(), 3u);
    ASSERT_EQUALS(cloned->getShardVersion().majorVersion(), 6);
}

TEST_F(ThreeChunkWithRangeGapFixture, MergeChunkFullRange) {
    string errMsg;
    unique_ptr<CollectionMetadata> cloned;
    ChunkVersion newShardVersion(5, 0, getCollMetadata().getShardVersion().epoch());

    // Add one chunk to complete the range
    ChunkType chunk;
    chunk.setMin(BSON("a" << 20));
    chunk.setMax(BSON("a" << 30));
    cloned.reset(getCollMetadata().clonePlusChunk(chunk, newShardVersion, &errMsg));
    ASSERT_EQUALS(errMsg, "");
    ASSERT_EQUALS(cloned->getNumChunks(), 4u);
    ASSERT(cloned != NULL);

    // Try to merge all chunks together
    newShardVersion.incMajor();
    cloned.reset(
        cloned->cloneMerge(BSON("a" << MINKEY), BSON("a" << MAXKEY), newShardVersion, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);
    ASSERT(cloned->keyBelongsToMe(BSON("a" << 10)));
    ASSERT(cloned->keyBelongsToMe(BSON("a" << 30)));
    ASSERT_EQUALS(cloned->getNumChunks(), 1u);
    ASSERT_EQUALS(cloned->getShardVersion().majorVersion(), 6);
}

TEST_F(ThreeChunkWithRangeGapFixture, MergeChunkMiddleRange) {
    string errMsg;
    unique_ptr<CollectionMetadata> cloned;
    ChunkVersion newShardVersion(5, 0, getCollMetadata().getShardVersion().epoch());

    // Add one chunk to complete the range
    ChunkType chunk;
    chunk.setMin(BSON("a" << 20));
    chunk.setMax(BSON("a" << 30));
    cloned.reset(getCollMetadata().clonePlusChunk(chunk, newShardVersion, &errMsg));
    ASSERT_EQUALS(errMsg, "");
    ASSERT_EQUALS(cloned->getNumChunks(), 4u);
    ASSERT(cloned != NULL);

    // Try to merge middle two chunks
    newShardVersion.incMajor();
    cloned.reset(cloned->cloneMerge(BSON("a" << 10), BSON("a" << 30), newShardVersion, &errMsg));

    ASSERT_EQUALS(errMsg, "");
    ASSERT(cloned != NULL);
    ASSERT(cloned->keyBelongsToMe(BSON("a" << 20)));
    ASSERT_EQUALS(cloned->getNumChunks(), 3u);
    ASSERT_EQUALS(cloned->getShardVersion().majorVersion(), 6);
}

TEST_F(ThreeChunkWithRangeGapFixture, CannotMergeWithHole) {
    string errMsg;
    ChunkVersion newShardVersion(5, 0, getCollMetadata().getShardVersion().epoch());

    // Try to merge middle two chunks with a hole in the middle.
    newShardVersion.incMajor();
    CollectionMetadata* result =
        getCollMetadata().cloneMerge(BSON("a" << 10), BSON("a" << 30), newShardVersion, &errMsg);
    ASSERT(result == NULL);
    ASSERT(!errMsg.empty());
}

}  // namespace
}  // namespace mongo
