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

#include "mongo/db/user_write_block/user_write_block_mode_op_observer.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/collection_operation_source.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/ddl/create_gen.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/user_write_block/global_user_write_block_state.h"
#include "mongo/db/user_write_block/write_block_bypass.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {

class UserWriteBlockModeOpObserverTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        // Set up mongod.
        ServiceContextMongoDTest::setUp();

        auto service = getServiceContext();
        auto opCtx = cc().makeOperationContext();
        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceMock>());

        // Set up ReplicationCoordinator and create oplog.
        repl::ReplicationCoordinator::set(
            service,
            std::make_unique<repl::ReplicationCoordinatorMock>(service, createReplSettings()));
        repl::createOplog(opCtx.get());

        // Ensure that we are primary.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx.get());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

        ASSERT_OK(createCollection(
            opCtx.get(),
            CreateCommand(NamespaceString::createNamespaceString_forTest("userDB.coll"))));
        ASSERT_OK(createCollection(opCtx.get(),
                                   CreateCommand(NamespaceString::createNamespaceString_forTest(
                                       "userDB.system.profile"))));
        ASSERT_OK(createCollection(
            opCtx.get(),
            CreateCommand(NamespaceString::createNamespaceString_forTest("admin.coll"))));
        ASSERT_OK(createCollection(
            opCtx.get(),
            CreateCommand(NamespaceString::createNamespaceString_forTest("admin.collForRename"))));
        ASSERT_OK(createCollection(
            opCtx.get(),
            CreateCommand(NamespaceString::createNamespaceString_forTest("local.coll"))));
        ASSERT_OK(createCollection(
            opCtx.get(),
            CreateCommand(NamespaceString::createNamespaceString_forTest("config.coll"))));
    }

