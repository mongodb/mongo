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

#include "mongo/db/query/client_cursor/cursor_manager.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/queued_data_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/client_cursor/clientcursor.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_server_params.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

const NamespaceString kTestNss = NamespaceString::createNamespaceString_forTest("test.collection");

class CursorManagerTestBase : public ServiceContextTest {
protected:
    CursorManagerTestBase()
        : ServiceContextTest(
              std::make_unique<ScopedGlobalServiceContextForTest>(ServiceContext::make(
                  std::make_unique<ClockSourceMock>(), std::make_unique<ClockSourceMock>()))) {}

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeFakePlanExecutor(
        OperationContext* opCtx) {
        // Create a mock ExpressionContext.
        auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx, kTestNss);
        auto workingSet = std::make_unique<WorkingSet>();
        auto queuedDataStage = std::make_unique<QueuedDataStage>(expCtx.get(), workingSet.get());
        return unittest::assertGet(
            plan_executor_factory::make(expCtx,
                                        std::move(workingSet),
                                        std::move(queuedDataStage),
                                        &CollectionPtr::null,
                                        PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                        QueryPlannerParams::DEFAULT,
                                        kTestNss));
    }

    ClientCursorParams makeParams(OperationContext* opCtx) {
        return {
            makeFakePlanExecutor(opCtx),
            kTestNss,
            {},
            APIParameters(),
            opCtx->getWriteConcern(),
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            BSONObj(),
            PrivilegeVector(),
        };
    }

    ClientCursorPin makeCursor(OperationContext* opCtx) {
        return _cursorManager.registerCursor(opCtx, makeParams(opCtx));
    }

    ClockSourceMock* useClock() {
        return static_cast<ClockSourceMock*>(getServiceContext()->getPreciseClockSource());
    }

    CursorManager _cursorManager{getServiceContext()->getPreciseClockSource()};
};

class CursorManagerTest : public CursorManagerTestBase {
protected:
    CursorManagerTest() : _opCtx(getClient()->makeOperationContext()) {}

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeFakePlanExecutor() {
        return CursorManagerTestBase::makeFakePlanExecutor(_opCtx.get());
    }

    ServiceContext::UniqueOperationContext _opCtx;
};

/**
 * Test that a CursorManager is registered with the global ServiceContext.
 */
TEST_F(CursorManagerTest, RegisteredWithGlobalServiceContext) {
    CursorManager* cursorManager = CursorManager::get(getGlobalServiceContext());
    ASSERT(cursorManager);
}

/**
 * Test that a CursorManager is registered with a custom ServiceContext.
 */
TEST_F(CursorManagerTest, RegisteredWithCustomServiceContext) {
    CursorManager* cursorManager = CursorManager::get(getServiceContext());
    ASSERT(cursorManager);
}

/**
 * Test that a CursorManager is accessible via an OperationContext.
 */
TEST_F(CursorManagerTest, CanAccessFromOperationContext) {
    CursorManager* cursorManager = CursorManager::get(_opCtx.get());
    ASSERT(cursorManager);
}

/**
 * Test that an attempt to kill a pinned cursor succeeds.
 */
TEST_F(CursorManagerTest, ShouldBeAbleToKillPinnedCursor) {
    OperationContext* const pinningOpCtx = _opCtx.get();

    auto cursorPin = _cursorManager.registerCursor(
        pinningOpCtx,
        {makeFakePlanExecutor(),
         kTestNss,
         {},
         APIParameters(),
         {},
         repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
         BSONObj(),
         PrivilegeVector()});

    auto cursorId = cursorPin.getCursor()->cursorid();
    ASSERT_OK(_cursorManager.killCursor(_opCtx.get(), cursorId));

    // The original operation should have been interrupted since the cursor was pinned.
    ASSERT_EQ(pinningOpCtx->checkForInterruptNoAssert(), ErrorCodes::CursorKilled);
}

/**
 * Test that an attempt to kill a pinned cursor succeeds with more than one client.
 */
