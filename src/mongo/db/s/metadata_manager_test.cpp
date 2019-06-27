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

#include <boost/optional.hpp>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/metadata_manager.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

using unittest::assertGet;

const NamespaceString kNss("TestDB", "TestColl");
const std::string kPattern = "key";
const KeyPattern kShardKeyPattern(BSON(kPattern << 1));
const std::string kThisShard{"thisShard"};
const std::string kOtherShard{"otherShard"};

class MetadataManagerTest : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();
        _manager = std::make_shared<MetadataManager>(getServiceContext(), kNss, executor().get());
    }

    /**
     * Returns an instance of CollectionMetadata which has no chunks owned by 'thisShard'.
     */
    static CollectionMetadata makeEmptyMetadata() {
        const OID epoch = OID::gen();

        auto rt = RoutingTableHistory::makeNew(
            kNss,
            UUID::gen(),
            kShardKeyPattern,
            nullptr,
            false,
            epoch,
            {ChunkType{kNss,
                       ChunkRange{BSON(kPattern << MINKEY), BSON(kPattern << MAXKEY)},
                       ChunkVersion(1, 0, epoch),
                       kOtherShard}});

        std::shared_ptr<ChunkManager> cm = std::make_shared<ChunkManager>(rt, boost::none);

        return CollectionMetadata(cm, kThisShard);
    }

    /**
     * Returns a new metadata's instance based on the current state by adding a chunk with the
     * specified bounds and version. The chunk's version must be higher than that of all chunks
     * which are in the input metadata.
     *
     * It will fassert if the chunk bounds are incorrect or overlap an existing chunk or if the
     * chunk version is lower than the maximum one.
     */
    static CollectionMetadata cloneMetadataPlusChunk(const ScopedCollectionMetadata& metadata,
                                                     const ChunkRange& range) {
        const BSONObj& minKey = range.getMin();
        const BSONObj& maxKey = range.getMax();
        ASSERT(!rangeMapOverlaps(metadata->getChunks(), minKey, maxKey));

        auto cm = metadata->getChunkManager();

        const auto chunkToSplit = cm->findIntersectingChunkWithSimpleCollation(minKey);
        ASSERT_BSONOBJ_GTE(minKey, chunkToSplit.getMin());
        ASSERT_BSONOBJ_LT(maxKey, chunkToSplit.getMax());

        std::vector<ChunkType> splitChunks;

        auto chunkVersion = cm->getVersion();

        if (SimpleBSONObjComparator::kInstance.evaluate(chunkToSplit.getMin() < minKey)) {
            chunkVersion.incMajor();
            splitChunks.emplace_back(
                kNss, ChunkRange(chunkToSplit.getMin(), minKey), chunkVersion, kOtherShard);
        }

        chunkVersion.incMajor();
        splitChunks.emplace_back(kNss, ChunkRange(minKey, maxKey), chunkVersion, kThisShard);

        chunkVersion.incMajor();
        splitChunks.emplace_back(
            kNss, ChunkRange(maxKey, chunkToSplit.getMax()), chunkVersion, kOtherShard);

        auto rt = cm->getRoutingHistory()->makeUpdated(splitChunks);

        return CollectionMetadata(std::make_shared<ChunkManager>(rt, boost::none), kThisShard);
    }

    static CollectionMetadata cloneMetadataMinusChunk(const ScopedCollectionMetadata& metadata,
                                                      const ChunkRange& range) {
        const BSONObj& minKey = range.getMin();
        const BSONObj& maxKey = range.getMax();
        ASSERT(rangeMapOverlaps(metadata->getChunks(), minKey, maxKey));

        auto cm = metadata->getChunkManager();

        const auto chunkToMoveOut = cm->findIntersectingChunkWithSimpleCollation(minKey);
        ASSERT_BSONOBJ_EQ(minKey, chunkToMoveOut.getMin());
        ASSERT_BSONOBJ_EQ(maxKey, chunkToMoveOut.getMax());

        auto chunkVersion = cm->getVersion();
        chunkVersion.incMajor();

        auto rt = cm->getRoutingHistory()->makeUpdated(
            {ChunkType(kNss, ChunkRange(minKey, maxKey), chunkVersion, kOtherShard)});

        return CollectionMetadata(std::make_shared<ChunkManager>(rt, boost::none), kThisShard);
    }

    std::shared_ptr<MetadataManager> _manager;
};

