// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/metadata_manager.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/chunk.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/sharding_environment/range_arithmetic.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/platform/atomic.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {


const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const std::string kPattern = "key";
const KeyPattern kShardKeyPattern(BSON(kPattern << 1));
// TODO SERVER-127411: use ShardRef instead of ShardId when all consumers have been migrated to
// ShardRef
const ShardId kThisShard{"thisShard"};
const ShardId kOtherShard{"otherShard"};

class MetadataManagerTest : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();
        _manager =
            std::make_shared<MetadataManager>(getServiceContext(), kNss, makeEmptyMetadata());
        orphanCleanupDelaySecs.store(1);
    }

    void tearDown() override {
        // Restore original `orphanCleanupDelaySecs` value for next unit tests
        orphanCleanupDelaySecs.store(_defaultOrphanCleanupDelaySecs);
        ShardServerTestFixture::tearDown();
    }

    /**
     * Returns an instance of CollectionMetadata which has no chunks owned by 'thisShard'.
     */
    static CollectionMetadata makeEmptyMetadata(
        const KeyPattern& shardKeyPattern = kShardKeyPattern,
        const ChunkRange& range = ChunkRange{BSON(kPattern << MINKEY), BSON(kPattern << MAXKEY)},
        UUID uuid = UUID::gen()) {
        const OID epoch = OID::gen();

        auto rt = RoutingTableHistory::makeNew(
            kNss,
            uuid,
            shardKeyPattern,
            false, /* unsplittable */
            nullptr,
            false,
            epoch,
            Timestamp(1, 1),
            boost::none /* timeseriesFields */,
            boost::none /* reshardingFields */,

            true,
            {ChunkType{uuid, range, ChunkVersion({epoch, Timestamp(1, 1)}, {1, 0}), kOtherShard}});

        return CollectionMetadata(
            CurrentChunkManager(makeStandaloneRoutingTableHistory(std::move(rt))), kThisShard);
    }

    /**
     * Returns a new metadata's instance based on the current state by adding a chunk with the
     * specified bounds and version. The chunk's version must be higher than that of all chunks
     * which are in the input metadata.
     *
     * It will fassert if the chunk bounds are incorrect or overlap an existing chunk or if the
     * chunk version is lower than the maximum one.
     */
    static CollectionMetadata cloneMetadataPlusChunk(const CollectionMetadata& collMetadata,
                                                     const ChunkRange& range) {
        const BSONObj& minKey = range.getMin();
        const BSONObj& maxKey = range.getMax();
        ASSERT(!rangeMapOverlaps(collMetadata.getChunks(), minKey, maxKey));

        auto cm = collMetadata.getChunkManager();

        const auto chunkToSplit = cm->findIntersectingChunkWithSimpleCollation(minKey);
        ASSERT_BSONOBJ_GTE(minKey, chunkToSplit.getMin());
        ASSERT_BSONOBJ_LT(maxKey, chunkToSplit.getMax());

        std::vector<ChunkType> splitChunks;

        auto chunkVersion = cm->getVersion();

        if (SimpleBSONObjComparator::kInstance.evaluate(chunkToSplit.getMin() < minKey)) {
            chunkVersion.incMajor();
            splitChunks.emplace_back(collMetadata.getUUID(),
                                     ChunkRange(chunkToSplit.getMin(), minKey),
                                     chunkVersion,
                                     kOtherShard);
        }

        chunkVersion.incMajor();
        splitChunks.emplace_back(
            collMetadata.getUUID(), ChunkRange(minKey, maxKey), chunkVersion, kThisShard);

        chunkVersion.incMajor();
        splitChunks.emplace_back(collMetadata.getUUID(),
                                 ChunkRange(maxKey, chunkToSplit.getMax()),
                                 chunkVersion,
                                 kOtherShard);

        auto rt =
            cm->getRoutingTableHistory_forTest().makeUpdated(boost::none /* timeseriesFields */,
                                                             boost::none /* reshardingFields */,
                                                             true,
                                                             false, /* unsplittable */
                                                             splitChunks);

        return CollectionMetadata(
            CurrentChunkManager(makeStandaloneRoutingTableHistory(std::move(rt))), kThisShard);
    }

    static CollectionMetadata cloneMetadataMinusChunk(const CollectionMetadata& metadata,
                                                      const ChunkRange& range) {
        const BSONObj& minKey = range.getMin();
        const BSONObj& maxKey = range.getMax();
        ASSERT(rangeMapOverlaps(metadata.getChunks(), minKey, maxKey));

        auto cm = metadata.getChunkManager();

        const auto chunkToMoveOut = cm->findIntersectingChunkWithSimpleCollation(minKey);
        ASSERT_BSONOBJ_EQ(minKey, chunkToMoveOut.getMin());
        ASSERT_BSONOBJ_EQ(maxKey, chunkToMoveOut.getMax());

        auto chunkVersion = cm->getVersion();
        chunkVersion.incMajor();

        auto rt = cm->getRoutingTableHistory_forTest().makeUpdated(
            boost::none /* timeseriesFields */,
            boost::none /* reshardingFields */,
            true,
            false, /* unsplittable */
            {ChunkType(metadata.getUUID(), ChunkRange(minKey, maxKey), chunkVersion, kOtherShard)});

        return CollectionMetadata(
            CurrentChunkManager(makeStandaloneRoutingTableHistory(std::move(rt))), kThisShard);
    }

    std::shared_ptr<MetadataManager> _manager;