protected:
    // Ensure that CUD ops with the given opCtx on the given namespace will succeed or fail
    // depending on the value of shouldSucceed.
    void runCUD(OperationContext* opCtx,
                const NamespaceString& nss,
                bool shouldSucceed,
                bool fromMigrate) {
        ASSERT(nss.isValid());

        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        if (!autoColl)
            FAIL(std::string(str::stream()
                             << "Collection " << nss.toStringForErrorMsg() << " doesn't exist"));

        UserWriteBlockModeOpObserver opObserver;
        std::vector<InsertStatement> inserts;
        const auto criteria = BSON("_id" << 0);
        const auto preImageDoc = criteria;
        const auto& documentKey = getDocumentKey(*autoColl, preImageDoc);
        CollectionUpdateArgs collectionUpdateArgs{preImageDoc};
        collectionUpdateArgs.criteria = criteria;
        collectionUpdateArgs.source =
            fromMigrate ? OperationSource::kFromMigrate : OperationSource::kStandard;
        OplogUpdateEntryArgs updateArgs(&collectionUpdateArgs, *autoColl);
        OplogDeleteEntryArgs deleteArgs;
        deleteArgs.fromMigrate = fromMigrate;
        if (shouldSucceed) {
            try {
                opObserver.onInserts(opCtx,
                                     *autoColl,
                                     inserts.begin(),
                                     inserts.end(),
                                     /*recordIds*/ {},
                                     /*fromMigrate=*/std::vector<bool>(inserts.size(), fromMigrate),
                                     /*defaultFromMigrate=*/fromMigrate);
                opObserver.onUpdate(opCtx, updateArgs);
                opObserver.onDelete(
                    opCtx, *autoColl, StmtId(), preImageDoc, documentKey, deleteArgs);
            } catch (...) {
                // Make it easier to see that this is where we failed.
                ASSERT_OK(exceptionToStatus());
            }
        } else {
            ASSERT_THROWS(
                opObserver.onInserts(opCtx,
                                     *autoColl,
                                     inserts.begin(),
                                     inserts.end(),
                                     /*recordIds*/ {},
                                     /*fromMigrate=*/std::vector<bool>(inserts.size(), fromMigrate),
                                     /*defaultFromMigrate=*/fromMigrate),
                AssertionException);
            ASSERT_THROWS(opObserver.onUpdate(opCtx, updateArgs), AssertionException);
            ASSERT_THROWS(opObserver.onDelete(
                              opCtx, *autoColl, StmtId(), preImageDoc, documentKey, deleteArgs),
                          AssertionException);
        }
    }

    // Ensure that all checked ops with the given opCtx on the given namespace will
    // succeed or fail depending on the value of shouldSucceed.
    void runCheckedOps(OperationContext* opCtx,
                       const NamespaceString& nss,
                       bool shouldSucceed,
                       bool fromMigrate = false) {
        runCUD(opCtx, nss, shouldSucceed, fromMigrate);
        UserWriteBlockModeOpObserver opObserver;
        auto uuid = UUID::gen();
        NamespaceString adminNss =
            NamespaceString::createNamespaceString_forTest("admin.collForRename");

        if (shouldSucceed) {
            try {
                opObserver.onCreateIndex(
                    opCtx, nss, uuid, IndexBuildInfo{BSONObj(), boost::none}, false, false);
                opObserver.onStartIndexBuild(opCtx, nss, uuid, uuid, {}, false, false);
                opObserver.onStartIndexBuildSinglePhase(opCtx, nss);
                opObserver.onCreateCollection(
                    opCtx, nss, {}, BSONObj(), OplogSlot(), boost::none, false, false);
                opObserver.onCollMod(opCtx, nss, uuid, BSONObj(), {}, boost::none, false);
                opObserver.onDropDatabase(opCtx, nss.dbName(), false /*fromMigrate*/);
                opObserver.onDropCollection(opCtx,
                                            nss,
                                            uuid,
                                            0,
                                            /*markFromMigrate=*/false,
                                            /*isTimeseries*/ false);
                opObserver.onDropIndex(opCtx, nss, uuid, "", BSONObj(), false);
                // For renames, make sure we check both from and to for the given namespace
                opObserver.preRenameCollection(opCtx,
                                               nss,
                                               adminNss,
                                               uuid,
                                               boost::none,
                                               0,
                                               /*stayTemp=*/false,
                                               /*markFromMigrate=*/false,
                                               /*isTimeseries*/ false);
                opObserver.preRenameCollection(opCtx,
                                               adminNss,
                                               nss,
                                               uuid,
                                               boost::none,
                                               0,
                                               /*stayTemp=*/false,
                                               /*markFromMigrate=*/false,
                                               /*isTimeseries*/ false);
                opObserver.onRenameCollection(opCtx,
                                              nss,
                                              adminNss,
                                              uuid,
                                              boost::none,
                                              0,
                                              /*stayTemp=*/false,
                                              /*markFromMigrate=*/false,
                                              /*isTimeseries*/ false);
                opObserver.onRenameCollection(opCtx,
                                              adminNss,
                                              nss,
                                              uuid,
                                              boost::none,
                                              0,
                                              /*stayTemp=*/false,
                                              /*markFromMigrate=*/false,
                                              /*isTimeseries*/ false);
                opObserver.onImportCollection(
                    opCtx, uuid, nss, 0, 0, BSONObj(), BSONObj(), false, false);
            } catch (...) {
                // Make it easier to see that this is where we failed.
                ASSERT_OK(exceptionToStatus());
            }
        } else {
            ASSERT_THROWS(
                opObserver.onCreateIndex(
                    opCtx, nss, uuid, IndexBuildInfo{BSONObj(), boost::none}, false, false),
                AssertionException);
            ASSERT_THROWS(opObserver.onStartIndexBuild(opCtx, nss, uuid, uuid, {}, false, false),
                          AssertionException);
            ASSERT_THROWS(opObserver.onStartIndexBuildSinglePhase(opCtx, nss), AssertionException);
            ASSERT_THROWS(opObserver.onCreateCollection(
                              opCtx, nss, {}, BSONObj(), OplogSlot(), boost::none, false, false),
                          AssertionException);
            ASSERT_THROWS(opObserver.onCollMod(opCtx, nss, uuid, BSONObj(), {}, boost::none, false),
                          AssertionException);
            ASSERT_THROWS(opObserver.onDropDatabase(opCtx, nss.dbName(), false /*fromMigrate*/),
                          AssertionException);
            ASSERT_THROWS(opObserver.onDropCollection(opCtx,
                                                      nss,
                                                      uuid,
                                                      0,
                                                      /*markFromMigrate=*/false,
                                                      /*isTimeseries*/ false),
                          AssertionException);
            ASSERT_THROWS(opObserver.onDropIndex(opCtx, nss, uuid, "", BSONObj(), false),
                          AssertionException);
            ASSERT_THROWS(opObserver.preRenameCollection(opCtx,
                                                         nss,
                                                         adminNss,
                                                         uuid,
                                                         boost::none,
                                                         0,
                                                         /*stayTemp=*/false,
                                                         /*markFromMigrate=*/false,
                                                         /*isTimeseries*/ false),
                          AssertionException);
            ASSERT_THROWS(opObserver.preRenameCollection(opCtx,
                                                         adminNss,
                                                         nss,
                                                         uuid,
                                                         boost::none,
                                                         0,
                                                         /*stayTemp=*/false,
                                                         /*markFromMigrate=*/false,
                                                         /*isTimeseries*/ false),
                          AssertionException);
            ASSERT_THROWS(opObserver.onRenameCollection(opCtx,
                                                        nss,
                                                        adminNss,
                                                        uuid,
                                                        boost::none,
                                                        0,
                                                        /*stayTemp=*/false,
                                                        /*markFromMigrate=*/false,
                                                        /*isTimeseries*/ false),
                          AssertionException);
            ASSERT_THROWS(opObserver.onRenameCollection(opCtx,
                                                        adminNss,
                                                        nss,
                                                        uuid,
                                                        boost::none,
                                                        0,
                                                        /*stayTemp=*/false,
                                                        /*markFromMigrate=*/false,
                                                        /*isTimeseries*/ false),
                          AssertionException);
            ASSERT_THROWS(opObserver.onImportCollection(
                              opCtx, uuid, nss, 0, 0, BSONObj(), BSONObj(), false, false),
                          AssertionException);
        }
    }

