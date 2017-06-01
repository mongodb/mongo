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
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/dist_lock_catalog_impl.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/sharding_mongod_test_fixture.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"


#include <boost/optional.hpp>

namespace mongo {
namespace {

using unittest::assertGet;

const NamespaceString kNss("TestDB", "TestColl");
const std::string kPattern = "X";
const BSONObj kShardKeyPattern{BSON(kPattern << 1)};
const std::string kShardName{"a"};
const HostAndPort dummyHost("dummy", 123);

class MetadataManagerTest : public ShardingMongodTestFixture {
public:
    std::shared_ptr<RemoteCommandTargeterMock> configTargeter() {
        return RemoteCommandTargeterMock::get(shardRegistry()->getConfigShard()->getTargeter());
    }

protected:
    void setUp() override {
        ShardingMongodTestFixture::setUp();
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
        initializeGlobalShardingStateForMongodForTest(ConnectionString(dummyHost));

        configTargeter()->setFindHostReturnValue(dummyHost);
    }

    std::unique_ptr<DistLockCatalog> makeDistLockCatalog(ShardRegistry* shardRegistry) override {
        invariant(shardRegistry);
        return stdx::make_unique<DistLockCatalogImpl>(shardRegistry);
    }

    std::unique_ptr<DistLockManager> makeDistLockManager(
        std::unique_ptr<DistLockCatalog> distLockCatalog) override {
        return stdx::make_unique<DistLockManagerMock>(std::move(distLockCatalog));
    }

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient(
        std::unique_ptr<DistLockManager> distLockManager) override {
        return stdx::make_unique<ShardingCatalogClientMock>(std::move(distLockManager));
    }

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

