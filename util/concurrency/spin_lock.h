// spin_lock.h

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mutex.h"

namespace mongo {

    /**
     * The spinlock currently requires late GCC support routines to be efficient.
     * Other platforms default to a mutex implemenation.
     */
    class SpinLock : boost::noncopyable {
    public:
        SpinLock();
        ~SpinLock();

        void lock();
        void unlock();

        static bool isfast(); // true if a real spinlock on this platform

    private:
#if defined(_WIN32)
        CRITICAL_SECTION _cs;
#elif defined(__USE_XOPEN2K)
        pthread_spinlock_t _lock;
#elif defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
        volatile bool _locked;
#else
        // default to a mutex if not implemented
        SimpleMutex _mutex;
#endif
    };
    
    class scoped_spinlock : boost::noncopyable {
    public:
        scoped_spinlock( SpinLock& l ) : _l(l) {
            _l.lock();
        }
        ~scoped_spinlock() {
            _l.unlock();}
    private:
        SpinLock& _l;
    };

}  // namespace mongo
