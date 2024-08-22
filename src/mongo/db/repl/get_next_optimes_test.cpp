/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"

namespace mongo {
namespace {
class GetNextOpTimesTest : public ServiceContextMongoDTest {
protected:
    explicit GetNextOpTimesTest(Options options = {})
        : ServiceContextMongoDTest(std::move(options)) {}

    void setUp() override {
        // Set up mongod.
        ServiceContextMongoDTest::setUp();

        auto service = getServiceContext();
        _opCtx = cc().makeOperationContext();
        // onStepUp() relies on the storage interface to create the config.transactions table.
        // repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());

        // Set up ReplicationCoordinator and create oplog.
        repl::ReplicationCoordinator::set(
            service,
            std::make_unique<repl::ReplicationCoordinatorMock>(service, repl::ReplSettings()));
        repl::createOplog(_opCtx.get());

        // Ensure that we are primary.
        auto replCoord = repl::ReplicationCoordinator::get(_opCtx.get());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    }

    void tearDown() override {
        serverGlobalParams.clusterRole = ClusterRole::None;
    }

protected:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(GetNextOpTimesTest, CantBlockWhileSlotOpen) {
    auto ru = shard_role_details::getRecoveryUnit(_opCtx.get());
    auto locker = shard_role_details::getLocker(_opCtx.get());

    ASSERT(ru->getBlockingAllowed());
    ASSERT_FALSE(locker->getAssertOnLockAttempt());

    {
        WriteUnitOfWork wuow{_opCtx.get()};

        auto slot1 = LocalOplogInfo::get(_opCtx.get())->getNextOpTimes(_opCtx.get(), 1);

        ASSERT_FALSE(ru->getBlockingAllowed());
        ASSERT(locker->getAssertOnLockAttempt());
        auto slot2 = LocalOplogInfo::get(_opCtx.get())->getNextOpTimes(_opCtx.get(), 1);

        ASSERT_FALSE(ru->getBlockingAllowed());
        ASSERT(locker->getAssertOnLockAttempt());
        // roll-back
    }

    ASSERT(ru->getBlockingAllowed());
    ASSERT_FALSE(locker->getAssertOnLockAttempt());

    {
        WriteUnitOfWork wuow{_opCtx.get()};

        auto slot1 = LocalOplogInfo::get(_opCtx.get())->getNextOpTimes(_opCtx.get(), 1);

        ASSERT_FALSE(ru->getBlockingAllowed());
        ASSERT(locker->getAssertOnLockAttempt());
        auto slot2 = LocalOplogInfo::get(_opCtx.get())->getNextOpTimes(_opCtx.get(), 1);

        ASSERT_FALSE(ru->getBlockingAllowed());
        ASSERT(locker->getAssertOnLockAttempt());
        wuow.commit();
    }

    ASSERT(ru->getBlockingAllowed());
    ASSERT_FALSE(locker->getAssertOnLockAttempt());
}

}  // namespace
}  // namespace mongo
