/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/query/cluster_cursor_manager.h"

#include <vector>

#include "mongo/s/query/cluster_client_cursor_mock.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

const NamespaceString nss("test.collection");

// Test that registering a cursor returns a pin to the same cursor.
TEST(ClusterCursorManagerTest, RegisterCursor) {
    ClusterCursorManager manager;
    auto cursor = stdx::make_unique<ClusterClientCursorMock>();
    cursor->queueResult(BSON("a" << 1));
    auto pinnedCursor =
        manager.registerCursor(std::move(cursor),
                               nss,
                               ClusterCursorManager::CursorType::NamespaceNotSharded,
                               ClusterCursorManager::CursorLifetime::Mortal);
    auto nextResult = pinnedCursor.next();
    ASSERT_OK(nextResult.getStatus());
    ASSERT(nextResult.getValue());
    ASSERT_EQ(BSON("a" << 1), *nextResult.getValue());
    nextResult = pinnedCursor.next();
    ASSERT_OK(nextResult.getStatus());
    ASSERT_FALSE(nextResult.getValue());

    // Clean up (return the exhausted cursor).
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::Exhausted);
}

// Test that checking out a cursor returns a pin to the correct cursor.
TEST(ClusterCursorManagerTest, CheckOutCursorBasic) {
    ClusterCursorManager manager;
    auto cursor = stdx::make_unique<ClusterClientCursorMock>();
    cursor->queueResult(BSON("a" << 1));
    auto registeredCursor =
        manager.registerCursor(std::move(cursor),
                               nss,
                               ClusterCursorManager::CursorType::NamespaceNotSharded,
                               ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = registeredCursor.getCursorId();
    registeredCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    auto checkedOutCursor = manager.checkOutCursor(nss, cursorId);
    ASSERT_OK(checkedOutCursor.getStatus());
    auto nextResult = checkedOutCursor.getValue().next();
    ASSERT_OK(nextResult.getStatus());
    ASSERT(nextResult.getValue());
    ASSERT_EQ(BSON("a" << 1), *nextResult.getValue());
    nextResult = checkedOutCursor.getValue().next();
    ASSERT_OK(nextResult.getStatus());
    ASSERT_FALSE(nextResult.getValue());

    // Clean up (return the exhausted cursor).
    checkedOutCursor.getValue().returnCursor(ClusterCursorManager::CursorState::Exhausted);
}

// Test that checking out a cursor returns a pin to the correct cursor, when multiple cursors are
// registered.
TEST(ClusterCursorManagerTest, CheckOutCursorMultipleCursors) {
    ClusterCursorManager manager;
    const int numCursors = 10;
    std::vector<CursorId> cursorIds(numCursors);
    for (int i = 0; i < numCursors; ++i) {
        auto cursor = stdx::make_unique<ClusterClientCursorMock>();
        cursor->queueResult(BSON("a" << i));
        auto pinnedCursor =
            manager.registerCursor(std::move(cursor),
                                   nss,
                                   ClusterCursorManager::CursorType::NamespaceNotSharded,
                                   ClusterCursorManager::CursorLifetime::Mortal);
        cursorIds[i] = pinnedCursor.getCursorId();
        pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    }
    for (int i = 0; i < numCursors; ++i) {
        auto pinnedCursor = manager.checkOutCursor(nss, cursorIds[i]);
        ASSERT_OK(pinnedCursor.getStatus());
        auto nextResult = pinnedCursor.getValue().next();
        ASSERT_OK(nextResult.getStatus());
        ASSERT(nextResult.getValue());
        ASSERT_EQ(BSON("a" << i), *nextResult.getValue());
        nextResult = pinnedCursor.getValue().next();
        ASSERT_OK(nextResult.getStatus());
        ASSERT_FALSE(nextResult.getValue());

        // Clean up (return the exhausted cursor).
        pinnedCursor.getValue().returnCursor(ClusterCursorManager::CursorState::Exhausted);
    }
}

// Test that checking out a pinned cursor returns an error with code ErrorCodes::CursorInUse.
TEST(ClusterCursorManagerTest, CheckOutCursorPinned) {
    ClusterCursorManager manager;
    auto pinnedCursor =
        manager.registerCursor(stdx::make_unique<ClusterClientCursorMock>(),
                               nss,
                               ClusterCursorManager::CursorType::NamespaceNotSharded,
                               ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = pinnedCursor.getCursorId();
    ASSERT_EQ(ErrorCodes::CursorInUse, manager.checkOutCursor(nss, cursorId).getStatus());

    // Clean up (exhaust and return the cursor).
    ASSERT_OK(pinnedCursor.next().getStatus());
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::Exhausted);
}

// Test that checking out a killed cursor returns an error with code ErrorCodes::CursorNotFound.
TEST(ClusterCursorManagerTest, CheckOutCursorKilled) {
    ClusterCursorManager manager;
    auto pinnedCursor =
        manager.registerCursor(stdx::make_unique<ClusterClientCursorMock>(),
                               nss,
                               ClusterCursorManager::CursorType::NamespaceNotSharded,
                               ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = pinnedCursor.getCursorId();
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT_OK(manager.killCursor(nss, cursorId));
    ASSERT_EQ(ErrorCodes::CursorNotFound, manager.checkOutCursor(nss, cursorId).getStatus());

    // Clean up (reap the zombie cursor).
    manager.reapZombieCursors();
}

// Test that checking out an unknown cursor returns an error with code ErrorCodes::CursorNotFound.
TEST(ClusterCursorManagerTest, CheckOutCursorUnknown) {
    ClusterCursorManager manager;
    ASSERT_EQ(ErrorCodes::CursorNotFound, manager.checkOutCursor(nss, 5).getStatus());
}

// Test that checking out a unknown cursor returns an error with code ErrorCodes::CursorNotFound,
// even if there is an existing cursor with the same cursor id but a different namespace.
TEST(ClusterCursorManagerTest, CheckOutCursorWrongNamespace) {
    ClusterCursorManager manager;
    const NamespaceString correctNamespace("test.correct");
    const NamespaceString incorrectNamespace("test.incorrect");
    auto registeredCursor =
        manager.registerCursor(stdx::make_unique<ClusterClientCursorMock>(),
                               correctNamespace,
                               ClusterCursorManager::CursorType::NamespaceNotSharded,
                               ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = registeredCursor.getCursorId();
    registeredCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT_EQ(ErrorCodes::CursorNotFound,
              manager.checkOutCursor(incorrectNamespace, cursorId).getStatus());

    // Clean up (kill and reap the cursor).
    ASSERT_OK(manager.killCursor(correctNamespace, cursorId));
    manager.reapZombieCursors();
}

// Test that checking out a unknown cursor returns an error with code ErrorCodes::CursorNotFound,
// even if there is an existing cursor with the same namespace but a different cursor id.
TEST(ClusterCursorManagerTest, CheckOutCursorWrongCursorId) {
    ClusterCursorManager manager;
    auto registeredCursor =
        manager.registerCursor(stdx::make_unique<ClusterClientCursorMock>(),
                               nss,
                               ClusterCursorManager::CursorType::NamespaceNotSharded,
                               ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = registeredCursor.getCursorId();
    registeredCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT_EQ(ErrorCodes::CursorNotFound, manager.checkOutCursor(nss, cursorId + 1).getStatus());

    // Clean up (kill and reap the cursor).
    ASSERT_OK(manager.killCursor(nss, cursorId));
    manager.reapZombieCursors();
}

// Test that killing a cursor by id successfully kills the cursor.
TEST(ClusterCursorManagerTest, KillCursorBasic) {
    ClusterCursorManager manager;
    bool killed = false;
    auto cursor = stdx::make_unique<ClusterClientCursorMock>([&killed]() { killed = true; });
    auto pinnedCursor =
        manager.registerCursor(std::move(cursor),
                               nss,
                               ClusterCursorManager::CursorType::NamespaceNotSharded,
                               ClusterCursorManager::CursorLifetime::Mortal);
    ASSERT_OK(manager.killCursor(nss, pinnedCursor.getCursorId()));
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT(!killed);
    manager.reapZombieCursors();
    ASSERT(killed);
}

// Test that killing a cursor by id successfully kills the correct cursor, when multiple cursors are
// registered.
TEST(ClusterCursorManagerTest, KillCursorMultipleCursors) {
    ClusterCursorManager manager;
    const size_t numCursors = 10;
    std::vector<CursorId> cursorIds(numCursors);
    // Register cursors and populate 'cursorIds' with the returned cursor ids.
    for (size_t i = 0; i < numCursors; ++i) {
        CursorId& cursorId = cursorIds[i];
        auto cursor = stdx::make_unique<ClusterClientCursorMock>([&cursorId]() { cursorId = 0; });
        auto pinnedCursor =
            manager.registerCursor(std::move(cursor),
                                   nss,
                                   ClusterCursorManager::CursorType::NamespaceNotSharded,
                                   ClusterCursorManager::CursorLifetime::Mortal);
        cursorId = pinnedCursor.getCursorId();
        pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    }
    // Kill each cursor and verify that its entry in 'cursorIds' was correctly zeroed.
    for (size_t i = 0; i < numCursors; ++i) {
        ASSERT_OK(manager.killCursor(nss, cursorIds[i]));
        ASSERT(cursorIds[i]);
        manager.reapZombieCursors();
        ASSERT(!cursorIds[i]);
    }
}

// Test that killing an unknown cursor returns an error with code ErrorCodes::CursorNotFound.
TEST(ClusterCursorManagerTest, KillCursorUnknown) {
    ClusterCursorManager manager;
    Status killResult = manager.killCursor(nss, 5);
    ASSERT_EQ(ErrorCodes::CursorNotFound, killResult);
}

// Test that killing an unknown cursor returns an error with code ErrorCodes::CursorNotFound,
// even if there is an existing cursor with the same cursor id but a different namespace.
TEST(ClusterCursorManagerTest, KillCursorWrongNamespace) {
    ClusterCursorManager manager;
    const NamespaceString correctNamespace("test.correct");
    const NamespaceString incorrectNamespace("test.incorrect");
    auto registeredCursor =
        manager.registerCursor(stdx::make_unique<ClusterClientCursorMock>(),
                               correctNamespace,
                               ClusterCursorManager::CursorType::NamespaceNotSharded,
                               ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = registeredCursor.getCursorId();
    registeredCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    Status killResult = manager.killCursor(incorrectNamespace, cursorId);
    ASSERT_EQ(ErrorCodes::CursorNotFound, killResult);

    // Clean up (kill and reap the cursor).
    ASSERT_OK(manager.killCursor(correctNamespace, cursorId));
    manager.reapZombieCursors();
}

// Test that killing an unknown cursor returns an error with code ErrorCodes::CursorNotFound,
// even if there is an existing cursor with the same namespace but a different cursor id.
TEST(ClusterCursorManagerTest, KillCursorWrongCursorId) {
    ClusterCursorManager manager;
    auto registeredCursor =
        manager.registerCursor(stdx::make_unique<ClusterClientCursorMock>(),
                               nss,
                               ClusterCursorManager::CursorType::NamespaceNotSharded,
                               ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = registeredCursor.getCursorId();
    registeredCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    Status killResult = manager.killCursor(nss, cursorId + 1);
    ASSERT_EQ(ErrorCodes::CursorNotFound, killResult);

    // Clean up (kill and reap the cursor).
    ASSERT_OK(manager.killCursor(nss, cursorId));
    manager.reapZombieCursors();
}

// Test that killing all cursors successfully kills all cursors.
TEST(ClusterCursorManagerTest, KillAllCursors) {
    ClusterCursorManager manager;
    size_t killCount = 0;
    const size_t numCursors = 10;
    for (size_t i = 0; i < numCursors; ++i) {
        auto cursor = stdx::make_unique<ClusterClientCursorMock>([&killCount]() { ++killCount; });
        auto pinnedCursor =
            manager.registerCursor(std::move(cursor),
                                   nss,
                                   ClusterCursorManager::CursorType::NamespaceNotSharded,
                                   ClusterCursorManager::CursorLifetime::Mortal);
        pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    }
    manager.killAllCursors();
    ASSERT_EQ(0U, killCount);
    manager.reapZombieCursors();
    ASSERT_EQ(numCursors, killCount);
}

// Test that reaping correctly calls kill() on the underlying ClusterClientCursor for a killed
// cursor.
TEST(ClusterCursorManagerTest, ReapZombieCursorsBasic) {
    ClusterCursorManager manager;
    bool killed = false;
    auto cursor = stdx::make_unique<ClusterClientCursorMock>([&killed]() { killed = true; });
    auto pinnedCursor =
        manager.registerCursor(std::move(cursor),
                               nss,
                               ClusterCursorManager::CursorType::NamespaceNotSharded,
                               ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = pinnedCursor.getCursorId();
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT_OK(manager.killCursor(nss, cursorId));
    ASSERT(!killed);
    manager.reapZombieCursors();
    ASSERT(killed);
}

// Test that reaping does not call kill() on the underlying ClusterClientCursor for a killed cursor
// that is still pinned.
TEST(ClusterCursorManagerTest, ReapZombieCursorsSkipPinned) {
    ClusterCursorManager manager;
    bool killed = false;
    auto cursor = stdx::make_unique<ClusterClientCursorMock>([&killed]() { killed = true; });
    auto pinnedCursor =
        manager.registerCursor(std::move(cursor),
                               nss,
                               ClusterCursorManager::CursorType::NamespaceNotSharded,
                               ClusterCursorManager::CursorLifetime::Mortal);
    ASSERT(!killed);
    manager.reapZombieCursors();
    ASSERT(!killed);

    // Clean up (exhaust and return the cursor).
    ASSERT_OK(pinnedCursor.next().getStatus());
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::Exhausted);
}

// Test that reaping does not call kill() on the underlying ClusterClientCursor for cursors that
// haven't been killed.
TEST(ClusterCursorManagerTest, ReapZombieCursorsSkipNonZombies) {
    ClusterCursorManager manager;
    bool killed = false;
    auto cursor = stdx::make_unique<ClusterClientCursorMock>([&killed]() { killed = true; });
    auto pinnedCursor =
        manager.registerCursor(std::move(cursor),
                               nss,
                               ClusterCursorManager::CursorType::NamespaceNotSharded,
                               ClusterCursorManager::CursorLifetime::Mortal);
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT(!killed);
    manager.reapZombieCursors();
    ASSERT(!killed);

    // Clean up (kill and reap the cursor).
    manager.killAllCursors();
    manager.reapZombieCursors();
}

// Test that getting the namespace for a cursor returns the correct namespace.
TEST(ClusterCursorManagerTest, GetNamespaceForCursorIdBasic) {
    ClusterCursorManager manager;
    auto pinnedCursor =
        manager.registerCursor(stdx::make_unique<ClusterClientCursorMock>(),
                               nss,
                               ClusterCursorManager::CursorType::NamespaceNotSharded,
                               ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = pinnedCursor.getCursorId();
    boost::optional<NamespaceString> cursorNamespace = manager.getNamespaceForCursorId(cursorId);
    ASSERT(cursorNamespace);
    ASSERT_EQ(nss.ns(), cursorNamespace->ns());

    // Clean up (exhaust and return the cursor).
    ASSERT_OK(pinnedCursor.next().getStatus());
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::Exhausted);
}

// Test that getting the namespace for a cursor returns the correct namespace, when there are
// multiple cursors registered on that namespace.
TEST(ClusterCursorManagerTest, GetNamespaceForCursorIdMultipleCursorsSameNamespace) {
    ClusterCursorManager manager;
    const size_t numCursors = 10;
    std::vector<CursorId> cursorIds(numCursors);
    for (size_t i = 0; i < numCursors; ++i) {
        auto cursor = manager.registerCursor(stdx::make_unique<ClusterClientCursorMock>(),
                                             nss,
                                             ClusterCursorManager::CursorType::NamespaceNotSharded,
                                             ClusterCursorManager::CursorLifetime::Mortal);
        cursorIds[i] = cursor.getCursorId();
        cursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    }
    for (size_t i = 0; i < numCursors; ++i) {
        boost::optional<NamespaceString> cursorNamespace =
            manager.getNamespaceForCursorId(cursorIds[i]);
        ASSERT(cursorNamespace);
        ASSERT_EQ(nss.ns(), cursorNamespace->ns());
    }

    // Clean up (kill and reap the cursors).
    manager.killAllCursors();
    manager.reapZombieCursors();
}

// Test that getting the namespace for a cursor returns the correct namespace, when there are
// multiple cursors registered on different namespaces.
TEST(ClusterCursorManagerTest, GetNamespaceForCursorIdMultipleCursorsDifferentNamespaces) {
    ClusterCursorManager manager;
    const size_t numCursors = 10;
    std::vector<std::pair<NamespaceString, CursorId>> cursors(numCursors);
    for (size_t i = 0; i < numCursors; ++i) {
        NamespaceString cursorNamespace(std::string(str::stream() << "test.collection" << i));
        auto cursor = manager.registerCursor(stdx::make_unique<ClusterClientCursorMock>(),
                                             cursorNamespace,
                                             ClusterCursorManager::CursorType::NamespaceNotSharded,
                                             ClusterCursorManager::CursorLifetime::Mortal);
        cursors[i] = {cursorNamespace, cursor.getCursorId()};
        cursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    }
    for (size_t i = 0; i < numCursors; ++i) {
        boost::optional<NamespaceString> cursorNamespace =
            manager.getNamespaceForCursorId(cursors[i].second);
        ASSERT(cursorNamespace);
        ASSERT_EQ(cursors[i].first.ns(), cursorNamespace->ns());
    }

    // Clean up (kill and reap the cursors).
    manager.killAllCursors();
    manager.reapZombieCursors();
}

// Test that getting the namespace for an unknown cursor returns boost::none.
TEST(ClusterCursorManagerTest, GetNamespaceForCursorIdUnknown) {
    ClusterCursorManager manager;
    boost::optional<NamespaceString> cursorNamespace = manager.getNamespaceForCursorId(5);
    ASSERT_FALSE(cursorNamespace);
}

// Test that the PinnedCursor default constructor creates a pin that owns no cursor.
TEST(ClusterCursorManagerTest, PinnedCursorDefaultConstructor) {
    ClusterCursorManager::PinnedCursor pinnedCursor;
    ASSERT_EQ(0, pinnedCursor.getCursorId());
}

// Test that returning a pinned cursor correctly unpins the cursor, and leaves the pin owning no
// cursor.
TEST(ClusterCursorManagerTest, PinnedCursorReturnCursorNotExhausted) {
    ClusterCursorManager manager;
    auto registeredCursor =
        manager.registerCursor(stdx::make_unique<ClusterClientCursorMock>(),
                               nss,
                               ClusterCursorManager::CursorType::NamespaceNotSharded,
                               ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = registeredCursor.getCursorId();
    ASSERT_NE(0, cursorId);
    registeredCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT_EQ(0, registeredCursor.getCursorId());
    auto checkedOutCursor = manager.checkOutCursor(nss, cursorId);
    ASSERT_OK(checkedOutCursor.getStatus());

    // Clean up (exhaust and return the cursor).
    ASSERT_OK(checkedOutCursor.getValue().next().getStatus());
    checkedOutCursor.getValue().returnCursor(ClusterCursorManager::CursorState::Exhausted);
}

// Test that returning a pinned cursor with 'Exhausted' correctly de-registers the cursor, and
// leaves the pin owning no cursor.
TEST(ClusterCursorManagerTest, PinnedCursorReturnCursorExhausted) {
    ClusterCursorManager manager;
    auto registeredCursor =
        manager.registerCursor(stdx::make_unique<ClusterClientCursorMock>(),
                               nss,
                               ClusterCursorManager::CursorType::NamespaceNotSharded,
                               ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = registeredCursor.getCursorId();
    ASSERT_NE(0, cursorId);
    ASSERT_OK(registeredCursor.next().getStatus());
    registeredCursor.returnCursor(ClusterCursorManager::CursorState::Exhausted);
    ASSERT_EQ(0, registeredCursor.getCursorId());
    ASSERT_NOT_OK(manager.checkOutCursor(nss, cursorId).getStatus());
}

// Test that the PinnedCursor move assignment operator correctly kills the cursor if it has not yet
// been returned.
TEST(ClusterCursorManagerTest, PinnedCursorMoveAssignmentKill) {
    ClusterCursorManager manager;
    bool killed = false;
    auto cursor = stdx::make_unique<ClusterClientCursorMock>([&killed]() { killed = true; });
    auto pinnedCursor =
        manager.registerCursor(std::move(cursor),
                               nss,
                               ClusterCursorManager::CursorType::NamespaceNotSharded,
                               ClusterCursorManager::CursorLifetime::Mortal);
    pinnedCursor = ClusterCursorManager::PinnedCursor();
    ASSERT(!killed);
    manager.reapZombieCursors();
    ASSERT(killed);
}

// Test that the PinnedCursor destructor correctly kills the cursor if it has not yet been returned.
TEST(ClusterCursorManagerTest, PinnedCursorDestructorKill) {
    ClusterCursorManager manager;
    bool killed = false;
    {
        auto cursor = stdx::make_unique<ClusterClientCursorMock>([&killed]() { killed = true; });
        auto pinnedCursor =
            manager.registerCursor(std::move(cursor),
                                   nss,
                                   ClusterCursorManager::CursorType::NamespaceNotSharded,
                                   ClusterCursorManager::CursorLifetime::Mortal);
    }
    ASSERT(!killed);
    manager.reapZombieCursors();
    ASSERT(killed);
}

}  // namespace

}  // namespace mongo
