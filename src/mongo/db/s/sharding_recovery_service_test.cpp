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

#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_critical_section_document_gen.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_server_op_observer.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/sharding_migration_critical_section.h"
#include "mongo/db/s/sharding_recovery_service.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"

namespace mongo {

class ShardingRecoveryServiceTest : public ShardServerTestFixture {
public:
    inline static const NamespaceString collNss =
        NamespaceString::createNamespaceString_forTest("TestDB", "TestCollection");
    inline static const BSONObj collOpReason =
        BSON("Dummy operation on collection" << collNss.ns_forTest());

    inline static const NamespaceString dbName =
        NamespaceString::createNamespaceString_forTest("TestDB");
    inline static const BSONObj dbOpReason =
        BSON("Dummy operation on database" << dbName.ns_forTest());

    inline static const BSONObj differentOpReason = BSON("Yet another dummy operation" << true);

    void setUp() override {
        ShardServerTestFixture::setUp();
        _opCtx = operationContext();
    }

    OperationContext* opCtx() {
        return _opCtx;
    }

    OpObserver& opObserver() {
        return _opObserver;
    }

    boost::optional<CollectionCriticalSectionDocument> readCriticalSectionDocument(
        const NamespaceString& nss, const BSONObj& reason) {
        FindCommandRequest findOp(NamespaceString::kCollectionCriticalSectionsNamespace);
        findOp.setFilter(
            BSON(CollectionCriticalSectionDocument::kNssFieldName << nss.toString_forTest()));

        DBDirectClient dbClient(opCtx());
        auto cursor = dbClient.find(std::move(findOp));

        if (!cursor->more()) {
            return boost::none;
        }

        auto bsonObj = cursor->next();
        auto doc = CollectionCriticalSectionDocument::parse(
            IDLParserContext("AcquireRecoverableCSBW"), bsonObj);

        // The document exists, so the reason must match.
        ASSERT(!doc.getReason().woCompare(reason));

        return doc;
    }

    void writeReadCriticalSectionDocument(const NamespaceString& nss,
                                          const BSONObj& reason,
                                          bool blockReads) {
        auto doc = readCriticalSectionDocument(nss, reason);
        if (!doc) {
            doc = CollectionCriticalSectionDocument(nss, reason, blockReads);

            DBDirectClient dbClient(opCtx());
            dbClient.insert(NamespaceString::kCollectionCriticalSectionsNamespace, doc->toBSON());

            return;
        }

        // The document exists, so the blockReads should change.
        ASSERT_NE(doc->getBlockReads(), blockReads);

        DBDirectClient dbClient(opCtx());
        dbClient.update(
            NamespaceString::kCollectionCriticalSectionsNamespace,
            BSON(CollectionCriticalSectionDocument::kNssFieldName << nss.toString_forTest()),
            BSON("$set" << BSON(CollectionCriticalSectionDocument::kBlockReadsFieldName
                                << blockReads)));
    }

    void deleteReadCriticalSectionDocument(const NamespaceString& nss, const BSONObj& reason) {
        // The document must exist.
        ASSERT(readCriticalSectionDocument(nss, reason));

        DBDirectClient dbClient(opCtx());
        dbClient.remove(
            NamespaceString::kCollectionCriticalSectionsNamespace,
            BSON(CollectionCriticalSectionDocument::kNssFieldName << nss.toString_forTest()),
            false /* removeMany */);
    }

