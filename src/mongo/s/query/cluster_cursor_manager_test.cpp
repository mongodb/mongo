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

#include <absl/container/node_hash_set.h>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <fmt/format.h>
#include <iterator>
#include <list>
#include <type_traits>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/s/query/cluster_client_cursor_mock.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_query_result.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {

using unittest::assertGet;

const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.collection");

class ClusterCursorManagerTest : public ServiceContextTest {
protected:
    ClusterCursorManagerTest() {
        _opCtx = makeOperationContext();
        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
    }

    ~ClusterCursorManagerTest() {
        _manager.shutdown(_opCtx.get());
    }

    static Status successAuthChecker(const boost::optional<UserName>&) {
        return Status::OK();
    };

    static Status failAuthChecker(const boost::optional<UserName>&) {
        return {ErrorCodes::Unauthorized, "Unauthorized"};
    };

    /**
     * Returns an unowned pointer to the manager owned by this test fixture.
     */
    ClusterCursorManager* getManager() {
        return &_manager;
    }

    /**
     * Returns an unowned pointer to the mock clock source owned by this test fixture.
     */
    ClockSourceMock* getClockSource() {
        return &_clockSourceMock;
    }

    /**
     * Returns an unowned pointer to the OperationContext owned by this test fixture.
     */
    OperationContext* getOperationContext() {
        return _opCtx.get();
    }

    /**
     * Allocates a mock cursor, which can be used with the 'isMockCursorKilled' method below.
     */
    std::unique_ptr<ClusterClientCursorMock> allocateMockCursor(
        boost::optional<LogicalSessionId> lsid = boost::none,
        boost::optional<TxnNumber> txnNumber = boost::none) {
        // Allocate a new boolean to our list to track when this cursor is killed.
        _cursorKilledFlags.push_back(false);

        // Allocate and return a cursor with a kill callback that sets the cursor's killed flag in
        // our list.  Note that it is safe to capture the kill flag in our list by reference
        // (std::list<>::push_back() does not invalidate references, and our list outlives the
        // manager).
        bool& killedFlag = _cursorKilledFlags.back();
        return std::make_unique<ClusterClientCursorMock>(
            std::move(lsid), std::move(txnNumber), [&killedFlag]() { killedFlag = true; });
    }

    /**
     * Returns whether or not the i-th allocated cursor been killed.  'i' should be zero-indexed,
     * i.e. the initial allocated cursor can be checked for a kill with 'isMockCursorKilled(0)'.
     */
    bool isMockCursorKilled(size_t i) const {
        invariant(i < _cursorKilledFlags.size());
        return *std::next(_cursorKilledFlags.begin(), i);
    }

    void killCursorFromDifferentOpCtx(CursorId cursorId) {
        // Set up another client to kill the cursor.
        auto killCursorClient = getServiceContext()->makeClient("killCursorClient");
        auto killCursorOpCtx = killCursorClient->makeOperationContext();
        AlternativeClientRegion acr(killCursorClient);
        ASSERT_OK(getManager()->killCursor(killCursorOpCtx.get(), cursorId));
    }


private:
    // List of flags representing whether our allocated cursors have been killed yet.  The value of
    // the flag is true iff the cursor has been killed.
    //
    // We use std::list<> for this member (instead of std::vector<>, for example) so that we can
    // keep references that don't get invalidated as the list grows.
    std::list<bool> _cursorKilledFlags;
    ClockSourceMock _clockSourceMock;
    ClusterCursorManager _manager{&_clockSourceMock};
    ServiceContext::UniqueOperationContext _opCtx;
};

// Test that registering a cursor and checking it out returns a pin to the same cursor.
TEST_F(ClusterCursorManagerTest, RegisterCursor) {
    auto cursor = allocateMockCursor();
    cursor->queueResult(BSON("a" << 1));
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               std::move(cursor),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto pinnedCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_OK(pinnedCursor.getStatus());
    auto nextResult = pinnedCursor.getValue()->next();
    ASSERT_OK(nextResult.getStatus());
    ASSERT(nextResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(BSON("a" << 1), *nextResult.getValue().getResult());
    nextResult = pinnedCursor.getValue()->next();
    ASSERT_OK(nextResult.getStatus());
    ASSERT_TRUE(nextResult.getValue().isEOF());
}

// Test that registering a cursor returns a non-zero cursor id.
TEST_F(ClusterCursorManagerTest, RegisterCursorReturnsNonZeroId) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    ASSERT_NE(0, cursorId);
}

