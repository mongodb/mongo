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

#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard_collection.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::string;
using std::unique_ptr;
using std::vector;
using unittest::assertGet;

const HostAndPort kConfigHostAndPort = HostAndPort("dummy", 123);
const NamespaceString kNss = NamespaceString("test.foo");
const NamespaceString kChunkMetadataNss = NamespaceString("config.chunks.test.foo");
const ShardId kShardId = ShardId("shard0");

class ShardMetadataUtilTest : public ShardServerTestFixture {
protected:
    /**
     * Call this after setUpChunks()! Both functions use the private _maxCollVersion variable. To
     * set the lastConsistentCollectionVersion correctly this must be called second.
     *
     * Inserts a config.collections entry.
     */
    void setUpCollection() {
        BSONObjBuilder builder;
        builder.append(ShardCollectionType::uuid(), kNss.ns());
        builder.append(ShardCollectionType::ns(), kNss.ns());
        builder.append(ShardCollectionType::keyPattern(), _keyPattern.toBSON());
        _maxCollVersion.appendWithFieldForCommands(
            &builder, ShardCollectionType::lastConsistentCollectionVersion());
        ShardCollectionType shardCollType = assertGet(ShardCollectionType::fromBSON(builder.obj()));

        try {
            DBDirectClient client(operationContext());
            auto insert(stdx::make_unique<BatchedInsertRequest>());
            insert->addToDocuments(shardCollType.toBSON());

            BatchedCommandRequest insertRequest(insert.release());
            insertRequest.setNS(NamespaceString(ShardCollectionType::ConfigNS));
            const BSONObj insertCmdObj = insertRequest.toBSON();

            rpc::UniqueReply commandResponse =
                client.runCommandWithMetadata("config",
                                              insertCmdObj.firstElementFieldName(),
                                              rpc::makeEmptyMetadata(),
                                              insertCmdObj);
            ASSERT_OK(getStatusFromCommandResult(commandResponse->getCommandReply()));
        } catch (const DBException& ex) {
            ASSERT(false);
        }
    }

    /**
     * Helper to make a number of chunks that can then be manipulated in various ways in the tests.
     * Chunks have the shard server's config.chunks.ns schema.
     */
    std::vector<ChunkType> makeFourChunks() {
        std::vector<ChunkType> chunks;
        BSONObj mins[] = {BSON("a" << MINKEY), BSON("a" << 10), BSON("a" << 50), BSON("a" << 100)};
        BSONObj maxs[] = {BSON("a" << 10), BSON("a" << 50), BSON("a" << 100), BSON("a" << MAXKEY)};

        for (int i = 0; i < 4; ++i) {
            _maxCollVersion.incMajor();
            BSONObj shardChunk = BSON(ChunkType::minShardID(mins[i])
                                      << ChunkType::max(maxs[i])
                                      << ChunkType::shard(kShardId.toString())
                                      << ChunkType::DEPRECATED_lastmod(Date_t::fromMillisSinceEpoch(
                                             _maxCollVersion.toLong())));

            chunks.push_back(
                assertGet(ChunkType::fromShardBSON(shardChunk, _maxCollVersion.epoch())));
        }

        return chunks;
    }

    /**
     * Inserts 'chunks' into the config.chunks.ns collection 'chunkMetadataNss'.
     */
    void setUpChunks(const NamespaceString& chunkMetadataNss, const std::vector<ChunkType> chunks) {
        try {
            DBDirectClient client(operationContext());
            auto insert(stdx::make_unique<BatchedInsertRequest>());

            for (auto& chunk : chunks) {
                insert->addToDocuments(chunk.toShardBSON());
            }

            BatchedCommandRequest insertRequest(insert.release());
            insertRequest.setNS(NamespaceString(chunkMetadataNss));
            const BSONObj insertCmdObj = insertRequest.toBSON();

            rpc::UniqueReply commandResponse =
                client.runCommandWithMetadata(chunkMetadataNss.db().toString(),
                                              insertCmdObj.firstElementFieldName(),
                                              rpc::makeEmptyMetadata(),
                                              insertCmdObj);
            ASSERT_OK(getStatusFromCommandResult(commandResponse->getCommandReply()));
        } catch (const DBException& ex) {
            ASSERT(false);
        }
    }

