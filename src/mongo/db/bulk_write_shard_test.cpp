/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/commands/bulk_write.h"
#include "mongo/db/commands/bulk_write_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

// BulkWriteCommand unit tests for a mongod that is a shard server. In order to
// do so, in setup() we install collection metadata (shard version & database
// version) on the node. Consequently any collection metadata attached to the
// bulk request will be compared to the installed metadata and a StaleConfig
// error will be thrown in case of a mismatch.
//
// The installed collection metadata looks as follows. For the exact values used
// for the database and shard versions, refer to the corresponding variables.
// +---------+-------------------------+-------------+---------------+---------------+
// | Db Name |          Coll Name      |   Sharded?  |   Db Version  | Shard Version |
// +---------+-------------------------+-------------+---------------+---------------+
// | testDB1 |   unsharded.radiohead   |     NO      |      dbV1     |   UNSHARDED() |
// | testDB1 | sharded.porcupine.tree  |     YES     |      dbV1     |       sV1     |
// | testDB2 |       sharded.oasis     |     YES     |      dbV2     |       sV2     |
// +---------+-------------------------+-------------+---------------+---------------+
class BulkWriteShardTest : public ServiceContextMongoDTest {
protected:
    OperationContext* opCtx() {
        return _opCtx.get();
    }

    void setUp() override;
    void tearDown() override;

    const ShardId thisShardId{"this"};

    const DatabaseName dbNameTestDb1{"testDB1"};
    const DatabaseVersion dbVersionTestDb1{UUID::gen(), Timestamp(1, 0)};
    const DatabaseName dbNameTestDb2{"testDB2"};
    const DatabaseVersion dbVersionTestDb2{UUID::gen(), Timestamp(2, 0)};

    const NamespaceString nssUnshardedCollection1 =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb1, "unsharded.radiohead");

    const NamespaceString nssShardedCollection1 =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb1, "sharded.porcupine.tree");
    const ShardVersion shardVersionShardedCollection1 = ShardVersionFactory::make(
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(5, 0)}, CollectionPlacement(10, 1)),
        boost::optional<CollectionIndexes>(boost::none));

    const NamespaceString nssShardedCollection2 =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb2, "sharded.oasis");
    const ShardVersion shardVersionShardedCollection2 = ShardVersionFactory::make(
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(6, 0)}, CollectionPlacement(10, 1)),
        boost::optional<CollectionIndexes>(boost::none));

    // Used to cause a database version mismatch.
    const DatabaseVersion incorrectDatabaseVersion{UUID::gen(), Timestamp(3, 0)};
    // Used to cause a shard version mismatch.
    const ShardVersion incorrectShardVersion =
        ShardVersionFactory::make(ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(12, 0)},
                                               CollectionPlacement(10, 1)),
                                  boost::optional<CollectionIndexes>(boost::none));

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

void createTestCollection(OperationContext* opCtx, const NamespaceString& nss) {
    uassertStatusOK(createCollection(opCtx, nss.dbName(), BSON("create" << nss.coll())));
}