// Test that checking out a cursor returns a pin to the correct cursor.
TEST_F(ClusterCursorManagerTest, CheckOutCursorBasic) {
    auto cursor = allocateMockCursor();
    cursor->queueResult(BSON("a" << 1));
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               std::move(cursor),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto checkedOutCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_OK(checkedOutCursor.getStatus());
    ASSERT_EQ(cursorId, checkedOutCursor.getValue().getCursorId());
    auto nextResult = checkedOutCursor.getValue()->next();
    ASSERT_OK(nextResult.getStatus());
    ASSERT(nextResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(BSON("a" << 1), *nextResult.getValue().getResult());
    nextResult = checkedOutCursor.getValue()->next();
    ASSERT_OK(nextResult.getStatus());
    ASSERT_TRUE(nextResult.getValue().isEOF());
}

// Test that checking out a cursor returns a pin to the correct cursor, when multiple cursors are
// registered.
TEST_F(ClusterCursorManagerTest, CheckOutCursorMultipleCursors) {
    const int numCursors = 10;
    std::vector<CursorId> cursorIds(numCursors);
    for (int i = 0; i < numCursors; ++i) {
        auto cursor = allocateMockCursor();
        cursor->queueResult(BSON("a" << i));
        cursorIds[i] =
            assertGet(getManager()->registerCursor(getOperationContext(),
                                                   std::move(cursor),
                                                   nss,
                                                   ClusterCursorManager::CursorType::SingleTarget,
                                                   ClusterCursorManager::CursorLifetime::Mortal,
                                                   boost::none));
    }
    for (int i = 0; i < numCursors; ++i) {
        auto pinnedCursor =
            getManager()->checkOutCursor(cursorIds[i], getOperationContext(), successAuthChecker);
        ASSERT_OK(pinnedCursor.getStatus());
        auto nextResult = pinnedCursor.getValue()->next();
        ASSERT_OK(nextResult.getStatus());
        ASSERT(nextResult.getValue().getResult());
        ASSERT_BSONOBJ_EQ(BSON("a" << i), *nextResult.getValue().getResult());
        nextResult = pinnedCursor.getValue()->next();
        ASSERT_OK(nextResult.getStatus());
        ASSERT_TRUE(nextResult.getValue().isEOF());
    }
}

// Test that checking out a pinned cursor returns an error with code ErrorCodes::CursorInUse.
TEST_F(ClusterCursorManagerTest, CheckOutCursorPinned) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto pinnedCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_OK(pinnedCursor.getStatus());
    ASSERT_EQ(ErrorCodes::CursorInUse,
              getManager()
                  ->checkOutCursor(cursorId, getOperationContext(), successAuthChecker)
                  .getStatus());
}

// Test that checking out a killed cursor returns an error with code ErrorCodes::CursorNotFound.
TEST_F(ClusterCursorManagerTest, CheckOutCursorKilled) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    killCursorFromDifferentOpCtx(cursorId);
    ASSERT_EQ(ErrorCodes::CursorNotFound,
              getManager()
                  ->checkOutCursor(cursorId, getOperationContext(), successAuthChecker)
                  .getStatus());
}

// Test that checking out an unknown cursor returns an error with code ErrorCodes::CursorNotFound.
TEST_F(ClusterCursorManagerTest, CheckOutCursorUnknown) {
    ASSERT_EQ(ErrorCodes::CursorNotFound,
              getManager()->checkOutCursor(5, nullptr, successAuthChecker).getStatus());
}

// Test that checking out a unknown cursor returns an error with code ErrorCodes::CursorNotFound,
// even if there is an existing cursor with the same namespace but a different cursor id.
TEST_F(ClusterCursorManagerTest, CheckOutCursorWrongCursorId) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    ASSERT_EQ(ErrorCodes::CursorNotFound,
              getManager()
                  ->checkOutCursor(cursorId + 1, getOperationContext(), successAuthChecker)
                  .getStatus());
}

// Test that checking out a cursor updates the 'last active' time associated with the cursor to the
// current time.
TEST_F(ClusterCursorManagerTest, CheckOutCursorUpdateActiveTime) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    Date_t cursorRegistrationTime = getClockSource()->now();
    getClockSource()->advance(Milliseconds(1));
    auto checkedOutCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_OK(checkedOutCursor.getStatus());
    checkedOutCursor.getValue().returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    getManager()->killMortalCursorsInactiveSince(getOperationContext(), cursorRegistrationTime);
    ASSERT(!isMockCursorKilled(0));
}

TEST_F(ClusterCursorManagerTest, CheckOutCursorAuthFails) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto checkedOutCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), failAuthChecker);
    ASSERT_EQ(checkedOutCursor.getStatus(), ErrorCodes::Unauthorized);
}


// Test that checking in a cursor updates the 'last active' time associated with the cursor to the
// current time.
TEST_F(ClusterCursorManagerTest, ReturnCursorUpdateActiveTime) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    Date_t cursorCheckOutTime = getClockSource()->now();
    auto checkedOutCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_OK(checkedOutCursor.getStatus());
    getClockSource()->advance(Milliseconds(1));
    checkedOutCursor.getValue().returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    getManager()->killMortalCursorsInactiveSince(getOperationContext(), cursorCheckOutTime);
    ASSERT(!isMockCursorKilled(0));
}

// Test that killing a pinned cursor by id successfully kills the cursor.
TEST_F(ClusterCursorManagerTest, KillUnpinnedCursorBasic) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    killCursorFromDifferentOpCtx(cursorId);
    ASSERT(isMockCursorKilled(0));
}

// Test that killing a pinned cursor by id successfully kills the cursor.
TEST_F(ClusterCursorManagerTest, KillPinnedCursorBasic) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto pinnedCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_OK(pinnedCursor.getStatus());
    killCursorFromDifferentOpCtx(pinnedCursor.getValue().getCursorId());

    // When the cursor is pinned the operation which checked out the cursor should be interrupted.
    ASSERT_EQ(getOperationContext()->checkForInterruptNoAssert(), ErrorCodes::CursorKilled);

    ASSERT(!isMockCursorKilled(0));
    pinnedCursor.getValue().returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT(isMockCursorKilled(0));
}

