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

#include "mongo/db/local_catalog/shard_role_api/shard_role.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/local_catalog/catalog_control.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_metadata.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state_mock.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"

#include <algorithm>
#include <string>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

using unittest::assertGet;

UUID getCollectionUUID(OperationContext* opCtx, const NamespaceString& nss) {
    const auto optUuid = CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, nss);
    ASSERT(optUuid);
    return *optUuid;
}

class ShardRoleTest : public ShardServerTestFixture {
protected:
    void setUp() override;

    void installUnshardedCollectionMetadata(OperationContext* opCtx, const NamespaceString& nss);
    void installShardedCollectionMetadata(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const DatabaseVersion& dbVersion,
                                          std::vector<ChunkType> chunks);

    const DatabaseName dbNameTestDb = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    const DatabaseVersion dbVersionTestDb{UUID::gen(), Timestamp(1, 0)};
    const NamespaceString nssUnshardedCollection1 =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "unsharded");

    const NamespaceString nssShardedCollection1 =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "sharded");
    const ShardVersion shardVersionShardedCollection1 = ShardVersionFactory::make(ChunkVersion(
        CollectionGeneration{OID::gen(), Timestamp(5, 0)}, CollectionPlacement(10, 1)));

    const ShardVersion shardVersionIgnore = ShardVersionFactory::make(ChunkVersion::IGNORED());

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

    // A replication rollback causes the collection catalog to close and re-open on a new snapshot.
    void simulateReplicationRollbackEvent(OperationContext* opCtx) {
        auto* storageEngine = opCtx->getServiceContext()->getStorageEngine();
        const auto stableTimestamp =
            repl::ReplicationCoordinator::get(opCtx)->getMyLastAppliedOpTime().getTimestamp();
        storageEngine->setStableTimestamp(stableTimestamp);

        auto newClient = opCtx->getServiceContext()->getService()->makeClient("AlternativeClient");
        AlternativeClientRegion acr(newClient);
        auto newOpCtx = acr->makeOperationContext();

        Lock::GlobalLock globalLk(newOpCtx.get(), MODE_X);
        auto catalogState = catalog::closeCatalog(newOpCtx.get());
        catalog::openCatalog(newOpCtx.get(), catalogState, stableTimestamp);
    }

    void testRestoreFailsWhenMetadataInvalidated(bool terminateSecondaryReadsUponRangeDeletion,
                                                 bool terminateSecondaryReadsOnOrphan,
                                                 bool mustFail);
};

void ShardRoleTest::setUp() {
    ShardServerTestFixture::setUp();
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;

    // Create nssUnshardedCollection1
    createTestCollection(operationContext(), nssUnshardedCollection1);
    installUnshardedCollectionMetadata(operationContext(), nssUnshardedCollection1);

    // Create nssShardedCollection1
    createTestCollection(operationContext(), nssShardedCollection1);
    const auto uuidShardedCollection1 =
        getCollectionUUID(operationContext(), nssShardedCollection1);
    installShardedCollectionMetadata(
        operationContext(),
        nssShardedCollection1,
        dbVersionTestDb,
        {ChunkType(uuidShardedCollection1,
                   ChunkRange{BSON("skey" << MINKEY), BSON("skey" << MAXKEY)},
                   shardVersionShardedCollection1.placementVersion(),
                   kMyShardName)});

    // Setup nssView
    createTestView(operationContext(), nssView, nssUnshardedCollection1, viewPipeline);
    installUnshardedCollectionMetadata(operationContext(), nssView);
}

void ShardRoleTest::installUnshardedCollectionMetadata(OperationContext* opCtx,
                                                       const NamespaceString& nss) {
    AutoGetCollection coll(
        opCtx,
        nss,
        MODE_IX,
        AutoGetCollection::Options{}.viewMode(auto_get_collection::ViewMode::kViewsPermitted));
    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss)
        ->setFilteringMetadata(opCtx, CollectionMetadata::UNTRACKED());
}

void ShardRoleTest::installShardedCollectionMetadata(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     const DatabaseVersion& dbVersion,
                                                     std::vector<ChunkType> chunks) {
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
                                           false, /* unsplittable */
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

    const auto collectionMetadata =
        CollectionMetadata(ChunkManager(rtHandle, boost::none), kMyShardName);

    AutoGetCollection coll(opCtx, nss, MODE_IX);
    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss)
        ->setFilteringMetadata(opCtx, collectionMetadata);
}

TEST_F(ShardRoleTest, NamespaceOrViewAcquisitionRequestWithOpCtxTakesPlacementFromOSS) {
    const auto nss = nssUnshardedCollection1;

    {
        auto acquisition = CollectionAcquisitionRequest::fromOpCtx(
            operationContext(), nss, AcquisitionPrerequisites::kWrite);
        ASSERT_EQ(boost::none, acquisition.placementConcern.getDbVersion());
        ASSERT_EQ(boost::none, acquisition.placementConcern.getShardVersion());
    }

    {
        const NamespaceString anotherCollection =
            NamespaceString::createNamespaceString_forTest("test2.foo");
        ScopedSetShardRole setShardRole(
            operationContext(), anotherCollection, ShardVersion::UNSHARDED(), dbVersionTestDb);
        auto acquisition = CollectionOrViewAcquisitionRequest::fromOpCtx(
            operationContext(), nss, AcquisitionPrerequisites::kWrite);
        ASSERT_EQ(boost::none, acquisition.placementConcern.getDbVersion());
        ASSERT_EQ(boost::none, acquisition.placementConcern.getShardVersion());
    }

    {
        const auto dbVersion = boost::none;
        const auto shardVersion = boost::none;
        ScopedSetShardRole setShardRole(operationContext(), nss, shardVersion, dbVersion);
        auto acquisition = CollectionOrViewAcquisitionRequest::fromOpCtx(
            operationContext(), nss, AcquisitionPrerequisites::kWrite);
        ASSERT_EQ(dbVersion, acquisition.placementConcern.getDbVersion());
        ASSERT_EQ(shardVersion, acquisition.placementConcern.getShardVersion());
    }

    {
        const auto dbVersion = dbVersionTestDb;
        const auto shardVersion = ShardVersion::UNSHARDED();
        ScopedSetShardRole setShardRole(operationContext(), nss, shardVersion, dbVersion);
        auto acquisition = CollectionOrViewAcquisitionRequest::fromOpCtx(
            operationContext(), nss, AcquisitionPrerequisites::kWrite);
        ASSERT_EQ(dbVersion, acquisition.placementConcern.getDbVersion());
        ASSERT_EQ(shardVersion, acquisition.placementConcern.getShardVersion());
    }

    {
        const auto dbVersion = boost::none;
        const auto shardVersion = shardVersionShardedCollection1;
        ScopedSetShardRole setShardRole(operationContext(), nss, shardVersion, dbVersion);
        auto acquisition = CollectionOrViewAcquisitionRequest::fromOpCtx(
            operationContext(), nss, AcquisitionPrerequisites::kWrite);
        ASSERT_EQ(dbVersion, acquisition.placementConcern.getDbVersion());
        ASSERT_EQ(shardVersion, acquisition.placementConcern.getShardVersion());
    }
}

TEST_F(ShardRoleTest, AcquisitionWithInvalidNamespaceFails) {
    const auto checkAcquisitionByNss = [&](const NamespaceString& nss) {
        // With locks
        ASSERT_THROWS_CODE(acquireCollection(operationContext(),
                                             {nss,
                                              PlacementConcern::kPretendUnsharded,
                                              repl::ReadConcernArgs(),
                                              AcquisitionPrerequisites::kWrite},
                                             MODE_IX),
                           DBException,
                           ErrorCodes::InvalidNamespace);

        // Without locks
        ASSERT_THROWS_CODE(
            acquireCollectionsOrViewsMaybeLockFree(operationContext(),
                                                   {{nss,
                                                     PlacementConcern::kPretendUnsharded,
                                                     repl::ReadConcernArgs(),
                                                     AcquisitionPrerequisites::kRead}}),
            DBException,
            ErrorCodes::InvalidNamespace);
    };

    const auto checkAcquisitionByNssOrUUID = [&](const NamespaceStringOrUUID& nssOrUuid) {
        // With locks
        ASSERT_THROWS_CODE(acquireCollection(operationContext(),
                                             {nssOrUuid,
                                              PlacementConcern::kPretendUnsharded,
                                              repl::ReadConcernArgs(),
                                              AcquisitionPrerequisites::kWrite},
                                             MODE_IX),
                           DBException,
                           ErrorCodes::InvalidNamespace);

        // Without locks
        ASSERT_THROWS_CODE(
            acquireCollectionsOrViewsMaybeLockFree(operationContext(),
                                                   {{nssOrUuid,
                                                     PlacementConcern::kPretendUnsharded,
                                                     repl::ReadConcernArgs(),
                                                     AcquisitionPrerequisites::kRead}}),
            DBException,
            ErrorCodes::InvalidNamespace);
    };

    const NamespaceString nssEmptyCollectionName =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "");
    checkAcquisitionByNss(nssEmptyCollectionName);
    checkAcquisitionByNssOrUUID(nssEmptyCollectionName);

    const NamespaceString nssEmptyDbName =
        NamespaceString::createNamespaceString_forTest("", "foo");
    checkAcquisitionByNss(nssEmptyDbName);
    checkAcquisitionByNssOrUUID(nssEmptyDbName);
    checkAcquisitionByNssOrUUID(NamespaceStringOrUUID(DatabaseName(), UUID::gen()));
}

// ---------------------------------------------------------------------------
// Placement checks when acquiring unsharded collections

TEST_F(ShardRoleTest, AcquireUnshardedCollWithCorrectPlacementVersion) {
    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};

    auto validateAcquisition = [&](auto& acquisition) {
        ASSERT_EQ(nssUnshardedCollection1, acquisition.nss());
        ASSERT_EQ(nssUnshardedCollection1, acquisition.getCollectionPtr()->ns());
        ASSERT_FALSE(acquisition.getShardingDescription().isSharded());
    };

    // With locks.
    {
        const auto acquisition = acquireCollection(operationContext(),
                                                   {nssUnshardedCollection1,
                                                    placementConcern,
                                                    repl::ReadConcernArgs(),
                                                    AcquisitionPrerequisites::kWrite},
                                                   MODE_IX);
        ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                        ->isDbLockedForMode(dbNameTestDb, MODE_IX));
        ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                        ->isCollectionLockedForMode(nssUnshardedCollection1, MODE_IX));
        validateAcquisition(acquisition);
    }

    // Without locks.
    {
        const auto acquisitions = makeAcquisitionMap(
            acquireCollectionsOrViewsMaybeLockFree(operationContext(),
                                                   {{nssUnshardedCollection1,
                                                     placementConcern,
                                                     repl::ReadConcernArgs(),
                                                     AcquisitionPrerequisites::kRead}}));

        ASSERT_EQ(1, acquisitions.size());
        ASSERT_EQ(nssUnshardedCollection1, acquisitions.begin()->first);
        ASSERT_TRUE(acquisitions.at(nssUnshardedCollection1).isCollection());
        const auto& acquisition = acquisitions.at(nssUnshardedCollection1).getCollection();

        ASSERT_FALSE(shard_role_details::getLocker(operationContext())
                         ->isDbLockedForMode(dbNameTestDb, MODE_IS));
        ASSERT_FALSE(shard_role_details::getLocker(operationContext())
                         ->isLockHeldForMode(
                             ResourceId{RESOURCE_COLLECTION, nssUnshardedCollection1}, MODE_IS));
        validateAcquisition(acquisition);
    }
}

TEST_F(ShardRoleTest, AcquireUnshardedCollWithIncorrectPlacementVersionThrows) {
    const auto incorrectDbVersion = DatabaseVersion(UUID::gen(), Timestamp(50, 0));
    PlacementConcern placementConcern{incorrectDbVersion, ShardVersion::UNSHARDED()};

    {
        auto scopedDss = DatabaseShardingStateMock::acquire(operationContext(), dbNameTestDb);
        scopedDss->expectFailureDbVersionCheckWithMismatchingVersion(dbVersionTestDb,
                                                                     incorrectDbVersion);
    }

    auto validateException = [&](const DBException& ex) {
        const auto exInfo = ex.extraInfo<StaleDbRoutingVersion>();
        ASSERT_EQ(dbNameTestDb, exInfo->getDb());
        ASSERT_EQ(incorrectDbVersion, exInfo->getVersionReceived());
        ASSERT_EQ(dbVersionTestDb, exInfo->getVersionWanted());
        ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
    };

    // With locks.
    ASSERT_THROWS_WITH_CHECK(acquireCollection(operationContext(),
                                               {
                                                   nssUnshardedCollection1,
                                                   placementConcern,
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kWrite,
                                               },
                                               MODE_IX),
                             ExceptionFor<ErrorCodes::StaleDbVersion>,
                             validateException);

    // Without locks.
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViewsMaybeLockFree(operationContext(),
                                               {{
                                                   nssUnshardedCollection1,
                                                   placementConcern,
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kRead,
                                               }}),
        ExceptionFor<ErrorCodes::StaleDbVersion>,
        validateException);

    auto scopedDss = DatabaseShardingStateMock::acquire(operationContext(), dbNameTestDb);
    scopedDss->clearExpectedFailureDbVersionCheck();
}

TEST_F(ShardRoleTest, AcquireUnshardedCollWhenShardDoesNotKnowThePlacementVersionThrows) {
    {
        auto scopedDss = DatabaseShardingStateMock::acquire(operationContext(), dbNameTestDb);
        scopedDss->expectFailureDbVersionCheckWithUnknownMetadata(dbVersionTestDb);
    }

    auto validateException = [&](const DBException& ex) {
        const auto exInfo = ex.extraInfo<StaleDbRoutingVersion>();
        ASSERT_EQ(dbNameTestDb, exInfo->getDb());
        ASSERT_EQ(dbVersionTestDb, exInfo->getVersionReceived());
        ASSERT_EQ(boost::none, exInfo->getVersionWanted());
        ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
    };

    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    ASSERT_THROWS_WITH_CHECK(acquireCollection(operationContext(),
                                               {nssUnshardedCollection1,
                                                placementConcern,
                                                repl::ReadConcernArgs(),
                                                AcquisitionPrerequisites::kWrite},
                                               MODE_IX),
                             ExceptionFor<ErrorCodes::StaleDbVersion>,
                             validateException);

    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViewsMaybeLockFree(operationContext(),
                                               {{nssUnshardedCollection1,
                                                 placementConcern,
                                                 repl::ReadConcernArgs(),
                                                 AcquisitionPrerequisites::kRead}}),
        ExceptionFor<ErrorCodes::StaleDbVersion>,
        validateException);

    auto scopedDss = DatabaseShardingStateMock::acquire(operationContext(), dbNameTestDb);
    scopedDss->clearExpectedFailureDbVersionCheck();
}

