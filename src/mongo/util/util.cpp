// @file util.cpp

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

#include "pch.h"
#include "goodies.h"
#include "mongo/util/startup_test.h"
#include "file_allocator.h"
#include "optime.h"
#include "time_support.h"
#include "mongoutils/str.h"
#include "timer.h"

namespace mongo {

    string hexdump(const char *data, unsigned len) {
        verify( len < 1000000 );
        const unsigned char *p = (const unsigned char *) data;
        stringstream ss;
        for( unsigned i = 0; i < 4 && i < len; i++ ) {
            ss << std::hex << setw(2) << setfill('0');
            unsigned n = p[i];
            ss << n;
            ss << ' ';
        }
        string s = ss.str();
        return s;
    }

    boost::thread_specific_ptr<string> _threadName;

    long long _setThreadName( const char * name ) {
        if ( ! name ) name = "NONE";

        static long long N = 0;

        if ( strcmp( name , "conn" ) == 0 ) {
            string* x = _threadName.get();
            if ( x && mongoutils::str::startsWith( *x , "conn" ) ) {
#if defined(_WIN32)
                long long n = _atoi64( x->c_str() + 4 );
#else
                long long n = atoll( x->c_str() + 4 );
#endif
                if ( n > 0 )
                    return n;
                warning() << "unexpected thread name [" << *x << "] parsed to " << n << endl;
            }
            long long n = ++N;
            stringstream ss;
            ss << name << n;
            _threadName.reset( new string( ss.str() ) );
            return n;
        }

        _threadName.reset( new string(name) );
        return 0;
    }

#if defined(_WIN32)
#define MS_VC_EXCEPTION 0x406D1388
#pragma pack(push,8)
    typedef struct tagTHREADNAME_INFO {
        DWORD dwType; // Must be 0x1000.
        LPCSTR szName; // Pointer to name (in user addr space).
        DWORD dwThreadID; // Thread ID (-1=caller thread).
        DWORD dwFlags; // Reserved for future use, must be zero.
    } THREADNAME_INFO;
#pragma pack(pop)

    void setWinThreadName(const char *name) {
        /* is the sleep here necessary???
           Sleep(10);
           */
        THREADNAME_INFO info;
        info.dwType = 0x1000;
        info.szName = name;
        info.dwThreadID = -1;
        info.dwFlags = 0;
        __try {
            RaiseException( MS_VC_EXCEPTION, 0, sizeof(info)/sizeof(ULONG_PTR), (ULONG_PTR*)&info );
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    long long setThreadName(const char *name) {
        long long n = _setThreadName( name );
#if !defined(_DEBUG)
        // naming might be expensive so don't do "conn*" over and over
        if( string("conn") == name )
            return n;
#endif
        setWinThreadName(name);
        return n;
    }

#else

    long long setThreadName(const char * name ) {
        return _setThreadName( name );
    }

#endif

    string getThreadName() {
        string * s = _threadName.get();
        if ( s )
            return *s;
        return "";
    }

    bool isPrime(int n) {
        int z = 2;
        while ( 1 ) {
            if ( z*z > n )
                break;
            if ( n % z == 0 )
                return false;
            z++;
        }
        return true;
    }

    int nextPrime(int n) {
        n |= 1; // 2 goes to 3...don't care...
        while ( !isPrime(n) )
            n += 2;
        return n;
    }

    struct UtilTest : public StartupTest {
        void run() {
            verify( isPrime(3) );
            verify( isPrime(2) );
            verify( isPrime(13) );
            verify( isPrime(17) );
            verify( !isPrime(9) );
            verify( !isPrime(6) );
            verify( nextPrime(4) == 5 );
            verify( nextPrime(8) == 11 );

            verify( endsWith("abcde", "de") );
            verify( !endsWith("abcde", "dasdfasdfashkfde") );
        }
    } utilTest;

    OpTime OpTime::last(0, 0);

    /* this is a good place to set a breakpoint when debugging, as lots of warning things
       (assert, wassert) call it.
    */
    void sayDbContext(const char *errmsg) {
        if ( errmsg ) {
            problem() << errmsg << endl;
        }
        printStackTrace();
    }

    ostream& operator<<( ostream &s, const ThreadSafeString &o ) {
        s << o.toString();
        return s;
    }

    bool StaticObserver::_destroyingStatics = false;

} // namespace mongo
