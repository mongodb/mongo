/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache_loader.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache_loader_mock.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/sharding_catalog_client_mock.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/range_deleter_service_test.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_mongod_test_fixture.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"

#include <chrono>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const std::string kShardKey = "_id";
const BSONObj kShardKeyPattern = BSON(kShardKey << 1);

class CollectionShardingRuntimeTest : public ShardServerTestFixture {
public:
    static CollectionMetadata makeShardedMetadata(OperationContext* opCtx,
                                                  UUID uuid = UUID::gen(),
                                                  ShardId chunkShardId = ShardId("other"),
                                                  ShardId collectionShardId = ShardId("0")) {
        const OID epoch = OID::gen();
        const Timestamp timestamp(Date_t::now());

        // Sleeping some time here to guarantee that any upcoming call to this function generates a
        // different timestamp
        stdx::this_thread::sleep_for(stdx::chrono::milliseconds(10));

        auto range = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY));
        auto chunk = ChunkType(
            uuid, std::move(range), ChunkVersion({epoch, timestamp}, {1, 0}), chunkShardId);
        ChunkManager cm(makeStandaloneRoutingTableHistory(
                            RoutingTableHistory::makeNew(kTestNss,
                                                         uuid,
                                                         kShardKeyPattern,
                                                         false, /* unsplittable */
                                                         nullptr,
                                                         false,
                                                         epoch,
                                                         timestamp,
                                                         boost::none /* timeseriesFields */,
                                                         boost::none /* reshardingFields */,

                                                         true,
                                                         {std::move(chunk)})),
                        boost::none);

        return CollectionMetadata(std::move(cm), collectionShardId);
    }

    uint64_t getNumMetadataManagerChanges(CollectionShardingRuntime& csr) {
        return csr._numMetadataManagerChanges;
    }

    MetadataManager* getMetadataManager(CollectionShardingRuntime& csr) {
        return csr._metadataManager.get();
    }
};

TEST_F(CollectionShardingRuntimeTest,
       GetCollectionDescriptionThrowsStaleConfigBeforeSetFilteringMetadataIsCalledAndNoOSSSet) {
    OperationContext* opCtx = operationContext();
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    ASSERT_FALSE(csr.getCollectionDescription(opCtx).isSharded());
    auto metadata = makeShardedMetadata(opCtx);
    ScopedSetShardRole scopedSetShardRole{
        opCtx, kTestNss, ShardVersionFactory::make(metadata), boost::none /* databaseVersion */};
    ASSERT_THROWS_CODE(csr.getCollectionDescription(opCtx), DBException, ErrorCodes::StaleConfig);
}

TEST_F(
    CollectionShardingRuntimeTest,
    GetCollectionDescriptionReturnsUnshardedAfterSetFilteringMetadataIsCalledWithUnshardedMetadata) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    csr.setFilteringMetadata(operationContext(), CollectionMetadata::UNTRACKED());
    ASSERT_FALSE(csr.getCollectionDescription(operationContext()).isSharded());
}

TEST_F(CollectionShardingRuntimeTest,
       GetCollectionDescriptionReturnsShardedAfterSetFilteringMetadataIsCalledWithShardedMetadata) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);
    ScopedSetShardRole scopedSetShardRole{
        opCtx, kTestNss, ShardVersionFactory::make(metadata), boost::none /* databaseVersion */};
    ASSERT_TRUE(csr.getCollectionDescription(opCtx).isSharded());
}

TEST_F(CollectionShardingRuntimeTest,
       GetCurrentMetadataIfKnownReturnsNoneBeforeSetFilteringMetadataIsCalled) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    ASSERT_FALSE(csr.getCurrentMetadataIfKnown());
}

TEST_F(
    CollectionShardingRuntimeTest,
    GetCurrentMetadataIfKnownReturnsUnshardedAfterSetFilteringMetadataIsCalledWithUntrackedMetadata) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    csr.setFilteringMetadata(operationContext(), CollectionMetadata::UNTRACKED());
    const auto optCurrMetadata = csr.getCurrentMetadataIfKnown();
    ASSERT_TRUE(optCurrMetadata);
    ASSERT_FALSE(optCurrMetadata->isSharded());
    ASSERT_EQ(optCurrMetadata->getShardPlacementVersion(), ChunkVersion::UNSHARDED());
}

