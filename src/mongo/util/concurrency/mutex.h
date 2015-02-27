// @file mutex.h

/*    Copyright 2009 10gen Inc.
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

#include <boost/noncopyable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/xtime.hpp>

#include "mongo/bson/inline_decls.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/heapcheck.h"
#include "mongo/util/concurrency/threadlocal.h"
#include "mongo/util/time_support.h"

// Macro to get line as a std::string constant
#define MONGO_STRINGIFY(X) #X
// Double-expansion trick to get preproc to actually substitute __LINE__
#define _MONGO_LINE_STRING(LINE) MONGO_STRINGIFY( LINE )
#define MONGO_LINE_STRING _MONGO_LINE_STRING( __LINE__ )

// Mutex names should be as <file>::<line> string
#define MONGO_FILE_LINE __FILE__ "::" MONGO_LINE_STRING

namespace mongo {

    inline boost::xtime incxtimemillis( long long s ) {
        boost::xtime xt;
        boost::xtime_get(&xt, MONGO_BOOST_TIME_UTC);
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

    using mutex = boost::mutex;

    /** The concept with SimpleMutex is that it is a basic lock/unlock with no 
          special functionality (such as try and try timeout).  Thus it can be 
          implemented using OS-specific facilities in all environments (if desired).
        On Windows, the implementation below is faster than boost mutex.
    */
#if defined(_WIN32)
    class SimpleMutex : boost::noncopyable {
    public:
        SimpleMutex( StringData ) { InitializeCriticalSection( &_cs ); }
        void dassertLocked() const { }
        void lock() { EnterCriticalSection( &_cs ); }
        void unlock() { LeaveCriticalSection( &_cs ); }
        class scoped_lock {
            SimpleMutex& _m;
        public:
            scoped_lock( SimpleMutex &m ) : _m(m) { _m.lock(); }
            ~scoped_lock() { _m.unlock(); }
            const SimpleMutex& m() const { return _m; }
        };

    private:
        CRITICAL_SECTION _cs;
    };
#else
    class SimpleMutex : boost::noncopyable {
    public:
        void dassertLocked() const { }
        SimpleMutex(StringData name) { verify( pthread_mutex_init(&_lock,0) == 0 ); }
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
        RecursiveMutex(StringData name) : m(name) { }
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
