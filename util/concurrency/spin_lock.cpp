// spin_lock.cpp

/**
*    Copyright (C) 2010 10gen Inc.
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

#include "pch.h"
#include <time.h>
#include "spin_lock.h"

namespace mongo {

    SpinLock::~SpinLock() {
#if defined(_WIN32)
        DeleteCriticalSection(&_cs);
#elif defined(__USE_XOPEN2K)
        pthread_spin_destroy(&_lock);
#endif
    }

    SpinLock::SpinLock()
#if defined(_WIN32)
    { InitializeCriticalSectionAndSpinCount(&_cs, 4000); }
#elif defined(__USE_XOPEN2K)
    { pthread_spin_init( &_lock , 0 ); }
#elif defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
    : _locked( false ) { }
#else
    : _mutex( "SpinLock" ) { }
#endif

    void SpinLock::lock() {
#if defined(_WIN32)
        EnterCriticalSection(&_cs);
#elif defined(__USE_XOPEN2K)
        
        /**
         * this is designed to perform close to the default spin lock
         * the reason for the mild insanity is to prevent horrible performance
         * when contention spikes 
         * it allows spinlocks to be used in many more places
         * which is good because even with this change they are about 8x faster on linux
         */
        
        if ( pthread_spin_trylock( &_lock ) == 0 )
            return;
        
        for ( int i=0; i<1000; i++ ) 
            if ( pthread_spin_trylock( &_lock ) == 0 )
                return;

        for ( int i=0; i<1000; i++ ) {
            if ( pthread_spin_trylock( &_lock ) == 0 )
                return;
            pthread_yield();
        }
        
        struct timespec t;
        t.tv_sec = 0;
        t.tv_nsec = 5000000;

        while ( pthread_spin_trylock( &_lock ) != 0 ) {
            nanosleep(&t, NULL);
        }
            
#elif defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
        // fast path
        if (!_locked && !__sync_lock_test_and_set(&_locked, true)) {
            return;
        }

        // wait for lock
        int wait = 1000;
        while ((wait-- > 0) && (_locked)) {
            asm volatile ( "pause" ) ;
        }

        // if failed to grab lock, sleep
        struct timespec t;
        t.tv_sec = 0;
        t.tv_nsec = 5000000;
        while (__sync_lock_test_and_set(&_locked, true)) {
            nanosleep(&t, NULL);
        }
#else
        // WARNING Missing spin lock in this platform. This can potentially
        // be slow.
        _mutex.lock();

#endif
    }

    void SpinLock::unlock() {
#if defined(_WIN32)
        LeaveCriticalSection(&_cs);
#elif defined(__USE_XOPEN2K)
        pthread_spin_unlock(&_lock);
#elif defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
        __sync_lock_release(&_locked);
#else
        _mutex.unlock();
#endif
    }

    bool SpinLock::isfast() {
#if defined(_WIN32)
        return true;
#elif defined(__USE_XOPEN2K)
        return true;
#elif defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
        return true;
#else
        return false;
#endif
    }


}  // namespace mongo
