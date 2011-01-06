// @file concurrency.h

/*
 *    Copyright (C) 2010 10gen Inc.
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
 */

/*mongod concurrency rules & notes will be placed here.

   Mutex heirarchy (1 = "leaf")
     name                   level
     Logstream::mutex       1
     ClientCursor::ccmutex  2
     dblock                 3

     End func name with _inlock to indicate "caller must lock before calling".
*/

#pragma once

#include "../util/concurrency/rwlock.h"
#include "../util/mmap.h"
#include "../util/time_support.h"

namespace mongo {

    string sayClientState();
    bool haveClient();

    class Client;
    Client* curopWaitingForLock( int type );
    void curopGotLock(Client*);

    /* mutex time stats */
    class MutexInfo {
        unsigned long long enter, timeLocked; // microseconds
        int locked;
        unsigned long long start; // last as we touch this least often

    public:
        MutexInfo() : timeLocked(0) , locked(0) {
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
        int isLocked() const { return locked; }
        void getTimingInfo(unsigned long long &s, unsigned long long &tl) const {
            s = start;
            tl = timeLocked;
        }
        unsigned long long getTimeLocked() const { return timeLocked; }
    };

}

#include "mongomutex.h"

namespace mongo {

    inline void dbunlocking_write() { }
    inline void dbunlocking_read() { }

    struct writelock {
        writelock() { dbMutex.lock(); }
        writelock(const string& ns) { dbMutex.lock(); }
        ~writelock() {
            DESTRUCTOR_GUARD(
                dbunlocking_write();
                dbMutex.unlock();
            );
        }
    };

    struct readlock {
        readlock(const string& ns) {
            dbMutex.lock_shared();
        }
        readlock() { dbMutex.lock_shared(); }
        ~readlock() {
            DESTRUCTOR_GUARD(
                dbunlocking_read();
                dbMutex.unlock_shared();
            );
        }
    };

    struct readlocktry {
        readlocktry( const string&ns , int tryms ) {
            _got = dbMutex.lock_shared_try( tryms );
        }
        ~readlocktry() {
            if ( _got ) {
                dbunlocking_read();
                dbMutex.unlock_shared();
            }
        }
        bool got() const { return _got; }
    private:
        bool _got;
    };

    struct writelocktry {
        writelocktry( const string&ns , int tryms ) {
            _got = dbMutex.lock_try( tryms );
        }
        ~writelocktry() {
            if ( _got ) {
                dbunlocking_read();
                dbMutex.unlock();
            }
        }
        bool got() const { return _got; }
    private:
        bool _got;
    };

    struct readlocktryassert : public readlocktry {
        readlocktryassert(const string& ns, int tryms) :
            readlocktry(ns,tryms) {
            uassert(13142, "timeout getting readlock", got());
        }
    };

    /** assure we have at least a read lock - they key with this being
        if you have a write lock, that's ok too.
    */
    struct atleastreadlock {
        atleastreadlock( const string& ns ) {
            _prev = dbMutex.getState();
            if ( _prev == 0 )
                dbMutex.lock_shared();
        }
        ~atleastreadlock() {
            if ( _prev == 0 )
                dbMutex.unlock_shared();
        }
    private:
        int _prev;
    };

    /* parameterized choice of read or write locking
       use readlock and writelock instead of this when statically known which you want
    */
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
            DESTRUCTOR_GUARD(
            if( _writelock ) {
            dbunlocking_write();
                dbMutex.unlock();
            }
            else {
                dbunlocking_read();
                dbMutex.unlock_shared();
            }
            );
        }
        /* this unlocks, does NOT upgrade. that works for our current usage */
        void releaseAndWriteLock();
    };

    /* deprecated - use writelock and readlock instead */
    struct dblock : public writelock {
        dblock() : writelock("") { }
    };

    // eliminate this - we should just type "dbMutex.assertWriteLocked();" instead
    inline void assertInWriteLock() { dbMutex.assertWriteLocked(); }

}
