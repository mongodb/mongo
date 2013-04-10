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
*/

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/util/atomic_int.h"
#include "rwlockimpl.h"

namespace mongo {

    class SimpleRWLock : boost::noncopyable { 
#if defined(NTDDI_VERSION) && defined(NTDDI_WIN7) && (NTDDI_VERSION >= NTDDI_WIN7)
        SRWLOCK _lock;
#else
        RWLockBase m;
#endif
    public:
        const string name;
        SimpleRWLock(const StringData& name = "" );
        class Shared : boost::noncopyable {
            SimpleRWLock& _r;
        public:
            Shared(SimpleRWLock& rwlock) : _r(rwlock) {_r.lock_shared(); }
            ~Shared() { _r.unlock_shared(); }
        };
        class Exclusive : boost::noncopyable {
            SimpleRWLock& _r;
        public:
            Exclusive(SimpleRWLock& rwlock) : _r(rwlock) {_r.lock(); }
            ~Exclusive() { _r.unlock(); }
        };
        void lock();
        void unlock();
        void lock_shared();
        void unlock_shared();
#if defined(_DEBUG) &&defined(_WIN32)
    private:
        // some code in this version which verifies we don't try to recursively lock, 
        // which is not supported by this class. done in _DEBUG only for performance reasons; 
        // done _WIN32 only just as that was convenient at the time and gives us a check in 
        // at least one build environment.
        AtomicUInt shares;
        ThreadLocalValue<int> s;
        unsigned tid;
#endif
    };

#if !(defined(_DEBUG) && defined(_WIN32))
    inline void SimpleRWLock::lock()          { m.lock(); }
    inline void SimpleRWLock::unlock()        { m.unlock(); }
    inline void SimpleRWLock::lock_shared()   { m.lock_shared(); }
    inline void SimpleRWLock::unlock_shared() { m.unlock_shared(); }
#endif

}