TEST_F(CursorManagerTest, ShouldBeAbleToKillPinnedCursorMultiClient) {
    OperationContext* const pinningOpCtx = _opCtx.get();

    // Pin the cursor from one client.
    auto cursorPin = _cursorManager.registerCursor(
        pinningOpCtx,
        {makeFakePlanExecutor(),
         kTestNss,
         {},
         APIParameters(),
         {},
         repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
         BSONObj(),
         PrivilegeVector()});

    auto cursorId = cursorPin.getCursor()->cursorid();

    {
        // Set up another client to kill the cursor.
        auto killCursorClient =
            getGlobalServiceContext()->getService()->makeClient("killCursorClient");
        AlternativeClientRegion acr(killCursorClient);
        auto killCursorOpCtx = acr->makeOperationContext();
        invariant(killCursorOpCtx);
        ASSERT_OK(_cursorManager.killCursor(killCursorOpCtx.get(), cursorId));
    }

    // The original operation should have been interrupted since the cursor was pinned.
    ASSERT_EQ(pinningOpCtx->checkForInterruptNoAssert(), ErrorCodes::CursorKilled);
}

/**
 * Test that client cursors time out and get deleted.
 */
TEST_F(CursorManagerTest, InactiveCursorShouldTimeout) {
    auto clock = useClock();

    _cursorManager.registerCursor(
        _opCtx.get(),
        {makeFakePlanExecutor(),
         NamespaceString::createNamespaceString_forTest("test.collection"),
         {},
         APIParameters(),
         {},
         repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
         BSONObj(),
         PrivilegeVector()});

    ASSERT_EQ(0UL, _cursorManager.timeoutCursors(_opCtx.get(), Date_t()));

    clock->advance(getDefaultCursorTimeoutMillis());
    ASSERT_EQ(1UL, _cursorManager.timeoutCursors(_opCtx.get(), clock->now()));
    ASSERT_EQ(0UL, _cursorManager.numCursors());

    _cursorManager.registerCursor(
        _opCtx.get(),
        {makeFakePlanExecutor(),
         NamespaceString::createNamespaceString_forTest("test.collection"),
         {},
         APIParameters(),
         {},
         repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
         BSONObj(),
         PrivilegeVector()});
    ASSERT_EQ(1UL, _cursorManager.timeoutCursors(_opCtx.get(), Date_t::max()));
    ASSERT_EQ(0UL, _cursorManager.numCursors());
}

/**
 * Test that pinned cursors do not get timed out.
 */
TEST_F(CursorManagerTest, InactivePinnedCursorShouldNotTimeout) {
    auto clock = useClock();

    auto cursorPin = _cursorManager.registerCursor(
        _opCtx.get(),
        {makeFakePlanExecutor(),
         NamespaceString::createNamespaceString_forTest("test.collection"),
         {},
         APIParameters(),
         {},
         repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
         BSONObj(),
         PrivilegeVector()});

    // The pin is still in scope, so it should not time out.
    clock->advance(getDefaultCursorTimeoutMillis());
    ASSERT_EQ(0UL, _cursorManager.timeoutCursors(_opCtx.get(), clock->now()));
}

/**
 * A cursor can be left in the CursorManager in a killed state when a pinned cursor is interrupted
 * with an unusual error code (a code other than ErrorCodes::Interrupted or
 * ErrorCodes::CursorKilled). Verify that such cursors get deregistered and deleted on an attempt to
 * pin.
 */
TEST_F(CursorManagerTest, MarkedAsKilledCursorsShouldBeDeletedOnCursorPin) {
    auto cursorPin = _cursorManager.registerCursor(
        _opCtx.get(),
        {makeFakePlanExecutor(),
         NamespaceString::createNamespaceString_forTest("test.collection"),
         {},
         APIParameters(),
         {},
         repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
         BSONObj(),
         PrivilegeVector()});
    auto cursorId = cursorPin->cursorid();

    // A cursor will stay alive, but be marked as killed, if it is interrupted with a code other
    // than ErrorCodes::Interrupted or ErrorCodes::CursorKilled and then unpinned.
    _opCtx->markKilled(ErrorCodes::InternalError);
    cursorPin.release();

    // The cursor should still be present in the manager.
    ASSERT_EQ(1UL, _cursorManager.numCursors());

    // Pinning the cursor should fail with the same error code that interrupted the OpCtx. The
    // cursor should no longer be present in the manager.
    ASSERT_EQ(_cursorManager.pinCursor(_opCtx.get(), cursorId, "getMore").getStatus(),
              ErrorCodes::InternalError);
    ASSERT_EQ(0UL, _cursorManager.numCursors());
}

