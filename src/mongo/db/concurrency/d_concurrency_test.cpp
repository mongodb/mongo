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

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_mgr_test_help.h"
#include "mongo/unittest/unittest.h"


// Most of the tests here will be removed once we move everything over to using LockManager
//

namespace mongo {

    TEST(DConcurrency, GlobalRead) {
        LockState ls;
        Lock::GlobalRead globalRead(&ls);
        ASSERT(ls.isR());
    }

    TEST(DConcurrency, GlobalWrite) {
        LockState ls;
        Lock::GlobalWrite globalWrite(&ls);
        ASSERT(ls.isW());
    }

    TEST(DConcurrency, GlobalWriteAndGlobalRead) {
        LockState ls;

        Lock::GlobalWrite globalWrite(&ls);
        ASSERT(ls.isW());

        {
            Lock::GlobalRead globalRead(&ls);
            ASSERT(ls.isW());
        }

        ASSERT(ls.isW());
    }

    TEST(DConcurrency, readlocktryTimeout) {
        LockState ls;
        writelocktry globalWrite(&ls, 0);
        ASSERT(globalWrite.got());

        {
            LockState lsTry;
            readlocktry lockTry(&lsTry, 1);
            ASSERT(!lockTry.got());
        }
    }

    TEST(DConcurrency, writelocktryTimeout) {
        LockState ls;
        writelocktry globalWrite(&ls, 0);
        ASSERT(globalWrite.got());

        {
            LockState lsTry;
            writelocktry lockTry(&lsTry, 1);
            ASSERT(!lockTry.got());
        }
    }

    TEST(DConcurrency, readlocktryTimeoutDueToFlushLock) {
        LockState ls;
        newlm::AutoAcquireFlushLockForMMAPV1Commit autoFlushLock(&ls);

        {
            LockState lsTry;
            readlocktry lockTry(&lsTry, 1);
            ASSERT(!lockTry.got());
        }
    }

    TEST(DConcurrency, writelocktryTimeoutDueToFlushLock) {
        LockState ls;
        newlm::AutoAcquireFlushLockForMMAPV1Commit autoFlushLock(&ls);

        {
            LockState lsTry;
            writelocktry lockTry(&lsTry, 1);
            ASSERT(!lockTry.got());
        }
    }

    TEST(DConcurrency, TempReleaseGlobalRead) {
        LockState ls;
        Lock::GlobalRead globalRead(&ls);

        {
            Lock::TempRelease tempRelease(&ls);
            ASSERT(!ls.isLocked());
        }

        ASSERT(ls.isR());
    }

    TEST(DConcurrency, TempReleaseGlobalWrite) {
        LockState ls;
        Lock::GlobalWrite globalWrite(&ls);

        {
            Lock::TempRelease tempRelease(&ls);
            ASSERT(!ls.isLocked());
        }

        ASSERT(ls.isW());
    }

    TEST(DConcurrency, MultipleWriteDBLocksOnSameThread) {
        LockState ls;

        Lock::DBWrite r1(&ls, "db1");
        Lock::DBWrite r2(&ls, "db1");

        ASSERT(ls.isWriteLocked("db1"));
    }

    TEST(DConcurrency, MultipleConflictingDBLocksOnSameThread) {
        LockState ls;

        Lock::DBWrite r1(&ls, "db1");
        Lock::DBRead r2(&ls, "db1");

        ASSERT(ls.isWriteLocked("db1"));
    }

} // namespace mongo