TEST_F(MetadataManagerTest, InitialMetadataIsUnknown) {
    ASSERT(!_manager->getActiveMetadata(_manager, boost::none));
    ASSERT(!_manager->getActiveMetadata(_manager, LogicalTime(Timestamp(10))));

    ASSERT_EQ(0UL, _manager->numberOfMetadataSnapshots());
    ASSERT_EQ(0UL, _manager->numberOfRangesToClean());
    ASSERT_EQ(0UL, _manager->numberOfRangesToCleanStillInUse());
}

TEST_F(MetadataManagerTest, MetadataAfterClearIsUnknown) {
    _manager->setFilteringMetadata(makeEmptyMetadata());
    ASSERT(_manager->getActiveMetadata(_manager, boost::none));
    ASSERT(_manager->getActiveMetadata(_manager, LogicalTime(Timestamp(10))));

    _manager->clearFilteringMetadata();
    ASSERT(!_manager->getActiveMetadata(_manager, boost::none));
    ASSERT(!_manager->getActiveMetadata(_manager, LogicalTime(Timestamp(10))));

    ASSERT_EQ(0UL, _manager->numberOfMetadataSnapshots());
    ASSERT_EQ(0UL, _manager->numberOfRangesToClean());
    ASSERT_EQ(0UL, _manager->numberOfRangesToCleanStillInUse());
}

TEST_F(MetadataManagerTest, GetActiveMetadataForUnshardedCollection) {
    _manager->setFilteringMetadata(CollectionMetadata());

    ASSERT(_manager->getActiveMetadata(_manager, boost::none));
    ASSERT(!(*_manager->getActiveMetadata(_manager, boost::none))->isSharded());

    ASSERT(_manager->getActiveMetadata(_manager, LogicalTime(Timestamp(10))));
    ASSERT(!(*_manager->getActiveMetadata(_manager, LogicalTime(Timestamp(10))))->isSharded());
}

TEST_F(MetadataManagerTest, CleanUpForMigrateIn) {
    _manager->setFilteringMetadata(makeEmptyMetadata());

    // Sanity checks
    ASSERT((*_manager->getActiveMetadata(_manager, boost::none))->isSharded());
    ASSERT_EQ(0UL, (*_manager->getActiveMetadata(_manager, boost::none))->getChunks().size());

    ChunkRange range1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange range2(BSON("key" << 10), BSON("key" << 20));

    auto notif1 = _manager->beginReceive(range1);
    ASSERT(!notif1.ready());

    auto notif2 = _manager->beginReceive(range2);
    ASSERT(!notif2.ready());

    ASSERT_EQ(2UL, _manager->numberOfRangesToClean());
    ASSERT_EQ(0UL, _manager->numberOfRangesToCleanStillInUse());

    notif1.abandon();
    notif2.abandon();
}

TEST_F(MetadataManagerTest, AddRangeNotificationsBlockAndYield) {
    _manager->setFilteringMetadata(makeEmptyMetadata());

    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));

    auto notifn1 = _manager->cleanUpRange(cr1, Date_t{});
    ASSERT_FALSE(notifn1.ready());
    ASSERT_EQ(_manager->numberOfRangesToClean(), 1UL);

    auto optNotifn = _manager->trackOrphanedDataCleanup(cr1);
    ASSERT_FALSE(notifn1.ready());
    ASSERT_FALSE(optNotifn->ready());
    ASSERT(notifn1 == *optNotifn);
    notifn1.abandon();
    optNotifn->abandon();
}

TEST_F(MetadataManagerTest, NotificationBlocksUntilDeletion) {
    _manager->setFilteringMetadata(makeEmptyMetadata());

    ChunkRange cr1(BSON("key" << 20), BSON("key" << 30));
    auto optNotif = _manager->trackOrphanedDataCleanup(cr1);
    ASSERT(!optNotif);

    {
        ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 0UL);
        ASSERT_EQ(_manager->numberOfRangesToClean(), 0UL);

        auto scm1 = _manager->getActiveMetadata(_manager, boost::none);  // and increment refcount

        const auto addChunk = [this] {
            _manager->setFilteringMetadata(
                cloneMetadataPlusChunk(*_manager->getActiveMetadata(_manager, boost::none),
                                       {BSON("key" << 0), BSON("key" << 20)}));
        };

        addChunk();                                                      // push new metadata
        auto scm2 = _manager->getActiveMetadata(_manager, boost::none);  // and increment refcount
        ASSERT_EQ(1ULL, (*scm2)->getChunks().size());

        // Simulate drop and recreate
        _manager->setFilteringMetadata(makeEmptyMetadata());

        addChunk();                                                      // push new metadata
        auto scm3 = _manager->getActiveMetadata(_manager, boost::none);  // and increment refcount
        ASSERT_EQ(1ULL, (*scm3)->getChunks().size());

        ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 0UL);
        ASSERT_EQ(_manager->numberOfRangesToClean(), 0UL);

        optNotif = _manager->cleanUpRange(cr1, Date_t{});
        ASSERT(optNotif);
        ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 0UL);
        ASSERT_EQ(_manager->numberOfRangesToClean(), 1UL);
    }

    // At this point scm1,2,3 above are destroyed and the refcount of each metadata goes to zero

    ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 0UL);
    ASSERT_EQ(_manager->numberOfRangesToClean(), 1UL);
    ASSERT(!optNotif->ready());

    auto optNotif2 = _manager->trackOrphanedDataCleanup(cr1);  // now tracking it in _rangesToClean
    ASSERT(optNotif2);

    ASSERT(!optNotif->ready());
    ASSERT(!optNotif2->ready());
    ASSERT(*optNotif == *optNotif2);

    optNotif->abandon();
    optNotif2->abandon();
}

