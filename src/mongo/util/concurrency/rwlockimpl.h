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
*/

#pragma once

#include "mutex.h"

#if( BOOST_VERSION >= 103500 ) 

# if defined(_WIN32)
#  include "shared_mutex_win.hpp"
// modified_shared_mutex has a tweak/fix to the boost::shared_mutex windows implementation
namespace mongo { typedef boost::modified_shared_mutex shared_mutex; }
# else
#  include <boost/thread/shared_mutex.hpp>
namespace mongo { using boost::shared_mutex; }
# endif

namespace mongo { 
    class RWLockBase_Boost : boost::noncopyable {
        shared_mutex _m;
    public:
        void lock() { _m.lock(); }
        void unlock() { _m.unlock(); }
        void lockAsUpgradable() { _m.lock_upgrade(); }
        void unlockFromUpgradable() { // upgradable -> unlocked
            _m.unlock_upgrade();
        }
        void upgrade() { // upgradable -> exclusive lock
            _m.unlock_upgrade_and_lock();
        }
        void lock_shared() { _m.lock_shared(); }
        void unlock_shared() { _m.unlock_shared(); }
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

#if defined(RWLOCK_TEST)
namespace mongo { 
    typedef RWLockBase1 RWLockBase;
}

#elif defined(NTDDI_VERSION) && defined(NTDDI_WIN7) && (NTDDI_VERSION >= NTDDI_WIN7)

// windows implementation, uses SlimReaderWriter locks when possible (dynamically loaded).

namespace mongo {

    class RW_Interface { 
    public:
        virtual ~RW_Interface() { }
        virtual void lock() = 0;
        virtual void unlock() = 0;
        virtual void lock_shared() = 0;
        virtual void unlock_shared() = 0;
    };
    class RWTry_Interface : public RW_Interface { 
    public:
        virtual bool lock_shared_try( int millis ) = 0;
        virtual bool lock_try( int millis = 0 ) = 0;
    };

    class RWLockBase : boost::noncopyable {
        scoped_ptr<RWTry_Interface> i;
        friend class SimpleRWLock;
    protected:
        RWLockBase();
        ~RWLockBase();
        void lock()          { i->lock(); }
        void unlock()        { i->unlock(); }
        void lock_shared()   { i->lock_shared(); }
        void unlock_shared() { i->unlock_shared(); }
        bool lock_shared_try( int millis ) { return i->lock_shared_try(millis); }
        bool lock_try( int millis = 0 )    { return i->lock_try(millis); }

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

namespace mongo {
    class RWLockBase : public RWLockBase_Boost { 
    };
}

#endif
