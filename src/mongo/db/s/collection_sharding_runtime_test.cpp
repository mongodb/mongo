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

#include <boost/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <chrono>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/range_deleter_service_test.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/sharding_index_catalog_ddl_util.h"
#include "mongo/db/s/sharding_mongod_test_fixture.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/vector_clock.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/catalog_cache_loader_mock.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/sharding_state.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace {

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const std::string kShardKey = "_id";
const BSONObj kShardKeyPattern = BSON(kShardKey << 1);

class CollectionShardingRuntimeTest : public ShardServerTestFixture {
protected:
    static CollectionMetadata makeShardedMetadata(OperationContext* opCtx,
                                                  UUID uuid = UUID::gen()) {
        const OID epoch = OID::gen();
        const Timestamp timestamp(Date_t::now());

        // Sleeping some time here to guarantee that any upcoming call to this function generates a
        // different timestamp
        stdx::this_thread::sleep_for(stdx::chrono::milliseconds(10));

        auto range = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY));
        auto chunk = ChunkType(
            uuid, std::move(range), ChunkVersion({epoch, timestamp}, {1, 0}), ShardId("other"));
        ChunkManager cm(ShardId("0"),
                        DatabaseVersion(UUID::gen(), timestamp),
                        makeStandaloneRoutingTableHistory(
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

        return CollectionMetadata(std::move(cm), ShardId("0"));
    }
};

TEST_F(CollectionShardingRuntimeTest,
       GetCollectionDescriptionThrowsStaleConfigBeforeSetFilteringMetadataIsCalledAndNoOSSSet) {
    OperationContext* opCtx = operationContext();
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    ASSERT_FALSE(csr.getCollectionDescription(opCtx).isSharded());
    auto metadata = makeShardedMetadata(opCtx);
    ScopedSetShardRole scopedSetShardRole{
        opCtx,
        kTestNss,
        ShardVersionFactory::make(metadata, boost::optional<CollectionIndexes>(boost::none)),
        boost::none /* databaseVersion */};
    ASSERT_THROWS_CODE(csr.getCollectionDescription(opCtx), DBException, ErrorCodes::StaleConfig);
}

TEST_F(
    CollectionShardingRuntimeTest,
    GetCollectionDescriptionReturnsUnshardedAfterSetFilteringMetadataIsCalledWithUnshardedMetadata) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    csr.setFilteringMetadata(operationContext(), CollectionMetadata());
    ASSERT_FALSE(csr.getCollectionDescription(operationContext()).isSharded());
}

TEST_F(CollectionShardingRuntimeTest,
       GetCollectionDescriptionReturnsShardedAfterSetFilteringMetadataIsCalledWithShardedMetadata) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);
    ScopedSetShardRole scopedSetShardRole{
        opCtx,
        kTestNss,
        ShardVersionFactory::make(metadata, boost::optional<CollectionIndexes>(boost::none)),
        boost::none /* databaseVersion */};
    ASSERT_TRUE(csr.getCollectionDescription(opCtx).isSharded());
}

TEST_F(CollectionShardingRuntimeTest,
       GetCurrentMetadataIfKnownReturnsNoneBeforeSetFilteringMetadataIsCalled) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    ASSERT_FALSE(csr.getCurrentMetadataIfKnown());
}

TEST_F(
    CollectionShardingRuntimeTest,
    GetCurrentMetadataIfKnownReturnsUnshardedAfterSetFilteringMetadataIsCalledWithUnshardedMetadata) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    csr.setFilteringMetadata(operationContext(), CollectionMetadata());
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
    ASSERT_EQ(csr.getNumMetadataManagerChanges_forTest(), 0);
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);
    // Should create a new MetadataManager object, bumping the count to 1.
    ASSERT_EQ(csr.getNumMetadataManagerChanges_forTest(), 1);
    // Set it again.
    csr.setFilteringMetadata(opCtx, metadata);
    // Should not have reset metadata, so the counter should still be 1.
    ASSERT_EQ(csr.getNumMetadataManagerChanges_forTest(), 1);
}

TEST_F(CollectionShardingRuntimeTest,
       SetFilteringMetadataWithDifferentUUIDReplacesPreviousMetadataManager) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);
    ScopedSetShardRole scopedSetShardRole{
        opCtx,
        kTestNss,
        ShardVersionFactory::make(metadata, boost::optional<CollectionIndexes>(boost::none)),
        boost::none /* databaseVersion */};
    ASSERT_EQ(csr.getNumMetadataManagerChanges_forTest(), 1);

    // Set it again with a different metadata object (UUID is generated randomly in
    // makeShardedMetadata()).
    auto newMetadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, newMetadata);

    ASSERT_EQ(csr.getNumMetadataManagerChanges_forTest(), 2);
    ASSERT(
        csr.getCollectionDescription(opCtx).uuidMatches(newMetadata.getChunkManager()->getUUID()));
}