TEST_F(
    CollectionShardingRuntimeTest,
    GetCurrentMetadataIfKnownReturnsShardedAfterSetFilteringMetadataIsCalledWithShardedMetadata) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);
    const auto optCurrMetadata = csr.getCurrentMetadataIfKnown();
    ASSERT_TRUE(optCurrMetadata);
    ASSERT_TRUE(optCurrMetadata->isSharded());
    ASSERT_EQ(optCurrMetadata->getShardPlacementVersion(), metadata.getShardPlacementVersion());
}

TEST_F(CollectionShardingRuntimeTest,
       GetCurrentMetadataIfKnownReturnsNoneAfterClearFilteringMetadataIsCalled) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    csr.setFilteringMetadata(opCtx, makeShardedMetadata(opCtx));
    csr.clearFilteringMetadata(opCtx);
    ASSERT_FALSE(csr.getCurrentMetadataIfKnown());
}

TEST_F(CollectionShardingRuntimeTest, SetFilteringMetadataWithSameUUIDKeepsSameMetadataManager) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    ASSERT_EQ(getNumMetadataManagerChanges(csr), 0);
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);
    // Should create a new MetadataManager object, bumping the count to 1.
    ASSERT_EQ(getNumMetadataManagerChanges(csr), 1);
    // Set it again.
    csr.setFilteringMetadata(opCtx, metadata);
    // Should not have reset metadata, so the counter should still be 1.
    ASSERT_EQ(getNumMetadataManagerChanges(csr), 1);
}

TEST_F(CollectionShardingRuntimeTest,
       SetFilteringMetadataWithDifferentUUIDReplacesPreviousMetadataManager) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);
    ScopedSetShardRole scopedSetShardRole{
        opCtx, kTestNss, ShardVersionFactory::make(metadata), boost::none /* databaseVersion */};
    ASSERT_EQ(getNumMetadataManagerChanges(csr), 1);

    // Set it again with a different metadata object (UUID is generated randomly in
    // makeShardedMetadata()).
    auto newMetadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, newMetadata);

    ASSERT_EQ(getNumMetadataManagerChanges(csr), 2);
    ASSERT(
        csr.getCollectionDescription(opCtx).uuidMatches(newMetadata.getChunkManager()->getUUID()));
}

TEST_F(CollectionShardingRuntimeTest,
       TransitioningFromUntrackedToTrackedMetadataKeepsSameMetadataManager) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    ASSERT_EQ(0, getNumMetadataManagerChanges(csr));

    // Set an UNTRACKED metadata
    csr.setFilteringMetadata(opCtx, CollectionMetadata::UNTRACKED());

    // Should create a new MetadataManager object, bumping the count to 1.
    ASSERT_EQ(1, getNumMetadataManagerChanges(csr));
    ASSERT_EQ(0, getMetadataManager(csr)->numberOfMetadataSnapshots());
    ASSERT_EQ(boost::none, getMetadataManager(csr)->getCollectionUuid());
    ASSERT_EQ(false, getMetadataManager(csr)->hasRoutingTable());

    // Retain the added metadata when a newer metadata is installed
    auto rangePreserver = csr.getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);

    // Set a TRACKED METADATA
    auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);

    // Should not have reset metadata, so the counter should still be 1.
    ASSERT_EQ(1, getNumMetadataManagerChanges(csr));
    ASSERT_EQ(1, getMetadataManager(csr)->numberOfMetadataSnapshots());
    ASSERT_EQ(metadata.getUUID(), getMetadataManager(csr)->getCollectionUuid().get());
    ASSERT_EQ(true, getMetadataManager(csr)->hasRoutingTable());
}

