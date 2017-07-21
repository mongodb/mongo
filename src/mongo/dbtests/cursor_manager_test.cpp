/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpekTestNssL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpekTestNssL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <algorithm>

#include <boost/optional/optional.hpp>
#include <boost/optional/optional_io.hpp>

#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {
const NamespaceString kTestNss{"test.collection"};

TEST(CursorManagerTest, IsGloballyManagedCursorShouldReturnFalseIfLeadingBitsAreZeroes) {
    ASSERT_FALSE(CursorManager::isGloballyManagedCursor(0x0000000000000000));
    ASSERT_FALSE(CursorManager::isGloballyManagedCursor(0x000000000FFFFFFF));
    ASSERT_FALSE(CursorManager::isGloballyManagedCursor(0x000000007FFFFFFF));
    ASSERT_FALSE(CursorManager::isGloballyManagedCursor(0x0FFFFFFFFFFFFFFF));
    ASSERT_FALSE(CursorManager::isGloballyManagedCursor(0x3FFFFFFFFFFFFFFF));
    ASSERT_FALSE(CursorManager::isGloballyManagedCursor(0x3dedbeefdeadbeef));
}

TEST(CursorManagerTest, IsGloballyManagedCursorShouldReturnTrueIfLeadingBitsAreZeroAndOne) {
    ASSERT_TRUE(CursorManager::isGloballyManagedCursor(0x4FFFFFFFFFFFFFFF));
    ASSERT_TRUE(CursorManager::isGloballyManagedCursor(0x5FFFFFFFFFFFFFFF));
    ASSERT_TRUE(CursorManager::isGloballyManagedCursor(0x6FFFFFFFFFFFFFFF));
    ASSERT_TRUE(CursorManager::isGloballyManagedCursor(0x7FFFFFFFFFFFFFFF));
    ASSERT_TRUE(CursorManager::isGloballyManagedCursor(0x4000000000000000));
    ASSERT_TRUE(CursorManager::isGloballyManagedCursor(0x4dedbeefdeadbeef));
}

TEST(CursorManagerTest, IsGloballyManagedCursorShouldReturnFalseIfLeadingBitIsAOne) {
    ASSERT_FALSE(CursorManager::isGloballyManagedCursor(~0LL));
    ASSERT_FALSE(CursorManager::isGloballyManagedCursor(0xFFFFFFFFFFFFFFFF));
    ASSERT_FALSE(CursorManager::isGloballyManagedCursor(0x8FFFFFFFFFFFFFFF));
    ASSERT_FALSE(CursorManager::isGloballyManagedCursor(0x8dedbeefdeadbeef));
}

class CursorManagerTest : public unittest::Test {
public:
    CursorManagerTest()
        : _queryServiceContext(stdx::make_unique<QueryTestServiceContext>()),
          _opCtx(_queryServiceContext->makeOperationContext()) {
        _opCtx->getServiceContext()->setPreciseClockSource(stdx::make_unique<ClockSourceMock>());
        _clock =
            static_cast<ClockSourceMock*>(_opCtx->getServiceContext()->getPreciseClockSource());
    }

    virtual ~CursorManagerTest() {
        _cursorManager.invalidateAll(_opCtx.get(), true, "end of test");
    }

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeFakePlanExecutor() {
        return makeFakePlanExecutor(_opCtx.get());
    }

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeFakePlanExecutor(
        OperationContext* opCtx) {
        auto workingSet = stdx::make_unique<WorkingSet>();
        auto queuedDataStage = stdx::make_unique<QueuedDataStage>(opCtx, workingSet.get());
        return unittest::assertGet(PlanExecutor::make(opCtx,
                                                      std::move(workingSet),
                                                      std::move(queuedDataStage),
                                                      kTestNss,
                                                      PlanExecutor::YieldPolicy::NO_YIELD));
    }

    ClientCursorParams makeParams(OperationContext* opCtx) {
        return {makeFakePlanExecutor(opCtx), kTestNss, {}, false, BSONObj()};
    }

    ClientCursorPin makeCursor(OperationContext* opCtx) {
        return _cursorManager.registerCursor(opCtx, makeParams(opCtx));
    }

    ClockSourceMock* useClock() {
        return _clock;
    }

