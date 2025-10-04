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

#include "mongo/db/global_catalog/ddl/shard_metadata_util.h"

#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_shard_collection.h"
#include "mongo/db/global_catalog/type_shard_collection_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <memory>
#include <utility>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

using namespace shardmetadatautil;

using std::string;
using std::unique_ptr;
using std::vector;
using unittest::assertGet;

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.foo");
const NamespaceString kChunkMetadataNss =
    NamespaceString::createNamespaceString_forTest("config.cache.chunks.test.foo");
const ShardId kShardId = ShardId("shard0");
const bool kUnique = false;

struct ShardMetadataUtilTest : public ShardServerTestFixture {
    /**
     * Inserts a collections collection entry for 'kNss'.
     */
    ShardCollectionType setUpCollection() {
        ShardCollectionType shardCollectionType(
            BSON(ShardCollectionType::kNssFieldName
                 << kNss.ns_forTest() << ShardCollectionType::kEpochFieldName
                 << maxCollPlacementVersion.epoch() << ShardCollectionType::kTimestampFieldName
                 << maxCollPlacementVersion.getTimestamp() << ShardCollectionType::kUuidFieldName
                 << uuid << ShardCollectionType::kKeyPatternFieldName << keyPattern.toBSON()
                 << ShardCollectionType::kDefaultCollationFieldName << defaultCollation
                 << ShardCollectionType::kUniqueFieldName << kUnique));
        shardCollectionType.setRefreshing(true);

        ASSERT_OK(updateShardCollectionsEntry(
            operationContext(),
            BSON(ShardCollectionType::kNssFieldName << kNss.ns_forTest()),
            shardCollectionType.toBSON(),
            true /*upsert*/));

        return shardCollectionType;
    }

    /**
     * Inserts 'chunks' into the shard's chunks collection.
     */
    void setUpChunks(const std::vector<ChunkType> chunks) {
        ASSERT_OK(
            updateShardChunks(operationContext(), kNss, chunks, maxCollPlacementVersion.epoch()));
    }

    /**
     * Helper to make four chunks that can then be manipulated in various ways in the tests.
     */
    std::vector<ChunkType> makeFourChunks() {
        std::vector<ChunkType> chunks;
        BSONObj mins[] = {BSON("a" << MINKEY), BSON("a" << 10), BSON("a" << 50), BSON("a" << 100)};
        BSONObj maxs[] = {BSON("a" << 10), BSON("a" << 50), BSON("a" << 100), BSON("a" << MAXKEY)};

        for (int i = 0; i < 4; ++i) {
            maxCollPlacementVersion.incMajor();
            BSONObj shardChunk =
                BSON(ChunkType::minShardID(mins[i])
                     << ChunkType::max(maxs[i]) << ChunkType::shard(kShardId.toString())
                     << ChunkType::lastmod(
                            Date_t::fromMillisSinceEpoch(maxCollPlacementVersion.toLong())));

            chunks.push_back(
                assertGet(ChunkType::parseFromShardBSON(shardChunk,
                                                        maxCollPlacementVersion.epoch(),
                                                        maxCollPlacementVersion.getTimestamp())));
        }

        return chunks;
    }

    /**
     * Sets up persisted chunk metadata. Inserts four chunks and a collections entry for kNss.
     */
    std::vector<ChunkType> setUpShardChunkMetadata() {
        std::vector<ChunkType> fourChunks = makeFourChunks();
        setUpChunks(fourChunks);
        setUpCollection();
        return fourChunks;
    }

    /**
     * Checks that 'nss' has no documents.
     */
    void checkCollectionIsEmpty(const NamespaceString& nss) {
        try {
            DBDirectClient client(operationContext());
            ASSERT_EQUALS(client.count(nss), 0ULL);
        } catch (const DBException&) {
            ASSERT(false);
        }
    }

    /**
     * Checks that each chunk in 'chunks' has been written to 'chunkMetadataNss'.
     */
    void checkChunks(const std::vector<ChunkType>& chunks) {
        try {
            DBDirectClient client(operationContext());
            for (auto& chunk : chunks) {
                FindCommandRequest findRequest{kChunkMetadataNss};
                findRequest.setFilter(BSON(ChunkType::minShardID()
                                           << chunk.getMin() << ChunkType::max()
                                           << chunk.getMax()));
                findRequest.setLimit(1);
                std::unique_ptr<DBClientCursor> cursor = client.find(
                    std::move(findRequest), ReadPreferenceSetting{ReadPreference::Nearest});
                ASSERT(cursor);

                ASSERT(cursor->more());
                BSONObj queryResult = cursor->nextSafe();
                ChunkType foundChunk = assertGet(ChunkType::parseFromShardBSON(
                    queryResult, chunk.getVersion().epoch(), chunk.getVersion().getTimestamp()));
                ASSERT_BSONOBJ_EQ(chunk.getMin(), foundChunk.getMin());
                ASSERT_BSONOBJ_EQ(chunk.getMax(), foundChunk.getMax());
                ASSERT_EQUALS(chunk.getShard(), foundChunk.getShard());
                ASSERT_EQUALS(chunk.getVersion(), foundChunk.getVersion());
            }
        } catch (const DBException&) {
            ASSERT(false);
        }
    }