TEST_F(CollectionShardingRuntimeTest,
       TransitioningFromUntrackedToUntrackedMetadataKeepsMetadataManager) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    ASSERT_EQ(0, getNumMetadataManagerChanges(csr));

    // Set an UNTRACKED metadata
    csr.setFilteringMetadata(opCtx, CollectionMetadata::UNTRACKED());

    // When a range preserver isn't bound to a metadata tracker, it gets automatically removed once
    // another filtering metadata is added. Hence, we should install a range preserver to avoid
    // getting a false positive result on this test.
    auto rangePreserver = csr.getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);

    // Should create a new MetadataManager object, bumping the count to 1.
    ASSERT_EQ(1, getNumMetadataManagerChanges(csr));
    ASSERT_EQ(0, getMetadataManager(csr)->numberOfMetadataSnapshots());
    ASSERT_EQ(boost::none, getMetadataManager(csr)->getCollectionUuid());
    ASSERT_EQ(false, getMetadataManager(csr)->hasRoutingTable());

    // Set UNTRACKED METADATA again
    csr.setFilteringMetadata(opCtx, CollectionMetadata::UNTRACKED());

    // Should not have reset metadata, so the counter should still be 1.
    // Should not have added any snapshot to the metadata manager.
    ASSERT_EQ(1, getNumMetadataManagerChanges(csr));
    ASSERT_EQ(0, getMetadataManager(csr)->numberOfMetadataSnapshots());
    ASSERT_EQ(boost::none, getMetadataManager(csr)->getCollectionUuid());
    ASSERT_EQ(false, getMetadataManager(csr)->hasRoutingTable());
}

TEST_F(CollectionShardingRuntimeTest,
       TransitioningFromTrackedToUntrackedMetadataRestoresMetadataManager) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    ASSERT_EQ(0, getNumMetadataManagerChanges(csr));

    // Set a TRACKED METADATA
    auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);

    // Should create a new MetadataManager object, bumping the count to 1.
    ASSERT_EQ(1, getNumMetadataManagerChanges(csr));
    ASSERT_EQ(0, getMetadataManager(csr)->numberOfMetadataSnapshots());
    ASSERT_EQ(metadata.getUUID(), getMetadataManager(csr)->getCollectionUuid().get());
    ASSERT_EQ(true, getMetadataManager(csr)->hasRoutingTable());

    // Set UNTRACKED METADATA
    csr.setFilteringMetadata(opCtx, CollectionMetadata::UNTRACKED());

    // Should have reset metadata, so the counter should have bumped to 1.
    ASSERT_EQ(2, getNumMetadataManagerChanges(csr));
    ASSERT_EQ(0, getMetadataManager(csr)->numberOfMetadataSnapshots());
    ASSERT_EQ(boost::none, getMetadataManager(csr)->getCollectionUuid());
    ASSERT_EQ(false, getMetadataManager(csr)->hasRoutingTable());
}

TEST_F(CollectionShardingRuntimeTest,
       TransitioningFromTrackedToTrackedMetadataWithSameUUIDKeepsMetadataManager) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    ASSERT_EQ(0, getNumMetadataManagerChanges(csr));

    auto metadata1 = makeShardedMetadata(opCtx);
    auto metadata2 = makeShardedMetadata(opCtx, metadata1.getUUID());

    // Set a TRACKED METADATA
    {
        csr.setFilteringMetadata(opCtx, metadata1);

        // Should create a new MetadataManager object, bumping the count to 1.
        ASSERT_EQ(1, getNumMetadataManagerChanges(csr));
        ASSERT_EQ(0, getMetadataManager(csr)->numberOfMetadataSnapshots());
        ASSERT_EQ(metadata1.getUUID(), getMetadataManager(csr)->getCollectionUuid().get());
        ASSERT_EQ(true, getMetadataManager(csr)->hasRoutingTable());
    }

    // Retain the added metadata when a newer metadata is installed
    auto rangePreserver = csr.getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);

    // Set a TRACKED METADATA again with the same UUID
    {
        csr.setFilteringMetadata(opCtx, metadata2);

        // Should keep the same MetadataManager object and increase the number of snapshots
        ASSERT_EQ(1, getNumMetadataManagerChanges(csr));
        ASSERT_EQ(1, getMetadataManager(csr)->numberOfMetadataSnapshots());
        ASSERT_EQ(metadata1.getUUID(), getMetadataManager(csr)->getCollectionUuid().get());
        ASSERT_EQ(true, getMetadataManager(csr)->hasRoutingTable());
    }
}