    void assertCriticalSectionCatchUpEnteredInMemory(const NamespaceString& nss) {
        if (nsIsDbOnly(nss.ns_forTest())) {
            AutoGetDb db(opCtx(), nss.dbName(), MODE_IS);
            const auto scopedDss =
                DatabaseShardingState::assertDbLockedAndAcquireShared(opCtx(), nss.dbName());
            ASSERT(scopedDss->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite));
            ASSERT(!scopedDss->getCriticalSectionSignal(ShardingMigrationCriticalSection::kRead));
        } else {
            AutoGetCollection coll(opCtx(), nss, MODE_IS);
            const auto csr =
                CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx(), nss);
            ASSERT(
                csr->getCriticalSectionSignal(opCtx(), ShardingMigrationCriticalSection::kWrite));
            ASSERT(
                !csr->getCriticalSectionSignal(opCtx(), ShardingMigrationCriticalSection::kRead));
        }
    }

    void assertCriticalSectionCommitEnteredInMemory(const NamespaceString& nss) {
        if (nsIsDbOnly(nss.ns_forTest())) {
            AutoGetDb db(opCtx(), nss.dbName(), MODE_IS);
            const auto scopedDss =
                DatabaseShardingState::assertDbLockedAndAcquireShared(opCtx(), nss.dbName());
            ASSERT(scopedDss->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite));
            ASSERT(scopedDss->getCriticalSectionSignal(ShardingMigrationCriticalSection::kRead));
        } else {
            AutoGetCollection coll(opCtx(), nss, MODE_IS);
            const auto csr =
                CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx(), nss);
            ASSERT(
                csr->getCriticalSectionSignal(opCtx(), ShardingMigrationCriticalSection::kWrite));
            ASSERT(csr->getCriticalSectionSignal(opCtx(), ShardingMigrationCriticalSection::kRead));
        }
    }

    void assertCriticalSectionLeftInMemory(const NamespaceString& nss) {
        if (nsIsDbOnly(nss.ns_forTest())) {
            AutoGetDb db(opCtx(), nss.dbName(), MODE_IS);
            const auto scopedDss =
                DatabaseShardingState::assertDbLockedAndAcquireShared(opCtx(), nss.dbName());
            ASSERT(!scopedDss->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite));
            ASSERT(!scopedDss->getCriticalSectionSignal(ShardingMigrationCriticalSection::kRead));
        } else {
            AutoGetCollection coll(opCtx(), nss, MODE_IS);
            const auto csr =
                CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx(), nss);
            ASSERT(
                !csr->getCriticalSectionSignal(opCtx(), ShardingMigrationCriticalSection::kWrite));
            ASSERT(
                !csr->getCriticalSectionSignal(opCtx(), ShardingMigrationCriticalSection::kRead));
        }
    }

    void assertCriticalSectionCatchUpEnteredOnDisk(const NamespaceString& nss,
                                                   const BSONObj& reason) {
        auto doc = readCriticalSectionDocument(nss, reason);
        ASSERT(doc);
        ASSERT(!doc->getBlockReads());
    }

    void assertCriticalSectionCommitEnteredOnDisk(const NamespaceString& nss,
                                                  const BSONObj& reason) {
        auto doc = readCriticalSectionDocument(nss, reason);
        ASSERT(doc);
        ASSERT(doc->getBlockReads());
    }

    void assertCriticalSectionLeftOnDisk(const NamespaceString& nss, const BSONObj& reason) {
        ASSERT(!readCriticalSectionDocument(nss, reason));
    }

private:
    OperationContext* _opCtx;
    ShardServerOpObserver _opObserver;
};

class ShardingRecoveryServiceTestOnPrimary : public ShardingRecoveryServiceTest {
public:
    void setUp() override {
        ShardingRecoveryServiceTest::setUp();

        auto replCoord = repl::ReplicationCoordinator::get(operationContext());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    }
};

class ShardingRecoveryServiceTestOnSecondary : public ShardingRecoveryServiceTest {
public:
    void setUp() override {
        ShardingRecoveryServiceTest::setUp();

        auto replCoord = repl::ReplicationCoordinator::get(operationContext());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_SECONDARY));

        // Create and lock the `config.collection_critical_sections` collection to allow
        // notifications on the operation observer.
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            opCtx());
        ASSERT_OK(createCollection(
            opCtx(), CreateCommand(NamespaceString::kCollectionCriticalSectionsNamespace)));
        _criticalSectionColl.emplace(
            opCtx(), NamespaceString::kCollectionCriticalSectionsNamespace, MODE_IX);
    }

    void tearDown() override {
        _criticalSectionColl = boost::none;

        ShardingRecoveryServiceTest::tearDown();
    }

    const CollectionPtr& criticalSectionColl() {
        return **_criticalSectionColl;
    }

private:
    boost::optional<AutoGetCollection> _criticalSectionColl;
};