// Test that killing a cursor by id successfully kills the correct cursor, when multiple cursors are
// registered.
TEST_F(ClusterCursorManagerTest, KillCursorMultipleCursors) {
    const size_t numCursors = 10;
    std::vector<CursorId> cursorIds(numCursors);
    // Register cursors and populate 'cursorIds' with the returned cursor ids.
    for (size_t i = 0; i < numCursors; ++i) {
        cursorIds[i] =
            assertGet(getManager()->registerCursor(getOperationContext(),
                                                   allocateMockCursor(),
                                                   nss,
                                                   ClusterCursorManager::CursorType::SingleTarget,
                                                   ClusterCursorManager::CursorLifetime::Mortal,
                                                   boost::none));
    }
    // Kill each cursor and verify that it was successfully killed.
    for (size_t i = 0; i < numCursors; ++i) {
        ASSERT_OK(getManager()->killCursor(getOperationContext(), cursorIds[i]));
        ASSERT(isMockCursorKilled(i));
    }
}

// Test that killing an unknown cursor returns an error with code ErrorCodes::CursorNotFound.
TEST_F(ClusterCursorManagerTest, KillCursorUnknown) {
    Status killResult = getManager()->killCursor(getOperationContext(), 5);
    ASSERT_EQ(ErrorCodes::CursorNotFound, killResult);
}

// Test that killing an unknown cursor returns an error with code ErrorCodes::CursorNotFound,
// even if there is an existing cursor with the same namespace but a different cursor id.
TEST_F(ClusterCursorManagerTest, KillCursorWrongCursorId) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    Status killResult = getManager()->killCursor(getOperationContext(), cursorId + 1);
    ASSERT_EQ(ErrorCodes::CursorNotFound, killResult);
}

// Test that killing all mortal expired cursors correctly kills a mortal expired cursor.
TEST_F(ClusterCursorManagerTest, KillMortalCursorsInactiveSinceBasic) {
    ASSERT_OK(getManager()->registerCursor(getOperationContext(),
                                           allocateMockCursor(),
                                           nss,
                                           ClusterCursorManager::CursorType::SingleTarget,
                                           ClusterCursorManager::CursorLifetime::Mortal,
                                           boost::none));
    getManager()->killMortalCursorsInactiveSince(getOperationContext(), getClockSource()->now());
    ASSERT(isMockCursorKilled(0));
}

// Test that killing all mortal expired cursors does not kill a cursor that is unexpired.
TEST_F(ClusterCursorManagerTest, KillMortalCursorsInactiveSinceSkipUnexpired) {
    Date_t timeBeforeCursorCreation = getClockSource()->now();
    getClockSource()->advance(Milliseconds(1));
    ASSERT_OK(getManager()->registerCursor(getOperationContext(),
                                           allocateMockCursor(),
                                           nss,
                                           ClusterCursorManager::CursorType::SingleTarget,
                                           ClusterCursorManager::CursorLifetime::Mortal,
                                           boost::none));
    getManager()->killMortalCursorsInactiveSince(getOperationContext(), timeBeforeCursorCreation);
    ASSERT(!isMockCursorKilled(0));
}

// Test that killing all mortal expired cursors does not kill a cursor that is immortal.
TEST_F(ClusterCursorManagerTest, KillMortalCursorsInactiveSinceSkipImmortal) {
    ASSERT_OK(getManager()->registerCursor(getOperationContext(),
                                           allocateMockCursor(),
                                           nss,
                                           ClusterCursorManager::CursorType::SingleTarget,
                                           ClusterCursorManager::CursorLifetime::Immortal,
                                           boost::none));
    getManager()->killMortalCursorsInactiveSince(getOperationContext(), getClockSource()->now());
    ASSERT(!isMockCursorKilled(0));
}

// Test that killing all mortal expired cursors does not kill a mortal expired cursor that is
// pinned.
TEST_F(ClusterCursorManagerTest, ShouldNotKillPinnedCursors) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto pin = assertGet(
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker));
    getManager()->killMortalCursorsInactiveSince(getOperationContext(), getClockSource()->now());
    ASSERT(!isMockCursorKilled(0));
    pin.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    getManager()->killMortalCursorsInactiveSince(getOperationContext(), getClockSource()->now());
    ASSERT(isMockCursorKilled(0));
}

