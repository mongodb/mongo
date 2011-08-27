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

namespace mongo { 

#if defined(_WIN32) || defined(__GNUC__)
        
    template< class T >
    struct TSP {
        thread_specific_ptr<T> tsp;
    public:
        T* get() const;
        void reset(T* v);
    };

# if defined(_WIN32)
#  define DECL_TSP(T,p) __declspec( thread ) T* _ ## p; \
    TSP<T> p; \
    template<> T* TSP<T>::get() const \
    { return _ ## p; } \
    void TSP<T>::reset(T* v) { \
        tsp.reset(v); \
        _ ## p = v; \
    } 
# else
#  define DECL_TSP(T,p) __thread T* _ ## p; \
    TSP<T> p; \
    template<> T* TSP<T>::get() const \
    { return _ ## p; } \
    void TSP<T>::reset(T* v) { \
        tsp.reset(v); \
        _ ## p = v; \
    } 
# endif

#else

    template< class T >
    struct TSP {
        thread_specific_ptr<T> tsp;
    public:
        T* get() const { return tsp.get(); }
        void reset(T* v) { tsp.reset(v); }
    };

# define DECL_TSP(T,p) \
    TSP<T> p; 

#endif

}
