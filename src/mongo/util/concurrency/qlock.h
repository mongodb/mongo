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
    public:
        void lock_r();
        void lock_w();
        void lock_R();
        void lock_W();
        void unlock_r();
        void unlock_w();
        void unlock_R();
        void unlock_W();
    };

    // "i will be reading. i promise to coordinate my activities with w's as i go with more 
    //  granular locks."
    inline void QLock::lock_r() {
        boost::mutex::scoped_lock lk(m);
        while( 1 ) {
            if( W.n == 0 )
                break;
            r.c.wait(m);
        }
        r.n++;
    }

    // "i will be writing. i promise to coordinate my activities with w's and r's as i go with more 
    //  granular locks."
    inline void QLock::lock_w() { 
        boost::mutex::scoped_lock lk(m);
        while( 1 ) {
            if( W.n + R.n == 0 )
                break;
            w.c.wait(m);
        }
        w.n++;
    }

    // "i will be reading. i will coordinate with no one. you better stop them if they
    // are writing."
    inline void QLock::lock_R() {
        boost::mutex::scoped_lock lk(m);
        while( 1 ) {
            if( W.n + w.n == 0 )
                break;
            R.c.wait(m);
        }
        R.n++;
    }

    // "i will be writing. i will coordinate with no one. you better stop them all"
    inline void QLock::lock_W() { // lock the world for writing 
        boost::mutex::scoped_lock lk(m);
        W.n++; // ahead of time so we are then greedy
        while( 1 ) {
            if( W.n + R.n + w.n + r.n == 1 )
                break;
            W.c.wait(m);
        }
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
        --W.n;
        if( W.n ) {
            W.c.notify_one();
        }
        else {
            R.c.notify_all();
            w.c.notify_all();
            r.c.notify_all();
        }
    }

}
