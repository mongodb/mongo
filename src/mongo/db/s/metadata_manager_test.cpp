/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/stdx/condition_variable.h"
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
        _manager = std::make_shared<MetadataManager>(
            getServiceContext(), kNss, executor(), makeEmptyMetadata());
        orphanCleanupDelaySecs.store(1);
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
            nullptr,
            false,
            epoch,
            Timestamp(1, 1),
            boost::none /* timeseriesFields */,
            boost::none,
            boost::none /* chunkSizeBytes */,
            true,
            {ChunkType{uuid, range, ChunkVersion(1, 0, epoch, Timestamp(1, 1)), kOtherShard}});

        return CollectionMetadata(ChunkManager(kThisShard,
                                               DatabaseVersion(UUID::gen(), Timestamp(1, 1)),
                                               makeStandaloneRoutingTableHistory(std::move(rt)),
                                               boost::none),
                                  kThisShard);
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
            splitChunks.emplace_back(*collMetadata.getUUID(),
                                     ChunkRange(chunkToSplit.getMin(), minKey),
                                     chunkVersion,
                                     kOtherShard);
        }

        chunkVersion.incMajor();
        splitChunks.emplace_back(
            *collMetadata.getUUID(), ChunkRange(minKey, maxKey), chunkVersion, kThisShard);

        chunkVersion.incMajor();
        splitChunks.emplace_back(*collMetadata.getUUID(),
                                 ChunkRange(maxKey, chunkToSplit.getMax()),
                                 chunkVersion,
                                 kOtherShard);

        auto rt = cm->getRoutingTableHistory_ForTest().makeUpdated(
            boost::none /* timeseriesFields */, boost::none, boost::none, true, splitChunks);

        return CollectionMetadata(ChunkManager(cm->dbPrimary(),
                                               cm->dbVersion(),
                                               makeStandaloneRoutingTableHistory(std::move(rt)),
                                               boost::none),
                                  kThisShard);
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

        auto rt = cm->getRoutingTableHistory_ForTest().makeUpdated(
            boost::none /* timeseriesFields */,
            boost::none,
            boost::none,
            true,
            {ChunkType(
                *metadata.getUUID(), ChunkRange(minKey, maxKey), chunkVersion, kOtherShard)});

        return CollectionMetadata(ChunkManager(cm->dbPrimary(),
                                               cm->dbVersion(),
                                               makeStandaloneRoutingTableHistory(std::move(rt)),
                                               boost::none),
                                  kThisShard);
    }

    std::shared_ptr<MetadataManager> _manager;
};

TEST_F(MetadataManagerTest, TrackOrphanedDataCleanupBlocksOnScheduledRangeDeletions) {
    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));

    // Enable fail point to suspendRangeDeletion.
    globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::alwaysOn);

    auto notifn1 = _manager->cleanUpRange(cr1, boost::none, false /*delayBeforeDeleting*/);
    ASSERT_FALSE(notifn1.isReady());
    ASSERT_EQ(_manager->numberOfRangesToClean(), 1UL);

    auto optNotifn = _manager->trackOrphanedDataCleanup(cr1);
    ASSERT_FALSE(notifn1.isReady());
    ASSERT_FALSE(optNotifn->isReady());

    globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::off);
}

TEST_F(MetadataManagerTest, CleanupNotificationsAreSignaledWhenMetadataManagerIsDestroyed) {
    const ChunkRange rangeToClean(BSON("key" << 20), BSON("key" << 30));

    _manager->setFilteringMetadata(cloneMetadataPlusChunk(
        _manager->getActiveMetadata(boost::none)->get(), {BSON("key" << 0), BSON("key" << 20)}));

    _manager->setFilteringMetadata(
        cloneMetadataPlusChunk(_manager->getActiveMetadata(boost::none)->get(), rangeToClean));

    // Optional so that it can be reset.
    boost::optional<ScopedCollectionDescription> cursorOnMovedMetadata{
        _manager->getActiveMetadata(boost::none)};

    _manager->setFilteringMetadata(
        cloneMetadataMinusChunk(_manager->getActiveMetadata(boost::none)->get(), rangeToClean));

    auto notif = _manager->cleanUpRange(rangeToClean, boost::none, false /*delayBeforeDeleting*/);
    ASSERT(!notif.isReady());

    auto optNotif = _manager->trackOrphanedDataCleanup(rangeToClean);
    ASSERT(optNotif);
    ASSERT(!optNotif->isReady());

    // Reset the original shared_ptr. The cursorOnMovedMetadata will still contain its own copy of
    // the shared_ptr though, so the destructor of ~MetadataManager won't yet be called.
    _manager.reset();
    ASSERT(!notif.isReady());
    ASSERT(!optNotif->isReady());

    // Destroys the ScopedCollectionDescription object and causes the destructor of MetadataManager
    // to run, which should trigger all deletion notifications.
    cursorOnMovedMetadata.reset();

    // Advance time to simulate orphanCleanupDelaySecs passing.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        network()->advanceTime(network()->now() + Seconds{5});
    }

    notif.wait();
    optNotif->wait();
}

