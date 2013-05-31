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

namespace mongo {

    /** separated out as later the implementation of this may be different than RWLock, 
        depending on OS, as there is no upgrade etc. facility herein.
    */
    class SimpleRWLock : boost::noncopyable { 
#if defined(NTDDI_VERSION) && defined(NTDDI_WIN7) && (NTDDI_VERSION >= NTDDI_WIN7)
        SRWLOCK _lock;
#else
        RWLockBase m;
#endif
#if defined(_WIN32) && defined(_DEBUG)
        AtomicUInt shares;
        ThreadLocalValue<int> s;
        unsigned tid;
#endif
    public:
        const string name;
        SimpleRWLock(const StringData& name = "" );
        void lock();
        void unlock();
        void lock_shared();
        void unlock_shared();
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
    };

}