void installDatabaseMetadata(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             const DatabaseVersion& dbVersion) {
    AutoGetDb autoDb(opCtx, dbName, MODE_X);
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

void BulkWriteShardTest::setUp() {
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
    installDatabaseMetadata(opCtx(), dbNameTestDb1, dbVersionTestDb1);
    installDatabaseMetadata(opCtx(), dbNameTestDb2, dbVersionTestDb2);

    // Create nssUnshardedCollection1
    createTestCollection(opCtx(), nssUnshardedCollection1);
    installUnshardedCollectionMetadata(opCtx(), nssUnshardedCollection1);

    // Create nssShardedCollection1
    createTestCollection(opCtx(), nssShardedCollection1);
    const auto uuidShardedCollection1 = getCollectionUUID(_opCtx.get(), nssShardedCollection1);
    installShardedCollectionMetadata(
        opCtx(),
        nssShardedCollection1,
        dbVersionTestDb1,
        {ChunkType(uuidShardedCollection1,
                   ChunkRange{BSON("skey" << MINKEY), BSON("skey" << MAXKEY)},
                   shardVersionShardedCollection1.placementVersion(),
                   thisShardId)},
        thisShardId);

    // Create nssShardedCollection2
    createTestCollection(opCtx(), nssShardedCollection2);
    const auto uuidShardedCollection2 = getCollectionUUID(_opCtx.get(), nssShardedCollection2);
    installShardedCollectionMetadata(
        opCtx(),
        nssShardedCollection2,
        dbVersionTestDb2,
        {ChunkType(uuidShardedCollection2,
                   ChunkRange{BSON("skey" << MINKEY), BSON("skey" << MAXKEY)},
                   shardVersionShardedCollection2.placementVersion(),
                   thisShardId)},
        thisShardId);
}

void BulkWriteShardTest::tearDown() {
    _opCtx.reset();
    ServiceContextMongoDTest::tearDown();
    repl::ReplicationCoordinator::set(getGlobalServiceContext(), nullptr);
}

NamespaceInfoEntry nsInfoWithShardDatabaseVersions(NamespaceString nss,
                                                   boost::optional<DatabaseVersion> dv,
                                                   boost::optional<ShardVersion> sv) {
    NamespaceInfoEntry nsInfoEntry(nss);
    nsInfoEntry.setDatabaseVersion(dv);
    nsInfoEntry.setShardVersion(sv);
    return nsInfoEntry;
}

// Three successful ordered inserts into different collections.
TEST_F(BulkWriteShardTest, ThreeSuccessfulInsertsOrdered) {
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(0, BSON("x" << 1)),
         BulkWriteInsertOp(1, BSON("x" << -1)),
         BulkWriteInsertOp(2, BSON("x" << -1))},
        {
            nsInfoWithShardDatabaseVersions(
                nssUnshardedCollection1, dbVersionTestDb1, ShardVersion::UNSHARDED()),
            nsInfoWithShardDatabaseVersions(
                nssShardedCollection1, dbVersionTestDb1, shardVersionShardedCollection1),
            nsInfoWithShardDatabaseVersions(
                nssShardedCollection2, dbVersionTestDb2, shardVersionShardedCollection2),
        });

    auto replyItems = bulk_write::performWrites(opCtx(), request);

    ASSERT_EQ(3, replyItems.size());
    for (const auto& reply : replyItems) {
        ASSERT_OK(reply.getStatus());
    }

    OperationShardingState::get(opCtx()).resetShardingOperationFailedStatus();
}

// An insert into a sharded collection and an unsharded collection but have the first
// insert fail, resulting in skipping the second insert.
TEST_F(BulkWriteShardTest, OneFailingShardedOneSkippedUnshardedSuccessInsertOrdered) {
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(0, BSON("x" << 1)), BulkWriteInsertOp(1, BSON("x" << -1))},
        {nsInfoWithShardDatabaseVersions(
             nssShardedCollection1, dbVersionTestDb1, incorrectShardVersion),
         nsInfoWithShardDatabaseVersions(
             nssUnshardedCollection1, dbVersionTestDb1, ShardVersion::UNSHARDED())});

    auto replyItems = bulk_write::performWrites(opCtx(), request);

    ASSERT_EQ(1, replyItems.size());
    ASSERT_EQ(ErrorCodes::StaleConfig, replyItems.back().getStatus().code());

    OperationShardingState::get(opCtx()).resetShardingOperationFailedStatus();
}

// Two ordered inserts into the same sharded collection, but the sharded collection's metadata
// is stale and so the first write should fail and the second write should be skipped.
TEST_F(BulkWriteShardTest, TwoFailingShardedInsertsOrdered) {
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(0, BSON("x" << 1)), BulkWriteInsertOp(0, BSON("x" << -1))},
        {
            nsInfoWithShardDatabaseVersions(
                nssShardedCollection1, dbVersionTestDb1, incorrectShardVersion),
        });

    auto replyItems = bulk_write::performWrites(opCtx(), request);

    ASSERT_EQ(1, replyItems.size());
    ASSERT_EQ(ErrorCodes::StaleConfig, replyItems.back().getStatus().code());

    OperationShardingState::get(opCtx()).resetShardingOperationFailedStatus();
}

// Two ordered inserts into different sharded collections. The first is successful and
// the second is failing.
TEST_F(BulkWriteShardTest, OneSuccessfulShardedOneFailingShardedOrdered) {
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(0, BSON("x" << 1)), BulkWriteInsertOp(1, BSON("x" << -1))},
        {nsInfoWithShardDatabaseVersions(
             nssShardedCollection1, dbVersionTestDb1, shardVersionShardedCollection1),
         nsInfoWithShardDatabaseVersions(
             nssShardedCollection2, dbVersionTestDb2, incorrectShardVersion)});

    auto replyItems = bulk_write::performWrites(opCtx(), request);

    ASSERT_EQ(2, replyItems.size());
    ASSERT_OK(replyItems.front().getStatus());
    ASSERT_EQ(ErrorCodes::StaleConfig, replyItems.back().getStatus().code());

    OperationShardingState::get(opCtx()).resetShardingOperationFailedStatus();
}

