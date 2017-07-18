/**
 *    Copyright (C) 2017 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/shard_metadata_util.h"

#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard_collection.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::string;
using std::unique_ptr;
using std::vector;
using unittest::assertGet;
using namespace shardmetadatautil;

const NamespaceString kNss = NamespaceString("test.foo");
const NamespaceString kChunkMetadataNss = NamespaceString("config.chunks.test.foo");
const ShardId kShardId = ShardId("shard0");
const bool kUnique = false;

class ShardMetadataUtilTest : public ShardServerTestFixture {
protected:
    /**
     * Inserts a collections collection entry for 'kNss'.
     */
    ShardCollectionType setUpCollection() {
        BSONObjBuilder builder;
        builder.append(ShardCollectionType::uuid(), kNss.ns());
        builder.append(ShardCollectionType::ns(), kNss.ns());
        builder.append(ShardCollectionType::epoch(), _maxCollVersion.epoch());
        builder.append(ShardCollectionType::keyPattern(), _keyPattern.toBSON());
        builder.append(ShardCollectionType::defaultCollation(), _defaultCollation);
        builder.append(ShardCollectionType::unique(), kUnique);
        ShardCollectionType shardCollectionType =
            assertGet(ShardCollectionType::fromBSON(builder.obj()));

        ASSERT_OK(updateShardCollectionsEntry(operationContext(),
                                              BSON(ShardCollectionType::uuid(kNss.ns())),
                                              shardCollectionType.toBSON(),
                                              true /*upsert*/));
        return shardCollectionType;
    }

    /**
     * Inserts 'chunks' into the shard's chunks collection for 'nss'.
     */
    void setUpChunks(const NamespaceString& nss, const std::vector<ChunkType> chunks) {
        NamespaceString chunkMetadataNss(ChunkType::ShardNSPrefix + nss.ns());

        ASSERT_OK(updateShardChunks(operationContext(), kNss, chunks, _maxCollVersion.epoch()));
    }

    /**
     * Helper to make four chunks that can then be manipulated in various ways in the tests.
     */
    std::vector<ChunkType> makeFourChunks() {
        std::vector<ChunkType> chunks;
        BSONObj mins[] = {BSON("a" << MINKEY), BSON("a" << 10), BSON("a" << 50), BSON("a" << 100)};
        BSONObj maxs[] = {BSON("a" << 10), BSON("a" << 50), BSON("a" << 100), BSON("a" << MAXKEY)};

        for (int i = 0; i < 4; ++i) {
            _maxCollVersion.incMajor();
            BSONObj shardChunk =
                BSON(ChunkType::minShardID(mins[i])
                     << ChunkType::max(maxs[i])
                     << ChunkType::shard(kShardId.toString())
                     << ChunkType::lastmod(Date_t::fromMillisSinceEpoch(_maxCollVersion.toLong())));

            chunks.push_back(
                assertGet(ChunkType::fromShardBSON(shardChunk, _maxCollVersion.epoch())));
        }

        return chunks;
    }

    /**
     * Sets up persisted chunk metadata. Inserts four chunks and a collections entry for kNss.
     */
    std::vector<ChunkType> setUpShardChunkMetadata() {
        std::vector<ChunkType> fourChunks = makeFourChunks();
        setUpChunks(kChunkMetadataNss, fourChunks);
        setUpCollection();
        return fourChunks;
    }

    /**
     * Checks that 'nss' has no documents.
     */
    void checkCollectionIsEmpty(const NamespaceString& nss) {
        try {
            DBDirectClient client(operationContext());
            ASSERT_EQUALS(client.count(nss.ns()), 0ULL);
        } catch (const DBException& ex) {
            ASSERT(false);
        }
    }

    /**
     * Checks that each chunk in 'chunks' has been written to 'chunkMetadataNss'.
     */
    void checkChunks(const NamespaceString& chunkMetadataNss,
                     const std::vector<ChunkType>& chunks) {
        try {
            DBDirectClient client(operationContext());
            for (auto& chunk : chunks) {
                Query query(BSON(ChunkType::minShardID() << chunk.getMin() << ChunkType::max()
                                                         << chunk.getMax()));
                query.readPref(ReadPreference::Nearest, BSONArray());

                std::unique_ptr<DBClientCursor> cursor =
                    client.query(chunkMetadataNss.ns(), query, 1);
                ASSERT(cursor);

                ASSERT(cursor->more());
                BSONObj queryResult = cursor->nextSafe();
                ChunkType foundChunk =
                    assertGet(ChunkType::fromShardBSON(queryResult, chunk.getVersion().epoch()));
                ASSERT_BSONOBJ_EQ(chunk.getMin(), foundChunk.getMin());
                ASSERT_BSONOBJ_EQ(chunk.getMax(), foundChunk.getMax());
                ASSERT_EQUALS(chunk.getShard(), foundChunk.getShard());
                ASSERT_EQUALS(chunk.getVersion(), foundChunk.getVersion());
            }
        } catch (const DBException& ex) {
            ASSERT(false);
        }
    }

    const ChunkVersion& getCollectionVersion() const {
        return _maxCollVersion;
    }

    const BSONObj& getKeyPattern() const {
        return _keyPattern.toBSON();
    }

    const BSONObj& getDefaultCollation() const {
        return _defaultCollation;
    }

