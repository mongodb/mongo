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

#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
#include <string>

#include <absl/container/node_hash_map.h>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version.h"
#include "mongo/s/index_version.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/sharding_state.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"

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
    scopedDss->setDbInfo(opCtx, {dbName, ShardId("ShardRoleTest"), dbVersion});
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

    const ShardId thisShardId{"ShardRoleTest"};

    const DatabaseName dbNameTestDb = DatabaseName::createDatabaseName_forTest(boost::none, "test");
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

    void changeOpCtx() {
        _opCtx.reset();
        _opCtx = getGlobalServiceContext()->makeOperationContext(&cc());
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

void ShardRoleTest::setUp() {
    ServiceContextMongoDTest::setUp();
    changeOpCtx();
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::RouterServer};

    const repl::ReplSettings replSettings = {};
    repl::ReplicationCoordinator::set(
        getGlobalServiceContext(),
        std::unique_ptr<repl::ReplicationCoordinator>(
            new repl::ReplicationCoordinatorMock(_opCtx->getServiceContext(), replSettings)));
    ASSERT_OK(repl::ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(repl::MemberState::RS_PRIMARY));

    repl::createOplog(_opCtx.get());

    const HostAndPort kConfigHostAndPort{"DummyConfig", 12345};
    ShardingState::get(getServiceContext())
        ->setRecoveryCompleted({OID::gen(),
                                ClusterRole::ShardServer,
                                ConnectionString(kConfigHostAndPort),
                                ShardId("ShardRoleTest")});

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

TEST_F(ShardRoleTest, AcquisitionWithInvalidNamespaceFails) {
    const auto checkAcquisitionByNss = [&](const NamespaceString& nss) {
        // With locks
        ASSERT_THROWS_CODE(
            acquireCollection(opCtx(),
                              {nss, {}, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
                              MODE_IX),
            DBException,
            ErrorCodes::InvalidNamespace);

        // Without locks
        ASSERT_THROWS_CODE(
            acquireCollectionsOrViewsMaybeLockFree(
                opCtx(), {{nss, {}, repl::ReadConcernArgs(), AcquisitionPrerequisites::kRead}}),
            DBException,
            ErrorCodes::InvalidNamespace);
    };

    const auto checkAcquisitionByNssOrUUID = [&](const NamespaceStringOrUUID& nssOrUuid) {
        // With locks
        ASSERT_THROWS_CODE(
            acquireCollection(
                opCtx(),
                {nssOrUuid, {}, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
                MODE_IX),
            DBException,
            ErrorCodes::InvalidNamespace);

        // Without locks
        ASSERT_THROWS_CODE(
            acquireCollectionsOrViewsMaybeLockFree(
                opCtx(),
                {{nssOrUuid, {}, repl::ReadConcernArgs(), AcquisitionPrerequisites::kRead}}),
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
        const auto acquisition = acquireCollection(opCtx(),
                                                   {nssUnshardedCollection1,
                                                    placementConcern,
                                                    repl::ReadConcernArgs(),
                                                    AcquisitionPrerequisites::kWrite},
                                                   MODE_IX);
        ASSERT_TRUE(
            shard_role_details::getLocker(opCtx())->isDbLockedForMode(dbNameTestDb, MODE_IX));
        ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isCollectionLockedForMode(
            nssUnshardedCollection1, MODE_IX));
        validateAcquisition(acquisition);
    }

    // Without locks.
    {
        const auto acquisitions =
            acquireCollectionsOrViewsMaybeLockFree(opCtx(),
                                                   {{nssUnshardedCollection1,
                                                     placementConcern,
                                                     repl::ReadConcernArgs(),
                                                     AcquisitionPrerequisites::kRead}});

        ASSERT_EQ(1, acquisitions.size());
        ASSERT_EQ(nssUnshardedCollection1, acquisitions.begin()->first);
        ASSERT_TRUE(acquisitions.at(nssUnshardedCollection1).isCollection());
        const auto& acquisition = acquisitions.at(nssUnshardedCollection1).getCollection();

        ASSERT_FALSE(
            shard_role_details::getLocker(opCtx())->isDbLockedForMode(dbNameTestDb, MODE_IS));
        ASSERT_FALSE(shard_role_details::getLocker(opCtx())->isLockHeldForMode(
            ResourceId{RESOURCE_COLLECTION, nssUnshardedCollection1}, MODE_IS));
        validateAcquisition(acquisition);
    }
}

TEST_F(ShardRoleTest, AcquireUnshardedCollWithIncorrectPlacementVersionThrows) {
    const auto incorrectDbVersion = DatabaseVersion(UUID::gen(), Timestamp(50, 0));

    PlacementConcern placementConcern{incorrectDbVersion, ShardVersion::UNSHARDED()};

    auto validateException = [&](const DBException& ex) {
        const auto exInfo = ex.extraInfo<StaleDbRoutingVersion>();
        ASSERT_EQ(dbNameTestDb, exInfo->getDb());
        ASSERT_EQ(incorrectDbVersion, exInfo->getVersionReceived());
        ASSERT_EQ(dbVersionTestDb, exInfo->getVersionWanted());
        ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
    };

    // With locks.
    ASSERT_THROWS_WITH_CHECK(acquireCollection(opCtx(),
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
        acquireCollectionsOrViewsMaybeLockFree(opCtx(),
                                               {{
                                                   nssUnshardedCollection1,
                                                   placementConcern,
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kRead,
                                               }}),
        ExceptionFor<ErrorCodes::StaleDbVersion>,
        validateException);
}

TEST_F(ShardRoleTest, AcquireUnshardedCollWhenShardDoesNotKnowThePlacementVersionThrows) {
    {
        // Clear the database metadata
        AutoGetDb autoDb(opCtx(), dbNameTestDb, MODE_X, {}, {});
        auto scopedDss =
            DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx(), dbNameTestDb);
        scopedDss->clearDbInfo(opCtx());
    }

    auto validateException = [&](const DBException& ex) {
        const auto exInfo = ex.extraInfo<StaleDbRoutingVersion>();
        ASSERT_EQ(dbNameTestDb, exInfo->getDb());
        ASSERT_EQ(dbVersionTestDb, exInfo->getVersionReceived());
        ASSERT_EQ(boost::none, exInfo->getVersionWanted());
        ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
    };

    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    ASSERT_THROWS_WITH_CHECK(acquireCollection(opCtx(),
                                               {nssUnshardedCollection1,
                                                placementConcern,
                                                repl::ReadConcernArgs(),
                                                AcquisitionPrerequisites::kWrite},
                                               MODE_IX),
                             ExceptionFor<ErrorCodes::StaleDbVersion>,
                             validateException);

    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViewsMaybeLockFree(opCtx(),
                                               {{nssUnshardedCollection1,
                                                 placementConcern,
                                                 repl::ReadConcernArgs(),
                                                 AcquisitionPrerequisites::kRead}}),
        ExceptionFor<ErrorCodes::StaleDbVersion>,
        validateException);
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

        auto validateException = [&](const DBException& ex) {
            const auto exInfo = ex.extraInfo<StaleDbRoutingVersion>();
            ASSERT_EQ(dbNameTestDb, exInfo->getDb());
            ASSERT_EQ(dbVersionTestDb, exInfo->getVersionReceived());
            ASSERT_EQ(boost::none, exInfo->getVersionWanted());
            ASSERT_TRUE(exInfo->getCriticalSectionSignal().is_initialized());
        };

        ASSERT_THROWS_WITH_CHECK(acquireCollection(opCtx(),
                                                   {nssUnshardedCollection1,
                                                    placementConcern,
                                                    repl::ReadConcernArgs(),
                                                    AcquisitionPrerequisites::kWrite},
                                                   MODE_IX),
                                 ExceptionFor<ErrorCodes::StaleDbVersion>,
                                 validateException);
        ASSERT_THROWS_WITH_CHECK(
            acquireCollectionsOrViewsMaybeLockFree(opCtx(),
                                                   {{nssUnshardedCollection1,
                                                     placementConcern,
                                                     repl::ReadConcernArgs(),
                                                     AcquisitionPrerequisites::kRead}}),
            ExceptionFor<ErrorCodes::StaleDbVersion>,
            validateException);
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

    auto validateAcquisition = [&](auto& acquisition) {
        ASSERT_EQ(nssUnshardedCollection1, acquisition.nss());
        ASSERT_EQ(nssUnshardedCollection1, acquisition.getCollectionPtr()->ns());
        ASSERT_FALSE(acquisition.getShardingDescription().isSharded());
    };

    // With locks.
    {
        const auto acquisition = acquireCollection(
            opCtx(),
            CollectionAcquisitionRequest::fromOpCtx(
                opCtx(), nssUnshardedCollection1, AcquisitionPrerequisites::kWrite),
            MODE_IX);

        ASSERT_TRUE(
            shard_role_details::getLocker(opCtx())->isDbLockedForMode(dbNameTestDb, MODE_IX));
        ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isCollectionLockedForMode(
            nssUnshardedCollection1, MODE_IX));
        validateAcquisition(acquisition);
    }

    // Without locks.
    {
        const auto acquisitions = acquireCollectionsOrViewsMaybeLockFree(
            opCtx(),
            {CollectionAcquisitionRequest::fromOpCtx(
                opCtx(), nssUnshardedCollection1, AcquisitionPrerequisites::kRead)});

        ASSERT_EQ(1, acquisitions.size());
        ASSERT_TRUE(acquisitions.at(nssUnshardedCollection1).isCollection());
        const auto& acquisition = acquisitions.at(nssUnshardedCollection1).getCollection();

        ASSERT_FALSE(
            shard_role_details::getLocker(opCtx())->isDbLockedForMode(dbNameTestDb, MODE_IS));
        ASSERT_FALSE(shard_role_details::getLocker(opCtx())->isLockHeldForMode(
            ResourceId{RESOURCE_COLLECTION, nssUnshardedCollection1}, MODE_IS));
        validateAcquisition(acquisition);
    }
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

DEATH_TEST_REGEX_F(ShardRoleTest,
                   AcquireLocalCatalogOnlyWithPotentialDataLossForbiddenToAccessFilter,
                   "Tripwire assertion.*7740800") {
    auto acquisition = acquireCollectionForLocalCatalogOnlyWithPotentialDataLoss(
        opCtx(), nssUnshardedCollection1, MODE_IX);

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
        const auto acquisition = acquireCollection(opCtx(),
                                                   {nssShardedCollection1,
                                                    placementConcern,
                                                    repl::ReadConcernArgs(),
                                                    AcquisitionPrerequisites::kWrite},
                                                   MODE_IX);
        ASSERT_TRUE(
            shard_role_details::getLocker(opCtx())->isDbLockedForMode(dbNameTestDb, MODE_IX));
        ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isCollectionLockedForMode(
            nssShardedCollection1, MODE_IX));
        validateAcquisition(acquisition);
    }

    // Without locks.
    {
        const auto acquisitions =
            acquireCollectionsOrViewsMaybeLockFree(opCtx(),
                                                   {{nssShardedCollection1,
                                                     placementConcern,
                                                     repl::ReadConcernArgs(),
                                                     AcquisitionPrerequisites::kRead}});

        ASSERT_EQ(1, acquisitions.size());
        ASSERT_TRUE(acquisitions.at(nssShardedCollection1).isCollection());
        const auto& acquisition = acquisitions.at(nssShardedCollection1).getCollection();

        ASSERT_FALSE(
            shard_role_details::getLocker(opCtx())->isDbLockedForMode(dbNameTestDb, MODE_IS));
        ASSERT_FALSE(shard_role_details::getLocker(opCtx())->isLockHeldForMode(
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
        ASSERT_EQ(ShardId("ShardRoleTest"), exInfo->getShardId());
        ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
    };

    ASSERT_THROWS_WITH_CHECK(acquireCollection(opCtx(),
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
        acquireCollectionsOrViewsMaybeLockFree(opCtx(),
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
        AutoGetCollection coll(opCtx(), nssShardedCollection1, MODE_IX);
        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx(),
                                                                             nssShardedCollection1)
            ->clearFilteringMetadata(opCtx());
    }

    PlacementConcern placementConcern{{}, shardVersionShardedCollection1};

    auto validateException = [&](const DBException& ex) {
        const auto exInfo = ex.extraInfo<StaleConfigInfo>();
        ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
        ASSERT_EQ(shardVersionShardedCollection1, exInfo->getVersionReceived());
        ASSERT_EQ(boost::none, exInfo->getVersionWanted());
        ASSERT_EQ(ShardId("ShardRoleTest"), exInfo->getShardId());
        ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
    };

    ASSERT_THROWS_WITH_CHECK(acquireCollection(opCtx(),
                                               {nssShardedCollection1,
                                                placementConcern,
                                                repl::ReadConcernArgs(),
                                                AcquisitionPrerequisites::kWrite},
                                               MODE_IX),
                             ExceptionFor<ErrorCodes::StaleConfig>,
                             validateException);
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViewsMaybeLockFree(opCtx(),
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
        AutoGetCollection coll(opCtx(), nssShardedCollection1, MODE_X);
        const auto& csr = CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
            opCtx(), nssShardedCollection1);
        csr->enterCriticalSectionCatchUpPhase(criticalSectionReason);
        csr->enterCriticalSectionCommitPhase(criticalSectionReason);
    }

    {
        PlacementConcern placementConcern{{}, shardVersionShardedCollection1};

        auto validateException = [&](const DBException& ex) {
            const auto exInfo = ex.extraInfo<StaleConfigInfo>();
            ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
            ASSERT_EQ(shardVersionShardedCollection1, exInfo->getVersionReceived());
            ASSERT_EQ(boost::none, exInfo->getVersionWanted());
            ASSERT_EQ(ShardId("ShardRoleTest"), exInfo->getShardId());
            ASSERT_TRUE(exInfo->getCriticalSectionSignal().is_initialized());
        };
        ASSERT_THROWS_WITH_CHECK(acquireCollection(opCtx(),
                                                   {nssShardedCollection1,
                                                    placementConcern,
                                                    repl::ReadConcernArgs(),
                                                    AcquisitionPrerequisites::kWrite},
                                                   MODE_IX),
                                 ExceptionFor<ErrorCodes::StaleConfig>,
                                 validateException);
        ASSERT_THROWS_WITH_CHECK(
            acquireCollectionsOrViewsMaybeLockFree(opCtx(),
                                                   {{nssShardedCollection1,
                                                     placementConcern,
                                                     repl::ReadConcernArgs(),
                                                     AcquisitionPrerequisites::kRead}}),
            ExceptionFor<ErrorCodes::StaleConfig>,
            validateException);
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
}

// ---------------------------------------------------------------------------
// Acquire inexistent collections

TEST_F(ShardRoleTest, AcquireCollectionNonExistentNamespace) {
    const NamespaceString inexistentNss =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "inexistent");

    // With locks.
    {
        auto acquisition =
            acquireCollection(opCtx(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  opCtx(), inexistentNss, AcquisitionPrerequisites::kWrite),
                              MODE_IX);
        ASSERT(!acquisition.getCollectionPtr());
        ASSERT(!acquisition.getShardingDescription().isSharded());
    }

    // Without locks.
    {
        auto acquisitions = acquireCollectionsOrViewsMaybeLockFree(
            opCtx(),
            {CollectionAcquisitionRequest::fromOpCtx(
                opCtx(), inexistentNss, AcquisitionPrerequisites::kRead)});

        ASSERT_EQ(1, acquisitions.size());
        ASSERT_TRUE(acquisitions.at(inexistentNss).isCollection());
        const auto& acquisition = acquisitions.at(inexistentNss).getCollection();

        ASSERT(!acquisition.getCollectionPtr());
        ASSERT(!acquisition.getShardingDescription().isSharded());
    }
}

TEST_F(ShardRoleTest, AcquireInexistentCollectionWithWrongPlacementThrowsBecauseWrongPlacement) {
    const auto incorrectDbVersion = dbVersionTestDb.makeUpdated();
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
    ASSERT_THROWS_WITH_CHECK(acquireCollection(opCtx(),
                                               {inexistentNss,
                                                placementConcern,
                                                repl::ReadConcernArgs(),
                                                AcquisitionPrerequisites::kWrite},
                                               MODE_IX),
                             ExceptionFor<ErrorCodes::StaleDbVersion>,
                             validateException);
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViewsMaybeLockFree(opCtx(),
                                               {{inexistentNss,
                                                 placementConcern,
                                                 repl::ReadConcernArgs(),
                                                 AcquisitionPrerequisites::kRead}}),
        ExceptionFor<ErrorCodes::StaleDbVersion>,
        validateException);
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
        opCtx()->setInMultiDocumentTransaction();
        shard_role_details::getRecoveryUnit(opCtx())->preallocateSnapshot();
        CollectionCatalog::stash(opCtx(), CollectionCatalog::get(opCtx()));
    }

    // Drop a collection
    {
        auto newClient =
            opCtx()->getServiceContext()->getService()->makeClient("AlternativeClient");
        AlternativeClientRegion acr(newClient);
        auto newOpCtx = cc().makeOperationContext();
        DBDirectClient directClient(newOpCtx.get());
        directClient.dropCollection(nssUnshardedCollection1);
    }

    const auto acquireForRead = acquireCollectionOrView(
        opCtx(),
        CollectionOrViewAcquisitionRequest::fromOpCtx(
            opCtx(), nssUnshardedCollection1, AcquisitionPrerequisites::kRead),
        MODE_IX);
    ASSERT_TRUE(acquireForRead.isCollection());

    ASSERT_THROWS_CODE(acquireCollectionOrView(
                           opCtx(),
                           CollectionOrViewAcquisitionRequest::fromOpCtx(
                               opCtx(), nssUnshardedCollection1, AcquisitionPrerequisites::kWrite),
                           MODE_IX),
                       DBException,
                       ErrorCodes::WriteConflict);
}

