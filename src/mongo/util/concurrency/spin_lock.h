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
#ifdef _WIN32
#include <windows.h>
#endif

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

        static bool isfast(); // true if a real spinlock on this platform

    private:
#if defined(_WIN32)
        CRITICAL_SECTION _cs;
    public:
        void lock() {EnterCriticalSection(&_cs); }
        void unlock() { LeaveCriticalSection(&_cs); }
#elif defined(__USE_XOPEN2K)
        pthread_spinlock_t _lock;
        void _lk();
    public:
        void unlock() { pthread_spin_unlock(&_lock); }
        void lock() {
            if ( MONGO_likely( pthread_spin_trylock( &_lock ) == 0 ) )
                return;
            _lk(); 
        }
#elif defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
        volatile bool _locked;
    public:
        void unlock() {__sync_lock_release(&_locked); }
        void lock();
#else
        // default to a mutex if not implemented
        SimpleMutex _mutex;
    public:
        void unlock() { _mutex.unlock(); }
        void lock() { _mutex.lock(); }
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
