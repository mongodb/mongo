/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

void createTestCollection(OperationContext* opCtx, const NamespaceString& nss) {
    uassertStatusOK(createCollection(opCtx, nss.dbName(), BSON("create" << nss.coll())));
}

void createTestView(OperationContext* opCtx,
                    const NamespaceString& nss,
                    const NamespaceString& viewOn,
                    const std::vector<BSONObj>& pipeline) {
    uassertStatusOK(createCollection(
        opCtx,
        nss.dbName(),
        BSON("create" << nss.coll() << "viewOn" << viewOn.coll() << "pipeline" << pipeline)));
}

void installDatabaseMetadata(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             const DatabaseVersion& dbVersion) {
    AutoGetDb autoDb(opCtx, dbName, MODE_X, {}, {});
    auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx, dbName);
    scopedDss->setDbInfo(opCtx, {dbName.db(), ShardId("this"), dbVersion});
}

void installUnshardedCollectionMetadata(OperationContext* opCtx, const NamespaceString& nss) {
    const auto unshardedCollectionMetadata = CollectionMetadata();
    AutoGetCollection coll(opCtx, nss, MODE_IX);
    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss)
        ->setFilteringMetadata(opCtx, unshardedCollectionMetadata);
}

void installShardedCollectionMetadata(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const DatabaseVersion& dbVersion,
                                      std::vector<ChunkType> chunks,
                                      ShardId thisShardId) {
    ASSERT(!chunks.empty());

    const auto uuid = [&] {
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        return autoColl.getCollection()->uuid();
    }();

    const std::string shardKey("skey");
    const ShardKeyPattern shardKeyPattern{BSON(shardKey << 1)};
    const auto epoch = chunks.front().getVersion().epoch();
    const auto timestamp = chunks.front().getVersion().getTimestamp();

    auto rt = RoutingTableHistory::makeNew(nss,
                                           uuid,
                                           shardKeyPattern.getKeyPattern(),
                                           nullptr,
                                           false,
                                           epoch,
                                           timestamp,
                                           boost::none /* timeseriesFields */,
                                           boost::none /* resharding Fields */,
                                           true /* allowMigrations */,
                                           chunks);

    const auto version = rt.getVersion();
    const auto rtHandle =
        RoutingTableHistoryValueHandle(std::make_shared<RoutingTableHistory>(std::move(rt)),
                                       ComparableChunkVersion::makeComparableChunkVersion(version));

    const auto collectionMetadata = CollectionMetadata(
        ChunkManager(thisShardId, dbVersion, rtHandle, boost::none), thisShardId);

    AutoGetCollection coll(opCtx, nss, MODE_IX);
    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss)
        ->setFilteringMetadata(opCtx, collectionMetadata);
}

UUID getCollectionUUID(OperationContext* opCtx, const NamespaceString& nss) {
    const auto optUuid = CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, nss);
    ASSERT(optUuid);
    return *optUuid;
}

class ShardRoleTest : public ServiceContextMongoDTest {
protected:
    OperationContext* opCtx() {
        return _opCtx.get();
    }

    void setUp() override;
    void tearDown() override;

    const ShardId thisShardId{"this"};

    const DatabaseName dbNameTestDb{"test"};
    const DatabaseVersion dbVersionTestDb{UUID::gen(), Timestamp(1, 0)};

    const NamespaceString nssUnshardedCollection1 =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "unsharded");

    const NamespaceString nssShardedCollection1 =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "sharded");
    const ShardVersion shardVersionShardedCollection1 = ShardVersionFactory::make(
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(5, 0)}, CollectionPlacement(10, 1)),
        boost::optional<CollectionIndexes>(boost::none));

    const NamespaceString nssView =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "view");
    const std::vector<BSONObj> viewPipeline = {BSON("$match" << BSON("x" << 1))};

    // Workaround to be able to write parametrized TEST_F
    void testRestoreFailsIfCollectionNoLongerExists(
        AcquisitionPrerequisites::OperationType operationType);
    void testRestoreFailsIfCollectionBecomesCreated(
        AcquisitionPrerequisites::OperationType operationType);
    void testRestoreFailsIfCollectionRenamed(AcquisitionPrerequisites::OperationType operationType);
    void testRestoreFailsIfCollectionDroppedAndRecreated(
        AcquisitionPrerequisites::OperationType operationType);
    void testRestoreFailsIfCollectionIsNowAView(
        AcquisitionPrerequisites::OperationType operationType);

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

void ShardRoleTest::setUp() {
    ServiceContextMongoDTest::setUp();
    _opCtx = getGlobalServiceContext()->makeOperationContext(&cc());
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;

    const repl::ReplSettings replSettings = {};
    repl::ReplicationCoordinator::set(
        getGlobalServiceContext(),
        std::unique_ptr<repl::ReplicationCoordinator>(
            new repl::ReplicationCoordinatorMock(_opCtx->getServiceContext(), replSettings)));
    ASSERT_OK(repl::ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(repl::MemberState::RS_PRIMARY));

    repl::createOplog(_opCtx.get());

    ShardingState::get(getServiceContext())->setInitialized(ShardId("this"), OID::gen());

    // Setup test collections and metadata
    installDatabaseMetadata(opCtx(), dbNameTestDb, dbVersionTestDb);

    // Create nssUnshardedCollection1
    createTestCollection(opCtx(), nssUnshardedCollection1);
    installUnshardedCollectionMetadata(opCtx(), nssUnshardedCollection1);

    // Create nssShardedCollection1
    createTestCollection(opCtx(), nssShardedCollection1);
    const auto uuidShardedCollection1 = getCollectionUUID(_opCtx.get(), nssShardedCollection1);
    installShardedCollectionMetadata(
        opCtx(),
        nssShardedCollection1,
        dbVersionTestDb,
        {ChunkType(uuidShardedCollection1,
                   ChunkRange{BSON("skey" << MINKEY), BSON("skey" << MAXKEY)},
                   shardVersionShardedCollection1.placementVersion(),
                   thisShardId)},
        thisShardId);

    // Setup nssView
    createTestView(opCtx(), nssView, nssUnshardedCollection1, viewPipeline);
}

void ShardRoleTest::tearDown() {
    _opCtx.reset();
    ServiceContextMongoDTest::tearDown();
    repl::ReplicationCoordinator::set(getGlobalServiceContext(), nullptr);
}

