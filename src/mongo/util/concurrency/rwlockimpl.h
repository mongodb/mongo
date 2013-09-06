// @file rwlockimpl.h

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

#include "mutex.h"

#if defined(RWLOCK_TEST)
namespace mongo { 
    typedef RWLockBase1 RWLockBase;
}
#elif defined(NTDDI_VERSION) && defined(NTDDI_WIN7) && (NTDDI_VERSION >= NTDDI_WIN7)

// windows slimreaderwriter version.  newer windows versions only

namespace mongo {
    unsigned long long curTimeMicros64();

    class RWLockBase : boost::noncopyable {
        friend class SimpleRWLock;
        SRWLOCK _lock;
    protected:
        RWLockBase() { InitializeSRWLock(&_lock); }
        ~RWLockBase() {
            // no special action needed to destroy a SRWLOCK
        }
        void lock()          { AcquireSRWLockExclusive(&_lock); }
        void unlock()        { ReleaseSRWLockExclusive(&_lock); }
        void lock_shared()   { AcquireSRWLockShared(&_lock); }
        void unlock_shared() { ReleaseSRWLockShared(&_lock); }
        bool lock_shared_try( int millis ) {
            if( TryAcquireSRWLockShared(&_lock) )
                return true;
            if( millis == 0 )
                return false;
            unsigned long long end = curTimeMicros64() + millis*1000;
            while( 1 ) {
                Sleep(1);
                if( TryAcquireSRWLockShared(&_lock) )
                    return true;
                if( curTimeMicros64() >= end )
                    break;
            }
            return false;
        }
        bool lock_try( int millis = 0 ) {
            if( TryAcquireSRWLockExclusive(&_lock) ) // quick check to optimistically avoid calling curTimeMicros64
                return true;
            if( millis == 0 )
                return false;
            unsigned long long end = curTimeMicros64() + millis*1000;
            do {
                Sleep(1);
                if( TryAcquireSRWLockExclusive(&_lock) )
                    return true;
            } while( curTimeMicros64() < end );
            return false;
        }
        // no upgradable for this impl
        void lockAsUpgradable() { lock(); }
        void unlockFromUpgradable() { unlock(); }
        void upgrade() { }
    public:
        const char * implType() const { return "WINSRW"; }
    };
}

#elif( BOOST_VERSION < 103500 ) 

# if !defined(BOOST_VERSION)
#  error BOOST_VERSION is not defined 
# endif
# if defined(_WIN32)
#  error need boost >= 1.35 for windows
# endif

// pthreads version

# include <pthread.h>
# include <errno.h>

namespace mongo { 
    class RWLockBase : boost::noncopyable {
        friend class SimpleRWLock;
        pthread_rwlock_t _lock;
        static void check( int x ) {
            verify( x == 0 );
        }        
    protected:
        ~RWLockBase() {
            if ( ! StaticObserver::_destroyingStatics ) {
                wassert( pthread_rwlock_destroy( &_lock ) == 0 ); // wassert as don't want to throw from a destructor
            }
        }
        RWLockBase() {
            check( pthread_rwlock_init( &_lock , 0 ) );
        }
        void lock() { check( pthread_rwlock_wrlock( &_lock ) ); }
        void unlock() { check( pthread_rwlock_unlock( &_lock ) ); }
        void lock_shared() { check( pthread_rwlock_rdlock( &_lock ) ); }
        void unlock_shared() { check( pthread_rwlock_unlock( &_lock ) ); }
        bool lock_shared_try( int millis ) { return _try( millis , false ); }
        bool lock_try( int millis = 0 ) { return _try( millis , true ); }
        bool _try( int millis , bool write ) {
            while ( true ) {
                int x = write ?
                        pthread_rwlock_trywrlock( &_lock ) :
                        pthread_rwlock_tryrdlock( &_lock );
                if ( x <= 0 )
                    return true;
                if ( millis-- <= 0 )
                    return false;
                if ( x == EBUSY ) {
                    sleepmillis(1);
                    continue;
                }
                check(x);
            }
            return false;
        }
        // no upgradable for this impl
        void lockAsUpgradable() { lock(); }
        void unlockFromUpgradable() { unlock(); }
        void upgrade() { }
    public:
        const char * implType() const { return "posix"; }
    };
}

#else

// Boost version

# if defined(_WIN32)
#  include "shared_mutex_win.hpp"
namespace mongo { typedef boost::modified_shared_mutex shared_mutex; }
# else
#  include <boost/thread/shared_mutex.hpp>
namespace mongo { using boost::shared_mutex; }
# endif

namespace mongo { 
    class RWLockBase : boost::noncopyable {
        friend class SimpleRWLock;
        shared_mutex _m;
    protected:
        void lock() {
             _m.lock();
        }
        void unlock() {
            _m.unlock();
        }
        void lockAsUpgradable() { 
            _m.lock_upgrade();
        }
        void unlockFromUpgradable() { // upgradable -> unlocked
            _m.unlock_upgrade();
        }
        void upgrade() { // upgradable -> exclusive lock
            _m.unlock_upgrade_and_lock();
        }
        void lock_shared() {
            _m.lock_shared();
        }
        void unlock_shared() {
            _m.unlock_shared();
        }
        bool lock_shared_try( int millis ) {
            return _m.timed_lock_shared( boost::posix_time::milliseconds(millis) );
        }
        bool lock_try( int millis = 0 ) {
            return _m.timed_lock( boost::posix_time::milliseconds(millis) );
        }
    public:
        const char * implType() const { return "boost"; }
    };
}

#endif
