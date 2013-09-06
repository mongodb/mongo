// spin_lock.h

/**
*    Copyright (C) 2008 10gen Inc.
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

#ifdef _WIN32
#include "mongo/platform/windows_basic.h"
#endif

#include "mutex.h"

namespace mongo {

    /**
     * The spinlock currently requires late GCC support routines to be efficient.
     * Other platforms default to a mutex implemenation.
     */
    class SpinLock : boost::noncopyable {
    public:
        SpinLock();
        ~SpinLock();

        static bool isfast(); // true if a real spinlock on this platform

    private:
#if defined(_WIN32)
        CRITICAL_SECTION _cs;
    public:
        void lock() {EnterCriticalSection(&_cs); }
        void unlock() { LeaveCriticalSection(&_cs); }
#elif defined(__USE_XOPEN2K)
        pthread_spinlock_t _lock;
        void _lk();
    public:
        void unlock() { pthread_spin_unlock(&_lock); }
        void lock() {
            if ( MONGO_likely( pthread_spin_trylock( &_lock ) == 0 ) )
                return;
            _lk(); 
        }
#elif defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
        volatile bool _locked;
    public:
        void unlock() {__sync_lock_release(&_locked); }
        void lock();
#else
        // default to a mutex if not implemented
        SimpleMutex _mutex;
    public:
        void unlock() { _mutex.unlock(); }
        void lock() { _mutex.lock(); }
#endif
    };
    
    class scoped_spinlock : boost::noncopyable {
    public:
        scoped_spinlock( SpinLock& l ) : _l(l) {
            _l.lock();
        }
        ~scoped_spinlock() {
            _l.unlock();}
    private:
        SpinLock& _l;
    };

}  // namespace mongo