// Test that killing all mortal expired cursors kills the correct cursors when multiple cursors are
// registered.
TEST_F(ClusterCursorManagerTest, KillMortalCursorsInactiveSinceMultipleCursors) {
    const size_t numCursors = 10;
    const size_t numKilledCursorsExpected = 3;
    Date_t cutoff;
    for (size_t i = 0; i < numCursors; ++i) {
        if (i < numKilledCursorsExpected) {
            cutoff = getClockSource()->now();
        }
        ASSERT_OK(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
        getClockSource()->advance(Milliseconds(1));
    }
    getManager()->killMortalCursorsInactiveSince(getOperationContext(), cutoff);
    for (size_t i = 0; i < numCursors; ++i) {
        if (i < numKilledCursorsExpected) {
            ASSERT(isMockCursorKilled(i));
        } else {
            ASSERT(!isMockCursorKilled(i));
        }
    }
}

// Test that killMortalCursorsInactiveSince() increases cursorsTimeOut().
TEST_F(ClusterCursorManagerTest, KillMortalCursorsInactiveSinceCursorsTimedOut) {
    ASSERT_EQ(0ULL, getManager()->cursorsTimedOut());
    ASSERT_OK(getManager()->registerCursor(getOperationContext(),
                                           allocateMockCursor(),
                                           nss,
                                           ClusterCursorManager::CursorType::SingleTarget,
                                           ClusterCursorManager::CursorLifetime::Mortal,
                                           boost::none));
    ASSERT_EQ(1ULL,
              getManager()->killMortalCursorsInactiveSince(getOperationContext(),
                                                           getClockSource()->now()));
    ASSERT_EQ(1ULL, getManager()->cursorsTimedOut());
}

// Test that killing all cursors successfully kills all cursors.
TEST_F(ClusterCursorManagerTest, KillAllCursors) {
    const size_t numCursors = 10;
    for (size_t i = 0; i < numCursors; ++i) {
        ASSERT_OK(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    }
    getManager()->killAllCursors(getOperationContext());
    for (size_t i = 0; i < numCursors; ++i) {
        ASSERT(isMockCursorKilled(i));
    }
}

TEST_F(ClusterCursorManagerTest, KillCursorsSatisfyingAlwaysTrueKillsAllCursors) {
    const size_t numCursors = 10;
    for (size_t i = 0; i < numCursors; ++i) {
        ASSERT_OK(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    }
    auto pred = [](CursorId, const ClusterCursorManager::CursorEntry&) {
        return true;
    };
    auto nKilled = getManager()->killCursorsSatisfying(getOperationContext(), std::move(pred));
    ASSERT_EQ(nKilled, numCursors);
    for (size_t i = 0; i < numCursors; ++i) {
        ASSERT(isMockCursorKilled(i));
    }
}

TEST_F(ClusterCursorManagerTest, KillCursorsSatisfyingAlwaysFalseKillsNoCursors) {
    const size_t numCursors = 10;
    for (size_t i = 0; i < numCursors; ++i) {
        ASSERT_OK(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    }
    auto pred = [](CursorId, const ClusterCursorManager::CursorEntry&) {
        return false;
    };
    auto nKilled = getManager()->killCursorsSatisfying(getOperationContext(), std::move(pred));
    ASSERT_EQ(nKilled, 0);
    for (size_t i = 0; i < numCursors; ++i) {
        ASSERT(!isMockCursorKilled(i));
    }
}

TEST_F(ClusterCursorManagerTest, KillCursorsSatisfyingOnlyKillsMatchingSubset) {
    const size_t numCursors = 10;
    stdx::unordered_set<CursorId> idsToKill;
    auto shouldKillCursor = [](size_t idx) {
        return idx % 2 == 0;
    };
    for (size_t i = 0; i < numCursors; ++i) {
        auto swCursorId =
            getManager()->registerCursor(getOperationContext(),
                                         allocateMockCursor(),
                                         nss,
                                         ClusterCursorManager::CursorType::SingleTarget,
                                         ClusterCursorManager::CursorLifetime::Mortal,
                                         boost::none);
        ASSERT_OK(swCursorId);
        if (shouldKillCursor(i))
            idsToKill.insert(swCursorId.getValue());
    }
    auto pred = [&](CursorId id, const ClusterCursorManager::CursorEntry&) {
        return idsToKill.contains(id);
    };
    auto nKilled = getManager()->killCursorsSatisfying(getOperationContext(), std::move(pred));
    ASSERT_EQ(nKilled, numCursors / 2);
    for (size_t i = 0; i < numCursors; ++i) {
        if (shouldKillCursor(i))
            ASSERT(isMockCursorKilled(i));
        else
            ASSERT(!isMockCursorKilled(i));
    }
}

// Tests that we can kill cusors based on their opkey.
TEST_F(ClusterCursorManagerTest, KillCursorsSatisfyingBasedOnOpKey) {
    const size_t numCursors = 10;
    for (size_t i = 0; i < numCursors; ++i) {
        ASSERT_OK(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    }
    auto pred = [&](CursorId id, const ClusterCursorManager::CursorEntry& entry) {
        return entry.getOperationKey() == getOperationContext()->getOperationKey();
    };
    auto nKilled = getManager()->killCursorsSatisfying(getOperationContext(), std::move(pred));
    ASSERT_EQ(nKilled, numCursors);
    for (size_t i = 0; i < numCursors; ++i) {
        ASSERT(isMockCursorKilled(i));
    }
}

// Test that the Client that registered a cursor is correctly recorded.
TEST_F(ClusterCursorManagerTest, CorrectlyRecordsOriginatingClient) {
    ASSERT_OK(getManager()->registerCursor(getOperationContext(),
                                           allocateMockCursor(),
                                           nss,
                                           ClusterCursorManager::CursorType::MultiTarget,
                                           ClusterCursorManager::CursorLifetime::Mortal,
                                           boost::none));
    // Now insert some cursors under a different client.
    const size_t numAltClientCursors = 10;
    {
        auto otherClient = getServiceContext()->makeClient("otherClient");
        auto otherOpCtx = otherClient->makeOperationContext();
        AlternativeClientRegion acr(otherClient);
        for (size_t i = 0; i < numAltClientCursors; ++i) {
            ASSERT_OK(getManager()->registerCursor(otherOpCtx.get(),
                                                   allocateMockCursor(),
                                                   nss,
                                                   ClusterCursorManager::CursorType::MultiTarget,
                                                   ClusterCursorManager::CursorLifetime::Mortal,
                                                   boost::none));
        }
    }

    auto pred = [client = getClient()](CursorId id,
                                       const ClusterCursorManager::CursorEntry& entry) {
        return entry.originatingClientUuid() == client->getUUID();
    };
    // Ensure we only matched the initial client, not the alternate one.
    auto nKilled = getManager()->killCursorsSatisfying(getOperationContext(), std::move(pred));
    ASSERT_EQ(nKilled, 1);
    ASSERT(isMockCursorKilled(0));
    for (size_t i = 1; i < numAltClientCursors + 1; ++i) {
        ASSERT(!isMockCursorKilled(i));
    }
}

// Test that a new ClusterCursorManager's stats() is initially zero for the cursor counts.
TEST_F(ClusterCursorManagerTest, StatsInitAsZero) {
    ASSERT_EQ(0U, getManager()->stats().cursorsMultiTarget);
    ASSERT_EQ(0U, getManager()->stats().cursorsSingleTarget);
    ASSERT_EQ(0U, getManager()->stats().cursorsPinned);
}

// Test that registering a sharded cursor updates the corresponding counter in stats().
TEST_F(ClusterCursorManagerTest, StatsRegisterShardedCursor) {
    ASSERT_OK(getManager()->registerCursor(getOperationContext(),
                                           allocateMockCursor(),
                                           nss,
                                           ClusterCursorManager::CursorType::MultiTarget,
                                           ClusterCursorManager::CursorLifetime::Mortal,
                                           boost::none));
    ASSERT_EQ(1U, getManager()->stats().cursorsMultiTarget);
}

// Test that registering a not-sharded cursor updates the corresponding counter in stats().
TEST_F(ClusterCursorManagerTest, StatsRegisterNotShardedCursor) {
    ASSERT_OK(getManager()->registerCursor(getOperationContext(),
                                           allocateMockCursor(),
                                           nss,
                                           ClusterCursorManager::CursorType::SingleTarget,
                                           ClusterCursorManager::CursorLifetime::Mortal,
                                           boost::none));
    ASSERT_EQ(1U, getManager()->stats().cursorsSingleTarget);
}

// Test that checking out a cursor updates the pinned counter in stats().
TEST_F(ClusterCursorManagerTest, StatsPinCursor) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::MultiTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto pinnedCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_EQ(1U, getManager()->stats().cursorsPinned);
}

// Test that registering multiple sharded and not-sharded cursors updates the corresponding
// counters in stats().
TEST_F(ClusterCursorManagerTest, StatsRegisterMultipleCursors) {
    const size_t numShardedCursors = 10;
    for (size_t i = 0; i < numShardedCursors; ++i) {
        ASSERT_OK(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::MultiTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
        ASSERT_EQ(i + 1, getManager()->stats().cursorsMultiTarget);
        ASSERT_EQ(0U, getManager()->stats().cursorsSingleTarget);
    }
    const size_t numNotShardedCursors = 10;
    for (size_t i = 0; i < numNotShardedCursors; ++i) {
        ASSERT_OK(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
        ASSERT_EQ(numShardedCursors, getManager()->stats().cursorsMultiTarget);
        ASSERT_EQ(i + 1, getManager()->stats().cursorsSingleTarget);
    }
}

// Test that killing a sharded cursor decrements the corresponding counter in stats().
TEST_F(ClusterCursorManagerTest, StatsKillShardedCursor) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::MultiTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    ASSERT_EQ(1U, getManager()->stats().cursorsMultiTarget);
    ASSERT_OK(getManager()->killCursor(getOperationContext(), cursorId));
    ASSERT_EQ(0U, getManager()->stats().cursorsMultiTarget);
}

// Test that killing a not-sharded cursor decrements the corresponding counter in stats().
TEST_F(ClusterCursorManagerTest, StatsKillNotShardedCursor) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    ASSERT_EQ(1U, getManager()->stats().cursorsSingleTarget);
    ASSERT_OK(getManager()->killCursor(getOperationContext(), cursorId));
    ASSERT_EQ(0U, getManager()->stats().cursorsSingleTarget);
}

// Test that killing a pinned cursor decrements the corresponding counter in stats().
TEST_F(ClusterCursorManagerTest, StatsKillPinnedCursor) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::MultiTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto pinnedCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_EQ(1U, getManager()->stats().cursorsPinned);

    killCursorFromDifferentOpCtx(cursorId);

    ASSERT_EQ(getOperationContext()->checkForInterruptNoAssert(), ErrorCodes::CursorKilled);
    ASSERT_EQ(0U, getManager()->stats().cursorsPinned);
}

// Test that exhausting a sharded cursor decrements the corresponding counter in stats().
TEST_F(ClusterCursorManagerTest, StatsExhaustShardedCursor) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::MultiTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto pinnedCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_OK(pinnedCursor.getStatus());
    ASSERT_OK(pinnedCursor.getValue()->next().getStatus());
    ASSERT_EQ(1U, getManager()->stats().cursorsMultiTarget);
    pinnedCursor.getValue().returnCursor(ClusterCursorManager::CursorState::Exhausted);
    ASSERT_EQ(0U, getManager()->stats().cursorsMultiTarget);
}

// Test that exhausting a not-sharded cursor decrements the corresponding counter in stats().
TEST_F(ClusterCursorManagerTest, StatsExhaustNotShardedCursor) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto pinnedCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_OK(pinnedCursor.getStatus());
    ASSERT_OK(pinnedCursor.getValue()->next().getStatus());
    ASSERT_EQ(1U, getManager()->stats().cursorsSingleTarget);
    pinnedCursor.getValue().returnCursor(ClusterCursorManager::CursorState::Exhausted);
    ASSERT_EQ(0U, getManager()->stats().cursorsSingleTarget);
}

// Test that checking a pinned cursor in as exhausted decrements the corresponding counter in
// stats().
TEST_F(ClusterCursorManagerTest, StatsExhaustPinnedCursor) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto pinnedCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_OK(pinnedCursor.getStatus());
    ASSERT_OK(pinnedCursor.getValue()->next().getStatus());
    ASSERT_EQ(1U, getManager()->stats().cursorsPinned);
    pinnedCursor.getValue().returnCursor(ClusterCursorManager::CursorState::Exhausted);
    ASSERT_EQ(0U, getManager()->stats().cursorsPinned);
}