class ShardingRecoveryServiceTestonInitialData : public ShardingRecoveryServiceTest {
public:
    void setUp() override {
        ShardingRecoveryServiceTest::setUp();

        // Create the `config.collection_critical_sections` collection to allow notifications on the
        // operation observer.
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            opCtx());
        ASSERT_OK(createCollection(
            opCtx(), CreateCommand(NamespaceString::kCollectionCriticalSectionsNamespace)));
    }
};

using ShardingRecoveryServiceTestAfterRollback = ShardingRecoveryServiceTestonInitialData;

////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(ShardingRecoveryServiceTestOnPrimary, BlockAndUnblockOperationsOnDatabase) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(opCtx())->acquireRecoverableCriticalSectionBlockWrites(
        opCtx(), dbName, dbOpReason, ShardingCatalogClient::kLocalWriteConcern);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(dbName);

    // Check that the document has been appropriately saved.
    assertCriticalSectionCatchUpEnteredOnDisk(dbName, dbOpReason);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    ShardingRecoveryService::get(opCtx())->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx(), dbName, dbOpReason, ShardingCatalogClient::kLocalWriteConcern);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(dbName);

    // Check that the document has been appropriately updated.
    assertCriticalSectionCommitEnteredOnDisk(dbName, dbOpReason);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    ShardingRecoveryService::get(opCtx())->releaseRecoverableCriticalSection(
        opCtx(), dbName, dbOpReason, ShardingCatalogClient::kLocalWriteConcern);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(dbName);

    // Check that the document has been deleted.
    assertCriticalSectionLeftOnDisk(dbName, dbOpReason);
}

TEST_F(ShardingRecoveryServiceTestOnPrimary, BlockAndUnblockOperationsTwiceOnDatabase) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(opCtx())->acquireRecoverableCriticalSectionBlockWrites(
        opCtx(), dbName, dbOpReason, ShardingCatalogClient::kLocalWriteConcern);

    ShardingRecoveryService::get(opCtx())->acquireRecoverableCriticalSectionBlockWrites(
        opCtx(), dbName, dbOpReason, ShardingCatalogClient::kLocalWriteConcern);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(dbName);

    // Check that the document has been appropriately saved.
    assertCriticalSectionCatchUpEnteredOnDisk(dbName, dbOpReason);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    ShardingRecoveryService::get(opCtx())->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx(), dbName, dbOpReason, ShardingCatalogClient::kLocalWriteConcern);

    ShardingRecoveryService::get(opCtx())->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx(), dbName, dbOpReason, ShardingCatalogClient::kLocalWriteConcern);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(dbName);

    // Check that the document has been appropriately updated.
    assertCriticalSectionCommitEnteredOnDisk(dbName, dbOpReason);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    ShardingRecoveryService::get(opCtx())->releaseRecoverableCriticalSection(
        opCtx(), dbName, dbOpReason, ShardingCatalogClient::kLocalWriteConcern);

    ShardingRecoveryService::get(opCtx())->releaseRecoverableCriticalSection(
        opCtx(), dbName, dbOpReason, ShardingCatalogClient::kLocalWriteConcern);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(dbName);

    // Check that the document has been deleted.
    assertCriticalSectionLeftOnDisk(dbName, dbOpReason);
}

DEATH_TEST_F(ShardingRecoveryServiceTestOnPrimary,
             FailBlockingWritesTwiceOnDatabaseWithDifferentReasons,
             "invariant") {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(opCtx())->acquireRecoverableCriticalSectionBlockWrites(
        opCtx(), dbName, dbOpReason, ShardingCatalogClient::kLocalWriteConcern);

    ShardingRecoveryService::get(opCtx())->acquireRecoverableCriticalSectionBlockWrites(
        opCtx(), dbName, differentOpReason, ShardingCatalogClient::kLocalWriteConcern);
}

DEATH_TEST_F(ShardingRecoveryServiceTestOnPrimary,
             FailBlockingReadsOnDatabaseWithDifferentReasons,
             "invariant") {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(opCtx())->acquireRecoverableCriticalSectionBlockWrites(
        opCtx(), dbName, dbOpReason, ShardingCatalogClient::kLocalWriteConcern);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    ShardingRecoveryService::get(opCtx())->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx(), dbName, differentOpReason, ShardingCatalogClient::kLocalWriteConcern);
}