TEST_F(CollectionShardingRuntimeTest,
       TransitioningFromTrackedToTrackedMetadataWithDifferentUUIDRestoresMetadataManager) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    ASSERT_EQ(0, getNumMetadataManagerChanges(csr));

    const auto metadata1 = makeShardedMetadata(opCtx);
    const auto metadata2 = makeShardedMetadata(opCtx);
    ASSERT_NE(metadata1.getUUID(), metadata2.getUUID());

    // Set a TRACKED METADATA
    {
        csr.setFilteringMetadata(opCtx, metadata1);

        // Should create a new MetadataManager object, bumping the count to 1.
        ASSERT_EQ(1, getNumMetadataManagerChanges(csr));
        ASSERT_EQ(0, getMetadataManager(csr)->numberOfMetadataSnapshots());
        ASSERT_EQ(metadata1.getUUID(), getMetadataManager(csr)->getCollectionUuid().get());
        ASSERT_EQ(true, getMetadataManager(csr)->hasRoutingTable());
    }

    // Set a TRACKED METADATA again with a different UUID
    {
        csr.setFilteringMetadata(opCtx, metadata2);

        // Should restore the MetadataManager object.
        ASSERT_EQ(2, getNumMetadataManagerChanges(csr));
        ASSERT_EQ(0, getMetadataManager(csr)->numberOfMetadataSnapshots());
        ASSERT_EQ(metadata2.getUUID(), getMetadataManager(csr)->getCollectionUuid().get());
        ASSERT_EQ(true, getMetadataManager(csr)->hasRoutingTable());
    }
}


TEST_F(CollectionShardingRuntimeTest, ShardVersionCheckDetectsClusterTimeConflicts) {
    OperationContext* opCtx = operationContext();
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    const auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);

    const auto collectionTimestamp = metadata.getShardPlacementVersion().getTimestamp();

    auto receivedShardVersion = ShardVersionFactory::make(metadata);

    // Test that conflict is thrown when transaction 'atClusterTime' is not valid the current shard
    // version.
    {
        const auto previousReadConcern = repl::ReadConcernArgs::get(operationContext());
        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);

        // Valid atClusterTime (equal or later than collection timestamp).
        {
            repl::ReadConcernArgs::get(operationContext())
                .setArgsAtClusterTimeForSnapshot(collectionTimestamp + 1);
            ScopedSetShardRole scopedSetShardRole{
                opCtx, kTestNss, receivedShardVersion, boost::none /* databaseVersion */};
            ASSERT_DOES_NOT_THROW(csr.checkShardVersionOrThrow(opCtx));
        }

        // Conflicting atClusterTime (earlier than collection timestamp).
        repl::ReadConcernArgs::get(operationContext())
            .setArgsAtClusterTimeForSnapshot(collectionTimestamp - 1);
        ScopedSetShardRole scopedSetShardRole{
            opCtx, kTestNss, receivedShardVersion, boost::none /* databaseVersion */};
        ASSERT_THROWS_CODE(
            csr.checkShardVersionOrThrow(opCtx), DBException, ErrorCodes::SnapshotUnavailable);

        repl::ReadConcernArgs::get(operationContext()) = previousReadConcern;
    }

    // Test that conflict is thrown when transaction 'placementConflictTime' is not valid the
    // current shard version.
    {
        // Valid placementConflictTime (equal or later than collection timestamp).
        {
            receivedShardVersion.setPlacementConflictTime(LogicalTime(collectionTimestamp + 1));
            ScopedSetShardRole scopedSetShardRole{
                opCtx, kTestNss, receivedShardVersion, boost::none /* databaseVersion */};
            ASSERT_DOES_NOT_THROW(csr.checkShardVersionOrThrow(opCtx));
        }

        // Conflicting placementConflictTime (earlier than collection timestamp).
        receivedShardVersion.setPlacementConflictTime(LogicalTime(collectionTimestamp - 1));
        ScopedSetShardRole scopedSetShardRole{
            opCtx, kTestNss, receivedShardVersion, boost::none /* databaseVersion */};
        ASSERT_THROWS_CODE(
            csr.checkShardVersionOrThrow(opCtx), DBException, ErrorCodes::SnapshotUnavailable);
    }
}