TEST_F(ShardRoleTest, NamespaceOrViewAcquisitionRequestWithOpCtxTakesPlacementFromOSS) {
    const auto nss = nssUnshardedCollection1;

    {
        auto acquisition =
            CollectionAcquisitionRequest::fromOpCtx(opCtx(), nss, AcquisitionPrerequisites::kWrite);
        ASSERT_EQ(boost::none, acquisition.placementConcern.dbVersion);
        ASSERT_EQ(boost::none, acquisition.placementConcern.shardVersion);
    }

    {
        const NamespaceString anotherCollection =
            NamespaceString::createNamespaceString_forTest("test2.foo");
        ScopedSetShardRole setShardRole(
            opCtx(), anotherCollection, ShardVersion::UNSHARDED(), dbVersionTestDb);
        auto acquisition = CollectionOrViewAcquisitionRequest::fromOpCtx(
            opCtx(), nss, AcquisitionPrerequisites::kWrite);
        ASSERT_EQ(boost::none, acquisition.placementConcern.dbVersion);
        ASSERT_EQ(boost::none, acquisition.placementConcern.shardVersion);
    }

    {
        const auto dbVersion = boost::none;
        const auto shardVersion = boost::none;
        ScopedSetShardRole setShardRole(opCtx(), nss, shardVersion, dbVersion);
        auto acquisition = CollectionOrViewAcquisitionRequest::fromOpCtx(
            opCtx(), nss, AcquisitionPrerequisites::kWrite);
        ASSERT_EQ(dbVersion, acquisition.placementConcern.dbVersion);
        ASSERT_EQ(shardVersion, acquisition.placementConcern.shardVersion);
    }

    {
        const auto dbVersion = dbVersionTestDb;
        const auto shardVersion = ShardVersion::UNSHARDED();
        ScopedSetShardRole setShardRole(opCtx(), nss, shardVersion, dbVersion);
        auto acquisition = CollectionOrViewAcquisitionRequest::fromOpCtx(
            opCtx(), nss, AcquisitionPrerequisites::kWrite);
        ASSERT_EQ(dbVersion, acquisition.placementConcern.dbVersion);
        ASSERT_EQ(shardVersion, acquisition.placementConcern.shardVersion);
    }

    {
        const auto dbVersion = boost::none;
        const auto shardVersion = shardVersionShardedCollection1;
        ScopedSetShardRole setShardRole(opCtx(), nss, shardVersion, dbVersion);
        auto acquisition = CollectionOrViewAcquisitionRequest::fromOpCtx(
            opCtx(), nss, AcquisitionPrerequisites::kWrite);
        ASSERT_EQ(dbVersion, acquisition.placementConcern.dbVersion);
        ASSERT_EQ(shardVersion, acquisition.placementConcern.shardVersion);
    }
}

// ---------------------------------------------------------------------------
// Placement checks when acquiring unsharded collections

TEST_F(ShardRoleTest, AcquireUnshardedCollWithCorrectPlacementVersion) {
    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    const auto acquisition = acquireCollection(opCtx(),
                                               {nssUnshardedCollection1,
                                                placementConcern,
                                                repl::ReadConcernArgs(),
                                                AcquisitionPrerequisites::kWrite},
                                               MODE_IX);

    ASSERT_EQ(nssUnshardedCollection1, acquisition.nss());
    ASSERT_EQ(nssUnshardedCollection1, acquisition.getCollectionPtr()->ns());
    ASSERT_FALSE(acquisition.getShardingDescription().isSharded());
    ASSERT_FALSE(acquisition.getShardingFilter().has_value());
}

TEST_F(ShardRoleTest, AcquireUnshardedCollWithIncorrectPlacementVersionThrows) {
    const auto incorrectDbVersion = DatabaseVersion(UUID::gen(), Timestamp(50, 0));

    PlacementConcern placementConcern{incorrectDbVersion, ShardVersion::UNSHARDED()};
    ASSERT_THROWS_WITH_CHECK(acquireCollection(opCtx(),
                                               {
                                                   nssUnshardedCollection1,
                                                   placementConcern,
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kWrite,
                                               },
                                               MODE_IX),
                             ExceptionFor<ErrorCodes::StaleDbVersion>,
                             [&](const DBException& ex) {
                                 const auto exInfo = ex.extraInfo<StaleDbRoutingVersion>();
                                 ASSERT_EQ(dbNameTestDb.db(), exInfo->getDb());
                                 ASSERT_EQ(incorrectDbVersion, exInfo->getVersionReceived());
                                 ASSERT_EQ(dbVersionTestDb, exInfo->getVersionWanted());
                                 ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
                             });
}

TEST_F(ShardRoleTest, AcquireUnshardedCollWhenShardDoesNotKnowThePlacementVersionThrows) {
    {
        // Clear the database metadata
        AutoGetDb autoDb(opCtx(), dbNameTestDb, MODE_X, {}, {});
        auto scopedDss =
            DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx(), dbNameTestDb);
        scopedDss->clearDbInfo(opCtx());
    }

    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    ASSERT_THROWS_WITH_CHECK(acquireCollection(opCtx(),
                                               {nssUnshardedCollection1,
                                                placementConcern,
                                                repl::ReadConcernArgs(),
                                                AcquisitionPrerequisites::kWrite},
                                               MODE_IX),
                             ExceptionFor<ErrorCodes::StaleDbVersion>,
                             [&](const DBException& ex) {
                                 const auto exInfo = ex.extraInfo<StaleDbRoutingVersion>();
                                 ASSERT_EQ(dbNameTestDb.db(), exInfo->getDb());
                                 ASSERT_EQ(dbVersionTestDb, exInfo->getVersionReceived());
                                 ASSERT_EQ(boost::none, exInfo->getVersionWanted());
                                 ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
                             });
}

TEST_F(ShardRoleTest, AcquireUnshardedCollWhenCriticalSectionIsActiveThrows) {
    const BSONObj criticalSectionReason = BSON("reason" << 1);
    {
        // Enter critical section.
        AutoGetDb autoDb(opCtx(), dbNameTestDb, MODE_X, {}, {});
        auto scopedDss =
            DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx(), dbNameTestDb);
        scopedDss->enterCriticalSectionCatchUpPhase(opCtx(), criticalSectionReason);
        scopedDss->enterCriticalSectionCommitPhase(opCtx(), criticalSectionReason);
    }

    {
        PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
        ASSERT_THROWS_WITH_CHECK(acquireCollection(opCtx(),
                                                   {nssUnshardedCollection1,
                                                    placementConcern,
                                                    repl::ReadConcernArgs(),
                                                    AcquisitionPrerequisites::kWrite},
                                                   MODE_IX),
                                 ExceptionFor<ErrorCodes::StaleDbVersion>,
                                 [&](const DBException& ex) {
                                     const auto exInfo = ex.extraInfo<StaleDbRoutingVersion>();
                                     ASSERT_EQ(dbNameTestDb.db(), exInfo->getDb());
                                     ASSERT_EQ(dbVersionTestDb, exInfo->getVersionReceived());
                                     ASSERT_EQ(boost::none, exInfo->getVersionWanted());
                                     ASSERT_TRUE(
                                         exInfo->getCriticalSectionSignal().is_initialized());
                                 });
    }

    {
        // Exit critical section.
        AutoGetDb autoDb(opCtx(), dbNameTestDb, MODE_X, {}, {});
        const BSONObj criticalSectionReason = BSON("reason" << 1);
        auto scopedDss =
            DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx(), dbNameTestDb);
        scopedDss->exitCriticalSection(opCtx(), criticalSectionReason);
    }
}

