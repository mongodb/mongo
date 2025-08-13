/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/auth/auth_op_observer.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/db/auth/authorization_backend_mock.h"
#include "mongo/db/auth/authorization_client_handle_shard.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_router_impl_for_test.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <set>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

BSONObj makeUserDocument(
    StringData id, StringData userName, StringData dbName, BSONObj credentials, BSONArray roles) {
    return BSON("_id" << id << "user" << userName << "db" << dbName << "credentials" << credentials
                      << "roles" << roles);
}

class AuthOpObserverTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        // Set up mongod.
        ServiceContextMongoDTest::setUp();

        auto service = getServiceContext();
        auto opCtx = cc().makeOperationContext();
        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceMock>());

        authzManager = AuthorizationManager::get(getService());

        // This is the only test that needs to access the AuthorizationRouter directly.
        auto impl = reinterpret_cast<AuthorizationManagerImpl*>(authzManager);
        invariant(impl);
        impl->setAuthEnabled(true);
        mockRouter = reinterpret_cast<AuthorizationRouterImplForTest*>(
            impl->getAuthorizationRouter_forTest());
        invariant(mockRouter);

        // Ensure that we are primary.
        repl::ReplicationCoordinator::set(
            service,
            std::make_unique<repl::ReplicationCoordinatorMock>(service, createReplSettings()));
        repl::createOplog(opCtx.get());

        auto replCoord = repl::ReplicationCoordinator::get(service);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

        // Create test and users collections.
        testNss = NamespaceString::createNamespaceString_forTest("test", "coll");
        writeConflictRetry(opCtx.get(), "createColl", testNss, [&] {
            shard_role_details::getRecoveryUnit(opCtx.get())
                ->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
            shard_role_details::getRecoveryUnit(opCtx.get())->abandonSnapshot();

            WriteUnitOfWork wunit(opCtx.get());
            AutoGetCollection collRaii(opCtx.get(), testNss, MODE_X);

            auto db = collRaii.ensureDbExists(opCtx.get());
            invariant(db->createCollection(opCtx.get(), testNss, {}));
            wunit.commit();
        });

        usersNss = NamespaceString::makeTenantUsersCollection(boost::none);
        writeConflictRetry(opCtx.get(), "createColl", usersNss, [&] {
            shard_role_details::getRecoveryUnit(opCtx.get())
                ->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
            shard_role_details::getRecoveryUnit(opCtx.get())->abandonSnapshot();

            WriteUnitOfWork wunit(opCtx.get());
            AutoGetCollection collRaii(opCtx.get(), usersNss, MODE_X);

            auto db = collRaii.ensureDbExists(opCtx.get());
            invariant(db->createCollection(opCtx.get(), usersNss, {}));
            wunit.commit();
        });

        credentials = BSON("SCRAM-SHA-1"
                           << scram::Secrets<SHA1Block>::generateCredentials("password", 10000)
                           << "SCRAM-SHA-256"
                           << scram::Secrets<SHA256Block>::generateCredentials("password", 15000));

        userDocument = makeUserDocument("admin.v2read"_sd,
                                        "v2read",
                                        "test",
                                        credentials,
                                        BSON_ARRAY(BSON("role" << "read"
                                                               << "db"
                                                               << "test")));
    }

    void doInsert(const NamespaceString& nss,
                  const std::vector<BSONObj> insertDocs,
                  bool shouldInvalidateCache) {
        AuthOpObserver opObserver;
        auto opCtx = cc().makeOperationContext();
        mockRouter->resetCounts();

        std::vector<InsertStatement> stmts;
        std::transform(insertDocs.begin(),
                       insertDocs.end(),
                       std::back_inserter(stmts),
                       [](auto doc) { return InsertStatement(doc.getOwned()); });

        WriteUnitOfWork wuow(opCtx.get());
        AutoGetCollection coll(opCtx.get(), nss, MODE_IX);
        opObserver.onInserts(opCtx.get(),
                             *coll,
                             stmts.cbegin(),
                             stmts.cend(),
                             /*recordIds*/ {},
                             /*fromMigrate=*/std::vector<bool>(stmts.size(), false),
                             /*defaultFromMigrate=*/false);
        mockRouter->assertCounts(0, 0, 0);
        wuow.commit();

        // The cache should only invalidate after the WUOW commits if shouldInvalidateCache is true.
        if (shouldInvalidateCache) {
            mockRouter->assertCounts(0, 1, 0);
        } else {
            mockRouter->assertCounts(0, 0, 0);
        }
    }

    void doUpdate(const NamespaceString& nss,
                  BSONObj originalDoc,
                  BSONObj updatedDoc,
                  bool shouldInvalidateCache) {
        AuthOpObserver opObserver;
        auto opCtx = cc().makeOperationContext();
        mockRouter->resetCounts();

        const auto criteria = updatedDoc["_id"].wrap();
        const auto preImageDoc = originalDoc;
        CollectionUpdateArgs updateArgs{preImageDoc};
        updateArgs.criteria = criteria;
        updateArgs.update = BSON("$set" << updatedDoc);
        updateArgs.updatedDoc = updatedDoc;

        WriteUnitOfWork wuow(opCtx.get());
        AutoGetCollection autoColl(opCtx.get(), nss, MODE_IX);
        OplogUpdateEntryArgs entryArgs(&updateArgs, *autoColl);
        opObserver.onUpdate(opCtx.get(), entryArgs);

        mockRouter->assertCounts(0, 0, 0);
        wuow.commit();

        if (shouldInvalidateCache) {
            mockRouter->assertCounts(0, 1, 0);
        } else {
            mockRouter->assertCounts(0, 0, 0);
        }
    }

    void doDelete(const NamespaceString& nss, BSONObj deleteDoc, bool shouldInvalidateCache) {
        AuthOpObserver opObserver;
        auto opCtx = cc().makeOperationContext();
        mockRouter->resetCounts();

        // Deleting a user document should trigger cache invalidation after the WUOW commits.
        WriteUnitOfWork wuow(opCtx.get());
        AutoGetCollection coll(opCtx.get(), nss, MODE_IX);
        OplogDeleteEntryArgs args;

        const auto& deleteDocumentKey = getDocumentKey(*coll, deleteDoc);
        opObserver.onDelete(opCtx.get(), *coll, {}, deleteDoc, deleteDocumentKey, args);
        mockRouter->assertCounts(0, 0, 0);
        wuow.commit();

        if (shouldInvalidateCache) {
            mockRouter->assertCounts(0, 1, 0);
        } else {
            mockRouter->assertCounts(0, 0, 0);
        }
    }

    void doDropDatabase(const DatabaseName& dbname, bool shouldInvalidateCache) {
        AuthOpObserver opObserver;
        auto opCtx = cc().makeOperationContext();
        mockRouter->resetCounts();

        WriteUnitOfWork wuow(opCtx.get());
        opObserver.onDropDatabase(opCtx.get(), dbname, false /*fromMigrate*/);
        mockRouter->assertCounts(0, 0, 0);
        wuow.commit();

        if (shouldInvalidateCache) {
            mockRouter->assertCounts(1, 0, 0);
        } else {
            mockRouter->assertCounts(0, 0, 0);
        }
    }

    NamespaceString testNss;
    NamespaceString usersNss;
    AuthorizationManager* authzManager;
    AuthorizationRouterImplForTest* mockRouter;
    BSONObj userDocument;
    BSONObj credentials;

