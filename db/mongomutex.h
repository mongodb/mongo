// @file mongomutex.h

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

#pragma once

namespace mongo { 

    class MongoMutex {
    public:
        MongoMutex(const char * name) : _m(name) { }

        /** @return
         *    > 0  write lock
         *    = 0  no lock
         *    < 0  read lock
         */
        int getState() const { return _state.get(); }
        bool isWriteLocked() const { return getState() > 0; }
        void assertWriteLocked() const { 
            assert( getState() > 0 ); 
            DEV assert( !_releasedEarly.get() );
        }
        bool atLeastReadLocked() const { return _state.get() != 0; }
        void assertAtLeastReadLocked() const { assert(atLeastReadLocked()); }

        bool _checkWriteLockAlready(){
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

            curopWaitingForLock( 1 ); // stats
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

    private:
        MutexInfo _minfo;
        RWLock _m;

        /* > 0 write lock with recurse count
           < 0 read lock 
        */
        ThreadLocalValue<int> _state;

        /* See the releaseEarly() method.
           we use a separate TLS value for releasedEarly - that is ok as 
           our normal/common code path, we never even touch it */
        ThreadLocalValue<bool> _releasedEarly;
    };

    extern MongoMutex &dbMutex;

}
