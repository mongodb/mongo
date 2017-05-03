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

    CollectionMetadata* addChunk(MetadataManager* manager) {
        ScopedCollectionMetadata scopedMetadata1 = manager->getActiveMetadata();

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
    MetadataManager manager(getServiceContext(), kNss, executor());
    std::unique_ptr<CollectionMetadata> cm = makeEmptyMetadata();
    auto cmPtr = cm.get();

    manager.refreshActiveMetadata(std::move(cm));
    ScopedCollectionMetadata scopedMetadata = manager.getActiveMetadata();

    ASSERT_EQ(cmPtr, scopedMetadata.getMetadata());
};


TEST_F(MetadataManagerTest, ResetActiveMetadata) {
    MetadataManager manager(getServiceContext(), kNss, executor());
    manager.refreshActiveMetadata(makeEmptyMetadata());
    auto cm2Ptr = addChunk(&manager);
    ScopedCollectionMetadata scopedMetadata2 = manager.getActiveMetadata();
    ASSERT_EQ(cm2Ptr, scopedMetadata2.getMetadata());
};

// In the following tests, the ranges-to-clean is not drained by the background deleter thread
// because the collection involved has no CollectionShardingState, so the task just returns without
// doing anything.

TEST_F(MetadataManagerTest, CleanUpForMigrateIn) {
    MetadataManager manager(getServiceContext(), kNss, executor());
    manager.refreshActiveMetadata(makeEmptyMetadata());

    ChunkRange range2(BSON("key" << 10), BSON("key" << 20));
    ASSERT(manager.beginReceive(ChunkRange(BSON("key" << 0), BSON("key" << 10))));
    ASSERT(manager.beginReceive(ChunkRange(BSON("key" << 10), BSON("key" << 20))));
    ASSERT_EQ(manager.numberOfRangesToClean(), 2UL);
    ASSERT_EQ(manager.numberOfRangesToCleanStillInUse(), 0UL);
}

TEST_F(MetadataManagerTest, AddRangeNotificationsBlockAndYield) {
    MetadataManager manager(getServiceContext(), kNss, executor());
    manager.refreshActiveMetadata(makeEmptyMetadata());

    ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ASSERT_OK(manager.cleanUpRange(cr1));
    ASSERT_EQ(manager.numberOfRangesToClean(), 1UL);
    auto notification = manager.trackOrphanedDataCleanup(cr1);
    ASSERT(notification != nullptr && !bool(*notification));
    notification->set(Status::OK());
    ASSERT(bool(*notification));
    ASSERT_OK(notification->get(operationContext()));
}

TEST_F(MetadataManagerTest, NotificationBlocksUntilDeletion) {
    ChunkRange cr1(BSON("key" << 20), BSON("key" << 30));
    MetadataManager manager(getServiceContext(), kNss, executor());
    manager.refreshActiveMetadata(makeEmptyMetadata());
    auto notif = manager.trackOrphanedDataCleanup(cr1);
    ASSERT(notif.get() == nullptr);
    {
        ASSERT_EQ(manager.numberOfMetadataSnapshots(), 0UL);
        ASSERT_EQ(manager.numberOfRangesToClean(), 0UL);

        auto scm = manager.getActiveMetadata();  // and increment scm's refcount
        ASSERT(bool(scm));
        addChunk(&manager);  // push new metadata

        ASSERT_EQ(manager.numberOfMetadataSnapshots(), 1UL);
        ASSERT_EQ(manager.numberOfRangesToClean(), 0UL);  // not yet...

        manager.cleanUpRange(cr1);
        ASSERT_EQ(manager.numberOfMetadataSnapshots(), 1UL);
        ASSERT_EQ(manager.numberOfRangesToClean(), 1UL);

        notif = manager.trackOrphanedDataCleanup(cr1);  // will wake when scm goes away
    }  // scm destroyed, refcount of tracker goes to zero
    ASSERT_EQ(manager.numberOfMetadataSnapshots(), 0UL);
    ASSERT_EQ(manager.numberOfRangesToClean(), 1UL);
    ASSERT(bool(notif));                            // woke
    notif = manager.trackOrphanedDataCleanup(cr1);  // now tracking the range in _rangesToClean
    ASSERT(notif.get() != nullptr);
}

TEST_F(MetadataManagerTest, RefreshAfterSuccessfulMigrationSinglePending) {
    MetadataManager manager(getServiceContext(), kNss, executor());
    manager.refreshActiveMetadata(makeEmptyMetadata());
    const ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    ASSERT_EQ(manager.getActiveMetadata()->getChunks().size(), 0UL);

    ChunkVersion version = manager.getActiveMetadata()->getCollVersion();
    version.incMajor();

    manager.refreshActiveMetadata(cloneMetadataPlusChunk(
        *manager.getActiveMetadata().getMetadata(), cr1.getMin(), cr1.getMax(), version));
    ASSERT_EQ(manager.getActiveMetadata()->getChunks().size(), 1UL);
}


