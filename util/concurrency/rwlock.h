// @file rwlock.h generic reader-writer lock (cross platform support)

/*
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

#pragma once

#include "mutex.h"
#include "../time_support.h"

// this requires newer windows versions
// it works better than sharable_mutex under high contention
#if defined(_WIN64)
//#define MONGO_USE_SRW_ON_WINDOWS 1
#endif

#if !defined(MONGO_USE_SRW_ON_WINDOWS)

#if BOOST_VERSION >= 103500
# define BOOST_RWLOCK
#else
# if defined(_WIN32)
#  error need boost >= 1.35 for windows
# endif
# include <pthread.h>
#endif

#if defined(_WIN32)
# include "shared_mutex_win.hpp"
namespace mongo {
    typedef boost::modified_shared_mutex shared_mutex;
}
# undef assert
# define assert MONGO_assert
#elif defined(BOOST_RWLOCK)
# include <boost/thread/shared_mutex.hpp>
# undef assert
# define assert MONGO_assert
#endif

#endif

namespace mongo {

#if defined(MONGO_USE_SRW_ON_WINDOWS) && defined(_WIN32)

    // Windows RWLock implementation (requires newer versions of windows thus the above macro)
    class RWLock : boost::noncopyable {
    public:
        RWLock(const char *, int lowPriorityWaitMS=0 ) : _lowPriorityWaitMS(lowPriorityWaitMS)
          { InitializeSRWLock(&_lock); }
        ~RWLock() { }
        const char * implType() const { return "WINSRW"; }
        int lowPriorityWaitMS() const { return _lowPriorityWaitMS; }
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
    private:
        SRWLOCK _lock;
        const int _lowPriorityWaitMS;
    };

#elif defined(BOOST_RWLOCK)

    // Boost RWLock implementation
    class RWLock : boost::noncopyable {
        shared_mutex _m;
        const int _lowPriorityWaitMS;
    public:
#if defined(_DEBUG)
        const char *_name;
        RWLock(const char *name, int lowPriorityWait=0) : _lowPriorityWaitMS(lowPriorityWait) , _name(name) { }
#else
        RWLock(const char *, int lowPriorityWait=0) : _lowPriorityWaitMS(lowPriorityWait) { }
#endif

        const char * implType() const { return "boost"; }

        int lowPriorityWaitMS() const { return _lowPriorityWaitMS; }

        void lock() {
            _m.lock();
#if defined(_DEBUG)
            mutexDebugger.entering(_name);
#endif
        }
        void unlock() {
#if defined(_DEBUG)
            mutexDebugger.leaving(_name);
#endif
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
            if( _m.timed_lock_shared( boost::posix_time::milliseconds(millis) ) ) {
                return true;
            }
            return false;
        }

        bool lock_try( int millis = 0 ) {
            if( _m.timed_lock( boost::posix_time::milliseconds(millis) ) ) {
#if defined(_DEBUG)
                mutexDebugger.entering(_name);
#endif
                return true;
            }
            return false;
        }
    };

#else

    // Posix RWLock implementation
    class RWLock : boost::noncopyable {
        pthread_rwlock_t _lock;
        const int _lowPriorityWaitMS;    
        inline static void check( int x ) {
            if( x == 0 )
                return;
            log() << "pthread rwlock failed: " << x << endl;
            assert( x == 0 );
        }
        
    public:
#if defined(_DEBUG)
        const char *_name;
        RWLock(const char *name, int lowPriorityWaitMS=0) : _lowPriorityWaitMS(lowPriorityWaitMS), _name(name)
#else
        RWLock(const char *, int lowPriorityWaitMS=0) : _lowPriorityWaitMS( lowPriorityWaitMS )
#endif
        {
            check( pthread_rwlock_init( &_lock , 0 ) );
        }
        
        ~RWLock() {
            if ( ! StaticObserver::_destroyingStatics ) {
                wassert( pthread_rwlock_destroy( &_lock ) == 0 ); // wassert as don't want to throw from a destructor
            }
        }

        const char * implType() const { return "posix"; }

        int lowPriorityWaitMS() const { return _lowPriorityWaitMS; }

        void lock() {
            check( pthread_rwlock_wrlock( &_lock ) );
#if defined(_DEBUG)
            mutexDebugger.entering(_name);
#endif
        }
        void unlock() {
#if defined(_DEBUG)
            mutexDebugger.leaving(_name);
#endif
            check( pthread_rwlock_unlock( &_lock ) );
        }

        void lock_shared() {
            check( pthread_rwlock_rdlock( &_lock ) );
        }

        void unlock_shared() {
            check( pthread_rwlock_unlock( &_lock ) );
        }

        bool lock_shared_try( int millis ) {
            return _try( millis , false );
        }

        bool lock_try( int millis = 0 ) {
            if( _try( millis , true ) ) {
#if defined(_DEBUG)
                mutexDebugger.entering(_name);
#endif
                return true;
            }
            return false;
        }

        bool _try( int millis , bool write ) {
            while ( true ) {
                int x = write ?
                        pthread_rwlock_trywrlock( &_lock ) :
                        pthread_rwlock_tryrdlock( &_lock );

                if ( x <= 0 ) {
                    return true;
                }

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

    };

#endif

    /** throws on failure to acquire in the specified time period. */
    class rwlock_try_write : boost::noncopyable {
    public:
        struct exception { };
        rwlock_try_write(RWLock& l, int millis = 0) : _l(l) {
            if( !l.lock_try(millis) )
                throw exception();
        }
        ~rwlock_try_write() { _l.unlock(); }
    private:
        RWLock& _l;
    };

    class rwlock_shared : boost::noncopyable {
    public:
        rwlock_shared(RWLock& rwlock) : _r(rwlock) {_r.lock_shared(); }
        ~rwlock_shared() { _r.unlock_shared(); }
    private:
        RWLock& _r;
    };

    /* scoped lock for RWLock */
    class rwlock : boost::noncopyable {
    public:
        /**
         * @param write acquire write lock if true sharable if false
         * @param lowPriority if > 0, will try to get the lock non-greedily for that many ms
         */
        rwlock( const RWLock& lock , bool write, /* bool alreadyHaveLock = false , */int lowPriorityWaitMS = 0 )
            : _lock( (RWLock&)lock ) , _write( write ) {
            
            {
                if ( _write ) {
                    
                    if ( ! lowPriorityWaitMS && lock.lowPriorityWaitMS() )
                        lowPriorityWaitMS = lock.lowPriorityWaitMS();
                    
                    if ( lowPriorityWaitMS ) { 
                        bool got = false;
                        for ( int i=0; i<lowPriorityWaitMS; i++ ) {
                            if ( _lock.lock_try(0) ) {
                                got = true;
                                break;
                            }
                            
                            int sleep = 1;
                            if ( i > ( lowPriorityWaitMS / 20 ) )
                                sleep = 10;
                            sleepmillis(sleep);
                            i += ( sleep - 1 );
                        }
                        if ( ! got ) {
                            log() << "couldn't get lazy rwlock" << endl;
                            _lock.lock();
                        }
                    }
                    else { 
                        _lock.lock();
                    }

                }
                else { 
                    _lock.lock_shared();
                }
            }
        }
        ~rwlock() {
            if ( _write )
                _lock.unlock();
            else
                _lock.unlock_shared();
        }
    private:
        RWLock& _lock;
        const bool _write;
    };

    /** recursive on shared locks is ok for this implementation */
    class RWLockRecursive : boost::noncopyable {
        ThreadLocalValue<int> _state;
        RWLock _lk;
        friend class Exclusive;
    public:
        /** @param lpwaitms lazy wait */
        RWLockRecursive(const char *name, int lpwaitms) : _lk(name, lpwaitms) { }

        void assertExclusivelyLocked() {
            dassert( _state.get() < 0 );
        }

        class Exclusive : boost::noncopyable { 
            RWLockRecursive& _r;
            rwlock *_scopedLock;
        public:
            Exclusive(RWLockRecursive& r) : _r(r), _scopedLock(0) {
                int s = _r._state.get();
                dassert( s <= 0 );
                if( s == 0 )
                    _scopedLock = new rwlock(_r._lk, true);
                _r._state.set(s-1);
            }
            ~Exclusive() {
                int s = _r._state.get();
                DEV wassert( s < 0 ); // wassert: don't throw from destructors
                _r._state.set(s+1);
                delete _scopedLock;
            }
        };

        class Shared : boost::noncopyable { 
            RWLockRecursive& _r;
            bool _alreadyExclusive;
        public:
            Shared(RWLockRecursive& r) : _r(r) {
                int s = _r._state.get();
                _alreadyExclusive = s < 0;
                if( !_alreadyExclusive ) {
                    dassert( s >= 0 ); // -1 would mean exclusive
                    if( s == 0 )
                        _r._lk.lock_shared(); 
                    _r._state.set(s+1);
                }
            }
            ~Shared() {
                if( _alreadyExclusive ) {
                    DEV wassert( _r._state.get() < 0 );
                }
                else {
                    int s = _r._state.get() - 1;
                    if( s == 0 ) 
                        _r._lk.unlock_shared();
                    _r._state.set(s);
                    DEV wassert( s >= 0 );
                }
            }
        };
    };
}