/**
 * Test that client cursors which have been marked as killed time out and get deleted.
 */
TEST_F(CursorManagerTest, InactiveKilledCursorsShouldTimeout) {
    auto clock = useClock();

    auto cursorPin = _cursorManager.registerCursor(
        _opCtx.get(),
        {makeFakePlanExecutor(),
         NamespaceString::createNamespaceString_forTest("test.collection"),
         {},
         APIParameters(),
         {},
         repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
         BSONObj(),
         PrivilegeVector()});

    // A cursor will stay alive, but be marked as killed, if it is interrupted with a code other
    // than ErrorCodes::Interrupted or ErrorCodes::CursorKilled and then unpinned.
    _opCtx->markKilled(ErrorCodes::InternalError);
    cursorPin.release();

    // The cursor should still be present in the manager.
    ASSERT_EQ(1UL, _cursorManager.numCursors());

    // Advance the clock to simulate time passing, and verify that the cursor times out.
    clock->advance(getDefaultCursorTimeoutMillis());
    ASSERT_EQ(1UL, _cursorManager.timeoutCursors(_opCtx.get(), clock->now()));
    ASSERT_EQ(0UL, _cursorManager.numCursors());
}

/**
 * Test that using a cursor updates its time of last use.
 */
TEST_F(CursorManagerTest, UsingACursorShouldUpdateTimeOfLastUse) {
    auto clock = useClock();

    // Register a cursor which we will look at again.
    auto cursorPin = _cursorManager.registerCursor(
        _opCtx.get(),
        {makeFakePlanExecutor(),
         kTestNss,
         {},
         APIParameters(),
         {},
         repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
         BSONObj(),
         PrivilegeVector()});
    auto usedCursorId = cursorPin.getCursor()->cursorid();
    cursorPin.release();

    // Register a cursor to immediately forget about, to make sure it will time out on a normal
    // schedule.
    _cursorManager.registerCursor(_opCtx.get(),
                                  {makeFakePlanExecutor(),
                                   kTestNss,
                                   {},
                                   APIParameters(),
                                   {},
                                   repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
                                   ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                   BSONObj(),
                                   PrivilegeVector()});

    // Advance the clock to simulate time passing.
    clock->advance(Milliseconds(1));

    // Touch the cursor with id 'usedCursorId' to advance its time of last use.
    _cursorManager.pinCursor(_opCtx.get(), usedCursorId, "getMore")
        .status_with_transitional_ignore();

    // We should be able to time out the unused cursor, but the one we used should stay alive.
    ASSERT_EQ(2UL, _cursorManager.numCursors());
    clock->advance(getDefaultCursorTimeoutMillis() - Milliseconds(1));
    ASSERT_EQ(1UL, _cursorManager.timeoutCursors(_opCtx.get(), clock->now()));
    ASSERT_EQ(1UL, _cursorManager.numCursors());

    // We should be able to time out the used cursor after one more millisecond.
    clock->advance(Milliseconds(1));
    ASSERT_EQ(1UL, _cursorManager.timeoutCursors(_opCtx.get(), clock->now()));
    ASSERT_EQ(0UL, _cursorManager.numCursors());
}

/**
 * Test that a cursor cannot be timed out while in use, and that it's time of last use is updated
 * when it is unpinned.
 */
