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

namespace mongo {

    boost::thread_specific_ptr<string> _threadName;
    
    void _setThreadName( const char * name ){
        static unsigned N = 0;
        if ( strcmp( name , "conn" ) == 0 ){
            stringstream ss;
            ss << name << ++N;
            _threadName.reset( new string( ss.str() ) );
        }
        else {
            _threadName.reset( new string(name) );        
        }
    }

#if defined(_WIN32)
#define MS_VC_EXCEPTION 0x406D1388
#pragma pack(push,8)
    typedef struct tagTHREADNAME_INFO
    {
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
        __try
        {
            RaiseException( MS_VC_EXCEPTION, 0, sizeof(info)/sizeof(ULONG_PTR), (ULONG_PTR*)&info );
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void setThreadName(const char *name)
    {
        _setThreadName( name );
#if !defined(_DEBUG)
        // naming might be expensive so don't do "conn*" over and over
        if( string("conn") == name )
            return;
#endif
        setWinThreadName(name);
    }

#else

    void setThreadName(const char * name ) { 
        _setThreadName( name );
    }

#endif

    string getThreadName(){
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
    
    bool goingAway = false;

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
            assert( WrappingInt(0) <= WrappingInt(0) );
            assert( WrappingInt(0) <= WrappingInt(1) );
            assert( !(WrappingInt(1) <= WrappingInt(0)) );
            assert( (WrappingInt(0xf0000000) <= WrappingInt(0)) );
            assert( (WrappingInt(0xf0000000) <= WrappingInt(9000)) );
            assert( !(WrappingInt(300) <= WrappingInt(0xe0000000)) );

            assert( tdiff(3, 4) == 1 );
            assert( tdiff(4, 3) == -1 );
            assert( tdiff(0xffffffff, 0) == 1 );

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
    
    // The mutex contained in this object may be held on shutdown.
    FileAllocator &theFileAllocator_ = *(new FileAllocator());
    FileAllocator &theFileAllocator() { return theFileAllocator_; }
    
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

    ostream& operator<<( ostream &s, const ThreadSafeString &o ){
        s << o.toString();
        return s;
    }

    bool __destroyingStatics = false;
    
} // namespace mongo