DEATH_TEST_F(ShardingRecoveryServiceTestOnPrimary,
             FailUnblockingOperationsOnDatabaseWithDifferentReasons,
             "invariant") {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(opCtx())->acquireRecoverableCriticalSectionBlockWrites(
        opCtx(), dbName, dbOpReason, ShardingCatalogClient::kLocalWriteConcern);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    ShardingRecoveryService::get(opCtx())->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx(), dbName, dbOpReason, ShardingCatalogClient::kLocalWriteConcern);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    ShardingRecoveryService::get(opCtx())->releaseRecoverableCriticalSection(
        opCtx(), dbName, differentOpReason, ShardingCatalogClient::kLocalWriteConcern);
}

TEST_F(ShardingRecoveryServiceTestOnPrimary, BlockAndUnblockOperationsOnCollection) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(opCtx())->acquireRecoverableCriticalSectionBlockWrites(
        opCtx(), collNss, collOpReason, ShardingCatalogClient::kLocalWriteConcern);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(collNss);

    // Check that the document has been appropriately saved.
    assertCriticalSectionCatchUpEnteredOnDisk(collNss, collOpReason);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    ShardingRecoveryService::get(opCtx())->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx(), collNss, collOpReason, ShardingCatalogClient::kLocalWriteConcern);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(collNss);

    // Check that the document has been appropriately updated.
    assertCriticalSectionCommitEnteredOnDisk(collNss, collOpReason);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    ShardingRecoveryService::get(opCtx())->releaseRecoverableCriticalSection(
        opCtx(), collNss, collOpReason, ShardingCatalogClient::kLocalWriteConcern);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(collNss);

    // Check that the document has been deleted.
    assertCriticalSectionLeftOnDisk(collNss, collOpReason);
}

TEST_F(ShardingRecoveryServiceTestOnPrimary, BlockAndUnblockOperationsTwiceOnCollection) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(opCtx())->acquireRecoverableCriticalSectionBlockWrites(
        opCtx(), collNss, collOpReason, ShardingCatalogClient::kLocalWriteConcern);

    ShardingRecoveryService::get(opCtx())->acquireRecoverableCriticalSectionBlockWrites(
        opCtx(), collNss, collOpReason, ShardingCatalogClient::kLocalWriteConcern);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(collNss);

    // Check that the document has been appropriately saved.
    assertCriticalSectionCatchUpEnteredOnDisk(collNss, collOpReason);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    ShardingRecoveryService::get(opCtx())->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx(), collNss, collOpReason, ShardingCatalogClient::kLocalWriteConcern);

    ShardingRecoveryService::get(opCtx())->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx(), collNss, collOpReason, ShardingCatalogClient::kLocalWriteConcern);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(collNss);

    // Check that the document has been appropriately updated.
    assertCriticalSectionCommitEnteredOnDisk(collNss, collOpReason);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    ShardingRecoveryService::get(opCtx())->releaseRecoverableCriticalSection(
        opCtx(), collNss, collOpReason, ShardingCatalogClient::kLocalWriteConcern);

    ShardingRecoveryService::get(opCtx())->releaseRecoverableCriticalSection(
        opCtx(), collNss, collOpReason, ShardingCatalogClient::kLocalWriteConcern);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(collNss);

    // Check that the document has been deleted.
    assertCriticalSectionLeftOnDisk(collNss, collOpReason);
}

DEATH_TEST_F(ShardingRecoveryServiceTestOnPrimary,
             FailBlockingWritesTwiceOnCollectionWithDifferentReasons,
             "invariant") {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(opCtx())->acquireRecoverableCriticalSectionBlockWrites(
        opCtx(), collNss, collOpReason, ShardingCatalogClient::kLocalWriteConcern);

    ShardingRecoveryService::get(opCtx())->acquireRecoverableCriticalSectionBlockWrites(
        opCtx(), collNss, differentOpReason, ShardingCatalogClient::kLocalWriteConcern);
}