private:
    const int _defaultOrphanCleanupDelaySecs = orphanCleanupDelaySecs.load();
};

TEST_F(MetadataManagerTest, RefreshAfterSuccessfulMigrationSinglePending) {
    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));

    _manager->setCollectionMetadata(
        cloneMetadataPlusChunk(_manager->getActiveMetadata(boost::none, false)->get(), cr1));
    ASSERT_EQ(_manager->getActiveMetadata(boost::none, false)->get().getChunks().size(), 1UL);
}

TEST_F(MetadataManagerTest, RefreshAfterSuccessfulMigrationMultiplePending) {
    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));

    {
        _manager->setCollectionMetadata(
            cloneMetadataPlusChunk(_manager->getActiveMetadata(boost::none, false)->get(), cr1));
        ASSERT_EQ(_manager->getActiveMetadata(boost::none, false)->get().getChunks().size(), 1UL);
    }

    {
        _manager->setCollectionMetadata(
            cloneMetadataPlusChunk(_manager->getActiveMetadata(boost::none, false)->get(), cr2));
        ASSERT_EQ(_manager->getActiveMetadata(boost::none, false)->get().getChunks().size(), 2UL);
    }
}

TEST_F(MetadataManagerTest, RefreshAfterNotYetCompletedMigrationMultiplePending) {
    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));

    _manager->setCollectionMetadata(
        cloneMetadataPlusChunk(_manager->getActiveMetadata(boost::none, false)->get(),
                               {BSON("key" << 50), BSON("key" << 60)}));
    ASSERT_EQ(_manager->getActiveMetadata(boost::none, false)->get().getChunks().size(), 1UL);
}

TEST_F(MetadataManagerTest, BeginReceiveWithOverlappingRange) {
    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));

    _manager->setCollectionMetadata(
        cloneMetadataPlusChunk(_manager->getActiveMetadata(boost::none, false)->get(), cr1));
    _manager->setCollectionMetadata(
        cloneMetadataPlusChunk(_manager->getActiveMetadata(boost::none, false)->get(), cr2));

    ChunkRange crOverlap(BSON("key" << 5), BSON("key" << 35));
}

TEST_F(MetadataManagerTest, GetActiveMetadataDoesNotAlwaysPreserveRange) {
    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));
    ChunkRange cr3(BSON("key" << 50), BSON("key" << 60));

    auto metadataWithTimestamp = _manager->getActiveMetadata(LogicalTime(Timestamp(1, 1)), false);

    _manager->setCollectionMetadata(cloneMetadataPlusChunk(metadataWithTimestamp->get(), cr1));
    ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 0);

    auto metadataWithoutTimestampNoPreservation = _manager->getActiveMetadata(boost::none, false);

    _manager->setCollectionMetadata(
        cloneMetadataPlusChunk(metadataWithoutTimestampNoPreservation->get(), cr2));
    ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 0);

    auto metadataWithoutTimestampPreserveRange =
        _manager->getActiveMetadata(boost::none, true /* preserveRange */);

    _manager->setCollectionMetadata(
        cloneMetadataPlusChunk(metadataWithoutTimestampPreserveRange->get(), cr3));
    ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 1);
}