TEST_F(ShardRoleTest, ConflictIsThrownWhenShardVersionUnshardedButStashedCatalogDiffersFromLatest) {
    opCtx()->setInMultiDocumentTransaction();
    shard_role_details::getRecoveryUnit(opCtx())->preallocateSnapshot();
    CollectionCatalog::stash(opCtx(), CollectionCatalog::get(opCtx()));

    // Drop a collection
    {
        auto newClient =
            opCtx()->getServiceContext()->getService()->makeClient("AlternativeClient");
        AlternativeClientRegion acr(newClient);
        auto newOpCtx = cc().makeOperationContext();
        DBDirectClient directClient(newOpCtx.get());
        ASSERT_TRUE(directClient.dropCollection(nssUnshardedCollection1));
    }

    // Try to acquire the now-dropped collection, with declared placement concern
    // ShardVersion::UNSHARDED. Expect a conflict to be detected.
    {
        ScopedSetShardRole setShardRole(
            opCtx(), nssUnshardedCollection1, ShardVersion::UNSHARDED(), boost::none);
        ASSERT_THROWS_CODE(
            acquireCollectionOrView(
                opCtx(),
                CollectionOrViewAcquisitionRequest::fromOpCtx(
                    opCtx(), nssUnshardedCollection1, AcquisitionPrerequisites::kRead),
                MODE_IX),
            DBException,
            ErrorCodes::SnapshotUnavailable);
    }
}

