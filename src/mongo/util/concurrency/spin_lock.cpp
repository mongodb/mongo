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

#include "pch.h" // todo eliminate this include
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

#if defined(__USE_XOPEN2K)
    NOINLINE_DECL void SpinLock::_lk() {
        /**
         * this is designed to perform close to the default spin lock
         * the reason for the mild insanity is to prevent horrible performance
         * when contention spikes 
         * it allows spinlocks to be used in many more places
         * which is good because even with this change they are about 8x faster on linux
         */
        
        for ( int i=0; i<1000; i++ ) {            
            if ( pthread_spin_trylock( &_lock ) == 0 )
                return;
#if defined(__i386__) || defined(__x86_64__)
            asm volatile ( "pause" ) ; // maybe trylock does this; just in case.
#endif
        }

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
    }
#elif defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
    void SpinLock::lock() {

        // fast path
        if (!_locked && !__sync_lock_test_and_set(&_locked, true)) {
            return;
        }

        // wait for lock
        int wait = 1000;
        while ((wait-- > 0) && (_locked)) {
#if defined(__i386__) || defined(__x86_64__)
            asm volatile ( "pause" ) ;
#endif
        }

        // if failed to grab lock, sleep
        struct timespec t;
        t.tv_sec = 0;
        t.tv_nsec = 5000000;
        while (__sync_lock_test_and_set(&_locked, true)) {
            nanosleep(&t, NULL);
        }
    }
#endif

    bool SpinLock::isfast() {
#if defined(_WIN32) || defined(__USE_XOPEN2K) || defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
        return true;
#else
        return false;
#endif
    }


}  // namespace mongo
