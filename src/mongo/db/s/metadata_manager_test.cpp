/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/s/metadata_manager.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

using unittest::assertGet;

class MetadataManagerTest : public ServiceContextMongoDTest {
protected:
    static std::unique_ptr<CollectionMetadata> makeEmptyMetadata() {
        const OID epoch = OID::gen();

        return stdx::make_unique<CollectionMetadata>(
            BSON("key" << 1),
            ChunkVersion(1, 0, epoch),
            ChunkVersion(0, 0, epoch),
            SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<CachedChunkInfo>());
    }

    /**
     * Returns a new metadata's instance based on the current state by adding a chunk with the
     * specified bounds and version. The chunk's version must be higher than that of all chunks
     * which are in the input metadata.
     *
     * It will fassert if the chunk bounds are incorrect or overlap an existing chunk or if the
     * chunk version is lower than the maximum one.
     */
    static std::unique_ptr<CollectionMetadata> cloneMetadataPlusChunk(
        const CollectionMetadata& metadata,
        const BSONObj& minKey,
        const BSONObj& maxKey,
        const ChunkVersion& chunkVersion) {
        invariant(chunkVersion.epoch() == metadata.getShardVersion().epoch());
        invariant(chunkVersion.isSet());
        invariant(chunkVersion > metadata.getCollVersion());
        invariant(minKey.woCompare(maxKey) < 0);
        invariant(!rangeMapOverlaps(metadata.getChunks(), minKey, maxKey));

        auto chunksMap = metadata.getChunks();
        chunksMap.insert(
            std::make_pair(minKey.getOwned(), CachedChunkInfo(maxKey.getOwned(), chunkVersion)));

        return stdx::make_unique<CollectionMetadata>(
            metadata.getKeyPattern(), chunkVersion, chunkVersion, std::move(chunksMap));
    }

    std::shared_ptr<MetadataManager> manager_ptr{std::make_shared<MetadataManager>(
        getServiceContext(), NamespaceString("TestDb", "CollDB"))};
    MetadataManager& manager{*this->manager_ptr};
};

TEST_F(MetadataManagerTest, SetAndGetActiveMetadata) {
    std::unique_ptr<CollectionMetadata> cm = makeEmptyMetadata();
    auto cmPtr = cm.get();

    manager.refreshActiveMetadata(std::move(cm));
    ScopedCollectionMetadata scopedMetadata = manager.getActiveMetadata(manager_ptr);

    ASSERT_EQ(cmPtr, scopedMetadata.getMetadata());
}

TEST_F(MetadataManagerTest, ResetActiveMetadata) {
    manager.refreshActiveMetadata(makeEmptyMetadata());

    ScopedCollectionMetadata scopedMetadata1 = manager.getActiveMetadata(manager_ptr);

    ChunkVersion newVersion = scopedMetadata1->getCollVersion();
    newVersion.incMajor();
    std::unique_ptr<CollectionMetadata> cm2 = cloneMetadataPlusChunk(
        *scopedMetadata1.getMetadata(), BSON("key" << 0), BSON("key" << 10), newVersion);
    auto cm2Ptr = cm2.get();

    manager.refreshActiveMetadata(std::move(cm2));
    ScopedCollectionMetadata scopedMetadata2 = manager.getActiveMetadata(manager_ptr);

    ASSERT_EQ(cm2Ptr, scopedMetadata2.getMetadata());
}

TEST_F(MetadataManagerTest, RefreshAfterSuccessfulMigrationSinglePending) {
    manager.refreshActiveMetadata(makeEmptyMetadata());

    const ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    manager.beginReceive(cr1);
    ASSERT_EQ(manager.getCopyOfReceivingChunks().size(), 1UL);
    ASSERT_EQ(manager.getActiveMetadata(manager_ptr)->getChunks().size(), 0UL);

    ChunkVersion version = manager.getActiveMetadata(manager_ptr)->getCollVersion();
    version.incMajor();

    manager.refreshActiveMetadata(
        cloneMetadataPlusChunk(*manager.getActiveMetadata(manager_ptr).getMetadata(),
                               cr1.getMin(),
                               cr1.getMax(),
                               version));
    ASSERT_EQ(manager.getCopyOfReceivingChunks().size(), 0UL);
    ASSERT_EQ(manager.getActiveMetadata(manager_ptr)->getChunks().size(), 1UL);
}