// ---------------------------------------------------------------------------
// MaybeLockFree
TEST_F(ShardRoleTest, AcquireCollectionMaybeLockFreeTakesLocksWhenInMultiDocTransaction) {
    opCtx()->setInMultiDocumentTransaction();
    const auto acquisition =
        acquireCollectionMaybeLockFree(opCtx(),
                                       {nssUnshardedCollection1,
                                        {dbVersionTestDb, ShardVersion::UNSHARDED()},
                                        repl::ReadConcernArgs(),
                                        AcquisitionPrerequisites::kRead});
    ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isCollectionLockedForMode(
        nssUnshardedCollection1, MODE_IS));
}

TEST_F(ShardRoleTest, AcquireCollectionMaybeLockFreeDoesNotTakeLocksWhenNotInMultiDocTransaction) {
    const auto acquisition =
        acquireCollectionMaybeLockFree(opCtx(),
                                       {nssUnshardedCollection1,
                                        {dbVersionTestDb, ShardVersion::UNSHARDED()},
                                        repl::ReadConcernArgs(),
                                        AcquisitionPrerequisites::kRead});
    ASSERT_FALSE(shard_role_details::getLocker(opCtx())->isLockHeldForMode(
        ResourceId{RESOURCE_COLLECTION, nssUnshardedCollection1}, MODE_IS));
}