private:
    // Creates a reasonable set of ReplSettings for most tests.
    repl::ReplSettings createReplSettings() {
        repl::ReplSettings settings;
        settings.setOplogSizeBytes(5 * 1024 * 1024);
        settings.setReplSetString("mySet/node1:12345");
        return settings;
    }
};

TEST_F(UserWriteBlockModeOpObserverTest, WriteBlockingDisabledNoBypass) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_X);

    // Disable blocking and ensure bypass is disabled
    GlobalUserWriteBlockState::get(opCtx.get())->disableUserWriteBlocking(opCtx.get());
    ASSERT(!WriteBlockBypass::get(opCtx.get()).isWriteBlockBypassEnabled());

    // Ensure writes succeed
    runCheckedOps(opCtx.get(), NamespaceString::createNamespaceString_forTest("userDB.coll"), true);
    runCheckedOps(opCtx.get(), NamespaceString::createNamespaceString_forTest("admin.coll"), true);
    runCheckedOps(opCtx.get(), NamespaceString::createNamespaceString_forTest("local.coll"), true);
    runCheckedOps(opCtx.get(), NamespaceString::createNamespaceString_forTest("config.coll"), true);
}

TEST_F(UserWriteBlockModeOpObserverTest, WriteBlockingDisabledWithBypass) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_X);

    // Disable blocking and enable bypass
    GlobalUserWriteBlockState::get(opCtx.get())->disableUserWriteBlocking(opCtx.get());
    auto authSession = AuthorizationSession::get(opCtx->getClient());
    authSession->grantInternalAuthorization();
    ASSERT(authSession->mayBypassWriteBlockingMode());

    WriteBlockBypass::get(opCtx.get()).setFromMetadata(opCtx.get(), {});
    ASSERT(WriteBlockBypass::get(opCtx.get()).isWriteBlockBypassEnabled());

    // Ensure writes succeed
    runCheckedOps(opCtx.get(), NamespaceString::createNamespaceString_forTest("userDB.coll"), true);
    runCheckedOps(opCtx.get(), NamespaceString::createNamespaceString_forTest("admin.coll"), true);
    runCheckedOps(opCtx.get(), NamespaceString::createNamespaceString_forTest("local.coll"), true);
    runCheckedOps(opCtx.get(), NamespaceString::createNamespaceString_forTest("config.coll"), true);
}

TEST_F(UserWriteBlockModeOpObserverTest, WriteBlockingEnabledNoBypass) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_X);

    // Enable blocking and ensure bypass is disabled
    GlobalUserWriteBlockState::get(opCtx.get())
        ->enableUserWriteBlocking(opCtx.get(), UserWritesBlockReasonEnum::kUnspecified);
    ASSERT(!WriteBlockBypass::get(opCtx.get()).isWriteBlockBypassEnabled());

    // Ensure user writes now fail, while non-user writes still succeed
    runCheckedOps(
        opCtx.get(), NamespaceString::createNamespaceString_forTest("userDB.coll"), false);
    runCheckedOps(opCtx.get(), NamespaceString::createNamespaceString_forTest("admin.coll"), true);
    runCheckedOps(opCtx.get(), NamespaceString::createNamespaceString_forTest("local.coll"), true);
    runCheckedOps(opCtx.get(), NamespaceString::createNamespaceString_forTest("config.coll"), true);

    // Ensure that CUD ops from migrations succeed
    runCUD(opCtx.get(),
           NamespaceString::createNamespaceString_forTest("userDB.coll"),
           true,
           true /* fromMigrate */);

    // Ensure that writes to the <db>.system.profile collections are always allowed
    runCUD(opCtx.get(),
           NamespaceString::createNamespaceString_forTest("userDB.system.profile"),
           true /* shouldSucceed */,
           false /* fromMigrate */);
}

TEST_F(UserWriteBlockModeOpObserverTest, WriteBlockingEnabledWithBypass) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_X);

    // Enable blocking and enable bypass
    GlobalUserWriteBlockState::get(opCtx.get())
        ->enableUserWriteBlocking(opCtx.get(), UserWritesBlockReasonEnum::kUnspecified);
    auto authSession = AuthorizationSession::get(opCtx->getClient());
    authSession->grantInternalAuthorization();
    ASSERT(authSession->mayBypassWriteBlockingMode());

    WriteBlockBypass::get(opCtx.get()).setFromMetadata(opCtx.get(), {});
    ASSERT(WriteBlockBypass::get(opCtx.get()).isWriteBlockBypassEnabled());

    // Ensure user writes succeed

    runCheckedOps(opCtx.get(), NamespaceString::createNamespaceString_forTest("userDB.coll"), true);
    runCheckedOps(opCtx.get(), NamespaceString::createNamespaceString_forTest("admin.coll"), true);
    runCheckedOps(opCtx.get(), NamespaceString::createNamespaceString_forTest("local.coll"), true);
    runCheckedOps(opCtx.get(), NamespaceString::createNamespaceString_forTest("config.coll"), true);
}

}  // namespace
}  // namespace mongo