    CollectionMetadata* addChunk(std::shared_ptr<MetadataManager>& manager) {
        ScopedCollectionMetadata scopedMetadata1 = manager->getActiveMetadata(manager);

        ChunkVersion newVersion = scopedMetadata1->getCollVersion();
        newVersion.incMajor();
        std::unique_ptr<CollectionMetadata> cm2 = cloneMetadataPlusChunk(
            *scopedMetadata1.getMetadata(), BSON("key" << 0), BSON("key" << 20), newVersion);
        auto cm2Ptr = cm2.get();

        manager->refreshActiveMetadata(std::move(cm2));
        return cm2Ptr;
    }
};

TEST_F(MetadataManagerTest, SetAndGetActiveMetadata) {
    std::shared_ptr<MetadataManager> manager =
        std::make_shared<MetadataManager>(getServiceContext(), kNss, executor());
    std::unique_ptr<CollectionMetadata> cm = makeEmptyMetadata();
    auto cmPtr = cm.get();

    manager->refreshActiveMetadata(std::move(cm));
    ScopedCollectionMetadata scopedMetadata = manager->getActiveMetadata(manager);

    ASSERT_EQ(cmPtr, scopedMetadata.getMetadata());
};


TEST_F(MetadataManagerTest, ResetActiveMetadata) {
    std::shared_ptr<MetadataManager> manager =
        std::make_shared<MetadataManager>(getServiceContext(), kNss, executor());
    manager->refreshActiveMetadata(makeEmptyMetadata());
    auto cm2Ptr = addChunk(manager);
    ScopedCollectionMetadata scopedMetadata2 = manager->getActiveMetadata(manager);
    ASSERT_EQ(cm2Ptr, scopedMetadata2.getMetadata());
};

// In the following tests, the ranges-to-clean is not drained by the background deleter thread
// because the collection involved has no CollectionShardingState, so the task just returns without
// doing anything.

TEST_F(MetadataManagerTest, CleanUpForMigrateIn) {
    std::shared_ptr<MetadataManager> manager =
        std::make_shared<MetadataManager>(getServiceContext(), kNss, executor());
    manager->refreshActiveMetadata(makeEmptyMetadata());

    ChunkRange range1(BSON("key" << 0), BSON("key" << 10));
    ChunkRange range2(BSON("key" << 10), BSON("key" << 20));
    auto notif1 = manager->beginReceive(range1);
    ASSERT_TRUE(!notif1.ready());
    auto notif2 = manager->beginReceive(range2);
    ASSERT_TRUE(!notif2.ready());
    ASSERT_EQ(manager->numberOfRangesToClean(), 2UL);
    ASSERT_EQ(manager->numberOfRangesToCleanStillInUse(), 0UL);
    notif1.abandon();
    notif2.abandon();
}

TEST_F(MetadataManagerTest, AddRangeNotificationsBlockAndYield) {
    std::shared_ptr<MetadataManager> manager =
        std::make_shared<MetadataManager>(getServiceContext(), kNss, executor());
    manager->refreshActiveMetadata(makeEmptyMetadata());

    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    auto notifn1 = manager->cleanUpRange(cr1);
    ASSERT_FALSE(notifn1.ready());
    ASSERT_EQ(manager->numberOfRangesToClean(), 1UL);
    auto optNotifn = manager->trackOrphanedDataCleanup(cr1);
    ASSERT_FALSE(notifn1.ready());
    ASSERT_FALSE(optNotifn->ready());
    ASSERT(notifn1 == *optNotifn);
    notifn1.abandon();
    optNotifn->abandon();
}

TEST_F(MetadataManagerTest, NotificationBlocksUntilDeletion) {
    ChunkRange cr1(BSON("key" << 20), BSON("key" << 30));
    std::shared_ptr<MetadataManager> manager =
        std::make_shared<MetadataManager>(getServiceContext(), kNss, executor());
    manager->refreshActiveMetadata(makeEmptyMetadata());
    auto optNotif = manager->trackOrphanedDataCleanup(cr1);
    ASSERT_FALSE(optNotif);  // nothing to track yet
    {
        ASSERT_EQ(manager->numberOfMetadataSnapshots(), 0UL);
        ASSERT_EQ(manager->numberOfRangesToClean(), 0UL);

        auto scm = manager->getActiveMetadata(manager);  // and increment scm's refcount
        ASSERT(bool(scm));
        addChunk(manager);  // push new metadata

        ASSERT_EQ(manager->numberOfMetadataSnapshots(), 1UL);
        ASSERT_EQ(manager->numberOfRangesToClean(), 0UL);  // not yet...

        optNotif = manager->cleanUpRange(cr1);
        ASSERT_EQ(manager->numberOfMetadataSnapshots(), 1UL);
        ASSERT_EQ(manager->numberOfRangesToClean(), 1UL);
    }  // scm destroyed, refcount of metadata goes to zero
    ASSERT_EQ(manager->numberOfMetadataSnapshots(), 0UL);
    ASSERT_EQ(manager->numberOfRangesToClean(), 1UL);
    ASSERT_FALSE(optNotif->ready());
    auto optNotif2 = manager->trackOrphanedDataCleanup(cr1);  // now tracking it in _rangesToClean
    ASSERT_TRUE(optNotif && !optNotif->ready());
    ASSERT_TRUE(optNotif2 && !optNotif2->ready());
    ASSERT(*optNotif == *optNotif2);
    optNotif->abandon();
    optNotif2->abandon();
}

TEST_F(MetadataManagerTest, RefreshAfterSuccessfulMigrationSinglePending) {
    std::shared_ptr<MetadataManager> manager =
        std::make_shared<MetadataManager>(getServiceContext(), kNss, executor());
    manager->refreshActiveMetadata(makeEmptyMetadata());
    const ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ASSERT_EQ(manager->getActiveMetadata(manager)->getChunks().size(), 0UL);

    ChunkVersion version = manager->getActiveMetadata(manager)->getCollVersion();
    version.incMajor();

    manager->refreshActiveMetadata(cloneMetadataPlusChunk(
        *manager->getActiveMetadata(manager).getMetadata(), cr1.getMin(), cr1.getMax(), version));
    ASSERT_EQ(manager->getActiveMetadata(manager)->getChunks().size(), 1UL);
}


TEST_F(MetadataManagerTest, RefreshAfterSuccessfulMigrationMultiplePending) {
    std::shared_ptr<MetadataManager> manager =
        std::make_shared<MetadataManager>(getServiceContext(), kNss, executor());
    manager->refreshActiveMetadata(makeEmptyMetadata());

    const ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    const ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));
    ASSERT_EQ(manager->getActiveMetadata(manager)->getChunks().size(), 0UL);

    {
        ChunkVersion version = manager->getActiveMetadata(manager)->getCollVersion();
        version.incMajor();

        manager->refreshActiveMetadata(
            cloneMetadataPlusChunk(*manager->getActiveMetadata(manager).getMetadata(),
                                   cr1.getMin(),
                                   cr1.getMax(),
                                   version));
        ASSERT_EQ(manager->numberOfRangesToClean(), 0UL);
        ASSERT_EQ(manager->getActiveMetadata(manager)->getChunks().size(), 1UL);
    }

