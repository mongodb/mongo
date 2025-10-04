#ifndef BOOST_THREAD_DETAIL_STRING_TO_UNSIGNED_HPP_INCLUDED
#define BOOST_THREAD_DETAIL_STRING_TO_UNSIGNED_HPP_INCLUDED

// Copyright 2023 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <string>
#include <climits>

namespace boost
{
namespace thread_detail
{

inline bool string_to_unsigned( std::string const& s, unsigned& v )
{
    v = 0;

    if( s.empty() )
    {
        return false;
    }

    for( char const* p = s.c_str(); *p; ++p )
    {
        unsigned char ch = static_cast<unsigned char>( *p );

        if( ch < '0' || ch > '9' )
        {
            return false;
        }

        if( v > UINT_MAX / 10 )
        {
            return false;
        }

        unsigned q = static_cast<unsigned>( ch - '0' );

        if( v == UINT_MAX / 10 && q > UINT_MAX % 10 )
        {
            return false;
        }

        v = v * 10 + q;
    }

    return true;
}

} // namespace thread_detail
} // namespace boost

#endif // #ifndef BOOST_THREAD_DETAIL_STRING_TO_UNSIGNED_HPP_INCLUDED
