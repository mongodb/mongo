// @file rwlockimpl.cpp

/**
*    Copyright (C) 2012 10gen Inc.
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
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/


#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <boost/version.hpp>
#include <map>
#include <set>

#include "mongo/config.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/rwlockimpl.h"
#include "mongo/util/concurrency/simplerwlock.h"
#include "mongo/util/concurrency/threadlocal.h"
#include "mongo/util/time_support.h"

using namespace std;

namespace mongo {

#if defined(NTDDI_VERSION) && defined(NTDDI_WIN7) && (NTDDI_VERSION >= NTDDI_WIN7)
SimpleRWLock::SimpleRWLock(StringData p) : name(p.toString()) {
    InitializeSRWLock(&_lock);
}
#if defined(MONGO_CONFIG_DEBUG_BUILD)
// the code below in a debug build will check that we don't try to recursively lock,
// which is not supported by this class.  also checks that you don't unlock without
// having locked
void SimpleRWLock::lock() {
    unsigned me = GetCurrentThreadId();
    int& state = s.getRef();
    dassert(state == 0);
    state--;
    AcquireSRWLockExclusive(&_lock);
    tid = me;  // this is for use in the debugger to see who does have the lock
}
void SimpleRWLock::unlock() {
    int& state = s.getRef();
    dassert(state == -1);
    state++;
    tid = 0xffffffff;
    ReleaseSRWLockExclusive(&_lock);
}
void SimpleRWLock::lock_shared() {
    int& state = s.getRef();
    dassert(state == 0);
    state++;
    AcquireSRWLockShared(&_lock);
    shares.fetchAndAdd(1);
}
void SimpleRWLock::unlock_shared() {
    int& state = s.getRef();
    dassert(state == 1);
    state--;
    shares.fetchAndSubtract(1);
    ReleaseSRWLockShared(&_lock);
}
#else
void SimpleRWLock::lock() {
    AcquireSRWLockExclusive(&_lock);
}
void SimpleRWLock::unlock() {
    ReleaseSRWLockExclusive(&_lock);
}
void SimpleRWLock::lock_shared() {
    AcquireSRWLockShared(&_lock);
}
void SimpleRWLock::unlock_shared() {
    ReleaseSRWLockShared(&_lock);
}
#endif
#else
SimpleRWLock::SimpleRWLock(StringData p) : name(p.toString()) {}
void SimpleRWLock::lock() {
    m.lock();
}
void SimpleRWLock::unlock() {
    m.unlock();
}
void SimpleRWLock::lock_shared() {
    m.lock_shared();
}
void SimpleRWLock::unlock_shared() {
    m.unlock_shared();
}
#endif
}