TEST_F(ShardRoleTest, AcquireUnshardedCollWithoutSpecifyingPlacementVersion) {
    const auto acquisition =
        acquireCollection(opCtx(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              opCtx(), nssUnshardedCollection1, AcquisitionPrerequisites::kWrite),
                          MODE_IX);

    ASSERT_EQ(nssUnshardedCollection1, acquisition.nss());
    ASSERT_EQ(nssUnshardedCollection1, acquisition.getCollectionPtr()->ns());
    ASSERT_FALSE(acquisition.getShardingDescription().isSharded());
    ASSERT_FALSE(acquisition.getShardingFilter().has_value());
}

TEST_F(ShardRoleTest, AcquireLocalCatalogOnlyWithPotentialDataLossUnsharded) {
    auto acquisition = acquireCollectionForLocalCatalogOnlyWithPotentialDataLoss(
        opCtx(), nssUnshardedCollection1, MODE_IX);

    ASSERT_EQ(nssUnshardedCollection1, acquisition.nss());
    ASSERT_EQ(nssUnshardedCollection1, acquisition.getCollectionPtr()->ns());
}

TEST_F(ShardRoleTest, AcquireLocalCatalogOnlyWithPotentialDataLossSharded) {
    auto acquisition = acquireCollectionForLocalCatalogOnlyWithPotentialDataLoss(
        opCtx(), nssShardedCollection1, MODE_IX);

    ASSERT_EQ(nssShardedCollection1, acquisition.nss());
    ASSERT_EQ(nssShardedCollection1, acquisition.getCollectionPtr()->ns());
}

DEATH_TEST_F(ShardRoleTest,
             AcquireLocalCatalogOnlyWithPotentialDataLossForbiddenToAccessDescription,
             "Invariant failure") {
    auto acquisition = acquireCollectionForLocalCatalogOnlyWithPotentialDataLoss(
        opCtx(), nssUnshardedCollection1, MODE_IX);

    (void)acquisition.getShardingDescription();
}

DEATH_TEST_F(ShardRoleTest,
             AcquireLocalCatalogOnlyWithPotentialDataLossForbiddenToAccessFilter,
             "Invariant failure") {
    auto acquisition = acquireCollectionForLocalCatalogOnlyWithPotentialDataLoss(
        opCtx(), nssUnshardedCollection1, MODE_IX);

    (void)acquisition.getShardingFilter();
}

// ---------------------------------------------------------------------------
// Placement checks when acquiring sharded collections

TEST_F(ShardRoleTest, AcquireShardedCollWithCorrectPlacementVersion) {
    PlacementConcern placementConcern{{}, shardVersionShardedCollection1};
    const auto acquisition = acquireCollection(opCtx(),
                                               {nssShardedCollection1,
                                                placementConcern,
                                                repl::ReadConcernArgs(),
                                                AcquisitionPrerequisites::kWrite},
                                               MODE_IX);

    ASSERT_EQ(nssShardedCollection1, acquisition.nss());
    ASSERT_EQ(nssShardedCollection1, acquisition.getCollectionPtr()->ns());
    ASSERT_TRUE(acquisition.getShardingDescription().isSharded());
    ASSERT_TRUE(acquisition.getShardingFilter().has_value());
}

TEST_F(ShardRoleTest, AcquireShardedCollWithIncorrectPlacementVersionThrows) {
    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    ASSERT_THROWS_WITH_CHECK(acquireCollection(opCtx(),
                                               {
                                                   nssShardedCollection1,
                                                   placementConcern,
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kWrite,
                                               },
                                               MODE_IX),
                             ExceptionFor<ErrorCodes::StaleConfig>,
                             [&](const DBException& ex) {
                                 const auto exInfo = ex.extraInfo<StaleConfigInfo>();
                                 ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
                                 ASSERT_EQ(ShardVersion::UNSHARDED(), exInfo->getVersionReceived());
                                 ASSERT_EQ(shardVersionShardedCollection1,
                                           exInfo->getVersionWanted());
                                 ASSERT_EQ(ShardId("this"), exInfo->getShardId());
                                 ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
                             });
}

TEST_F(ShardRoleTest, AcquireShardedCollWhenShardDoesNotKnowThePlacementVersionThrows) {
    {
        // Clear the collection filtering metadata on the shard.
        AutoGetCollection coll(opCtx(), nssShardedCollection1, MODE_IX);
        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx(),
                                                                             nssShardedCollection1)
            ->clearFilteringMetadata(opCtx());
    }

    PlacementConcern placementConcern{{}, shardVersionShardedCollection1};
    ASSERT_THROWS_WITH_CHECK(acquireCollection(opCtx(),
                                               {nssShardedCollection1,
                                                placementConcern,
                                                repl::ReadConcernArgs(),
                                                AcquisitionPrerequisites::kWrite},
                                               MODE_IX),
                             ExceptionFor<ErrorCodes::StaleConfig>,
                             [&](const DBException& ex) {
                                 const auto exInfo = ex.extraInfo<StaleConfigInfo>();
                                 ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
                                 ASSERT_EQ(shardVersionShardedCollection1,
                                           exInfo->getVersionReceived());
                                 ASSERT_EQ(boost::none, exInfo->getVersionWanted());
                                 ASSERT_EQ(ShardId("this"), exInfo->getShardId());
                                 ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
                             });
}

