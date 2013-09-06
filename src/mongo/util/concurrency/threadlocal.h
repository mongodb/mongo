#pragma once

/**
*    Copyright (C) 2011 10gen Inc.
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

#include "mongo/client/undef_macros.h"
#include <boost/thread/tss.hpp>
#include <boost/bind.hpp>
#include "mongo/client/redef_macros.h"


namespace mongo { 

    using boost::thread_specific_ptr;

    /* thread local "value" rather than a pointer
       good for things which have copy constructors (and the copy constructor is fast enough)
       e.g.
         ThreadLocalValue<int> myint;
    */
    template<class T>
    class ThreadLocalValue {
    public:
        ThreadLocalValue( T def = 0 ) : _default( def ) { }

        T get() const {
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

        T& getRef() {
            T *v = _val.get();
            if( v ) {
                return *v; 
            }
            v = new T(_default);
            _val.reset( v );
            return *v;
        }

    private:
        boost::thread_specific_ptr<T> _val;
        const T _default;
    };

    /* TSP
       These macros use intrinsics which are faster than boost::thread_specific_ptr. 
       However the intrinsics don't free up objects on thread closure. Thus we use 
       a combination here, with the assumption that reset's are infrequent, so that 
       get's are fast.
    */
#if defined(_WIN32) || (defined(__GNUC__) && defined(__linux__))
        
    template< class T >
    struct TSP {
        boost::thread_specific_ptr<T> tsp;
    public:
        T* get() const;
        void reset(T* v);
        T* getMake() { 
            T *t = get();
            if( t == 0 )
                reset( t = new T() );
            return t;
        }
    };

# if defined(_WIN32)

#  define TSP_DECLARE(T,p) extern TSP<T> p;

#  define TSP_DEFINE(T,p) __declspec( thread ) T* _ ## p; \
    TSP<T> p; \
    template<> T* TSP<T>::get() const { return _ ## p; } \
    void TSP<T>::reset(T* v) { \
        tsp.reset(v); \
        _ ## p = v; \
    } 
# else

#  define TSP_DECLARE(T,p) \
    extern __thread T* _ ## p; \
    template<> inline T* TSP<T>::get() const { return _ ## p; }	\
    extern TSP<T> p;

#  define TSP_DEFINE(T,p) \
    __thread T* _ ## p; \
    template<> void TSP<T>::reset(T* v) { \
        tsp.reset(v); \
        _ ## p = v; \
    } \
    TSP<T> p;
# endif

#elif defined(__APPLE__)
    template< class T>
    struct TSP {
        pthread_key_t _key;
    public:
        TSP() {
            verify( pthread_key_create( &_key, TSP::dodelete ) == 0 );
        }

        ~TSP() {
            pthread_key_delete( _key );
        }

        static void dodelete( void* x ) {
            T* t = reinterpret_cast<T*>(x);
            delete t;
        }
        
        T* get() const { 
            return reinterpret_cast<T*>( pthread_getspecific( _key ) ); 
        }
        
        void reset(T* v) { 
            T* old = get();
            delete old;
            verify( pthread_setspecific( _key, v ) == 0 ); 
        }

        T* getMake() { 
            T *t = get();
            if( t == 0 ) {
                t = new T();
                reset( t );
            }
            return t;
        }
    };

#  define TSP_DECLARE(T,p) extern TSP<T> p;

#  define TSP_DEFINE(T,p) TSP<T> p; 

#else

    template< class T >
    struct TSP {
        thread_specific_ptr<T> tsp;
    public:
        T* get() const { return tsp.get(); }
        void reset(T* v) { tsp.reset(v); }
        T* getMake() { 
            T *t = get();
            if( t == 0 )
                reset( t = new T() );
            return t;
        }
    };

#  define TSP_DECLARE(T,p) extern TSP<T> p;

# define TSP_DEFINE(T,p) TSP<T> p; 

#endif

}
