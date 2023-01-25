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
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role.h"

namespace mongo {
namespace {

void createTestCollection(OperationContext* opCtx, const NamespaceString& nss) {
    uassertStatusOK(createCollection(opCtx, nss.dbName(), BSON("create" << nss.coll())));
}

void installDatabaseMetadata(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             const DatabaseVersion& dbVersion) {
    AutoGetDb autoDb(opCtx, dbName, MODE_X, {});
    auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquire(
        opCtx, dbName, DSSAcquisitionMode::kExclusive);
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
                                      const ShardVersion& shardVersion) {
    const auto uuid = [&] {
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        return autoColl.getCollection()->uuid();
    }();

    const std::string shardKey("skey");
    const ShardKeyPattern shardKeyPattern{BSON(shardKey << 1)};
    const ShardId thisShardId("this");

    auto rt = RoutingTableHistory::makeNew(
        nss,
        uuid,
        shardKeyPattern.getKeyPattern(),
        nullptr,
        false,
        shardVersion.placementVersion().epoch(),
        shardVersion.placementVersion().getTimestamp(),
        boost::none /* timeseriesFields */,
        boost::none /* resharding Fields */,
        boost::none /* chunkSizeBytes */,
        true /* allowMigrations */,
        {ChunkType{uuid,
                   ChunkRange{BSON(shardKey << MINKEY), BSON(shardKey << MAXKEY)},
                   shardVersion.placementVersion(),
                   thisShardId}});

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

    const DatabaseName dbNameTestDb{"test"};
    const DatabaseVersion dbVersionTestDb{UUID::gen(), Timestamp(1, 0)};

    const NamespaceString nssUnshardedCollection1{dbNameTestDb, "unsharded"};

    const NamespaceString nssShardedCollection1{dbNameTestDb, "sharded"};
    const ShardVersion shardVersionShardedCollection1{
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(5, 0)}, CollectionPlacement(10, 1)),
        boost::optional<CollectionIndexes>(boost::none)};

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

    createTestCollection(opCtx(), nssUnshardedCollection1);
    installUnshardedCollectionMetadata(opCtx(), nssUnshardedCollection1);

    createTestCollection(opCtx(), nssShardedCollection1);
    installDatabaseMetadata(opCtx(), dbNameTestDb, dbVersionTestDb);
    installShardedCollectionMetadata(
        opCtx(), nssShardedCollection1, dbVersionTestDb, shardVersionShardedCollection1);
}

void ShardRoleTest::tearDown() {
    _opCtx.reset();
    ServiceContextMongoDTest::tearDown();
    repl::ReplicationCoordinator::set(getGlobalServiceContext(), nullptr);
}

TEST_F(ShardRoleTest, NamespaceOrViewAcquisitionRequestWithOpCtxTakesPlacementFromOSS) {
    const auto nss = nssUnshardedCollection1;

    {
        NamespaceOrViewAcquisitionRequest acquisitionRequest(opCtx(), nss, {});
        ASSERT_EQ(boost::none, acquisitionRequest.placementConcern.dbVersion);
        ASSERT_EQ(boost::none, acquisitionRequest.placementConcern.shardVersion);
    }

    {
        const NamespaceString anotherCollection("test2.foo");
        ScopedSetShardRole setShardRole(
            opCtx(), anotherCollection, ShardVersion::UNSHARDED(), dbVersionTestDb);
        NamespaceOrViewAcquisitionRequest acquisitionRequest(opCtx(), nss, {});
        ASSERT_EQ(boost::none, acquisitionRequest.placementConcern.dbVersion);
        ASSERT_EQ(boost::none, acquisitionRequest.placementConcern.shardVersion);
    }

    {
        const auto dbVersion = boost::none;
        const auto shardVersion = boost::none;
        ScopedSetShardRole setShardRole(opCtx(), nss, shardVersion, dbVersion);
        NamespaceOrViewAcquisitionRequest acquisitionRequest(opCtx(), nss, {});
        ASSERT_EQ(dbVersion, acquisitionRequest.placementConcern.dbVersion);
        ASSERT_EQ(shardVersion, acquisitionRequest.placementConcern.shardVersion);
    }

    {
        const auto dbVersion = dbVersionTestDb;
        const auto shardVersion = ShardVersion::UNSHARDED();
        ScopedSetShardRole setShardRole(opCtx(), nss, shardVersion, dbVersion);
        NamespaceOrViewAcquisitionRequest acquisitionRequest(opCtx(), nss, {});
        ASSERT_EQ(dbVersion, acquisitionRequest.placementConcern.dbVersion);
        ASSERT_EQ(shardVersion, acquisitionRequest.placementConcern.shardVersion);
    }

    {
        const auto dbVersion = boost::none;
        const auto shardVersion = shardVersionShardedCollection1;
        ScopedSetShardRole setShardRole(opCtx(), nss, shardVersion, dbVersion);
        NamespaceOrViewAcquisitionRequest acquisitionRequest(opCtx(), nss, {});
        ASSERT_EQ(dbVersion, acquisitionRequest.placementConcern.dbVersion);
        ASSERT_EQ(shardVersion, acquisitionRequest.placementConcern.shardVersion);
    }
}

