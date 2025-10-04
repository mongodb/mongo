#ifndef BOOST_UUID_DETAIL_NUMERIC_CAST_INCLUDED
#define BOOST_UUID_DETAIL_NUMERIC_CAST_INCLUDED

// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/uuid/detail/static_assert.hpp>
#include <boost/throw_exception.hpp>
#include <stdexcept>
#include <limits>
#include <type_traits>

namespace boost {
namespace uuids {
namespace detail {

template<class T, class U> T numeric_cast( U u )
{
    BOOST_UUID_STATIC_ASSERT( std::is_integral<T>::value );
    BOOST_UUID_STATIC_ASSERT( std::is_unsigned<T>::value );

    BOOST_UUID_STATIC_ASSERT( std::is_integral<U>::value );
    BOOST_UUID_STATIC_ASSERT( std::is_unsigned<U>::value );

    if( u > std::numeric_limits<T>::max() )
    {
        BOOST_THROW_EXCEPTION( std::range_error( "Argument to numeric_cast is out of range of destination type" ) );
    }

    return static_cast<T>( u );
}

} // detail
} // uuids
} // boost

#endif // #ifndef BOOST_UUID_DETAIL_NUMERIC_CAST_INCLUDED
