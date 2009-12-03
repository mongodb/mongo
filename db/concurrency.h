/* concurrency.h

   mongod concurrency rules & notes will be placed here.

   Mutex heirarchy (1 = "leaf")
     name                   level
     Logstream::mutex       1
     ClientCursor::ccmutex  2
     dblock                 3

     End func name with _inlock to indicate "caller must lock before calling".
*/

#pragma once

#if BOOST_VERSION >= 103500
#include <boost/thread/shared_mutex.hpp>
#undef assert
#define assert xassert
#endif

namespace mongo {

    /* mutex time stats */
    class MutexInfo {
        unsigned long long start, enter, timeLocked; // all in microseconds
        int locked;

    public:
        MutexInfo() : locked(0) {
            start = curTimeMicros64();
        }
        void entered() {
            if ( locked == 0 )
                enter = curTimeMicros64();
            locked++;
            assert( locked == 1 );
        }
        void leaving() {
            locked--;
            assert( locked == 0 );
            if ( locked == 0 )
                timeLocked += curTimeMicros64() - enter;
        }
        int isLocked() const {
            return locked;
        }
        void getTimingInfo(unsigned long long &s, unsigned long long &tl) const {
            s = start;
            tl = timeLocked;
        }
    };

#if BOOST_VERSION >= 103500
    class MongoMutex {
        MutexInfo _minfo;
        boost::shared_mutex _m;
        ThreadLocalValue<int> _state;
    public:
        void lock() { 
            DEV cout << "LOCK" << endl;
            DEV assert( _state.get() == 0 );
            DEV _state.set(1);
            _m.lock(); 
            _minfo.entered();
        }
        void unlock() { 
            DEV cout << "UNLOCK" << endl;
            DEV assert( _state.get() == 1 );
            DEV _state.set(0);
            _minfo.leaving();
            _m.unlock(); 
        }
        void lock_shared() { 
            DEV cout << " LOCKSHARED" << endl;
            DEV assert( _state.get() == 0 );
            DEV _state.set(2);
            _m.lock_shared(); 
        }
        void unlock_shared() { 
            DEV cout << " UNLOCKSHARED" << endl;
            DEV assert( _state.get() == 2 );
            DEV _state.set(0);
            _m.unlock_shared(); 
        }
        MutexInfo& info() { return _minfo; }
    };
#else
    /* this will be for old versions of boost */
    class MongoMutex { 
        MutexInfo _minfo;
        boost::recursive_mutex m;
    public:
        MongoMutex() { }
        void lock() { 
            boost::detail::thread::lock_ops<boost::recursive_mutex>::lock(m);
            _minfo.entered();
        }

        void unlock() {
            _minfo.leaving();
            // boost >1.35 would be: m.unlock();
            boost::detail::thread::lock_ops<boost::recursive_mutex>::unlock(m);
        }

        void lock_shared() { lock(); }
        void unlock_shared() { unlock(); }
        MutexInfo& info() { return _minfo; }
    };
#endif

    extern MongoMutex &dbMutex;

	void dbunlocking_write();
	void dbunlocking_read();

    struct writelock {
        writelock(const string& ns) {
            dbMutex.lock();
        }
        ~writelock() { 
            dbunlocking_write();
            dbMutex.unlock();
        }
    };
    
    struct readlock {
        readlock(const string& ns) {
            dbMutex.lock_shared();
        }
        ~readlock() { 
            dbunlocking_read();
            dbMutex.unlock_shared();
        }
    };
    
    class mongolock {
        bool _writelock;
    public:
        mongolock(bool write) : _writelock(write) {
            if( _writelock ) {
                dbMutex.lock();
            }
            else
                dbMutex.lock_shared();
        }
        ~mongolock() { 
            if( _writelock ) { 
                dbunlocking_write();
                dbMutex.unlock();
            }
            else {
                dbunlocking_read();
                dbMutex.unlock_shared();
            }
        }
        /* this unlocks, does NOT upgrade. that works for our current usage */
        void releaseAndWriteLock();
    };
    
	/* use writelock and readlock instead */
    struct dblock : public writelock {
        dblock() : writelock("") { }
        ~dblock() { 
        }
    };
    
    /* a scoped release of a mutex temporarily -- like a scopedlock but reversed.
    */
/*
    struct temprelease {
        boost::mutex& m;
        temprelease(boost::mutex& _m) : m(_m) {
#if BOOST_VERSION >= 103500
            m.unlock();
#else
            boost::detail::thread::lock_ops<boost::mutex>::unlock(m);
#endif
        }
        ~temprelease() {
#if BOOST_VERSION >= 103500
            m.lock();
#else
            boost::detail::thread::lock_ops<boost::mutex>::lock(m);
#endif
        }
    };
*/

    inline void assertInWriteLock() { 
/* TEMP        assert( dbMutexInfo.isLocked() );
*/
    }

}
