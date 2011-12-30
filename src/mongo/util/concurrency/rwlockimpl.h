// @file rwlockimpl.h

#pragma once

//#define RWLOCK_TEST 1

namespace mongo {
    class RWLockBase1 : boost::noncopyable { 
        unsigned reading;
        unsigned writing;
        unsigned wantToWrite;
        boost::mutex m;
        boost::condition m_cond;
        boost::mutex writer;
    public:
        RWLockBase1();
        ~RWLockBase1();
        const char * implType() const { return "mongo"; }
        void lock();
        void unlock();
        void lock_shared();
        void unlock_shared();

        bool lock_shared_try(int millis);
        bool lock_try(int millis = 0);

        void lockAsUpgradable();
        void unlockFromUpgradable();
        void upgrade();
    };
}

#if defined(RWLOCK_TEST)
namespace mongo { 
    typedef RWLockBase1 RWLockBase;
}
#elif defined(MONGO_USE_SRW_ON_WINDOWS) && defined(_WIN32)

// windows slimreaderwriter version.  newer windows versions only

namespace mongo {
    unsigned long long curTimeMicros64();

    class RWLockBase : boost::noncopyable {
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

namespace mongo { 
    class RWLockBase : boost::noncopyable {
        pthread_rwlock_t _lock;
        static void check( int x ) {
            if( x == 0 ) return;
            log() << "pthread rwlock failed: " << x << endl;
            assert( x == 0 );
        }        

        ~RWLockBase() {
            if ( ! StaticObserver::_destroyingStatics ) {
                wassert( pthread_rwlock_destroy( &_lock ) == 0 ); // wassert as don't want to throw from a destructor
            }
        }

    protected:
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
# undef assert
# define assert MONGO_assert

namespace mongo { 
    class RWLockBase : boost::noncopyable {
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