TEST_F(CollectionShardingRuntimeTest, InvalidateRangePreserversOlderThanShardVersion) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    // By default, makeShardedMetadata assigns different shard IDs to the chunk ("other") and the
    // collection ("0"), resulting in a placement version of {0,0}. For this test, we want to ensure
    // the chunk and collection share the same shard ID ("0") to generate comparable chunk versions.
    // This setup is required to correctly test invalidateRangePreserversOlderThanShardVersion,
    // which compares shard placement versions for invalidation.
    ShardId metadataShardId("0");
    auto metadataInThePast =
        makeShardedMetadata(opCtx, UUID::gen(), metadataShardId, metadataShardId);
    auto metadata = makeShardedMetadata(opCtx, UUID::gen(), metadataShardId, metadataShardId);
    csr.setFilteringMetadata(opCtx, metadata);
    const auto optCurrMetadata = csr.getCurrentMetadataIfKnown();
    ASSERT_TRUE(optCurrMetadata);
    ASSERT_TRUE(optCurrMetadata->isSharded());
    ASSERT_EQ(optCurrMetadata->getShardPlacementVersion(), metadata.getShardPlacementVersion());

    auto ownershipFilter = csr.getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);

    ASSERT_TRUE(ownershipFilter.isRangePreserverStillValid());

    // Test that the trackers will not be invalidated with version ChunkVersion::IGNORED()
    csr.invalidateRangePreserversOlderThanShardVersion(opCtx, ChunkVersion::IGNORED());
    ASSERT_TRUE(ownershipFilter.isRangePreserverStillValid());

    // Test that the trackers will not be invalidated with version which is older
    csr.invalidateRangePreserversOlderThanShardVersion(
        opCtx, metadataInThePast.getShardPlacementVersion());
    ASSERT_TRUE(ownershipFilter.isRangePreserverStillValid());

    // Test that the trackers will be invalidated with current version
    csr.invalidateRangePreserversOlderThanShardVersion(opCtx, metadata.getShardPlacementVersion());
    ASSERT_FALSE(ownershipFilter.isRangePreserverStillValid());
}

TEST_F(CollectionShardingRuntimeTest, InvalidateRangePreserversOlderThanUnshardedVersion) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);

    auto ownershipFilter = csr.getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);

    ASSERT_TRUE(ownershipFilter.isRangePreserverStillValid());

    // Test that the trackers will be invalidated with version ChunkVersion::UNSHARDED().
    // Test is prepared for the case when UNSHARDED metadata will be started to be tracked. In this
    // case ownershipFilter::shardPlacementVersion = UNSHARDED. Currently it's not possible to test
    // as in this case metadataManager is not created for unsharded collection. When it will be
    // changed it will be possible to test against a current version.
    csr.invalidateRangePreserversOlderThanShardVersion(opCtx, ChunkVersion::UNSHARDED());
    ASSERT_FALSE(ownershipFilter.isRangePreserverStillValid());
}

TEST_F(CollectionShardingRuntimeTest, InvalidateRangePreserversUntrackedCollection) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    csr.setFilteringMetadata(opCtx, CollectionMetadata::UNTRACKED());

    auto ownershipFilter = csr.getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup, true);

    ASSERT_TRUE(ownershipFilter.isRangePreserverStillValid());

    // Promote the collection to a sharded collection since it only make sense to invalidat range
    // preservers on sharded collections.
    const auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);

    ASSERT_TRUE(ownershipFilter.isRangePreserverStillValid());

    csr.invalidateRangePreserversOlderThanShardVersion(opCtx, metadata.getShardPlacementVersion());
    ASSERT_FALSE(ownershipFilter.isRangePreserverStillValid());
}


