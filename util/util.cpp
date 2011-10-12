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
#include "unittest.h"
#include "file_allocator.h"
#include "optime.h"
#include "time_support.h"
#include "mongoutils/str.h"
#include "timer.h"

namespace mongo {

#if defined(_WIN32)
    unsigned long long Timer::countsPerSecond;
    struct AtStartup {
        AtStartup() {
            LARGE_INTEGER x;
            bool ok = QueryPerformanceFrequency(&x);
            assert(ok);
            Timer::countsPerSecond = x.QuadPart;
        }
    } atstartuputil;
#endif

    string hexdump(const char *data, unsigned len) {
        assert( len < 1000000 );
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

    unsigned _setThreadName( const char * name ) {
        if ( ! name ) name = "NONE";

        static unsigned N = 0;

        if ( strcmp( name , "conn" ) == 0 ) {
            string* x = _threadName.get();
            if ( x && mongoutils::str::startsWith( *x , "conn" ) ) {
                int n = atoi( x->c_str() + 4 );
                if ( n > 0 )
                    return n;
                warning() << "unexpected thread name [" << *x << "] parsed to " << n << endl;
            }
            unsigned n = ++N;
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

    unsigned setThreadName(const char *name) {
        unsigned n = _setThreadName( name );
#if !defined(_DEBUG)
        // naming might be expensive so don't do "conn*" over and over
        if( string("conn") == name )
            return n;
#endif
        setWinThreadName(name);
        return n;
    }

#else

    unsigned setThreadName(const char * name ) {
        return _setThreadName( name );
    }

#endif

    string getThreadName() {
        string * s = _threadName.get();
        if ( s )
            return *s;
        return "";
    }

    vector<UnitTest*> *UnitTest::tests = 0;
    bool UnitTest::running = false;

    const char *default_getcurns() { return ""; }
    const char * (*getcurns)() = default_getcurns;

    int logLevel = 0;
    int tlogLevel = 0;
    mongo::mutex Logstream::mutex("Logstream");
    int Logstream::doneSetup = Logstream::magicNumber();

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

    struct UtilTest : public UnitTest {
        void run() {
            assert( isPrime(3) );
            assert( isPrime(2) );
            assert( isPrime(13) );
            assert( isPrime(17) );
            assert( !isPrime(9) );
            assert( !isPrime(6) );
            assert( nextPrime(4) == 5 );
            assert( nextPrime(8) == 11 );

            assert( endsWith("abcde", "de") );
            assert( !endsWith("abcde", "dasdfasdfashkfde") );

            assert( swapEndian(0x01020304) == 0x04030201 );

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

    /* note: can't use malloc herein - may be in signal handler.
             logLockless() likely does not comply and should still be fixed todo
             likewise class string?
    */
    void rawOut( const string &s ) {
        if( s.empty() ) return;

        char buf[64];
        time_t_to_String( time(0) , buf );
        /* truncate / don't show the year: */
        buf[19] = ' ';
        buf[20] = 0;

        Logstream::logLockless(buf);
        Logstream::logLockless(s);
        Logstream::logLockless("\n");
    }

    ostream& operator<<( ostream &s, const ThreadSafeString &o ) {
        s << o.toString();
        return s;
    }

    bool StaticObserver::_destroyingStatics = false;

} // namespace mongo