// ---------------------------------------------------------------------------
// Placement checks when acquiring unsharded collections

TEST_F(ShardRoleTest, AcquireUnshardedCollWithCorrectPlacementVersion) {
    NamespaceOrViewAcquisitionRequest::PlacementConcern placementConcern =
        NamespaceOrViewAcquisitionRequest::PlacementConcern{dbVersionTestDb,
                                                            ShardVersion::UNSHARDED()};
    const auto acquisitions =
        acquireCollectionsOrViews(opCtx(),
                                  {{nssUnshardedCollection1,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
                                  MODE_IX);

    ASSERT_EQ(1, acquisitions.size());
    ASSERT_EQ(nssUnshardedCollection1, acquisitions.front().nss());
    ASSERT_EQ(nssUnshardedCollection1, acquisitions.front().getCollectionPtr()->ns());
    ASSERT_FALSE(acquisitions.front().isView());
    ASSERT_FALSE(acquisitions.front().getShardingDescription().isSharded());
}

TEST_F(ShardRoleTest, AcquireUnshardedCollWithIncorrectPlacementVersionThrows) {
    const auto incorrectDbVersion = DatabaseVersion(UUID::gen(), Timestamp(50, 0));

    NamespaceOrViewAcquisitionRequest::PlacementConcern placementConcern =
        NamespaceOrViewAcquisitionRequest::PlacementConcern{incorrectDbVersion,
                                                            ShardVersion::UNSHARDED()};
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViews(opCtx(),
                                  {{nssUnshardedCollection1,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
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
        AutoGetDb autoDb(opCtx(), dbNameTestDb, MODE_X, {});
        auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquire(
            opCtx(), dbNameTestDb, DSSAcquisitionMode::kExclusive);
        scopedDss->clearDbInfo(opCtx());
    }

    NamespaceOrViewAcquisitionRequest::PlacementConcern placementConcern =
        NamespaceOrViewAcquisitionRequest::PlacementConcern{dbVersionTestDb,
                                                            ShardVersion::UNSHARDED()};
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViews(opCtx(),
                                  {{nssUnshardedCollection1,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
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
        AutoGetDb autoDb(opCtx(), dbNameTestDb, MODE_X, {});
        const auto dss =
            DatabaseShardingState::acquire(opCtx(), dbNameTestDb, DSSAcquisitionMode::kExclusive);
        dss->enterCriticalSectionCatchUpPhase(opCtx(), criticalSectionReason);
        dss->enterCriticalSectionCommitPhase(opCtx(), criticalSectionReason);
    }

    {
        NamespaceOrViewAcquisitionRequest::PlacementConcern placementConcern =
            NamespaceOrViewAcquisitionRequest::PlacementConcern{dbVersionTestDb,
                                                                ShardVersion::UNSHARDED()};
        ASSERT_THROWS_WITH_CHECK(
            acquireCollectionsOrViews(opCtx(),
                                      {{nssUnshardedCollection1,
                                        placementConcern,
                                        repl::ReadConcernArgs(),
                                        NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
                                      MODE_IX),
            ExceptionFor<ErrorCodes::StaleDbVersion>,
            [&](const DBException& ex) {
                const auto exInfo = ex.extraInfo<StaleDbRoutingVersion>();
                ASSERT_EQ(dbNameTestDb.db(), exInfo->getDb());
                ASSERT_EQ(dbVersionTestDb, exInfo->getVersionReceived());
                ASSERT_EQ(boost::none, exInfo->getVersionWanted());
                ASSERT_TRUE(exInfo->getCriticalSectionSignal().is_initialized());
            });
    }

    {
        // Exit critical section.
        AutoGetDb autoDb(opCtx(), dbNameTestDb, MODE_X, {});
        const BSONObj criticalSectionReason = BSON("reason" << 1);
        const auto dss =
            DatabaseShardingState::acquire(opCtx(), dbNameTestDb, DSSAcquisitionMode::kExclusive);
        dss->exitCriticalSection(opCtx(), criticalSectionReason);
    }
}

TEST_F(ShardRoleTest, AcquireUnshardedCollWithoutSpecifyingPlacementVersion) {
    NamespaceOrViewAcquisitionRequest::PlacementConcern placementConcern =
        NamespaceOrViewAcquisitionRequest::kPretendUnshardedDueToDirectConnection;
    const auto acquisitions =
        acquireCollectionsOrViews(opCtx(),
                                  {{nssUnshardedCollection1,
                                    {placementConcern},
                                    repl::ReadConcernArgs(),
                                    NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
                                  MODE_IX);

    ASSERT_EQ(1, acquisitions.size());
    ASSERT_EQ(nssUnshardedCollection1, acquisitions.front().nss());
    ASSERT_EQ(nssUnshardedCollection1, acquisitions.front().getCollectionPtr()->ns());
    ASSERT_FALSE(acquisitions.front().isView());
    ASSERT_FALSE(acquisitions.front().getShardingDescription().isSharded());
}

// ---------------------------------------------------------------------------
// Placement checks when acquiring sharded collections

TEST_F(ShardRoleTest, AcquireShardedCollWithCorrectPlacementVersion) {
    NamespaceOrViewAcquisitionRequest::PlacementConcern placementConcern =
        NamespaceOrViewAcquisitionRequest::PlacementConcern{{} /* dbVersion */,
                                                            shardVersionShardedCollection1};
    const auto acquisitions =
        acquireCollectionsOrViews(opCtx(),
                                  {{nssShardedCollection1,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
                                  MODE_IX);

    ASSERT_EQ(1, acquisitions.size());
    ASSERT_EQ(nssShardedCollection1, acquisitions.front().nss());
    ASSERT_EQ(nssShardedCollection1, acquisitions.front().getCollectionPtr()->ns());
    ASSERT_FALSE(acquisitions.front().isView());
    ASSERT_TRUE(acquisitions.front().getShardingDescription().isSharded());
}

TEST_F(ShardRoleTest, AcquireShardedCollWithIncorrectPlacementVersionThrows) {
    NamespaceOrViewAcquisitionRequest::PlacementConcern placementConcern =
        NamespaceOrViewAcquisitionRequest::PlacementConcern{dbVersionTestDb,
                                                            ShardVersion::UNSHARDED()};
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViews(opCtx(),
                                  {{nssShardedCollection1,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
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

TEST_F(ShardRoleTest, AcquireShardedCollWhenShardDoesNotKnowThePlacementVersionThrows) {
    {
        // Clear the collection filtering metadata on the shard.
        AutoGetCollection coll(opCtx(), nssShardedCollection1, MODE_IX);
        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx(),
                                                                             nssShardedCollection1)
            ->clearFilteringMetadata(opCtx());
    }

    NamespaceOrViewAcquisitionRequest::PlacementConcern placementConcern =
        NamespaceOrViewAcquisitionRequest::PlacementConcern{{}, shardVersionShardedCollection1};
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViews(opCtx(),
                                  {{nssShardedCollection1,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
                                  MODE_IX),
        ExceptionFor<ErrorCodes::StaleConfig>,
        [&](const DBException& ex) {
            const auto exInfo = ex.extraInfo<StaleConfigInfo>();
            ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
            ASSERT_EQ(shardVersionShardedCollection1, exInfo->getVersionReceived());
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
        NamespaceOrViewAcquisitionRequest::PlacementConcern placementConcern =
            NamespaceOrViewAcquisitionRequest::PlacementConcern{{}, shardVersionShardedCollection1};
        ASSERT_THROWS_WITH_CHECK(
            acquireCollectionsOrViews(opCtx(),
                                      {{nssShardedCollection1,
                                        placementConcern,
                                        repl::ReadConcernArgs(),
                                        NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
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
    NamespaceOrViewAcquisitionRequest::PlacementConcern placementConcern =
        NamespaceOrViewAcquisitionRequest::kPretendUnshardedDueToDirectConnection;
    const auto acquisitions =
        acquireCollectionsOrViews(opCtx(),
                                  {{nssShardedCollection1,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
                                  MODE_IX);

    ASSERT_EQ(1, acquisitions.size());
    ASSERT_EQ(nssShardedCollection1, acquisitions.front().nss());
    ASSERT_EQ(nssShardedCollection1, acquisitions.front().getCollectionPtr()->ns());
    ASSERT_FALSE(acquisitions.front().isView());

    // Note that the collection is treated as unsharded because the operation is unversioned.
    ASSERT_FALSE(acquisitions.front().getShardingDescription().isSharded());
}

// ---------------------------------------------------------------------------
// Acquire inexistent collections

TEST_F(ShardRoleTest, AcquireCollectionFailsIfItDoesNotExist) {
    const NamespaceString inexistentNss(dbNameTestDb, "inexistent");
    NamespaceOrViewAcquisitionRequest::PlacementConcern placementConcern;
    ASSERT_THROWS_CODE(
        acquireCollectionsOrViews(opCtx(),
                                  {{inexistentNss,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
                                  MODE_IX),
        DBException,
        ErrorCodes::NamespaceNotFound);
}

TEST_F(ShardRoleTest, AcquireInexistentCollectionWithWrongPlacementThrowsBecauseWrongPlacement) {
    const auto incorrectDbVersion = dbVersionTestDb.makeUpdated();
    const NamespaceString inexistentNss(dbNameTestDb, "inexistent");

    NamespaceOrViewAcquisitionRequest::PlacementConcern placementConcern{incorrectDbVersion, {}};
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViews(opCtx(),
                                  {{inexistentNss,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
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

// ---------------------------------------------------------------------------
// Acquire multiple collections

TEST_F(ShardRoleTest, AcquireMultipleCollectionsAllWithCorrectPlacementConcern) {
    const auto acquisitions = acquireCollectionsOrViews(
        opCtx(),
        {{nssUnshardedCollection1,
          NamespaceOrViewAcquisitionRequest::PlacementConcern{dbVersionTestDb,
                                                              ShardVersion::UNSHARDED()},
          repl::ReadConcernArgs(),
          NamespaceOrViewAcquisitionRequest::kMustBeCollection},
         {nssShardedCollection1,
          NamespaceOrViewAcquisitionRequest::PlacementConcern{{}, shardVersionShardedCollection1},
          repl::ReadConcernArgs(),
          NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
        MODE_IX);

    ASSERT_EQ(2, acquisitions.size());

    const auto& acquisitionUnshardedColl =
        std::find_if(acquisitions.begin(),
                     acquisitions.end(),
                     [nss = nssUnshardedCollection1](const auto& acquisition) {
                         return acquisition.nss() == nss;
                     });
    ASSERT(acquisitionUnshardedColl != acquisitions.end());
    ASSERT_FALSE(acquisitionUnshardedColl->isView());
    ASSERT_FALSE(acquisitionUnshardedColl->getShardingDescription().isSharded());

    const auto& acquisitionShardedColl =
        std::find_if(acquisitions.begin(),
                     acquisitions.end(),
                     [nss = nssShardedCollection1](const auto& acquisition) {
                         return acquisition.nss() == nss;
                     });
    ASSERT(acquisitionShardedColl != acquisitions.end());
    ASSERT_FALSE(acquisitionShardedColl->isView());
    ASSERT_TRUE(acquisitionShardedColl->getShardingDescription().isSharded());
}

TEST_F(ShardRoleTest, AcquireMultipleCollectionsWithIncorrectPlacementConcernThrows) {
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViews(opCtx(),
                                  {{nssUnshardedCollection1,
                                    NamespaceOrViewAcquisitionRequest::PlacementConcern{
                                        dbVersionTestDb, ShardVersion::UNSHARDED()},
                                    repl::ReadConcernArgs(),
                                    NamespaceOrViewAcquisitionRequest::kMustBeCollection},
                                   {nssShardedCollection1,
                                    NamespaceOrViewAcquisitionRequest::PlacementConcern{
                                        dbVersionTestDb, ShardVersion::UNSHARDED()},
                                    repl::ReadConcernArgs(),
                                    NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
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

// ---------------------------------------------------------------------------
// Acquire collection by UUID

TEST_F(ShardRoleTest, AcquireCollectionByUUID) {
    const auto uuid = getCollectionUUID(opCtx(), nssUnshardedCollection1);
    const auto acquisitions =
        acquireCollectionsOrViews(opCtx(),
                                  {{NamespaceStringOrUUID(dbNameTestDb, uuid),
                                    NamespaceOrViewAcquisitionRequest::PlacementConcern{
                                        dbVersionTestDb, ShardVersion::UNSHARDED()},
                                    repl::ReadConcernArgs(),
                                    NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
                                  MODE_IX);

    ASSERT_EQ(1, acquisitions.size());
    ASSERT_EQ(nssUnshardedCollection1, acquisitions.front().nss());
    ASSERT_EQ(nssUnshardedCollection1, acquisitions.front().getCollectionPtr()->ns());
}

TEST_F(ShardRoleTest, AcquireCollectionByUUIDButWrongDbNameThrows) {
    const auto uuid = getCollectionUUID(opCtx(), nssUnshardedCollection1);
    ASSERT_THROWS_CODE(
        acquireCollectionsOrViews(opCtx(),
                                  {{NamespaceStringOrUUID("anotherDbName", uuid),
                                    {},
                                    repl::ReadConcernArgs(),
                                    NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
                                  MODE_IX),
        DBException,
        ErrorCodes::NamespaceNotFound);
}

TEST_F(ShardRoleTest, AcquireCollectionByWrongUUID) {
    const auto uuid = UUID::gen();
    ASSERT_THROWS_CODE(
        acquireCollectionsOrViews(opCtx(),
                                  {{NamespaceStringOrUUID(dbNameTestDb, uuid),
                                    {},
                                    repl::ReadConcernArgs(),
                                    NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
                                  MODE_IX),
        DBException,
        ErrorCodes::NamespaceNotFound);
}

// ---------------------------------------------------------------------------
// Acquire collection by nss and expected UUID

TEST_F(ShardRoleTest, AcquireCollectionByNssAndExpectedUUID) {
    const auto uuid = getCollectionUUID(opCtx(), nssUnshardedCollection1);
    const auto acquisitions =
        acquireCollectionsOrViews(opCtx(),
                                  {{nssUnshardedCollection1,
                                    uuid,
                                    {},
                                    repl::ReadConcernArgs(),
                                    NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
                                  MODE_IX);

    ASSERT_EQ(1, acquisitions.size());
    ASSERT_EQ(nssUnshardedCollection1, acquisitions.front().nss());
    ASSERT_EQ(nssUnshardedCollection1, acquisitions.front().getCollectionPtr()->ns());
}

TEST_F(ShardRoleTest, AcquireCollectionByNssAndWrongExpectedUUIDThrows) {
    const auto nss = nssUnshardedCollection1;
    const auto wrongUuid = UUID::gen();
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViews(opCtx(),
                                  {{nss,
                                    wrongUuid,
                                    {},
                                    repl::ReadConcernArgs(),
                                    NamespaceOrViewAcquisitionRequest::kMustBeCollection}},
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

}  // namespace
}  // namespace mongo