    {
        ChunkVersion version = manager->getActiveMetadata(manager)->getCollVersion();
        version.incMajor();

        manager->refreshActiveMetadata(
            cloneMetadataPlusChunk(*manager->getActiveMetadata(manager).getMetadata(),
                                   cr2.getMin(),
                                   cr2.getMax(),
                                   version));
        ASSERT_EQ(manager->getActiveMetadata(manager)->getChunks().size(), 2UL);
    }
}

TEST_F(MetadataManagerTest, RefreshAfterNotYetCompletedMigrationMultiplePending) {
    std::shared_ptr<MetadataManager> manager =
        std::make_shared<MetadataManager>(getServiceContext(), kNss, executor());
    manager->refreshActiveMetadata(makeEmptyMetadata());

    const ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    const ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));
    ASSERT_EQ(manager->getActiveMetadata(manager)->getChunks().size(), 0UL);

    ChunkVersion version = manager->getActiveMetadata(manager)->getCollVersion();
    version.incMajor();

    manager->refreshActiveMetadata(
        cloneMetadataPlusChunk(*manager->getActiveMetadata(manager).getMetadata(),
                               BSON("key" << 50),
                               BSON("key" << 60),
                               version));
    ASSERT_EQ(manager->getActiveMetadata(manager)->getChunks().size(), 1UL);
}

TEST_F(MetadataManagerTest, BeginReceiveWithOverlappingRange) {
    std::shared_ptr<MetadataManager> manager =
        std::make_shared<MetadataManager>(getServiceContext(), kNss, executor());
    manager->refreshActiveMetadata(makeEmptyMetadata());

    const ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    const ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));
    const ChunkRange crOverlap(BSON("key" << 5), BSON("key" << 35));

    ASSERT_EQ(manager->getActiveMetadata(manager)->getChunks().size(), 0UL);
}

TEST_F(MetadataManagerTest, RefreshMetadataAfterDropAndRecreate) {
    std::shared_ptr<MetadataManager> manager =
        std::make_shared<MetadataManager>(getServiceContext(), kNss, executor());
    manager->refreshActiveMetadata(makeEmptyMetadata());

    {
        auto metadata = manager->getActiveMetadata(manager);
        ChunkVersion newVersion = metadata->getCollVersion();
        newVersion.incMajor();

        manager->refreshActiveMetadata(cloneMetadataPlusChunk(
            *metadata.getMetadata(), BSON("key" << 0), BSON("key" << 10), newVersion));
    }

    // Now, pretend that the collection was dropped and recreated
    auto recreateMetadata = makeEmptyMetadata();
    ChunkVersion newVersion = manager->getActiveMetadata(manager)->getCollVersion();
    newVersion.incMajor();
    manager->refreshActiveMetadata(cloneMetadataPlusChunk(
        *recreateMetadata, BSON("key" << 20), BSON("key" << 30), newVersion));
    ASSERT_EQ(manager->getActiveMetadata(manager)->getChunks().size(), 1UL);

    const auto chunkEntry = manager->getActiveMetadata(manager)->getChunks().begin();
    ASSERT_BSONOBJ_EQ(BSON("key" << 20), chunkEntry->first);
    ASSERT_BSONOBJ_EQ(BSON("key" << 30), chunkEntry->second.getMaxKey());
    ASSERT_EQ(newVersion, chunkEntry->second.getVersion());
}

// Tests membership functions for _rangesToClean
TEST_F(MetadataManagerTest, RangesToCleanMembership) {
    std::shared_ptr<MetadataManager> manager =
        std::make_shared<MetadataManager>(getServiceContext(), kNss, executor());
    manager->refreshActiveMetadata(makeEmptyMetadata());

    ASSERT(manager->numberOfRangesToClean() == 0UL);

    ChunkRange cr1 = ChunkRange(BSON("key" << 0), BSON("key" << 10));
    auto notifn = manager->cleanUpRange(cr1);
    ASSERT(!notifn.ready());
    ASSERT(manager->numberOfRangesToClean() == 1UL);
    notifn.abandon();
}

}  // namespace
}  // namespace mongo
