// @file rwlockimpl.cpp

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
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
#include "../time_support.h"
#include "rwlockimpl.h"

#if defined(RWLOCK_TEST)

namespace mongo { 

#if defined(_DEBUG) && defined(_WIN32)

    __declspec( thread ) void *what;
    __declspec( thread ) int state = 0;
    void check(RWLockBase1* t, int old, int nw) { 
        if( what == 0 ) 
            what = t;
        if( what == t ) { 
            assert( old == state );
            state = nw;
        }
    }

#else
    void check(RWLockBase1* t, int old, int nw) {}
#endif

    RWLockBase1::RWLockBase1() { 
        reading = writing = wantToWrite = 0;
    }

    RWLockBase1::~RWLockBase1() { 
        fassert(0, reading == 0 );
        fassert(0, writing == 0 );
        fassert(0, wantToWrite == 0);
    }

    void RWLockBase1::lockAsUpgradable() { lock(); }
    void RWLockBase1::unlockFromUpgradable() { unlock(); }
    void RWLockBase1::upgrade() { }

    void RWLockBase1::unlock_shared() { 
        check(this,1,0);
        boost::mutex::scoped_lock lk(m);
        assert( reading > 0 );
        reading--;
        if( reading == 0 && wantToWrite ) { 
            m_cond.notify_all();
        }
    }

    void RWLockBase1::lock_shared() { 
        check(this,0,1);
        while( 1 ) {
            {
                boost::mutex::scoped_lock lk(m);
                if( (wantToWrite|writing) == 0 ) {
                    reading++;
                    break;
                }
            }

            // TODO: this is wrong, we race pendingwriters for writer
            //       which won't crash but could starve it or fail 
            //       greedy semantic.

            // wait for writer to finish, then try again
            boost::mutex::scoped_lock lk(writer);
        }
    }

    bool RWLockBase1::lock_shared_try(int millis) { 
        // TODO 
        return false;
    }

    void RWLockBase1::lock() {
        check(this,0,-1);
        boost::mutex::scoped_lock lk(m);
        wantToWrite++;
        while( reading ) {
            // wait for the readers
            m_cond.wait(lk);
        }

        // writer is our exclusive mutex.
        // we spin -- there is only one contender within the code at this point because of 
        //   mutex m above, so at worst we spin one core. (**PER RWLOCK** that is)
        while( 1 ) {
            if( writer.try_lock() )
                break;
            boost::thread::yield();
        }

        assert( writing == 0 );
        writing++;
        wantToWrite--;
    }

    bool RWLockBase1::lock_try(int millis) { 
//        if( !m.timed_lock( boost::posix_time::milliseconds(millis) ) )
        if( !m.try_lock() )
            return false;

        if( reading ) {
            // TEMP NOT REALLY DONE COULD WAIT SOME HERE:
            m.unlock();
            return false;
        }
        wantToWrite++;
        bool ok = writer.try_lock();
//        bool ok = writer.timed_lock( boost::posix_time::milliseconds(millis) );
        wantToWrite--;
        if( ok ) {
            writing++;
            check(this, 0,-1);
        }
        m.unlock();
        return ok;
    }

    void RWLockBase1::unlock() {
        check(this,-1,0);
        assert( writing == 1 );
        writing--;
        writer.unlock();
    }

}

#endif