TEST_F(ShardRoleTest, AcquireUnshardedCollWhenCriticalSectionIsActiveThrows) {
    {
        auto scopedDss = DatabaseShardingStateMock::acquire(operationContext(), dbNameTestDb);
        scopedDss->expectFailureDbVersionCheckWithCriticalSection(dbVersionTestDb, BSONObj());
    }

    {
        PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};

        auto validateException = [&](const DBException& ex) {
            const auto exInfo = ex.extraInfo<StaleDbRoutingVersion>();
            ASSERT_EQ(dbNameTestDb, exInfo->getDb());
            ASSERT_EQ(dbVersionTestDb, exInfo->getVersionReceived());
            ASSERT_EQ(boost::none, exInfo->getVersionWanted());
            ASSERT_TRUE(exInfo->getCriticalSectionSignal().is_initialized());
        };

        ASSERT_THROWS_WITH_CHECK(acquireCollection(operationContext(),
                                                   {nssUnshardedCollection1,
                                                    placementConcern,
                                                    repl::ReadConcernArgs(),
                                                    AcquisitionPrerequisites::kWrite},
                                                   MODE_IX),
                                 ExceptionFor<ErrorCodes::StaleDbVersion>,
                                 validateException);
        ASSERT_THROWS_WITH_CHECK(
            acquireCollectionsOrViewsMaybeLockFree(operationContext(),
                                                   {{nssUnshardedCollection1,
                                                     placementConcern,
                                                     repl::ReadConcernArgs(),
                                                     AcquisitionPrerequisites::kRead}}),
            ExceptionFor<ErrorCodes::StaleDbVersion>,
            validateException);
    }


    // Exit critical section.
    auto scopedDss = DatabaseShardingStateMock::acquire(operationContext(), dbNameTestDb);
    scopedDss->clearExpectedFailureDbVersionCheck();
}

TEST_F(ShardRoleTest, AcquireUnshardedCollWithoutSpecifyingPlacementVersion) {

    auto validateAcquisition = [&](auto& acquisition) {
        ASSERT_EQ(nssUnshardedCollection1, acquisition.nss());
        ASSERT_EQ(nssUnshardedCollection1, acquisition.getCollectionPtr()->ns());
        ASSERT_FALSE(acquisition.getShardingDescription().isSharded());
    };

    // With locks.
    {
        const auto acquisition = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), nssUnshardedCollection1, AcquisitionPrerequisites::kWrite),
            MODE_IX);

        ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                        ->isDbLockedForMode(dbNameTestDb, MODE_IX));
        ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                        ->isCollectionLockedForMode(nssUnshardedCollection1, MODE_IX));
        validateAcquisition(acquisition);
    }

    // Without locks.
    {
        const auto acquisitions = makeAcquisitionMap(acquireCollectionsOrViewsMaybeLockFree(
            operationContext(),
            {CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), nssUnshardedCollection1, AcquisitionPrerequisites::kRead)}));

        ASSERT_EQ(1, acquisitions.size());
        ASSERT_TRUE(acquisitions.at(nssUnshardedCollection1).isCollection());
        const auto& acquisition = acquisitions.at(nssUnshardedCollection1).getCollection();

        ASSERT_FALSE(shard_role_details::getLocker(operationContext())
                         ->isDbLockedForMode(dbNameTestDb, MODE_IS));
        ASSERT_FALSE(shard_role_details::getLocker(operationContext())
                         ->isLockHeldForMode(
                             ResourceId{RESOURCE_COLLECTION, nssUnshardedCollection1}, MODE_IS));
        validateAcquisition(acquisition);
    }
}

TEST_F(ShardRoleTest, AcquireLocalCatalogOnlyWithPotentialDataLossUnsharded) {
    auto acquisition = acquireCollectionForLocalCatalogOnlyWithPotentialDataLoss(
        operationContext(), nssUnshardedCollection1, MODE_IX);

    ASSERT_EQ(nssUnshardedCollection1, acquisition.nss());
    ASSERT_EQ(nssUnshardedCollection1, acquisition.getCollectionPtr()->ns());
}

TEST_F(ShardRoleTest, AcquireLocalCatalogOnlyWithPotentialDataLossSharded) {
    auto acquisition = acquireCollectionForLocalCatalogOnlyWithPotentialDataLoss(
        operationContext(), nssShardedCollection1, MODE_IX);

    ASSERT_EQ(nssShardedCollection1, acquisition.nss());
    ASSERT_EQ(nssShardedCollection1, acquisition.getCollectionPtr()->ns());
}

DEATH_TEST_REGEX_F(ShardRoleTest,
                   AcquireLocalCatalogOnlyWithPotentialDataLossForbiddenToAccessDescription,
                   "Tripwire assertion.*10566704") {
    auto acquisition = acquireCollectionForLocalCatalogOnlyWithPotentialDataLoss(
        operationContext(), nssUnshardedCollection1, MODE_IX);

    (void)acquisition.getShardingDescription();
}

DEATH_TEST_REGEX_F(ShardRoleTest,
                   AcquireLocalCatalogOnlyWithPotentialDataLossForbiddenToAccessFilter,
                   "Tripwire assertion.*7740800") {
    auto acquisition = acquireCollectionForLocalCatalogOnlyWithPotentialDataLoss(
        operationContext(), nssUnshardedCollection1, MODE_IX);

    (void)acquisition.getShardingFilter();
}

// ---------------------------------------------------------------------------
// Placement checks when acquiring sharded collections

TEST_F(ShardRoleTest, AcquireShardedCollWithCorrectPlacementVersion) {
    PlacementConcern placementConcern{{}, shardVersionShardedCollection1};

    auto validateAcquisition = [&](auto& acquisition) {
        ASSERT_EQ(nssShardedCollection1, acquisition.nss());
        ASSERT_EQ(nssShardedCollection1, acquisition.getCollectionPtr()->ns());
        ASSERT_TRUE(acquisition.getShardingDescription().isSharded());
        ASSERT_TRUE(acquisition.getShardingFilter().has_value());
    };

    // With locks.
    {
        const auto acquisition = acquireCollection(operationContext(),
                                                   {nssShardedCollection1,
                                                    placementConcern,
                                                    repl::ReadConcernArgs(),
                                                    AcquisitionPrerequisites::kWrite},
                                                   MODE_IX);
        ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                        ->isDbLockedForMode(dbNameTestDb, MODE_IX));
        ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                        ->isCollectionLockedForMode(nssShardedCollection1, MODE_IX));
        validateAcquisition(acquisition);
    }

    // Without locks.
    {
        const auto acquisitions = makeAcquisitionMap(
            acquireCollectionsOrViewsMaybeLockFree(operationContext(),
                                                   {{nssShardedCollection1,
                                                     placementConcern,
                                                     repl::ReadConcernArgs(),
                                                     AcquisitionPrerequisites::kRead}}));

        ASSERT_EQ(1, acquisitions.size());
        ASSERT_TRUE(acquisitions.at(nssShardedCollection1).isCollection());
        const auto& acquisition = acquisitions.at(nssShardedCollection1).getCollection();

        ASSERT_FALSE(shard_role_details::getLocker(operationContext())
                         ->isDbLockedForMode(dbNameTestDb, MODE_IS));
        ASSERT_FALSE(shard_role_details::getLocker(operationContext())
                         ->isLockHeldForMode(
                             ResourceId{RESOURCE_COLLECTION, nssUnshardedCollection1}, MODE_IS));
        validateAcquisition(acquisition);
    }
}

TEST_F(ShardRoleTest, AcquireShardedCollWithIncorrectPlacementVersionThrows) {
    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};

    auto validateException = [&](const DBException& ex) {
        const auto exInfo = ex.extraInfo<StaleConfigInfo>();
        ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
        ASSERT_EQ(ShardVersion::UNSHARDED(), exInfo->getVersionReceived());
        ASSERT_EQ(shardVersionShardedCollection1, exInfo->getVersionWanted());
        ASSERT_EQ(kMyShardName, exInfo->getShardId());
        ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
    };

    ASSERT_THROWS_WITH_CHECK(acquireCollection(operationContext(),
                                               {
                                                   nssShardedCollection1,
                                                   placementConcern,
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kWrite,
                                               },
                                               MODE_IX),
                             ExceptionFor<ErrorCodes::StaleConfig>,
                             validateException);

    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViewsMaybeLockFree(operationContext(),
                                               {{
                                                   nssShardedCollection1,
                                                   placementConcern,
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kRead,
                                               }}),
        ExceptionFor<ErrorCodes::StaleConfig>,
        validateException);
}

TEST_F(ShardRoleTest, AcquireShardedCollWhenShardDoesNotKnowThePlacementVersionThrows) {
    {
        // Clear the collection filtering metadata on the shard.
        AutoGetCollection coll(operationContext(), nssShardedCollection1, MODE_IX);
        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(operationContext(),
                                                                             nssShardedCollection1)
            ->clearFilteringMetadata(operationContext());
    }

    PlacementConcern placementConcern{{}, shardVersionShardedCollection1};

    auto validateException = [&](const DBException& ex) {
        const auto exInfo = ex.extraInfo<StaleConfigInfo>();
        ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
        ASSERT_EQ(shardVersionShardedCollection1, exInfo->getVersionReceived());
        ASSERT_EQ(boost::none, exInfo->getVersionWanted());
        ASSERT_EQ(kMyShardName, exInfo->getShardId());
        ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
    };

    ASSERT_THROWS_WITH_CHECK(acquireCollection(operationContext(),
                                               {nssShardedCollection1,
                                                placementConcern,
                                                repl::ReadConcernArgs(),
                                                AcquisitionPrerequisites::kWrite},
                                               MODE_IX),
                             ExceptionFor<ErrorCodes::StaleConfig>,
                             validateException);
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViewsMaybeLockFree(operationContext(),
                                               {{nssShardedCollection1,
                                                 placementConcern,
                                                 repl::ReadConcernArgs(),
                                                 AcquisitionPrerequisites::kRead}}),
        ExceptionFor<ErrorCodes::StaleConfig>,
        validateException);
}

TEST_F(ShardRoleTest, AcquireShardedCollWhenCriticalSectionIsActiveThrows) {
    const BSONObj criticalSectionReason = BSON("reason" << 1);
    {
        // Enter the critical section.
        AutoGetCollection coll(operationContext(), nssShardedCollection1, MODE_X);
        const auto& csr = CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
            operationContext(), nssShardedCollection1);
        csr->enterCriticalSectionCatchUpPhase(criticalSectionReason);
        csr->enterCriticalSectionCommitPhase(criticalSectionReason);
    }

    ON_BLOCK_EXIT([&] {
        AutoGetCollection coll(operationContext(), nssShardedCollection1, MODE_X);
        const auto& csr = CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
            operationContext(), nssShardedCollection1);
        csr->exitCriticalSection(criticalSectionReason);
    });

    PlacementConcern placementConcern{{}, shardVersionShardedCollection1};

    auto validateException = [&](const DBException& ex) {
        const auto exInfo = ex.extraInfo<StaleConfigInfo>();
        ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
        ASSERT_EQ(shardVersionShardedCollection1, exInfo->getVersionReceived());
        ASSERT_EQ(boost::none, exInfo->getVersionWanted());
        ASSERT_EQ(kMyShardName, exInfo->getShardId());
        ASSERT_TRUE(exInfo->getCriticalSectionSignal().is_initialized());
    };
    ASSERT_THROWS_WITH_CHECK(acquireCollection(operationContext(),
                                               {nssShardedCollection1,
                                                placementConcern,
                                                repl::ReadConcernArgs(),
                                                AcquisitionPrerequisites::kWrite},
                                               MODE_IX),
                             ExceptionFor<ErrorCodes::StaleConfig>,
                             validateException);
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViewsMaybeLockFree(operationContext(),
                                               {{nssShardedCollection1,
                                                 placementConcern,
                                                 repl::ReadConcernArgs(),
                                                 AcquisitionPrerequisites::kRead}}),
        ExceptionFor<ErrorCodes::StaleConfig>,
        validateException);
}

TEST_F(ShardRoleTest, AcquireShardedCollWithoutSpecifyingPlacementVersion) {
    const auto acquisition = acquireCollection(
        operationContext(),
        CollectionAcquisitionRequest::fromOpCtx(
            operationContext(), nssShardedCollection1, AcquisitionPrerequisites::kWrite),
        MODE_IX);

    ASSERT_EQ(nssShardedCollection1, acquisition.nss());
    ASSERT_EQ(nssShardedCollection1, acquisition.getCollectionPtr()->ns());

    // Note that the collection is treated as unsharded because the operation is unversioned.
    ASSERT_FALSE(acquisition.getShardingDescription().isSharded());
}


TEST_F(ShardRoleTest, AcquireInMultiDocumentTransactionMustCheckForPlacementConflictTime_NoThrow) {
    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    OperationContext* opCtx = operationContext();
    // Simulate a routed request, this necessary for the check to throw.
    ScopedSetShardRole setShardRole(
        opCtx, nssUnshardedCollection1, ShardVersion::UNSHARDED(), dbVersionTestDb);

    // Never throw if we are not a multi-document transaction.
    ASSERT_DOES_NOT_THROW(
        acquireCollection(opCtx,
                          CollectionAcquisitionRequest(nssUnshardedCollection1,
                                                       placementConcern,
                                                       repl::ReadConcernArgs(),
                                                       AcquisitionPrerequisites::kRead),
                          MODE_IS));

    // Setting the operation context in multi-document transaction requires the version to include
    // the PlacementConflictTime
    opCtx->setInMultiDocumentTransaction();
    // Attaching the PlacementConflictTime must now work regularly.
    auto dbVersionDbWithPlacementConflictTime = dbVersionTestDb;
    dbVersionDbWithPlacementConflictTime.setPlacementConflictTime(LogicalTime(Timestamp{0, 0}));
    placementConcern =
        PlacementConcern{dbVersionDbWithPlacementConflictTime, ShardVersion::UNSHARDED()};

    ASSERT_DOES_NOT_THROW(
        acquireCollection(opCtx,
                          CollectionAcquisitionRequest(nssUnshardedCollection1,
                                                       placementConcern,
                                                       repl::ReadConcernArgs(),
                                                       AcquisitionPrerequisites::kRead),
                          MODE_IS));

    // The PlacementConflictTime can also be set on the shardVersion
    auto shardVersionWithPlacementConflictTime = ShardVersion::UNSHARDED();
    shardVersionWithPlacementConflictTime.setPlacementConflictTime(LogicalTime(Timestamp{0, 0}));
    placementConcern = PlacementConcern{dbVersionTestDb, shardVersionWithPlacementConflictTime};
    ASSERT_DOES_NOT_THROW(
        acquireCollection(opCtx,
                          CollectionAcquisitionRequest(nssUnshardedCollection1,
                                                       placementConcern,
                                                       repl::ReadConcernArgs(),
                                                       AcquisitionPrerequisites::kRead),
                          MODE_IS));
}