class CollectionShardingRuntimeTestWithMockedLoader
    : public ShardServerTestFixtureWithCatalogCacheLoaderMock {
public:
    const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.foo");
    const UUID kCollUUID = UUID::gen();
    const std::string kShardKey = "x";
    const std::vector<ShardType> kShardList = {ShardType(kMyShardName.toString(), "Host0:12345")};

    void setUp() override {
        ShardServerTestFixtureWithCatalogCacheLoaderMock::setUp();

        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());

        for (const auto& shard : kShardList) {
            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            HostAndPort host(shard.getHost());
            targeter->setConnectionStringReturnValue(ConnectionString(host));
            targeter->setFindHostReturnValue(host);
            targeterFactory()->addTargeterToReturn(ConnectionString(host), std::move(targeter));
        }
    }

    void tearDown() override {
        WaitForMajorityService::get(getServiceContext()).shutDown();

        ShardServerTestFixtureWithCatalogCacheLoaderMock::tearDown();
    }

    class StaticCatalogClient final : public ShardingCatalogClientMock {
    public:
        StaticCatalogClient(std::vector<ShardType> shards) : _shards(std::move(shards)) {}

        repl::OpTimeWith<std::vector<ShardType>> getAllShards(OperationContext* opCtx,
                                                              repl::ReadConcernLevel readConcern,
                                                              BSONObj filter) override {
            return repl::OpTimeWith<std::vector<ShardType>>(_shards);
        }

        std::vector<CollectionType> getShardedCollections(OperationContext* opCtx,
                                                          const DatabaseName& dbName,
                                                          repl::ReadConcernLevel readConcernLevel,
                                                          const BSONObj& sort) override {
            return {};
        }

        std::vector<CollectionType> getCollections(OperationContext* opCtx,
                                                   const DatabaseName& dbName,
                                                   repl::ReadConcernLevel readConcernLevel,
                                                   const BSONObj& sort) override {
            return _colls;
        }

        void setCollections(std::vector<CollectionType> colls) {
            _colls = std::move(colls);
        }

    private:
        const std::vector<ShardType> _shards;
        std::vector<CollectionType> _colls;
    };

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        return std::make_unique<StaticCatalogClient>(kShardList);
    }

    CollectionType createCollection(const OID& epoch, const Timestamp& timestamp) {
        CollectionType res(kNss, epoch, timestamp, Date_t::now(), kCollUUID, BSON(kShardKey << 1));
        res.setAllowMigrations(false);
        return res;
    }

    std::vector<ChunkType> createChunks(const OID& epoch,
                                        const UUID& uuid,
                                        const Timestamp& timestamp) {
        auto range1 = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << 5));
        ChunkType chunk1(
            uuid, range1, ChunkVersion({epoch, timestamp}, {1, 0}), kShardList[0].getName());

        auto range2 = ChunkRange(BSON(kShardKey << 5), BSON(kShardKey << MAXKEY));
        ChunkType chunk2(
            uuid, range2, ChunkVersion({epoch, timestamp}, {1, 1}), kShardList[0].getName());

        return {chunk1, chunk2};
    }
};

/**
 * Fixture for when range deletion functionality is required in CollectionShardingRuntime tests.
 */
class CollectionShardingRuntimeWithRangeDeleterTest : service_context_test::WithSetupTransportLayer,
                                                      public CollectionShardingRuntimeTest {
public:
    void setUp() override {
        CollectionShardingRuntimeTest::setUp();
        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());
        // Set up replication coordinator to be primary and have no replication delay.
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext());
        replCoord->setCanAcceptNonLocalWrites(true);
        std::ignore = replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY);
        // Make waitForWriteConcern return immediately.
        replCoord->setAwaitReplicationReturnValueFunction([this](OperationContext* opCtx,
                                                                 const repl::OpTime& opTime) {
            return repl::ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0));
        });
        repl::ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));

        createTestCollection(operationContext(), kTestNss);

        AutoGetCollection autoColl(operationContext(), kTestNss, MODE_IX);
        _uuid = autoColl.getCollection()->uuid();

        auto opCtx = operationContext();
        RangeDeleterService::get(opCtx)->onStartup(opCtx);
        RangeDeleterService::get(opCtx)->onStepUpComplete(opCtx, 0L);
        RangeDeleterService::get(opCtx)->getRangeDeleterServiceInitializationFuture().get(opCtx);
    }

    void tearDown() override {
        DBDirectClient client(operationContext());
        client.dropCollection(kTestNss);

        RangeDeleterService::get(operationContext())->onStepDown();
        RangeDeleterService::get(operationContext())->onShutdown();
        WaitForMajorityService::get(getServiceContext()).shutDown();
        CollectionShardingRuntimeTest::tearDown();
    }

    CollectionShardingRuntime::ScopedExclusiveCollectionShardingRuntime csr() {
        AutoGetCollection autoColl(operationContext(), kTestNss, MODE_IX);
        return CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
            operationContext(), kTestNss);
    }

    const UUID& uuid() const {
        return _uuid;
    }

private:
    UUID _uuid{UUID::gen()};
};

