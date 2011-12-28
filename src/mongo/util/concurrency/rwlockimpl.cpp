// @file rwlockimpl.cpp

#include <boost/version.hpp>

#if !defined(_WIN32) && BOOST_VERSION >= 103500

#include "rwlockimpl.h"

#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <map>
#include <set>
#include <boost/noncopyable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/recursive_mutex.hpp>
#include <boost/thread/condition.hpp>

using namespace std;

namespace mongo { 

    using boost::mutex;

    class RWLockBase1 : boost::noncopyable { 
        unsigned reading;
        unsigned writing;
        unsigned wantToWrite;
        boost::mutex m;
        boost::condition m_cond;
        boost::mutex writer;
    public:
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

    bool RWLockBase1::lock_shared_try(int millis) { 
        //TODO
        lock_shared();
        return true;
    }

    bool RWLockBase1::lock_try(int millis) { 
        //TODO
        lock();
        return true;
    }

    void RWLockBase1::unlock_shared() { 
        mutex::scoped_lock lk(m);
        reading--;
        if( reading == 0 && wantToWrite ) { 
            m_cond.notify_one();
        }
    }

    void RWLockBase1::lock_shared() { 
        mutex::scoped_lock lk(m);
        while( 1 ) { 
            if( (wantToWrite|writing) == 0 )
                break;
            // wait for writer to finish
            mutex::scoped_lock lk(writer);
        }
        reading++;
    }

    void RWLockBase1::lock() {
        mutex::scoped_lock lk(m);
        wantToWrite++;
        while( reading ) {
            // wait for the readers
            m_cond.wait(lk);
        }
        // wait for the writer.
        writer.lock();
        assert( writing == 0 );
        writing++;
        wantToWrite--;
    }

    void RWLockBase1::unlock() {
        writing--;
        assert( writing == 0 );
        writer.unlock();
    }

}