TEST_F(ShardRoleTest, AcquireShardedCollWhenCriticalSectionIsActiveThrows) {
    const BSONObj criticalSectionReason = BSON("reason" << 1);
    {
        // Enter the critical section.
        AutoGetCollection coll(opCtx(), nssShardedCollection1, MODE_X);
        const auto& csr = CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
            opCtx(), nssShardedCollection1);
        csr->enterCriticalSectionCatchUpPhase(criticalSectionReason);
        csr->enterCriticalSectionCommitPhase(criticalSectionReason);
    }

    {
        PlacementConcern placementConcern{{}, shardVersionShardedCollection1};
        ASSERT_THROWS_WITH_CHECK(
            acquireCollection(opCtx(),
                              {nssShardedCollection1,
                               placementConcern,
                               repl::ReadConcernArgs(),
                               AcquisitionPrerequisites::kWrite},
                              MODE_IX),
            ExceptionFor<ErrorCodes::StaleConfig>,
            [&](const DBException& ex) {
                const auto exInfo = ex.extraInfo<StaleConfigInfo>();
                ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
                ASSERT_EQ(shardVersionShardedCollection1, exInfo->getVersionReceived());
                ASSERT_EQ(boost::none, exInfo->getVersionWanted());
                ASSERT_EQ(ShardId("this"), exInfo->getShardId());
                ASSERT_TRUE(exInfo->getCriticalSectionSignal().is_initialized());
            });
    }

    {
        // Exit the critical section.
        AutoGetCollection coll(opCtx(), nssShardedCollection1, MODE_X);
        const auto& csr = CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
            opCtx(), nssShardedCollection1);
        csr->exitCriticalSection(criticalSectionReason);
    }
}

TEST_F(ShardRoleTest, AcquireShardedCollWithoutSpecifyingPlacementVersion) {
    const auto acquisition =
        acquireCollection(opCtx(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              opCtx(), nssShardedCollection1, AcquisitionPrerequisites::kWrite),
                          MODE_IX);

    ASSERT_EQ(nssShardedCollection1, acquisition.nss());
    ASSERT_EQ(nssShardedCollection1, acquisition.getCollectionPtr()->ns());

    // Note that the collection is treated as unsharded because the operation is unversioned.
    ASSERT_FALSE(acquisition.getShardingDescription().isSharded());
    ASSERT_FALSE(acquisition.getShardingFilter().has_value());
}

// ---------------------------------------------------------------------------
// Acquire inexistent collections

TEST_F(ShardRoleTest, AcquireCollectionNonExistentNamespace) {
    const NamespaceString inexistentNss =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "inexistent");
    auto acquisition =
        acquireCollection(opCtx(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              opCtx(), inexistentNss, AcquisitionPrerequisites::kWrite),
                          MODE_IX);
    ASSERT(!acquisition.getCollectionPtr());
    ASSERT(!acquisition.getShardingDescription().isSharded());
    ASSERT(!acquisition.getShardingFilter());
}

TEST_F(ShardRoleTest, AcquireInexistentCollectionWithWrongPlacementThrowsBecauseWrongPlacement) {
    const auto incorrectDbVersion = dbVersionTestDb.makeUpdated();
    const NamespaceString inexistentNss =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "inexistent");

    PlacementConcern placementConcern{incorrectDbVersion, {}};
    ASSERT_THROWS_WITH_CHECK(acquireCollection(opCtx(),
                                               {inexistentNss,
                                                placementConcern,
                                                repl::ReadConcernArgs(),
                                                AcquisitionPrerequisites::kWrite},
                                               MODE_IX),
                             ExceptionFor<ErrorCodes::StaleDbVersion>,
                             [&](const DBException& ex) {
                                 const auto exInfo = ex.extraInfo<StaleDbRoutingVersion>();
                                 ASSERT_EQ(dbNameTestDb.db(), exInfo->getDb());
                                 ASSERT_EQ(incorrectDbVersion, exInfo->getVersionReceived());
                                 ASSERT_EQ(dbVersionTestDb, exInfo->getVersionWanted());
                                 ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
                             });
}

TEST_F(ShardRoleTest, AcquireCollectionButItIsAView) {
    ASSERT_THROWS_CODE(acquireCollection(opCtx(),
                                         CollectionAcquisitionRequest::fromOpCtx(
                                             opCtx(), nssView, AcquisitionPrerequisites::kWrite),
                                         MODE_IX),
                       DBException,
                       ErrorCodes::CommandNotSupportedOnView);

    const auto acquisition =
        acquireCollectionOrView(opCtx(),
                                CollectionOrViewAcquisitionRequest::fromOpCtx(
                                    opCtx(), nssView, AcquisitionPrerequisites::kWrite),
                                MODE_IX);

    ASSERT_TRUE(std::holds_alternative<ScopedViewAcquisition>(acquisition));
    const ScopedViewAcquisition& viewAcquisition = std::get<ScopedViewAcquisition>(acquisition);

    ASSERT_EQ(nssView, viewAcquisition.nss());
    ASSERT_EQ(nssUnshardedCollection1, viewAcquisition.getViewDefinition().viewOn());
    ASSERT(std::equal(viewPipeline.begin(),
                      viewPipeline.end(),
                      viewAcquisition.getViewDefinition().pipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

// ---------------------------------------------------------------------------
// Acquire multiple collections

TEST_F(ShardRoleTest, AcquireMultipleCollectionsAllWithCorrectPlacementConcern) {
    const auto acquisitions =
        acquireCollections(opCtx(),
                           {{nssUnshardedCollection1,
                             PlacementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()},
                             repl::ReadConcernArgs(),
                             AcquisitionPrerequisites::kWrite},
                            {nssShardedCollection1,
                             PlacementConcern{{}, shardVersionShardedCollection1},
                             repl::ReadConcernArgs(),
                             AcquisitionPrerequisites::kWrite}},
                           MODE_IX);

    ASSERT_EQ(2, acquisitions.size());

    const auto& acquisitionUnshardedColl =
        std::find_if(acquisitions.begin(),
                     acquisitions.end(),
                     [nss = nssUnshardedCollection1](const auto& acquisition) {
                         return acquisition.nss() == nss;
                     });
    ASSERT(acquisitionUnshardedColl != acquisitions.end());
    ASSERT_FALSE(acquisitionUnshardedColl->getShardingDescription().isSharded());
    ASSERT_FALSE(acquisitionUnshardedColl->getShardingFilter().has_value());

    const auto& acquisitionShardedColl =
        std::find_if(acquisitions.begin(),
                     acquisitions.end(),
                     [nss = nssShardedCollection1](const auto& acquisition) {
                         return acquisition.nss() == nss;
                     });
    ASSERT(acquisitionShardedColl != acquisitions.end());
    ASSERT_TRUE(acquisitionShardedColl->getShardingDescription().isSharded());
    ASSERT_TRUE(acquisitionShardedColl->getShardingFilter().has_value());

    // Assert the DB lock is held, but not recursively (i.e. only once).
    ASSERT_TRUE(opCtx()->lockState()->isDbLockedForMode(dbNameTestDb, MODE_IX));
    ASSERT_FALSE(opCtx()->lockState()->isGlobalLockedRecursively());

    // Assert both collections are locked.
    ASSERT_TRUE(opCtx()->lockState()->isCollectionLockedForMode(nssUnshardedCollection1, MODE_IX));
    ASSERT_TRUE(opCtx()->lockState()->isCollectionLockedForMode(nssShardedCollection1, MODE_IX));
}

TEST_F(ShardRoleTest, AcquireMultipleCollectionsWithIncorrectPlacementConcernThrows) {
    ASSERT_THROWS_WITH_CHECK(
        acquireCollections(opCtx(),
                           {{nssUnshardedCollection1,
                             PlacementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()},
                             repl::ReadConcernArgs(),
                             AcquisitionPrerequisites::kWrite},
                            {nssShardedCollection1,
                             PlacementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()},
                             repl::ReadConcernArgs(),
                             AcquisitionPrerequisites::kWrite}},
                           MODE_IX),
        ExceptionFor<ErrorCodes::StaleConfig>,
        [&](const DBException& ex) {
            const auto exInfo = ex.extraInfo<StaleConfigInfo>();
            ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
            ASSERT_EQ(ShardVersion::UNSHARDED(), exInfo->getVersionReceived());
            ASSERT_EQ(shardVersionShardedCollection1, exInfo->getVersionWanted());
            ASSERT_EQ(ShardId("this"), exInfo->getShardId());
            ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
        });
}