TEST_F(CursorManagerTest, CursorShouldNotTimeOutUntilIdleForLongEnoughAfterBeingUnpinned) {
    auto clock = useClock();

    // Register a cursor which we will look at again.
    auto cursorPin = _cursorManager.registerCursor(
        _opCtx.get(),
        {makeFakePlanExecutor(),
         kTestNss,
         {},
         APIParameters(),
         {},
         repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
         BSONObj(),
         PrivilegeVector()});

    // Advance the clock to simulate time passing.
    clock->advance(getDefaultCursorTimeoutMillis() + Milliseconds(1));

    // Make sure the pinned cursor does not time out, before or after unpinning it.
    ASSERT_EQ(1UL, _cursorManager.numCursors());
    ASSERT_EQ(0UL, _cursorManager.timeoutCursors(_opCtx.get(), clock->now()));
    ASSERT_EQ(1UL, _cursorManager.numCursors());

    cursorPin.release();

    ASSERT_EQ(1UL, _cursorManager.numCursors());
    ASSERT_EQ(0UL, _cursorManager.timeoutCursors(_opCtx.get(), clock->now()));
    ASSERT_EQ(1UL, _cursorManager.numCursors());

    // Advance the clock to simulate more time passing, then assert that the now-inactive cursor
    // times out.
    clock->advance(getDefaultCursorTimeoutMillis() + Milliseconds(1));
    ASSERT_EQ(1UL, _cursorManager.timeoutCursors(_opCtx.get(), clock->now()));
    ASSERT_EQ(0UL, _cursorManager.numCursors());
}

/**
 * Test that a cursor correctly stores API parameters.
 */
TEST_F(CursorManagerTest, CursorStoresAPIParameters) {
    APIParameters apiParams = APIParameters();
    apiParams.setAPIVersion("2");
    apiParams.setAPIStrict(true);
    apiParams.setAPIDeprecationErrors(true);

    auto cursorPin = _cursorManager.registerCursor(
        _opCtx.get(),
        {makeFakePlanExecutor(),
         kTestNss,
         {},
         apiParams,
         {},
         repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
         BSONObj(),
         PrivilegeVector()});

    auto storedAPIParams = cursorPin->getAPIParameters();
    ASSERT_EQ("2", *storedAPIParams.getAPIVersion());
    ASSERT_TRUE(*storedAPIParams.getAPIStrict());
    ASSERT_TRUE(*storedAPIParams.getAPIDeprecationErrors());
}

class CursorManagerTestCustomOpCtx : public CursorManagerTestBase {
protected:
    auto makeOperationContext() {
        return getClient()->makeOperationContext();
    }
};

/**
 * Test that cursors inherit the logical session id from their operation context
 */
TEST_F(CursorManagerTestCustomOpCtx, LogicalSessionIdOnOperationCtxTest) {
    // Cursors created on an op ctx without a session id have no session id.
    {
        auto opCtx = makeOperationContext();
        auto pinned = makeCursor(opCtx.get());

        ASSERT_EQUALS(pinned.getCursor()->getSessionId(), boost::none);
    }

    // Cursors created on an op ctx with a session id have a session id.
    {
        auto lsid = makeLogicalSessionIdForTest();
        auto opCtx2 = makeOperationContext();
        opCtx2->setLogicalSessionId(lsid);
        auto pinned2 = makeCursor(opCtx2.get());

        ASSERT_EQUALS(pinned2.getCursor()->getSessionId(), lsid);
    }
}

/**
 * Test that a manager whose cursors do not have sessions does not return them.
 */
TEST_F(CursorManagerTestCustomOpCtx, CursorsWithoutSessions) {
    // Add a cursor with no session to the cursor manager.
    auto opCtx = makeOperationContext();
    auto pinned = makeCursor(opCtx.get());
    ASSERT_EQUALS(pinned.getCursor()->getSessionId(), boost::none);

    // Retrieve all sessions active in manager - set should be empty.
    LogicalSessionIdSet lsids;
    _cursorManager.appendActiveSessions(&lsids);
    ASSERT(lsids.empty());
}

/**
 * Test a manager that has one cursor running inside of a session.
 */
