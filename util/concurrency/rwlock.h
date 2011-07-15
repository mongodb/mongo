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
 */

#pragma once

#include "mutex.h"
#include "../time_support.h"
#include "rwlockimpl.h"

namespace mongo {

    class RWLock : public RWLockBase { 
        const int _lowPriorityWaitMS;
    public:
        const char * const _name;
        int lowPriorityWaitMS() const { return _lowPriorityWaitMS; }
        RWLock(const char *name, int lowPriorityWait=0) : _lowPriorityWaitMS(lowPriorityWait) , _name(name) { }
        void lock() {
            RWLockBase::lock();
            DEV mutexDebugger.entering(_name);
        }
        void unlock() {
            DEV mutexDebugger.leaving(_name);
            RWLockBase::unlock();
        }
        void lockAsUpgradable() { RWLockBase::lockAsUpgradable(); }
        void unlockFromUpgradable() { // upgradable -> unlocked
            RWLockBase::unlockFromUpgradable();
        }
        void upgrade() { // upgradable -> exclusive lock
            RWLockBase::upgrade();
            DEV mutexDebugger.entering(_name);
        }
        void lock_shared() { RWLockBase::lock_shared(); }
        void unlock_shared() { RWLockBase::unlock_shared(); }
        bool lock_shared_try( int millis ) { return RWLockBase::lock_shared_try(millis); }
        bool lock_try( int millis = 0 ) {
            if( RWLockBase::lock_try(millis) ) {
                DEV mutexDebugger.entering(_name);
                return true;
            }
            return false;
        }
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
                    
                    if ( ! lowPriorityWaitMS && lock.lowPriorityWaitMS() )
                        lowPriorityWaitMS = lock.lowPriorityWaitMS();
                    
                    if ( lowPriorityWaitMS ) { 
                        bool got = false;
                        for ( int i=0; i<lowPriorityWaitMS; i++ ) {
                            if ( _lock.lock_try(0) ) {
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
                            log() << "couldn't get lazy rwlock" << endl;
                            _lock.lock();
                        }
                    }
                    else { 
                        _lock.lock();
                    }

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

    /** recursive on shared locks is ok for this implementation */
    class RWLockRecursive : boost::noncopyable {
        ThreadLocalValue<int> _state;
        RWLock _lk;
        friend class Exclusive;
    public:
        /** @param lpwaitms lazy wait */
        RWLockRecursive(const char *name, int lpwaitms) : _lk(name, lpwaitms) { }

        void assertExclusivelyLocked() {
            dassert( _state.get() < 0 );
        }

        // RWLockRecursive::Exclusive scoped lock
        class Exclusive : boost::noncopyable { 
            RWLockRecursive& _r;
            rwlock *_scopedLock;
        public:
            Exclusive(RWLockRecursive& r) : _r(r), _scopedLock(0) {
                int s = _r._state.get();
                dassert( s <= 0 );
                if( s == 0 )
                    _scopedLock = new rwlock(_r._lk, true);
                _r._state.set(s-1);
            }
            ~Exclusive() {
                int s = _r._state.get();
                DEV wassert( s < 0 ); // wassert: don't throw from destructors
                _r._state.set(s+1);
                delete _scopedLock;
            }
        };

        // RWLockRecursive::Shared scoped lock
        class Shared : boost::noncopyable { 
            RWLockRecursive& _r;
            bool _alreadyExclusive;
        public:
            Shared(RWLockRecursive& r) : _r(r) {
                int s = _r._state.get();
                _alreadyExclusive = s < 0;
                if( !_alreadyExclusive ) {
                    dassert( s >= 0 ); // -1 would mean exclusive
                    if( s == 0 )
                        _r._lk.lock_shared(); 
                    _r._state.set(s+1);
                }
            }
            ~Shared() {
                if( _alreadyExclusive ) {
                    DEV wassert( _r._state.get() < 0 );
                }
                else {
                    int s = _r._state.get() - 1;
                    if( s == 0 ) 
                        _r._lk.unlock_shared();
                    _r._state.set(s);
                    DEV wassert( s >= 0 );
                }
            }
        };
    };
}