private:
    // Creates a reasonable set of ReplSettings for most tests.  We need to be able to
    // override this to create a larger oplog.
    virtual repl::ReplSettings createReplSettings() {
        repl::ReplSettings settings;
        settings.setOplogSizeBytes(5 * 1024 * 1024);
        settings.setReplSetString("mySet/node1:12345");
        return settings;
    }
};

TEST_F(AuthOpObserverTest, OnInsert) {
    // Test that cache invalidation occurs for inserts to usersNss but not testNss.
    std::vector<BSONObj> userDocs = {userDocument};
    doInsert(usersNss, userDocs, true);
    doInsert(testNss, userDocs, false);
}

TEST_F(AuthOpObserverTest, OnRollbackInvalidatesAuthCacheWhenAuthNamespaceRolledBack) {
    AuthOpObserver opObserver;
    auto opCtx = cc().makeOperationContext();
    auto initCacheGen = authzManager->getCacheGeneration();

    // Verify that the rollback op observer invalidates the user cache for each auth namespace by
    // checking that the cache generation changes after a call to the rollback observer method.
    OpObserver::RollbackObserverInfo rbInfo;
    rbInfo.rollbackNamespaces = {NamespaceString::kAdminRolesNamespace};
    opObserver.onReplicationRollback(opCtx.get(), rbInfo);
    ASSERT_NE(initCacheGen, authzManager->getCacheGeneration());

    initCacheGen = authzManager->getCacheGeneration();
    rbInfo.rollbackNamespaces = {NamespaceString::kAdminUsersNamespace};
    opObserver.onReplicationRollback(opCtx.get(), rbInfo);
    ASSERT_NE(initCacheGen, authzManager->getCacheGeneration());

    initCacheGen = authzManager->getCacheGeneration();
    rbInfo.rollbackNamespaces = {NamespaceString::kServerConfigurationNamespace};
    opObserver.onReplicationRollback(opCtx.get(), rbInfo);
    ASSERT_NE(initCacheGen, authzManager->getCacheGeneration());
}

TEST_F(AuthOpObserverTest, OnRollbackDoesntInvalidateAuthCacheWhenNoAuthNamespaceRolledBack) {
    AuthOpObserver opObserver;
    auto opCtx = cc().makeOperationContext();
    auto initCacheGen = authzManager->getCacheGeneration();

    // Verify that the rollback op observer doesn't invalidate the user cache.
    OpObserver::RollbackObserverInfo rbInfo;
    opObserver.onReplicationRollback(opCtx.get(), rbInfo);
    auto newCacheGen = authzManager->getCacheGeneration();
    ASSERT_EQ(newCacheGen, initCacheGen);
}

TEST_F(AuthOpObserverTest, OnUpdate) {
    // Updating a user document should trigger cache invalidation after the WUOW commits.
    BSONObj updatedUserDoc = makeUserDocument("admin.v2read"_sd,
                                              "v2read",
                                              "test",
                                              credentials,
                                              BSON_ARRAY(BSON("role" << "readwrite"
                                                                     << "db"
                                                                     << "test")));

    doUpdate(usersNss, userDocument, updatedUserDoc, true);

    // Updating a document on a test collection shouldn't trigger cache invalidation.
    doUpdate(testNss, userDocument, updatedUserDoc, false);
}

TEST_F(AuthOpObserverTest, OnDelete) {
    // Deleting a user document should trigger cache invalidation after the WUOW commits.
    auto userDoc = BSON("_id" << "admin.v2read");
    doDelete(usersNss, userDoc, true);

    // Deleting a random document in the test collection should not trigger cache invalidation.
    doDelete(testNss, userDoc, false);
}

TEST_F(AuthOpObserverTest, OnDropDatabase) {
    // Dropping the admin database should invalidate the entire user cache.
    doDropDatabase(DatabaseName::kAdmin, true);

    // Dropping the test database should not result in any user cache invalidation.
    doDropDatabase(DatabaseName::createDatabaseName_forTest(boost::none, "test"), false);
}

}  // namespace
}  // namespace mongo