DEATH_TEST_REGEX_F(ShardRoleTest,
                   ForbiddenToAcquireMultipleCollectionsOnDifferentDatabases,
                   "Tripwire assertion") {
    ASSERT_THROWS_CODE(
        acquireCollections(opCtx(),
                           {CollectionAcquisitionRequest::fromOpCtx(
                                opCtx(), nssUnshardedCollection1, AcquisitionPrerequisites::kWrite),
                            CollectionAcquisitionRequest::fromOpCtx(
                                opCtx(),
                                NamespaceString::createNamespaceString_forTest("anotherDb", "foo"),
                                AcquisitionPrerequisites::kWrite)},
                           MODE_IX),
        DBException,
        7300400);
}

// ---------------------------------------------------------------------------
// Acquire collection by UUID

TEST_F(ShardRoleTest, AcquireCollectionByUUID) {
    const auto uuid = getCollectionUUID(opCtx(), nssUnshardedCollection1);
    const auto acquisition =
        acquireCollection(opCtx(),
                          {NamespaceStringOrUUID(dbNameTestDb, uuid),
                           PlacementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()},
                           repl::ReadConcernArgs(),
                           AcquisitionPrerequisites::kWrite},
                          MODE_IX);

    ASSERT_EQ(nssUnshardedCollection1, acquisition.nss());
    ASSERT_EQ(nssUnshardedCollection1, acquisition.getCollectionPtr()->ns());
}

TEST_F(ShardRoleTest, AcquireCollectionByUUIDButWrongDbNameThrows) {
    const auto uuid = getCollectionUUID(opCtx(), nssUnshardedCollection1);
    ASSERT_THROWS_CODE(acquireCollection(opCtx(),
                                         {NamespaceStringOrUUID("anotherDbName", uuid),
                                          {},
                                          repl::ReadConcernArgs(),
                                          AcquisitionPrerequisites::kWrite},
                                         MODE_IX),
                       DBException,
                       ErrorCodes::NamespaceNotFound);
}

TEST_F(ShardRoleTest, AcquireCollectionByWrongUUID) {
    const auto uuid = UUID::gen();
    ASSERT_THROWS_CODE(acquireCollection(opCtx(),
                                         {NamespaceStringOrUUID(dbNameTestDb, uuid),
                                          {},
                                          repl::ReadConcernArgs(),
                                          AcquisitionPrerequisites::kWrite},
                                         MODE_IX),
                       DBException,
                       ErrorCodes::NamespaceNotFound);
}

// ---------------------------------------------------------------------------
// Acquire by nss and expected UUID

TEST_F(ShardRoleTest, AcquireCollectionByNssAndExpectedUUID) {
    const auto uuid = getCollectionUUID(opCtx(), nssUnshardedCollection1);
    const auto acquisition = acquireCollection(opCtx(),
                                               {nssUnshardedCollection1,
                                                uuid,
                                                {},
                                                repl::ReadConcernArgs(),
                                                AcquisitionPrerequisites::kWrite},
                                               MODE_IX);

    ASSERT_EQ(nssUnshardedCollection1, acquisition.nss());
    ASSERT_EQ(nssUnshardedCollection1, acquisition.getCollectionPtr()->ns());
}