    CursorManager* useCursorManager() {
        return &_cursorManager;
    }

protected:
    std::unique_ptr<QueryTestServiceContext> _queryServiceContext;
    ServiceContext::UniqueOperationContext _opCtx;

private:
    ClockSourceMock* _clock;
    CursorManager _cursorManager{kTestNss};
};

class CursorManagerTestCustomOpCtx : public CursorManagerTest {
    void setUp() override {
        _queryServiceContext->getClient()->resetOperationContext();
    }

    void tearDown() override {
        _queryServiceContext->getClient()->setOperationContext(_opCtx.get());
    }
};

TEST_F(CursorManagerTest, GlobalCursorManagerShouldReportOwnershipOfCursorsItCreated) {
    for (int i = 0; i < 1000; i++) {
        auto cursorPin = CursorManager::getGlobalCursorManager()->registerCursor(
            _opCtx.get(),
            {makeFakePlanExecutor(), NamespaceString{"test.collection"}, {}, false, BSONObj()});
        ASSERT_TRUE(CursorManager::isGloballyManagedCursor(cursorPin.getCursor()->cursorid()));
    }
}

TEST_F(CursorManagerTest,
       CursorsFromCollectionCursorManagerShouldNotReportBeingManagedByGlobalCursorManager) {
    CursorManager* cursorManager = useCursorManager();
    auto opCtx = cc().makeOperationContext();
    for (int i = 0; i < 1000; i++) {
        auto cursorPin = cursorManager->registerCursor(
            _opCtx.get(), {makeFakePlanExecutor(), kTestNss, {}, false, BSONObj()});
        ASSERT_FALSE(CursorManager::isGloballyManagedCursor(cursorPin.getCursor()->cursorid()));
    }
}

uint32_t extractLeading32Bits(CursorId cursorId) {
    return static_cast<uint32_t>((cursorId & 0xFFFFFFFF00000000) >> 32);
}

TEST_F(CursorManagerTest,
       AllCursorsFromCollectionCursorManagerShouldContainIdentical32BitPrefixes) {
    CursorManager* cursorManager = useCursorManager();
    boost::optional<uint32_t> prefix;
    for (int i = 0; i < 1000; i++) {
        auto cursorPin = cursorManager->registerCursor(
            _opCtx.get(), {makeFakePlanExecutor(), kTestNss, {}, false, BSONObj()});
        auto cursorId = cursorPin.getCursor()->cursorid();
        if (prefix) {
            ASSERT_EQ(*prefix, extractLeading32Bits(cursorId));
        } else {
            prefix = extractLeading32Bits(cursorId);
        }
    }
}

/**
 * Tests that invalidating a cursor without dropping the collection while the cursor is not in use
 * will keep the cursor registered. After being invalidated, pinning the cursor should take
 * ownership of the cursor and calling getNext() on its PlanExecutor should return an error
 * including the error message.
 */
TEST_F(CursorManagerTest, InvalidateCursor) {
    CursorManager* cursorManager = useCursorManager();
    auto cursorPin = cursorManager->registerCursor(
        _opCtx.get(), {makeFakePlanExecutor(), kTestNss, {}, false, BSONObj()});

    auto cursorId = cursorPin.getCursor()->cursorid();
    cursorPin.release();

    ASSERT_EQUALS(1U, cursorManager->numCursors());
    auto invalidateReason = "Invalidate Test";
    const bool collectionGoingAway = false;
    cursorManager->invalidateAll(_opCtx.get(), collectionGoingAway, invalidateReason);
    // Since the collection is not going away, the cursor should remain open, but be killed.
    ASSERT_EQUALS(1U, cursorManager->numCursors());

    // Pinning a killed cursor should result in an error and clean up the cursor.
    ASSERT_EQ(ErrorCodes::QueryPlanKilled,
              cursorManager->pinCursor(_opCtx.get(), cursorId).getStatus());
    ASSERT_EQUALS(0U, cursorManager->numCursors());
}

/**
 * Tests that invalidating a cursor and dropping the collection while the cursor is not in use will
 * not keep the cursor registered.
 */
