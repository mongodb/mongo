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

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/vector_clock.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog_cache_loader_mock.h"
#include "mongo/util/fail_point.h"

#include "mongo/db/s/sharding_index_catalog_util.h"

namespace mongo {
namespace {

const NamespaceString kTestNss("TestDB", "TestColl");
const std::string kShardKey = "_id";
const BSONObj kShardKeyPattern = BSON(kShardKey << 1);

class CollectionShardingRuntimeTest : public ShardServerTestFixture {
protected:
    static CollectionMetadata makeShardedMetadata(OperationContext* opCtx,
                                                  UUID uuid = UUID::gen()) {
        const OID epoch = OID::gen();
        const Timestamp timestamp(1, 1);
        auto range = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY));
        auto chunk = ChunkType(
            uuid, std::move(range), ChunkVersion({epoch, timestamp}, {1, 0}), ShardId("other"));
        ChunkManager cm(ShardId("0"),
                        DatabaseVersion(UUID::gen(), timestamp),
                        makeStandaloneRoutingTableHistory(
                            RoutingTableHistory::makeNew(kTestNss,
                                                         uuid,
                                                         kShardKeyPattern,
                                                         nullptr,
                                                         false,
                                                         epoch,
                                                         timestamp,
                                                         boost::none /* timeseriesFields */,
                                                         boost::none,
                                                         boost::none /* chunkSizeBytes */,
                                                         true,
                                                         {std::move(chunk)})),
                        boost::none);

        return CollectionMetadata(std::move(cm), ShardId("0"));
    }
};

TEST_F(CollectionShardingRuntimeTest,
       GetCollectionDescriptionThrowsStaleConfigBeforeSetFilteringMetadataIsCalledAndNoOSSSet) {
    OperationContext* opCtx = operationContext();
    CollectionShardingRuntime csr(getServiceContext(), kTestNss, executor());
    ASSERT_FALSE(csr.getCollectionDescription(opCtx).isSharded());
    auto metadata = makeShardedMetadata(opCtx);
    ScopedSetShardRole scopedSetShardRole{opCtx,
                                          kTestNss,
                                          ShardVersion(metadata.getShardVersion()),
                                          boost::none /* databaseVersion */};
    ASSERT_THROWS_CODE(csr.getCollectionDescription(opCtx), DBException, ErrorCodes::StaleConfig);
}

TEST_F(
    CollectionShardingRuntimeTest,
    GetCollectionDescriptionReturnsUnshardedAfterSetFilteringMetadataIsCalledWithUnshardedMetadata) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss, executor());
    csr.setFilteringMetadata(operationContext(), CollectionMetadata());
    ASSERT_FALSE(csr.getCollectionDescription(operationContext()).isSharded());
}

TEST_F(CollectionShardingRuntimeTest,
       GetCollectionDescriptionReturnsShardedAfterSetFilteringMetadataIsCalledWithShardedMetadata) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss, executor());
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);
    ScopedSetShardRole scopedSetShardRole{opCtx,
                                          kTestNss,
                                          ShardVersion(metadata.getShardVersion()),
                                          boost::none /* databaseVersion */};
    ASSERT_TRUE(csr.getCollectionDescription(opCtx).isSharded());
}

TEST_F(CollectionShardingRuntimeTest,
       GetCurrentMetadataIfKnownReturnsNoneBeforeSetFilteringMetadataIsCalled) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss, executor());
    ASSERT_FALSE(csr.getCurrentMetadataIfKnown());
}

TEST_F(
    CollectionShardingRuntimeTest,
    GetCurrentMetadataIfKnownReturnsUnshardedAfterSetFilteringMetadataIsCalledWithUnshardedMetadata) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss, executor());
    csr.setFilteringMetadata(operationContext(), CollectionMetadata());
    const auto optCurrMetadata = csr.getCurrentMetadataIfKnown();
    ASSERT_TRUE(optCurrMetadata);
    ASSERT_FALSE(optCurrMetadata->isSharded());
    ASSERT_EQ(optCurrMetadata->getShardVersion(), ChunkVersion::UNSHARDED());
}

