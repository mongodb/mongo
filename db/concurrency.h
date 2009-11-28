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
            assert( locked >= 1 );
        }
        void leaving() {
            locked--;
            assert( locked >= 0 );
            if ( locked == 0 )
                timeLocked += curTimeMicros64() - enter;
        }
        int isLocked() const {
            return locked;
        }
        void timingInfo(unsigned long long &s, unsigned long long &tl) {
            s = start;
            tl = timeLocked;
        }
    };

    extern boost::recursive_mutex &dbMutex;
    extern MutexInfo dbMutexInfo;

    struct lock {
        recursive_boostlock bl_;
        MutexInfo& info_;
        lock( boost::recursive_mutex &mutex, MutexInfo &info ) :
                bl_( mutex ),
                info_( info ) {
            info_.entered();
        }
        ~lock() {
            info_.leaving();
        }
    };

	void dbunlocking_write();
	void dbunlocking_read();

	/* use writelock and readlock instead */
    struct dblock : public lock {
        dblock() :
            lock( dbMutex, dbMutexInfo ) {
        }
        ~dblock() { 
            dbunlocking_write();
        }
    };
    
    struct writelock : public lock {
        writelock(const string& ns) :
            lock( dbMutex, dbMutexInfo ) {
        }
        writelock(const char * ns) :
            lock( dbMutex, dbMutexInfo ) {
        }
        ~writelock() { 
            dbunlocking_write();
        }
    };
    
    struct readlock : public lock {
        readlock(const string& ns) :
            lock( dbMutex, dbMutexInfo ) {
        }
        readlock(const char* ns) :
            lock( dbMutex, dbMutexInfo ) {
        }
        ~readlock() { 
            dbunlocking_read();
        }
    };
    
    /* a scoped release of a mutex temporarily -- like a scopedlock but reversed.
    */
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

    inline void assertInWriteLock() { 
        assert( dbMutexInfo.isLocked() );
    }

}