TEST_F(CursorManagerTest, InvalidateCursorWithDrop) {
    CursorManager* cursorManager = useCursorManager();

    auto cursorPin = cursorManager->registerCursor(
        _opCtx.get(), {makeFakePlanExecutor(), kTestNss, {}, false, BSONObj()});

    auto cursorId = cursorPin.getCursor()->cursorid();
    cursorPin.release();

    ASSERT_EQUALS(1U, cursorManager->numCursors());
    auto invalidateReason = "Invalidate Test";
    const bool collectionGoingAway = true;
    cursorManager->invalidateAll(_opCtx.get(), collectionGoingAway, invalidateReason);
    // Since the collection is going away, the cursor should not remain open.
    ASSERT_EQ(ErrorCodes::CursorNotFound,
              cursorManager->pinCursor(_opCtx.get(), cursorId).getStatus());
    ASSERT_EQUALS(0U, cursorManager->numCursors());
}

/**
 * Tests that invalidating a cursor while it is in use will deregister it from the cursor manager,
 * transferring ownership to the pinned cursor.
 */
TEST_F(CursorManagerTest, InvalidatePinnedCursor) {
    CursorManager* cursorManager = useCursorManager();

    auto cursorPin = cursorManager->registerCursor(
        _opCtx.get(), {makeFakePlanExecutor(), kTestNss, {}, false, BSONObj()});

    // If the cursor is pinned, it sticks around, even after invalidation.
    ASSERT_EQUALS(1U, cursorManager->numCursors());
    const std::string invalidateReason("InvalidatePinned Test");
    cursorManager->invalidateAll(_opCtx.get(), false, invalidateReason);
    ASSERT_EQUALS(0U, cursorManager->numCursors());

    // The invalidation should have killed the plan executor.
    BSONObj objOut;
    ASSERT_EQUALS(PlanExecutor::DEAD, cursorPin.getCursor()->getExecutor()->getNext(&objOut, NULL));
    ASSERT(WorkingSetCommon::isValidStatusMemberObject(objOut));
    const Status status = WorkingSetCommon::getMemberObjectStatus(objOut);
    ASSERT(status.reason().find(invalidateReason) != std::string::npos);

    cursorPin.release();
    ASSERT_EQUALS(0U, cursorManager->numCursors());
}

/**
 * Test that an attempt to kill a pinned cursor fails and produces an appropriate assertion.
 */
TEST_F(CursorManagerTest, ShouldNotBeAbleToKillPinnedCursor) {
    CursorManager* cursorManager = useCursorManager();

    auto cursorPin = cursorManager->registerCursor(
        _opCtx.get(), {makeFakePlanExecutor(), kTestNss, {}, false, BSONObj()});

    auto cursorId = cursorPin.getCursor()->cursorid();

    const bool shouldAudit = false;
    ASSERT_EQ(cursorManager->eraseCursor(_opCtx.get(), cursorId, shouldAudit),
              ErrorCodes::OperationFailed);
}

/**
 * Test that client cursors time out and get deleted.
 */
TEST_F(CursorManagerTest, InactiveCursorShouldTimeout) {
    CursorManager* cursorManager = useCursorManager();
    auto clock = useClock();

    cursorManager->registerCursor(
        _opCtx.get(),
        {makeFakePlanExecutor(), NamespaceString{"test.collection"}, {}, false, BSONObj()});

    ASSERT_EQ(0UL, cursorManager->timeoutCursors(_opCtx.get(), Date_t()));

    clock->advance(Milliseconds(CursorManager::kDefaultCursorTimeoutMinutes));
    ASSERT_EQ(1UL, cursorManager->timeoutCursors(_opCtx.get(), clock->now()));
    ASSERT_EQ(0UL, cursorManager->numCursors());

    cursorManager->registerCursor(
        _opCtx.get(),
        {makeFakePlanExecutor(), NamespaceString{"test.collection"}, {}, false, BSONObj()});
    ASSERT_EQ(1UL, cursorManager->timeoutCursors(_opCtx.get(), Date_t::max()));
    ASSERT_EQ(0UL, cursorManager->numCursors());
}

/**
 * Test that pinned cursors do not get timed out.
 */