    /**
     * Sets up persisted chunk metadata. Inserts four chunks for kNss into kChunkMetadataNss and a
     * corresponding collection entry in config.collections.
     */
    void setUpShardingMetadata() {
        setUpChunks(kChunkMetadataNss, makeFourChunks());
        setUpCollection();
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

    const KeyPattern& getKeyPattern() const {
        return _keyPattern;
    }

private:
    ChunkVersion _maxCollVersion{0, 0, OID::gen()};
    const KeyPattern _keyPattern{BSON("a" << 1)};
};

TEST_F(ShardMetadataUtilTest, UpdateAndReadCollectionsDocument) {
    // Insert document
    BSONObj shardCollectionTypeObj =
        BSON(ShardCollectionType::uuid(kNss.ns())
             << ShardCollectionType::ns(kNss.ns())
             << ShardCollectionType::keyPattern(getKeyPattern().toBSON()));
    ASSERT_OK(
        shardmetadatautil::updateShardCollectionEntry(operationContext(),
                                                      kNss,
                                                      BSON(ShardCollectionType::uuid(kNss.ns())),
                                                      shardCollectionTypeObj));

    ShardCollectionType readShardCollectionType =
        assertGet(shardmetadatautil::readShardCollectionEntry(operationContext(), kNss));
    ASSERT_BSONOBJ_EQ(shardCollectionTypeObj, readShardCollectionType.toBSON());

    // Update document
    BSONObjBuilder updateBuilder;
    getCollectionVersion().appendWithFieldForCommands(
        &updateBuilder, ShardCollectionType::lastConsistentCollectionVersion());
    ASSERT_OK(shardmetadatautil::updateShardCollectionEntry(
        operationContext(), kNss, BSON(ShardCollectionType::uuid(kNss.ns())), updateBuilder.obj()));

    readShardCollectionType =
        assertGet(shardmetadatautil::readShardCollectionEntry(operationContext(), kNss));

    ShardCollectionType updatedShardCollectionType =
        assertGet(ShardCollectionType::fromBSON(shardCollectionTypeObj));
    updatedShardCollectionType.setLastConsistentCollectionVersion(getCollectionVersion());

    ASSERT_BSONOBJ_EQ(updatedShardCollectionType.toBSON(), readShardCollectionType.toBSON());
}

TEST_F(ShardMetadataUtilTest, WriteNewChunks) {
    std::vector<ChunkType> chunks = makeFourChunks();
    shardmetadatautil::writeNewChunks(
        operationContext(), kNss, chunks, getCollectionVersion().epoch());
    checkChunks(kChunkMetadataNss, chunks);
}

TEST_F(ShardMetadataUtilTest, WriteAndReadChunks) {
    setUpCollection();

    std::vector<ChunkType> chunks = makeFourChunks();
    shardmetadatautil::writeNewChunks(
        operationContext(), kNss, chunks, getCollectionVersion().epoch());
    checkChunks(kChunkMetadataNss, chunks);

    // read all the chunks
    std::vector<ChunkType> readChunks = assertGet(shardmetadatautil::readShardChunks(
        operationContext(), kNss, ChunkVersion(0, 0, getCollectionVersion().epoch())));
    for (auto chunkIt = chunks.begin(), readChunkIt = readChunks.begin();
         chunkIt != chunks.end() && readChunkIt != readChunks.end();
         ++chunkIt, ++readChunkIt) {
        ASSERT_BSONOBJ_EQ(chunkIt->toShardBSON(), readChunkIt->toShardBSON());
    }

    // read only the highest version chunk
    readChunks = assertGet(
        shardmetadatautil::readShardChunks(operationContext(), kNss, getCollectionVersion()));

    ASSERT_TRUE(readChunks.size() == 1);
    ASSERT_BSONOBJ_EQ(chunks.back().toShardBSON(), readChunks.front().toShardBSON());
}

TEST_F(ShardMetadataUtilTest, UpdateWithWriteNewChunks) {
    // Load some chunk metadata.

    std::vector<ChunkType> chunks = makeFourChunks();
    ASSERT_OK(shardmetadatautil::writeNewChunks(
        operationContext(), kNss, chunks, getCollectionVersion().epoch()));
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

    ASSERT_OK(shardmetadatautil::writeNewChunks(
        operationContext(), kNss, newChunks, collVersion.epoch()));

    chunks.push_back(splitChunkOne);
    chunks.push_back(splitChunkTwoMoved);
    chunks.push_back(frontChunkControl);
    checkChunks(kChunkMetadataNss, chunks);
}

TEST_F(ShardMetadataUtilTest, DropChunksAndDeleteCollectionsEntry) {
    setUpShardingMetadata();
    ASSERT_OK(shardmetadatautil::dropChunksAndDeleteCollectionsEntry(operationContext(), kNss));
    checkCollectionIsEmpty(NamespaceString(ShardCollectionType::ConfigNS));
    checkCollectionIsEmpty(kChunkMetadataNss);
}

}  // namespace
}  // namespace mongo
