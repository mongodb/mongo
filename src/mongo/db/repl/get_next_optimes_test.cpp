// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/util/tick_source_mock.h"


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
