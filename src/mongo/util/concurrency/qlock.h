// @file qlock.h

#pragma once

#include <boost/thread/mutex.hpp>
#include <boost/thread/condition.hpp>
#include "../assert_util.h"

namespace mongo { 

    // "Quad Lock"
    class QLock : boost::noncopyable { 
        struct Z { 
            Z() : n(0) { }
            boost::condition c;
            int n;
        };
        boost::mutex m;
        Z r,w,R,W;
        int greed; // 1 if someone wants to acquire a write lock
        int greedyWrites;
        int stopped;
    public:
        QLock() : greedyWrites(1), greed(0), stopped(0) { }
        void lock_r();
        void lock_w();
        void lock_R();
        bool lock_R(int millis); // try
        void lock_W();
        bool lock_W(int millis); // try
        void unlock_r();
        void unlock_w();
        void unlock_R();
        void unlock_W();
        void start_greed();
        void stop_greed();
        void W_to_R();
    };

    inline void QLock::stop_greed() {
        boost::mutex::scoped_lock lk(m);
        if( ++stopped == 1 ) // ie recursion on stop_greed/start_greed is ok
            greedyWrites = 0;
    }

    inline void QLock::start_greed() { 
        boost::mutex::scoped_lock lk(m);
        if( --stopped == 1 ) 
            greedyWrites = 1;
    }

    // "i will be reading. i promise to coordinate my activities with w's as i go with more 
    //  granular locks."
    inline void QLock::lock_r() {
        boost::mutex::scoped_lock lk(m);
        while( greed + W.n ) {
            r.c.wait(m);
        }
        r.n++;
    }

    // "i will be writing. i promise to coordinate my activities with w's and r's as i go with more 
    //  granular locks."
    inline void QLock::lock_w() { 
        boost::mutex::scoped_lock lk(m);
        while( greed + W.n + R.n ) {
            w.c.wait(m);
        }
        w.n++;
    }

    // "i will be reading. i will coordinate with no one. you better stop them if they
    // are writing."
    inline void QLock::lock_R() {
        boost::mutex::scoped_lock lk(m);
        while( greed + W.n + w.n ) { 
            R.c.wait(m);
        }
        R.n++;
    }

    inline bool QLock::lock_R(int millis) {
        boost::mutex::scoped_lock lk(m);
        while( greed + W.n + w.n ) { 
            if( R.c.timed_wait(m, boost::posix_time::milliseconds(millis)) == false ) { 
                return false;
            }
        }
        R.n++;
        return true;
    }

    inline bool QLock::lock_W(int millis) { 
        boost::mutex::scoped_lock lk(m);
        int g = greedyWrites;
        greed += g;
        while( W.n + R.n + w.n + r.n ) {
            if( W.c.timed_wait(m, boost::posix_time::milliseconds(millis)) == false ) { 
                // timed out
                dassert( greed > 0 );
                greed -= g;
                return false;
            }
        }
        W.n += 1;
        dassert( W.n == 1 );
        greed -= g;
        return true;
    }

    // downgrade from W state to R state
    inline void QLock::W_to_R() { 
        boost::mutex::scoped_lock lk(m);
        assert( W.n == 1 );
        assert( R.n == 0 );
        W.n = 0;
        R.n = 1;
    }

    // "i will be writing. i will coordinate with no one. you better stop them all"
    inline void QLock::lock_W() { // lock the world for writing 
        boost::mutex::scoped_lock lk(m);
        int g = greedyWrites;
        greed += g;
        while( W.n + R.n + w.n + r.n ) {
            W.c.wait(m);
        }
        W.n++;
        greed -= g;
    }

    inline void QLock::unlock_r() {
        boost::mutex::scoped_lock lk(m);
        fassert(0, r.n > 0);
        r.n--;
        if( R.n + w.n + r.n == 0 )
            W.c.notify_one();
    }
    inline void QLock::unlock_w() {
        boost::mutex::scoped_lock lk(m);
        fassert(0, w.n > 0);
        w.n--;
        if( w.n == 0 ) {
            W.c.notify_one();
            R.c.notify_all();
        }
    }
    inline void QLock::unlock_R() {
        boost::mutex::scoped_lock lk(m);
        fassert(0, R.n > 0);
        R.n--;
        if( R.n == 0 ) {
            W.c.notify_one();
            w.c.notify_all();
        }
    }    
    inline void QLock::unlock_W() {
        boost::mutex::scoped_lock lk(m);
        W.n--;
        dassert( W.n == 0 );
        W.c.notify_one();
        if( greed ) {
            // someone else would like to write, no need to notify further
        }
        else {
            R.c.notify_all();
            w.c.notify_all();
            r.c.notify_all();
        }
    }

}
