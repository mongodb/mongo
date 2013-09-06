// @file qlock.h

/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

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

          r w R W X  <== lock that was around
        r * * * - -
        w * * - - -            * allowed
        R * - * - -            - not allowed (blocks)
        W - - - - -            ! See NOTE(!).
        X - ! - - -
        ^
        lock we are requesting

        NOTE(!): The "X" state can only be reached from the "w" state.  A thread successfully
        transitions from "w" to "X" when w_to_X() returns true, and fails to transition to that
        state (remaining in "w") when that function returns false.  For one thread to successfully
        transition, all threads in the "w" state must be blocked in w_to_X().  When all threads in
        the "w" state are blocked in w_to_X(), one thread will be released in the X state.  The
        other threads remain blocked in w_to_X() until the thread in the X state calls X_to_w().
    */
    class QLock : boost::noncopyable {
        struct Z { 
            Z() : n(0) { }
            boost::condition c;
            int n;
        };
        boost::mutex m;
        Z r,w,R,W,U,X;
        int numPendingGlobalWrites;  // >0 if someone wants to acquire a write lock
        long long generationX;
        long long generationXExit;
        void _lock_W();
        void _unlock_R();
        bool _areQueueJumpingGlobalWritesPending() const {
            return numPendingGlobalWrites > 0;
        }

        bool W_legal() const { return r.n + w.n + R.n + W.n + X.n == 0; }
        bool R_legal_ignore_greed() const { return w.n + W.n + X.n == 0; }
        bool r_legal_ignore_greed() const { return W.n + X.n == 0; }
        bool w_legal_ignore_greed() const { return R.n + W.n + X.n == 0; }

        bool R_legal() const {
            return !_areQueueJumpingGlobalWritesPending() && R_legal_ignore_greed();
        }

        bool w_legal() const {
            return !_areQueueJumpingGlobalWritesPending() && w_legal_ignore_greed();
        }

        bool r_legal() const {
            return !_areQueueJumpingGlobalWritesPending() && r_legal_ignore_greed();
        }

        bool X_legal() const { return w.n + r.n + R.n + W.n == 0; }

        void notifyWeUnlocked(char me);
        static bool i_block(char me, char them);
    public:
        QLock() :
            numPendingGlobalWrites(0),
            generationX(0),
            generationXExit(0) {
        }

        void lock_r();
        void lock_w();
        void lock_R();
        bool lock_R_try(int millis);
        void lock_W();
        bool lock_W_try(int millis);
        void unlock_r();
        void unlock_w();
        void unlock_R();
        void unlock_W();
        void W_to_R();
        void R_to_W(); // caution see notes below
        bool w_to_X();
        void X_to_w();
    };

    inline bool QLock::i_block(char me, char them) {
        switch( me ) {
        case 'W' : return true;
        case 'R' : return them == 'W' || them == 'w' || them == 'X';
        case 'w' : return them == 'W' || them == 'R' || them == 'X';
        case 'r' : return them == 'W' || them == 'X';
        case 'X' : return true;
        default  : fassertFailed(16200);
        }
        return false;
    }

    inline void QLock::notifyWeUnlocked(char me) {
        fassert(16201, W.n == 0);
        if ( me == 'X' ) {
            X.c.notify_all();
        }
        if( U.n ) {
            // U is highest priority
            if( (r.n + w.n + W.n + X.n == 0) && (R.n == 1) ) {
                U.c.notify_one();
                return;
            }
        }
        if ( X_legal() && i_block(me, 'X') ) {
            X.c.notify_one();
        }
        if ( W_legal() && i_block(me, 'W') ) {
            W.c.notify_one();
            if( _areQueueJumpingGlobalWritesPending() )
                return;
        }
        if ( R_legal_ignore_greed() && i_block(me, 'R') ) {
            R.c.notify_all();
        }
        if ( w_legal_ignore_greed() && i_block(me, 'w') ) {
            w.c.notify_all();
        }
        if ( r_legal_ignore_greed() && i_block(me, 'r') ) {
            r.c.notify_all();
        }
    }

    // "i will be reading. i promise to coordinate my activities with w's as i go with more 
    //  granular locks."
    inline void QLock::lock_r() {
        boost::mutex::scoped_lock lk(m);
        while( !r_legal() ) {
            r.c.wait(m);
        }
        r.n++;
    }

    // "i will be writing. i promise to coordinate my activities with w's and r's as i go with more 
    //  granular locks."
    inline void QLock::lock_w() { 
        boost::mutex::scoped_lock lk(m);
        while( !w_legal() ) {
            w.c.wait(m);
        }
        w.n++;
    }

    // "i will be reading. i will coordinate with no one. you better stop them if they
    // are writing."
    inline void QLock::lock_R() {
        boost::mutex::scoped_lock lk(m);
        while( ! R_legal() ) {
            R.c.wait(m);
        }
        R.n++;
    }

    inline bool QLock::lock_R_try(int millis) {
        unsigned long long end = curTimeMillis64() + millis;
        boost::mutex::scoped_lock lk(m);
        while( !R_legal() && curTimeMillis64() < end ) {
            R.c.timed_wait(m, boost::posix_time::milliseconds(millis));
        }
        if ( R_legal() ) {
            R.n++;
            return true;
        }
        return false;
    }

    inline bool QLock::lock_W_try(int millis) {
        unsigned long long end = curTimeMillis64() + millis;
        boost::mutex::scoped_lock lk(m);

        ++numPendingGlobalWrites;
        while (!W_legal() && curTimeMillis64() < end) {
            W.c.timed_wait(m, boost::posix_time::milliseconds(millis));
        }
        --numPendingGlobalWrites;

        if (W_legal()) {
            W.n++;
            fassert( 16202, W.n == 1 );
            return true;
        }

        return false;
    }


    // downgrade from W state to R state
    inline void QLock::W_to_R() { 
        boost::mutex::scoped_lock lk(m);
        fassert(16203, W.n == 1);
        fassert(16204, R.n == 0);
        fassert(16205, U.n == 0);
        W.n = 0;
        R.n = 1;
        notifyWeUnlocked('W');
    }

    // upgrade from R to W state.
    //
    // This transition takes precedence over all pending requests by threads to enter
    // any state other than '\0'.
    //
    // there is no "upgradable" state so this is NOT a classic upgrade -
    // if two threads try to do this you will deadlock.
    //
    // NOTE: ONLY CALL THIS FUNCTION ON A THREAD THAT GOT TO R BY CALLING W_to_R(), OR
    // YOU MAY DEADLOCK WITH THREADS LEAVING THE X STATE.
    inline void QLock::R_to_W() { 
        boost::mutex::scoped_lock lk(m);
        fassert(16206, R.n > 0);
        fassert(16207, W.n == 0);
        fassert(16208, U.n == 0);

        U.n = 1;

        ++numPendingGlobalWrites;

        while( W.n + R.n + w.n + r.n > 1 ) {
            U.c.wait(m);
        }
        --numPendingGlobalWrites;

        fassert(16209, R.n == 1);
        fassert(16210, W.n == 0);
        fassert(16211, U.n == 1);

        R.n = 0;
        W.n = 1;
        U.n = 0;
    }

    inline bool QLock::w_to_X() {
        boost::mutex::scoped_lock lk(m);

        fassert( 16212, w.n > 0 );

        ++X.n;
        --w.n;

        long long myGeneration = generationX;

        while ( !X_legal() && (myGeneration == generationX) )
            X.c.wait(m);

        if ( myGeneration == generationX ) {
            fassert( 16214, X_legal() );
            fassert( 16215, w.n == 0 );
            ++generationX;
            notifyWeUnlocked('w');
            return true;
        }

        while ( myGeneration == generationXExit )
            X.c.wait(m);

        fassert( 16216, R.n == 0 );
        fassert( 16217, w.n > 0 );
        return false;
    }

    inline void QLock::X_to_w() {
        boost::mutex::scoped_lock lk(m);

        fassert( 16219, W.n == 0 );
        fassert( 16220, R.n == 0 );
        fassert( 16221, w.n == 0 );
        fassert( 16222, X.n > 0 );

        w.n = X.n;
        X.n = 0;
        ++generationXExit;
        notifyWeUnlocked('X');
    }

    // "i will be writing. i will coordinate with no one. you better stop them all"
    inline void QLock::_lock_W() {
        ++numPendingGlobalWrites;
        while( !W_legal() ) {
            W.c.wait(m);
        }
        --numPendingGlobalWrites;
        W.n++;
    }
    inline void QLock::lock_W() {
        boost::mutex::scoped_lock lk(m);
        _lock_W();
    }

    inline void QLock::unlock_r() {
        boost::mutex::scoped_lock lk(m);
        fassert(16137, r.n > 0);
        --r.n;
        notifyWeUnlocked('r');
    }
    inline void QLock::unlock_w() {
        boost::mutex::scoped_lock lk(m);
        fassert(16138, w.n > 0);
        --w.n;
        notifyWeUnlocked('w');
    }

    inline void QLock::unlock_R() {
        boost::mutex::scoped_lock lk(m);
        _unlock_R();
    }

    inline void QLock::_unlock_R() {
        fassert(16139, R.n > 0);
        --R.n;
        notifyWeUnlocked('R');
    }

    inline void QLock::unlock_W() {
        boost::mutex::scoped_lock lk(m);
        fassert(16140, W.n == 1);
        --W.n;
        notifyWeUnlocked('W');
    }

}