TEST_F(ShardRoleTest,
       AcquireInMultiDocumentTransactionMissingPlacementConflictTime_excluded_versions) {
    OperationContext* opCtx = operationContext();
    opCtx->setInMultiDocumentTransaction();

    // Do not throw for unrouted operations
    {
        PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
        ScopedSetShardRole setShardRole(opCtx, nssUnshardedCollection1, boost::none, boost::none);
        ASSERT_DOES_NOT_THROW(
            acquireCollection(opCtx,
                              CollectionAcquisitionRequest(nssUnshardedCollection1,
                                                           placementConcern,
                                                           repl::ReadConcernArgs(),
                                                           AcquisitionPrerequisites::kRead),
                              MODE_IS));
    }

    // Do not throw for (boost::none, unsharded)
    {
        PlacementConcern placementConcern{boost::none, ShardVersion::UNSHARDED()};
        ScopedSetShardRole setShardRole(
            opCtx, nssUnshardedCollection1, ShardVersion::UNSHARDED(), dbVersionTestDb);
        ASSERT_DOES_NOT_THROW(
            acquireCollection(opCtx,
                              CollectionAcquisitionRequest(nssUnshardedCollection1,
                                                           placementConcern,
                                                           repl::ReadConcernArgs(),
                                                           AcquisitionPrerequisites::kRead),
                              MODE_IS));
    }

    // Do not throw for (boost::none, ignore)
    {
        PlacementConcern placementConcern{boost::none, shardVersionIgnore};
        ScopedSetShardRole setShardRole(
            opCtx, nssUnshardedCollection1, ShardVersion::UNSHARDED(), dbVersionTestDb);
        ASSERT_DOES_NOT_THROW(
            acquireCollection(opCtx,
                              CollectionAcquisitionRequest(nssUnshardedCollection1,
                                                           placementConcern,
                                                           repl::ReadConcernArgs(),
                                                           AcquisitionPrerequisites::kRead),
                              MODE_IS));
    }
}

DEATH_TEST_REGEX_F(ShardRoleTest,
                   AcquireInMultiDocumentTransactionMissingPlacementConflictTime_version_unsharded,
                   "Tripwire assertion.*10206300") {
    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    OperationContext* opCtx = operationContext();
    // Simulate a routed request, this necessary for the check to throw.
    ScopedSetShardRole setShardRole(
        opCtx, nssUnshardedCollection1, ShardVersion::UNSHARDED(), dbVersionTestDb);
    // Setting the operation context in multi-document transaction requires the version to include
    // the PlacementConflictTime
    opCtx->setInMultiDocumentTransaction();
    acquireCollection(opCtx,
                      CollectionAcquisitionRequest(nssUnshardedCollection1,
                                                   placementConcern,
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kRead),
                      MODE_IS);
}

DEATH_TEST_REGEX_F(ShardRoleTest,
                   AcquireInMultiDocumentTransactionMissingPlacementConflictTime_version_sharded,
                   "Tripwire assertion.*10206300") {
    PlacementConcern placementConcern{dbVersionTestDb, shardVersionShardedCollection1};
    OperationContext* opCtx = operationContext();
    ScopedSetShardRole setShardRole(
        opCtx, nssShardedCollection1, shardVersionShardedCollection1, dbVersionTestDb);
    // Setting the operation context in multi-document transaction requires the version to include
    // the PlacementConflictTime
    opCtx->setInMultiDocumentTransaction();
    ASSERT_THROWS(acquireCollection(opCtx,
                                    CollectionAcquisitionRequest(nssShardedCollection1,
                                                                 placementConcern,
                                                                 repl::ReadConcernArgs(),
                                                                 AcquisitionPrerequisites::kRead),
                                    MODE_IS),
                  unittest::TestAssertionFailureException);
}

// ---------------------------------------------------------------------------
// Acquire inexistent collections

TEST_F(ShardRoleTest, AcquireCollectionNonExistentNamespace) {
    const NamespaceString inexistentNss =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "inexistent");

    // With locks.
    {
        auto acquisition = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), inexistentNss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        ASSERT(!acquisition.getCollectionPtr());
        ASSERT(!acquisition.getShardingDescription().isSharded());
    }

    // Without locks.
    {
        auto acquisitions = makeAcquisitionMap(acquireCollectionsOrViewsMaybeLockFree(
            operationContext(),
            {CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), inexistentNss, AcquisitionPrerequisites::kRead)}));

        ASSERT_EQ(1, acquisitions.size());
        ASSERT_TRUE(acquisitions.at(inexistentNss).isCollection());
        const auto& acquisition = acquisitions.at(inexistentNss).getCollection();

        ASSERT(!acquisition.getCollectionPtr());
        ASSERT(!acquisition.getShardingDescription().isSharded());
    }
}

TEST_F(ShardRoleTest, AcquireInexistentCollectionWithWrongPlacementThrowsBecauseWrongPlacement) {
    const auto incorrectDbVersion = dbVersionTestDb.makeUpdated();

    {
        auto scopedDss = DatabaseShardingStateMock::acquire(operationContext(), dbNameTestDb);
        scopedDss->expectFailureDbVersionCheckWithMismatchingVersion(dbVersionTestDb,
                                                                     incorrectDbVersion);
    }

    const NamespaceString inexistentNss =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "inexistent");

    PlacementConcern placementConcern{incorrectDbVersion, {}};

    auto validateException = [&](const DBException& ex) {
        const auto exInfo = ex.extraInfo<StaleDbRoutingVersion>();
        ASSERT_EQ(dbNameTestDb, exInfo->getDb());
        ASSERT_EQ(incorrectDbVersion, exInfo->getVersionReceived());
        ASSERT_EQ(dbVersionTestDb, exInfo->getVersionWanted());
        ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
    };
    ASSERT_THROWS_WITH_CHECK(acquireCollection(operationContext(),
                                               {inexistentNss,
                                                placementConcern,
                                                repl::ReadConcernArgs(),
                                                AcquisitionPrerequisites::kWrite},
                                               MODE_IX),
                             ExceptionFor<ErrorCodes::StaleDbVersion>,
                             validateException);
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViewsMaybeLockFree(operationContext(),
                                               {{inexistentNss,
                                                 placementConcern,
                                                 repl::ReadConcernArgs(),
                                                 AcquisitionPrerequisites::kRead}}),
        ExceptionFor<ErrorCodes::StaleDbVersion>,
        validateException);

    auto scopedDss = DatabaseShardingStateMock::acquire(operationContext(), dbNameTestDb);
    scopedDss->clearExpectedFailureDbVersionCheck();
}

TEST_F(ShardRoleTest, AcquireCollectionButItIsAView) {
    ASSERT_THROWS_CODE(
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), nssView, AcquisitionPrerequisites::kWrite),
                          MODE_IX),
        DBException,
        ErrorCodes::CommandNotSupportedOnView);

    const auto acquisition =
        acquireCollectionOrView(operationContext(),
                                CollectionOrViewAcquisitionRequest::fromOpCtx(
                                    operationContext(), nssView, AcquisitionPrerequisites::kWrite),
                                MODE_IX);

    ASSERT_TRUE(acquisition.isView());
    const ViewAcquisition& viewAcquisition = acquisition.getView();

    ASSERT_EQ(nssView, viewAcquisition.nss());
    ASSERT_EQ(nssUnshardedCollection1, viewAcquisition.getViewDefinition().viewOn());
    ASSERT(std::equal(viewPipeline.begin(),
                      viewPipeline.end(),
                      viewAcquisition.getViewDefinition().pipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}


TEST_F(ShardRoleTest, WritesOnMultiDocTransactionsUseLatestCatalog) {

    {
        operationContext()->setInMultiDocumentTransaction();
        shard_role_details::getRecoveryUnit(operationContext())->preallocateSnapshot();
        CollectionCatalog::stash(operationContext(), CollectionCatalog::get(operationContext()));
    }

    // Drop a collection
    {
        auto newClient =
            operationContext()->getServiceContext()->getService()->makeClient("AlternativeClient");
        AlternativeClientRegion acr(newClient);
        auto newOpCtx = cc().makeOperationContext();
        DBDirectClient directClient(newOpCtx.get());
        directClient.dropCollection(nssUnshardedCollection1);
    }

    const auto acquireForRead = acquireCollectionOrView(
        operationContext(),
        CollectionOrViewAcquisitionRequest::fromOpCtx(
            operationContext(), nssUnshardedCollection1, AcquisitionPrerequisites::kRead),
        MODE_IX);
    ASSERT_TRUE(acquireForRead.isCollection());

    ASSERT_THROWS_CODE(
        acquireCollectionOrView(
            operationContext(),
            CollectionOrViewAcquisitionRequest::fromOpCtx(
                operationContext(), nssUnshardedCollection1, AcquisitionPrerequisites::kWrite),
            MODE_IX),
        DBException,
        ErrorCodes::WriteConflict);
}

TEST_F(ShardRoleTest, ConflictIsThrownWhenShardVersionUnshardedButStashedCatalogDiffersFromLatest) {
    operationContext()->setInMultiDocumentTransaction();
    shard_role_details::getRecoveryUnit(operationContext())->preallocateSnapshot();
    CollectionCatalog::stash(operationContext(), CollectionCatalog::get(operationContext()));

    // Drop a collection
    {
        auto newClient =
            operationContext()->getServiceContext()->getService()->makeClient("AlternativeClient");
        AlternativeClientRegion acr(newClient);
        auto newOpCtx = cc().makeOperationContext();
        DBDirectClient directClient(newOpCtx.get());
        ASSERT_TRUE(directClient.dropCollection(nssUnshardedCollection1));
    }

    // Try to acquire the now-dropped collection, with declared placement concern
    // ShardVersion::UNSHARDED. Expect a conflict to be detected.
    {
        ScopedSetShardRole setShardRole(
            operationContext(), nssUnshardedCollection1, ShardVersion::UNSHARDED(), boost::none);
        ASSERT_THROWS_CODE(
            acquireCollectionOrView(
                operationContext(),
                CollectionOrViewAcquisitionRequest::fromOpCtx(
                    operationContext(), nssUnshardedCollection1, AcquisitionPrerequisites::kRead),
                MODE_IX),
            DBException,
            ErrorCodes::SnapshotUnavailable);
    }
}

TEST_F(ShardRoleTest, NoExceptionIsThrownWhenShardVersionUnshardedButStashedIsEquivalentToLatest) {
    operationContext()->setInMultiDocumentTransaction();
    shard_role_details::getRecoveryUnit(operationContext())->preallocateSnapshot();
    CollectionCatalog::stash(operationContext(), CollectionCatalog::get(operationContext()));

    // Re-open the local catalog at its latest version, simulating a rollback event that has no
    // effects on the metadata of the unsharded collection (although it does force the creation of a
    // new instance for the "most recent collection snapshot" held by the catalog).
    simulateReplicationRollbackEvent(operationContext());

    // The acquisition of the unsharded collection is expected to succeed, even though the stashed
    // snapshot points to a different object than the one kept by the catalog.
    {
        ScopedSetShardRole setShardRole(
            operationContext(), nssUnshardedCollection1, ShardVersion::UNSHARDED(), boost::none);
        auto acquisition = acquireCollectionOrView(
            operationContext(),
            CollectionOrViewAcquisitionRequest::fromOpCtx(
                operationContext(), nssUnshardedCollection1, AcquisitionPrerequisites::kRead),
            MODE_IX);

        ASSERT_TRUE(acquisition.collectionExists());
    }
}

// ---------------------------------------------------------------------------
// MaybeLockFree
TEST_F(ShardRoleTest, AcquireCollectionMaybeLockFreeTakesLocksWhenInMultiDocTransaction) {
    operationContext()->setInMultiDocumentTransaction();
    auto dbVersionDbWithPlacementConflictTime = dbVersionTestDb;
    dbVersionDbWithPlacementConflictTime.setPlacementConflictTime(LogicalTime(Timestamp{0, 0}));
    const auto acquisition = acquireCollectionMaybeLockFree(
        operationContext(),
        {nssUnshardedCollection1,
         {dbVersionDbWithPlacementConflictTime, ShardVersion::UNSHARDED()},
         repl::ReadConcernArgs(),
         AcquisitionPrerequisites::kRead});
    ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                    ->isCollectionLockedForMode(nssUnshardedCollection1, MODE_IS));
}

TEST_F(ShardRoleTest, AcquireCollectionMaybeLockFreeDoesNotTakeLocksWhenNotInMultiDocTransaction) {
    const auto acquisition =
        acquireCollectionMaybeLockFree(operationContext(),
                                       {nssUnshardedCollection1,
                                        {dbVersionTestDb, ShardVersion::UNSHARDED()},
                                        repl::ReadConcernArgs(),
                                        AcquisitionPrerequisites::kRead});
    ASSERT_FALSE(
        shard_role_details::getLocker(operationContext())
            ->isLockHeldForMode(ResourceId{RESOURCE_COLLECTION, nssUnshardedCollection1}, MODE_IS));
}

DEATH_TEST_REGEX_F(ShardRoleTest,
                   AcquireCollectionMaybeLockFreeAllowedOnlyForRead,
                   "Tripwire assertion") {
    ASSERT_THROWS_CODE(acquireCollectionMaybeLockFree(operationContext(),
                                                      {nssUnshardedCollection1,
                                                       {dbVersionTestDb, ShardVersion::UNSHARDED()},
                                                       repl::ReadConcernArgs(),
                                                       AcquisitionPrerequisites::kWrite}),
                       DBException,
                       7740500);
}

// ---------------------------------------------------------------------------
// Acquire multiple collections

