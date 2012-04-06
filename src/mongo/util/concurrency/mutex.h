// @file mutex.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#ifdef _WIN32
# include <concrt.h>
#endif

#include <boost/noncopyable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/xtime.hpp>

#include "mongo/util/assert_util.h"
#include "mongo/util/heapcheck.h"
#include "mongo/util/concurrency/threadlocal.h"

#if defined(_DEBUG)
#include "mongo/util/concurrency/mutexdebugger.h"
#endif

namespace mongo {

    inline boost::xtime incxtimemillis( long long s ) {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        xt.sec += (int)( s / 1000 );
        xt.nsec += (int)(( s % 1000 ) * 1000000);
        if ( xt.nsec >= 1000000000 ) {
            xt.nsec -= 1000000000;
            xt.sec++;
        }
        return xt;
    }

    // If you create a local static instance of this class, that instance will be destroyed
    // before all global static objects are destroyed, so _destroyingStatics will be set
    // to true before the global static variables are destroyed.
    class StaticObserver : boost::noncopyable {
    public:
        static bool _destroyingStatics;
        ~StaticObserver() { _destroyingStatics = true; }
    };

    /** On pthread systems, it is an error to destroy a mutex while held (boost mutex 
     *    may use pthread).  Static global mutexes may be held upon shutdown in our 
     *    implementation, and this way we avoid destroying them.
     *  NOT recursive.
     */
    class mutex : boost::noncopyable {
    public:
        const char * const _name;
        mutex(const char *name) : _name(name)
        {
            _m = new boost::timed_mutex();
            IGNORE_OBJECT( _m  );   // Turn-off heap checking on _m
        }
        ~mutex() {
            if( !StaticObserver::_destroyingStatics ) {
                UNIGNORE_OBJECT( _m );
                delete _m;
            }
        }

        class try_lock : boost::noncopyable {
        public:
            try_lock( mongo::mutex &m , int millis = 0 )
                : _l( m.boost() , incxtimemillis( millis ) ) ,
                  ok( _l.owns_lock() )
            { }
        private:
            boost::timed_mutex::scoped_timed_lock _l;
        public:
            const bool ok;
        };

        class scoped_lock : boost::noncopyable {
        public:
#if defined(_DEBUG)
            struct PostStaticCheck {
                PostStaticCheck();
            } _check;
            mongo::mutex * const _mut;
#endif
            scoped_lock( mongo::mutex &m ) : 
#if defined(_DEBUG)
            _mut(&m),
#endif
            _l( m.boost() ) {
#if defined(_DEBUG)
                mutexDebugger.entering(_mut->_name);
#endif
            }
            ~scoped_lock() {
#if defined(_DEBUG)
                mutexDebugger.leaving(_mut->_name);
#endif
            }
            boost::timed_mutex::scoped_lock &boost() { return _l; }
        private:
            boost::timed_mutex::scoped_lock _l;
        };
    private:
        boost::timed_mutex &boost() { return *_m; }
        boost::timed_mutex *_m;
    };

    typedef mongo::mutex::scoped_lock scoped_lock;

    /** The concept with SimpleMutex is that it is a basic lock/unlock with no 
          special functionality (such as try and try timeout).  Thus it can be 
          implemented using OS-specific facilities in all environments (if desired).
        On Windows, the implementation below is faster than boost mutex.
    */
#if defined(_WIN32)
    class SimpleMutex  {
    public:
        SimpleMutex( const char * ) {}
        void dassertLocked() const { }
        void lock() { _cs.lock(); }
        void unlock() { _cs.unlock(); }
        class scoped_lock {
            SimpleMutex& _m;
        public:
            scoped_lock( SimpleMutex &m ) : _m(m) { _m.lock(); }
            ~scoped_lock() { _m.unlock(); }
            const SimpleMutex& m() const { return _m; }
        };

    private:
        Concurrency::critical_section _cs;
    };
#else
    class SimpleMutex : boost::noncopyable {
    public:
        void dassertLocked() const { }
        SimpleMutex(const char* name) { verify( pthread_mutex_init(&_lock,0) == 0 ); }
        ~SimpleMutex(){ 
            if ( ! StaticObserver::_destroyingStatics ) { 
                verify( pthread_mutex_destroy(&_lock) == 0 ); 
            }
        }

        void lock() { verify( pthread_mutex_lock(&_lock) == 0 ); }
        void unlock() { verify( pthread_mutex_unlock(&_lock) == 0 ); }
    public:
        class scoped_lock : boost::noncopyable {
            SimpleMutex& _m;
        public:
            scoped_lock( SimpleMutex &m ) : _m(m) { _m.lock(); }
            ~scoped_lock() { _m.unlock(); }
            const SimpleMutex& m() const { return _m; }
        };

    private:
        pthread_mutex_t _lock;
    };
    
#endif

    /** This can be used instead of boost recursive mutex. The advantage is the _DEBUG checks
     *  and ability to assertLocked(). This has not yet been tested for speed vs. the boost one.
     */
    class RecursiveMutex : boost::noncopyable {
    public:
        RecursiveMutex(const char* name) : m(name) { }
        bool isLocked() const { return n.get() > 0; }
        class scoped_lock : boost::noncopyable {
            RecursiveMutex& rm;
            int& nLocksByMe;
        public:
            scoped_lock( RecursiveMutex &m ) : rm(m), nLocksByMe(rm.n.getRef()) { 
                if( nLocksByMe++ == 0 ) 
                    rm.m.lock(); 
            }
            ~scoped_lock() { 
                verify( nLocksByMe > 0 );
                if( --nLocksByMe == 0 ) {
                    rm.m.unlock(); 
                }
            }
        };
    private:
        SimpleMutex m;
        ThreadLocalValue<int> n;
    };

}