DEATH_TEST_F(ShardingRecoveryServiceTestOnPrimary,
             FailBlockingReadsOnCollectionWithDifferentReasons,
             "invariant") {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(opCtx())->acquireRecoverableCriticalSectionBlockWrites(
        opCtx(), collNss, collOpReason, ShardingCatalogClient::kLocalWriteConcern);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    ShardingRecoveryService::get(opCtx())->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx(), collNss, differentOpReason, ShardingCatalogClient::kLocalWriteConcern);
}

DEATH_TEST_F(ShardingRecoveryServiceTestOnPrimary,
             FailUnblockingOperationsOnCollectionWithDifferentReasons,
             "invariant") {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    ShardingRecoveryService::get(opCtx())->acquireRecoverableCriticalSectionBlockWrites(
        opCtx(), collNss, collOpReason, ShardingCatalogClient::kLocalWriteConcern);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    ShardingRecoveryService::get(opCtx())->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx(), collNss, collOpReason, ShardingCatalogClient::kLocalWriteConcern);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    ShardingRecoveryService::get(opCtx())->releaseRecoverableCriticalSection(
        opCtx(), collNss, differentOpReason, ShardingCatalogClient::kLocalWriteConcern);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(ShardingRecoveryServiceTestOnSecondary, BlockAndUnblockOperationsOnDatabase) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    // Simulate an insert notification on the `config.collection_critical_sections` collection, that
    // is what a secondary node would receive when the primary node enters the catch-up phase of the
    // critical section.
    auto doc = CollectionCriticalSectionDocument(dbName, dbOpReason, false);
    {
        std::vector<InsertStatement> inserts;
        inserts.emplace_back(doc.toBSON());

        WriteUnitOfWork wuow(opCtx());
        AutoGetDb db(opCtx(), dbName.dbName(), MODE_IX);
        opObserver().onInserts(opCtx(),
                               criticalSectionColl(),
                               inserts.begin(),
                               inserts.end(),
                               /*fromMigrate=*/std::vector<bool>(inserts.size(), false),
                               /*defaultFromMigrate=*/false);
        wuow.commit();
    }

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(dbName);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    // Simulate an update notification on the `config.collection_critical_sections` collection, that
    // is what a secondary node would receive when the primary node enters the commit phase of the
    // critical section.
    auto preImageDoc = doc.toBSON();
    doc.setBlockReads(true);
    {
        CollectionUpdateArgs updateArgs{preImageDoc};
        updateArgs.updatedDoc = doc.toBSON();
        OplogUpdateEntryArgs update(&updateArgs, criticalSectionColl());

        WriteUnitOfWork wuow(opCtx());
        AutoGetDb db(opCtx(), dbName.dbName(), MODE_IX);
        opObserver().onUpdate(opCtx(), update);
        wuow.commit();
    }

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(dbName);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    // Simulate an delete notification on the `config.collection_critical_sections` collection, that
    // is what a secondary node would receive when the primary node leaves the critical section.
    {
        WriteUnitOfWork wuow(opCtx());
        AutoGetDb db(opCtx(), dbName.dbName(), MODE_IX);
        OplogDeleteEntryArgs args;
        opObserver().aboutToDelete(opCtx(), criticalSectionColl(), doc.toBSON(), &args);
        opObserver().onDelete(
            opCtx(), criticalSectionColl(), kUninitializedStmtId, doc.toBSON(), args);
        wuow.commit();
    }

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(dbName);
}