TEST_F(ShardRoleTest, AcquireMultipleCollectionsAllWithCorrectPlacementConcern) {
    const auto acquisitions = makeAcquisitionMap(
        acquireCollections(operationContext(),
                           {{nssUnshardedCollection1,
                             PlacementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()},
                             repl::ReadConcernArgs(),
                             AcquisitionPrerequisites::kWrite},
                            {nssShardedCollection1,
                             PlacementConcern{{}, shardVersionShardedCollection1},
                             repl::ReadConcernArgs(),
                             AcquisitionPrerequisites::kWrite}},
                           MODE_IX));

    ASSERT_EQ(2, acquisitions.size());

    const auto& acquisitionUnshardedColl = acquisitions.at(nssUnshardedCollection1);
    ASSERT_FALSE(acquisitionUnshardedColl.getShardingDescription().isSharded());

    const auto& acquisitionShardedColl = acquisitions.at(nssShardedCollection1);
    ASSERT_TRUE(acquisitionShardedColl.getShardingDescription().isSharded());
    ASSERT_TRUE(acquisitionShardedColl.getShardingFilter().has_value());

    // Assert the DB lock is held, but not recursively (i.e. only once).
    ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                    ->isDbLockedForMode(dbNameTestDb, MODE_IX));
    ASSERT_FALSE(shard_role_details::getLocker(operationContext())->isGlobalLockedRecursively());

    // Assert both collections are locked.
    ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                    ->isCollectionLockedForMode(nssUnshardedCollection1, MODE_IX));
    ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                    ->isCollectionLockedForMode(nssShardedCollection1, MODE_IX));
}

TEST_F(ShardRoleTest, AcquireMultipleCollectionsWithIncorrectPlacementConcernThrows) {
    ASSERT_THROWS_WITH_CHECK(
        acquireCollections(operationContext(),
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
            ASSERT_EQ(kMyShardName, exInfo->getShardId());
            ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
        });
}

DEATH_TEST_REGEX_F(ShardRoleTest,
                   ForbiddenToAcquireMultipleCollectionsOnDifferentDatabases,
                   "Tripwire assertion") {
    ASSERT_THROWS_CODE(
        acquireCollections(
            operationContext(),
            {CollectionAcquisitionRequest::fromOpCtx(
                 operationContext(), nssUnshardedCollection1, AcquisitionPrerequisites::kWrite),
             CollectionAcquisitionRequest::fromOpCtx(
                 operationContext(),
                 NamespaceString::createNamespaceString_forTest("anotherDb", "foo"),
                 AcquisitionPrerequisites::kWrite)},
            MODE_IX),
        DBException,
        7300400);
}

// ---------------------------------------------------------------------------
// Acquire collection by UUID

TEST_F(ShardRoleTest, AcquireCollectionByUUID) {
    const auto uuid = getCollectionUUID(operationContext(), nssUnshardedCollection1);
    const auto acquisition =
        acquireCollection(operationContext(),
                          {NamespaceStringOrUUID(dbNameTestDb, uuid),
                           PlacementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()},
                           repl::ReadConcernArgs(),
                           AcquisitionPrerequisites::kWrite},
                          MODE_IX);

    ASSERT_EQ(nssUnshardedCollection1, acquisition.nss());
    ASSERT_EQ(nssUnshardedCollection1, acquisition.getCollectionPtr()->ns());
}

TEST_F(ShardRoleTest, AcquireCollectionByUUIDButWrongDbNameThrows) {
    const auto uuid = getCollectionUUID(operationContext(), nssUnshardedCollection1);
    ASSERT_THROWS_CODE(
        acquireCollection(
            operationContext(),
            {NamespaceStringOrUUID(
                 DatabaseName::createDatabaseName_forTest(boost::none, "anotherDbName"), uuid),
             PlacementConcern::kPretendUnsharded,
             repl::ReadConcernArgs(),
             AcquisitionPrerequisites::kWrite},
            MODE_IX),
        DBException,
        ErrorCodes::NamespaceNotFound);
}

TEST_F(ShardRoleTest, AcquireCollectionByWrongUUID) {
    const auto uuid = UUID::gen();
    ASSERT_THROWS_CODE(acquireCollection(operationContext(),
                                         {NamespaceStringOrUUID(dbNameTestDb, uuid),
                                          PlacementConcern::kPretendUnsharded,
                                          repl::ReadConcernArgs(),
                                          AcquisitionPrerequisites::kWrite},
                                         MODE_IX),
                       DBException,
                       ErrorCodes::NamespaceNotFound);
}

TEST_F(ShardRoleTest, AcquireCollectionByUUIDInCommitPendingCollection) {
    NamespaceString nss(NamespaceString::createNamespaceString_forTest(dbNameTestDb, "TestColl"));
    const auto uuid = UUID::gen();

    unsigned int numCalls = 0;
    stdx::condition_variable cv;
    stdx::mutex mutex;

    stdx::thread parallelThread([&] {
        ThreadClient client(operationContext()->getService());
        auto newOpCtx = client->makeOperationContext();

        WriteUnitOfWork wuow(newOpCtx.get());

        // Register a hook that will block until the main thread has finished its openCollection
        // lookup.
        auto commitHandler = [&]() {
            stdx::unique_lock lock(mutex);

            // Let the main thread know we have committed to the storage engine.
            numCalls = 1;
            cv.notify_all();

            // Wait until the main thread has finished its openCollection lookup.
            cv.wait(lock, [&numCalls]() { return numCalls == 2; });
        };

        const auto acquisition =
            acquireCollection(newOpCtx.get(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  newOpCtx.get(), nss, AcquisitionPrerequisites::kWrite),
                              MODE_IX);

        // Create the collection
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            newOpCtx.get(), nss);
        CollectionOptions collectionOptions;
        collectionOptions.uuid = uuid;
        auto db = DatabaseHolder::get(newOpCtx.get())->openDb(newOpCtx.get(), nss.dbName());
        db->createCollection(newOpCtx.get(), nss, collectionOptions);

        // The preCommit handler must be registered after the DDL operation so it's executed
        // after any preCommit hooks set up in the operation.
        shard_role_details::getRecoveryUnit(newOpCtx.get())
            ->registerPreCommitHook([&commitHandler](OperationContext* opCtx) { commitHandler(); });

        wuow.commit();
    });

    // Wait for the thread above to start its commit of the DDL operation.
    {
        stdx::unique_lock lock(mutex);
        cv.wait(lock, [&numCalls]() { return numCalls == 1; });
    }

    const NamespaceStringOrUUID nssOrUUID{dbNameTestDb, uuid};
    ASSERT_EQUALS(CollectionCatalog::get(operationContext())
                      ->resolveNamespaceStringOrUUIDWithCommitPendingEntries_UNSAFE(
                          operationContext(), nssOrUUID),
                  nss);

    ASSERT_THROWS_CODE(acquireCollection(operationContext(),
                                         {nssOrUUID,
                                          PlacementConcern::kPretendUnsharded,
                                          repl::ReadConcernArgs(),
                                          AcquisitionPrerequisites::kWrite},
                                         MODE_IX),
                       DBException,
                       ErrorCodes::NamespaceNotFound);

    {
        stdx::unique_lock lock(mutex);
        numCalls = 2;
        cv.notify_all();
    }

    parallelThread.join();
}

TEST_F(ShardRoleTest, AcquireCollectionByUUIDInCommitPendingCollectionAfterDurableCommit) {
    NamespaceString nss(NamespaceString::createNamespaceString_forTest(dbNameTestDb, "TestColl"));
    const auto uuid = UUID::gen();

    unsigned int numCalls = 0;
    stdx::condition_variable cv;
    stdx::mutex mutex;

    stdx::thread parallelThread([&] {
        ThreadClient client(operationContext()->getService());
        auto newOpCtx = client->makeOperationContext();

        WriteUnitOfWork wuow(newOpCtx.get());

        // Register a hook either that will block until the main thread has finished its
        // openCollection lookup.
        auto commitHandler = [&]() {
            stdx::unique_lock lock(mutex);

            // Let the main thread know we have committed to the storage engine.
            numCalls = 1;
            cv.notify_all();

            // Wait until the main thread has finished its openCollection lookup.
            cv.wait(lock, [&numCalls]() { return numCalls == 2; });
        };

        const auto acquisition =
            acquireCollection(newOpCtx.get(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  newOpCtx.get(), nss, AcquisitionPrerequisites::kWrite),
                              MODE_IX);


        shard_role_details::getRecoveryUnit(newOpCtx.get())
            ->onCommit([&commitHandler](OperationContext* opCtx, const auto&) { commitHandler(); });

        // Create the collection
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            newOpCtx.get(), nss);
        CollectionOptions collectionOptions;
        collectionOptions.uuid = uuid;
        auto db = DatabaseHolder::get(newOpCtx.get())->openDb(newOpCtx.get(), nss.dbName());
        db->createCollection(newOpCtx.get(), nss, collectionOptions);

        wuow.commit();
    });

    // Wait for the thread above to start its commit of the DDL operation.
    {
        stdx::unique_lock lock(mutex);
        cv.wait(lock, [&numCalls]() { return numCalls == 1; });
    }

    const NamespaceStringOrUUID nssOrUUID{dbNameTestDb, uuid};
    ASSERT_EQUALS(CollectionCatalog::get(operationContext())
                      ->resolveNamespaceStringOrUUIDWithCommitPendingEntries_UNSAFE(
                          operationContext(), nssOrUUID),
                  nss);

    ASSERT(acquireCollection(operationContext(),
                             {nssOrUUID,
                              PlacementConcern::kPretendUnsharded,
                              repl::ReadConcernArgs(),
                              AcquisitionPrerequisites::kWrite},
                             MODE_IX)
               .exists());

    {
        stdx::unique_lock lock(mutex);
        numCalls = 2;
        cv.notify_all();
    }

    parallelThread.join();
}

TEST_F(ShardRoleTest, AcquireCollectionByUUIDWithShardVersionAttachedThrows) {
    const auto uuid = getCollectionUUID(operationContext(), nssShardedCollection1);
    const auto dbVersion = boost::none;
    const auto shardVersion = shardVersionShardedCollection1;
    ScopedSetShardRole setShardRole(
        operationContext(), nssShardedCollection1, shardVersion, dbVersion);
    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    ASSERT_THROWS_CODE(acquireCollection(operationContext(),
                                         {NamespaceStringOrUUID(dbNameTestDb, uuid),
                                          placementConcern,
                                          repl::ReadConcernArgs(),
                                          AcquisitionPrerequisites::kWrite},
                                         MODE_IX),
                       DBException,
                       ErrorCodes::IncompatibleShardingMetadata);
    ASSERT_THROWS_CODE(
        acquireCollectionsOrViewsMaybeLockFree(operationContext(),
                                               {{NamespaceStringOrUUID(dbNameTestDb, uuid),
                                                 placementConcern,
                                                 repl::ReadConcernArgs(),
                                                 AcquisitionPrerequisites::kRead}}),
        DBException,
        ErrorCodes::IncompatibleShardingMetadata);
}

// ---------------------------------------------------------------------------
// Acquire by nss and expected UUID

TEST_F(ShardRoleTest, AcquireCollectionByNssAndExpectedUUID) {
    const auto uuid = getCollectionUUID(operationContext(), nssUnshardedCollection1);
    const auto acquisition = acquireCollection(operationContext(),
                                               {nssUnshardedCollection1,
                                                uuid,
                                                PlacementConcern::kPretendUnsharded,
                                                repl::ReadConcernArgs(),
                                                AcquisitionPrerequisites::kWrite},
                                               MODE_IX);

    ASSERT_EQ(nssUnshardedCollection1, acquisition.nss());
    ASSERT_EQ(nssUnshardedCollection1, acquisition.getCollectionPtr()->ns());
}

TEST_F(ShardRoleTest, AcquireCollectionByNssAndWrongExpectedUUIDThrows) {
    const auto nss = nssUnshardedCollection1;
    const auto wrongUuid = UUID::gen();

    auto validateException = [&](const DBException& ex) {
        const auto exInfo = ex.extraInfo<CollectionUUIDMismatchInfo>();
        ASSERT_EQ(nss.dbName(), exInfo->dbName());
        ASSERT_EQ(wrongUuid, exInfo->collectionUUID());
        ASSERT_EQ(nss.coll(), exInfo->expectedCollection());
        ASSERT_EQ(boost::none, exInfo->actualCollection());
    };

    ASSERT_THROWS_WITH_CHECK(acquireCollection(operationContext(),
                                               {nss,
                                                wrongUuid,
                                                PlacementConcern::kPretendUnsharded,
                                                repl::ReadConcernArgs(),
                                                AcquisitionPrerequisites::kWrite},
                                               MODE_IX),
                             ExceptionFor<ErrorCodes::CollectionUUIDMismatch>,
                             validateException);
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViewsMaybeLockFree(operationContext(),
                                               {{nss,
                                                 wrongUuid,
                                                 PlacementConcern::kPretendUnsharded,
                                                 repl::ReadConcernArgs(),
                                                 AcquisitionPrerequisites::kRead}}),
        ExceptionFor<ErrorCodes::CollectionUUIDMismatch>,
        validateException);
}

TEST_F(ShardRoleTest, AcquireViewWithExpectedUUIDAlwaysThrows) {
    // Because views don't really have a uuid.
    const auto expectedUUID = UUID::gen();
    ASSERT_THROWS_CODE(acquireCollectionsOrViews(operationContext(),
                                                 {{nssView,
                                                   expectedUUID,
                                                   PlacementConcern::kPretendUnsharded,
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
    ASSERT_THROWS_CODE(acquireCollectionOrView(operationContext(),
                                               {
                                                   nssView,
                                                   PlacementConcern::kPretendUnsharded,
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kWrite,
                                                   AcquisitionPrerequisites::kMustBeCollection,
                                               },
                                               MODE_IX),
                       DBException,
                       ErrorCodes::CommandNotSupportedOnView);

    {
        auto acquisition = acquireCollectionOrView(operationContext(),
                                                   {
                                                       nssView,
                                                       PlacementConcern::kPretendUnsharded,
                                                       repl::ReadConcernArgs(),
                                                       AcquisitionPrerequisites::kWrite,
                                                       AcquisitionPrerequisites::kCanBeView,
                                                   },
                                                   MODE_IX);
        ASSERT_TRUE(acquisition.isView());
    }

    {
        auto acquisition = acquireCollectionOrView(operationContext(),
                                                   {
                                                       nssUnshardedCollection1,
                                                       PlacementConcern::kPretendUnsharded,
                                                       repl::ReadConcernArgs(),
                                                       AcquisitionPrerequisites::kWrite,
                                                       AcquisitionPrerequisites::kCanBeView,
                                                   },
                                                   MODE_IX);
        ASSERT_TRUE(acquisition.isCollection());
    }
}

// ---------------------------------------------------------------------------
// Yield and restore

TEST_F(ShardRoleTest, YieldAndRestoreAcquisitionWithLocks) {
    const auto nss = nssUnshardedCollection1;

    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    const auto acquisition = acquireCollection(operationContext(),
                                               {
                                                   nss,
                                                   placementConcern,
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kWrite,
                                               },
                                               MODE_IX);

    ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                    ->isDbLockedForMode(nss.dbName(), MODE_IX));
    ASSERT_TRUE(
        shard_role_details::getLocker(operationContext())->isCollectionLockedForMode(nss, MODE_IX));

    // Yield the resources
    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(operationContext());
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    ASSERT_FALSE(shard_role_details::getLocker(operationContext())
                     ->isDbLockedForMode(nss.dbName(), MODE_IX));
    ASSERT_FALSE(
        shard_role_details::getLocker(operationContext())->isCollectionLockedForMode(nss, MODE_IX));

    // Restore the resources
    restoreTransactionResourcesToOperationContext(operationContext(),
                                                  std::move(yieldedTransactionResources));
    ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                    ->isDbLockedForMode(nss.dbName(), MODE_IX));
    ASSERT_TRUE(
        shard_role_details::getLocker(operationContext())->isCollectionLockedForMode(nss, MODE_IX));
}

TEST_F(ShardRoleTest, YieldAndRestoreAcquisitionWithoutLocks) {
    const auto nss = nssUnshardedCollection1;

    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    const auto acquisitions = makeAcquisitionMap(
        acquireCollectionsOrViewsMaybeLockFree(operationContext(),
                                               {{
                                                   nss,
                                                   placementConcern,
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kRead,
                                               }}));

    ASSERT_EQ(1, acquisitions.size());
    ASSERT_TRUE(acquisitions.at(nss).isCollection());

    ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                    ->isLockHeldForMode(resourceIdGlobal, MODE_IS));
    ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                    ->isDbLockedForMode(nss.dbName(), MODE_NONE));

    // Yield the resources
    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(operationContext());
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    ASSERT_FALSE(shard_role_details::getLocker(operationContext())
                     ->isLockHeldForMode(resourceIdGlobal, MODE_IS));
    ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                    ->isDbLockedForMode(nss.dbName(), MODE_NONE));

    // Restore the resources
    restoreTransactionResourcesToOperationContext(operationContext(),
                                                  std::move(yieldedTransactionResources));
    ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                    ->isLockHeldForMode(resourceIdGlobal, MODE_IS));
    ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                    ->isDbLockedForMode(nss.dbName(), MODE_NONE));
}