// Test that checking a pinned cursor in as *not* exhausted decrements the corresponding counter in
// stats().
TEST_F(ClusterCursorManagerTest, StatsCheckInWithoutExhaustingPinnedCursor) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto pinnedCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_OK(pinnedCursor.getStatus());
    ASSERT_OK(pinnedCursor.getValue()->next().getStatus());
    ASSERT_EQ(1U, getManager()->stats().cursorsPinned);
    pinnedCursor.getValue().returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT_EQ(0U, getManager()->stats().cursorsPinned);
}

// Test that the PinnedCursor default constructor creates a pin that owns no cursor.
TEST_F(ClusterCursorManagerTest, PinnedCursorDefaultConstructor) {
    ClusterCursorManager::PinnedCursor pinnedCursor;
    ASSERT_EQ(0, pinnedCursor.getCursorId());
}

// Test that returning a pinned cursor correctly unpins the cursor, and leaves the pin owning no
// cursor.
TEST_F(ClusterCursorManagerTest, PinnedCursorReturnCursorNotExhausted) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto registeredCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_OK(registeredCursor.getStatus());
    ASSERT_EQ(cursorId, registeredCursor.getValue().getCursorId());
    ASSERT_NE(0, cursorId);
    registeredCursor.getValue().returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT_EQ(0, registeredCursor.getValue().getCursorId());
    auto checkedOutCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_OK(checkedOutCursor.getStatus());
}