TEST_F(CursorManagerTest, InactivePinnedCursorShouldNotTimeout) {
    CursorManager* cursorManager = useCursorManager();
    auto clock = useClock();

    auto cursorPin = cursorManager->registerCursor(
        _opCtx.get(),
        {makeFakePlanExecutor(), NamespaceString{"test.collection"}, {}, false, BSONObj()});

    // The pin is still in scope, so it should not time out.
    clock->advance(Milliseconds(CursorManager::kDefaultCursorTimeoutMinutes));
    ASSERT_EQ(0UL, cursorManager->timeoutCursors(_opCtx.get(), clock->now()));
}

/**
 * Test that client cursors which have been marked as killed time out and get deleted.
 */
TEST_F(CursorManagerTest, InactiveKilledCursorsShouldTimeout) {
    CursorManager* cursorManager = useCursorManager();
    auto clock = useClock();

    // Make a cursor from the plan executor, and immediately kill it.
    auto cursorPin = cursorManager->registerCursor(
        _opCtx.get(),
        {makeFakePlanExecutor(), NamespaceString{"test.collection"}, {}, false, BSONObj()});
    cursorPin.release();
    const bool collectionGoingAway = false;
    cursorManager->invalidateAll(
        _opCtx.get(), collectionGoingAway, "KilledCursorsShouldTimeoutTest");

    // Advance the clock to simulate time passing.
    clock->advance(Milliseconds(CursorManager::kDefaultCursorTimeoutMinutes));

    ASSERT_EQ(1UL, cursorManager->timeoutCursors(_opCtx.get(), clock->now()));
    ASSERT_EQ(0UL, cursorManager->numCursors());
}

/**
 * Test that client cursors which have been marked as killed but are still pinned *do not* time out.
 */
TEST_F(CursorManagerTest, InactiveKilledCursorsThatAreStillPinnedShouldNotTimeout) {
    CursorManager* cursorManager = useCursorManager();
    auto clock = useClock();

    // Make a cursor from the plan executor, and immediately kill it.
    auto cursorPin = cursorManager->registerCursor(
        _opCtx.get(),
        {makeFakePlanExecutor(), NamespaceString{"test.collection"}, {}, false, BSONObj()});
    const bool collectionGoingAway = false;
    cursorManager->invalidateAll(
        _opCtx.get(), collectionGoingAway, "KilledCursorsShouldTimeoutTest");

    // Advance the clock to simulate time passing.
    clock->advance(Milliseconds(CursorManager::kDefaultCursorTimeoutMinutes));

    // The pin is still in scope, so it should not time out.
    ASSERT_EQ(0UL, cursorManager->timeoutCursors(_opCtx.get(), clock->now()));
}

/**
 * Test that using a cursor updates its time of last use.
 */
TEST_F(CursorManagerTest, UsingACursorShouldUpdateTimeOfLastUse) {
    CursorManager* cursorManager = useCursorManager();
    auto clock = useClock();

    // Register a cursor which we will look at again.
    auto cursorPin = cursorManager->registerCursor(
        _opCtx.get(), {makeFakePlanExecutor(), kTestNss, {}, false, BSONObj()});
    auto usedCursorId = cursorPin.getCursor()->cursorid();
    cursorPin.release();

    // Register a cursor to immediately forget about, to make sure it will time out on a normal
    // schedule.
    cursorManager->registerCursor(_opCtx.get(),
                                  {makeFakePlanExecutor(), kTestNss, {}, false, BSONObj()});

    // Advance the clock to simulate time passing.
    clock->advance(Milliseconds(1));

    // Touch the cursor with id 'usedCursorId' to advance its time of last use.
    cursorManager->pinCursor(_opCtx.get(), usedCursorId).status_with_transitional_ignore();

    // We should be able to time out the unused cursor, but the one we used should stay alive.
    ASSERT_EQ(2UL, cursorManager->numCursors());
    clock->advance(Milliseconds(CursorManager::kDefaultCursorTimeoutMinutes) - Milliseconds(1));
    ASSERT_EQ(1UL, cursorManager->timeoutCursors(_opCtx.get(), clock->now()));
    ASSERT_EQ(1UL, cursorManager->numCursors());

    // We should be able to time out the used cursor after one more millisecond.
    clock->advance(Milliseconds(1));
    ASSERT_EQ(1UL, cursorManager->timeoutCursors(_opCtx.get(), clock->now()));
    ASSERT_EQ(0UL, cursorManager->numCursors());
}

