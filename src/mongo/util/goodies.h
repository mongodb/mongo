// @file goodies.h
// miscellaneous

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

#include <sstream>

#include <boost/detail/endian.hpp>

#include "mongo/bson/util/misc.h"

namespace mongo {

    /* @return a dump of the buffer as hex byte ascii output */
    string hexdump(const char *data, unsigned len);

    template<class T>
    inline string ToString(const T& t) {
        stringstream s;
        s << t;
        return s.str();
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
        }
        catch (...) {
        }
    }

// PRINT(2+2);  prints "2+2: 4"
#define MONGO_PRINT(x) cout << #x ": " << (x) << endl
#define PRINT MONGO_PRINT
// PRINTFL; prints file:line
#define MONGO_PRINTFL cout << __FILE__ ":" << __LINE__ << endl
#define PRINTFL MONGO_PRINTFL
#define MONGO_FLOG log() << __FILE__ ":" << __LINE__ << endl
#define FLOG MONGO_FLOG

    inline bool startsWith(const char *str, const char *prefix) {
        size_t l = strlen(prefix);
        if ( strlen(str) < l ) return false;
        return strncmp(str, prefix, l) == 0;
    }
    inline bool startsWith(const std::string& s, const std::string& p) {
        return startsWith(s.c_str(), p.c_str());
    }

    inline bool endsWith(const char *p, const char *suffix) {
        size_t a = strlen(p);
        size_t b = strlen(suffix);
        if ( b > a ) return false;
        return strcmp(p + a - b, suffix) == 0;
    }

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
#elif defined(BOOST_BIG_ENDIAN)
    inline unsigned long fixEndian(unsigned long x) {
        return swapEndian(x);
    }
#else
#error no boost endian header defined
#endif

#if !defined(_WIN32)
    typedef int HANDLE;
    inline void strcpy_s(char *dst, unsigned len, const char *src) {
        verify( strlen(src) < len );
        strcpy(dst, src);
    }
#else
    typedef void *HANDLE;
#endif


    /**
     * this is a thread safe string
     * you will never get a bad pointer, though data may be mungedd
     */
    class ThreadSafeString : boost::noncopyable {
    public:
        ThreadSafeString( size_t size=256 )
            : _size( size ) , _buf( new char[size] ) {
            memset( _buf , 0 , _size );
        }

        ThreadSafeString( const ThreadSafeString& other )
            : _size( other._size ) , _buf( new char[_size] ) {
            strncpy( _buf , other._buf , _size );
        }

        ~ThreadSafeString() {
            delete[] _buf;
            _buf = 0;
        }

        string toString() const {
            string s = _buf;
            return s;
        }

        ThreadSafeString& operator=( const char * str ) {
            size_t s = strlen(str);
            if ( s >= _size - 2 )
                s = _size - 2;
            strncpy( _buf , str , s );
            _buf[s] = 0;
            return *this;
        }

        bool operator==( const ThreadSafeString& other ) const {
            return strcmp( _buf , other._buf ) == 0;
        }

        bool operator==( const char * str ) const {
            return strcmp( _buf , str ) == 0;
        }

        bool operator!=( const char * str ) const {
            return strcmp( _buf , str ) != 0;
        }

        bool empty() const {
            return _buf == 0 || _buf[0] == 0;
        }

    private:
        size_t _size;
        char * _buf;
    };

    ostream& operator<<( ostream &s, const ThreadSafeString &o );

    /** A generic pointer type for function arguments.
     *  It will convert from any pointer type except auto_ptr.
     *  Semantics are the same as passing the pointer returned from get()
     *  const ptr<T>  =>  T * const
     *  ptr<const T>  =>  T const *  or  const T*
     */
    template <typename T>
    struct ptr {

        ptr() : _p(NULL) {}

        // convert to ptr<T>
        ptr(T* p) : _p(p) {} // needed for NULL
        template<typename U> ptr(U* p) : _p(p) {}
        template<typename U> ptr(const ptr<U>& p) : _p(p) {}
        template<typename U> ptr(const boost::shared_ptr<U>& p) : _p(p.get()) {}
        template<typename U> ptr(const boost::scoped_ptr<U>& p) : _p(p.get()) {}
        //template<typename U> ptr(const auto_ptr<U>& p) : _p(p.get()) {}

        // assign to ptr<T>
        ptr& operator= (T* p) { _p = p; return *this; } // needed for NULL
        template<typename U> ptr& operator= (U* p) { _p = p; return *this; }
        template<typename U> ptr& operator= (const ptr<U>& p) { _p = p; return *this; }
        template<typename U> ptr& operator= (const boost::shared_ptr<U>& p) { _p = p.get(); return *this; }
        template<typename U> ptr& operator= (const boost::scoped_ptr<U>& p) { _p = p.get(); return *this; }
        //template<typename U> ptr& operator= (const auto_ptr<U>& p) { _p = p.get(); return *this; }

        // use
        T* operator->() const { return _p; }
        T& operator*() const { return *_p; }

        // convert from ptr<T>
        operator T* () const { return _p; }

    private:
        T* _p;
    };



    using boost::shared_ptr;
    using boost::scoped_ptr;
    using boost::scoped_array;
    using boost::intrusive_ptr;
    using boost::dynamic_pointer_cast;
} // namespace mongo

