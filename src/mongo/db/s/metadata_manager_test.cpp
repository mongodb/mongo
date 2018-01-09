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

#include <boost/optional.hpp>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/catalog/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/metadata_manager.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/sharding_mongod_test_fixture.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

using unittest::assertGet;

const NamespaceString kNss("TestDB", "TestColl");
const std::string kPattern = "key";
const BSONObj kShardKeyPatternBSON{BSON(kPattern << 1)};
const KeyPattern kShardKeyPattern{kShardKeyPatternBSON};
const std::string kThisShard{"thisShard"};
const std::string kOtherShard{"otherShard"};
const HostAndPort dummyHost("dummy", 123);

class MetadataManagerTest : public ShardingMongodTestFixture {
protected:
    void setUp() override {
        ShardingMongodTestFixture::setUp();
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
        initializeGlobalShardingStateForMongodForTest(ConnectionString(dummyHost))
            .transitional_ignore();

        configTargeter()->setFindHostReturnValue(dummyHost);

        _manager = std::make_shared<MetadataManager>(getServiceContext(), kNss, executor());
    }

    std::shared_ptr<RemoteCommandTargeterMock> configTargeter() const {
        return RemoteCommandTargeterMock::get(shardRegistry()->getConfigShard()->getTargeter());
    }

    static std::unique_ptr<CollectionMetadata> makeEmptyMetadata() {
        const OID epoch = OID::gen();

        auto cm = ChunkManager::makeNew(
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
        return stdx::make_unique<CollectionMetadata>(cm, kThisShard);
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
        const CollectionMetadata& metadata, const BSONObj& minKey, const BSONObj& maxKey) {
        invariant(minKey.woCompare(maxKey) < 0);
        invariant(!rangeMapOverlaps(metadata.getChunks(), minKey, maxKey));

        auto cm = metadata.getChunkManager();

        const auto chunkToSplit = cm->findIntersectingChunkWithSimpleCollation(minKey);
        ASSERT(SimpleBSONObjComparator::kInstance.evaluate(maxKey <= chunkToSplit->getMax()))
            << "maxKey == " << maxKey
            << " and chunkToSplit->getMax() == " << chunkToSplit->getMax();
        auto v1 = cm->getVersion();
        v1.incMajor();
        auto v2 = v1;
        v2.incMajor();
        auto v3 = v2;
        v3.incMajor();
        cm = cm->makeUpdated(
            {ChunkType{kNss, ChunkRange{chunkToSplit->getMin(), minKey}, v1, kOtherShard},
             ChunkType{kNss, ChunkRange{minKey, maxKey}, v2, kThisShard},
             ChunkType{kNss, ChunkRange{maxKey, chunkToSplit->getMax()}, v3, kOtherShard}});
        return stdx::make_unique<CollectionMetadata>(cm, kThisShard);
    }

    CollectionMetadata* addChunk(std::shared_ptr<MetadataManager>& manager) {
        ScopedCollectionMetadata scopedMetadata1 = manager->getActiveMetadata(manager);

        std::unique_ptr<CollectionMetadata> cm2 = cloneMetadataPlusChunk(
            *scopedMetadata1.getMetadata(), BSON("key" << 0), BSON("key" << 20));
        auto cm2Ptr = cm2.get();

        manager->refreshActiveMetadata(std::move(cm2));
        return cm2Ptr;
    }

    std::shared_ptr<MetadataManager> _manager;
};

// In the following tests, the ranges-to-clean is not drained by the background deleter thread
// because the collection involved has no CollectionShardingState, so the task just returns without
// doing anything.

TEST_F(MetadataManagerTest, CleanUpForMigrateIn) {
    _manager->refreshActiveMetadata(makeEmptyMetadata());

    ChunkRange range1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange range2(BSON("key" << 10), BSON("key" << 20));
    auto notif1 = _manager->beginReceive(range1);
    ASSERT_TRUE(!notif1.ready());
    auto notif2 = _manager->beginReceive(range2);
    ASSERT_TRUE(!notif2.ready());
    ASSERT_EQ(_manager->numberOfRangesToClean(), 2UL);
    ASSERT_EQ(_manager->numberOfRangesToCleanStillInUse(), 0UL);
    notif1.abandon();
    notif2.abandon();
}

TEST_F(MetadataManagerTest, AddRangeNotificationsBlockAndYield) {
    _manager->refreshActiveMetadata(makeEmptyMetadata());

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
    ChunkRange cr1(BSON("key" << 20), BSON("key" << 30));
    _manager->refreshActiveMetadata(makeEmptyMetadata());
    auto optNotif = _manager->trackOrphanedDataCleanup(cr1);
    ASSERT_FALSE(optNotif);  // nothing to track yet
    {
        ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 0UL);
        ASSERT_EQ(_manager->numberOfRangesToClean(), 0UL);

        auto scm1 = _manager->getActiveMetadata(_manager);  // and increment refcount
        ASSERT_TRUE(bool(scm1));
        ASSERT_EQ(0ULL, scm1->getChunks().size());

        addChunk(_manager);                                 // push new metadata
        auto scm2 = _manager->getActiveMetadata(_manager);  // and increment refcount
        ASSERT_EQ(1ULL, scm2->getChunks().size());

        // this is here solely to pacify an invariant in addChunk
        _manager->refreshActiveMetadata(makeEmptyMetadata());

        addChunk(_manager);                                 // push new metadata
        auto scm3 = _manager->getActiveMetadata(_manager);  // and increment refcount
        ASSERT_EQ(1ULL, scm3->getChunks().size());

        auto overlaps = _manager->overlappingMetadata(
            _manager, ChunkRange(BSON("key" << 0), BSON("key" << 10)));
        ASSERT_EQ(2ULL, overlaps.size());
        std::vector<ScopedCollectionMetadata> ref;
        ref.push_back(std::move(scm3));
        ref.push_back(std::move(scm2));
        ASSERT(ref == overlaps);

        ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 3UL);
        ASSERT_EQ(_manager->numberOfRangesToClean(), 0UL);  // not yet...