TEST_F(MetadataManagerTest, CleanupNotificationsAreSignaledOnDropAndRecreate) {
    const ChunkRange rangeToClean(BSON("key" << 20), BSON("key" << 30));

    _manager->setFilteringMetadata(makeEmptyMetadata());
    _manager->setFilteringMetadata(
        cloneMetadataPlusChunk(*_manager->getActiveMetadata(_manager, boost::none),
                               {BSON("key" << 0), BSON("key" << 20)}));

    _manager->setFilteringMetadata(
        cloneMetadataPlusChunk(*_manager->getActiveMetadata(_manager, boost::none), rangeToClean));
    auto cursorOnMovedMetadata = _manager->getActiveMetadata(_manager, boost::none);

    _manager->setFilteringMetadata(
        cloneMetadataMinusChunk(*_manager->getActiveMetadata(_manager, boost::none), rangeToClean));

    auto notif = _manager->cleanUpRange(rangeToClean, Date_t{});
    ASSERT(!notif.ready());

    auto optNotif = _manager->trackOrphanedDataCleanup(rangeToClean);
    ASSERT(optNotif);
    ASSERT(!optNotif->ready());

    _manager->setFilteringMetadata(makeEmptyMetadata());
    ASSERT(notif.ready());
    ASSERT(optNotif->ready());
}

TEST_F(MetadataManagerTest, RefreshAfterSuccessfulMigrationSinglePending) {
    _manager->setFilteringMetadata(makeEmptyMetadata());

    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));

    _manager->setFilteringMetadata(
        cloneMetadataPlusChunk(*_manager->getActiveMetadata(_manager, boost::none), cr1));
    ASSERT_EQ((*_manager->getActiveMetadata(_manager, boost::none))->getChunks().size(), 1UL);
}

TEST_F(MetadataManagerTest, RefreshAfterSuccessfulMigrationMultiplePending) {
    _manager->setFilteringMetadata(makeEmptyMetadata());

    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));

    {
        _manager->setFilteringMetadata(
            cloneMetadataPlusChunk(*_manager->getActiveMetadata(_manager, boost::none), cr1));
        ASSERT_EQ(_manager->numberOfRangesToClean(), 0UL);
        ASSERT_EQ((*_manager->getActiveMetadata(_manager, boost::none))->getChunks().size(), 1UL);
    }

    {
        _manager->setFilteringMetadata(
            cloneMetadataPlusChunk(*_manager->getActiveMetadata(_manager, boost::none), cr2));
        ASSERT_EQ((*_manager->getActiveMetadata(_manager, boost::none))->getChunks().size(), 2UL);
    }
}

TEST_F(MetadataManagerTest, RefreshAfterNotYetCompletedMigrationMultiplePending) {
    _manager->setFilteringMetadata(makeEmptyMetadata());

    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));

    _manager->setFilteringMetadata(
        cloneMetadataPlusChunk(*_manager->getActiveMetadata(_manager, boost::none),
                               {BSON("key" << 50), BSON("key" << 60)}));
    ASSERT_EQ((*_manager->getActiveMetadata(_manager, boost::none))->getChunks().size(), 1UL);
}

TEST_F(MetadataManagerTest, BeginReceiveWithOverlappingRange) {
    _manager->setFilteringMetadata(makeEmptyMetadata());

    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));

    _manager->setFilteringMetadata(
        cloneMetadataPlusChunk(*_manager->getActiveMetadata(_manager, boost::none), cr1));
    _manager->setFilteringMetadata(
        cloneMetadataPlusChunk(*_manager->getActiveMetadata(_manager, boost::none), cr2));

    ChunkRange crOverlap(BSON("key" << 5), BSON("key" << 35));
}

