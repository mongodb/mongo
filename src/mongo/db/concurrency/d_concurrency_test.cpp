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

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_mgr_test_help.h"
#include "mongo/unittest/unittest.h"


// Most of the tests here will be removed once we move everything over to using LockManager
//

namespace mongo {

    TEST(DConcurrency, GlobalRead) {
        MMAPV1LockerImpl ls(1);
        Lock::GlobalRead globalRead(&ls);
        ASSERT(ls.isR());
    }

    TEST(DConcurrency, GlobalWrite) {
        MMAPV1LockerImpl ls(1);
        Lock::GlobalWrite globalWrite(&ls);
        ASSERT(ls.isW());
    }

    TEST(DConcurrency, GlobalWriteAndGlobalRead) {
        MMAPV1LockerImpl ls(1);

        Lock::GlobalWrite globalWrite(&ls);
        ASSERT(ls.isW());

        {
            Lock::GlobalRead globalRead(&ls);
            ASSERT(ls.isW());
        }

        ASSERT(ls.isW());
    }

    TEST(DConcurrency, readlocktryTimeout) {
        MMAPV1LockerImpl ls(1);
        writelocktry globalWrite(&ls, 0);
        ASSERT(globalWrite.got());

        {
            MMAPV1LockerImpl lsTry(2);
            readlocktry lockTry(&lsTry, 1);
            ASSERT(!lockTry.got());
        }
    }

    TEST(DConcurrency, writelocktryTimeout) {
        MMAPV1LockerImpl ls(1);
        writelocktry globalWrite(&ls, 0);
        ASSERT(globalWrite.got());

        {
            MMAPV1LockerImpl lsTry(2);
            writelocktry lockTry(&lsTry, 1);
            ASSERT(!lockTry.got());
        }
    }

    TEST(DConcurrency, readlocktryTimeoutDueToFlushLock) {
        MMAPV1LockerImpl ls(1);
        AutoAcquireFlushLockForMMAPV1Commit autoFlushLock(&ls);

        {
            MMAPV1LockerImpl lsTry(2);
            readlocktry lockTry(&lsTry, 1);

            ASSERT(!lockTry.got());
        }
    }

    TEST(DConcurrency, writelocktryTimeoutDueToFlushLock) {
        MMAPV1LockerImpl ls(1);
        AutoAcquireFlushLockForMMAPV1Commit autoFlushLock(&ls);

        {
            MMAPV1LockerImpl lsTry(2);
            writelocktry lockTry(&lsTry, 1);
            ASSERT(!lockTry.got());
        }
    }

    TEST(DConcurrency, TempReleaseGlobalWrite) {
        MMAPV1LockerImpl ls(1);
        Lock::GlobalWrite globalWrite(&ls);

        {
            Lock::TempRelease tempRelease(&ls);
            ASSERT(!ls.isLocked());
        }

        ASSERT(ls.isW());
    }

    TEST(DConcurrency, DBReadTakesS) {
        MMAPV1LockerImpl ls(1);

        Lock::DBRead dbRead(&ls, "db");

        const ResourceId resIdDb(RESOURCE_DATABASE, string("db"));
        ASSERT(ls.getLockMode(resIdDb) == MODE_S);
    }

    TEST(DConcurrency, DBLockTakesX) {
        MMAPV1LockerImpl ls(1);

        Lock::DBLock dbWrite(&ls, "db", MODE_X);

        const ResourceId resIdDb(RESOURCE_DATABASE, string("db"));
        ASSERT(ls.getLockMode(resIdDb) == MODE_X);
    }

    TEST(DConcurrency, MultipleWriteDBLocksOnSameThread) {
        MMAPV1LockerImpl ls(1);

        Lock::DBLock r1(&ls, "db1", MODE_X);
        Lock::DBLock r2(&ls, "db1", MODE_X);

        ASSERT(ls.isWriteLocked("db1"));
    }

    TEST(DConcurrency, MultipleConflictingDBLocksOnSameThread) {
        MMAPV1LockerImpl ls(1);

        Lock::DBLock r1(&ls, "db1", MODE_X);
        Lock::DBRead r2(&ls, "db1");

        ASSERT(ls.isWriteLocked("db1"));
    }

    TEST(DConcurrency, IntentCollectionLock) {
        MMAPV1LockerImpl ls(1);

        const std::string ns("db1.coll");
        const ResourceId id(RESOURCE_COLLECTION, ns);
        Lock::DBLock r1(&ls, "db1", MODE_X);
        {
            Lock::CollectionLock r2(&ls, ns, MODE_S);
            ASSERT(ls.isAtLeastReadLocked(ns));
        }
        ASSERT(ls.getLockMode(id) == MODE_NONE);
    }
} // namespace mongo