TEST_F(CursorManagerTestCustomOpCtx, OneCursorWithASession) {
    // Add a cursor with a session to the cursor manager.
    auto lsid = makeLogicalSessionIdForTest();
    auto opCtx = makeOperationContext();
    opCtx->setLogicalSessionId(lsid);
    auto pinned = makeCursor(opCtx.get());

    // Retrieve all sessions active in manager - set should contain just lsid.
    LogicalSessionIdSet lsids;
    _cursorManager.appendActiveSessions(&lsids);
    ASSERT_EQ(lsids.size(), size_t(1));
    ASSERT(lsids.find(lsid) != lsids.end());

    // Retrieve all cursors for this lsid - should be just ours.
    auto cursors = _cursorManager.getCursorsForSession(lsid);
    ASSERT_EQ(cursors.size(), size_t(1));
    auto cursorId = pinned.getCursor()->cursorid();
    ASSERT(cursors.find(cursorId) != cursors.end());

    // Remove the cursor from the manager.
    pinned.release();
    ASSERT_OK(_cursorManager.killCursor(opCtx.get(), cursorId));

    // There should be no more cursor entries by session id.
    LogicalSessionIdSet sessions;
    _cursorManager.appendActiveSessions(&sessions);
    ASSERT(sessions.empty());
    ASSERT(_cursorManager.getCursorsForSession(lsid).empty());
}

/**
 * Test a manager with multiple cursors running inside of the same session.
 */
TEST_F(CursorManagerTestCustomOpCtx, MultipleCursorsWithSameSession) {
    // Add two cursors on the same session to the cursor manager.
    auto lsid = makeLogicalSessionIdForTest();
    auto opCtx = makeOperationContext();
    opCtx->setLogicalSessionId(lsid);
    auto pinned = makeCursor(opCtx.get());
    auto pinned2 = makeCursor(opCtx.get());

    auto cursorId1 = pinned.getCursor()->cursorid();
    auto cursorId2 = pinned2.getCursor()->cursorid();

    // Retrieve all sessions - set should contain just lsid.
    stdx::unordered_set<LogicalSessionId, LogicalSessionIdHash> lsids;
    _cursorManager.appendActiveSessions(&lsids);
    ASSERT_EQ(lsids.size(), size_t(1));
    ASSERT(lsids.find(lsid) != lsids.end());

    // Retrieve all cursors for session - should be both cursors.
    auto cursors = _cursorManager.getCursorsForSession(lsid);
    ASSERT_EQ(cursors.size(), size_t(2));
    ASSERT(cursors.find(cursorId1) != cursors.end());
    ASSERT(cursors.find(cursorId2) != cursors.end());

    // Remove one cursor from the manager.
    pinned.release();
    ASSERT_OK(_cursorManager.killCursor(opCtx.get(), cursorId1));

    // Should still be able to retrieve the session.
    lsids.clear();
    _cursorManager.appendActiveSessions(&lsids);
    ASSERT_EQ(lsids.size(), size_t(1));
    ASSERT(lsids.find(lsid) != lsids.end());

    // Should still be able to retrieve remaining cursor by session.
    cursors = _cursorManager.getCursorsForSession(lsid);
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
        auto opCtx1 = makeOperationContext();
        opCtx1->setLogicalSessionId(lsid1);
        cursor1 = makeCursor(opCtx1.get()).getCursor()->cursorid();
    }

    // Cursor with session 2.
    {
        auto opCtx2 = makeOperationContext();
        opCtx2->setLogicalSessionId(lsid2);
        cursor2 = makeCursor(opCtx2.get()).getCursor()->cursorid();
    }

    // Cursor with no session.
    {
        auto opCtx3 = makeOperationContext();
        makeCursor(opCtx3.get()).getCursor();
    }

    // Retrieve all sessions - should be both lsids.
    LogicalSessionIdSet lsids;
    _cursorManager.appendActiveSessions(&lsids);
    ASSERT_EQ(lsids.size(), size_t(2));
    ASSERT(lsids.find(lsid1) != lsids.end());
    ASSERT(lsids.find(lsid2) != lsids.end());

    // Retrieve cursors for each session - should be just one.
    auto cursors1 = _cursorManager.getCursorsForSession(lsid1);
    ASSERT_EQ(cursors1.size(), size_t(1));
    ASSERT(cursors1.find(cursor1) != cursors1.end());

    auto cursors2 = _cursorManager.getCursorsForSession(lsid2);
    ASSERT_EQ(cursors2.size(), size_t(1));
    ASSERT(cursors2.find(cursor2) != cursors2.end());
}

TEST_F(CursorManagerTestCustomOpCtx, CursorsWithoutOperationKeys) {
    auto opCtx = makeOperationContext();
    auto pinned = makeCursor(opCtx.get());
    ASSERT_EQUALS(pinned.getCursor()->getOperationKey(), boost::none);
}

