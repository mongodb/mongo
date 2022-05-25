#ifndef BOOST_SYSTEM_DETAIL_APPEND_INT_HPP_INCLUDED
#define BOOST_SYSTEM_DETAIL_APPEND_INT_HPP_INCLUDED

// Copyright 2021 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt)

#include <boost/system/detail/snprintf.hpp>
#include <string>

//

namespace boost
{
namespace system
{
namespace detail
{

inline void append_int( std::string& s, int v )
{
    char buffer[ 32 ];
    detail::snprintf( buffer, sizeof( buffer ), ":%d", v );

    s += buffer;
}

} // namespace detail
} // namespace system
} // namespace boost

#endif // #ifndef BOOST_SYSTEM_DETAIL_APPEND_INT_HPP_INCLUDED
