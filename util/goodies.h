// goodies.h
// miscellaneous junk

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

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace mongo {

#if !defined(_WIN32) && !defined(NOEXECINFO) && !defined(__freebsd__)

} // namespace mongo

#include <pthread.h>
#include <execinfo.h>

namespace mongo {

    inline pthread_t GetCurrentThreadId() {
        return pthread_self();
    }

    /* use "addr2line -CFe <exe>" to parse. */
    inline void printStackTrace( ostream &o = cout ) {
        void *b[20];
        size_t size;
        char **strings;
        size_t i;

        size = backtrace(b, 20);
        strings = backtrace_symbols(b, size);

        for (i = 0; i < size; i++)
            o << hex << b[i] << dec << ' ';
        o << '\n';
        for (i = 0; i < size; i++)
            o << ' ' << strings[i] << '\n';

        free (strings);
    }
#else
    inline void printStackTrace( ostream &o = cout ) { }
#endif

    /* set to TRUE if we are exiting */
    extern bool goingAway;

    /* find the multimap member which matches a particular key and value.

       note this can be slow if there are a lot with the same key.
    */
    template<class C,class K,class V> inline typename C::iterator kv_find(C& c, const K& k,const V& v) {
        pair<typename C::iterator,typename C::iterator> p = c.equal_range(k);

        for ( typename C::iterator it=p.first; it!=p.second; ++it)
            if ( it->second == v )
                return it;

        return c.end();
    }

    bool isPrime(int n);
    int nextPrime(int n);

    inline void dumpmemory(const char *data, int len) {
        if ( len > 1024 )
            len = 1024;
        try {
            const char *q = data;
            const char *p = q;
            while ( len > 0 ) {
                for ( int i = 0; i < 16; i++ ) {
                    if ( *p >= 32 && *p <= 126 )
                        cout << *p;
                    else
                        cout << '.';
                    p++;
                }
                cout << "  ";
                p -= 16;
                for ( int i = 0; i < 16; i++ )
                    cout << (unsigned) ((unsigned char)*p++) << ' ';
                cout << endl;
                len -= 16;
            }
        } catch (...) {
        }
    }

#undef yassert

#undef assert
#define assert xassert
#define yassert 1

    struct WrappingInt {
        WrappingInt() {
            x = 0;
        }
        WrappingInt(unsigned z) : x(z) { }
        volatile unsigned x;
        operator unsigned() const {
            return x;
        }

        // returns original value (like x++)
        WrappingInt atomicIncrement(){
#if defined(_WIN32)
            // InterlockedIncrement returns the new value
            return InterlockedIncrement((volatile long*)&x)-1; //long is 32bits in Win64
#elif defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
            // this is in GCC >= 4.1
            return __sync_fetch_and_add(&x, 1);
#elif defined(__GNUC__)  && (defined(__i386__) || defined(__x86_64__))
            // from boost 1.39 interprocess/detail/atomic.hpp
            int r;
            int val = 1;
            asm volatile
            (
               "lock\n\t"
               "xadd %1, %0":
               "+m"( x ), "=r"( r ): // outputs (%0, %1)
               "1"( val ): // inputs (%2 == %1)
               "memory", "cc" // clobbers
            );
            return r;
#else
#  error "unsupported compiler or platform"
#endif
        }

        static int diff(unsigned a, unsigned b) {
            return a-b;
        }
        bool operator<=(WrappingInt r) {
            // platform dependent
            int df = (r.x - x);
            return df >= 0;
        }
        bool operator>(WrappingInt r) {
            return !(r<=*this);
        }
    };

} // namespace mongo

#include <ctime>

namespace mongo {

    inline void time_t_to_String(time_t t, char *buf) {
#if defined(_WIN32)
        ctime_s(buf, 64, &t);
#else
        ctime_r(&t, buf);
#endif
        buf[24] = 0; // don't want the \n
    }

#define asctime _asctime_not_threadsafe_
#define gmtime _gmtime_not_threadsafe_
#define localtime _localtime_not_threadsafe_
#define ctime _ctime_is_not_threadsafe_

#if defined(_WIN32) || defined(__sunos__)
    inline void sleepsecs(int s) {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        xt.sec += s;
        boost::thread::sleep(xt);
    }
    inline void sleepmillis(int s) {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        xt.sec += ( s / 1000 );
        xt.nsec += ( s % 1000 ) * 1000000;
        if ( xt.nsec >= 1000000000 ) {
            xt.nsec -= 1000000000;
            xt.sec++;
        }        
        boost::thread::sleep(xt);
    }
    inline void sleepmicros(int s) {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        xt.sec += ( s / 1000000 );
        xt.nsec += ( s % 1000000 ) * 1000;
        if ( xt.nsec >= 1000000000 ) {
            xt.nsec -= 1000000000;
            xt.sec++;
        }        
        boost::thread::sleep(xt);
    }
#else
    inline void sleepsecs(int s) {
        struct timespec t;
        t.tv_sec = s;
        t.tv_nsec = 0;
        if ( nanosleep( &t , 0 ) ){
            cout << "nanosleep failed" << endl;
        }
    }
    inline void sleepmicros(int s) {
        struct timespec t;
        t.tv_sec = (int)(s / 1000000);
        t.tv_nsec = s % 1000000;
        if ( nanosleep( &t , 0 ) ){
            cout << "nanosleep failed" << endl;
        }
    }
    inline void sleepmillis(int s) {
        sleepmicros( s * 1000 );
    }
#endif

// note this wraps
    inline int tdiff(unsigned told, unsigned tnew) {
        return WrappingInt::diff(tnew, told);
    }
    inline unsigned curTimeMillis() {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        unsigned t = xt.nsec / 1000000;
        return (xt.sec & 0xfffff) * 1000 + t;
    }