// Test that returning a pinned cursor with 'Exhausted' correctly de-registers and destroys the
// cursor, and leaves the pin owning no cursor.
TEST_F(ClusterCursorManagerTest, PinnedCursorReturnCursorExhausted) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto registeredCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_OK(registeredCursor.getStatus());
    ASSERT_EQ(cursorId, registeredCursor.getValue().getCursorId());
    ASSERT_NE(0, cursorId);
    ASSERT_OK(registeredCursor.getValue()->next().getStatus());
    registeredCursor.getValue().returnCursor(ClusterCursorManager::CursorState::Exhausted);
    ASSERT_EQ(0, registeredCursor.getValue().getCursorId());

    // Cursor should have been killed and destroyed.
    ASSERT_NOT_OK(getManager()
                      ->checkOutCursor(cursorId, getOperationContext(), successAuthChecker)
                      .getStatus());
    ASSERT(isMockCursorKilled(0));
}

// Test that when a cursor is returned as exhausted but is still managing non-exhausted remote
// cursors, the cursor is destroyed immediately.
TEST_F(ClusterCursorManagerTest, PinnedCursorReturnCursorExhaustedWithNonExhaustedRemotes) {
    auto mockCursor = allocateMockCursor();

    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               std::move(mockCursor),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto registeredCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_OK(registeredCursor.getStatus());
    ASSERT_EQ(cursorId, registeredCursor.getValue().getCursorId());
    ASSERT_NE(0, cursorId);
    ASSERT_OK(registeredCursor.getValue()->next().getStatus());
    registeredCursor.getValue().returnCursor(ClusterCursorManager::CursorState::Exhausted);
    ASSERT_EQ(0, registeredCursor.getValue().getCursorId());

    // Cursor should be killed as soon as it's checked in.
    ASSERT(isMockCursorKilled(0));
    ASSERT_NOT_OK(getManager()
                      ->checkOutCursor(cursorId, getOperationContext(), successAuthChecker)
                      .getStatus());
}

// Test that the PinnedCursor move assignment operator correctly kills the cursor if it has not yet
// been returned.
TEST_F(ClusterCursorManagerTest, PinnedCursorMoveAssignmentKill) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto pinnedCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    pinnedCursor = ClusterCursorManager::PinnedCursor();
    ASSERT(isMockCursorKilled(0));
}

// Test that the PinnedCursor destructor correctly kills the cursor if it has not yet been returned.
TEST_F(ClusterCursorManagerTest, PinnedCursorDestructorKill) {
    {
        auto cursorId =
            assertGet(getManager()->registerCursor(getOperationContext(),
                                                   allocateMockCursor(),
                                                   nss,
                                                   ClusterCursorManager::CursorType::SingleTarget,
                                                   ClusterCursorManager::CursorLifetime::Mortal,
                                                   boost::none));
        auto pinnedCursor =
            getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    }
    ASSERT(isMockCursorKilled(0));
}

// Test that PinnedCursor::remotesExhausted() correctly forwards to the underlying mock cursor.
TEST_F(ClusterCursorManagerTest, RemotesExhausted) {
    auto mockCursor = allocateMockCursor();
    ASSERT_FALSE(mockCursor->remotesExhausted());

    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               std::move(mockCursor),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto pinnedCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_OK(pinnedCursor.getStatus());
    ASSERT_FALSE(pinnedCursor.getValue()->remotesExhausted());
}

