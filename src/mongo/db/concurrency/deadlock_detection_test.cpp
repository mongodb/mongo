/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/concurrency/lock_manager_test_help.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(Deadlock, NoDeadlock) {
    const ResourceId resId(RESOURCE_DATABASE, std::string("A"));

    LockerForTests locker1(MODE_IS);
    LockerForTests locker2(MODE_IS);

    ASSERT_EQUALS(LOCK_OK, locker1.lockBegin(nullptr, resId, MODE_S));
    ASSERT_EQUALS(LOCK_OK, locker2.lockBegin(nullptr, resId, MODE_S));

    DeadlockDetector wfg1(*getGlobalLockManager(), &locker1);
    ASSERT(!wfg1.check().hasCycle());

    DeadlockDetector wfg2(*getGlobalLockManager(), &locker2);
    ASSERT(!wfg2.check().hasCycle());
}

TEST(Deadlock, Simple) {
    const ResourceId resIdA(RESOURCE_DATABASE, std::string("A"));
    const ResourceId resIdB(RESOURCE_DATABASE, std::string("B"));

    LockerForTests locker1(MODE_IX);
    LockerForTests locker2(MODE_IX);

    ASSERT_EQUALS(LOCK_OK, locker1.lockBegin(nullptr, resIdA, MODE_X));
    ASSERT_EQUALS(LOCK_OK, locker2.lockBegin(nullptr, resIdB, MODE_X));

    // 1 -> 2
    ASSERT_EQUALS(LOCK_WAITING, locker1.lockBegin(nullptr, resIdB, MODE_X));

    // 2 -> 1
    ASSERT_EQUALS(LOCK_WAITING, locker2.lockBegin(nullptr, resIdA, MODE_X));

    DeadlockDetector wfg1(*getGlobalLockManager(), &locker1);
    ASSERT(wfg1.check().hasCycle());

    DeadlockDetector wfg2(*getGlobalLockManager(), &locker2);
    ASSERT(wfg2.check().hasCycle());

    // Cleanup, so that LockerImpl doesn't complain about leaked locks
    locker1.unlock(resIdB);
    locker2.unlock(resIdA);
}

TEST(Deadlock, SimpleUpgrade) {
    const ResourceId resId(RESOURCE_DATABASE, std::string("A"));

    LockerForTests locker1(MODE_IX);
    LockerForTests locker2(MODE_IX);

    // Both acquire lock in intent mode
    ASSERT_EQUALS(LOCK_OK, locker1.lockBegin(nullptr, resId, MODE_IX));
    ASSERT_EQUALS(LOCK_OK, locker2.lockBegin(nullptr, resId, MODE_IX));

    // Both try to upgrade
    ASSERT_EQUALS(LOCK_WAITING, locker1.lockBegin(nullptr, resId, MODE_X));
    ASSERT_EQUALS(LOCK_WAITING, locker2.lockBegin(nullptr, resId, MODE_X));

    DeadlockDetector wfg1(*getGlobalLockManager(), &locker1);
    ASSERT(wfg1.check().hasCycle());

    DeadlockDetector wfg2(*getGlobalLockManager(), &locker2);
    ASSERT(wfg2.check().hasCycle());

    // Cleanup, so that LockerImpl doesn't complain about leaked locks
    locker1.unlock(resId);
    locker2.unlock(resId);
}

TEST(Deadlock, Indirect) {
    const ResourceId resIdA(RESOURCE_DATABASE, std::string("A"));
    const ResourceId resIdB(RESOURCE_DATABASE, std::string("B"));

    LockerForTests locker1(MODE_IX);
    LockerForTests locker2(MODE_IX);
    LockerForTests lockerIndirect(MODE_IX);

    ASSERT_EQUALS(LOCK_OK, locker1.lockBegin(nullptr, resIdA, MODE_X));
    ASSERT_EQUALS(LOCK_OK, locker2.lockBegin(nullptr, resIdB, MODE_X));

    // 1 -> 2
    ASSERT_EQUALS(LOCK_WAITING, locker1.lockBegin(nullptr, resIdB, MODE_X));

    // 2 -> 1
    ASSERT_EQUALS(LOCK_WAITING, locker2.lockBegin(nullptr, resIdA, MODE_X));

    // 3 -> 2
    ASSERT_EQUALS(LOCK_WAITING, lockerIndirect.lockBegin(nullptr, resIdA, MODE_X));

    DeadlockDetector wfg1(*getGlobalLockManager(), &locker1);
    ASSERT(wfg1.check().hasCycle());

    DeadlockDetector wfg2(*getGlobalLockManager(), &locker2);
    ASSERT(wfg2.check().hasCycle());

    // Indirect locker should not report the cycle since it does not participate in it
    DeadlockDetector wfgIndirect(*getGlobalLockManager(), &lockerIndirect);
    ASSERT(!wfgIndirect.check().hasCycle());

    // Cleanup, so that LockerImpl doesn't complain about leaked locks
    locker1.unlock(resIdB);
    locker2.unlock(resIdA);
}

}  // namespace mongo
