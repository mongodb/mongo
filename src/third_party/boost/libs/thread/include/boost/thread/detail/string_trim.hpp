#ifndef BOOST_THREAD_DETAIL_STRING_TRIM_HPP_INCLUDED
#define BOOST_THREAD_DETAIL_STRING_TRIM_HPP_INCLUDED

// Copyright 2023 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <string>

namespace boost
{
namespace thread_detail
{

inline std::string string_trim( std::string const& s )
{
    std::size_t i = s.find_first_not_of( " \t\r\n" );

    if( i == std::string::npos ) return std::string();

    std::size_t j = s.find_last_not_of( " \t\r\n" );

    return s.substr( i, j + 1 - i );
}

} // namespace thread_detail
} // namespace boost

#endif // #ifndef BOOST_THREAD_DETAIL_STRING_TRIM_HPP_INCLUDED