TEST_F(ShardRoleTest, AcquireCollectionByNssAndWrongExpectedUUIDThrows) {
    const auto nss = nssUnshardedCollection1;
    const auto wrongUuid = UUID::gen();
    ASSERT_THROWS_WITH_CHECK(
        acquireCollection(
            opCtx(),
            {nss, wrongUuid, {}, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
            MODE_IX),
        ExceptionFor<ErrorCodes::CollectionUUIDMismatch>,
        [&](const DBException& ex) {
            const auto exInfo = ex.extraInfo<CollectionUUIDMismatchInfo>();
            ASSERT_EQ(nss.dbName(), exInfo->dbName());
            ASSERT_EQ(wrongUuid, exInfo->collectionUUID());
            ASSERT_EQ(nss.coll(), exInfo->expectedCollection());
            ASSERT_EQ(boost::none, exInfo->actualCollection());
        });
}

TEST_F(ShardRoleTest, AcquireViewWithExpectedUUIDAlwaysThrows) {
    // Because views don't really have a uuid.
    const auto expectedUUID = UUID::gen();
    ASSERT_THROWS_CODE(acquireCollectionsOrViews(opCtx(),
                                                 {{nssView,
                                                   expectedUUID,
                                                   {},
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kWrite,
                                                   AcquisitionPrerequisites::kCanBeView}},
                                                 MODE_IX),
                       DBException,
                       ErrorCodes::CollectionUUIDMismatch);
}

// ---------------------------------------------------------------------------
// Acquire collection or view

TEST_F(ShardRoleTest, AcquireCollectionOrView) {
    ASSERT_THROWS_CODE(acquireCollectionOrView(opCtx(),
                                               {
                                                   nssView,
                                                   {},
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kWrite,
                                                   AcquisitionPrerequisites::kMustBeCollection,
                                               },
                                               MODE_IX),
                       DBException,
                       ErrorCodes::CommandNotSupportedOnView);

    {
        auto acquisition = acquireCollectionOrView(opCtx(),
                                                   {
                                                       nssView,
                                                       {},
                                                       repl::ReadConcernArgs(),
                                                       AcquisitionPrerequisites::kWrite,
                                                       AcquisitionPrerequisites::kCanBeView,
                                                   },
                                                   MODE_IX);
        ASSERT_TRUE(std::holds_alternative<ScopedViewAcquisition>(acquisition));
    }

    {
        auto acquisition = acquireCollectionOrView(opCtx(),
                                                   {
                                                       nssUnshardedCollection1,
                                                       {},
                                                       repl::ReadConcernArgs(),
                                                       AcquisitionPrerequisites::kWrite,
                                                       AcquisitionPrerequisites::kCanBeView,
                                                   },
                                                   MODE_IX);
        ASSERT_TRUE(std::holds_alternative<ScopedCollectionAcquisition>(acquisition));
    }
}

// ---------------------------------------------------------------------------
// Yield and restore

TEST_F(ShardRoleTest, YieldAndRestoreAcquisitionWithLocks) {
    const auto nss = nssUnshardedCollection1;

    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    const auto acquisition = acquireCollection(opCtx(),
                                               {
                                                   nss,
                                                   placementConcern,
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kWrite,
                                               },
                                               MODE_IX);

    ASSERT_TRUE(opCtx()->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
    ASSERT_TRUE(opCtx()->lockState()->isCollectionLockedForMode(nss, MODE_IX));

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());
    ASSERT_FALSE(opCtx()->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
    ASSERT_FALSE(opCtx()->lockState()->isCollectionLockedForMode(nss, MODE_IX));

    // Restore the resources
    restoreTransactionResourcesToOperationContext(opCtx(), std::move(yieldedTransactionResources));
    ASSERT_TRUE(opCtx()->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
    ASSERT_TRUE(opCtx()->lockState()->isCollectionLockedForMode(nss, MODE_IX));
}

TEST_F(ShardRoleTest, RestoreForWriteFailsIfPlacementConcernNoLongerMet) {
    const auto nss = nssShardedCollection1;

    PlacementConcern placementConcern{{}, shardVersionShardedCollection1};
    const auto acquisition = acquireCollection(
        opCtx(),
        {nss, placementConcern, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
        MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());

    // Placement changes
    const auto newShardVersion = [&]() {
        auto newPlacementVersion = shardVersionShardedCollection1.placementVersion();
        newPlacementVersion.incMajor();
        return ShardVersionFactory::make(newPlacementVersion,
                                         boost::optional<CollectionIndexes>(boost::none));
    }();
    const auto uuid = getCollectionUUID(opCtx(), nss);
    installShardedCollectionMetadata(
        opCtx(),
        nss,
        dbVersionTestDb,
        {ChunkType(uuid,
                   ChunkRange{BSON("skey" << MINKEY), BSON("skey" << MAXKEY)},
                   newShardVersion.placementVersion(),
                   thisShardId)},
        thisShardId);

    // Try to restore the resources should fail because placement concern is no longer met.
    ASSERT_THROWS_WITH_CHECK(restoreTransactionResourcesToOperationContext(
                                 opCtx(), std::move(yieldedTransactionResources)),
                             ExceptionFor<ErrorCodes::StaleConfig>,
                             [&](const DBException& ex) {
                                 const auto exInfo = ex.extraInfo<StaleConfigInfo>();
                                 ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
                                 ASSERT_EQ(shardVersionShardedCollection1,
                                           exInfo->getVersionReceived());
                                 ASSERT_EQ(newShardVersion, exInfo->getVersionWanted());
                                 ASSERT_EQ(ShardId("this"), exInfo->getShardId());
                                 ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
                             });

    ASSERT_FALSE(opCtx()->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
    ASSERT_FALSE(opCtx()->lockState()->isCollectionLockedForMode(nss, MODE_IX));
}

TEST_F(ShardRoleTest, RestoreWithShardVersionIgnored) {
    const auto nss = nssShardedCollection1;

    PlacementConcern placementConcern{
        {}, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none)};
    const auto acquisition = acquireCollection(opCtx(),
                                               {
                                                   nss,
                                                   placementConcern,
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kWrite,
                                               },
                                               MODE_IX);

    ASSERT_TRUE(acquisition.getShardingDescription().isSharded());
    ASSERT_TRUE(acquisition.getShardingFilter().has_value());

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());

    // Placement changes
    const auto newShardVersion = [&]() {
        auto newPlacementVersion = shardVersionShardedCollection1.placementVersion();
        newPlacementVersion.incMajor();
        return ShardVersionFactory::make(newPlacementVersion, boost::none);
    }();

    const auto uuid = getCollectionUUID(opCtx(), nss);
    installShardedCollectionMetadata(
        opCtx(),
        nss,
        dbVersionTestDb,
        {ChunkType(uuid,
                   ChunkRange{BSON("skey" << MINKEY), BSON("skey" << MAXKEY)},
                   newShardVersion.placementVersion(),
                   thisShardId)},
        thisShardId);

    // Try to restore the resources should work because placement concern (IGNORED) can be met.
    restoreTransactionResourcesToOperationContext(opCtx(), std::move(yieldedTransactionResources));
    ASSERT_TRUE(opCtx()->lockState()->isCollectionLockedForMode(nss, MODE_IX));
}

void ShardRoleTest::testRestoreFailsIfCollectionBecomesCreated(
    AcquisitionPrerequisites::OperationType operationType) {
    NamespaceString nss(NamespaceString::createNamespaceString_forTest(
        dbNameTestDb, "NonExistentCollectionWhichWillBeCreated"));

    const auto acquisition = acquireCollection(
        opCtx(), CollectionAcquisitionRequest::fromOpCtx(opCtx(), nss, operationType), MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());

    // Create the collection
    createTestCollection(opCtx(), nss);

    // Try to restore the resources should fail because the collection showed-up after a restore
    // where it didn't exist before that.
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           opCtx(), std::move(yieldedTransactionResources)),
                       DBException,
                       743870);
}
TEST_F(ShardRoleTest, RestoreForReadFailsIfCollectionBecomesCreated) {
    testRestoreFailsIfCollectionBecomesCreated(AcquisitionPrerequisites::kRead);
}
TEST_F(ShardRoleTest, RestoreForWriteFailsIfCollectionBecomesCreated) {
    testRestoreFailsIfCollectionBecomesCreated(AcquisitionPrerequisites::kWrite);
}

void ShardRoleTest::testRestoreFailsIfCollectionNoLongerExists(
    AcquisitionPrerequisites::OperationType operationType) {
    const auto nss = nssShardedCollection1;

    PlacementConcern placementConcern{{}, shardVersionShardedCollection1};
    const auto acquisition = acquireCollection(
        opCtx(), {nss, placementConcern, repl::ReadConcernArgs(), operationType}, MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());

    // Drop the collection
    {
        DBDirectClient client(opCtx());
        client.dropCollection(nss);
    }

    // Try to restore the resources should fail because the collection no longer exists.
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           opCtx(), std::move(yieldedTransactionResources)),
                       DBException,
                       ErrorCodes::NamespaceNotFound);
}
TEST_F(ShardRoleTest, RestoreForReadFailsIfCollectionNoLongerExists) {
    testRestoreFailsIfCollectionNoLongerExists(AcquisitionPrerequisites::kRead);
}
TEST_F(ShardRoleTest, RestoreForWriteFailsIfCollectionNoLongerExists) {
    testRestoreFailsIfCollectionNoLongerExists(AcquisitionPrerequisites::kWrite);
}