        optNotif = _manager->cleanUpRange(cr1, Date_t{});
        ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 3UL);
        ASSERT_EQ(_manager->numberOfRangesToClean(), 1UL);
    }  // scm1,2,3 destroyed, refcount of each metadata goes to zero
    ASSERT_EQ(_manager->numberOfMetadataSnapshots(), 0UL);
    ASSERT_EQ(_manager->numberOfRangesToClean(), 1UL);
    ASSERT_FALSE(optNotif->ready());
    auto optNotif2 = _manager->trackOrphanedDataCleanup(cr1);  // now tracking it in _rangesToClean
    ASSERT_TRUE(optNotif && !optNotif->ready());
    ASSERT_TRUE(optNotif2 && !optNotif2->ready());
    ASSERT(*optNotif == *optNotif2);
    optNotif->abandon();
    optNotif2->abandon();
}

TEST_F(MetadataManagerTest, RefreshAfterSuccessfulMigrationSinglePending) {
    _manager->refreshActiveMetadata(makeEmptyMetadata());
    const ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ASSERT_EQ(_manager->getActiveMetadata(_manager)->getChunks().size(), 0UL);

    _manager->refreshActiveMetadata(cloneMetadataPlusChunk(
        *_manager->getActiveMetadata(_manager).getMetadata(), cr1.getMin(), cr1.getMax()));
    ASSERT_EQ(_manager->getActiveMetadata(_manager)->getChunks().size(), 1UL);
}


TEST_F(MetadataManagerTest, RefreshAfterSuccessfulMigrationMultiplePending) {
    _manager->refreshActiveMetadata(makeEmptyMetadata());

    const ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    const ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));
    ASSERT_EQ(_manager->getActiveMetadata(_manager)->getChunks().size(), 0UL);

    {
        _manager->refreshActiveMetadata(cloneMetadataPlusChunk(
            *_manager->getActiveMetadata(_manager).getMetadata(), cr1.getMin(), cr1.getMax()));
        ASSERT_EQ(_manager->numberOfRangesToClean(), 0UL);
        ASSERT_EQ(_manager->getActiveMetadata(_manager)->getChunks().size(), 1UL);
    }

    {
        _manager->refreshActiveMetadata(cloneMetadataPlusChunk(
            *_manager->getActiveMetadata(_manager).getMetadata(), cr2.getMin(), cr2.getMax()));
        ASSERT_EQ(_manager->getActiveMetadata(_manager)->getChunks().size(), 2UL);
    }
}

TEST_F(MetadataManagerTest, RefreshAfterNotYetCompletedMigrationMultiplePending) {
    _manager->refreshActiveMetadata(makeEmptyMetadata());

    const ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    const ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));
    ASSERT_EQ(_manager->getActiveMetadata(_manager)->getChunks().size(), 0UL);

    _manager->refreshActiveMetadata(
        cloneMetadataPlusChunk(*_manager->getActiveMetadata(_manager).getMetadata(),
                               BSON("key" << 50),
                               BSON("key" << 60)));
    ASSERT_EQ(_manager->getActiveMetadata(_manager)->getChunks().size(), 1UL);
}

TEST_F(MetadataManagerTest, BeginReceiveWithOverlappingRange) {
    _manager->refreshActiveMetadata(makeEmptyMetadata());

    const ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    const ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));
    const ChunkRange crOverlap(BSON("key" << 5), BSON("key" << 35));

    ASSERT_EQ(_manager->getActiveMetadata(_manager)->getChunks().size(), 0UL);
}

TEST_F(MetadataManagerTest, RefreshMetadataAfterDropAndRecreate) {
    _manager->refreshActiveMetadata(makeEmptyMetadata());

    {
        auto metadata = _manager->getActiveMetadata(_manager);
        _manager->refreshActiveMetadata(
            cloneMetadataPlusChunk(*metadata.getMetadata(), BSON("key" << 0), BSON("key" << 10)));
    }

    // Now, pretend that the collection was dropped and recreated
    auto recreateMetadata = makeEmptyMetadata();
    _manager->refreshActiveMetadata(
        cloneMetadataPlusChunk(*recreateMetadata, BSON("key" << 20), BSON("key" << 30)));
    ASSERT_EQ(_manager->getActiveMetadata(_manager)->getChunks().size(), 1UL);

    const auto chunkEntry = _manager->getActiveMetadata(_manager)->getChunks().begin();
    ASSERT_BSONOBJ_EQ(BSON("key" << 20), chunkEntry->first);
    ASSERT_BSONOBJ_EQ(BSON("key" << 30), chunkEntry->second);
}

// Tests membership functions for _rangesToClean
TEST_F(MetadataManagerTest, RangesToCleanMembership) {
    _manager->refreshActiveMetadata(makeEmptyMetadata());

    ASSERT(_manager->numberOfRangesToClean() == 0UL);

    ChunkRange cr1 = ChunkRange(BSON("key" << 0), BSON("key" << 10));
    auto notifn = _manager->cleanUpRange(cr1, Date_t{});
    ASSERT(!notifn.ready());
    ASSERT(_manager->numberOfRangesToClean() == 1UL);
    notifn.abandon();
}

}  // namespace
}  // namespace mongo