/**
 * Test that a cursor cannot be timed out while in use, and that it's time of last use is updated
 * when it is unpinned.
 */
TEST_F(CursorManagerTest, CursorShouldNotTimeOutUntilIdleForLongEnoughAfterBeingUnpinned) {
    CursorManager* cursorManager = useCursorManager();
    auto clock = useClock();

    // Register a cursor which we will look at again.
    auto cursorPin = cursorManager->registerCursor(
        _opCtx.get(), {makeFakePlanExecutor(), kTestNss, {}, false, BSONObj()});

    // Advance the clock to simulate time passing.
    clock->advance(CursorManager::kDefaultCursorTimeoutMinutes + Milliseconds(1));

    // Make sure the pinned cursor does not time out, before or after unpinning it.
    ASSERT_EQ(1UL, cursorManager->numCursors());
    ASSERT_EQ(0UL, cursorManager->timeoutCursors(_opCtx.get(), clock->now()));
    ASSERT_EQ(1UL, cursorManager->numCursors());

    cursorPin.release();

    ASSERT_EQ(1UL, cursorManager->numCursors());
    ASSERT_EQ(0UL, cursorManager->timeoutCursors(_opCtx.get(), clock->now()));
    ASSERT_EQ(1UL, cursorManager->numCursors());

    // Advance the clock to simulate more time passing, then assert that the now-inactive cursor
    // times out.
    clock->advance(CursorManager::kDefaultCursorTimeoutMinutes + Milliseconds(1));
    ASSERT_EQ(1UL, cursorManager->timeoutCursors(_opCtx.get(), clock->now()));
    ASSERT_EQ(0UL, cursorManager->numCursors());
}

/**
 * Test that cursors inherit the logical session id from their operation context
 */
TEST_F(CursorManagerTestCustomOpCtx, LogicalSessionIdOnOperationCtxTest) {
    // Cursors created on an op ctx without a session id have no session id.
    {
        auto opCtx = _queryServiceContext->makeOperationContext();
        auto pinned = makeCursor(opCtx.get());

        ASSERT_EQUALS(pinned.getCursor()->getSessionId(), boost::none);
    }

    // Cursors created on an op ctx with a session id have a session id.
    {
        auto lsid = makeLogicalSessionIdForTest();
        auto opCtx2 = _queryServiceContext->makeOperationContext(lsid);
        auto pinned2 = makeCursor(opCtx2.get());

        ASSERT_EQUALS(pinned2.getCursor()->getSessionId(), lsid);
    }
}

/**
 * Test that a manager whose cursors do not have sessions does not return them.
 */
TEST_F(CursorManagerTestCustomOpCtx, CursorsWithoutSessions) {
    // Add a cursor with no session to the cursor manager.
    auto opCtx = _queryServiceContext->makeOperationContext();
    auto pinned = makeCursor(opCtx.get());
    ASSERT_EQUALS(pinned.getCursor()->getSessionId(), boost::none);

    // Retrieve all sessions active in manager - set should be empty.
    LogicalSessionIdSet lsids;
    useCursorManager()->appendActiveSessions(&lsids);
    ASSERT(lsids.empty());
}

/**
 * Test a manager that has one cursor running inside of a session.
 */
TEST_F(CursorManagerTestCustomOpCtx, OneCursorWithASession) {
    // Add a cursor with a session to the cursor manager.
    auto lsid = makeLogicalSessionIdForTest();
    auto opCtx = _queryServiceContext->makeOperationContext(lsid);
    auto pinned = makeCursor(opCtx.get());

    // Retrieve all sessions active in manager - set should contain just lsid.
    LogicalSessionIdSet lsids;
    useCursorManager()->appendActiveSessions(&lsids);
    ASSERT_EQ(lsids.size(), size_t(1));
    ASSERT(lsids.find(lsid) != lsids.end());

    // Retrieve all cursors for this lsid - should be just ours.
    auto cursors = useCursorManager()->getCursorsForSession(lsid);
    ASSERT_EQ(cursors.size(), size_t(1));
    auto cursorId = pinned.getCursor()->cursorid();
    ASSERT(cursors.find(cursorId) != cursors.end());

    // Remove the cursor from the manager.
    pinned.release();
    ASSERT_OK(useCursorManager()->eraseCursor(opCtx.get(), cursorId, false));

    // There should be no more cursor entries by session id.
    LogicalSessionIdSet sessions;
    useCursorManager()->appendActiveSessions(&sessions);
    ASSERT(sessions.empty());
    ASSERT(useCursorManager()->getCursorsForSession(lsid).empty());
}