DEATH_TEST_REGEX_F(ShardRoleTest,
                   AcquireCollectionMaybeLockFreeAllowedOnlyForRead,
                   "Tripwire assertion") {
    ASSERT_THROWS_CODE(acquireCollectionMaybeLockFree(opCtx(),
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

    const auto& acquisitionUnshardedColl = acquisitions.at(nssUnshardedCollection1);
    ASSERT_FALSE(acquisitionUnshardedColl.getShardingDescription().isSharded());

    const auto& acquisitionShardedColl = acquisitions.at(nssShardedCollection1);
    ASSERT_TRUE(acquisitionShardedColl.getShardingDescription().isSharded());
    ASSERT_TRUE(acquisitionShardedColl.getShardingFilter().has_value());

    // Assert the DB lock is held, but not recursively (i.e. only once).
    ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isDbLockedForMode(dbNameTestDb, MODE_IX));
    ASSERT_FALSE(shard_role_details::getLocker(opCtx())->isGlobalLockedRecursively());

    // Assert both collections are locked.
    ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isCollectionLockedForMode(
        nssUnshardedCollection1, MODE_IX));
    ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isCollectionLockedForMode(
        nssShardedCollection1, MODE_IX));
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
            ASSERT_EQ(ShardId("ShardRoleTest"), exInfo->getShardId());
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
    ASSERT_THROWS_CODE(
        acquireCollection(
            opCtx(),
            {NamespaceStringOrUUID(
                 DatabaseName::createDatabaseName_forTest(boost::none, "anotherDbName"), uuid),
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

TEST_F(ShardRoleTest, AcquireCollectionByUUIDWithShardVersionAttachedThrows) {
    const auto uuid = getCollectionUUID(opCtx(), nssShardedCollection1);
    const auto dbVersion = boost::none;
    const auto shardVersion = shardVersionShardedCollection1;
    ScopedSetShardRole setShardRole(opCtx(), nssShardedCollection1, shardVersion, dbVersion);
    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    ASSERT_THROWS_CODE(acquireCollection(opCtx(),
                                         {NamespaceStringOrUUID(dbNameTestDb, uuid),
                                          placementConcern,
                                          repl::ReadConcernArgs(),
                                          AcquisitionPrerequisites::kWrite},
                                         MODE_IX),
                       DBException,
                       ErrorCodes::IncompatibleShardingMetadata);
    ASSERT_THROWS_CODE(
        acquireCollectionsOrViewsMaybeLockFree(opCtx(),
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

    auto validateException = [&](const DBException& ex) {
        const auto exInfo = ex.extraInfo<CollectionUUIDMismatchInfo>();
        ASSERT_EQ(nss.dbName(), exInfo->dbName());
        ASSERT_EQ(wrongUuid, exInfo->collectionUUID());
        ASSERT_EQ(nss.coll(), exInfo->expectedCollection());
        ASSERT_EQ(boost::none, exInfo->actualCollection());
    };

    ASSERT_THROWS_WITH_CHECK(
        acquireCollection(
            opCtx(),
            {nss, wrongUuid, {}, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
            MODE_IX),
        ExceptionFor<ErrorCodes::CollectionUUIDMismatch>,
        validateException);
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViewsMaybeLockFree(
            opCtx(),
            {{nss, wrongUuid, {}, repl::ReadConcernArgs(), AcquisitionPrerequisites::kRead}}),
        ExceptionFor<ErrorCodes::CollectionUUIDMismatch>,
        validateException);
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
        ASSERT_TRUE(acquisition.isView());
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
        ASSERT_TRUE(acquisition.isCollection());
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

    ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isDbLockedForMode(nss.dbName(), MODE_IX));
    ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isCollectionLockedForMode(nss, MODE_IX));

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());
    shard_role_details::getRecoveryUnit(opCtx())->abandonSnapshot();

    ASSERT_FALSE(shard_role_details::getLocker(opCtx())->isDbLockedForMode(nss.dbName(), MODE_IX));
    ASSERT_FALSE(shard_role_details::getLocker(opCtx())->isCollectionLockedForMode(nss, MODE_IX));

    // Restore the resources
    restoreTransactionResourcesToOperationContext(opCtx(), std::move(yieldedTransactionResources));
    ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isDbLockedForMode(nss.dbName(), MODE_IX));
    ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isCollectionLockedForMode(nss, MODE_IX));
}

