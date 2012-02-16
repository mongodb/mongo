// @file qlock.h

#pragma once

#include <boost/noncopyable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition.hpp>
#include "../assert_util.h"
#include "../time_support.h"

namespace mongo { 

    /** "Quad Lock"
        we want to be able to do semi-granular locking now, and read/write style locking for that. 
        if that is all we want we could just have a rwlock per lockable entity, and we are done. 
        however at times we want to stop-the-world.  in addition, sometimes we want to stop the 
        world *for writing only*.  

        A hierarchy of locks could achieve this; instead here we've modeled it in one synchronization
        object our "QLock".  Our possible locked states are:

          w - i will write, and i will granularly lock after the qlock acquisition
          r - i will read,  and i will granularly lock after the qlock acquisition
          W - i will write globally. stop the world.
          R - i will read globally. stop any writer.

        For example there is a point during journal batch commits where we wish to block all writers 
        but no readers.

        Non-recursive.

          r w R W  <== lock that was around
        r - - - X
        w - - X X             - allowed
        R - X - X             X not allowed (blocks)
        W X X X X
        ^
        lock we are requesting
    */
    class QLock : boost::noncopyable { 
        struct Z { 
            Z() : n(0) { }
            boost::condition c;
            int n;
        };
        boost::mutex m;
        Z r,w,R,W;
        int greed;           // >0 if someone wants to acquire a write lock
        int greedyWrites;
        int stopped;         // greed stopped?
        void _stop_greed();  // we are already inlock for these underscore methods
        void _lock_W();
    public:
        QLock() : greedyWrites(1), greed(0), stopped(0) { }
        void lock_r();
        void lock_w();
        void lock_R();
        bool lock_R_try(int millis);
        void lock_W();
        bool lock_W_try(int millis);
        void lock_W_stop_greed();
        void unlock_r();
        void unlock_w();
        void unlock_R();
        void unlock_W();
        void start_greed();
        void stop_greed();
        void W_to_R();
        void R_to_W(); // caution see notes below
    };

    inline void QLock::_stop_greed() {
        if( ++stopped == 1 ) // recursion on stop_greed/start_greed is ok
            greedyWrites = 0;
    }
    inline void QLock::stop_greed() {
        boost::mutex::scoped_lock lk(m);
        _stop_greed();
    }

    inline void QLock::start_greed() { 
        boost::mutex::scoped_lock lk(m);
        if( --stopped == 0 ) 
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

    inline bool QLock::lock_R_try(int millis) {
        unsigned long long end = curTimeMillis64() + millis;
        boost::mutex::scoped_lock lk(m);
        while( 1 ) {
            if( greed + W.n + w.n == 0 )
                break;
            R.c.timed_wait(m, boost::posix_time::milliseconds(millis));
            if( greed + W.n + w.n == 0 )
                break;
            if( curTimeMillis64() >= end )
                return false;
        }
        R.n++;
        return true;
    }

    inline bool QLock::lock_W_try(int millis) { 
        unsigned long long end = curTimeMillis64() + millis;
        boost::mutex::scoped_lock lk(m);
        int g = greedyWrites;
        greed += g;
        while( 1 ) {
            if( W.n + R.n + w.n + r.n == 0 )
                break;
            W.c.timed_wait(m, boost::posix_time::milliseconds(millis));
            if( W.n + R.n + w.n + r.n == 0 )
                break;
            if( curTimeMillis64() >= end ) {
                // timed out
                dassert( greed > 0 );
                greed -= g;
                // we do notify_one on W.c so we should be careful not to leave someone 
                // else waiting when we give up here perhaps
                W.c.notify_one();
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
        if( greed ) {
            // writers await (greedily) but our R state stops them,
            // so no need to notify anyone
        }
        else {
            // no need to W.c.notify, you can't do W while R state is engaged
            R.c.notify_all();
            w.c.notify_all();
            r.c.notify_all();
        }
    }

    // upgrade from R to W state.
    // there is no "upgradable" state so this is NOT a classic upgrade - 
    // if two threads try to do this you will deadlock.
    inline void QLock::R_to_W() { 
        boost::mutex::scoped_lock lk(m);
        assert( R.n > 0 && W.n == 0 );
        int g = greedyWrites;
        greed += g;
        while( W.n + R.n + w.n + r.n > 1 ) {
            W.c.wait(m);
        }
        W.n++;
        R.n--;
        greed -= g;
        assert( R.n == 0 && W.n == 1 );
    }

    // "i will be writing. i will coordinate with no one. you better stop them all"
    inline void QLock::_lock_W() {
        int g = greedyWrites;
        greed += g;
        while( W.n + R.n + w.n + r.n ) {
            W.c.wait(m);
        }
        W.n++;
        greed -= g;
    }
    inline void QLock::lock_W() {
        boost::mutex::scoped_lock lk(m);
        _lock_W();
    }
    inline void QLock::lock_W_stop_greed() {
        boost::mutex::scoped_lock lk(m);
        _lock_W();
        _stop_greed();
    }

    inline void QLock::unlock_r() {
        boost::mutex::scoped_lock lk(m);
        fassert(0, r.n > 0);
        r.n--;
        if( R.n + w.n + r.n == 0 )
            W.c.notify_one(); // only thing we possibly would have blocked would be a W
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
        int writersWereQueued = greed;
        W.c.notify_one();
        if( writersWereQueued ) {
            // someone else would like to write, no need to notify further
        }
        else {
            R.c.notify_all();
            w.c.notify_all();
            r.c.notify_all();
        }
    }

}
