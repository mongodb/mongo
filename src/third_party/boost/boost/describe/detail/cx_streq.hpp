#ifndef BOOST_DESCRIBE_DETAIL_CX_STREQ_HPP_INCLUDED
#define BOOST_DESCRIBE_DETAIL_CX_STREQ_HPP_INCLUDED

// Copyright 2021 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/describe/detail/config.hpp>

#if defined(BOOST_DESCRIBE_CXX11)

namespace boost
{
namespace describe
{
namespace detail
{

constexpr bool cx_streq( char const * s1, char const * s2 )
{
    return s1[0] == s2[0] && ( s1[0] == 0 || cx_streq( s1 + 1, s2 + 1 ) );
}

} // namespace detail
} // namespace describe
} // namespace boost

#endif // defined(BOOST_DESCRIBE_CXX11)

#endif // #ifndef BOOST_DESCRIBE_DETAIL_CX_STREQ_HPP_INCLUDED