TEST_F(ShardingRecoveryServiceTestOnSecondary, BlockAndUnblockOperationsOnCollection) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    // Simulate an insert notification on the `config.collection_critical_sections` collection, that
    // is what a secondary node would receive when the primary node enters the catch-up phase of the
    // critical section.
    auto doc = CollectionCriticalSectionDocument(collNss, collOpReason, false);
    auto preImageDoc = doc.toBSON();
    {
        std::vector<InsertStatement> inserts;
        inserts.emplace_back(doc.toBSON());

        WriteUnitOfWork wuow(opCtx());
        AutoGetCollection coll(opCtx(), collNss, MODE_IX);
        opObserver().onInserts(opCtx(),
                               criticalSectionColl(),
                               inserts.begin(),
                               inserts.end(),
                               /*fromMigrate=*/std::vector<bool>(inserts.size(), false),
                               /*defaultFromMigrate=*/false);
        wuow.commit();
    }

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(collNss);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    // Simulate an update notification on the `config.collection_critical_sections` collection, that
    // is what a secondary node would receive when the primary node enters the commit phase of the
    // critical section.
    doc.setBlockReads(true);
    {
        CollectionUpdateArgs updateArgs{preImageDoc};
        updateArgs.updatedDoc = doc.toBSON();
        OplogUpdateEntryArgs update(&updateArgs, criticalSectionColl());

        WriteUnitOfWork wuow(opCtx());
        AutoGetCollection coll(opCtx(), collNss, MODE_IX);
        opObserver().onUpdate(opCtx(), update);
        wuow.commit();
    }

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(collNss);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    // Simulate an delete notification on the `config.collection_critical_sections` collection, that
    // is what a secondary node would receive when the primary node leaves the critical section.
    {
        WriteUnitOfWork wuow(opCtx());
        AutoGetCollection coll(opCtx(), collNss, MODE_IX);
        OplogDeleteEntryArgs args;
        opObserver().aboutToDelete(opCtx(), criticalSectionColl(), doc.toBSON(), &args);
        opObserver().onDelete(
            opCtx(), criticalSectionColl(), kUninitializedStmtId, doc.toBSON(), args);
        wuow.commit();
    }

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(collNss);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(ShardingRecoveryServiceTestonInitialData, BlockAndUnblockOperationsOnDatabase) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    // Insert a document in the `config.collection_critical_sections` collection to simulate
    // entering the catch-up phase of the critical section for a database, then react to an
    // hypothetical availability of initial data.
    {
        AutoGetDb db(opCtx(), dbName.dbName(), MODE_IX);
        writeReadCriticalSectionDocument(dbName, dbOpReason, false /* blockReads */);
    }
    ShardingRecoveryService::get(opCtx())->onInitialDataAvailable(opCtx(), false);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(dbName);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    // Update the document in the `config.collection_critical_sections` collection to simulate
    // entering the commit phase of the critical section for the database, then react to an
    // hypothetical availability of initial data.
    {
        AutoGetDb db(opCtx(), dbName.dbName(), MODE_IX);
        writeReadCriticalSectionDocument(dbName, dbOpReason, true /* blockReads */);
    }
    ShardingRecoveryService::get(opCtx())->onInitialDataAvailable(opCtx(), false);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(dbName);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    // Delete the document in the `config.collection_critical_sections` collection to simulate
    // leaving the critical section for the database, then react to an hypothetical availability of
    // initial data.
    {
        AutoGetDb db(opCtx(), dbName.dbName(), MODE_IX);
        deleteReadCriticalSectionDocument(dbName, dbOpReason);
    }
    ShardingRecoveryService::get(opCtx())->onInitialDataAvailable(opCtx(), false);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(dbName);
}