TEST_F(MetadataManagerTest, ClearUnneededChunkManagerObjectsLastSnapshotInList) {
    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));

    auto scm1 = _manager->getActiveMetadata(boost::none, true);
    {
        _manager->setCollectionMetadata(cloneMetadataPlusChunk(scm1->get(), cr1));
        ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 1UL);

        auto scm2 = _manager->getActiveMetadata(boost::none, true);
        ASSERT_EQ(scm2->get().getChunks().size(), 1UL);
        _manager->setCollectionMetadata(cloneMetadataPlusChunk(scm2->get(), cr2));
        ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 2UL);
        ASSERT_EQ(_manager->numberOfEmptyMetadataSnapshots(), 0);
    }

    // The CollectionMetadata in scm2 should be set to boost::none because the object accessing it
    // is now out of scope, but that in scm1 should remain
    ASSERT_EQ(_manager->numberOfEmptyMetadataSnapshots(), 1);
    ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 2UL);

    auto scm = _manager->getActiveMetadata(boost::none, false);
    ASSERT_EQ(scm->get().getChunks().size(), 2UL);
}

TEST_F(MetadataManagerTest, ClearUnneededChunkManagerObjectSnapshotInMiddleOfList) {
    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));
    ChunkRange cr3(BSON("key" << 50), BSON("key" << 80));
    ChunkRange cr4(BSON("key" << 90), BSON("key" << 100));

    auto scm = _manager->getActiveMetadata(boost::none, true);
    _manager->setCollectionMetadata(cloneMetadataPlusChunk(scm->get(), cr1));
    ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 1UL);

    auto scm2 = _manager->getActiveMetadata(boost::none, true);
    ASSERT_EQ(scm2->get().getChunks().size(), 1UL);
    _manager->setCollectionMetadata(cloneMetadataPlusChunk(scm2->get(), cr2));

    {
        auto scm3 = _manager->getActiveMetadata(boost::none, true);
        ASSERT_EQ(scm3->get().getChunks().size(), 2UL);
        _manager->setCollectionMetadata(cloneMetadataPlusChunk(scm3->get(), cr3));
        ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 3UL);
        ASSERT_EQ(_manager->numberOfEmptyMetadataSnapshots(), 0);

        /**
         * The CollectionMetadata object created when creating scm2 above will be set to boost::none
         * when we overrwrite scm2 below. The _metadata list will then look like:
         * [
         *      CollectionMetadataTracker{ metadata: xxx, orphans: [], usageCounter: 1},
         *      CollectionMetadataTracker{ metadata: boost::none, orphans: [], usageCounter: 0},
         *      CollectionMetadataTracker{ metadata: xxx, orphans: [], usageCounter: 1},
         *      CollectionMetadataTracker{ metadata: xxx, orphans: [], usageCounter: 1}
         * ]
         */
        scm2 = _manager->getActiveMetadata(boost::none, true);
        ASSERT_EQ(scm2->get().getChunks().size(), 3UL);
        _manager->setCollectionMetadata(cloneMetadataPlusChunk(scm2->get(), cr4));
        ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 4UL);
        ASSERT_EQ(_manager->numberOfEmptyMetadataSnapshots(), 1);
    }


    /** The CollectionMetadata in scm3 should be set to boost::none because the object accessing it
     * is now out of scope. The _metadata list should look like:
     * [
     *      CollectionMetadataTracker{ metadata: xxx, orphans: [], usageCounter: 1},
     *      CollectionMetadataTracker{ metadata: boost::none, orphans: [], usageCounter: 0},
     *      CollectionMetadataTracker{ metadata: boost::none, orphans: [], usageCounter: 0},
     *      CollectionMetadataTracker{ metadata: xxx, orphans: [], usageCounter: 1}
     * ]
     */

    ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 4UL);
    ASSERT_EQ(_manager->numberOfEmptyMetadataSnapshots(), 2);
}

}  // namespace
}  // namespace mongo