// The range deleter service test util will register a task with the range deleter with pending set
// to true, insert the task, and then remove the pending field. We must create the task with pending
// set to true so that the removal of the pending field succeeds.
RangeDeletionTask createRangeDeletionTask(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const UUID& uuid,
                                          const ChunkRange& range,
                                          int64_t numOrphans) {
    auto migrationId = UUID::gen();
    RangeDeletionTask t(migrationId, nss, uuid, ShardId("donor"), range, CleanWhenEnum::kNow);
    t.setNumOrphanDocs(numOrphans);
    const auto currentTime = VectorClock::get(opCtx)->getTime();
    t.setTimestamp(currentTime.clusterTime().asTimestamp());
    t.setPending(true);
    return t;
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanReturnsErrorIfMetadataManagerDoesNotExist) {
    auto status = CollectionShardingRuntime::waitForClean(
        operationContext(),
        kTestNss,
        uuid(),
        ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)),
        Date_t::max());
    ASSERT_EQ(status.code(), ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanReturnsErrorIfCollectionUUIDDoesNotMatchFilteringMetadata) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx, uuid());
    csr()->setFilteringMetadata(opCtx, metadata);
    auto randomUuid = UUID::gen();

    auto status = CollectionShardingRuntime::waitForClean(
        opCtx,
        kTestNss,
        randomUuid,
        ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)),
        Date_t::max());
    ASSERT_EQ(status.code(), ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanReturnsOKIfNoDeletionsAreScheduled) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx, uuid());
    csr()->setFilteringMetadata(opCtx, metadata);

    auto status = CollectionShardingRuntime::waitForClean(
        opCtx,
        kTestNss,
        uuid(),
        ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)),
        Date_t::max());

    ASSERT_OK(status);
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanBlocksBehindOneScheduledDeletion) {
    // Enable fail point to suspendRangeDeletion.
    globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::alwaysOn);
    ScopeGuard resetFailPoint(
        [=] { globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::off); });

    OperationContext* opCtx = operationContext();

    auto metadata = makeShardedMetadata(opCtx, uuid());
    csr()->setFilteringMetadata(opCtx, metadata);
    const ChunkRange range = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY));

    const auto task = createRangeDeletionTask(opCtx, kTestNss, uuid(), range, 0);
    auto taskCompletionFuture = registerAndCreatePersistentTask(
        opCtx, task, SemiFuture<void>::makeReady() /* waitForActiveQueries */);

    opCtx->setDeadlineAfterNowBy(Milliseconds(100), ErrorCodes::MaxTimeMSExpired);
    auto status =
        CollectionShardingRuntime::waitForClean(opCtx, kTestNss, uuid(), range, Date_t::max());

    ASSERT_EQ(status.code(), ErrorCodes::MaxTimeMSExpired);

    globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::off);
    taskCompletionFuture.get();
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanBlocksBehindAllScheduledDeletions) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx, uuid());
    csr()->setFilteringMetadata(opCtx, metadata);

    const auto middleKey = 5;
    const ChunkRange range1 = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << middleKey));
    const auto task1 = createRangeDeletionTask(opCtx, kTestNss, uuid(), range1, 0);
    const ChunkRange range2 = ChunkRange(BSON(kShardKey << middleKey), BSON(kShardKey << MAXKEY));
    const auto task2 = createRangeDeletionTask(opCtx, kTestNss, uuid(), range2, 0);

    auto cleanupCompleteFirst = registerAndCreatePersistentTask(
        opCtx, task1, SemiFuture<void>::makeReady() /* waitForActiveQueries */);

    auto cleanupCompleteSecond = registerAndCreatePersistentTask(
        opCtx, task2, SemiFuture<void>::makeReady() /* waitForActiveQueries */);

    auto status = CollectionShardingRuntime::waitForClean(
        opCtx,
        kTestNss,
        uuid(),
        ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)),
        Date_t::max());

    // waitForClean should block until both cleanup tasks have run. This is a best-effort check,
    // since even if it did not block, it is possible that the cleanup tasks could complete before
    // reaching these lines.
    ASSERT(cleanupCompleteFirst.isReady());
    ASSERT(cleanupCompleteSecond.isReady());

    ASSERT_OK(status);
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanReturnsOKAfterSuccessfulDeletion) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx, uuid());
    csr()->setFilteringMetadata(opCtx, metadata);
    const ChunkRange range = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY));
    const auto task = createRangeDeletionTask(opCtx, kTestNss, uuid(), range, 0);

    auto cleanupComplete = registerAndCreatePersistentTask(
        opCtx, task, SemiFuture<void>::makeReady() /* waitForActiveQueries */);

    auto status =
        CollectionShardingRuntime::waitForClean(opCtx, kTestNss, uuid(), range, Date_t::max());

    ASSERT_OK(status);
    ASSERT(cleanupComplete.isReady());
}

