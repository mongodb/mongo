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
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/time_support.h"


namespace mongo {
namespace {
class GetNextOpTimesTest : public ServiceContextMongoDTest {
protected:
    explicit GetNextOpTimesTest(Options options = {})
        : ServiceContextMongoDTest(std::move(options)) {}

    void setUp() override {
        // Set up mongod.
        ServiceContextMongoDTest::setUp();
        _tickSource.setAdvanceOnRead(Milliseconds{1});
        auto service = getServiceContext();
        _opCtx = cc().makeOperationContext();
        LocalOplogInfo::getOplogSlotTimeContext(_opCtx.get()).setTickSource(&_tickSource);

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
    TickSourceMock<Milliseconds> _tickSource;
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(GetNextOpTimesTest, CantBlockWhileSlotOpen) {
    auto ru = shard_role_details::getRecoveryUnit(_opCtx.get());
    auto locker = shard_role_details::getLocker(_opCtx.get());

    ASSERT(ru->getBlockingAllowed());
    ASSERT_FALSE(locker->getAssertOnLockAttempt());
    const auto& slotTimeCtx = LocalOplogInfo::getOplogSlotTimeContext(_opCtx.get());

    {
        WriteUnitOfWork wuow{_opCtx.get()};
        ASSERT_EQUALS(0, slotTimeCtx.getBatchCount());

        auto slot1 = LocalOplogInfo::get(_opCtx.get())->getNextOpTimes(_opCtx.get(), 1);

        ASSERT_EQUALS(1, slotTimeCtx.getBatchCount());
        ASSERT_FALSE(ru->getBlockingAllowed());
        ASSERT(locker->getAssertOnLockAttempt());
        auto slot2 = LocalOplogInfo::get(_opCtx.get())->getNextOpTimes(_opCtx.get(), 1);

        ASSERT_EQUALS(2, slotTimeCtx.getBatchCount());
        ASSERT_FALSE(ru->getBlockingAllowed());
        ASSERT(locker->getAssertOnLockAttempt());
        // roll-back
    }
    ASSERT_EQUALS(0, slotTimeCtx.getBatchCount());
    // Expecting timer to have done 2 ticks by now: one from reset on first batch and one on
    // duration update on the rollback
    ASSERT_EQUALS(Milliseconds{2}, slotTimeCtx.getTimer().elapsed());
    ASSERT_EQUALS(Milliseconds{1}, slotTimeCtx.getTotalMicros());

    ASSERT(ru->getBlockingAllowed());
    ASSERT_FALSE(locker->getAssertOnLockAttempt());

    {
        WriteUnitOfWork wuow{_opCtx.get()};
        ASSERT_EQUALS(0, slotTimeCtx.getBatchCount());
        // Timer must be still ticking (one more tick from previous read)
        ASSERT_EQUALS(Milliseconds{3}, slotTimeCtx.getTimer().elapsed());
        auto slot1 = LocalOplogInfo::get(_opCtx.get())->getNextOpTimes(_opCtx.get(), 1);

        // Make sure we reset the timer for new transaction (one tick from reset)
        ASSERT_EQUALS(Milliseconds{1}, slotTimeCtx.getTimer().elapsed());
        ASSERT_EQUALS(1, slotTimeCtx.getBatchCount());
        ASSERT_FALSE(ru->getBlockingAllowed());
        ASSERT(locker->getAssertOnLockAttempt());
        auto slot2 = LocalOplogInfo::get(_opCtx.get())->getNextOpTimes(_opCtx.get(), 1);

        ASSERT_EQUALS(2, slotTimeCtx.getBatchCount());
        ASSERT_FALSE(ru->getBlockingAllowed());
        ASSERT(locker->getAssertOnLockAttempt());
        wuow.commit();
    }
    // One tick from reset, one tick from previous test read, one tick from commit duration update
    ASSERT_EQUALS(Milliseconds{3}, slotTimeCtx.getTimer().elapsed());
    // 1+2=3 ticks: 2 transactions one tick for first one, two ticks for second one(extra tick
    // because of test read after reset)
    ASSERT_EQUALS(Milliseconds{3}, slotTimeCtx.getTotalMicros());
    ASSERT_EQUALS(0, slotTimeCtx.getBatchCount());
    ASSERT(ru->getBlockingAllowed());
    ASSERT_FALSE(locker->getAssertOnLockAttempt());
}

}  // namespace
}  // namespace mongo