/**
 * Test a manager with multiple cursors running inside of the same session.
 */
TEST_F(CursorManagerTestCustomOpCtx, MultipleCursorsWithSameSession) {
    // Add two cursors on the same session to the cursor manager.
    auto lsid = makeLogicalSessionIdForTest();
    auto opCtx = _queryServiceContext->makeOperationContext(lsid);
    auto pinned = makeCursor(opCtx.get());
    auto pinned2 = makeCursor(opCtx.get());

    auto cursorId1 = pinned.getCursor()->cursorid();
    auto cursorId2 = pinned2.getCursor()->cursorid();

    // Retrieve all sessions - set should contain just lsid.
    stdx::unordered_set<LogicalSessionId, LogicalSessionIdHash> lsids;
    useCursorManager()->appendActiveSessions(&lsids);
    ASSERT_EQ(lsids.size(), size_t(1));
    ASSERT(lsids.find(lsid) != lsids.end());

    // Retrieve all cursors for session - should be both cursors.
    auto cursors = useCursorManager()->getCursorsForSession(lsid);
    ASSERT_EQ(cursors.size(), size_t(2));
    ASSERT(cursors.find(cursorId1) != cursors.end());
    ASSERT(cursors.find(cursorId2) != cursors.end());

    // Remove one cursor from the manager.
    pinned.release();
    ASSERT_OK(useCursorManager()->eraseCursor(opCtx.get(), cursorId1, false));

    // Should still be able to retrieve the session.
    lsids.clear();
    useCursorManager()->appendActiveSessions(&lsids);
    ASSERT_EQ(lsids.size(), size_t(1));
    ASSERT(lsids.find(lsid) != lsids.end());

    // Should still be able to retrieve remaining cursor by session.
    cursors = useCursorManager()->getCursorsForSession(lsid);
    ASSERT_EQ(cursors.size(), size_t(1));
    ASSERT(cursors.find(cursorId2) != cursors.end());
}

/**
 * Test a manager with multiple cursors running inside of different sessions.
 */
TEST_F(CursorManagerTestCustomOpCtx, MultipleCursorsMultipleSessions) {
    auto lsid1 = makeLogicalSessionIdForTest();
    auto lsid2 = makeLogicalSessionIdForTest();

    CursorId cursor1;
    CursorId cursor2;

    // Cursor with session 1.
    {
        auto opCtx1 = _queryServiceContext->makeOperationContext(lsid1);
        cursor1 = makeCursor(opCtx1.get()).getCursor()->cursorid();
    }

    // Cursor with session 2.
    {
        auto opCtx2 = _queryServiceContext->makeOperationContext(lsid2);
        cursor2 = makeCursor(opCtx2.get()).getCursor()->cursorid();
    }

    // Cursor with no session.
    {
        auto opCtx3 = _queryServiceContext->makeOperationContext();
        makeCursor(opCtx3.get()).getCursor();
    }

    // Retrieve all sessions - should be both lsids.
    LogicalSessionIdSet lsids;
    useCursorManager()->appendActiveSessions(&lsids);
    ASSERT_EQ(lsids.size(), size_t(2));
    ASSERT(lsids.find(lsid1) != lsids.end());
    ASSERT(lsids.find(lsid2) != lsids.end());

    // Retrieve cursors for each session - should be just one.
    auto cursors1 = useCursorManager()->getCursorsForSession(lsid1);
    ASSERT_EQ(cursors1.size(), size_t(1));
    ASSERT(cursors1.find(cursor1) != cursors1.end());

    auto cursors2 = useCursorManager()->getCursorsForSession(lsid2);
    ASSERT_EQ(cursors2.size(), size_t(1));
    ASSERT(cursors2.find(cursor2) != cursors2.end());
}

}  // namespace
}  // namespace mongo