class CollectionShardingRuntimeWithCatalogTest
    : public CollectionShardingRuntimeWithRangeDeleterTest {
public:
    void setUp() override {
        CollectionShardingRuntimeWithRangeDeleterTest::setUp();
        DBDirectClient client(operationContext());
        client.createCollection(NamespaceString::kShardIndexCatalogNamespace);
        client.createCollection(NamespaceString::kShardCollectionCatalogNamespace);
    }

    void tearDown() override {
        OpObserver::Times::get(operationContext()).reservedOpTimes.clear();
        CollectionShardingRuntimeWithRangeDeleterTest::tearDown();
    }
};

// Test the CSR before and after the initialization of the ShardingState with ClusterRole::None.
TEST_F(ShardingMongoDTestFixture, ShardingStateDisabledReturnsUntrackedVersion) {
    OperationContext* opCtx = operationContext();
    const auto metadata = CollectionShardingRuntimeTest::makeShardedMetadata(opCtx);
    auto receivedShardVersion = ShardVersionFactory::make(metadata);
    ScopedSetShardRole scopedSetShardRole{
        opCtx, kTestNss, receivedShardVersion, boost::none /* databaseVersion */};

    // While the ShardingState has not yet been recovered, we expect the CollectionShardingRuntime
    // to present all collections as UNTRACKED.
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    ASSERT_DOES_NOT_THROW(csr.getCollectionDescription(opCtx));
    ASSERT_DOES_NOT_THROW(csr.checkShardVersionOrThrow(opCtx));

    // Setting the recovery completed as ClusterRole::None is also equilvament to initialize a
    // standalone replica-set. The CollectionShardingState should continue to present collections as
    // UNTRACKED.
    ShardingState::RecoveredClusterRole rcr;
    rcr.role = ClusterRole::None;
    auto shardingState = ShardingState::get(opCtx);
    shardingState->setRecoveryCompleted(rcr);
    ASSERT_DOES_NOT_THROW(csr.getCollectionDescription(opCtx));
    ASSERT_DOES_NOT_THROW(csr.checkShardVersionOrThrow(opCtx));
}

// Test the CSR before and after the initialization of the ShardingState with shard server role.
TEST_F(ShardingMongoDTestFixture, ShardingStateEnabledReturnsTrackedVersion) {
    OperationContext* opCtx = operationContext();
    const auto metadata = CollectionShardingRuntimeTest::makeShardedMetadata(opCtx);
    auto receivedShardVersion = ShardVersionFactory::make(metadata);
    ScopedSetShardRole scopedSetShardRole{
        opCtx, kTestNss, receivedShardVersion, boost::none /* databaseVersion */};

    // While the ShardingState has not yet been recovered, we expect the CollectionShardingRuntime
    // to present all collections as UNTRACKED.
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    ASSERT_DOES_NOT_THROW(csr.getCollectionDescription(opCtx));
    ASSERT_DOES_NOT_THROW(csr.checkShardVersionOrThrow(opCtx));

    // After completing the ShardingState recovery as a ClusterRole::ShardServer,
    // CollectionShardingRuntime will throw StaleConfig because the metadata needs to be recovered.
    ShardingState::RecoveredClusterRole rcr;
    rcr.role = ClusterRole::ShardServer;
    ShardingState::get(opCtx)->setRecoveryCompleted(rcr);
    ASSERT_THROWS_CODE(csr.getCollectionDescription(opCtx), DBException, ErrorCodes::StaleConfig);
    ASSERT_THROWS_CODE(csr.checkShardVersionOrThrow(opCtx), DBException, ErrorCodes::StaleConfig);
}

}  // namespace mongo