TEST_F(ShardRoleTest, YieldAndRestoreViewAcquisitionWithLocks) {
    const auto opCtx = operationContext();
    const auto nss = nssView;

    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    const auto acquisition = acquireCollectionOrView(opCtx,
                                                     {
                                                         nss,
                                                         placementConcern,
                                                         repl::ReadConcernArgs(),
                                                         AcquisitionPrerequisites::kRead,
                                                     },
                                                     MODE_IS);

    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isDbLockedForMode(nss.dbName(), MODE_IS));
    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_IS));

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx);
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->isDbLockedForMode(nss.dbName(), MODE_IS));
    ASSERT_FALSE(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_IS));

    // Restore the resources
    restoreTransactionResourcesToOperationContext(opCtx, std::move(yieldedTransactionResources));
    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isDbLockedForMode(nss.dbName(), MODE_IS));
    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_IS));

    // Yield the resources
    yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx);
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

    // Change the view definition
    {
        DBDirectClient client(opCtx);
        client.dropCollection(nss);
        createTestView(operationContext(),
                       nssView,
                       nssUnshardedCollection1,
                       {BSON("$match" << BSON("somethingDifferent" << 1))});
    }

    // Attempt to restore. It must fail.
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           opCtx, std::move(yieldedTransactionResources)),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
}

TEST_F(ShardRoleTest, YieldAndRestoreViewAcquisitionWithoutLocks) {
    const auto opCtx = operationContext();
    const auto nss = nssView;

    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    const auto acquisition =
        acquireCollectionOrViewMaybeLockFree(opCtx,
                                             {
                                                 nss,
                                                 placementConcern,
                                                 repl::ReadConcernArgs(),
                                                 AcquisitionPrerequisites::kRead,
                                             });

    ASSERT_TRUE(acquisition.isView());

    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isLockHeldForMode(resourceIdGlobal, MODE_IS));
    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isDbLockedForMode(nss.dbName(), MODE_NONE));

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx);
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

    ASSERT_FALSE(
        shard_role_details::getLocker(opCtx)->isLockHeldForMode(resourceIdGlobal, MODE_IS));
    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isDbLockedForMode(nss.dbName(), MODE_NONE));

    // Restore the resources
    restoreTransactionResourcesToOperationContext(opCtx, std::move(yieldedTransactionResources));
    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isLockHeldForMode(resourceIdGlobal, MODE_IS));
    ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isDbLockedForMode(nss.dbName(), MODE_NONE));

    // Yield the resources
    yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx);
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

    // Change the view definition
    {
        DBDirectClient client(opCtx);
        client.dropCollection(nss);
        createTestView(operationContext(),
                       nssView,
                       nssUnshardedCollection1,
                       {BSON("$match" << BSON("somethingDifferent" << 1))});
    }

    // Attempt to restore. It must fail.
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           opCtx, std::move(yieldedTransactionResources)),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
}