    ChunkVersion maxCollPlacementVersion{{OID::gen(), Timestamp(1, 1)}, {0, 0}};
    const KeyPattern keyPattern{BSON("a" << 1)};
    const BSONObj defaultCollation{BSON("locale" << "fr_CA")};
    const UUID uuid = UUID::gen();
};

TEST_F(ShardMetadataUtilTest, UpdateAndReadCollectionsEntry) {
    ShardCollectionType updateShardCollectionType = setUpCollection();
    ShardCollectionType readShardCollectionType =
        assertGet(readShardCollectionsEntry(operationContext(), kNss));

    ASSERT_EQUALS(updateShardCollectionType.getUuid(), readShardCollectionType.getUuid());
    ASSERT_EQUALS(updateShardCollectionType.getNss(), readShardCollectionType.getNss());
    ASSERT_EQUALS(updateShardCollectionType.getEpoch(), readShardCollectionType.getEpoch());
    ASSERT_EQUALS(updateShardCollectionType.getTimestamp(), readShardCollectionType.getTimestamp());
    ASSERT_BSONOBJ_EQ(updateShardCollectionType.getKeyPattern().toBSON(),
                      readShardCollectionType.getKeyPattern().toBSON());
    ASSERT_BSONOBJ_EQ(updateShardCollectionType.getDefaultCollation(),
                      readShardCollectionType.getDefaultCollation());
    ASSERT_EQUALS(updateShardCollectionType.getUnique(), readShardCollectionType.getUnique());
    ASSERT_EQUALS(*updateShardCollectionType.getRefreshing(),
                  *readShardCollectionType.getRefreshing());

    // Refresh fields should not have been set.
    ASSERT(!updateShardCollectionType.getLastRefreshedCollectionPlacementVersion());
    ASSERT(!readShardCollectionType.getLastRefreshedCollectionPlacementVersion());
}

TEST_F(ShardMetadataUtilTest, PersistedRefreshSignalStartAndFinish) {
    setUpCollection();

    ShardCollectionType shardCollectionsEntry =
        assertGet(readShardCollectionsEntry(operationContext(), kNss));

    ASSERT_EQUALS(shardCollectionsEntry.getUuid(), uuid);
    ASSERT_EQUALS(shardCollectionsEntry.getNss().ns_forTest(), kNss.ns_forTest());
    ASSERT_EQUALS(shardCollectionsEntry.getEpoch(), maxCollPlacementVersion.epoch());
    ASSERT_EQUALS(shardCollectionsEntry.getTimestamp(), maxCollPlacementVersion.getTimestamp());
    ASSERT_BSONOBJ_EQ(shardCollectionsEntry.getKeyPattern().toBSON(), keyPattern.toBSON());
    ASSERT_BSONOBJ_EQ(shardCollectionsEntry.getDefaultCollation(), defaultCollation);
    ASSERT_EQUALS(shardCollectionsEntry.getUnique(), kUnique);
    ASSERT_EQUALS(*shardCollectionsEntry.getRefreshing(), true);
    ASSERT(!shardCollectionsEntry.getLastRefreshedCollectionPlacementVersion());

    // Signal refresh start again to make sure nothing changes
    ASSERT_OK(updateShardCollectionsEntry(
        operationContext(),
        BSON(ShardCollectionType::kNssFieldName << kNss.ns_forTest()),
        BSON("$set" << BSON(ShardCollectionType::kRefreshingFieldName << true)),
        false));

    RefreshState state = assertGet(getPersistedRefreshFlags(operationContext(), kNss));

    ASSERT(state.generation.isSameCollection(maxCollPlacementVersion));
    ASSERT_EQUALS(state.refreshing, true);
    ASSERT_EQUALS(
        state.lastRefreshedCollectionPlacementVersion,
        ChunkVersion({maxCollPlacementVersion.epoch(), maxCollPlacementVersion.getTimestamp()},
                     {0, 0}));

    // Signal refresh finish
    ASSERT_OK(unsetPersistedRefreshFlags(operationContext(), kNss, maxCollPlacementVersion));

    state = assertGet(getPersistedRefreshFlags(operationContext(), kNss));

    ASSERT(state.generation.isSameCollection(maxCollPlacementVersion));
    ASSERT_EQUALS(state.refreshing, false);
    ASSERT_EQUALS(state.lastRefreshedCollectionPlacementVersion, maxCollPlacementVersion);
}

