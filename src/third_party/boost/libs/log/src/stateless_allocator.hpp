/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   stateless_allocator.hpp
 * \author Andrey Semashev
 * \date   11.02.2012
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_STATELESS_ALLOCATOR_HPP_INCLUDED_
#define BOOST_LOG_STATELESS_ALLOCATOR_HPP_INCLUDED_

#include <boost/log/detail/config.hpp>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <boost/log/detail/header.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

#if defined(_STLPORT_VERSION)

#if !defined(BOOST_NO_CXX11_TEMPLATE_ALIASES)

template< typename T >
using stateless_allocator = std::allocator< T >;

#else

template< typename T >
struct stateless_allocator :
    public std::allocator< T >
{
};

#endif

#else

template< typename T >
struct stateless_allocator
{
    template< typename U >
    struct rebind
    {
        typedef stateless_allocator< U > other;
    };

    typedef T value_type;
    typedef value_type* pointer;
    typedef value_type const* const_pointer;
    typedef value_type& reference;
    typedef value_type const& const_reference;
    typedef std::size_t size_type;
    typedef std::ptrdiff_t difference_type;

    static pointer allocate(size_type n, const void* = NULL)
    {
        pointer p = static_cast< pointer >(std::malloc(n * sizeof(value_type)));
        if (p)
            return p;
        else
            throw std::bad_alloc();
    }
    static void deallocate(pointer p, size_type)
    {
        std::free(p);
    }
};

#endif

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_STATELESS_ALLOCATOR_HPP_INCLUDED_