// Test that killed cursors which are still pinned are not destroyed immediately.
TEST_F(ClusterCursorManagerTest, DoNotDestroyKilledPinnedCursors) {
    const Date_t cutoff = getClockSource()->now();
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto pinnedCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_OK(pinnedCursor.getStatus());

    killCursorFromDifferentOpCtx(cursorId);

    ASSERT_EQ(getOperationContext()->checkForInterruptNoAssert(), ErrorCodes::CursorKilled);
    ASSERT(!isMockCursorKilled(0));

    // The cursor cleanup system should not destroy the cursor either.
    getManager()->killMortalCursorsInactiveSince(getOperationContext(), cutoff);

    // The cursor's operation context should be marked as interrupted, but the cursor itself should
    // not have been destroyed.
    ASSERT(!isMockCursorKilled(0));

    // The cursor can be destroyed once it is returned to the manager.
    pinnedCursor.getValue().returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT(isMockCursorKilled(0));
}

// Test that a cursor correctly stores API parameters.
TEST_F(ClusterCursorManagerTest, CursorStoresAPIParameters) {
    APIParameters apiParams = APIParameters();
    apiParams.setAPIVersion("2");
    apiParams.setAPIStrict(true);
    apiParams.setAPIDeprecationErrors(true);

    auto cursor = allocateMockCursor();
    cursor->setAPIParameters(apiParams);

    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               std::move(cursor),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto pinnedCursor = assertGet(
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker));

    auto storedAPIParams = pinnedCursor->getAPIParameters();
    ASSERT_EQ("2", *storedAPIParams.getAPIVersion());
    ASSERT_TRUE(*storedAPIParams.getAPIStrict());
    ASSERT_TRUE(*storedAPIParams.getAPIDeprecationErrors());
}

TEST_F(ClusterCursorManagerTest, CannotRegisterCursorDuringShutdown) {
    ASSERT_OK(getManager()->registerCursor(getOperationContext(),
                                           allocateMockCursor(),
                                           nss,
                                           ClusterCursorManager::CursorType::SingleTarget,
                                           ClusterCursorManager::CursorLifetime::Mortal,
                                           boost::none));
    ASSERT(!isMockCursorKilled(0));

    getManager()->shutdown(getOperationContext());

    ASSERT(isMockCursorKilled(0));

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress,
                  getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
}

TEST_F(ClusterCursorManagerTest, PinnedCursorNotKilledOnShutdown) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));

    auto pinnedCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    getManager()->shutdown(getOperationContext());

    ASSERT_EQ(getOperationContext()->checkForInterruptNoAssert(), ErrorCodes::CursorKilled);
    ASSERT(!isMockCursorKilled(0));

    // Even if it's checked back in as not exhausted, it should have been marked as killed when
    // shutdown() was called.
    pinnedCursor.getValue().returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT(isMockCursorKilled(0));
}

TEST_F(ClusterCursorManagerTest, CannotCheckoutCursorDuringShutdown) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    ASSERT(!isMockCursorKilled(0));

    getManager()->shutdown(getOperationContext());

    ASSERT(isMockCursorKilled(0));

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress,
                  getManager()
                      ->checkOutCursor(cursorId, getOperationContext(), successAuthChecker)
                      .getStatus());
}

/**
 * Test that a manager whose cursors do not have sessions does not return them.
 */
TEST_F(ClusterCursorManagerTest, CursorsWithoutSessions) {
    // Add a cursor with no session to the cursor manager.
    ASSERT_OK(getManager()->registerCursor(getOperationContext(),
                                           allocateMockCursor(),
                                           nss,
                                           ClusterCursorManager::CursorType::SingleTarget,
                                           ClusterCursorManager::CursorLifetime::Mortal,
                                           boost::none));

    // Manager should have no active sessions.
    LogicalSessionIdSet lsids;
    getManager()->appendActiveSessions(&lsids);
    ASSERT_EQ(lsids.size(), size_t(0));
}

/**
 * Test a manager that has one cursor running inside of a session.
 */
TEST_F(ClusterCursorManagerTest, OneCursorWithASession) {
    // Add a cursor with a session to the cursor manager.
    auto lsid = makeLogicalSessionIdForTest();
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(lsid),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));

    // Retrieve all sessions active in manager - set should contain just lsid.
    LogicalSessionIdSet lsids;
    getManager()->appendActiveSessions(&lsids);
    ASSERT_EQ(lsids.size(), size_t(1));
    ASSERT(lsids.find(lsid) != lsids.end());

    // Retrieve all cursors for this lsid - should be just ours.
    auto cursors = getManager()->getCursorsForSession(lsid);
    ASSERT_EQ(cursors.size(), size_t(1));
    ASSERT(cursors.find(cursorId) != cursors.end());

    // Remove the cursor from the manager.
    ASSERT_OK(getManager()->killCursor(getOperationContext(), cursorId));

    // There should be no more cursor entries by session id.
    LogicalSessionIdSet sessions;
    getManager()->appendActiveSessions(&sessions);
    ASSERT(sessions.empty());
    ASSERT(getManager()->getCursorsForSession(lsid).empty());
}

/**
 * Test getting the lsid of a cursor while it is checked out of the manager.
 */
TEST_F(ClusterCursorManagerTest, GetSessionIdsWhileCheckedOut) {
    // Add a cursor with a session to the cursor manager.
    auto lsid = makeLogicalSessionIdForTest();
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(lsid),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));

    // Check the cursor out, then try to append cursors, see that we get one.
    auto res = getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT(res.isOK());

    auto cursors = getManager()->getCursorsForSession(lsid);
    ASSERT_EQ(cursors.size(), size_t(1));
}

/**
 * Test a manager with multiple cursors running inside of the same session.
 */