TEST_F(ShardRoleTest,
       RestoreForWriteInvalidatesAcquisitionIfPlacementConcernShardVersionNoLongerMet) {
    const auto nss = nssShardedCollection1;

    PlacementConcern placementConcern{{}, shardVersionShardedCollection1};
    const auto acquisition = acquireCollection(
        operationContext(),
        {nss, placementConcern, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
        MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(operationContext());
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    // Placement changes
    const auto newShardVersion = [&]() {
        auto newPlacementVersion = shardVersionShardedCollection1.placementVersion();
        newPlacementVersion.incMajor();
        return ShardVersionFactory::make(newPlacementVersion);
    }();
    const auto uuid = getCollectionUUID(operationContext(), nss);
    installShardedCollectionMetadata(
        operationContext(),
        nss,
        dbVersionTestDb,
        {ChunkType(uuid,
                   ChunkRange{BSON("skey" << MINKEY), BSON("skey" << MAXKEY)},
                   newShardVersion.placementVersion(),
                   kMyShardName)});

    // Try to restore the resources should fail because placement concern is no longer met.
    ASSERT_THROWS_WITH_CHECK(restoreTransactionResourcesToOperationContext(
                                 operationContext(), std::move(yieldedTransactionResources)),
                             ExceptionFor<ErrorCodes::StaleConfig>,
                             [&](const DBException& ex) {
                                 const auto exInfo = ex.extraInfo<StaleConfigInfo>();
                                 ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
                                 ASSERT_EQ(shardVersionShardedCollection1,
                                           exInfo->getVersionReceived());
                                 ASSERT_EQ(newShardVersion, exInfo->getVersionWanted());
                                 ASSERT_EQ(kMyShardName, exInfo->getShardId());
                                 ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
                             });

    ASSERT_FALSE(shard_role_details::getLocker(operationContext())
                     ->isDbLockedForMode(nss.dbName(), MODE_IX));
    ASSERT_FALSE(
        shard_role_details::getLocker(operationContext())->isCollectionLockedForMode(nss, MODE_IX));
}

TEST_F(ShardRoleTest, RestoreForWriteInvalidatesAcquisitionIfPlacementConcernTimestampChanged) {
    const auto nss = nssShardedCollection1;

    PlacementConcern placementConcern{{}, shardVersionShardedCollection1};
    const auto acquisition = acquireCollection(
        operationContext(),
        {nss, placementConcern, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
        MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(operationContext());
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    // Placement changes
    const auto newShardVersion = [&]() {
        // Advance the collection timestamp and reset the major/minor version
        auto currentCollectionTimestamp =
            shardVersionShardedCollection1.placementVersion().getTimestamp();
        Timestamp newCollectionTimestamp{currentCollectionTimestamp.getSecs() + 100, 0};

        return ShardVersionFactory::make(
            ChunkVersion({OID::gen(), newCollectionTimestamp}, {1, 0}));
    }();
    const auto uuid = getCollectionUUID(operationContext(), nss);
    installShardedCollectionMetadata(
        operationContext(),
        nss,
        dbVersionTestDb,
        {ChunkType(uuid,
                   ChunkRange{BSON("skey" << MINKEY), BSON("skey" << MAXKEY)},
                   newShardVersion.placementVersion(),
                   kMyShardName)});

    // Try to restore the resources should fail because placement concern is no longer met.
    ASSERT_THROWS_WITH_CHECK(restoreTransactionResourcesToOperationContext(
                                 operationContext(), std::move(yieldedTransactionResources)),
                             ExceptionFor<ErrorCodes::StaleConfig>,
                             [&](const DBException& ex) {
                                 const auto exInfo = ex.extraInfo<StaleConfigInfo>();
                                 ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
                                 ASSERT_EQ(shardVersionShardedCollection1,
                                           exInfo->getVersionReceived());
                                 ASSERT_EQ(newShardVersion, exInfo->getVersionWanted());
                                 ASSERT_EQ(kMyShardName, exInfo->getShardId());
                                 ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
                             });

    ASSERT_FALSE(shard_role_details::getLocker(operationContext())
                     ->isDbLockedForMode(nss.dbName(), MODE_IX));
    ASSERT_FALSE(
        shard_role_details::getLocker(operationContext())->isCollectionLockedForMode(nss, MODE_IX));
}

TEST_F(ShardRoleTest, RestoreForWriteInvalidatesAcquisitionIfPlacementConcernDbVersionNoLongerMet) {
    const auto nss = nssUnshardedCollection1;

    PlacementConcern placementConcern{dbVersionTestDb, {}};
    const auto acquisition = acquireCollection(
        operationContext(),
        {nss, placementConcern, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
        MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(operationContext());
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    const auto newDbVersion = dbVersionTestDb.makeUpdated();

    // Placement changes
    {
        auto scopedDss = DatabaseShardingStateMock::acquire(operationContext(),
                                                            nssUnshardedCollection1.dbName());
        scopedDss->expectFailureDbVersionCheckWithMismatchingVersion(newDbVersion, dbVersionTestDb);
    }

    // Try to restore the resources should fail because placement concern is no longer met.
    ASSERT_THROWS_WITH_CHECK(restoreTransactionResourcesToOperationContext(
                                 operationContext(), std::move(yieldedTransactionResources)),
                             ExceptionFor<ErrorCodes::StaleDbVersion>,
                             [&](const DBException& ex) {
                                 const auto exInfo = ex.extraInfo<StaleDbRoutingVersion>();
                                 ASSERT_EQ(nss.dbName(), exInfo->getDb());
                                 ASSERT_EQ(dbVersionTestDb, exInfo->getVersionReceived());
                                 ASSERT_EQ(newDbVersion, exInfo->getVersionWanted());
                                 ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
                             });

    ASSERT_FALSE(shard_role_details::getLocker(operationContext())
                     ->isDbLockedForMode(nss.dbName(), MODE_IX));
    ASSERT_FALSE(
        shard_role_details::getLocker(operationContext())->isCollectionLockedForMode(nss, MODE_IX));

    auto scopedDss =
        DatabaseShardingStateMock::acquire(operationContext(), nssUnshardedCollection1.dbName());
    scopedDss->clearExpectedFailureDbVersionCheck();
}

TEST_F(ShardRoleTest, RestoreWithShardVersionIgnored) {
    const auto nss = nssShardedCollection1;

    PlacementConcern placementConcern{{}, ShardVersionFactory::make(ChunkVersion::IGNORED())};
    const auto acquisition = acquireCollection(operationContext(),
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
    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(operationContext());
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    // Placement changes
    const auto newShardVersion = [&]() {
        auto newPlacementVersion = shardVersionShardedCollection1.placementVersion();
        newPlacementVersion.incMajor();
        return ShardVersionFactory::make(newPlacementVersion);
    }();

    const auto uuid = getCollectionUUID(operationContext(), nss);
    installShardedCollectionMetadata(
        operationContext(),
        nss,
        dbVersionTestDb,
        {ChunkType(uuid,
                   ChunkRange{BSON("skey" << MINKEY), BSON("skey" << MAXKEY)},
                   newShardVersion.placementVersion(),
                   kMyShardName)});

    // Try to restore the resources should work because placement concern (IGNORED) can be met.
    restoreTransactionResourcesToOperationContext(operationContext(),
                                                  std::move(yieldedTransactionResources));
    ASSERT_TRUE(
        shard_role_details::getLocker(operationContext())->isCollectionLockedForMode(nss, MODE_IX));
}

TEST_F(ShardRoleTest, RestoreForWriteJoinsCriticalSectionWhenNotRetryableWrite) {
    const auto nss = nssShardedCollection1;
    const BSONObj criticalSectionReason = BSON("reason" << 1);

    unittest::Barrier barrier(2);
    AtomicWord<bool> restoreCompleted{false};

    stdx::thread parallelThread([&] {
        ThreadClient client(operationContext()->getService());
        auto newUniqueOpCtx = client->makeOperationContext();
        auto newOpCtx = newUniqueOpCtx.get();

        PlacementConcern placementConcern{{}, ShardVersionFactory::make(ChunkVersion::IGNORED())};
        const auto acquisition = acquireCollection(
            newOpCtx,
            {nss, placementConcern, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
            MODE_IX);
        ASSERT_TRUE(acquisition.exists());

        // Yield the resources
        auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(newOpCtx);
        shard_role_details::getRecoveryUnit(newOpCtx)->abandonSnapshot();

        // Activate the critical section
        {
            AutoGetCollection coll(newOpCtx, nssShardedCollection1, MODE_X);
            const auto& csr = CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
                newOpCtx, nssShardedCollection1);
            csr->enterCriticalSectionCatchUpPhase(criticalSectionReason);
            csr->enterCriticalSectionCommitPhase(criticalSectionReason);
        }

        barrier.countDownAndWait();

        // Restore the resources
        ASSERT_DOES_NOT_THROW(restoreTransactionResourcesToOperationContext(
            newOpCtx, std::move(yieldedTransactionResources)));

        restoreCompleted.store(true);
    });

    // Wait for parallelThread to have yielded and activated the critical section.
    barrier.countDownAndWait();

    // Wait a bit for parallelThread. It should not have finished restore yet, because it should be
    // blocked waiting for the critical section to be released.
    auto sleepMillis = 50;
    sleepFor(mongo::Milliseconds{sleepMillis});
    ASSERT_FALSE(restoreCompleted.load());

    // Release the critical section.
    {
        AutoGetCollection coll(operationContext(), nssShardedCollection1, MODE_X);
        const auto& csr = CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
            operationContext(), nssShardedCollection1);
        csr->exitCriticalSection(criticalSectionReason);
    }

    // Now parallel thread should be able to restore and finish.
    parallelThread.join();
    ASSERT_TRUE(restoreCompleted.load());
}

TEST_F(ShardRoleTest, RestoreForWriteDoesNotJoinCriticalSectionWhenRetryableWrite) {
    auto opCtx = operationContext();

    // Setup opCtx state simulating a retryable write.
    opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx->setTxnNumber(0);

    const auto nss = nssShardedCollection1;
    PlacementConcern placementConcern{{}, shardVersionShardedCollection1};
    const auto acquisition = acquireCollection(
        operationContext(),
        {nss, placementConcern, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
        MODE_IX);
    ASSERT_TRUE(acquisition.exists());

    // Yield the resources
    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(operationContext());
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    // Activate the critical section
    const BSONObj criticalSectionReason = BSON("reason" << 1);
    {
        AutoGetCollection coll(operationContext(), nssShardedCollection1, MODE_X);
        const auto& csr = CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
            operationContext(), nssShardedCollection1);
        csr->enterCriticalSectionCatchUpPhase(criticalSectionReason);
        csr->enterCriticalSectionCommitPhase(criticalSectionReason);
    }

    ON_BLOCK_EXIT([&] {
        AutoGetCollection coll(operationContext(), nssShardedCollection1, MODE_X);
        const auto& csr = CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
            operationContext(), nssShardedCollection1);
        csr->exitCriticalSection(criticalSectionReason);
    });

    // Restore the resources
    ASSERT_THROWS_WITH_CHECK(restoreTransactionResourcesToOperationContext(
                                 operationContext(), std::move(yieldedTransactionResources)),
                             ExceptionFor<ErrorCodes::StaleConfig>,
                             [&](const DBException& ex) {
                                 const auto exInfo = ex.extraInfo<StaleConfigInfo>();
                                 ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
                                 ASSERT_TRUE(exInfo->getCriticalSectionSignal().is_initialized());
                             });
}

void ShardRoleTest::testRestoreFailsIfCollectionBecomesCreated(
    AcquisitionPrerequisites::OperationType operationType) {
    NamespaceString nss(NamespaceString::createNamespaceString_forTest(
        dbNameTestDb, "NonExistentCollectionWhichWillBeCreated"));

    const auto acquisition = acquireCollection(
        operationContext(),
        CollectionAcquisitionRequest::fromOpCtx(operationContext(), nss, operationType),
        MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(operationContext());
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    // Create the collection
    createTestCollection(operationContext(), nss);

    // Try to restore the resources should fail because the collection showed-up after a restore
    // where it didn't exist before that.
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           operationContext(), std::move(yieldedTransactionResources)),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
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
    const auto acquisition =
        acquireCollection(operationContext(),
                          {nss, placementConcern, repl::ReadConcernArgs(), operationType},
                          MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(operationContext());
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    // Drop the collection
    {
        DBDirectClient client(operationContext());
        client.dropCollection(nss);
    }

    // Try to restore the resources should fail because the collection no longer exists.
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           operationContext(), std::move(yieldedTransactionResources)),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
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
    const auto acquisition =
        acquireCollection(operationContext(),
                          {nss, placementConcern, repl::ReadConcernArgs(), operationType},
                          MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(operationContext());
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    // Rename the collection.
    {
        auto newClient = getService()->makeClient("AlternativeClient");
        AlternativeClientRegion acr(newClient);
        auto newOpCtxHolder = acr->makeOperationContext();

        DBDirectClient client(newOpCtxHolder.get());
        BSONObj info;
        ASSERT_TRUE(client.runCommand(
            DatabaseName::kAdmin,
            BSON("renameCollection"
                 << nss.ns_forTest() << "to"
                 << NamespaceString::createNamespaceString_forTest(dbNameTestDb, "foo2")
                        .ns_forTest()),
            info));
    }

    // Try to restore the resources should fail because the collection has been renamed.
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           operationContext(), std::move(yieldedTransactionResources)),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
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
    const auto acquisition =
        acquireCollection(operationContext(),
                          {nss, placementConcern, repl::ReadConcernArgs(), operationType},
                          MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(operationContext());
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    // Drop the collection and create a new one with the same nss.
    {
        DBDirectClient client(operationContext());
        client.dropCollection(nss);
        createTestCollection(operationContext(), nss);
    }

    // Try to restore the resources should fail because the collection no longer exists.
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           operationContext(), std::move(yieldedTransactionResources)),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
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
            operationContext(),
            {nss, placementConcern, repl::ReadConcernArgs(), AcquisitionPrerequisites::kRead},
            MODE_IX);

        ongoingQueriesCompletionFuture =
            CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(operationContext(),
                                                                              nss)
                ->getOngoingQueriesCompletionFuture(
                    getCollectionUUID(operationContext(), nss),
                    ChunkRange(BSON("skey" << MINKEY), BSON("skey" << MAXKEY)));

        // Yield the resources
        auto yieldedTransactionResources =
            yieldTransactionResourcesFromOperationContext(operationContext());
        shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

        ASSERT_FALSE(ongoingQueriesCompletionFuture.isReady());
        ASSERT_TRUE(acquisition.getShardingFilter().has_value());
        ASSERT_TRUE(acquisition.getShardingFilter()->keyBelongsToMe(BSON("skey" << 0)));

        // Placement changes
        const auto newShardVersion = [&]() {
            auto newPlacementVersion = shardVersionShardedCollection1.placementVersion();
            newPlacementVersion.incMajor();
            return ShardVersionFactory::make(newPlacementVersion);
        }();

        const auto uuid = getCollectionUUID(operationContext(), nss);
        installShardedCollectionMetadata(
            operationContext(),
            nss,
            dbVersionTestDb,
            {ChunkType(uuid,
                       ChunkRange{BSON("skey" << MINKEY), BSON("skey" << MAXKEY)},
                       newShardVersion.placementVersion(),
                       ShardId("AnotherShard"))});

        // Restore should work for reads even though placement has changed.
        restoreTransactionResourcesToOperationContext(operationContext(),
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

void ShardRoleTest::testRestoreFailsIfCollectionIsNowAView(
    AcquisitionPrerequisites::OperationType operationType) {
    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};

    const auto acquisition = acquireCollection(
        operationContext(),
        {nssUnshardedCollection1, placementConcern, repl::ReadConcernArgs(), operationType},
        MODE_IX);

    // Yield the resources.
    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(operationContext());
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    // Drop collection and create a view in its place.
    {
        DBDirectClient client(operationContext());
        client.dropCollection(nssUnshardedCollection1);
        createTestView(operationContext(), nssUnshardedCollection1, nssShardedCollection1, {});
    }

    // Restore should fail.
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           operationContext(), std::move(yieldedTransactionResources)),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
}
TEST_F(ShardRoleTest, RestoreForReadFailsIfCollectionIsNowAView) {
    testRestoreFailsIfCollectionIsNowAView(AcquisitionPrerequisites::kRead);
}
TEST_F(ShardRoleTest, RestoreForWriteFailsIfCollectionIsNowAView) {
    testRestoreFailsIfCollectionIsNowAView(AcquisitionPrerequisites::kWrite);
}

// Test that collection acquisiton does not change the ReadSource on a secondary when constraints
// are relaxed.
TEST_F(ShardRoleTest, ReadSourceDoesNotChangeOnSecondary) {
    const auto nss = nssUnshardedCollection1;
    ASSERT_OK(repl::ReplicationCoordinator::get(getServiceContext())
                  ->setFollowerMode(repl::MemberState::RS_SECONDARY));

    ASSERT_EQUALS(
        RecoveryUnit::ReadSource::kNoTimestamp,
        shard_role_details::getRecoveryUnit(operationContext())->getTimestampReadSource());

    operationContext()->setEnforceConstraints(false);

    const auto coll = acquireCollection(
        operationContext(),
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(operationContext()),
                                     AcquisitionPrerequisites::kWrite),
        MODE_IX);

    ASSERT_TRUE(coll.exists());

    ASSERT_EQUALS(
        RecoveryUnit::ReadSource::kNoTimestamp,
        shard_role_details::getRecoveryUnit(operationContext())->getTimestampReadSource());
}

TEST_F(ShardRoleTest, WriteAcquisitionsDontChangeReadSourceToLastApplied) {
    const auto nss = nssUnshardedCollection1;

    // Set up secondary read state.
    ASSERT_OK(repl::ReplicationCoordinator::get(getServiceContext())
                  ->setFollowerMode(repl::MemberState::RS_SECONDARY));

    // Initially we start with kNoTimestamp as our ReadSource.
    ASSERT_EQUALS(
        RecoveryUnit::ReadSource::kNoTimestamp,
        shard_role_details::getRecoveryUnit(operationContext())->getTimestampReadSource());

    [[maybe_unused]] const auto acquisitions =
        acquireCollection(operationContext(),
                          {
                              nssUnshardedCollection1,
                              PlacementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()},
                              repl::ReadConcernArgs(),
                              AcquisitionPrerequisites::kUnreplicatedWrite,
                          },
                          MODE_IX);

    // Our read source should be kNoTimestamp
    ASSERT_EQUALS(
        RecoveryUnit::ReadSource::kNoTimestamp,
        shard_role_details::getRecoveryUnit(operationContext())->getTimestampReadSource());

    // Yield the resources
    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(operationContext());

    // Restore the resources
    restoreTransactionResourcesToOperationContext(operationContext(),
                                                  std::move(yieldedTransactionResources));

    // Our read source should still be kNoTimestamp.
    ASSERT_EQUALS(
        RecoveryUnit::ReadSource::kNoTimestamp,
        shard_role_details::getRecoveryUnit(operationContext())->getTimestampReadSource());
}

TEST_F(ShardRoleTest, ReadAcquisitionsChangeReadSourceToLastApplied) {
    for (bool lockFreeAcquisition : {true, false}) {
        const auto nss = nssUnshardedCollection1;

        // Set up secondary read state.
        ASSERT_OK(repl::ReplicationCoordinator::get(getServiceContext())
                      ->setFollowerMode(repl::MemberState::RS_SECONDARY));

        // Initially we start with kNoTimestamp as our ReadSource.
        ASSERT_EQUALS(
            RecoveryUnit::ReadSource::kNoTimestamp,
            shard_role_details::getRecoveryUnit(operationContext())->getTimestampReadSource());

        [[maybe_unused]] const auto acquisitions = !lockFreeAcquisition
            ? acquireCollection(operationContext(),
                                {
                                    nssUnshardedCollection1,
                                    PlacementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()},
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kRead,
                                },
                                MODE_IX)
            : acquireCollectionMaybeLockFree(
                  operationContext(),
                  {
                      nssUnshardedCollection1,
                      PlacementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()},
                      repl::ReadConcernArgs(),
                      AcquisitionPrerequisites::kRead,
                  });

        // Our read source should be kLastApplied
        ASSERT_EQUALS(
            RecoveryUnit::ReadSource::kLastApplied,
            shard_role_details::getRecoveryUnit(operationContext())->getTimestampReadSource());

        // Yield the resources
        auto yieldedTransactionResources =
            yieldTransactionResourcesFromOperationContext(operationContext());

        // Restore the resources
        restoreTransactionResourcesToOperationContext(operationContext(),
                                                      std::move(yieldedTransactionResources));

        // Our read source should still be kLastApplied.
        ASSERT_EQUALS(
            RecoveryUnit::ReadSource::kLastApplied,
            shard_role_details::getRecoveryUnit(operationContext())->getTimestampReadSource());
    }
}

TEST_F(ShardRoleTest, RestoreChangesReadSourceAfterStepUp) {
    const auto nss = nssShardedCollection1;

    // Set up secondary read state.
    ASSERT_OK(repl::ReplicationCoordinator::get(getServiceContext())
                  ->setFollowerMode(repl::MemberState::RS_SECONDARY));

    // Initially we start with kNoTimestamp as our ReadSource.
    ASSERT_EQUALS(
        RecoveryUnit::ReadSource::kNoTimestamp,
        shard_role_details::getRecoveryUnit(operationContext())->getTimestampReadSource());

    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    [[maybe_unused]] const auto acquisitions =
        acquireCollectionsOrViewsMaybeLockFree(operationContext(),
                                               {{
                                                   nssUnshardedCollection1,
                                                   placementConcern,
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kRead,
                                               }});

    // Our read source should have been updated to kLastApplied.
    ASSERT_EQUALS(
        RecoveryUnit::ReadSource::kLastApplied,
        shard_role_details::getRecoveryUnit(operationContext())->getTimestampReadSource());

    // Yield the resources
    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(operationContext());
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    // Step up.
    ASSERT_OK(repl::ReplicationCoordinator::get(getServiceContext())
                  ->setFollowerMode(repl::MemberState::RS_PRIMARY));

    // Restore the resources
    restoreTransactionResourcesToOperationContext(operationContext(),
                                                  std::move(yieldedTransactionResources));

    // Our read source should have been updated to kNoTimestamp.
    ASSERT_EQUALS(
        RecoveryUnit::ReadSource::kNoTimestamp,
        shard_role_details::getRecoveryUnit(operationContext())->getTimestampReadSource());
}