void ShardRoleTest::testRestoreFailsIfCollectionRenamed(
    AcquisitionPrerequisites::OperationType operationType) {
    const auto nss = nssUnshardedCollection1;

    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    const auto acquisition = acquireCollection(
        opCtx(), {nss, placementConcern, repl::ReadConcernArgs(), operationType}, MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());

    // Rename the collection.
    {
        DBDirectClient client(opCtx());
        BSONObj info;
        ASSERT_TRUE(client.runCommand(
            DatabaseName(boost::none, dbNameTestDb.db()),
            BSON("renameCollection"
                 << nss.ns() << "to"
                 << NamespaceString::createNamespaceString_forTest(dbNameTestDb, "foo2").ns()),
            info));
    }

    // Try to restore the resources should fail because the collection has been renamed.
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           opCtx(), std::move(yieldedTransactionResources)),
                       DBException,
                       ErrorCodes::NamespaceNotFound);
}
TEST_F(ShardRoleTest, RestoreForReadFailsIfCollectionRenamed) {
    testRestoreFailsIfCollectionRenamed(AcquisitionPrerequisites::kRead);
}
TEST_F(ShardRoleTest, RestoreForWriteFailsIfCollectionRenamed) {
    testRestoreFailsIfCollectionRenamed(AcquisitionPrerequisites::kWrite);
}

void ShardRoleTest::testRestoreFailsIfCollectionDroppedAndRecreated(
    AcquisitionPrerequisites::OperationType operationType) {
    const auto nss = nssUnshardedCollection1;

    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    const auto acquisition = acquireCollection(
        opCtx(), {nss, placementConcern, repl::ReadConcernArgs(), operationType}, MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());

    // Drop the collection and create a new one with the same nss.
    {
        DBDirectClient client(opCtx());
        client.dropCollection(nss);
        createTestCollection(opCtx(), nss);
    }

    // Try to restore the resources should fail because the collection no longer exists.
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           opCtx(), std::move(yieldedTransactionResources)),
                       DBException,
                       ErrorCodes::CollectionUUIDMismatch);
}
TEST_F(ShardRoleTest, RestoreForWriteFailsIfCollectionDroppedAndRecreated) {
    testRestoreFailsIfCollectionDroppedAndRecreated(AcquisitionPrerequisites::kWrite);
}
TEST_F(ShardRoleTest, RestoreForReadFailsIfCollectionDroppedAndRecreated) {
    testRestoreFailsIfCollectionDroppedAndRecreated(AcquisitionPrerequisites::kRead);
}

TEST_F(ShardRoleTest, RestoreForReadSucceedsEvenIfPlacementHasChanged) {
    const auto nss = nssShardedCollection1;

    PlacementConcern placementConcern{{}, shardVersionShardedCollection1};

    SharedSemiFuture<void> ongoingQueriesCompletionFuture;

    {
        const auto acquisition = acquireCollection(
            opCtx(),
            {nss, placementConcern, repl::ReadConcernArgs(), AcquisitionPrerequisites::kRead},
            MODE_IX);

        ongoingQueriesCompletionFuture =
            CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx(), nss)
                ->getOngoingQueriesCompletionFuture(
                    getCollectionUUID(opCtx(), nss),
                    ChunkRange(BSON("skey" << MINKEY), BSON("skey" << MAXKEY)));

        // Yield the resources
        auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());

        ASSERT_FALSE(ongoingQueriesCompletionFuture.isReady());
        ASSERT_TRUE(acquisition.getShardingFilter().has_value());
        ASSERT_TRUE(acquisition.getShardingFilter()->keyBelongsToMe(BSON("skey" << 0)));

        // Placement changes
        const auto newShardVersion = [&]() {
            auto newPlacementVersion = shardVersionShardedCollection1.placementVersion();
            newPlacementVersion.incMajor();
            return ShardVersionFactory::make(newPlacementVersion,
                                             boost::optional<CollectionIndexes>(boost::none));
        }();

        const auto uuid = getCollectionUUID(opCtx(), nss);
        installShardedCollectionMetadata(
            opCtx(),
            nss,
            dbVersionTestDb,
            {ChunkType(uuid,
                       ChunkRange{BSON("skey" << MINKEY), BSON("skey" << MAXKEY)},
                       newShardVersion.placementVersion(),
                       ShardId("anotherShard"))},
            thisShardId);

        // Restore should work for reads even though placement has changed.
        restoreTransactionResourcesToOperationContext(opCtx(),
                                                      std::move(yieldedTransactionResources));

        ASSERT_FALSE(ongoingQueriesCompletionFuture.isReady());

        // Even though placement has changed, the filter (and preserver) still point to the original
        // placement.
        ASSERT_TRUE(acquisition.getShardingFilter().has_value());
        ASSERT_TRUE(acquisition.getShardingFilter()->keyBelongsToMe(BSON("skey" << 0)));
    }

    // Acquisition released. Now the range is no longer in use.
    ASSERT_TRUE(ongoingQueriesCompletionFuture.isReady());
}

DEATH_TEST_REGEX_F(ShardRoleTest, YieldingViewAcquisitionIsForbidden, "Tripwire assertion") {
    const auto acquisition =
        acquireCollectionOrView(opCtx(),
                                CollectionOrViewAcquisitionRequest::fromOpCtx(
                                    opCtx(), nssView, AcquisitionPrerequisites::kWrite),
                                MODE_IX);

    ASSERT_THROWS_CODE(
        yieldTransactionResourcesFromOperationContext(opCtx()), DBException, 7300502);
}