TEST_F(MetadataManagerTest, RefreshAfterSuccessfulMigrationMultiplePending) {
    manager.refreshActiveMetadata(makeEmptyMetadata());

    const ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    manager.beginReceive(cr1);

    const ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));
    manager.beginReceive(cr2);

    ASSERT_EQ(manager.getCopyOfReceivingChunks().size(), 2UL);
    ASSERT_EQ(manager.getActiveMetadata(manager_ptr)->getChunks().size(), 0UL);

    {
        ChunkVersion version = manager.getActiveMetadata(manager_ptr)->getCollVersion();
        version.incMajor();

        manager.refreshActiveMetadata(
            cloneMetadataPlusChunk(*manager.getActiveMetadata(manager_ptr).getMetadata(),
                                   cr1.getMin(),
                                   cr1.getMax(),
                                   version));
        ASSERT_EQ(manager.getCopyOfReceivingChunks().size(), 1UL);
        ASSERT_EQ(manager.getActiveMetadata(manager_ptr)->getChunks().size(), 1UL);
    }

    {
        ChunkVersion version = manager.getActiveMetadata(manager_ptr)->getCollVersion();
        version.incMajor();

        manager.refreshActiveMetadata(
            cloneMetadataPlusChunk(*manager.getActiveMetadata(manager_ptr).getMetadata(),
                                   cr2.getMin(),
                                   cr2.getMax(),
                                   version));
        ASSERT_EQ(manager.getCopyOfReceivingChunks().size(), 0UL);
        ASSERT_EQ(manager.getActiveMetadata(manager_ptr)->getChunks().size(), 2UL);
    }
}

TEST_F(MetadataManagerTest, RefreshAfterNotYetCompletedMigrationMultiplePending) {
    manager.refreshActiveMetadata(makeEmptyMetadata());

    const ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    manager.beginReceive(cr1);

    const ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));
    manager.beginReceive(cr2);

    ASSERT_EQ(manager.getCopyOfReceivingChunks().size(), 2UL);
    ASSERT_EQ(manager.getActiveMetadata(manager_ptr)->getChunks().size(), 0UL);

    ChunkVersion version = manager.getActiveMetadata(manager_ptr)->getCollVersion();
    version.incMajor();

    manager.refreshActiveMetadata(
        cloneMetadataPlusChunk(*manager.getActiveMetadata(manager_ptr).getMetadata(),
                               BSON("key" << 50),
                               BSON("key" << 60),
                               version));
    ASSERT_EQ(manager.getCopyOfReceivingChunks().size(), 2UL);
    ASSERT_EQ(manager.getActiveMetadata(manager_ptr)->getChunks().size(), 1UL);
}

TEST_F(MetadataManagerTest, BeginReceiveWithOverlappingRange) {
    manager.refreshActiveMetadata(makeEmptyMetadata());

    const ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    manager.beginReceive(cr1);

    const ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));
    manager.beginReceive(cr2);

    const ChunkRange crOverlap(BSON("key" << 5), BSON("key" << 35));
    manager.beginReceive(crOverlap);

    const auto copyOfPending = manager.getCopyOfReceivingChunks();

    ASSERT_EQ(copyOfPending.size(), 1UL);
    ASSERT_EQ(manager.getActiveMetadata(manager_ptr)->getChunks().size(), 0UL);

    const auto it = copyOfPending.find(BSON("key" << 5));
    ASSERT(it != copyOfPending.end());
    ASSERT_BSONOBJ_EQ(it->second.getMaxKey(), BSON("key" << 35));
}

TEST_F(MetadataManagerTest, RefreshMetadataAfterDropAndRecreate) {
    manager.refreshActiveMetadata(makeEmptyMetadata());

    {
        auto metadata = manager.getActiveMetadata(manager_ptr);
        ChunkVersion newVersion = metadata->getCollVersion();
        newVersion.incMajor();

        manager.refreshActiveMetadata(cloneMetadataPlusChunk(
            *metadata.getMetadata(), BSON("key" << 0), BSON("key" << 10), newVersion));
    }

    // Now, pretend that the collection was dropped and recreated
    auto recreateMetadata = makeEmptyMetadata();
    ChunkVersion newVersion = recreateMetadata->getCollVersion();
    newVersion.incMajor();

    manager.refreshActiveMetadata(cloneMetadataPlusChunk(
        *recreateMetadata, BSON("key" << 20), BSON("key" << 30), newVersion));
    ASSERT_EQ(manager.getActiveMetadata(manager_ptr)->getChunks().size(), 1UL);

    const auto chunkEntry = manager.getActiveMetadata(manager_ptr)->getChunks().begin();
    ASSERT_BSONOBJ_EQ(BSON("key" << 20), chunkEntry->first);
    ASSERT_BSONOBJ_EQ(BSON("key" << 30), chunkEntry->second.getMaxKey());
    ASSERT_EQ(newVersion, chunkEntry->second.getVersion());
}

}  // namespace
}  // namespace mongo