// Two unordered inserts into the same sharded collection. On most errors we proceed
// with the rest of the operations, but on StaleConfig errors we don't.
TEST_F(BulkWriteShardTest, OneFailingShardedOneSkippedShardedUnordered) {
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(0, BSON("x" << 1)), BulkWriteInsertOp(0, BSON("x" << -1))},
        {nsInfoWithShardDatabaseVersions(
            nssShardedCollection1, dbVersionTestDb1, incorrectShardVersion)});
    request.setOrdered(false);

    auto replyItems = bulk_write::performWrites(opCtx(), request);

    ASSERT_EQ(1, replyItems.size());
    ASSERT_EQ(ErrorCodes::StaleConfig, replyItems.back().getStatus().code());

    OperationShardingState::get(opCtx()).resetShardingOperationFailedStatus();
}

// Two unordered inserts into different sharded collections. Despite being unordered
// inserts, the implementation will halt on the very first error.
TEST_F(BulkWriteShardTest, OneSuccessfulShardedOneFailingShardedUnordered) {
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(0, BSON("x" << 1)), BulkWriteInsertOp(1, BSON("x" << -1))},
        {nsInfoWithShardDatabaseVersions(
             nssShardedCollection1, dbVersionTestDb1, incorrectShardVersion),
         nsInfoWithShardDatabaseVersions(
             nssShardedCollection2, dbVersionTestDb2, shardVersionShardedCollection2)});
    request.setOrdered(false);

    auto replyItems = bulk_write::performWrites(opCtx(), request);

    ASSERT_EQ(1, replyItems.size());
    ASSERT_EQ(ErrorCodes::StaleConfig, replyItems.back().getStatus().code());

    OperationShardingState::get(opCtx()).resetShardingOperationFailedStatus();
}

// Ordered inserts and updates into different collections where all succeed.
TEST_F(BulkWriteShardTest, InsertsAndUpdatesSuccessOrdered) {
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(2, BSON("x" << 1)),
         BulkWriteInsertOp(0, BSON("x" << 3)),
         BulkWriteUpdateOp(0, BSON("x" << BSON("$gt" << 0)), BSON("x" << -9)),
         BulkWriteInsertOp(1, BSON("x" << -1))},
        {nsInfoWithShardDatabaseVersions(
             nssShardedCollection1, dbVersionTestDb1, shardVersionShardedCollection1),
         nsInfoWithShardDatabaseVersions(
             nssShardedCollection2, dbVersionTestDb2, shardVersionShardedCollection2),
         nsInfoWithShardDatabaseVersions(
             nssUnshardedCollection1, dbVersionTestDb1, ShardVersion::UNSHARDED())});

    auto replyItems = bulk_write::performWrites(opCtx(), request);

    ASSERT_EQ(4, replyItems.size());
    for (const auto& reply : replyItems) {
        ASSERT_OK(reply.getStatus());
    }

    OperationShardingState::get(opCtx()).resetShardingOperationFailedStatus();
}

// Unordered inserts and updates into different collections where all succeed.
TEST_F(BulkWriteShardTest, InsertsAndUpdatesSuccessUnordered) {
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(2, BSON("x" << 1)),
         BulkWriteInsertOp(0, BSON("x" << 3)),
         BulkWriteUpdateOp(0, BSON("x" << BSON("$gt" << 0)), BSON("x" << -9)),
         BulkWriteInsertOp(1, BSON("x" << -1))},
        {nsInfoWithShardDatabaseVersions(
             nssShardedCollection1, dbVersionTestDb1, shardVersionShardedCollection1),
         nsInfoWithShardDatabaseVersions(
             nssShardedCollection2, dbVersionTestDb2, shardVersionShardedCollection2),
         nsInfoWithShardDatabaseVersions(
             nssUnshardedCollection1, dbVersionTestDb1, ShardVersion::UNSHARDED())});

    request.setOrdered(false);

    auto replyItems = bulk_write::performWrites(opCtx(), request);

    ASSERT_EQ(4, replyItems.size());
    for (const auto& reply : replyItems) {
        ASSERT_OK(reply.getStatus());
    }

    OperationShardingState::get(opCtx()).resetShardingOperationFailedStatus();
}

