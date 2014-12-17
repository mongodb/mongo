#ifndef BOOST_SMART_PTR_DETAIL_SHARED_ARRAY_NMT_HPP_INCLUDED
#define BOOST_SMART_PTR_DETAIL_SHARED_ARRAY_NMT_HPP_INCLUDED

//
//  detail/shared_array_nmt.hpp - shared_array.hpp without member templates
//
//  (C) Copyright Greg Colvin and Beman Dawes 1998, 1999.
//  Copyright (c) 2001, 2002 Peter Dimov
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
//  See http://www.boost.org/libs/smart_ptr/shared_array.htm for documentation.
//

#include <boost/assert.hpp>
#include <boost/checked_delete.hpp>
#include <boost/throw_exception.hpp>
#include <boost/smart_ptr/detail/atomic_count.hpp>

#include <cstddef>          // for std::ptrdiff_t
#include <algorithm>        // for std::swap
#include <functional>       // for std::less
#include <new>              // for std::bad_alloc

namespace boost
{

template<class T> class shared_array
{
private:

    typedef detail::atomic_count count_type;

public:

    typedef T element_type;
      
    explicit shared_array(T * p = 0): px(p)
    {
#ifndef BOOST_NO_EXCEPTIONS

        try  // prevent leak if new throws
        {
            pn = new count_type(1);
        }
        catch(...)
        {
            boost::checked_array_delete(p);
            throw;
        }

#else

        pn = new count_type(1);

        if(pn == 0)
        {
            boost::checked_array_delete(p);
            boost::throw_exception(std::bad_alloc());
        }

#endif
    }

    ~shared_array()
    {
        if(--*pn == 0)
        {
            boost::checked_array_delete(px);
            delete pn;
        }
    }

    shared_array(shared_array const & r) : px(r.px)  // never throws
    {
        pn = r.pn;
        ++*pn;
    }

    shared_array & operator=(shared_array const & r)
    {
        shared_array(r).swap(*this);
        return *this;
    }

    void reset(T * p = 0)
    {
        BOOST_ASSERT(p == 0 || p != px);
        shared_array(p).swap(*this);
    }

    T * get() const  // never throws
    {
        return px;
    }

    T & operator[](std::ptrdiff_t i) const  // never throws
    {
        BOOST_ASSERT(px != 0);
        BOOST_ASSERT(i >= 0);
        return px[i];
    }

    long use_count() const  // never throws
    {
        return *pn;
    }

    bool unique() const  // never throws
    {
        return *pn == 1;
    }

    void swap(shared_array<T> & other)  // never throws
    {
        std::swap(px, other.px);
        std::swap(pn, other.pn);
    }

private:

    T * px;            // contained pointer
    count_type * pn;   // ptr to reference counter
      
};  // shared_array

template<class T, class U> inline bool operator==(shared_array<T> const & a, shared_array<U> const & b)
{
    return a.get() == b.get();
}

template<class T, class U> inline bool operator!=(shared_array<T> const & a, shared_array<U> const & b)
{
    return a.get() != b.get();
}

template<class T> inline bool operator<(shared_array<T> const & a, shared_array<T> const & b)
{
    return std::less<T*>()(a.get(), b.get());
}

template<class T> void swap(shared_array<T> & a, shared_array<T> & b)
{
    a.swap(b);
}

} // namespace boost

#endif  // #ifndef BOOST_SMART_PTR_DETAIL_SHARED_ARRAY_NMT_HPP_INCLUDED
