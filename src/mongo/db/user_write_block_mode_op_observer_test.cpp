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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/s/global_user_write_block_state.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/user_write_block_mode_op_observer.h"
#include "mongo/db/write_block_bypass.h"

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
    }

protected:
    // Ensure that inserts, updates, and deletes with the given opCtx on the given namespace will
    // succeed or fail depending on the value of shouldSucceed.
    void runCUD(OperationContext* opCtx, const NamespaceString& nss, bool shouldSucceed) {
        UserWriteBlockModeOpObserver opObserver;
        std::vector<InsertStatement> inserts;
        auto uuid = UUID::gen();
        OplogUpdateEntryArgs updateArgs(nullptr, nss, uuid);
        updateArgs.nss = nss;

        if (shouldSucceed) {
            try {
                opObserver.onInserts(opCtx, nss, uuid, inserts.begin(), inserts.end(), false);
                opObserver.onUpdate(opCtx, updateArgs);
                opObserver.onDelete(opCtx, nss, uuid, StmtId(), OplogDeleteEntryArgs{});
            } catch (...) {
                // Make it easier to see that this is where we failed.
                ASSERT_OK(exceptionToStatus());
            }
        } else {
            ASSERT_THROWS(
                opObserver.onInserts(opCtx, nss, uuid, inserts.begin(), inserts.end(), false),
                AssertionException);
            ASSERT_THROWS(opObserver.onUpdate(opCtx, updateArgs), AssertionException);
            ASSERT_THROWS(opObserver.onDelete(opCtx, nss, uuid, StmtId(), OplogDeleteEntryArgs{}),
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
    runCUD(opCtx.get(), NamespaceString("a.b"), true);
    runCUD(opCtx.get(), NamespaceString("admin"), true);
    runCUD(opCtx.get(), NamespaceString("local"), true);
    runCUD(opCtx.get(), NamespaceString("config"), true);
}

TEST_F(UserWriteBlockModeOpObserverTest, WriteBlockingDisabledWithBypass) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_X);

    // Disable blocking and enable bypass
    GlobalUserWriteBlockState::get(opCtx.get())->disableUserWriteBlocking(opCtx.get());
    auto authSession = AuthorizationSession::get(opCtx->getClient());
    authSession->grantInternalAuthorization(opCtx.get());
    ASSERT(authSession->mayBypassWriteBlockingMode());

    WriteBlockBypass::get(opCtx.get()).setFromMetadata(opCtx.get(), BSONElement());
    ASSERT(WriteBlockBypass::get(opCtx.get()).isWriteBlockBypassEnabled());

    // Ensure writes succeed
    runCUD(opCtx.get(), NamespaceString("a.b"), true);
    runCUD(opCtx.get(), NamespaceString("admin"), true);
    runCUD(opCtx.get(), NamespaceString("local"), true);
    runCUD(opCtx.get(), NamespaceString("config"), true);
}

TEST_F(UserWriteBlockModeOpObserverTest, WriteBlockingEnabledNoBypass) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_X);

    // Enable blocking and ensure bypass is disabled
    GlobalUserWriteBlockState::get(opCtx.get())->enableUserWriteBlocking(opCtx.get());
    ASSERT(!WriteBlockBypass::get(opCtx.get()).isWriteBlockBypassEnabled());

    // Ensure user writes now fail, while non-user writes still succeed
    runCUD(opCtx.get(), NamespaceString("a.b"), false);
    runCUD(opCtx.get(), NamespaceString("admin"), true);
    runCUD(opCtx.get(), NamespaceString("local"), true);
    runCUD(opCtx.get(), NamespaceString("config"), true);
}

TEST_F(UserWriteBlockModeOpObserverTest, WriteBlockingEnabledWithBypass) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_X);

    // Enable blocking and enable bypass
    GlobalUserWriteBlockState::get(opCtx.get())->enableUserWriteBlocking(opCtx.get());
    auto authSession = AuthorizationSession::get(opCtx->getClient());
    authSession->grantInternalAuthorization(opCtx.get());
    ASSERT(authSession->mayBypassWriteBlockingMode());

    WriteBlockBypass::get(opCtx.get()).setFromMetadata(opCtx.get(), BSONElement());
    ASSERT(WriteBlockBypass::get(opCtx.get()).isWriteBlockBypassEnabled());

    // Ensure user writes still fail under these conditions
    // TODO SERVER-63456 After implementing bypass into OpObservers, this expectation will change to
    // success.
    runCUD(opCtx.get(), NamespaceString("a.b"), false);
    runCUD(opCtx.get(), NamespaceString("admin"), true);
    runCUD(opCtx.get(), NamespaceString("local"), true);
    runCUD(opCtx.get(), NamespaceString("config"), true);
}

}  // namespace
}  // namespace mongo
