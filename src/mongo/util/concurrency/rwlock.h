// @file rwlock.h generic reader-writer lock (cross platform support)

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

#include "mutex.h"
#include "../time_support.h"
#include "rwlockimpl.h"
#if defined(_DEBUG)
#include "mutexdebugger.h"
#endif
#include "simplerwlock.h"

namespace mongo {

    class RWLock : public RWLockBase { 
        enum { NilState, UpgradableState, Exclusive } x; // only bother to set when doing upgradable related things
    public:
        const char * const _name;
        RWLock(const char *name) : _name(name) { 
            x = NilState;
        }
        void lock() {
            RWLockBase::lock();
#if defined(_DEBUG)
            mutexDebugger.entering(_name);
#endif
        }
        void unlock() {
#if defined(_DEBUG)            
            mutexDebugger.leaving(_name);
#endif
            RWLockBase::unlock();
        }

        void lock_shared() { RWLockBase::lock_shared(); }
        void unlock_shared() { RWLockBase::unlock_shared(); }
    private:
        void lockAsUpgradable() { RWLockBase::lockAsUpgradable(); }
        void unlockFromUpgradable() { // upgradable -> unlocked
            RWLockBase::unlockFromUpgradable();
        }
    public:
        void upgrade() { // upgradable -> exclusive lock
            verify( x == UpgradableState );
            RWLockBase::upgrade();
            x = Exclusive;
        }

        bool lock_shared_try( int millis ) { return RWLockBase::lock_shared_try(millis); }

        bool lock_try( int millis = 0 ) {
            if( RWLockBase::lock_try(millis) ) {
#if defined(_DEBUG)            
                mutexDebugger.entering(_name);
#endif
                return true;
            }
            return false;
        }

        /** acquire upgradable state.  You must be unlocked before creating.
            unlocks on destruction, whether in upgradable state or upgraded to exclusive
            in the interim.
            */
        class Upgradable : boost::noncopyable { 
            RWLock& _r;
        public:
            Upgradable(RWLock& r) : _r(r) { 
                r.lockAsUpgradable();
                verify( _r.x == NilState );
                _r.x = RWLock::UpgradableState;
            }
            ~Upgradable() {
                if( _r.x == RWLock::UpgradableState ) {
                    _r.x = NilState;
                    _r.unlockFromUpgradable();
                }
                else {
                    //TEMP                     verify( _r.x == Exclusive ); // has been upgraded
                    _r.x = NilState;
                    _r.unlock();
                }
            }
        };
    };

    /** throws on failure to acquire in the specified time period. */
    class rwlock_try_write : boost::noncopyable {
    public:
        struct exception { };
        rwlock_try_write(RWLock& l, int millis = 0) : _l(l) {
            if( !l.lock_try(millis) )
                throw exception();
        }
        ~rwlock_try_write() { _l.unlock(); }
    private:
        RWLock& _l;
    };

    class rwlock_shared : boost::noncopyable {
    public:
        rwlock_shared(RWLock& rwlock) : _r(rwlock) {_r.lock_shared(); }
        ~rwlock_shared() { _r.unlock_shared(); }
    private:
        RWLock& _r;
    };

    /* scoped lock for RWLock */
    class rwlock : boost::noncopyable {
    public:
        /**
         * @param write acquire write lock if true sharable if false
         * @param lowPriority if > 0, will try to get the lock non-greedily for that many ms
         */
        rwlock( const RWLock& lock , bool write, /* bool alreadyHaveLock = false , */int lowPriorityWaitMS = 0 )
            : _lock( (RWLock&)lock ) , _write( write ) {            
            {
                if ( _write ) {
                    _lock.lock();
                }
                else { 
                    _lock.lock_shared();
                }
            }
        }
        ~rwlock() {
            if ( _write )
                _lock.unlock();
            else
                _lock.unlock_shared();
        }
    private:
        RWLock& _lock;
        const bool _write;
    };

    // ----------------------------------------------------------------------------------------

    /** recursive on shared locks is ok for this implementation */
    class RWLockRecursive : protected RWLockBase {
    protected:
        ThreadLocalValue<int> _state;
        void lock(); // not implemented - Lock() should be used; didn't overload this name to avoid mistakes
        virtual void Lock() { RWLockBase::lock(); }
    public:
        virtual ~RWLockRecursive() { }
        const char * const _name;
        RWLockRecursive(const char *name) : _name(name) { }

        void assertAtLeastReadLocked() { 
            verify( _state.get() != 0 );
        }
        void assertExclusivelyLocked() { 
            verify( _state.get() < 0 );
        }

        class Exclusive : boost::noncopyable { 
            RWLockRecursive& _r;
        public:
            Exclusive(RWLockRecursive& r) : _r(r) {
                int s = _r._state.get();
                dassert( s <= 0 );
                if( s == 0 )
                    _r.Lock();
                _r._state.set(s-1);
            }
            ~Exclusive() {
                int s = _r._state.get();
                DEV wassert( s < 0 ); // wassert: don't throw from destructors
                ++s;
                _r._state.set(s);
                if ( s == 0 )
                    _r.unlock();
            }
        };

        class Shared : boost::noncopyable { 
            RWLockRecursive& _r;
            bool _alreadyLockedExclusiveByUs;
        public:
            Shared(RWLockRecursive& r) : _r(r) {
                int s = _r._state.get();
                _alreadyLockedExclusiveByUs = s < 0;
                if( !_alreadyLockedExclusiveByUs ) {
                    dassert( s >= 0 ); // -1 would mean exclusive
                    if( s == 0 )
                        _r.lock_shared(); 
                    _r._state.set(s+1);
                }
            }
            ~Shared() {
                if( _alreadyLockedExclusiveByUs ) {
                    DEV wassert( _r._state.get() < 0 );
                }
                else {
                    int s = _r._state.get() - 1;
                    DEV wassert( s >= 0 );
                    _r._state.set(s);
                    if( s == 0 ) 
                        _r.unlock_shared();
                }
            }
        };
    };

    class RWLockRecursiveNongreedy : public RWLockRecursive { 
        virtual void Lock() { 
            bool got = false;
            for ( int i=0; i<lowPriorityWaitMS; i++ ) {
                if ( lock_try(0) ) {
                    got = true;
                    break;
                }                            
                int sleep = 1;
                if ( i > ( lowPriorityWaitMS / 20 ) )
                    sleep = 10;
                sleepmillis(sleep);
                i += ( sleep - 1 );
            }
            if ( ! got ) {
                log() << "couldn't lazily get rwlock" << endl;
                RWLockBase::lock();
            }
        }

    public:
        const int lowPriorityWaitMS;
        RWLockRecursiveNongreedy(const char *nm, int lpwaitms) : RWLockRecursive(nm), lowPriorityWaitMS(lpwaitms) { }
        const char * implType() const { return RWLockRecursive::implType(); }

        //just for testing:
        bool __lock_try( int millis ) { return RWLockRecursive::lock_try(millis); }
    };

}
