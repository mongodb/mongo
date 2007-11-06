#ifndef BOOST_SCOPED_ARRAY_HPP_INCLUDED
#define BOOST_SCOPED_ARRAY_HPP_INCLUDED

//  (C) Copyright Greg Colvin and Beman Dawes 1998, 1999.
//  Copyright (c) 2001, 2002 Peter Dimov
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
//  http://www.boost.org/libs/smart_ptr/scoped_array.htm
//

#include <boost/assert.hpp>
#include <boost/checked_delete.hpp>
#include <boost/config.hpp>   // in case ptrdiff_t not in std

#include <boost/detail/workaround.hpp>

#include <cstddef>            // for std::ptrdiff_t

namespace boost
{

// Debug hooks

#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)

void sp_array_constructor_hook(void * p);
void sp_array_destructor_hook(void * p);

#endif

//  scoped_array extends scoped_ptr to arrays. Deletion of the array pointed to
//  is guaranteed, either on destruction of the scoped_array or via an explicit
//  reset(). Use shared_array or std::vector if your needs are more complex.

template<class T> class scoped_array // noncopyable
{
private:

    T * ptr;

    scoped_array(scoped_array const &);
    scoped_array & operator=(scoped_array const &);

    typedef scoped_array<T> this_type;

public:

    typedef T element_type;

    explicit scoped_array(T * p = 0) : ptr(p) // never throws
    {
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        boost::sp_array_constructor_hook(ptr);
#endif
    }

    ~scoped_array() // never throws
    {
#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
        boost::sp_array_destructor_hook(ptr);
#endif
        boost::checked_array_delete(ptr);
    }

    void reset(T * p = 0) // never throws
    {
        BOOST_ASSERT(p == 0 || p != ptr); // catch self-reset errors
        this_type(p).swap(*this);
    }

    T & operator[](std::ptrdiff_t i) const // never throws
    {
        BOOST_ASSERT(ptr != 0);
        BOOST_ASSERT(i >= 0);
        return ptr[i];
    }

    T * get() const // never throws
    {
        return ptr;
    }

    // implicit conversion to "bool"

#if defined(__SUNPRO_CC) && BOOST_WORKAROUND(__SUNPRO_CC, <= 0x530)

    operator bool () const
    {
        return ptr != 0;
    }

#elif defined(__MWERKS__) && BOOST_WORKAROUND(__MWERKS__, BOOST_TESTED_AT(0x3003))
    typedef T * (this_type::*unspecified_bool_type)() const;
    
    operator unspecified_bool_type() const // never throws
    {
        return ptr == 0? 0: &this_type::get;
    }

#else 

    typedef T * this_type::*unspecified_bool_type;

    operator unspecified_bool_type() const // never throws
    {
        return ptr == 0? 0: &this_type::ptr;
    }

#endif

    bool operator! () const // never throws
    {
        return ptr == 0;
    }

    void swap(scoped_array & b) // never throws
    {
        T * tmp = b.ptr;
        b.ptr = ptr;
        ptr = tmp;
    }

};

template<class T> inline void swap(scoped_array<T> & a, scoped_array<T> & b) // never throws
{
    a.swap(b);
}

} // namespace boost

#endif  // #ifndef BOOST_SCOPED_ARRAY_HPP_INCLUDED