TEST_F(ClusterCursorManagerTest, MultipleCursorsWithSameSession) {
    // Add two cursors on the same session to the cursor manager.
    auto lsid = makeLogicalSessionIdForTest();
    auto cursorId1 =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(lsid),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    auto cursorId2 =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(lsid),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));

    // Retrieve all sessions - set should contain just lsid.
    stdx::unordered_set<LogicalSessionId, LogicalSessionIdHash> lsids;
    getManager()->appendActiveSessions(&lsids);
    ASSERT_EQ(lsids.size(), size_t(1));
    ASSERT(lsids.find(lsid) != lsids.end());

    // Retrieve all cursors for session - should be both cursors.
    auto cursors = getManager()->getCursorsForSession(lsid);
    ASSERT_EQ(cursors.size(), size_t(2));
    ASSERT(cursors.find(cursorId1) != cursors.end());
    ASSERT(cursors.find(cursorId2) != cursors.end());

    // Remove one cursor from the manager.
    ASSERT_OK(getManager()->killCursor(getOperationContext(), cursorId1));

    // Should still be able to retrieve the session.
    lsids.clear();
    getManager()->appendActiveSessions(&lsids);
    ASSERT_EQ(lsids.size(), size_t(1));
    ASSERT(lsids.find(lsid) != lsids.end());

    // Should still be able to retrieve remaining cursor by session.
    cursors = getManager()->getCursorsForSession(lsid);
    ASSERT_EQ(cursors.size(), size_t(1));
    ASSERT(cursors.find(cursorId2) != cursors.end());
}

/**
 * Test a manager with multiple cursors running inside of different sessions.
 */
TEST_F(ClusterCursorManagerTest, MultipleCursorsMultipleSessions) {
    auto lsid1 = makeLogicalSessionIdForTest();
    auto lsid2 = makeLogicalSessionIdForTest();

    // Register two cursors with different lsids, and one without.
    CursorId cursor1 =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(lsid1),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));

    CursorId cursor2 =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(lsid2),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));

    ASSERT_OK(getManager()->registerCursor(getOperationContext(),
                                           allocateMockCursor(),
                                           nss,
                                           ClusterCursorManager::CursorType::SingleTarget,
                                           ClusterCursorManager::CursorLifetime::Mortal,
                                           boost::none));

    // Retrieve all sessions - should be both lsids.
    LogicalSessionIdSet lsids;
    getManager()->appendActiveSessions(&lsids);
    ASSERT_EQ(lsids.size(), size_t(2));
    ASSERT(lsids.find(lsid1) != lsids.end());
    ASSERT(lsids.find(lsid2) != lsids.end());

    // Retrieve cursors for each session - should be just one.
    auto cursors1 = getManager()->getCursorsForSession(lsid1);
    ASSERT_EQ(cursors1.size(), size_t(1));
    ASSERT(cursors1.find(cursor1) != cursors1.end());

    auto cursors2 = getManager()->getCursorsForSession(lsid2);
    ASSERT_EQ(cursors2.size(), size_t(1));
    ASSERT(cursors2.find(cursor2) != cursors2.end());
}

/**
 * Test a manager with many cursors running inside of different sessions.
 */
TEST_F(ClusterCursorManagerTest, ManyCursorsManySessions) {
    const int count = 10000;
    for (int i = 0; i < count; i++) {
        auto lsid = makeLogicalSessionIdForTest();
        ASSERT_OK(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(lsid),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));
    }

    // Retrieve all sessions.
    LogicalSessionIdSet lsids;
    getManager()->appendActiveSessions(&lsids);
    ASSERT_EQ(lsids.size(), size_t(count));
}

TEST_F(ClusterCursorManagerTest, CheckAuthForKillCursors) {
    auto cursorId =
        assertGet(getManager()->registerCursor(getOperationContext(),
                                               allocateMockCursor(),
                                               nss,
                                               ClusterCursorManager::CursorType::SingleTarget,
                                               ClusterCursorManager::CursorLifetime::Mortal,
                                               boost::none));

    ASSERT_EQ(ErrorCodes::CursorNotFound,
              getManager()->checkAuthForKillCursors(
                  getOperationContext(), cursorId + 1, successAuthChecker));
    ASSERT_EQ(
        ErrorCodes::Unauthorized,
        getManager()->checkAuthForKillCursors(getOperationContext(), cursorId, failAuthChecker));
    ASSERT_OK(
        getManager()->checkAuthForKillCursors(getOperationContext(), cursorId, successAuthChecker));
}

TEST_F(ClusterCursorManagerTest, PinnedCursorReturnsUnderlyingCursorTxnNumber) {
    const TxnNumber txnNumber = 5;
    auto cursorId = assertGet(
        getManager()->registerCursor(getOperationContext(),
                                     allocateMockCursor(makeLogicalSessionIdForTest(), txnNumber),
                                     nss,
                                     ClusterCursorManager::CursorType::SingleTarget,
                                     ClusterCursorManager::CursorLifetime::Mortal,
                                     boost::none));

    auto pinnedCursor =
        getManager()->checkOutCursor(cursorId, getOperationContext(), successAuthChecker);
    ASSERT_OK(pinnedCursor.getStatus());

    // The underlying cursor's txnNumber should be returned.
    ASSERT(pinnedCursor.getValue()->getTxnNumber());
    ASSERT_EQ(txnNumber, *pinnedCursor.getValue()->getTxnNumber());
}

}  // namespace

}  // namespace mongo