// Unordered inserts and updates into different collections where some fail.
TEST_F(BulkWriteShardTest, InsertsAndUpdatesFailUnordered) {
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(2, BSON("x" << 1)),
         BulkWriteInsertOp(0, BSON("x" << 3)),
         BulkWriteUpdateOp(0, BSON("x" << BSON("$gt" << 0)), BSON("x" << -9)),
         BulkWriteInsertOp(1, BSON("x" << -1))},
        {nsInfoWithShardDatabaseVersions(
             nssShardedCollection1, dbVersionTestDb1, incorrectShardVersion),
         nsInfoWithShardDatabaseVersions(
             nssShardedCollection2, dbVersionTestDb2, shardVersionShardedCollection2),
         nsInfoWithShardDatabaseVersions(
             nssUnshardedCollection1, dbVersionTestDb1, ShardVersion::UNSHARDED())});

    request.setOrdered(false);

    auto replyItems = bulk_write::performWrites(opCtx(), request);

    ASSERT_EQ(2, replyItems.size());
    ASSERT_OK(replyItems.front().getStatus());
    ASSERT_EQ(ErrorCodes::StaleConfig, replyItems.back().getStatus().code());

    OperationShardingState::get(opCtx()).resetShardingOperationFailedStatus();
}

// TODO (SERVER-75202): Re-enable this test & write a test for deletes.
// Unordered updates into different collections where some fail.
// TEST_F(BulkWriteShardTest, UpdatesFailUnordered) {
//     BulkWriteCommandRequest request(
//         {
//         BulkWriteUpdateOp(1, BSON("x" << BSON("$gt" << 0)), BSON("x" << -99)),
//         BulkWriteUpdateOp(0, BSON("x" << BSON("$gt" << 0)), BSON("x" << -9)),
//         BulkWriteInsertOp(1, BSON("x" << -1))},
//         {nsInfoWithShardDatabaseVersions(
//              nssShardedCollection1, dbVersionTestDb, incorrectShardVersion),
//          nsInfoWithShardDatabaseVersions(
//              nssShardedCollection2, dbVersionTestDb, shardVersionShardedCollection2)});

//     request.setOrdered(false);

//     auto replyItems = bulk_write::performWrites(opCtx(), request);

//     ASSERT_EQ(2, replyItems.size());
//     ASSERT_OK(replyItems.front().getStatus());
//     ASSERT_EQ(ErrorCodes::StaleConfig, replyItems[1].getStatus().code());

//     OperationShardingState::get(opCtx()).resetShardingOperationFailedStatus();
// }

// After the first insert fails due to an incorrect database version, the rest
// of the writes are skipped when operations are ordered.
TEST_F(BulkWriteShardTest, FirstFailsRestSkippedStaleDbVersionOrdered) {
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(0, BSON("x" << 1)),
         BulkWriteInsertOp(0, BSON("x" << -1)),
         BulkWriteInsertOp(1, BSON("x" << -2))},
        {nsInfoWithShardDatabaseVersions(
             nssShardedCollection1, incorrectDatabaseVersion, shardVersionShardedCollection1),
         nsInfoWithShardDatabaseVersions(
             nssShardedCollection2, dbVersionTestDb2, shardVersionShardedCollection2)});

    auto replyItems = bulk_write::performWrites(opCtx(), request);

    ASSERT_EQ(1, replyItems.size());
    ASSERT_EQ(ErrorCodes::StaleDbVersion, replyItems.back().getStatus().code());

    OperationShardingState::get(opCtx()).resetShardingOperationFailedStatus();
}

// After the second insert fails due to an incorrect database version, the rest
// of the writes are skipped when operations are unordered.
TEST_F(BulkWriteShardTest, FirstFailsRestSkippedStaleDbVersionUnordered) {
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(1, BSON("x" << 1)),
         BulkWriteInsertOp(0, BSON("x" << -1)),
         BulkWriteInsertOp(1, BSON("x" << -2))},
        {nsInfoWithShardDatabaseVersions(
             nssUnshardedCollection1, incorrectDatabaseVersion, ShardVersion::UNSHARDED()),
         nsInfoWithShardDatabaseVersions(
             nssShardedCollection2, dbVersionTestDb2, shardVersionShardedCollection2)});

    request.setOrdered(false);
    auto replyItems = bulk_write::performWrites(opCtx(), request);

    ASSERT_EQ(2, replyItems.size());
    ASSERT_OK(replyItems.front().getStatus());
    ASSERT_EQ(ErrorCodes::StaleDbVersion, replyItems.back().getStatus().code());

    OperationShardingState::get(opCtx()).resetShardingOperationFailedStatus();
}

}  // namespace
}  // namespace mongo