TEST_F(MetadataManagerTest, RefreshAfterSuccessfulMigrationSinglePending) {
    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));

    _manager->setFilteringMetadata(
        cloneMetadataPlusChunk(_manager->getActiveMetadata(boost::none)->get(), cr1));
    ASSERT_EQ(_manager->getActiveMetadata(boost::none)->get().getChunks().size(), 1UL);
}

TEST_F(MetadataManagerTest, RefreshAfterSuccessfulMigrationMultiplePending) {
    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));

    {
        _manager->setFilteringMetadata(
            cloneMetadataPlusChunk(_manager->getActiveMetadata(boost::none)->get(), cr1));
        ASSERT_EQ(_manager->numberOfRangesToClean(), 0UL);
        ASSERT_EQ(_manager->getActiveMetadata(boost::none)->get().getChunks().size(), 1UL);
    }

    {
        _manager->setFilteringMetadata(
            cloneMetadataPlusChunk(_manager->getActiveMetadata(boost::none)->get(), cr2));
        ASSERT_EQ(_manager->getActiveMetadata(boost::none)->get().getChunks().size(), 2UL);
    }
}

TEST_F(MetadataManagerTest, RefreshAfterNotYetCompletedMigrationMultiplePending) {
    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));

    _manager->setFilteringMetadata(cloneMetadataPlusChunk(
        _manager->getActiveMetadata(boost::none)->get(), {BSON("key" << 50), BSON("key" << 60)}));
    ASSERT_EQ(_manager->getActiveMetadata(boost::none)->get().getChunks().size(), 1UL);
}

TEST_F(MetadataManagerTest, BeginReceiveWithOverlappingRange) {
    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));

    _manager->setFilteringMetadata(
        cloneMetadataPlusChunk(_manager->getActiveMetadata(boost::none)->get(), cr1));
    _manager->setFilteringMetadata(
        cloneMetadataPlusChunk(_manager->getActiveMetadata(boost::none)->get(), cr2));

    ChunkRange crOverlap(BSON("key" << 5), BSON("key" << 35));
}

// Tests membership functions for _rangesToClean
TEST_F(MetadataManagerTest, RangesToCleanMembership) {
    ChunkRange cr(BSON("key" << 0), BSON("key" << 10));

    ASSERT_EQ(0UL, _manager->numberOfRangesToClean());

    // Enable fail point to suspendRangeDeletion.
    globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::alwaysOn);

    auto notifn = _manager->cleanUpRange(cr, boost::none, false /*delayBeforeDeleting*/);
    ASSERT(!notifn.isReady());
    ASSERT_EQ(1UL, _manager->numberOfRangesToClean());

    globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::off);
}

TEST_F(MetadataManagerTest, ClearUnneededChunkManagerObjectsLastSnapshotInList) {
    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));

    auto scm1 = _manager->getActiveMetadata(boost::none);
    {
        _manager->setFilteringMetadata(cloneMetadataPlusChunk(scm1->get(), cr1));
        ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 1UL);
        ASSERT_EQ(_manager->numberOfRangesToClean(), 0UL);

        auto scm2 = _manager->getActiveMetadata(boost::none);
        ASSERT_EQ(scm2->get().getChunks().size(), 1UL);
        _manager->setFilteringMetadata(cloneMetadataPlusChunk(scm2->get(), cr2));
        ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 2UL);
        ASSERT_EQ(_manager->numberOfEmptyMetadataSnapshots(), 0);
    }

    // The CollectionMetadata in scm2 should be set to boost::none because the object accessing it
    // is now out of scope, but that in scm1 should remain
    ASSERT_EQ(_manager->numberOfEmptyMetadataSnapshots(), 1);
    ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 2UL);

    auto scm = _manager->getActiveMetadata(boost::none);
    ASSERT_EQ(scm->get().getChunks().size(), 2UL);
}

TEST_F(MetadataManagerTest, ClearUnneededChunkManagerObjectSnapshotInMiddleOfList) {
    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));
    ChunkRange cr3(BSON("key" << 50), BSON("key" << 80));
    ChunkRange cr4(BSON("key" << 90), BSON("key" << 100));

    auto scm = _manager->getActiveMetadata(boost::none);
    _manager->setFilteringMetadata(cloneMetadataPlusChunk(scm->get(), cr1));
    ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 1UL);
    ASSERT_EQ(_manager->numberOfRangesToClean(), 0UL);

    auto scm2 = _manager->getActiveMetadata(boost::none);
    ASSERT_EQ(scm2->get().getChunks().size(), 1UL);
    _manager->setFilteringMetadata(cloneMetadataPlusChunk(scm2->get(), cr2));

    {
        auto scm3 = _manager->getActiveMetadata(boost::none);
        ASSERT_EQ(scm3->get().getChunks().size(), 2UL);
        _manager->setFilteringMetadata(cloneMetadataPlusChunk(scm3->get(), cr3));
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
        scm2 = _manager->getActiveMetadata(boost::none);
        ASSERT_EQ(scm2->get().getChunks().size(), 3UL);
        _manager->setFilteringMetadata(cloneMetadataPlusChunk(scm2->get(), cr4));
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
