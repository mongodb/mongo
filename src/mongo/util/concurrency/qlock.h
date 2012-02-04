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
        int pre, post;
    public:
        QLock() : pre(1),post(0) { }
        void lock_r();
        void lock_w();
        void lock_R();
        void start_greed();
        void stop_greed();
        void lock_W();
        bool lock_W(int millis);
        void unlock_r();
        void unlock_w();
        void unlock_R();
        void unlock_W();
    };

    inline void QLock::stop_greed() {
        boost::mutex::scoped_lock lk(m);
        pre = 0; 
        post = 1;
    }

    inline void QLock::start_greed() { 
        boost::mutex::scoped_lock lk(m);
        pre = 1;
        post = 0;
    }

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

    inline bool QLock::lock_W(int millis) { 
        boost::mutex::scoped_lock lk(m);
        int pre=this->pre; // snapshot the pre post greedy values. the case they change on you in the loop is when going from nongreedy to greedy
        int post=this->post;
        W.n += pre; // ahead of time so we are then greedy
        while( W.n + R.n + w.n + r.n != pre ) {
            if( W.c.timed_wait(m, boost::posix_time::milliseconds(millis)) == false ) { 
                // timed out
                dassert( W.n > 0 );
                W.n -= pre;
                return false;
            }
        }
        W.n += post;
        return true;
    }

    // "i will be writing. i will coordinate with no one. you better stop them all"
    inline void QLock::lock_W() { // lock the world for writing 
        boost::mutex::scoped_lock lk(m);
        int pre=this->pre; // snapshot the pre post greedy values. the case they change on you in the loop is when going from nongreedy to greedy
        int post=this->post;
        W.n += pre; // ahead of time so we are then greedy
        while( W.n + R.n + w.n + r.n != pre ) {
            W.c.wait(m);
        }
        W.n += post;
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
        W.n -= pre;
        W.n -= post;
        if( W.n ) {
            // someone else would like to write
            dassert( pre == 1 );
            W.c.notify_one();
        }
        else {
            R.c.notify_all();
            w.c.notify_all();
            r.c.notify_all();
        }
    }

}
