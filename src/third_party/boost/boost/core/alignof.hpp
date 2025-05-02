#ifndef BOOST_CORE_ALIGNOF_HPP_INCLUDED
#define BOOST_CORE_ALIGNOF_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

//  Copyright 2023 Peter Dimov
//  Distributed under the Boost Software License, Version 1.0.
//  https://www.boost.org/LICENSE_1_0.txt

#include <boost/config.hpp>
#include <cstddef>

#if !defined(BOOST_NO_CXX11_ALIGNOF)

#define BOOST_CORE_ALIGNOF alignof

#elif defined(__GNUC__)

#define BOOST_CORE_ALIGNOF __alignof__

#elif defined(_MSC_VER)

#define BOOST_CORE_ALIGNOF __alignof

#else

namespace boost
{
namespace core
{
namespace detail
{

template<class T> struct alignof_helper
{
    char x;
    T t;
};

} // namespace detail
} // namespace core
} // namespace boost

#if defined(__GNUC__)
// ignoring -Wvariadic-macros with #pragma doesn't work under GCC
# pragma GCC system_header
#endif

#define BOOST_CORE_ALIGNOF(...) offsetof( ::boost::core::detail::alignof_helper<__VA_ARGS__>, t );

#endif

#endif  // #ifndef BOOST_CORE_ALIGNOF_HPP_INCLUDED