TEST_F(
    CollectionShardingRuntimeTest,
    GetCurrentMetadataIfKnownReturnsShardedAfterSetFilteringMetadataIsCalledWithShardedMetadata) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss, executor());
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);
    const auto optCurrMetadata = csr.getCurrentMetadataIfKnown();
    ASSERT_TRUE(optCurrMetadata);
    ASSERT_TRUE(optCurrMetadata->isSharded());
    ASSERT_EQ(optCurrMetadata->getShardVersion(), metadata.getShardVersion());
}

TEST_F(CollectionShardingRuntimeTest,
       GetCurrentMetadataIfKnownReturnsNoneAfterClearFilteringMetadataIsCalled) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss, executor());
    OperationContext* opCtx = operationContext();
    csr.setFilteringMetadata(opCtx, makeShardedMetadata(opCtx));
    csr.clearFilteringMetadata(opCtx);
    ASSERT_FALSE(csr.getCurrentMetadataIfKnown());
}

TEST_F(CollectionShardingRuntimeTest, SetFilteringMetadataWithSameUUIDKeepsSameMetadataManager) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss, executor());
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
    CollectionShardingRuntime csr(getServiceContext(), kTestNss, executor());
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);
    ScopedSetShardRole scopedSetShardRole{opCtx,
                                          kTestNss,
                                          ShardVersion(metadata.getShardVersion()),
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
    const NamespaceString testNss("TestDBForServerless", "TestColl");
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

    CollectionShardingRuntime csr(getServiceContext(), testNss, executor());
    auto collectionFilter = csr.getOwnershipFilter(
        opCtx, CollectionShardingRuntime::OrphanCleanupPolicy::kAllowOrphanCleanup, true);
    ASSERT_FALSE(collectionFilter.isSharded());
    ASSERT_FALSE(csr.getCurrentMetadataIfKnown()->isSharded());
    ASSERT_FALSE(csr.getCollectionDescription(opCtx).isSharded());

    // Enable sharding state and set shard version on the OSS for logical session nss.
    ScopedSetShardRole scopedSetShardRole2{
        opCtx,
        NamespaceString::kLogicalSessionsNamespace,
        ShardVersion(ChunkVersion({OID::gen(), Timestamp(1, 1)}, {1, 0})), /* shardVersion */
        boost::none                                                        /* databaseVersion */
    };

    CollectionShardingRuntime csrLogicalSession(
        getServiceContext(), NamespaceString::kLogicalSessionsNamespace, executor());
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

class CollectionShardingRuntimeTestWithMockedLoader : public ShardServerTestFixture {
public:
    const NamespaceString kNss{"test.foo"};
    const UUID kCollUUID = UUID::gen();
    const std::string kShardKey = "x";
    const HostAndPort kConfigHostAndPort{"DummyConfig", 12345};
    const std::vector<ShardType> kShardList = {ShardType("shard0", "Host0:12345")};

    void setUp() override {
        // Don't call ShardServerTestFixture::setUp so we can install a mock catalog cache
        // loader.
        ShardingMongodTestFixture::setUp();

        replicationCoordinator()->alwaysAllowWrites(true);
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;

        _clusterId = OID::gen();
        ShardingState::get(getServiceContext())
            ->setInitialized(kShardList[0].getName(), _clusterId);

        auto mockLoader = std::make_unique<CatalogCacheLoaderMock>();
        _mockCatalogCacheLoader = mockLoader.get();
        CatalogCacheLoader::set(getServiceContext(), std::move(mockLoader));

        uassertStatusOK(
            initializeGlobalShardingStateForMongodForTest(ConnectionString(kConfigHostAndPort)));

        configTargeterMock()->setFindHostReturnValue(kConfigHostAndPort);

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

        ShardServerTestFixture::tearDown();
    }

    class StaticCatalogClient final : public ShardingCatalogClientMock {
    public:
        StaticCatalogClient(std::vector<ShardType> shards) : _shards(std::move(shards)) {}

        StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
            OperationContext* opCtx, repl::ReadConcernLevel readConcern) override {
            return repl::OpTimeWith<std::vector<ShardType>>(_shards);
        }

        std::vector<CollectionType> getCollections(
            OperationContext* opCtx,
            StringData dbName,
            repl::ReadConcernLevel readConcernLevel) override {
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

protected:
    CatalogCacheLoaderMock* _mockCatalogCacheLoader;
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
        replCoord->setAwaitReplicationReturnValueFunction([this](OperationContext* opCtx,
                                                                 const repl::OpTime& opTime) {
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
    }

    void tearDown() override {
        DBDirectClient client(operationContext());
        client.dropCollection(kTestNss.ns());

        WaitForMajorityService::get(getServiceContext()).shutDown();
        CollectionShardingRuntimeTest::tearDown();
    }

    // Creates the CSR if it does not exist and stashes it in the CollectionShardingStateMap. This
    // is required for waitForClean tests which use CollectionShardingRuntime::get().
    CollectionShardingRuntime& csr() {
        AutoGetCollection autoColl(operationContext(), kTestNss, MODE_IX);
        auto* css = CollectionShardingState::get(operationContext(), kTestNss);
        return *checked_cast<CollectionShardingRuntime*>(css);
    }

    const UUID& uuid() const {
        return _uuid;
    }

private:
    UUID _uuid{UUID::gen()};
};

// The 'pending' field must not be set in order for a range deletion task to succeed, but the
// ShardServerOpObserver will submit the task for deletion upon seeing an insert without the
// 'pending' field. The tests call removeDocumentsFromRange directly, so we want to avoid having
// the op observer also submit the task. The ShardServerOpObserver will ignore replacement
//  updates on the range deletions namespace though, so we can get around the issue by inserting
// the task with the 'pending' field set, and then remove the field using a replacement update
// after.
RangeDeletionTask insertRangeDeletionTask(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const UUID& uuid,
                                          const ChunkRange& range,
                                          int64_t numOrphans) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    auto migrationId = UUID::gen();
    RangeDeletionTask t(migrationId, nss, uuid, ShardId("donor"), range, CleanWhenEnum::kDelayed);
    t.setPending(true);
    t.setNumOrphanDocs(numOrphans);
    const auto currentTime = VectorClock::get(opCtx)->getTime();
    t.setTimestamp(currentTime.clusterTime().asTimestamp());
    store.add(opCtx, t);

    auto query = BSON(RangeDeletionTask::kIdFieldName << migrationId);
    t.setPending(boost::none);
    auto update = t.toBSON();
    store.update(opCtx, query, update);

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
    csr().setFilteringMetadata(opCtx, metadata);
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
    csr().setFilteringMetadata(opCtx, metadata);

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
    OperationContext* opCtx = operationContext();

    auto metadata = makeShardedMetadata(opCtx, uuid());
    csr().setFilteringMetadata(opCtx, metadata);
    const ChunkRange range = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY));
    const auto task = insertRangeDeletionTask(opCtx, kTestNss, uuid(), range, 0);

    auto cleanupComplete =
        csr().cleanUpRange(range, task.getId(), CollectionShardingRuntime::CleanWhen::kNow);

    opCtx->setDeadlineAfterNowBy(Milliseconds(100), ErrorCodes::MaxTimeMSExpired);
    auto status =
        CollectionShardingRuntime::waitForClean(opCtx, kTestNss, uuid(), range, Date_t::max());

    ASSERT_EQ(status.code(), ErrorCodes::MaxTimeMSExpired);

    globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::off);
    cleanupComplete.get();
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanBlocksBehindAllScheduledDeletions) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx, uuid());
    csr().setFilteringMetadata(opCtx, metadata);

    const auto middleKey = 5;
    const ChunkRange range1 = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << middleKey));
    const auto task1 = insertRangeDeletionTask(opCtx, kTestNss, uuid(), range1, 0);
    const ChunkRange range2 = ChunkRange(BSON(kShardKey << middleKey), BSON(kShardKey << MAXKEY));
    const auto task2 = insertRangeDeletionTask(opCtx, kTestNss, uuid(), range2, 0);

    auto cleanupCompleteFirst =
        csr().cleanUpRange(range1, task1.getId(), CollectionShardingRuntime::CleanWhen::kNow);

    auto cleanupCompleteSecond =
        csr().cleanUpRange(range2, task2.getId(), CollectionShardingRuntime::CleanWhen::kNow);

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
    csr().setFilteringMetadata(opCtx, metadata);
    const ChunkRange range = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY));
    const auto task = insertRangeDeletionTask(opCtx, kTestNss, uuid(), range, 0);

    auto cleanupComplete =
        csr().cleanUpRange(range, task.getId(), CollectionShardingRuntime::CleanWhen::kNow);

    auto status =
        CollectionShardingRuntime::waitForClean(opCtx, kTestNss, uuid(), range, Date_t::max());

    ASSERT_OK(status);
    ASSERT(cleanupComplete.isReady());
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest, IncreaseIndexVersionAsParticipant) {
    OperationContext* opCtx = operationContext();
    Timestamp indexVersion(1, 0);

    // Simulate the commit of the index by writing in the config.shard.collections collection.
    write_ops::UpdateCommandRequest updateCollectionOp(
        NamespaceString::kShardCollectionCatalogNamespace);
    updateCollectionOp.setUpdates({[&] {
        write_ops::UpdateOpEntry entry;
        entry.setQ(BSON(CollectionType::kNssFieldName << kTestNss.toString()
                                                      << CollectionType::kUuidFieldName << uuid()));
        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$set" << BSON(CollectionType::kIndexVersionFieldName << indexVersion))));
        entry.setUpsert(true);
        entry.setMulti(false);
        return entry;
    }()});
    updateCollectionOp.setWriteCommandRequestBase([] {
        write_ops::WriteCommandRequestBase wcb;
        wcb.setStmtId(1);
        return wcb;
    }());

    DBDirectClient client(opCtx);
    write_ops::checkWriteErrors(client.update(updateCollectionOp));

    ASSERT_EQ(indexVersion, csr().getIndexVersion(opCtx).value());
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanCorrectEvenAfterClearFollowedBySetFilteringMetadata) {
    globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::alwaysOn);
    ScopeGuard resetFailPoint(
        [=] { globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::off); });

    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx, uuid());
    csr().setFilteringMetadata(opCtx, metadata);
    const ChunkRange range = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY));
    const auto task = insertRangeDeletionTask(opCtx, kTestNss, uuid(), range, 0);

    // Schedule range deletion that will hang due to `suspendRangeDeletion` failpoint
    auto cleanupComplete =
        csr().cleanUpRange(range, task.getId(), CollectionShardingRuntime::CleanWhen::kNow);

    // Clear and set again filtering metadata
    csr().clearFilteringMetadata(opCtx);
    csr().setFilteringMetadata(opCtx, metadata);

    auto waitForCleanUp = [&](Date_t timeout) {
        return CollectionShardingRuntime::waitForClean(opCtx, kTestNss, uuid(), range, timeout);
    };

    // Check that the hanging range deletion is still tracked even following a clear of the metadata
    auto status = waitForCleanUp(Date_t::now() + Milliseconds(100));
    ASSERT_NOT_OK(status);
    ASSERT(!cleanupComplete.isReady());

    globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::off);
    resetFailPoint.dismiss();

    // Check that the range deletion is not tracked anymore after it succeeds
    status = waitForCleanUp(Date_t::max());
    ASSERT_OK(status);
    ASSERT(cleanupComplete.isReady());
}

}  // namespace
}  // namespace mongo