    inline unsigned long long jsTime() {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        unsigned long long t = xt.nsec / 1000000;
        return ((unsigned long long) xt.sec * 1000) + t;
    }

    inline unsigned long long curTimeMicros64() {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        unsigned long long t = xt.nsec / 1000;
        return (((unsigned long long) xt.sec) * 1000000) + t;
    }

// measures up to 1024 seconds.  or, 512 seconds with tdiff that is...
    inline unsigned curTimeMicros() {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        unsigned t = xt.nsec / 1000;
        unsigned secs = xt.sec % 1024;
        return secs*1000000 + t;
    }
    using namespace boost;
    typedef boost::mutex::scoped_lock boostlock;
    typedef boost::recursive_mutex::scoped_lock recursive_boostlock;

// simple scoped timer
    class Timer {
    public:
        Timer() {
            reset();
        }
        int seconds(){
            return (int)(micros() / 1000000);
        }
        int millis() {
            return (long)(micros() / 1000);
        }
        unsigned long long micros() {
            unsigned long long n = curTimeMicros64();
            return n - old;
        }
        unsigned long long micros(unsigned long long & n) { // returns cur time in addition to timer result
            n = curTimeMicros64();
            return n - old;
        }
        void reset() {
            old = curTimeMicros64();
        }
    private:
        unsigned long long old;
    };

    /*

    class DebugMutex : boost::noncopyable {
    	friend class lock;
    	boost::mutex m;
    	int locked;
    public:
    	DebugMutex() : locked(0); { }
    	bool isLocked() { return locked; }
    };

    */

//typedef boostlock lock;

    inline bool startsWith(const char *str, const char *prefix) {
        unsigned l = strlen(prefix);
        if ( strlen(str) < l ) return false;
        return strncmp(str, prefix, l) == 0;
    }

    inline bool endsWith(const char *p, const char *suffix) {
        int a = strlen(p);
        int b = strlen(suffix);
        if ( b > a ) return false;
        return strcmp(p + a - b, suffix) == 0;
    }

} // namespace mongo

#include "boost/detail/endian.hpp"

namespace mongo {

    inline unsigned long swapEndian(unsigned long x) {
        return
            ((x & 0xff) << 24) |
            ((x & 0xff00) << 8) |
            ((x & 0xff0000) >> 8) |
            ((x & 0xff000000) >> 24);
    }

#if defined(BOOST_LITTLE_ENDIAN)
    inline unsigned long fixEndian(unsigned long x) {
        return x;
    }
#else
    inline unsigned long fixEndian(unsigned long x) {
        return swapEndian(x);
    }
#endif

    // Like strlen, but only scans up to n bytes.
    // Returns -1 if no '0' found.
    inline int strnlen( const char *s, int n ) {
        for( int i = 0; i < n; ++i )
            if ( !s[ i ] )
                return i;
        return -1;
    }
    
#if !defined(_WIN32)
    typedef int HANDLE;
    inline void strcpy_s(char *dst, unsigned len, const char *src) {
        strcpy(dst, src);
    }
#else
    typedef void *HANDLE;
#endif
    
    /* thread local "value" rather than a pointer
       good for things which have copy constructors (and the copy constructor is fast enough)
       e.g. 
         ThreadLocalValue<int> myint;
    */
    template<class T>
    class ThreadLocalValue {
    public:
        ThreadLocalValue( T def = 0 ) : _default( def ) { }

        int get() {
            T * val = _val.get();
            if ( val )
                return *val;
            return _default;
        }
        void set( const T& i ) {
            T *v = _val.get();
            if( v ) { 
                *v = i;
                return;
            }
            v = new T(i);
            _val.reset( v );
        }
    private:
        T _default;
        boost::thread_specific_ptr<T> _val;
    };

    class ProgressMeter {
    public:
        ProgressMeter( long long total , int secondsBetween = 3 , int checkInterval = 100 )
            : _total( total ) , _secondsBetween( secondsBetween ) , _checkInterval( checkInterval ) ,
              _done(0) , _hits(0) , _lastTime( (int) time(0) ){
        }
        
        bool hit( int n = 1 ){
            _done += n;
            _hits++;
            if ( _hits % _checkInterval )
                return false;
            
            int t = (int) time(0);
            if ( t - _lastTime < _secondsBetween )
                return false;
            
            if ( _total > 0 ){
                int per = (int)( ( (double)_done * 100.0 ) / (double)_total );
                cout << "\t\t" << _done << "/" << _total << "\t" << per << "%" << endl;
            }
            _lastTime = t;
            return true;
        }

        long long done(){
            return _done;
        }
        
        long long hits(){
            return _hits;
        }

    private:
        
        long long _total;
        int _secondsBetween;
        int _checkInterval;

        long long _done;
        long long _hits;
        int _lastTime;
    };


} // namespace mongo