TEST_F(ShardRoleTest, YieldAndRestoreAcquisitionWithoutLocks) {
    const auto nss = nssUnshardedCollection1;

    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    const auto acquisitions =
        acquireCollectionsOrViewsMaybeLockFree(opCtx(),
                                               {{
                                                   nss,
                                                   placementConcern,
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kRead,
                                               }});

    ASSERT_EQ(1, acquisitions.size());
    ASSERT_TRUE(acquisitions.at(nss).isCollection());

    ASSERT_TRUE(
        shard_role_details::getLocker(opCtx())->isLockHeldForMode(resourceIdGlobal, MODE_IS));
    ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isDbLockedForMode(nss.dbName(), MODE_NONE));

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());
    shard_role_details::getRecoveryUnit(opCtx())->abandonSnapshot();

    ASSERT_FALSE(
        shard_role_details::getLocker(opCtx())->isLockHeldForMode(resourceIdGlobal, MODE_IS));
    ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isDbLockedForMode(nss.dbName(), MODE_NONE));

    // Restore the resources
    restoreTransactionResourcesToOperationContext(opCtx(), std::move(yieldedTransactionResources));
    ASSERT_TRUE(
        shard_role_details::getLocker(opCtx())->isLockHeldForMode(resourceIdGlobal, MODE_IS));
    ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isDbLockedForMode(nss.dbName(), MODE_NONE));
}

TEST_F(ShardRoleTest,
       RestoreForWriteInvalidatesAcquisitionIfPlacementConcernShardVersionNoLongerMet) {
    const auto nss = nssShardedCollection1;

    PlacementConcern placementConcern{{}, shardVersionShardedCollection1};
    const auto acquisition = acquireCollection(
        opCtx(),
        {nss, placementConcern, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
        MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());
    shard_role_details::getRecoveryUnit(opCtx())->abandonSnapshot();

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
                                 ASSERT_EQ(ShardId("ShardRoleTest"), exInfo->getShardId());
                                 ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
                             });

    ASSERT_FALSE(shard_role_details::getLocker(opCtx())->isDbLockedForMode(nss.dbName(), MODE_IX));
    ASSERT_FALSE(shard_role_details::getLocker(opCtx())->isCollectionLockedForMode(nss, MODE_IX));
}

TEST_F(ShardRoleTest, RestoreForWriteInvalidatesAcquisitionIfPlacementConcernDbVersionNoLongerMet) {
    const auto nss = nssUnshardedCollection1;

    PlacementConcern placementConcern{dbVersionTestDb, {}};
    const auto acquisition = acquireCollection(
        opCtx(),
        {nss, placementConcern, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
        MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());
    shard_role_details::getRecoveryUnit(opCtx())->abandonSnapshot();

    // Placement changes
    const auto newDbVersion = dbVersionTestDb.makeUpdated();
    installDatabaseMetadata(opCtx(), nssUnshardedCollection1.dbName(), newDbVersion);

    // Try to restore the resources should fail because placement concern is no longer met.
    ASSERT_THROWS_WITH_CHECK(restoreTransactionResourcesToOperationContext(
                                 opCtx(), std::move(yieldedTransactionResources)),
                             ExceptionFor<ErrorCodes::StaleDbVersion>,
                             [&](const DBException& ex) {
                                 const auto exInfo = ex.extraInfo<StaleDbRoutingVersion>();
                                 ASSERT_EQ(nss.dbName(), exInfo->getDb());
                                 ASSERT_EQ(dbVersionTestDb, exInfo->getVersionReceived());
                                 ASSERT_EQ(newDbVersion, exInfo->getVersionWanted());
                                 ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
                             });

    ASSERT_FALSE(shard_role_details::getLocker(opCtx())->isDbLockedForMode(nss.dbName(), MODE_IX));
    ASSERT_FALSE(shard_role_details::getLocker(opCtx())->isCollectionLockedForMode(nss, MODE_IX));
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
    shard_role_details::getRecoveryUnit(opCtx())->abandonSnapshot();

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
    ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isCollectionLockedForMode(nss, MODE_IX));
}