TEST_F(CursorManagerTestCustomOpCtx, OneCursorWithAnOperationKey) {
    auto opKey = UUID::gen();
    auto opCtx = makeOperationContext();
    opCtx->setOperationKey(opKey);
    auto pinned = makeCursor(opCtx.get());

    auto cursors = _cursorManager.getCursorsForOpKeys({opKey});
    ASSERT_EQ(cursors.size(), size_t(1));
    auto cursorId = pinned.getCursor()->cursorid();
    ASSERT(cursors.find(cursorId) != cursors.end());

    // Remove the cursor from the manager and verify that we can't retrieve it.
    pinned.release();
    ASSERT_OK(_cursorManager.killCursor(opCtx.get(), cursorId));
    ASSERT(_cursorManager.getCursorsForOpKeys({opKey}).empty());
}

TEST_F(CursorManagerTestCustomOpCtx, MultipleCursorsMultipleOperationKeys) {
    auto opKey1 = UUID::gen();
    auto opKey2 = UUID::gen();

    CursorId cursor1;
    CursorId cursor2;

    // Cursor with operationKey 1.
    {
        auto opCtx1 = makeOperationContext();
        opCtx1->setOperationKey(opKey1);
        cursor1 = makeCursor(opCtx1.get()).getCursor()->cursorid();
    }

    // Cursor with operationKey 2.
    {
        auto opCtx2 = makeOperationContext();
        opCtx2->setOperationKey(opKey2);
        cursor2 = makeCursor(opCtx2.get()).getCursor()->cursorid();
    }

    // Cursor with no operation key.
    {
        auto opCtx3 = makeOperationContext();
        makeCursor(opCtx3.get()).getCursor();
    }

    // Retrieve cursors for each operation key - should be one for each.
    auto cursors1 = _cursorManager.getCursorsForOpKeys({opKey1});
    ASSERT_EQ(cursors1.size(), size_t(1));
    ASSERT(cursors1.find(cursor1) != cursors1.end());

    auto cursors2 = _cursorManager.getCursorsForOpKeys({opKey2});
    ASSERT_EQ(cursors2.size(), size_t(1));
    ASSERT(cursors2.find(cursor2) != cursors2.end());

    // Retrieve cursors for both operation keys.
    auto cursors = _cursorManager.getCursorsForOpKeys({opKey1, opKey2});
    ASSERT_EQ(cursors.size(), size_t(2));
    ASSERT(cursors.find(cursor1) != cursors.end());
    ASSERT(cursors.find(cursor2) != cursors.end());
}

TEST_F(CursorManagerTestCustomOpCtx, MultipleCursorsSameOperationKey) {
    auto opKey = UUID::gen();

    auto opCtx = makeOperationContext();
    opCtx->setOperationKey(opKey);
    auto cursor1 = makeCursor(opCtx.get()).getCursor()->cursorid();
    auto cursor2 = makeCursor(opCtx.get()).getCursor()->cursorid();

    // Retrieve cursors for operation key - should be both cursors.
    auto cursors = _cursorManager.getCursorsForOpKeys({opKey});
    ASSERT_EQ(cursors.size(), size_t(2));
    ASSERT(cursors.find(cursor1) != cursors.end());
    ASSERT(cursors.find(cursor2) != cursors.end());

    // Now delete first one. The other should remain.
    ASSERT_OK(_cursorManager.killCursor(opCtx.get(), cursor1));
    cursors = _cursorManager.getCursorsForOpKeys({opKey});
    ASSERT_EQ(cursors.size(), size_t(1));
    ASSERT(cursors.find(cursor1) == cursors.end());
    ASSERT(cursors.find(cursor2) != cursors.end());

    // Now delete the other. None should remain.
    ASSERT_OK(_cursorManager.killCursor(opCtx.get(), cursor2));
    cursors = _cursorManager.getCursorsForOpKeys({opKey});
    ASSERT_EQ(cursors.size(), size_t(0));
}

