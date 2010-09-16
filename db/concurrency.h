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

#include "../util/concurrency/rwlock.h"
#include "../util/mmap.h"
#include "../util/time_support.h"

namespace mongo {

    string sayClientState();
    bool haveClient();
    
    void curopWaitingForLock( int type );
    void curopGotLock();

    /* mutex time stats */
    class MutexInfo {
        unsigned long long start, enter, timeLocked; // all in microseconds
        int locked;

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
        int isLocked() const {
            return locked;
        }
        void getTimingInfo(unsigned long long &s, unsigned long long &tl) const {
            s = start;
            tl = timeLocked;
        }
        unsigned long long getTimeLocked() const {
            return timeLocked;
        }
    };

    class MongoMutex {
        MutexInfo _minfo;
        RWLock _m;
        ThreadLocalValue<int> _state;

        /* we use a separate TLS value for releasedEarly - that is ok as 
           our normal/common code path, we never even touch it.
        */
        ThreadLocalValue<bool> _releasedEarly;
    public:
        MongoMutex(const char * name) : _m(name) { }

        /**
         * @return
         *    > 0  write lock
         *    = 0  no lock
         *    < 0  read lock
         */
        int getState() { return _state.get(); }
        bool isWriteLocked() { return getState() > 0; }
        void assertWriteLocked() { 
            assert( getState() > 0 ); 
            DEV assert( !_releasedEarly.get() );
        }
        bool atLeastReadLocked() { return _state.get() != 0; }
        void assertAtLeastReadLocked() { assert(atLeastReadLocked()); }

        bool _checkWriteLockAlready(){
            //DEV cout << "LOCK" << endl;
            DEV assert( haveClient() );
                
            int s = _state.get();
            if( s > 0 ) {
                _state.set(s+1);
                return true;
            }

            massert( 10293 , (string)"internal error: locks are not upgradeable: " + sayClientState() , s == 0 );

            return false;
        }

        void lock() { 
            if ( _checkWriteLockAlready() )
                return;
            
            _state.set(1);

            curopWaitingForLock( 1 );
            _m.lock(); 
            curopGotLock();

            _minfo.entered();

            MongoFile::lockAll();
        }

        bool lock_try( int millis ) { 
            if ( _checkWriteLockAlready() )
                return true;

            curopWaitingForLock( 1 );
            bool got = _m.lock_try( millis ); 
            curopGotLock();
            
            if ( got ){
                _minfo.entered();
                _state.set(1);
                MongoFile::lockAll();
            }                
            
            return got;
        }


        void unlock() { 
            //DEV cout << "UNLOCK" << endl;
            int s = _state.get();
            if( s > 1 ) { 
                _state.set(s-1);
                return;
            }
            if( s != 1 ) { 
                if( _releasedEarly.get() ) { 
                    _releasedEarly.set(false);
                    return;
                }
                massert( 12599, "internal error: attempt to unlock when wasn't in a write lock", false);
            }

            MongoFile::unlockAll();

            _state.set(0);
            _minfo.leaving();
            _m.unlock(); 
        }

        /* unlock (write lock), and when unlock() is called later, 
           be smart then and don't unlock it again.
           */
        void releaseEarly() {
            assert( getState() == 1 ); // must not be recursive
            assert( !_releasedEarly.get() );
            _releasedEarly.set(true);
            unlock();
        }

        void lock_shared() { 
            //DEV cout << " LOCKSHARED" << endl;
            int s = _state.get();
            if( s ) {
                if( s > 0 ) { 
                    // already in write lock - just be recursive and stay write locked
                    _state.set(s+1);
                    return;
                }
                else { 
                    // already in read lock - recurse
                    _state.set(s-1);
                    return;
                }
            }
            _state.set(-1);
            curopWaitingForLock( -1 );
            _m.lock_shared(); 
            curopGotLock();
        }
        
        bool lock_shared_try( int millis ) {
            int s = _state.get();
            if ( s ){
                // we already have a lock, so no need to try
                lock_shared();
                return true;
            }

            bool got = _m.lock_shared_try( millis );
            if ( got )
                _state.set(-1);
            return got;
        }
        
        void unlock_shared() { 
            //DEV cout << " UNLOCKSHARED" << endl;
            int s = _state.get();
            if( s > 0 ) { 
                assert( s > 1 ); /* we must have done a lock write first to have s > 1 */
                _state.set(s-1);
                return;
            }
            if( s < -1 ) { 
                _state.set(s+1);
                return;
            }
            assert( s == -1 );
            _state.set(0);
            _m.unlock_shared(); 
        }
        
        MutexInfo& info() { return _minfo; }
    };

    extern MongoMutex &dbMutex;

    inline void dbunlocking_write() { }
    inline void dbunlocking_read() { }

    struct writelock {
        writelock(const string& ns) {
            dbMutex.lock();
        }
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
        ~readlock() { 
            DESTRUCTOR_GUARD(
                dbunlocking_read();
                dbMutex.unlock_shared();
            );
        }
    };	

    struct readlocktry {
        readlocktry( const string&ns , int tryms ){
            _got = dbMutex.lock_shared_try( tryms );
        }
        ~readlocktry() {
            if ( _got ){
                dbunlocking_read();
                dbMutex.unlock_shared();
            }
        }
        bool got() const { return _got; }
    private:
        bool _got;
    };

    struct writelocktry {
        writelocktry( const string&ns , int tryms ){
            _got = dbMutex.lock_try( tryms );
        }
        ~writelocktry() {
            if ( _got ){
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
        atleastreadlock( const string& ns ){
            _prev = dbMutex.getState();
            if ( _prev == 0 )
                dbMutex.lock_shared();
        }
        ~atleastreadlock(){
            if ( _prev == 0 )
                dbMutex.unlock_shared();
        }
    private:
        int _prev;
    };

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
                } else {
                    dbunlocking_read();
                    dbMutex.unlock_shared();
                }
            );
        }
        /* this unlocks, does NOT upgrade. that works for our current usage */
        void releaseAndWriteLock();
    };
    
    /* use writelock and readlock instead */
    struct dblock : public writelock {
        dblock() : writelock("") { }
    };

    // eliminate
    inline void assertInWriteLock() { dbMutex.assertWriteLocked(); }

}