TEST_F(ShardingRecoveryServiceTestonInitialData, BlockAndUnblockOperationsOnCollection) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    // Insert a document in the `config.collection_critical_sections` collection to simulate
    // entering the catch-up phase of the critical section for a collection, then react to an
    // hypothetical  availability of initial data.
    {
        AutoGetCollection coll(opCtx(), collNss, MODE_IX);
        writeReadCriticalSectionDocument(collNss, collOpReason, false /* blockReads */);
    }
    ShardingRecoveryService::get(opCtx())->onInitialDataAvailable(opCtx(), false);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(collNss);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    // Update the document in the `config.collection_critical_sections` collection to simulate
    // entering the commit phase of the critical section for the collection, then react to an
    // hypothetical  availability of initial data.
    {
        AutoGetCollection coll(opCtx(), collNss, MODE_IX);
        writeReadCriticalSectionDocument(collNss, collOpReason, true /* blockReads */);
    }
    ShardingRecoveryService::get(opCtx())->onInitialDataAvailable(opCtx(), false);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(collNss);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    // Delete the document in the `config.collection_critical_sections` collection to simulate
    // leaving the critical section for the collection, then react to an hypothetical  availability
    // of initial data.
    {
        AutoGetCollection coll(opCtx(), collNss, MODE_IX);
        deleteReadCriticalSectionDocument(collNss, collOpReason);
    }
    ShardingRecoveryService::get(opCtx())->onInitialDataAvailable(opCtx(), false);

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(collNss);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(ShardingRecoveryServiceTestAfterRollback, BlockAndUnblockOperationsOnDatabase) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    // Insert a document in the `config.collection_critical_sections` collection to simulate
    // entering the catch-up phase of the critical section for a database, then react to an
    // hypothetical replication rollback.
    {
        AutoGetDb db(opCtx(), dbName.dbName(), MODE_IX);
        writeReadCriticalSectionDocument(dbName, dbOpReason, false /* blockReads */);
    }
    ShardingRecoveryService::get(opCtx())->recoverStates(
        opCtx(), {NamespaceString::kCollectionCriticalSectionsNamespace});

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(dbName);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    // Update the document in the `config.collection_critical_sections` collection to simulate
    // entering the commit phase of the critical section for the database, then react to an
    // hypothetical replication rollback.
    {
        AutoGetDb db(opCtx(), dbName.dbName(), MODE_IX);
        writeReadCriticalSectionDocument(dbName, dbOpReason, true /* blockReads */);
    }
    ShardingRecoveryService::get(opCtx())->recoverStates(
        opCtx(), {NamespaceString::kCollectionCriticalSectionsNamespace});

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(dbName);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    // Delete the document in the `config.collection_critical_sections` collection to simulate
    // leaving the critical section for the database, then react to an hypothetical replication
    // rollback.
    {
        AutoGetDb db(opCtx(), dbName.dbName(), MODE_IX);
        deleteReadCriticalSectionDocument(dbName, dbOpReason);
    }
    ShardingRecoveryService::get(opCtx())->recoverStates(
        opCtx(), {NamespaceString::kCollectionCriticalSectionsNamespace});

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(dbName);
}

TEST_F(ShardingRecoveryServiceTestAfterRollback, BlockAndUnblockOperationsOnCollection) {
    ///////////////////////////////////////////
    // Block write operations (catch-up phase)
    ///////////////////////////////////////////

    // Insert a document in the `config.collection_critical_sections` collection to simulate
    // entering the catch-up phase of the critical section for a collection, then react to an
    // hypothetical replication rollback.
    {
        AutoGetCollection coll(opCtx(), collNss, MODE_IX);
        writeReadCriticalSectionDocument(collNss, collOpReason, false /* blockReads */);
    }
    ShardingRecoveryService::get(opCtx())->recoverStates(
        opCtx(), {NamespaceString::kCollectionCriticalSectionsNamespace});

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCatchUpEnteredInMemory(collNss);

    //////////////////////////////////////////////
    // Block read/write operations (commit phase)
    //////////////////////////////////////////////

    // Update the document in the `config.collection_critical_sections` collection to simulate
    // entering the commit phase of the critical section for the collection, then react to an
    // hypothetical replication rollback.
    {
        AutoGetCollection coll(opCtx(), collNss, MODE_IX);
        writeReadCriticalSectionDocument(collNss, collOpReason, true /* blockReads */);
    }
    ShardingRecoveryService::get(opCtx())->recoverStates(
        opCtx(), {NamespaceString::kCollectionCriticalSectionsNamespace});

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionCommitEnteredInMemory(collNss);

    /////////////////////////////////
    // Unblock read/write operations
    /////////////////////////////////

    // Delete the document in the `config.collection_critical_sections` collection to simulate
    // leaving the critical section for the collection, then react to an hypothetical replication
    // rollback.
    {
        AutoGetCollection coll(opCtx(), collNss, MODE_IX);
        deleteReadCriticalSectionDocument(collNss, collOpReason);
    }
    ShardingRecoveryService::get(opCtx())->recoverStates(
        opCtx(), {NamespaceString::kCollectionCriticalSectionsNamespace});

    // Check that the in-memory status has been appropriately updated.
    assertCriticalSectionLeftInMemory(collNss);
}

}  // namespace mongo