TEST_F(CursorManagerTestCustomOpCtx, TimedOutCursorShouldNotBeReturnedForOpKeyLookup) {
    auto opKey = UUID::gen();
    auto opCtx = makeOperationContext();
    opCtx->setOperationKey(opKey);
    auto clock = useClock();

    auto cursor = makeCursor(opCtx.get());

    ASSERT_EQ(1UL, _cursorManager.numCursors());
    ASSERT_EQ(0UL, _cursorManager.timeoutCursors(opCtx.get(), Date_t()));

    // Advance the clock and verify that the cursor times out.
    cursor.release();
    clock->advance(getDefaultCursorTimeoutMillis() + Milliseconds(1));
    ASSERT_EQ(1UL, _cursorManager.timeoutCursors(opCtx.get(), clock->now()));
    ASSERT_EQ(0UL, _cursorManager.numCursors());

    // Verify that the timed out cursor is not returned when looking up by OperationKey.
    auto cursors = _cursorManager.getCursorsForOpKeys({opKey});
    ASSERT_EQ(cursors.size(), size_t(0));
}

TEST_F(CursorManagerTestCustomOpCtx, CursorsMarkedAsKilledAreReturnedForOpKeyLookup) {
    auto opKey = UUID::gen();
    auto opCtx = makeOperationContext();
    opCtx->setOperationKey(opKey);

    auto cursor = makeCursor(opCtx.get());

    // Mark the OperationContext as killed.
    {
        ClientLock clientLk(opCtx->getClient());
        // A cursor will stay alive, but be marked as killed, if it is interrupted with a code other
        // than ErrorCodes::Interrupted or ErrorCodes::CursorKilled and then unpinned.
        opCtx->getServiceContext()->killOperation(clientLk, opCtx.get(), ErrorCodes::InternalError);
    }
    cursor.release();

    // The cursor should still be present in the manager.
    ASSERT_EQ(1UL, _cursorManager.numCursors());

    // Verify that the killed cursor is still returned when looking up by OperationKey.
    auto cursors = _cursorManager.getCursorsForOpKeys({opKey});
    ASSERT_EQ(cursors.size(), size_t(1));
}

TEST_F(CursorManagerTestCustomOpCtx,
       GetCursorIdsForNamespaceReturnsSingleEntryForMatchingNamespace) {
    auto opCtx = makeOperationContext();
    auto pinned = makeCursor(opCtx.get());
    auto cursorId = pinned.getCursor()->cursorid();
    auto cursorsForNamespace = _cursorManager.getCursorIdsForNamespace(kTestNss);
    ASSERT_EQUALS(cursorsForNamespace.size(), 1ull);
    ASSERT_EQUALS(cursorsForNamespace[0], cursorId);
}

TEST_F(CursorManagerTestCustomOpCtx,
       GetCursorIdsForNamespaceReturnsMultipleEntriesForMatchingNamespace) {
    auto opCtx = makeOperationContext();
    auto pinned1 = makeCursor(opCtx.get());
    auto pinned2 = makeCursor(opCtx.get());
    auto cursorId1 = pinned1.getCursor()->cursorid();
    auto cursorId2 = pinned2.getCursor()->cursorid();
    auto cursorsForNamespace = _cursorManager.getCursorIdsForNamespace(kTestNss);
    ASSERT_EQUALS(cursorsForNamespace.size(), 2ull);
    // The results for cursorsForNamespace won't necessarily be the same as the order of insertion.
    std::set<CursorId> cursorsForNamespaceSet(cursorsForNamespace.begin(),
                                              cursorsForNamespace.end());

    ASSERT_EQUALS(cursorsForNamespaceSet.count(cursorId1), 1ull);
    ASSERT_EQUALS(cursorsForNamespaceSet.count(cursorId2), 1ull);
}

TEST_F(CursorManagerTestCustomOpCtx,
       GetCursorIdsForNamespaceDoesNotReturnEntriesForNonMatchingNamespace) {
    auto opCtx = makeOperationContext();
    // Add a cursor for kTestNss.
    auto pinned = makeCursor(opCtx.get());
    // Get cursors for a different NamespaceString.
    auto cursorsForNamespace = _cursorManager.getCursorIdsForNamespace(
        NamespaceString::createNamespaceString_forTest("somerandom.nss"));
    ASSERT_EQUALS(cursorsForNamespace.size(), 0ull);
}