TEST_F(ShardMetadataUtilTest, WriteAndReadChunks) {
    std::vector<ChunkType> chunks = makeFourChunks();
    ASSERT_OK(updateShardChunks(operationContext(), kNss, chunks, maxCollPlacementVersion.epoch()));
    checkChunks(chunks);

    // read all the chunks
    QueryAndSort allChunkDiff = createShardChunkDiffQuery(ChunkVersion(
        {maxCollPlacementVersion.epoch(), maxCollPlacementVersion.getTimestamp()}, {0, 0}));
    std::vector<ChunkType> readChunks =
        assertGet(readShardChunks(operationContext(),
                                  kNss,
                                  allChunkDiff.query,
                                  allChunkDiff.sort,
                                  boost::none,
                                  maxCollPlacementVersion.epoch(),
                                  maxCollPlacementVersion.getTimestamp()));
    for (auto chunkIt = chunks.begin(), readChunkIt = readChunks.begin();
         chunkIt != chunks.end() && readChunkIt != readChunks.end();
         ++chunkIt, ++readChunkIt) {
        ASSERT_BSONOBJ_EQ(chunkIt->toShardBSON(), readChunkIt->toShardBSON());
    }

    // read only the highest version chunk
    QueryAndSort oneChunkDiff = createShardChunkDiffQuery(maxCollPlacementVersion);
    readChunks = assertGet(readShardChunks(operationContext(),
                                           kNss,
                                           oneChunkDiff.query,
                                           oneChunkDiff.sort,
                                           boost::none,
                                           maxCollPlacementVersion.epoch(),
                                           maxCollPlacementVersion.getTimestamp()));

    ASSERT(readChunks.size() == 1);
    ASSERT_BSONOBJ_EQ(chunks.back().toShardBSON(), readChunks.front().toShardBSON());
}

TEST_F(ShardMetadataUtilTest, UpdateWithWriteNewChunks) {
    // Load some chunk metadata.

    std::vector<ChunkType> chunks = makeFourChunks();
    ASSERT_OK(updateShardChunks(operationContext(), kNss, chunks, maxCollPlacementVersion.epoch()));
    checkChunks(chunks);

    // Load some changes and make sure it's applied correctly.
    // Split the last chunk in two and move the new last chunk away.

    std::vector<ChunkType> newChunks;
    ChunkType lastChunk = chunks.back();
    chunks.pop_back();
    ChunkVersion collPlacementVersion = maxCollPlacementVersion;

    collPlacementVersion.incMinor();  // chunk only split
    BSONObjBuilder splitChunkOneBuilder;
    splitChunkOneBuilder.append(ChunkType::minShardID(), lastChunk.getMin());
    {
        BSONObjBuilder subMax(splitChunkOneBuilder.subobjStart(ChunkType::max()));
        subMax.append("a", 10000);
    }
    splitChunkOneBuilder.append(ChunkType::shard(), lastChunk.getShard().toString());
    splitChunkOneBuilder.appendTimestamp(ChunkType::lastmod(), collPlacementVersion.toLong());
    ChunkType splitChunkOne =
        assertGet(ChunkType::parseFromShardBSON(splitChunkOneBuilder.obj(),
                                                collPlacementVersion.epoch(),
                                                collPlacementVersion.getTimestamp()));
    newChunks.push_back(splitChunkOne);

    collPlacementVersion.incMajor();  // chunk split and moved

    BSONObjBuilder splitChunkTwoMovedBuilder;
    {
        BSONObjBuilder subMin(splitChunkTwoMovedBuilder.subobjStart(ChunkType::minShardID()));
        subMin.append("a", 10000);
    }
    splitChunkTwoMovedBuilder.append(ChunkType::max(), lastChunk.getMax());
    splitChunkTwoMovedBuilder.append(ChunkType::shard(), "altShard");
    splitChunkTwoMovedBuilder.appendTimestamp(ChunkType::lastmod(), collPlacementVersion.toLong());
    ChunkType splitChunkTwoMoved =
        assertGet(ChunkType::parseFromShardBSON(splitChunkTwoMovedBuilder.obj(),
                                                collPlacementVersion.epoch(),
                                                collPlacementVersion.getTimestamp()));
    newChunks.push_back(splitChunkTwoMoved);

    collPlacementVersion.incMinor();  // bump control chunk version
    ChunkType frontChunkControl = chunks.front();
    chunks.erase(chunks.begin());
    frontChunkControl.setVersion(collPlacementVersion);
    newChunks.push_back(frontChunkControl);

    ASSERT_OK(updateShardChunks(operationContext(), kNss, newChunks, collPlacementVersion.epoch()));

    chunks.push_back(splitChunkOne);
    chunks.push_back(splitChunkTwoMoved);
    chunks.push_back(frontChunkControl);
    checkChunks(chunks);
}

TEST_F(ShardMetadataUtilTest, DropChunksAndDeleteCollectionsEntry) {
    setUpShardChunkMetadata();
    ASSERT_OK(dropChunksAndDeleteCollectionsEntry(operationContext(), kNss));
    checkCollectionIsEmpty(kChunkMetadataNss);
    // Collections collection should be empty because it only had one entry.
    checkCollectionIsEmpty(NamespaceString::kShardConfigCollectionsNamespace);
}

}  // namespace
}  // namespace mongo
