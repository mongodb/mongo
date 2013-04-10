// @file rwlockimpl.cpp

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
*/

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <boost/version.hpp>
#include <boost/noncopyable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/scoped_ptr.hpp>

using boost::scoped_ptr;

#include "simplerwlock.h"
#include "rwlockimpl.h"
#include "../log.h"

using namespace std;

namespace mongo {


    SimpleRWLock::SimpleRWLock(const StringData& p) :
        name(p.toString())
    { }    

#if defined(_WIN32)

    string rwlockImpl = "?";

    class Fallback : public RWTry_Interface {
        RWLockBase_Boost _m;
    public:
        void lock() { _m.lock(); }
        void unlock() { _m.unlock(); }
        void lock_shared() { _m.lock_shared(); }
        void unlock_shared() { _m.unlock_shared(); }
        bool lock_shared_try( int millis ) { return _m.lock_shared_try(millis); }
        bool lock_try( int millis = 0 ) { return _m.lock_try(millis); }
    };

    class SRWImpl : public RWTry_Interface { 
        typedef BOOLEAN (WINAPI *bp)(PSRWLOCK);
        typedef VOID    (WINAPI *vp)(PSRWLOCK);
        static vp initializeSRWLock;
        static vp acquireSRWLockShared;
        static vp releaseSRWLockShared;
        static vp acquireSRWLockExclusive;
        static vp releaseSRWLockExclusive;
        static bp tryAcquireSRWLockExclusive;
        static bp tryAcquireSRWLockShared;
        static void init();
        static bool inited;
        SRWLOCK _lock;
    public:
        static bool fallback();
    public:
        SRWImpl()            { initializeSRWLock(&_lock); }
        void lock()          { acquireSRWLockExclusive(&_lock); }
        void unlock()        { releaseSRWLockExclusive(&_lock); } 
        void lock_shared()   { acquireSRWLockShared(&_lock); }
        void unlock_shared() { releaseSRWLockShared(&_lock); }
        bool lock_shared_try( int millis );
        bool lock_try( int millis = 0 );
    };

    SRWImpl::vp SRWImpl::initializeSRWLock;
    SRWImpl::vp SRWImpl::acquireSRWLockShared;
    SRWImpl::vp SRWImpl::releaseSRWLockShared;
    SRWImpl::vp SRWImpl::acquireSRWLockExclusive;
    SRWImpl::vp SRWImpl::releaseSRWLockExclusive;
    SRWImpl::bp SRWImpl::tryAcquireSRWLockExclusive;
    SRWImpl::bp SRWImpl::tryAcquireSRWLockShared;
    bool SRWImpl::inited = false;

    bool SRWImpl::lock_shared_try( int millis ) {
        if( TryAcquireSRWLockShared(&_lock) )
            return true;
        if( millis == 0 )
            return false;
        unsigned long long end = curTimeMicros64() + millis*1000;
        while( 1 ) {
            Sleep(1);
            if( tryAcquireSRWLockShared(&_lock) )
                return true;
            if( curTimeMicros64() >= end )
                break;
        }
        return false;
    }
    bool SRWImpl::lock_try( int millis ) {
        if( TryAcquireSRWLockExclusive(&_lock) ) // quick check to optimistically avoid calling curTimeMicros64
            return true;
        if( millis == 0 )
            return false;
        unsigned long long end = curTimeMicros64() + millis*1000;
        do {
            Sleep(1);
            if( tryAcquireSRWLockExclusive(&_lock) )
                return true;
        } while( curTimeMicros64() < end );
        return false;
    }

    bool SRWImpl::fallback() { 
        if( !inited )
            init();
        return tryAcquireSRWLockExclusive == 0;
#endif
#if defined(NTDDI_VERSION) && defined(NTDDI_WIN7) && (NTDDI_VERSION >= NTDDI_WIN7)
    SimpleRWLock::SimpleRWLock(const StringData& p) : name(p.toString()) {
        InitializeSRWLock(&_lock);
    }

    RWTry_Interface* getBestRWLock() { 
        if( SRWImpl::fallback() )
            return new Fallback();
        return new SRWImpl();
    }

    NOINLINE_DECL void SRWImpl::init() {
        HMODULE module = LoadLibrary(TEXT("kernel32.dll"));
        tryAcquireSRWLockExclusive = reinterpret_cast<bp>(GetProcAddress(module, "TryAcquireSRWLockExclusive"));
        if( tryAcquireSRWLockExclusive ) {
            tryAcquireSRWLockShared = reinterpret_cast<bp>(GetProcAddress(module, "TryAcquireSRWLockShared"));
            initializeSRWLock       = reinterpret_cast<vp>(GetProcAddress(module, "InitializeSRWLock"));
            acquireSRWLockShared    = reinterpret_cast<vp>(GetProcAddress(module, "AcquireSRWLockShared"));
            releaseSRWLockShared    = reinterpret_cast<vp>(GetProcAddress(module, "ReleaseSRWLockShared"));
            acquireSRWLockExclusive = reinterpret_cast<vp>(GetProcAddress(module, "AcquireSRWLockExclusive"));
            releaseSRWLockExclusive = reinterpret_cast<vp>(GetProcAddress(module, "ReleaseSRWLockExclusive"));
            rwlockImpl = "SRW";
        }
        else { 
            rwlockImpl = "fallback";
        }
        dlog(2) << "rwlockImpl: " << rwlockImpl << endl;
        inited = true;
    }

    RWLockBase::RWLockBase() : i( getBestRWLock() ) { }
    RWLockBase::~RWLockBase() {}

#if defined(_DEBUG)
    // the code below in _DEBUG build will check that we don't try to recursively lock, 
    // which is not supported by this class.  also checks that you don't unlock without 
    // having locked
    void SimpleRWLock::lock() {
        unsigned me = GetCurrentThreadId();
        int& state = s.getRef();
        dassert( state == 0 );
        state--;
        m.lock();
        tid = me; // this is for use in the debugger to see who does have the lock
    }
    void SimpleRWLock::unlock() { 
        int& state = s.getRef();
        dassert( state == -1 );
        state++;
        tid = 0xffffffff;
        m.unlock();
    }
    void SimpleRWLock::lock_shared() { 
        int& state = s.getRef();
        dassert( state == 0 );
        state++;
        m.lock_shared();
        shares++;
    }
    void SimpleRWLock::unlock_shared() { 
        int& state = s.getRef();
        dassert( state == 1 );
        state--;
        shares--;
        m.unlock_shared();
    }
#endif
#endif

}