void ShardRoleTest::testRestoreFailsIfCollectionBecomesCreated(
    AcquisitionPrerequisites::OperationType operationType) {
    NamespaceString nss(NamespaceString::createNamespaceString_forTest(
        dbNameTestDb, "NonExistentCollectionWhichWillBeCreated"));

    const auto acquisition = acquireCollection(
        opCtx(), CollectionAcquisitionRequest::fromOpCtx(opCtx(), nss, operationType), MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());
    shard_role_details::getRecoveryUnit(opCtx())->abandonSnapshot();

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
    shard_role_details::getRecoveryUnit(opCtx())->abandonSnapshot();

    // Drop the collection
    {
        DBDirectClient client(opCtx());
        client.dropCollection(nss);
    }

    // Try to restore the resources should fail because the collection no longer exists.
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           opCtx(), std::move(yieldedTransactionResources)),
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
    const auto acquisition = acquireCollection(
        opCtx(), {nss, placementConcern, repl::ReadConcernArgs(), operationType}, MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());
    shard_role_details::getRecoveryUnit(opCtx())->abandonSnapshot();

    // Rename the collection.
    {
        DBDirectClient client(opCtx());
        BSONObj info;
        ASSERT_TRUE(client.runCommand(
            dbNameTestDb,
            BSON("renameCollection"
                 << nss.ns_forTest() << "to"
                 << NamespaceString::createNamespaceString_forTest(dbNameTestDb, "foo2")
                        .ns_forTest()),
            info));
    }

    // Try to restore the resources should fail because the collection has been renamed.
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           opCtx(), std::move(yieldedTransactionResources)),
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
    const auto acquisition = acquireCollection(
        opCtx(), {nss, placementConcern, repl::ReadConcernArgs(), operationType}, MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());
    shard_role_details::getRecoveryUnit(opCtx())->abandonSnapshot();

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
        shard_role_details::getRecoveryUnit(opCtx())->abandonSnapshot();

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
    shard_role_details::getRecoveryUnit(opCtx())->abandonSnapshot();

    shard_role_details::getRecoveryUnit(opCtx())->abandonSnapshot();

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
    ASSERT_OK(repl::ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(repl::MemberState::RS_SECONDARY));

    ASSERT_EQUALS(RecoveryUnit::ReadSource::kNoTimestamp,
                  shard_role_details::getRecoveryUnit(opCtx())->getTimestampReadSource());

    opCtx()->setEnforceConstraints(false);

    const auto coll = acquireCollection(
        opCtx(),
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(opCtx()),
                                     AcquisitionPrerequisites::kWrite),
        MODE_IX);

    ASSERT_TRUE(coll.exists());

    ASSERT_EQUALS(RecoveryUnit::ReadSource::kNoTimestamp,
                  shard_role_details::getRecoveryUnit(opCtx())->getTimestampReadSource());
}

TEST_F(ShardRoleTest, RestoreChangesReadSourceAfterStepUp) {
    const auto nss = nssShardedCollection1;

    // Set up secondary read state.
    opCtx()->getClient()->setInDirectClient(true);
    ASSERT_OK(repl::ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(repl::MemberState::RS_SECONDARY));

    // Initially we start with kNoTimestamp as our ReadSource.
    ASSERT_EQUALS(RecoveryUnit::ReadSource::kNoTimestamp,
                  shard_role_details::getRecoveryUnit(opCtx())->getTimestampReadSource());

    PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    const auto acquisitions =
        acquireCollectionsOrViewsMaybeLockFree(opCtx(),
                                               {{
                                                   nssUnshardedCollection1,
                                                   placementConcern,
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kRead,
                                               }});

    // Our read source should have been updated to kLastApplied.
    ASSERT_EQUALS(RecoveryUnit::ReadSource::kLastApplied,
                  shard_role_details::getRecoveryUnit(opCtx())->getTimestampReadSource());

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());
    shard_role_details::getRecoveryUnit(opCtx())->abandonSnapshot();

    // Step up.
    ASSERT_OK(repl::ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(repl::MemberState::RS_PRIMARY));

    // Restore the resources
    restoreTransactionResourcesToOperationContext(opCtx(), std::move(yieldedTransactionResources));

    // Our read source should have been updated to kNoTimestamp.
    ASSERT_EQUALS(RecoveryUnit::ReadSource::kNoTimestamp,
                  shard_role_details::getRecoveryUnit(opCtx())->getTimestampReadSource());
}

TEST_F(ShardRoleTest, RestoreCollectionCreatedUnderScopedLocalCatalogWriteFence) {
    const auto nss = NamespaceString::createNamespaceString_forTest(dbNameTestDb, "inexistent");
    auto acquisition = acquireCollection(
        opCtx(),
        {nss, PlacementConcern{{}, {}}, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
        MODE_IX);
    ASSERT_FALSE(acquisition.exists());

    // Create the collection
    {
        WriteUnitOfWork wuow(opCtx());
        ScopedLocalCatalogWriteFence scopedLocalCatalogWriteFence(opCtx(), &acquisition);
        createTestCollection(opCtx(), nss);
        wuow.commit();
    }
    ASSERT_TRUE(acquisition.exists());

    // Yield
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());

    // Restore works
    restoreTransactionResourcesToOperationContext(opCtx(), std::move(yieldedTransactionResources));
}

TEST_F(ShardRoleTest,
       RestoreCollectionCreatedUnderScopedLocalCatalogWriteFenceFailsIfNoLongerExists) {
    const auto nss = NamespaceString::createNamespaceString_forTest(dbNameTestDb, "inexistent");
    auto acquisition = acquireCollection(
        opCtx(),
        {nss, PlacementConcern{{}, {}}, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
        MODE_IX);
    ASSERT_FALSE(acquisition.exists());

    // Create the collection
    {
        WriteUnitOfWork wuow(opCtx());
        ScopedLocalCatalogWriteFence scopedLocalCatalogWriteFence(opCtx(), &acquisition);
        createTestCollection(opCtx(), nss);
        wuow.commit();
    }
    ASSERT_TRUE(acquisition.exists());

    // Yield
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());

    // Drop the collection
    DBDirectClient client(opCtx());
    client.dropCollection(nss);

    // Restore should fail
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           opCtx(), std::move(yieldedTransactionResources)),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
}

// ---------------------------------------------------------------------------
// External yields