TEST_F(ShardRoleTest, RestoreCollectionCreatedUnderScopedLocalCatalogWriteFence) {
    const auto nss = NamespaceString::createNamespaceString_forTest(dbNameTestDb, "inexistent");
    auto acquisition = acquireCollection(
        operationContext(),
        {nss, PlacementConcern{{}, {}}, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
        MODE_IX);
    ASSERT_FALSE(acquisition.exists());

    // Create the collection
    {
        WriteUnitOfWork wuow(operationContext());
        ScopedLocalCatalogWriteFence scopedLocalCatalogWriteFence(operationContext(), &acquisition);
        createTestCollection(operationContext(), nss);
        wuow.commit();
    }
    ASSERT_TRUE(acquisition.exists());

    // Yield
    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(operationContext());

    // Restore works
    restoreTransactionResourcesToOperationContext(operationContext(),
                                                  std::move(yieldedTransactionResources));
}

TEST_F(ShardRoleTest,
       RestoreCollectionCreatedUnderScopedLocalCatalogWriteFenceFailsIfNoLongerExists) {
    const auto nss = NamespaceString::createNamespaceString_forTest(dbNameTestDb, "inexistent");
    auto acquisition = acquireCollection(
        operationContext(),
        {nss, PlacementConcern{{}, {}}, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
        MODE_IX);
    ASSERT_FALSE(acquisition.exists());

    // Create the collection
    {
        WriteUnitOfWork wuow(operationContext());
        ScopedLocalCatalogWriteFence scopedLocalCatalogWriteFence(operationContext(), &acquisition);
        createTestCollection(operationContext(), nss);
        wuow.commit();
    }
    ASSERT_TRUE(acquisition.exists());

    // Yield
    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(operationContext());

    // Drop the collection
    DBDirectClient client(operationContext());
    client.dropCollection(nss);

    // Restore should fail
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           operationContext(), std::move(yieldedTransactionResources)),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
}

// ---------------------------------------------------------------------------
// External yields

TEST_F(ShardRoleTest, YieldAndRestoreCursor) {
    const auto& nss = nssUnshardedCollection1;
    auto cursorId = [&] {
        PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};

        const auto acquisition = acquireCollection(operationContext(),
                                                   {nssUnshardedCollection1,
                                                    placementConcern,
                                                    repl::ReadConcernArgs(),
                                                    AcquisitionPrerequisites::kWrite},
                                                   MODE_IX);

        auto params = std::make_unique<DeleteStageParams>();
        auto exec =
            InternalPlanner::deleteWithCollectionScan(operationContext(),
                                                      acquisition,
                                                      std::move(params),
                                                      PlanYieldPolicy::YieldPolicy::YIELD_AUTO);

        auto pinnedCursor =
            CursorManager::get(operationContext())
                ->registerCursor(
                    operationContext(),
                    {std::move(exec),
                     nss,
                     AuthorizationSession::get(operationContext()->getClient())
                         ->getAuthenticatedUserName(),
                     APIParameters::get(operationContext()),
                     operationContext()->getWriteConcern(),
                     repl::ReadConcernArgs::get(operationContext()),
                     ReadPreferenceSetting::get(operationContext()),
                     BSON("x" << 1),
                     {Privilege(ResourcePattern::forExactNamespace(nss), ActionType::find)}});
        PlanExecutor* cursorExec = pinnedCursor->getExecutor();

        ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                        ->isDbLockedForMode(nss.dbName(), MODE_IX));
        ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                        ->isCollectionLockedForMode(nss, MODE_IX));

        // State will be restored on getMore.
        cursorExec->saveState();
        cursorExec->detachFromOperationContext();

        stashTransactionResourcesFromOperationContext(operationContext(), pinnedCursor.getCursor());

        ASSERT_FALSE(shard_role_details::getLocker(operationContext())
                         ->isDbLockedForMode(nss.dbName(), MODE_IX));
        ASSERT_FALSE(shard_role_details::getLocker(operationContext())
                         ->isCollectionLockedForMode(nss, MODE_IX));

        return pinnedCursor->cursorid();
    }();

    // Become a different operation so we can imitate the behaviour of getMore.
    auto newClient = getService()->makeClient("AlternativeClient");
    AlternativeClientRegion acr(newClient);
    auto newOpCtxHolder = acr->makeOperationContext();
    auto* newOpCtx = newOpCtxHolder.get();

    auto clientCursorPin =
        assertGet(CursorManager::get(newOpCtx)->pinCursor(newOpCtx, cursorId, "getMore"));

    ON_BLOCK_EXIT([&] {
        ASSERT_FALSE(
            shard_role_details::getLocker(newOpCtx)->isDbLockedForMode(nss.dbName(), MODE_IX));
        ASSERT_FALSE(
            shard_role_details::getLocker(newOpCtx)->isCollectionLockedForMode(nss, MODE_IX));
    });

    HandleTransactionResourcesFromStasher handler(newOpCtx, clientCursorPin.getCursor());

    ASSERT_TRUE(shard_role_details::getLocker(newOpCtx)->isDbLockedForMode(nss.dbName(), MODE_IX));
    ASSERT_TRUE(shard_role_details::getLocker(newOpCtx)->isCollectionLockedForMode(nss, MODE_IX));
}

// ---------------------------------------------------------------------------
// Storage snapshot

TEST_F(ShardRoleTest, SnapshotAttemptFailsIfReplTermChanges) {
    const auto nss = nssShardedCollection1;

    PlacementConcern placementConcern = PlacementConcern{{}, shardVersionShardedCollection1};

    NamespaceStringOrUUIDRequests requests = {{nss}};
    shard_role_details::SnapshotAttempt snapshotAttempt(operationContext(), requests);
    snapshotAttempt.snapshotInitialState();
    snapshotAttempt.changeReadSourceForSecondaryReads();
    snapshotAttempt.openStorageSnapshot();

    auto currentTerm = repl::ReplicationCoordinator::get(operationContext())->getTerm();
    ASSERT_OK(repl::ReplicationCoordinator::get(operationContext())
                  ->updateTerm(operationContext(), currentTerm + 1));

    ASSERT_FALSE(snapshotAttempt.getConsistentCatalog());
}

TEST_F(ShardRoleTest, SnapshotAttemptFailsIfCatalogChanges) {
    const auto nss = nssShardedCollection1;

    PlacementConcern placementConcern = PlacementConcern{{}, shardVersionShardedCollection1};

    NamespaceStringOrUUIDRequests requests = {{nss}};
    shard_role_details::SnapshotAttempt snapshotAttempt(operationContext(), requests);
    snapshotAttempt.snapshotInitialState();
    snapshotAttempt.changeReadSourceForSecondaryReads();
    snapshotAttempt.openStorageSnapshot();

    auto nss2 = NamespaceString::createNamespaceString_forTest(dbNameTestDb, "newCollection");
    createTestCollection(operationContext(), nss2);

    ASSERT_FALSE(snapshotAttempt.getConsistentCatalog());
}

TEST_F(ShardRoleTest, ReadSourceChangesOnSecondary) {
    const auto nss = nssShardedCollection1;

    // Set up secondary read state.
    operationContext()->getClient()->setInDirectClient(true);
    ASSERT_OK(repl::ReplicationCoordinator::get(getServiceContext())
                  ->setFollowerMode(repl::MemberState::RS_SECONDARY));

    // Initially we start with kNoTimestamp as our ReadSource.
    ASSERT_EQUALS(
        RecoveryUnit::ReadSource::kNoTimestamp,
        shard_role_details::getRecoveryUnit(operationContext())->getTimestampReadSource());

    PlacementConcern placementConcern = PlacementConcern{{}, shardVersionShardedCollection1};
    NamespaceStringOrUUIDRequests requests = {{nss}};
    shard_role_details::SnapshotAttempt snapshotAttempt(operationContext(), requests);
    snapshotAttempt.snapshotInitialState();
    snapshotAttempt.changeReadSourceForSecondaryReads();

    // Our read source should have been updated to kLastApplied.
    ASSERT_EQUALS(
        RecoveryUnit::ReadSource::kLastApplied,
        shard_role_details::getRecoveryUnit(operationContext())->getTimestampReadSource());

    snapshotAttempt.openStorageSnapshot();
    ASSERT_TRUE(snapshotAttempt.getConsistentCatalog());
}

// ---------------------------------------------------------------------------
// ScopedLocalCatalogWriteFence

TEST_F(ShardRoleTest, ScopedLocalCatalogWriteFenceWUOWCommitWithinWriterScope) {
    auto acquisition = acquireCollection(operationContext(),
                                         {nssShardedCollection1,
                                          PlacementConcern{{}, shardVersionShardedCollection1},
                                          repl::ReadConcernArgs(),
                                          AcquisitionPrerequisites::kRead},
                                         MODE_X);
    ASSERT(!acquisition.getCollectionPtr()->isTemporary());

    {
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter localCatalogWriter(operationContext(), &acquisition);
        localCatalogWriter.getWritableCollection(operationContext())
            ->setIsTemp(operationContext(), true);
        wuow.commit();
    }

    ASSERT(acquisition.getCollectionPtr()->isTemporary());
}

TEST_F(ShardRoleTest, ScopedLocalCatalogWriteFenceWUOWCommitAfterWriterScope) {
    auto acquisition = acquireCollection(operationContext(),
                                         {nssShardedCollection1,
                                          PlacementConcern{{}, shardVersionShardedCollection1},
                                          repl::ReadConcernArgs(),
                                          AcquisitionPrerequisites::kRead},
                                         MODE_X);
    ASSERT(!acquisition.getCollectionPtr()->isTemporary());

    WriteUnitOfWork wuow(operationContext());
    {
        CollectionWriter localCatalogWriter(operationContext(), &acquisition);
        localCatalogWriter.getWritableCollection(operationContext())
            ->setIsTemp(operationContext(), true);
    }
    ASSERT(acquisition.getCollectionPtr()->isTemporary());
    wuow.commit();
    ASSERT(acquisition.getCollectionPtr()->isTemporary());
}

TEST_F(ShardRoleTest, ScopedLocalCatalogWriteFenceOutsideWUOUCommit) {
    auto acquisition = acquireCollection(operationContext(),
                                         {nssShardedCollection1,
                                          PlacementConcern{{}, shardVersionShardedCollection1},
                                          repl::ReadConcernArgs(),
                                          AcquisitionPrerequisites::kRead},
                                         MODE_X);
    ASSERT(!acquisition.getCollectionPtr()->isTemporary());

    {
        CollectionWriter localCatalogWriter(operationContext(), &acquisition);
        WriteUnitOfWork wuow(operationContext());
        localCatalogWriter.getWritableCollection(operationContext())
            ->setIsTemp(operationContext(), true);
        ASSERT(localCatalogWriter->isTemporary());
        wuow.commit();
        ASSERT(localCatalogWriter->isTemporary());
    }
    ASSERT(acquisition.getCollectionPtr()->isTemporary());
}

TEST_F(ShardRoleTest, ScopedLocalCatalogWriteFenceOutsideWUOURollback) {
    auto acquisition = acquireCollection(operationContext(),
                                         {nssShardedCollection1,
                                          PlacementConcern{{}, shardVersionShardedCollection1},
                                          repl::ReadConcernArgs(),
                                          AcquisitionPrerequisites::kRead},
                                         MODE_X);
    ASSERT(!acquisition.getCollectionPtr()->isTemporary());

    {
        CollectionWriter localCatalogWriter(operationContext(), &acquisition);
        {
            WriteUnitOfWork wuow(operationContext());
            localCatalogWriter.getWritableCollection(operationContext())
                ->setIsTemp(operationContext(), true);
            ASSERT(localCatalogWriter->isTemporary());
        }
        ASSERT(!localCatalogWriter->isTemporary());
    }
    ASSERT(!acquisition.getCollectionPtr()->isTemporary());
}

TEST_F(ShardRoleTest, ScopedLocalCatalogWriteFenceWUOWRollbackAfterAcquisitionOutOfScope) {
    // Tests that nothing breaks if ScopedLocalCatalogWriteFence's onRollback handler is executed
    // when the collection acquisition has already gone out of scope.
    WriteUnitOfWork wuow1(operationContext());
    {
        auto acquisition = acquireCollection(operationContext(),
                                             {nssShardedCollection1,
                                              PlacementConcern{{}, shardVersionShardedCollection1},
                                              repl::ReadConcernArgs(),
                                              AcquisitionPrerequisites::kRead},
                                             MODE_IX);
        ScopedLocalCatalogWriteFence(operationContext(), &acquisition);
    }
}

TEST_F(ShardRoleTest, ScopedLocalCatalogWriteFenceWUOWRollbackAfterANotherClientCreatedCollection) {
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "inexistent");

    // Acquire a collection that does not exist.
    auto acquisition = acquireCollection(
        operationContext(),
        {nss, PlacementConcern{{}, {}}, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
        MODE_IX);
    ASSERT_FALSE(acquisition.exists());

    // Another client creates the collection
    {
        auto newClient = operationContext()->getServiceContext()->getService()->makeClient(
            "MigrationCoordinator");
        auto newOpCtx = newClient->makeOperationContext();
        createTestCollection(newOpCtx.get(), nss);
    }

    // Acquisition still reflects that the collection does not exist.
    ASSERT_FALSE(acquisition.exists());

    // Original client attempts to create the collection, which will result in a WriteConflict and
    // rollback.
    {
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            operationContext(), nss);

        WriteUnitOfWork wuow(operationContext());
        ScopedLocalCatalogWriteFence localCatalogWriteFence(operationContext(), &acquisition);
        auto db = DatabaseHolder::get(operationContext())->openDb(operationContext(), nss.dbName());
        db->createCollection(operationContext(), nss, CollectionOptions());
        ASSERT_THROWS_CODE(wuow.commit(), DBException, ErrorCodes::WriteConflict);
    }

    // Check that after rollback the acquisition has been updated to reflect the latest state of the
    // catalog (i.e. the collection exists).
    ASSERT_TRUE(acquisition.exists());
}

DEATH_TEST_F(ShardRoleTest,
             CannotAcquireWhileYielded,
             "Cannot obtain TransactionResources as they've been detached from the opCtx") {
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "inexistent");

    // Acquire a collection
    auto acquisition = acquireCollection(
        operationContext(),
        {nss, PlacementConcern{{}, {}}, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
        MODE_IX);

    auto yielded = yieldTransactionResourcesFromOperationContext(operationContext());

    const auto otherNss = NamespaceString::createNamespaceString_forTest(dbNameTestDb, "otherNss");
    acquireCollection(operationContext(),
                      {otherNss,
                       PlacementConcern{{}, {}},
                       repl::ReadConcernArgs(),
                       AcquisitionPrerequisites::kWrite},
                      MODE_IX);
}

