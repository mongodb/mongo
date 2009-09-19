// goodies.h
// miscellaneous junk

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
*/

#pragma once

namespace mongo {

#if !defined(_WIN32) && !defined(NOEXECINFO)

} // namespace mongo

#include <pthread.h>

namespace mongo {

    inline pthread_t GetCurrentThreadId() {
        return pthread_self();
    }

} // namespace mongo

#include <execinfo.h>

namespace mongo {

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
        unsigned x;
        operator unsigned() const {
            return x;
        }
        WrappingInt& operator++() {
            x++;
            return *this;
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
    
    class ThreadLocalInt {
    public:
        ThreadLocalInt( int def = 0 ) : _def( def ){}

        int get(){
            int * val = _val.get();
            if ( val )
                return *val;
            return _def;
        }
        void reset( int i ){
            int * val = new int[1];
            *val = i;
            _val.reset( val );
        }
    private:
        int _def;
        boost::thread_specific_ptr<int> _val;
    };

} // namespace mongo