TEST_F(ShardRoleTest, YieldAndRestoreCursor) {
    const auto& nss = nssUnshardedCollection1;
    auto cursorId = [&] {
        PlacementConcern placementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};

        const auto acquisition = acquireCollection(opCtx(),
                                                   {nssUnshardedCollection1,
                                                    placementConcern,
                                                    repl::ReadConcernArgs(),
                                                    AcquisitionPrerequisites::kWrite},
                                                   MODE_IX);

        auto params = std::make_unique<DeleteStageParams>();
        auto exec = InternalPlanner::deleteWithCollectionScan(
            opCtx(), acquisition, std::move(params), PlanYieldPolicy::YieldPolicy::YIELD_AUTO);

        auto pinnedCursor = CursorManager::get(opCtx())->registerCursor(
            opCtx(),
            {std::move(exec),
             nss,
             AuthorizationSession::get(opCtx()->getClient())->getAuthenticatedUserName(),
             APIParameters::get(opCtx()),
             opCtx()->getWriteConcern(),
             repl::ReadConcernArgs::get(opCtx()),
             ReadPreferenceSetting::get(opCtx()),
             BSON("x" << 1),
             {Privilege(ResourcePattern::forExactNamespace(nss), ActionType::find)}});
        PlanExecutor* cursorExec = pinnedCursor->getExecutor();

        ASSERT_TRUE(
            shard_role_details::getLocker(opCtx())->isDbLockedForMode(nss.dbName(), MODE_IX));
        ASSERT_TRUE(
            shard_role_details::getLocker(opCtx())->isCollectionLockedForMode(nss, MODE_IX));

        // State will be restored on getMore.
        cursorExec->saveState();
        cursorExec->detachFromOperationContext();

        stashTransactionResourcesFromOperationContext(opCtx(), pinnedCursor.getCursor());

        ASSERT_FALSE(
            shard_role_details::getLocker(opCtx())->isDbLockedForMode(nss.dbName(), MODE_IX));
        ASSERT_FALSE(
            shard_role_details::getLocker(opCtx())->isCollectionLockedForMode(nss, MODE_IX));

        return pinnedCursor->cursorid();
    }();

    // Become a different operation so we can imitate the behaviour of getMore.
    changeOpCtx();

    auto cursorPinStatus = CursorManager::get(opCtx())->pinCursor(opCtx(), cursorId);
    ASSERT_OK(cursorPinStatus);
    auto& cursorPin = cursorPinStatus.getValue();

    ON_BLOCK_EXIT([&] {
        ASSERT_FALSE(
            shard_role_details::getLocker(opCtx())->isDbLockedForMode(nss.dbName(), MODE_IX));
        ASSERT_FALSE(
            shard_role_details::getLocker(opCtx())->isCollectionLockedForMode(nss, MODE_IX));
    });

    HandleTransactionResourcesFromStasher handler(opCtx(), cursorPin.getCursor());

    ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isDbLockedForMode(nss.dbName(), MODE_IX));
    ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isCollectionLockedForMode(nss, MODE_IX));
}

// ---------------------------------------------------------------------------
// Storage snapshot

TEST_F(ShardRoleTest, SnapshotAttemptFailsIfReplTermChanges) {
    const auto nss = nssShardedCollection1;

    PlacementConcern placementConcern = PlacementConcern{{}, shardVersionShardedCollection1};

    std::vector<NamespaceStringOrUUID> requests = {{nss}};
    shard_role_details::SnapshotAttempt snapshotAttempt(opCtx(), requests);
    snapshotAttempt.snapshotInitialState();
    snapshotAttempt.changeReadSourceForSecondaryReads();
    snapshotAttempt.openStorageSnapshot();

    auto currentTerm = repl::ReplicationCoordinator::get(opCtx())->getTerm();
    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx())->updateTerm(opCtx(), currentTerm + 1));

    ASSERT_FALSE(snapshotAttempt.getConsistentCatalog());
}

TEST_F(ShardRoleTest, SnapshotAttemptFailsIfCatalogChanges) {
    const auto nss = nssShardedCollection1;

    PlacementConcern placementConcern = PlacementConcern{{}, shardVersionShardedCollection1};

    std::vector<NamespaceStringOrUUID> requests = {{nss}};
    shard_role_details::SnapshotAttempt snapshotAttempt(opCtx(), requests);
    snapshotAttempt.snapshotInitialState();
    snapshotAttempt.changeReadSourceForSecondaryReads();
    snapshotAttempt.openStorageSnapshot();

    auto nss2 = NamespaceString::createNamespaceString_forTest(dbNameTestDb, "newCollection");
    createTestCollection(opCtx(), nss2);

    ASSERT_FALSE(snapshotAttempt.getConsistentCatalog());
}