private:
    ChunkVersion _maxCollVersion{0, 0, OID::gen()};
    const KeyPattern _keyPattern{BSON("a" << 1)};
    const BSONObj _defaultCollation{BSON("locale"
                                         << "fr_CA")};
};

TEST_F(ShardMetadataUtilTest, UpdateAndReadCollectionsEntry) {
    ShardCollectionType updateShardCollectionType = setUpCollection();
    ShardCollectionType readShardCollectionType =
        assertGet(readShardCollectionsEntry(operationContext(), kNss));

    ASSERT_EQUALS(updateShardCollectionType.getUUID(), readShardCollectionType.getUUID());
    ASSERT_EQUALS(updateShardCollectionType.getNss(), readShardCollectionType.getNss());
    ASSERT_EQUALS(updateShardCollectionType.getEpoch(), readShardCollectionType.getEpoch());
    ASSERT_BSONOBJ_EQ(updateShardCollectionType.getKeyPattern().toBSON(),
                      readShardCollectionType.getKeyPattern().toBSON());
    ASSERT_BSONOBJ_EQ(updateShardCollectionType.getDefaultCollation(),
                      readShardCollectionType.getDefaultCollation());
    ASSERT_EQUALS(updateShardCollectionType.getUnique(), readShardCollectionType.getUnique());
    ASSERT_EQUALS(updateShardCollectionType.hasRefreshing(),
                  readShardCollectionType.hasRefreshing());

    // Refresh fields should not have been set.
    ASSERT(!updateShardCollectionType.hasLastRefreshedCollectionVersion());
    ASSERT(!readShardCollectionType.hasLastRefreshedCollectionVersion());
}

TEST_F(ShardMetadataUtilTest, PersistedRefreshSignalStartAndFinish) {
    setUpCollection();

    // Signal refresh start
    ASSERT_OK(setPersistedRefreshFlags(operationContext(), kNss));

    ShardCollectionType shardCollectionsEntry =
        assertGet(readShardCollectionsEntry(operationContext(), kNss));

    ASSERT_EQUALS(shardCollectionsEntry.getUUID(), kNss);
    ASSERT_EQUALS(shardCollectionsEntry.getNss(), kNss);
    ASSERT_EQUALS(shardCollectionsEntry.getEpoch(), getCollectionVersion().epoch());
    ASSERT_BSONOBJ_EQ(shardCollectionsEntry.getKeyPattern().toBSON(), getKeyPattern());
    ASSERT_BSONOBJ_EQ(shardCollectionsEntry.getDefaultCollation(), getDefaultCollation());
    ASSERT_EQUALS(shardCollectionsEntry.getUnique(), kUnique);
    ASSERT_EQUALS(shardCollectionsEntry.getRefreshing(), true);
    ASSERT(!shardCollectionsEntry.hasLastRefreshedCollectionVersion());

    // Signal refresh start again to make sure nothing changes
    ASSERT_OK(setPersistedRefreshFlags(operationContext(), kNss));

    RefreshState state = assertGet(getPersistedRefreshFlags(operationContext(), kNss));

    ASSERT_EQUALS(state.epoch, getCollectionVersion().epoch());
    ASSERT_EQUALS(state.refreshing, true);
    ASSERT_EQUALS(state.lastRefreshedCollectionVersion,
                  ChunkVersion(0, 0, getCollectionVersion().epoch()));

    // Signal refresh finish
    ASSERT_OK(unsetPersistedRefreshFlags(operationContext(), kNss, getCollectionVersion()));

    state = assertGet(getPersistedRefreshFlags(operationContext(), kNss));

    ASSERT_EQUALS(state.epoch, getCollectionVersion().epoch());
    ASSERT_EQUALS(state.refreshing, false);
    ASSERT_EQUALS(state.lastRefreshedCollectionVersion, getCollectionVersion());
}