DEATH_TEST_F(ShardRoleTest,
             FailedStateCannotAcceptAcquisitions,
             "Cannot make a new acquisition in the FAILED state") {
    const auto nss = nssShardedCollection1;

    PlacementConcern placementConcern{{}, shardVersionShardedCollection1};
    const auto acquisition = acquireCollection(
        operationContext(),
        {nss, placementConcern, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
        MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(operationContext());
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    // Placement changes
    const auto newShardVersion = [&]() {
        auto newPlacementVersion = shardVersionShardedCollection1.placementVersion();
        newPlacementVersion.incMajor();
        return ShardVersionFactory::make(newPlacementVersion);
    }();
    const auto uuid = getCollectionUUID(operationContext(), nss);
    installShardedCollectionMetadata(
        operationContext(),
        nss,
        dbVersionTestDb,
        {ChunkType(uuid,
                   ChunkRange{BSON("skey" << MINKEY), BSON("skey" << MAXKEY)},
                   newShardVersion.placementVersion(),
                   kMyShardName)});

    // Try to restore the resources should fail because placement concern is no longer met.
    ASSERT_THROWS_WITH_CHECK(restoreTransactionResourcesToOperationContext(
                                 operationContext(), std::move(yieldedTransactionResources)),
                             ExceptionFor<ErrorCodes::StaleConfig>,
                             [&](const DBException& ex) {
                                 const auto exInfo = ex.extraInfo<StaleConfigInfo>();
                                 ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
                                 ASSERT_EQ(shardVersionShardedCollection1,
                                           exInfo->getVersionReceived());
                                 ASSERT_EQ(newShardVersion, exInfo->getVersionWanted());
                                 ASSERT_EQ(kMyShardName, exInfo->getShardId());
                                 ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
                             });

    const NamespaceString otherNss =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "inexistent");

    // Trying to acquire now should invariant and crash the server since we're in the FAILED state.
    acquireCollection(operationContext(),
                      {otherNss,
                       PlacementConcern{{}, {}},
                       repl::ReadConcernArgs(),
                       AcquisitionPrerequisites::kWrite},
                      MODE_IX);
}

// ---------------------------------------------------------------------------
// Recursive locking
TEST_F(ShardRoleTest,
       acquiringMultipleCollectionsInSameAcquisitionDoesNotRecursivelyLockGlobalLock) {
    const auto testFn = [&](bool withLocks) {
        const auto acquisitionRequest1 =
            CollectionAcquisitionRequest(nssUnshardedCollection1,
                                         PlacementConcern::kPretendUnsharded,
                                         repl::ReadConcernArgs(),
                                         AcquisitionPrerequisites::kRead);

        const auto acquisitionRequest2 =
            CollectionAcquisitionRequest(nssShardedCollection1,
                                         PlacementConcern::kPretendUnsharded,
                                         repl::ReadConcernArgs(),
                                         AcquisitionPrerequisites::kRead);

        // Acquire two collections in the same acquisition.
        boost::optional<CollectionAcquisition> acquisition1;
        boost::optional<CollectionAcquisition> acquisition2;
        {
            CollectionAcquisitionRequests requests{acquisitionRequest1, acquisitionRequest2};
            CollectionAcquisitionMap acquisitions = makeAcquisitionMap(
                withLocks ? acquireCollections(operationContext(), requests, MODE_IS)
                          : acquireCollectionsMaybeLockFree(operationContext(), requests));
            acquisition1 = acquisitions.at(acquisitionRequest1.nssOrUUID.nss());
            acquisition2 = acquisitions.at(acquisitionRequest2.nssOrUUID.nss());
        }

        // Check not recursively locked.
        ASSERT_TRUE(
            shard_role_details::getLocker(operationContext())->isReadLocked());  // Global lock
        ASSERT_FALSE(
            shard_role_details::getLocker(operationContext())->isGlobalLockedRecursively());
        if (!withLocks) {
            ASSERT_TRUE(operationContext()->isLockFreeReadsOp());
        }

        // Release one acquisition. Global lock still held.
        acquisition1.reset();
        ASSERT_TRUE(
            shard_role_details::getLocker(operationContext())->isReadLocked());  // Global lock
        ASSERT_FALSE(
            shard_role_details::getLocker(operationContext())->isGlobalLockedRecursively());
        if (!withLocks) {
            ASSERT_TRUE(operationContext()->isLockFreeReadsOp());
        }

        // Release the other acquisition. Global lock no longer held.
        acquisition2.reset();
        ASSERT_FALSE(
            shard_role_details::getLocker(operationContext())->isReadLocked());  // Global lock
        ASSERT_FALSE(
            shard_role_details::getLocker(operationContext())->isGlobalLockedRecursively());
        if (!withLocks) {
            ASSERT_FALSE(operationContext()->isLockFreeReadsOp());
        }
    };

    testFn(true);   // with locks
    testFn(false);  // lock-free
}

TEST_F(ShardRoleTest,
       acquiringMultipleCollectionsInDifferentAcquisitionsDoesRecursivelyLocksGlobalLock) {
    const auto testFn = [&](bool withLocks) {
        const auto acquisitionRequest1 =
            CollectionAcquisitionRequest(nssUnshardedCollection1,
                                         PlacementConcern::kPretendUnsharded,
                                         repl::ReadConcernArgs(),
                                         AcquisitionPrerequisites::kRead);

        const auto acquisitionRequest2 =
            CollectionAcquisitionRequest(nssShardedCollection1,
                                         PlacementConcern::kPretendUnsharded,
                                         repl::ReadConcernArgs(),
                                         AcquisitionPrerequisites::kRead);

        // Acquire two collections in different acquisitions.
        boost::optional<CollectionAcquisition> acquisition1 = withLocks
            ? acquireCollection(operationContext(), acquisitionRequest1, MODE_IS)
            : acquireCollectionMaybeLockFree(operationContext(), acquisitionRequest1);

        boost::optional<CollectionAcquisition> acquisition2 = withLocks
            ? acquireCollection(operationContext(), acquisitionRequest2, MODE_IS)
            : acquireCollectionMaybeLockFree(operationContext(), acquisitionRequest2);

        // Check locked recursively.
        ASSERT_TRUE(
            shard_role_details::getLocker(operationContext())->isReadLocked());  // Global lock
        ASSERT_TRUE(shard_role_details::getLocker(operationContext())->isGlobalLockedRecursively());
        if (!withLocks) {
            ASSERT_TRUE(operationContext()->isLockFreeReadsOp());
        }

        // Release one acquisition. Check no longer locked recursively.
        acquisition1.reset();
        ASSERT_TRUE(
            shard_role_details::getLocker(operationContext())->isReadLocked());  // Global lock
        ASSERT_FALSE(
            shard_role_details::getLocker(operationContext())->isGlobalLockedRecursively());
        if (!withLocks) {
            ASSERT_TRUE(operationContext()->isLockFreeReadsOp());
        }

        // Release the other acquisition. Check no locks are held.
        acquisition2.reset();
        ASSERT_FALSE(
            shard_role_details::getLocker(operationContext())->isReadLocked());  // Global lock
        ASSERT_FALSE(
            shard_role_details::getLocker(operationContext())->isGlobalLockedRecursively());
        if (!withLocks) {
            ASSERT_FALSE(operationContext()->isLockFreeReadsOp());
        }
    };

    testFn(true);   // with locks
    testFn(false);  // lock-free
}

TEST_F(ShardRoleTest, restoreKillsTransactionOnCatalogEpochChange) {
    PlacementConcern placementConcern{{}, shardVersionShardedCollection1};

    for (int i = 0; i < 2; i++) {
        auto opCtx = operationContext();
        // Test multiple acquisition type
        const auto acquisition = [&]() {
            switch (i) {
                case 0: {
                    return acquireCollection(opCtx,
                                             {nssShardedCollection1,
                                              placementConcern,
                                              repl::ReadConcernArgs(),
                                              AcquisitionPrerequisites::kWrite},
                                             MODE_IX);
                }
                case 1: {
                    return acquireCollectionMaybeLockFree(opCtx,
                                                          {nssShardedCollection1,
                                                           placementConcern,
                                                           repl::ReadConcernArgs(),
                                                           AcquisitionPrerequisites::kRead});
                }
                default:
                    MONGO_UNREACHABLE;
            }
        }();

        // 1. Yield and restore the resources works as expected
        auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx);

        ASSERT_DOES_NOT_THROW(restoreTransactionResourcesToOperationContext(
            opCtx, std::move(yieldedTransactionResources)));

        // 2. A replication rollback event happening in between a yield and a restore will cause the
        // query to terminate.
        auto yieldedTransactionResources2 = yieldTransactionResourcesFromOperationContext(opCtx);
        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

        // Simulate a replication rollback event (which causes the collection catalog to close and
        // re-open and the epoch to change)
        simulateReplicationRollbackEvent(opCtx);

        // Try to restore the resources should fail because the catalog epoch changed.
        ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                               opCtx, std::move(yieldedTransactionResources2)),
                           DBException,
                           ErrorCodes::QueryPlanKilled);
    }
}

TEST_F(ShardRoleTest, RestoreFailsWhenMetadataInvalidated) {
    testRestoreFailsWhenMetadataInvalidated(true, true, true);
    testRestoreFailsWhenMetadataInvalidated(false, true, false);
    testRestoreFailsWhenMetadataInvalidated(true, false, false);
}

void ShardRoleTest::testRestoreFailsWhenMetadataInvalidated(
    bool terminateSecondaryReadsUponRangeDeletion,
    bool terminateSecondaryReadsOnOrphan,
    bool mustFail) {
    RAIIServerParameterControllerForTest featureFlagController{
        "featureFlagTerminateSecondaryReadsUponRangeDeletion",
        terminateSecondaryReadsUponRangeDeletion};

    terminateSecondaryReadsOnOrphanCleanup.store(terminateSecondaryReadsOnOrphan);

    const auto nss = nssShardedCollection1;

    PlacementConcern placementConcern{{}, shardVersionShardedCollection1};

    const auto acquisition = acquireCollection(
        operationContext(),
        {nss, placementConcern, repl::ReadConcernArgs(), AcquisitionPrerequisites::kRead},
        MODE_IX);

    ASSERT_TRUE(acquisition.getShardingFilter().has_value());
    ASSERT_TRUE(acquisition.getShardingFilter()->keyBelongsToMe(BSON("skey" << 0)));

    // Yield the resources
    auto yieldedTransactionResources =
        yieldTransactionResourcesFromOperationContext(operationContext());
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();

    {
        const auto& csr = CollectionShardingRuntime::acquireExclusive(operationContext(), nss);
        csr->invalidateRangePreserversOlderThanShardVersion(
            operationContext(), shardVersionShardedCollection1.placementVersion());
    }

    if (mustFail) {
        // Attempt to restore. It must fail as collection metadata trackers are invalidated.
        ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                               operationContext(), std::move(yieldedTransactionResources)),
                           DBException,
                           ErrorCodes::QueryPlanKilled);
    } else {
        restoreTransactionResourcesToOperationContext(operationContext(),
                                                      std::move(yieldedTransactionResources));
    }
}

TEST_F(ShardRoleTest, StashTransactionResourcesForMultiDocumentTransactionComprehensive) {
    // Start a multi-document transaction
    operationContext()->setInMultiDocumentTransaction();

    // Acquire multiple collections
    PlacementConcern unshardedConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    PlacementConcern shardedConcern{{}, shardVersionShardedCollection1};
    PlacementConcern viewConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};

    auto acquisition1 = acquireCollection(operationContext(),
                                          {nssUnshardedCollection1,
                                           unshardedConcern,
                                           repl::ReadConcernArgs(),
                                           AcquisitionPrerequisites::kRead},
                                          MODE_IS);

    auto acquisition2 = acquireCollection(operationContext(),
                                          {nssShardedCollection1,
                                           shardedConcern,
                                           repl::ReadConcernArgs(),
                                           AcquisitionPrerequisites::kRead},
                                          MODE_IS);

    auto acquisition3 = acquireCollectionOrView(
        operationContext(),
        {nssView, viewConcern, repl::ReadConcernArgs(), AcquisitionPrerequisites::kRead},
        MODE_IS);

    ASSERT_TRUE(acquisition1.exists());
    ASSERT_TRUE(acquisition2.exists());
    ASSERT_TRUE(acquisition3.isView());
    ASSERT_TRUE(shard_role_details::TransactionResources::isPresent(operationContext()));

    {
        StashTransactionResourcesForMultiDocumentTransaction stasher(operationContext());

        // After stashing, we should have left attached empty transaction resources
        auto& txnResources = shard_role_details::TransactionResources::get(operationContext());
        ASSERT_TRUE(txnResources.isPresent(operationContext()) && txnResources.isEmpty());

        stasher.restoreOnCommit();

        // After restore, transaction resources should be present
        ASSERT_TRUE(shard_role_details::TransactionResources::isPresent(operationContext()));
    }

    // Verify both acquisitions are still valid
    ASSERT_TRUE(acquisition1.exists());
    ASSERT_TRUE(acquisition2.exists());
    ASSERT_TRUE(acquisition3.isView());
    ASSERT_EQ(nssUnshardedCollection1, acquisition1.nss());
    ASSERT_EQ(nssShardedCollection1, acquisition2.nss());
    ASSERT_EQ(nssView, acquisition3.nss());
}


TEST_F(ShardRoleTest, StashTransactionResourcesForMultiDocumentTransactionRestoreOnAbort) {
    const auto nss = nssUnshardedCollection1;

    // Start a multi-document transaction
    operationContext()->setInMultiDocumentTransaction();

    // Acquire a collection to have some transaction resources
    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    auto acquisition = acquireCollection(
        operationContext(),
        {nss, placementConcern, repl::ReadConcernArgs(), AcquisitionPrerequisites::kRead},
        MODE_IS);

    ASSERT_TRUE(acquisition.exists());
    ASSERT_TRUE(shard_role_details::TransactionResources::isPresent(operationContext()));

    {
        StashTransactionResourcesForMultiDocumentTransaction stasher(operationContext());
        // Don't call restoreOnCommit, let destructor handle abort case
    }

    // After destructor (abort case), transaction resources should be in FAILED state
    ASSERT_TRUE(shard_role_details::TransactionResources::isPresent(operationContext()));
    auto& txnResources = shard_role_details::TransactionResources::get(operationContext());
    ASSERT_EQ(txnResources.state, shard_role_details::TransactionResources::State::FAILED);
}

TEST_F(ShardRoleTest, StashTransactionResourcesForMultiDocumentTransactionNoResourcesToStash) {
    // Start a multi-document transaction but don't acquire any resources
    operationContext()->setInMultiDocumentTransaction();
    // Ensure no transaction resources are present
    auto& txnResources = shard_role_details::TransactionResources::get(operationContext());
    txnResources.detachFromOpCtx(operationContext());
    {
        StashTransactionResourcesForMultiDocumentTransaction stasher(operationContext());
        ASSERT_FALSE(shard_role_details::TransactionResources::isPresent(operationContext()));
        stasher.restoreOnCommit();

        // Still no transaction resources should be present
        ASSERT_FALSE(shard_role_details::TransactionResources::isPresent(operationContext()));
    }

    // After destructor, still no transaction resources
    ASSERT_FALSE(shard_role_details::TransactionResources::isPresent(operationContext()));
}

TEST_F(ShardRoleTest, StashTransactionResourcesForMultiDocumentTransactionDoubleRestore) {
    const auto nss = nssUnshardedCollection1;

    // Start a multi-document transaction
    operationContext()->setInMultiDocumentTransaction();

    // Acquire a collection
    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    auto acquisition = acquireCollection(
        operationContext(),
        {nss, placementConcern, repl::ReadConcernArgs(), AcquisitionPrerequisites::kRead},
        MODE_IS);

    ASSERT_TRUE(acquisition.exists());

    // Create the stasher
    StashTransactionResourcesForMultiDocumentTransaction stasher(operationContext());

    // First restore on commit
    stasher.restoreOnCommit();
    ASSERT_TRUE(shard_role_details::TransactionResources::isPresent(operationContext()));

    // Second restore on commit should be safe (no-op)
    stasher.restoreOnCommit();
    ASSERT_TRUE(shard_role_details::TransactionResources::isPresent(operationContext()));

    // Acquisition should still be valid
    ASSERT_TRUE(acquisition.exists());
    ASSERT_EQ(nss, acquisition.nss());
}

}  // namespace
}  // namespace mongo