TEST_F(ShardRoleTest, ReadSourceChangesOnSecondary) {
    const auto nss = nssShardedCollection1;

    // Set up secondary read state.
    opCtx()->getClient()->setInDirectClient(true);
    ASSERT_OK(repl::ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(repl::MemberState::RS_SECONDARY));

    // Initially we start with kNoTimestamp as our ReadSource.
    ASSERT_EQUALS(RecoveryUnit::ReadSource::kNoTimestamp,
                  shard_role_details::getRecoveryUnit(opCtx())->getTimestampReadSource());

    PlacementConcern placementConcern = PlacementConcern{{}, shardVersionShardedCollection1};
    std::vector<NamespaceStringOrUUID> requests = {{nss}};
    shard_role_details::SnapshotAttempt snapshotAttempt(opCtx(), requests);
    snapshotAttempt.snapshotInitialState();
    snapshotAttempt.changeReadSourceForSecondaryReads();

    // Our read source should have been updated to kLastApplied.
    ASSERT_EQUALS(RecoveryUnit::ReadSource::kLastApplied,
                  shard_role_details::getRecoveryUnit(opCtx())->getTimestampReadSource());

    snapshotAttempt.openStorageSnapshot();
    ASSERT_TRUE(snapshotAttempt.getConsistentCatalog());
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

TEST_F(ShardRoleTest, ScopedLocalCatalogWriteFenceWUOWRollbackAfterAcquisitionOutOfScope) {
    // Tests that nothing breaks if ScopedLocalCatalogWriteFence's onRollback handler is executed
    // when the collection acquisition has already gone out of scope.
    WriteUnitOfWork wuow1(opCtx());
    {
        auto acquisition = acquireCollection(opCtx(),
                                             {nssShardedCollection1,
                                              PlacementConcern{{}, shardVersionShardedCollection1},
                                              repl::ReadConcernArgs(),
                                              AcquisitionPrerequisites::kRead},
                                             MODE_IX);
        ScopedLocalCatalogWriteFence(opCtx(), &acquisition);
    }
}

TEST_F(ShardRoleTest, ScopedLocalCatalogWriteFenceWUOWRollbackAfterANotherClientCreatedCollection) {
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "inexistent");

    // Acquire a collection that does not exist.
    auto acquisition = acquireCollection(
        opCtx(),
        {nss, PlacementConcern{{}, {}}, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
        MODE_IX);
    ASSERT_FALSE(acquisition.exists());

    // Another client creates the collection
    {
        auto newClient =
            opCtx()->getServiceContext()->getService()->makeClient("MigrationCoordinator");
        auto newOpCtx = newClient->makeOperationContext();
        createTestCollection(newOpCtx.get(), nss);
    }

    // Acquisition still reflects that the collection does not exist.
    ASSERT_FALSE(acquisition.exists());

    // Original client attempts to create the collection, which will result in a WriteConflict and
    // rollback.
    {
        WriteUnitOfWork wuow(opCtx());
        ScopedLocalCatalogWriteFence localCatalogWriteFence(opCtx(), &acquisition);
        auto db = DatabaseHolder::get(opCtx())->openDb(opCtx(), nss.dbName());
        db->createCollection(opCtx(), nss, CollectionOptions());
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
        opCtx(),
        {nss, PlacementConcern{{}, {}}, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
        MODE_IX);

    auto yielded = yieldTransactionResourcesFromOperationContext(opCtx());

    const auto otherNss = NamespaceString::createNamespaceString_forTest(dbNameTestDb, "otherNss");
    acquireCollection(opCtx(),
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
        opCtx(),
        {nss, placementConcern, repl::ReadConcernArgs(), AcquisitionPrerequisites::kWrite},
        MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());
    shard_role_details::getRecoveryUnit(opCtx())->abandonSnapshot();

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
                                 ASSERT_EQ(ShardId("ShardRoleTest"), exInfo->getShardId());
                                 ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
                             });

    const NamespaceString otherNss =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "inexistent");

    // Trying to acquire now should invariant and crash the server since we're in the FAILED state.
    acquireCollection(opCtx(),
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
                                         PlacementConcern{},
                                         repl::ReadConcernArgs(),
                                         AcquisitionPrerequisites::kRead);

        const auto acquisitionRequest2 =
            CollectionAcquisitionRequest(nssShardedCollection1,
                                         PlacementConcern{},
                                         repl::ReadConcernArgs(),
                                         AcquisitionPrerequisites::kRead);

        // Acquire two collections in the same acquisition.
        boost::optional<CollectionAcquisition> acquisition1;
        boost::optional<CollectionAcquisition> acquisition2;
        {
            CollectionAcquisitions acquisitions = withLocks
                ? acquireCollections(opCtx(), {acquisitionRequest1, acquisitionRequest2}, MODE_IS)
                : acquireCollectionsMaybeLockFree(opCtx(),
                                                  {acquisitionRequest1, acquisitionRequest2});
            acquisition1 = acquisitions.at(acquisitionRequest1.nssOrUUID.nss());
            acquisition2 = acquisitions.at(acquisitionRequest2.nssOrUUID.nss());
        }

        // Check not recursively locked.
        ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isReadLocked());  // Global lock
        ASSERT_FALSE(shard_role_details::getLocker(opCtx())->isGlobalLockedRecursively());
        if (!withLocks) {
            ASSERT_TRUE(opCtx()->isLockFreeReadsOp());
        }

        // Release one acquisition. Global lock still held.
        acquisition1.reset();
        ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isReadLocked());  // Global lock
        ASSERT_FALSE(shard_role_details::getLocker(opCtx())->isGlobalLockedRecursively());
        if (!withLocks) {
            ASSERT_TRUE(opCtx()->isLockFreeReadsOp());
        }

        // Release the other acquisition. Global lock no longer held.
        acquisition2.reset();
        ASSERT_FALSE(shard_role_details::getLocker(opCtx())->isReadLocked());  // Global lock
        ASSERT_FALSE(shard_role_details::getLocker(opCtx())->isGlobalLockedRecursively());
        if (!withLocks) {
            ASSERT_FALSE(opCtx()->isLockFreeReadsOp());
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
                                         PlacementConcern{},
                                         repl::ReadConcernArgs(),
                                         AcquisitionPrerequisites::kRead);

        const auto acquisitionRequest2 =
            CollectionAcquisitionRequest(nssShardedCollection1,
                                         PlacementConcern{},
                                         repl::ReadConcernArgs(),
                                         AcquisitionPrerequisites::kRead);

        // Acquire two collections in different acquisitions.
        boost::optional<CollectionAcquisition> acquisition1 = withLocks
            ? acquireCollection(opCtx(), acquisitionRequest1, MODE_IS)
            : acquireCollectionMaybeLockFree(opCtx(), acquisitionRequest1);

        boost::optional<CollectionAcquisition> acquisition2 = withLocks
            ? acquireCollection(opCtx(), acquisitionRequest2, MODE_IS)
            : acquireCollectionMaybeLockFree(opCtx(), acquisitionRequest2);

        // Check locked recursively.
        ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isReadLocked());  // Global lock
        ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isGlobalLockedRecursively());
        if (!withLocks) {
            ASSERT_TRUE(opCtx()->isLockFreeReadsOp());
        }

        // Release one acquisition. Check no longer locked recursively.
        acquisition1.reset();
        ASSERT_TRUE(shard_role_details::getLocker(opCtx())->isReadLocked());  // Global lock
        ASSERT_FALSE(shard_role_details::getLocker(opCtx())->isGlobalLockedRecursively());
        if (!withLocks) {
            ASSERT_TRUE(opCtx()->isLockFreeReadsOp());
        }

        // Release the other acquisition. Check no locks are held.
        acquisition2.reset();
        ASSERT_FALSE(shard_role_details::getLocker(opCtx())->isReadLocked());  // Global lock
        ASSERT_FALSE(shard_role_details::getLocker(opCtx())->isGlobalLockedRecursively());
        if (!withLocks) {
            ASSERT_FALSE(opCtx()->isLockFreeReadsOp());
        }
    };

    testFn(true);   // with locks
    testFn(false);  // lock-free
}

}  // namespace
}  // namespace mongo
