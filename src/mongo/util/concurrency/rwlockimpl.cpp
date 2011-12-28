// @file rwlockimpl.cpp

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <map>
#include <set>
#include <boost/version.hpp>
#include <boost/noncopyable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/recursive_mutex.hpp>
#include <boost/thread/condition.hpp>

using namespace std;

#include "../assert_util.h"
#include "rwlockimpl.h"

namespace mongo { 

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
        boost::mutex::scoped_lock lk(m);
        reading--;
        if( reading == 0 && wantToWrite ) { 
            m_cond.notify_one();
        }
    }

    void RWLockBase1::lock_shared() { 
        boost::mutex::scoped_lock lk(m);
        while( 1 ) { 
            if( (wantToWrite|writing) == 0 )
                break;
            // wait for writer to finish
            boost::mutex::scoped_lock lk(writer);
        }
        reading++;
    }

    void RWLockBase1::lock() {
        boost::mutex::scoped_lock lk(m);
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