TEST_F(MetadataManagerTest, RefreshMetadataAfterDropAndRecreate) {
    _manager->setFilteringMetadata(makeEmptyMetadata());
    _manager->setFilteringMetadata(
        cloneMetadataPlusChunk(*_manager->getActiveMetadata(_manager, boost::none),
                               {BSON("key" << 0), BSON("key" << 10)}));

    // Now, pretend that the collection was dropped and recreated
    _manager->setFilteringMetadata(makeEmptyMetadata());
    _manager->setFilteringMetadata(
        cloneMetadataPlusChunk(*_manager->getActiveMetadata(_manager, boost::none),
                               {BSON("key" << 20), BSON("key" << 30)}));

    const auto chunks = (*_manager->getActiveMetadata(_manager, boost::none))->getChunks();
    ASSERT_EQ(1UL, chunks.size());
    const auto chunkEntry = chunks.begin();
    ASSERT_BSONOBJ_EQ(BSON("key" << 20), chunkEntry->first);
    ASSERT_BSONOBJ_EQ(BSON("key" << 30), chunkEntry->second);
}

// Tests membership functions for _rangesToClean
TEST_F(MetadataManagerTest, RangesToCleanMembership) {
    _manager->setFilteringMetadata(makeEmptyMetadata());

    ChunkRange cr(BSON("key" << 0), BSON("key" << 10));

    ASSERT_EQ(0UL, _manager->numberOfRangesToClean());

    auto notifn = _manager->cleanUpRange(cr, Date_t{});
    ASSERT(!notifn.ready());
    ASSERT_EQ(1UL, _manager->numberOfRangesToClean());

    notifn.abandon();
}

TEST_F(MetadataManagerTest, ClearUnneededChunkManagerObjectsLastSnapshotInList) {
    _manager->setFilteringMetadata(makeEmptyMetadata());
    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));

    auto scm1 = *_manager->getActiveMetadata(_manager, boost::none);
    {
        _manager->setFilteringMetadata(cloneMetadataPlusChunk(scm1, cr1));
        ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 1UL);
        ASSERT_EQ(_manager->numberOfRangesToClean(), 0UL);

        auto scm2 = *_manager->getActiveMetadata(_manager, boost::none);
        ASSERT_EQ(scm2->getChunks().size(), 1UL);
        _manager->setFilteringMetadata(cloneMetadataPlusChunk(scm2, cr2));
        ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 2UL);
        ASSERT_EQ(_manager->numberOfEmptyMetadataSnapshots(), 0);
    }

    // The CollectionMetadata in scm2 should be set to boost::none because the object accessing it
    // is now out of scope, but that in scm1 should remain
    ASSERT_EQ(_manager->numberOfEmptyMetadataSnapshots(), 1);
    ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 2UL);
    ASSERT_EQ((*_manager->getActiveMetadata(_manager, boost::none))->getChunks().size(), 2UL);
}

TEST_F(MetadataManagerTest, ClearUnneededChunkManagerObjectSnapshotInMiddleOfList) {
    _manager->setFilteringMetadata(makeEmptyMetadata());
    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));
    ChunkRange cr3(BSON("key" << 50), BSON("key" << 80));
    ChunkRange cr4(BSON("key" << 90), BSON("key" << 100));

    auto scm = *_manager->getActiveMetadata(_manager, boost::none);
    _manager->setFilteringMetadata(cloneMetadataPlusChunk(scm, cr1));
    ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 1UL);
    ASSERT_EQ(_manager->numberOfRangesToClean(), 0UL);

    auto scm2 = *_manager->getActiveMetadata(_manager, boost::none);
    ASSERT_EQ(scm2->getChunks().size(), 1UL);
    _manager->setFilteringMetadata(cloneMetadataPlusChunk(scm2, cr2));

    {
        auto scm3 = *_manager->getActiveMetadata(_manager, boost::none);
        ASSERT_EQ(scm3->getChunks().size(), 2UL);
        _manager->setFilteringMetadata(cloneMetadataPlusChunk(scm3, cr3));
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
        scm2 = *_manager->getActiveMetadata(_manager, boost::none);
        ASSERT_EQ(scm2->getChunks().size(), 3UL);
        _manager->setFilteringMetadata(cloneMetadataPlusChunk(scm2, cr4));
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