TEST_F(CollectionShardingRuntimeTest, ReturnUnshardedMetadataInServerlessMode) {
    const NamespaceString testNss =
        NamespaceString::createNamespaceString_forTest("TestDBForServerless", "TestColl");
    OperationContext* opCtx = operationContext();

    // Enable serverless mode in global settings.
    repl::ReplSettings severlessRs;
    severlessRs.setServerlessMode();
    repl::ReplSettings originalRs = getGlobalReplSettings();
    setGlobalReplSettings(severlessRs);
    ASSERT_TRUE(getGlobalReplSettings().isServerless());

    // Enable sharding state and set shard version on the OSS for testNss.
    ScopedSetShardRole scopedSetShardRole1{
        opCtx,
        testNss,
        ShardVersion::UNSHARDED(), /* shardVersion */
        boost::none                /* databaseVersion */
    };

    CollectionShardingRuntime csr(getServiceContext(), testNss);
    auto collectionFilter = csr.getOwnershipFilter(
        opCtx, CollectionShardingRuntime::OrphanCleanupPolicy::kAllowOrphanCleanup, true);
    ASSERT_FALSE(collectionFilter.isSharded());
    ASSERT_FALSE(csr.getCurrentMetadataIfKnown()->isSharded());
    ASSERT_FALSE(csr.getCollectionDescription(opCtx).isSharded());

    // Enable sharding state and set shard version on the OSS for logical session nss.
    CollectionGeneration gen{OID::gen(), Timestamp(1, 1)};
    ScopedSetShardRole scopedSetShardRole2{
        opCtx,
        NamespaceString::kLogicalSessionsNamespace,
        ShardVersionFactory::make(
            ChunkVersion(gen, {1, 0}),
            boost::optional<CollectionIndexes>(boost::none)), /* shardVersion */
        boost::none                                           /* databaseVersion */
    };

    CollectionShardingRuntime csrLogicalSession(getServiceContext(),
                                                NamespaceString::kLogicalSessionsNamespace);
    ASSERT(csrLogicalSession.getCurrentMetadataIfKnown() == boost::none);
    ASSERT_THROWS_CODE(
        csrLogicalSession.getCollectionDescription(opCtx), DBException, ErrorCodes::StaleConfig);
    ASSERT_THROWS_CODE(
        csrLogicalSession.getOwnershipFilter(
            opCtx, CollectionShardingRuntime::OrphanCleanupPolicy::kAllowOrphanCleanup, true),
        DBException,
        ErrorCodes::StaleConfig);

    // Reset the global settings.
    setGlobalReplSettings(originalRs);
}

TEST_F(CollectionShardingRuntimeTest, ShardVersionCheckDetectsClusterTimeConflicts) {
    OperationContext* opCtx = operationContext();
    CollectionShardingRuntime csr(getServiceContext(), kTestNss);
    const auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);

    const auto collectionTimestamp = metadata.getShardPlacementVersion().getTimestamp();

    auto receivedShardVersion =
        ShardVersionFactory::make(metadata, boost::optional<CollectionIndexes>(boost::none));

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

        StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
            OperationContext* opCtx,
            repl::ReadConcernLevel readConcern,
            bool excludeDraining) override {
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
class CollectionShardingRuntimeWithRangeDeleterTest : public CollectionShardingRuntimeTest {
public:
    void setUp() override {
        CollectionShardingRuntimeTest::setUp();
        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());
        // Set up replication coordinator to be primary and have no replication delay.
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext());
        replCoord->setCanAcceptNonLocalWrites(true);
        std::ignore = replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY);
        // Make waitForWriteConcern return immediately.
        replCoord->setAwaitReplicationReturnValueFunction([this](const repl::OpTime& opTime) {
            return repl::ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0));
        });
        repl::ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));

        {
            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(operationContext());
            uassertStatusOK(createCollection(
                operationContext(), kTestNss.dbName(), BSON("create" << kTestNss.coll())));
        }

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

TEST_F(CollectionShardingRuntimeWithCatalogTest, TestShardingIndexesCatalogCache) {
    OperationContext* opCtx = operationContext();

    ASSERT_EQ(false, csr()->getIndexes(opCtx).is_initialized());

    Timestamp indexVersion(1, 0);
    addShardingIndexCatalogEntryToCollection(
        opCtx, kTestNss, "x_1", BSON("x" << 1), BSONObj(), uuid(), indexVersion, boost::none);

    ASSERT_EQ(true, csr()->getIndexes(opCtx).is_initialized());
    ASSERT_EQ(CollectionIndexes(uuid(), indexVersion), *csr()->getCollectionIndexes(opCtx));
}
}  // namespace
}  // namespace mongo
