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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/config.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/concurrency/threadlocal.h"

namespace mongo {

/** separated out as later the implementation of this may be different than RWLock,
    depending on OS, as there is no upgrade etc. facility herein.
*/
class SimpleRWLock {
    MONGO_DISALLOW_COPYING(SimpleRWLock);
#if defined(NTDDI_VERSION) && defined(NTDDI_WIN7) && (NTDDI_VERSION >= NTDDI_WIN7)
    SRWLOCK _lock;
#else
    RWLockBase m;
#endif
#if defined(_WIN32) && defined(MONGO_CONFIG_DEBUG_BUILD)
    AtomicUInt32 shares;
    ThreadLocalValue<int> s;
    unsigned tid;
#endif
public:
    const std::string name;
    SimpleRWLock(StringData name = "");
    void lock();
    void unlock();
    void lock_shared();
    void unlock_shared();
    class Shared {
        MONGO_DISALLOW_COPYING(Shared);
        SimpleRWLock& _r;

    public:
        Shared(SimpleRWLock& rwlock) : _r(rwlock) {
            _r.lock_shared();
        }
        ~Shared() {
            _r.unlock_shared();
        }
    };
    class Exclusive {
        MONGO_DISALLOW_COPYING(Exclusive);
        SimpleRWLock& _r;

    public:
        Exclusive(SimpleRWLock& rwlock) : _r(rwlock) {
            _r.lock();
        }
        ~Exclusive() {
            _r.unlock();
        }
    };
};
}