TEST_F(CursorManagerTestBase, CursorLifespanHistogramCorrectlyUpdated) {
    Date_t present = Date_t::now();

    auto expectedCount = cursorStats().lifespanLessThan1Second.get() + 1;
    incrementCursorLifespanMetric(present, present + Milliseconds(500));
    ASSERT_EQUALS(expectedCount, cursorStats().lifespanLessThan1Second.get());
    auto countForLt1Second = cursorStats().lifespanLessThan1Second.get();

    expectedCount = cursorStats().lifespanLessThan5Seconds.get() + 1;
    incrementCursorLifespanMetric(present, present + Seconds(2));
    ASSERT_EQUALS(expectedCount, cursorStats().lifespanLessThan5Seconds.get());

    expectedCount = cursorStats().lifespanLessThan15Seconds.get() + 1;
    incrementCursorLifespanMetric(present, present + Seconds(10));
    ASSERT_EQUALS(expectedCount, cursorStats().lifespanLessThan15Seconds.get());

    expectedCount = cursorStats().lifespanLessThan30Seconds.get() + 1;
    incrementCursorLifespanMetric(present, present + Seconds(20));
    ASSERT_EQUALS(expectedCount, cursorStats().lifespanLessThan30Seconds.get());

    expectedCount = cursorStats().lifespanLessThan1Minute.get() + 2;
    // We'll just do the 30s - 60s twice to make it a little different.
    incrementCursorLifespanMetric(present, present + Seconds(40));
    incrementCursorLifespanMetric(present, present + Seconds(40));
    ASSERT_EQUALS(expectedCount, cursorStats().lifespanLessThan1Minute.get());

    expectedCount = cursorStats().lifespanLessThan10Minutes.get() + 1;
    incrementCursorLifespanMetric(present, present + Minutes(2));
    ASSERT_EQUALS(expectedCount, cursorStats().lifespanLessThan10Minutes.get());

    expectedCount = cursorStats().lifespanGreaterThanOrEqual10Minutes.get() + 1;
    incrementCursorLifespanMetric(present, present + Minutes(12));
    ASSERT_EQUALS(expectedCount, cursorStats().lifespanGreaterThanOrEqual10Minutes.get());

    // Just make sure the smallest bucket counter is unchanged.
    ASSERT_EQUALS(countForLt1Second, cursorStats().lifespanLessThan1Second.get());
}

/**
 * Test that an attempt to kill a pinned cursor succeeds with a passing auth check.
 */
TEST_F(CursorManagerTest, KillCursorWithPassingAuthCheckSucceeds) {
    OperationContext* const pinningOpCtx = _opCtx.get();

    auto cursorPin = _cursorManager.registerCursor(
        pinningOpCtx,
        {makeFakePlanExecutor(),
         kTestNss,
         {},
         APIParameters(),
         {},
         repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
         BSONObj(),
         PrivilegeVector()});

    auto cursorId = cursorPin.getCursor()->cursorid();
    cursorPin.release();

    auto pinCheck = [&](const ClientCursor& cc) {
        uassertStatusOK(Status::OK());
    };
    ASSERT_OK(_cursorManager.killCursorWithAuthCheck(_opCtx.get(), cursorId, pinCheck));
}

/**
 * Test that an attempt to kill fails due to an auth check.
 */
TEST_F(CursorManagerTest, KillCursorWithFailingAuthCheckFails) {
    OperationContext* const pinningOpCtx = _opCtx.get();

    auto cursorPin = _cursorManager.registerCursor(
        pinningOpCtx,
        {makeFakePlanExecutor(),
         kTestNss,
         {},
         APIParameters(),
         {},
         repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
         BSONObj(),
         PrivilegeVector()});

    auto cursorId = cursorPin.getCursor()->cursorid();
    cursorPin.release();

    auto pinCheck = [&](const ClientCursor& cc) {
        uassertStatusOK(Status(ErrorCodes::Unauthorized, "Unauthorized"));
    };
    ASSERT_THROWS_CODE(_cursorManager.killCursorWithAuthCheck(_opCtx.get(), cursorId, pinCheck),
                       DBException,
                       ErrorCodes::Unauthorized);
}

}  // namespace
}  // namespace mongo
