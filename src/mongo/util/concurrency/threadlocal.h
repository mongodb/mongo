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
*/

#include "mongo/client/undef_macros.h"
#include <boost/thread/tss.hpp>
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
