#ifndef BOOST_SMART_PTR_SHARED_ARRAY_HPP_INCLUDED
#define BOOST_SMART_PTR_SHARED_ARRAY_HPP_INCLUDED

//
//  shared_array.hpp
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

#include <boost/config.hpp>   // for broken compiler workarounds

#if defined(BOOST_NO_MEMBER_TEMPLATES) && !defined(BOOST_MSVC6_MEMBER_TEMPLATES)
#include <boost/smart_ptr/detail/shared_array_nmt.hpp>
#else

#include <memory>             // TR1 cyclic inclusion fix

#include <boost/assert.hpp>
#include <boost/checked_delete.hpp>

#include <boost/smart_ptr/detail/shared_count.hpp>
#include <boost/detail/workaround.hpp>

#include <cstddef>            // for std::ptrdiff_t
#include <algorithm>          // for std::swap
#include <functional>         // for std::less

namespace boost
{

//
//  shared_array
//
//  shared_array extends shared_ptr to arrays.
//  The array pointed to is deleted when the last shared_array pointing to it
//  is destroyed or reset.
//

template<class T> class shared_array
{
private:

    // Borland 5.5.1 specific workarounds
    typedef checked_array_deleter<T> deleter;
    typedef shared_array<T> this_type;

public:

    typedef T element_type;

    explicit shared_array(T * p = 0): px(p), pn(p, deleter())
    {
    }

    //
    // Requirements: D's copy constructor must not throw
    //
    // shared_array will release p by calling d(p)
    //

    template<class D> shared_array(T * p, D d): px(p), pn(p, d)
    {
    }

//  generated copy constructor, destructor are fine...

#if defined( BOOST_HAS_RVALUE_REFS )

// ... except in C++0x, move disables the implicit copy

    shared_array( shared_array const & r ): px( r.px ), pn( r.pn ) // never throws
    {
    }

#endif

    // assignment

    shared_array & operator=( shared_array const & r ) // never throws
    {
        this_type( r ).swap( *this );
        return *this;
    }

    void reset(T * p = 0)
    {
        BOOST_ASSERT(p == 0 || p != px);
        this_type(p).swap(*this);
    }

    template <class D> void reset(T * p, D d)
    {
        this_type(p, d).swap(*this);
    }

    T & operator[] (std::ptrdiff_t i) const // never throws
    {
        BOOST_ASSERT(px != 0);
        BOOST_ASSERT(i >= 0);
        return px[i];
    }
    
    T * get() const // never throws
    {
        return px;
    }

// implicit conversion to "bool"
#include <boost/smart_ptr/detail/operator_bool.hpp>

    bool unique() const // never throws
    {
        return pn.unique();
    }

    long use_count() const // never throws
    {
        return pn.use_count();
    }

    void swap(shared_array<T> & other) // never throws
    {
        std::swap(px, other.px);
        pn.swap(other.pn);
    }

    void * _internal_get_deleter( boost::detail::sp_typeinfo const & ti ) const
    {
        return pn.get_deleter( ti );
    }

private:

    T * px;                     // contained pointer
    detail::shared_count pn;    // reference counter

};  // shared_array

template<class T> inline bool operator==(shared_array<T> const & a, shared_array<T> const & b) // never throws
{
    return a.get() == b.get();
}

template<class T> inline bool operator!=(shared_array<T> const & a, shared_array<T> const & b) // never throws
{
    return a.get() != b.get();
}

template<class T> inline bool operator<(shared_array<T> const & a, shared_array<T> const & b) // never throws
{
    return std::less<T*>()(a.get(), b.get());
}

template<class T> void swap(shared_array<T> & a, shared_array<T> & b) // never throws
{
    a.swap(b);
}

template< class D, class T > D * get_deleter( shared_array<T> const & p )
{
    return static_cast< D * >( p._internal_get_deleter( BOOST_SP_TYPEID(D) ) );
}

} // namespace boost

#endif  // #if defined(BOOST_NO_MEMBER_TEMPLATES) && !defined(BOOST_MSVC6_MEMBER_TEMPLATES)

#endif  // #ifndef BOOST_SMART_PTR_SHARED_ARRAY_HPP_INCLUDED