void ShardRoleTest::testRestoreFailsIfCollectionIsNowAView(
    AcquisitionPrerequisites::OperationType operationType) {
    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};

    const auto acquisition = acquireCollection(
        opCtx(),
        {nssUnshardedCollection1, placementConcern, repl::ReadConcernArgs(), operationType},
        MODE_IX);

    // Yield the resources.
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());

    // Drop collection and create a view in its place.
    {
        DBDirectClient client(opCtx());
        client.dropCollection(nssUnshardedCollection1);
        createTestView(opCtx(), nssUnshardedCollection1, nssShardedCollection1, {});
    }

    // Restore should fail.
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           opCtx(), std::move(yieldedTransactionResources)),
                       DBException,
                       ErrorCodes::CollectionUUIDMismatch);
}
TEST_F(ShardRoleTest, RestoreForReadFailsIfCollectionIsNowAView) {
    testRestoreFailsIfCollectionIsNowAView(AcquisitionPrerequisites::kRead);
}
TEST_F(ShardRoleTest, RestoreForWriteFailsIfCollectionIsNowAView) {
    testRestoreFailsIfCollectionIsNowAView(AcquisitionPrerequisites::kWrite);
}

// ---------------------------------------------------------------------------
// ScopedLocalCatalogWriteFence

TEST_F(ShardRoleTest, ScopedLocalCatalogWriteFenceWUOWCommitWithinWriterScope) {
    auto acquisition = acquireCollection(opCtx(),
                                         {nssShardedCollection1,
                                          PlacementConcern{{}, shardVersionShardedCollection1},
                                          repl::ReadConcernArgs(),
                                          AcquisitionPrerequisites::kRead},
                                         MODE_X);
    ASSERT(!acquisition.getCollectionPtr()->isTemporary());

    {
        WriteUnitOfWork wuow(opCtx());
        CollectionWriter localCatalogWriter(opCtx(), &acquisition);
        localCatalogWriter.getWritableCollection(opCtx())->setIsTemp(opCtx(), true);
        wuow.commit();
    }

    ASSERT(acquisition.getCollectionPtr()->isTemporary());
}

TEST_F(ShardRoleTest, ScopedLocalCatalogWriteFenceWUOWCommitAfterWriterScope) {
    auto acquisition = acquireCollection(opCtx(),
                                         {nssShardedCollection1,
                                          PlacementConcern{{}, shardVersionShardedCollection1},
                                          repl::ReadConcernArgs(),
                                          AcquisitionPrerequisites::kRead},
                                         MODE_X);
    ASSERT(!acquisition.getCollectionPtr()->isTemporary());

    WriteUnitOfWork wuow(opCtx());
    {
        CollectionWriter localCatalogWriter(opCtx(), &acquisition);
        localCatalogWriter.getWritableCollection(opCtx())->setIsTemp(opCtx(), true);
    }
    ASSERT(acquisition.getCollectionPtr()->isTemporary());
    wuow.commit();
    ASSERT(acquisition.getCollectionPtr()->isTemporary());
}

TEST_F(ShardRoleTest, ScopedLocalCatalogWriteFenceWUOWRollbackWithinWriterScope) {
    auto acquisition = acquireCollection(opCtx(),
                                         {nssShardedCollection1,
                                          PlacementConcern{{}, shardVersionShardedCollection1},
                                          repl::ReadConcernArgs(),
                                          AcquisitionPrerequisites::kRead},
                                         MODE_X);
    ASSERT(!acquisition.getCollectionPtr()->isTemporary());

    {
        WriteUnitOfWork wuow(opCtx());
        CollectionWriter localCatalogWriter(opCtx(), &acquisition);
        localCatalogWriter.getWritableCollection(opCtx())->setIsTemp(opCtx(), true);
    }
    ASSERT(!acquisition.getCollectionPtr()->isTemporary());
}

TEST_F(ShardRoleTest, ScopedLocalCatalogWriteFenceWUOWRollbackAfterWriterScope) {
    auto acquisition = acquireCollection(opCtx(),
                                         {nssShardedCollection1,
                                          PlacementConcern{{}, shardVersionShardedCollection1},
                                          repl::ReadConcernArgs(),
                                          AcquisitionPrerequisites::kRead},
                                         MODE_X);
    ASSERT(!acquisition.getCollectionPtr()->isTemporary());

    {
        WriteUnitOfWork wuow(opCtx());
        {
            CollectionWriter localCatalogWriter(opCtx(), &acquisition);
            localCatalogWriter.getWritableCollection(opCtx())->setIsTemp(opCtx(), true);
        }
        ASSERT(acquisition.getCollectionPtr()->isTemporary());
    }
    ASSERT(!acquisition.getCollectionPtr()->isTemporary());
}

TEST_F(ShardRoleTest, ScopedLocalCatalogWriteFenceOutsideWUOUCommit) {
    auto acquisition = acquireCollection(opCtx(),
                                         {nssShardedCollection1,
                                          PlacementConcern{{}, shardVersionShardedCollection1},
                                          repl::ReadConcernArgs(),
                                          AcquisitionPrerequisites::kRead},
                                         MODE_X);
    ASSERT(!acquisition.getCollectionPtr()->isTemporary());

    {
        CollectionWriter localCatalogWriter(opCtx(), &acquisition);
        WriteUnitOfWork wuow(opCtx());
        localCatalogWriter.getWritableCollection(opCtx())->setIsTemp(opCtx(), true);
        ASSERT(localCatalogWriter->isTemporary());
        wuow.commit();
        ASSERT(localCatalogWriter->isTemporary());
    }
    ASSERT(acquisition.getCollectionPtr()->isTemporary());
}

TEST_F(ShardRoleTest, ScopedLocalCatalogWriteFenceOutsideWUOURollback) {
    auto acquisition = acquireCollection(opCtx(),
                                         {nssShardedCollection1,
                                          PlacementConcern{{}, shardVersionShardedCollection1},
                                          repl::ReadConcernArgs(),
                                          AcquisitionPrerequisites::kRead},
                                         MODE_X);
    ASSERT(!acquisition.getCollectionPtr()->isTemporary());

    {
        CollectionWriter localCatalogWriter(opCtx(), &acquisition);
        {
            WriteUnitOfWork wuow(opCtx());
            localCatalogWriter.getWritableCollection(opCtx())->setIsTemp(opCtx(), true);
            ASSERT(localCatalogWriter->isTemporary());
        }
        ASSERT(!localCatalogWriter->isTemporary());
    }
    ASSERT(!acquisition.getCollectionPtr()->isTemporary());
}

}  // namespace
}  // namespace mongo