TEST_F(MetadataManagerTest, RefreshAfterSuccessfulMigrationMultiplePending) {
    MetadataManager manager(getServiceContext(), kNss, executor());
    manager.refreshActiveMetadata(makeEmptyMetadata());

    const ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    const ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));
    ASSERT_EQ(manager.getActiveMetadata()->getChunks().size(), 0UL);

    {
        ChunkVersion version = manager.getActiveMetadata()->getCollVersion();
        version.incMajor();

        manager.refreshActiveMetadata(cloneMetadataPlusChunk(
            *manager.getActiveMetadata().getMetadata(), cr1.getMin(), cr1.getMax(), version));
        ASSERT_EQ(manager.numberOfRangesToClean(), 0UL);
        ASSERT_EQ(manager.getActiveMetadata()->getChunks().size(), 1UL);
    }

    {
        ChunkVersion version = manager.getActiveMetadata()->getCollVersion();
        version.incMajor();

        manager.refreshActiveMetadata(cloneMetadataPlusChunk(
            *manager.getActiveMetadata().getMetadata(), cr2.getMin(), cr2.getMax(), version));
        ASSERT_EQ(manager.getActiveMetadata()->getChunks().size(), 2UL);
    }
}

TEST_F(MetadataManagerTest, RefreshAfterNotYetCompletedMigrationMultiplePending) {
    MetadataManager manager(getServiceContext(), kNss, executor());
    manager.refreshActiveMetadata(makeEmptyMetadata());

    const ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    const ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));
    ASSERT_EQ(manager.getActiveMetadata()->getChunks().size(), 0UL);

    ChunkVersion version = manager.getActiveMetadata()->getCollVersion();
    version.incMajor();

    manager.refreshActiveMetadata(cloneMetadataPlusChunk(
        *manager.getActiveMetadata().getMetadata(), BSON("key" << 50), BSON("key" << 60), version));
    ASSERT_EQ(manager.getActiveMetadata()->getChunks().size(), 1UL);
}

TEST_F(MetadataManagerTest, BeginReceiveWithOverlappingRange) {
    MetadataManager manager(getServiceContext(), kNss, executor());
    manager.refreshActiveMetadata(makeEmptyMetadata());

    const ChunkRange cr1(BSON("key" << 0), BSON("key" << 10));
    const ChunkRange cr2(BSON("key" << 30), BSON("key" << 40));
    const ChunkRange crOverlap(BSON("key" << 5), BSON("key" << 35));

    ASSERT_EQ(manager.getActiveMetadata()->getChunks().size(), 0UL);
}

TEST_F(MetadataManagerTest, RefreshMetadataAfterDropAndRecreate) {
    MetadataManager manager(getServiceContext(), kNss, executor());
    manager.refreshActiveMetadata(makeEmptyMetadata());

    {
        auto metadata = manager.getActiveMetadata();
        ChunkVersion newVersion = metadata->getCollVersion();
        newVersion.incMajor();

        manager.refreshActiveMetadata(cloneMetadataPlusChunk(
            *metadata.getMetadata(), BSON("key" << 0), BSON("key" << 10), newVersion));
    }

    // Now, pretend that the collection was dropped and recreated
    auto recreateMetadata = makeEmptyMetadata();
    ChunkVersion newVersion = manager.getActiveMetadata()->getCollVersion();
    newVersion.incMajor();
    manager.refreshActiveMetadata(cloneMetadataPlusChunk(
        *recreateMetadata, BSON("key" << 20), BSON("key" << 30), newVersion));
    ASSERT_EQ(manager.getActiveMetadata()->getChunks().size(), 1UL);

    const auto chunkEntry = manager.getActiveMetadata()->getChunks().begin();
    ASSERT_BSONOBJ_EQ(BSON("key" << 20), chunkEntry->first);
    ASSERT_BSONOBJ_EQ(BSON("key" << 30), chunkEntry->second.getMaxKey());
    ASSERT_EQ(newVersion, chunkEntry->second.getVersion());
}

// Tests membership functions for _rangesToClean
TEST_F(MetadataManagerTest, RangesToCleanMembership) {
    MetadataManager manager(getServiceContext(), kNss, executor());
    manager.refreshActiveMetadata(makeEmptyMetadata());

    ASSERT(manager.numberOfRangesToClean() == 0UL);

    ChunkRange cr1 = ChunkRange(BSON("key" << 0), BSON("key" << 10));
    ASSERT_OK(manager.cleanUpRange(cr1));

    ASSERT(manager.numberOfRangesToClean() == 1UL);
}

}  // namespace
}  // namespace mongo