TEST_F(ShardMetadataUtilTest, WriteAndReadChunks) {
    std::vector<ChunkType> chunks = makeFourChunks();
    ASSERT_OK(updateShardChunks(operationContext(), kNss, chunks, getCollectionVersion().epoch()));
    checkChunks(kChunkMetadataNss, chunks);

    // read all the chunks
    QueryAndSort allChunkDiff =
        createShardChunkDiffQuery(ChunkVersion(0, 0, getCollectionVersion().epoch()));
    std::vector<ChunkType> readChunks = assertGet(readShardChunks(operationContext(),
                                                                  kNss,
                                                                  allChunkDiff.query,
                                                                  allChunkDiff.sort,
                                                                  boost::none,
                                                                  getCollectionVersion().epoch()));
    for (auto chunkIt = chunks.begin(), readChunkIt = readChunks.begin();
         chunkIt != chunks.end() && readChunkIt != readChunks.end();
         ++chunkIt, ++readChunkIt) {
        ASSERT_BSONOBJ_EQ(chunkIt->toShardBSON(), readChunkIt->toShardBSON());
    }

    // read only the highest version chunk
    QueryAndSort oneChunkDiff = createShardChunkDiffQuery(getCollectionVersion());
    readChunks = assertGet(readShardChunks(operationContext(),
                                           kNss,
                                           oneChunkDiff.query,
                                           oneChunkDiff.sort,
                                           boost::none,
                                           getCollectionVersion().epoch()));

    ASSERT_TRUE(readChunks.size() == 1);
    ASSERT_BSONOBJ_EQ(chunks.back().toShardBSON(), readChunks.front().toShardBSON());
}

TEST_F(ShardMetadataUtilTest, UpdatingChunksFindsNewEpoch) {
    std::vector<ChunkType> chunks = makeFourChunks();
    ASSERT_OK(updateShardChunks(operationContext(), kNss, chunks, getCollectionVersion().epoch()));
    checkChunks(kChunkMetadataNss, chunks);

    ChunkVersion originalChunkVersion = chunks.back().getVersion();
    chunks.back().setVersion(ChunkVersion(1, 0, OID::gen()));
    ASSERT_EQUALS(
        updateShardChunks(operationContext(), kNss, chunks, getCollectionVersion().epoch()).code(),
        ErrorCodes::ConflictingOperationInProgress);

    // Check that the chunk with a different epoch did not get written.
    chunks.back().setVersion(std::move(originalChunkVersion));
    checkChunks(kChunkMetadataNss, chunks);
}

TEST_F(ShardMetadataUtilTest, UpdateWithWriteNewChunks) {
    // Load some chunk metadata.

    std::vector<ChunkType> chunks = makeFourChunks();
    ASSERT_OK(updateShardChunks(operationContext(), kNss, chunks, getCollectionVersion().epoch()));
    checkChunks(kChunkMetadataNss, chunks);

    // Load some changes and make sure it's applied correctly.
    // Split the last chunk in two and move the new last chunk away.

    std::vector<ChunkType> newChunks;
    ChunkType lastChunk = chunks.back();
    chunks.pop_back();
    ChunkVersion collVersion = getCollectionVersion();

    collVersion.incMinor();  // chunk only split
    BSONObjBuilder splitChunkOneBuilder;
    splitChunkOneBuilder.append(ChunkType::minShardID(), lastChunk.getMin());
    {
        BSONObjBuilder subMax(splitChunkOneBuilder.subobjStart(ChunkType::max()));
        subMax.append("a", 10000);
    }
    splitChunkOneBuilder.append(ChunkType::shard(), lastChunk.getShard().toString());
    collVersion.appendForChunk(&splitChunkOneBuilder);
    ChunkType splitChunkOne =
        assertGet(ChunkType::fromShardBSON(splitChunkOneBuilder.obj(), collVersion.epoch()));
    newChunks.push_back(splitChunkOne);

    collVersion.incMajor();  // chunk split and moved
    BSONObjBuilder splitChunkTwoMovedBuilder;
    {
        BSONObjBuilder subMin(splitChunkTwoMovedBuilder.subobjStart(ChunkType::minShardID()));
        subMin.append("a", 10000);
    }
    splitChunkTwoMovedBuilder.append(ChunkType::max(), lastChunk.getMax());
    splitChunkTwoMovedBuilder.append(ChunkType::shard(), "altShard");
    collVersion.appendForChunk(&splitChunkTwoMovedBuilder);
    ChunkType splitChunkTwoMoved =
        assertGet(ChunkType::fromShardBSON(splitChunkTwoMovedBuilder.obj(), collVersion.epoch()));
    newChunks.push_back(splitChunkTwoMoved);

    collVersion.incMinor();  // bump control chunk version
    ChunkType frontChunkControl = chunks.front();
    chunks.erase(chunks.begin());
    frontChunkControl.setVersion(collVersion);
    newChunks.push_back(frontChunkControl);

    ASSERT_OK(updateShardChunks(operationContext(), kNss, newChunks, collVersion.epoch()));

    chunks.push_back(splitChunkOne);
    chunks.push_back(splitChunkTwoMoved);
    chunks.push_back(frontChunkControl);
    checkChunks(kChunkMetadataNss, chunks);
}

TEST_F(ShardMetadataUtilTest, DropChunksAndDeleteCollectionsEntry) {
    setUpShardChunkMetadata();
    ASSERT_OK(dropChunksAndDeleteCollectionsEntry(operationContext(), kNss));
    checkCollectionIsEmpty(kChunkMetadataNss);
    // Collections collection should be empty because it only had one entry.
    checkCollectionIsEmpty(NamespaceString(ShardCollectionType::ConfigNS));
}

}  // namespace
}  // namespace mongo
