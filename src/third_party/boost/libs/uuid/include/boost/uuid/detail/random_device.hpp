#ifndef BOOST_UUID_DETAIL_RANDOM_DEVICE_HPP_INCLUDED
#define BOOST_UUID_DETAIL_RANDOM_DEVICE_HPP_INCLUDED

// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#if defined(__MINGW32__)

// Under MinGW up to GCC 9, std::random_device is
// deterministic and always produces the same
// sequence

#include <boost/throw_exception.hpp>
#include <system_error>
#include <limits>
#include <stdlib.h>

extern "C" int __cdecl rand_s( unsigned int *randomValue );

namespace boost {
namespace uuids {
namespace detail {

struct random_device
{
    // noncopyable to match std::random_device
    random_device() = default;
    random_device( random_device&& ) = delete;
    random_device& operator=( random_device&& ) = delete;

    using result_type = unsigned;

    static constexpr result_type min()
    {
        return std::numeric_limits<result_type>::min();
    }

    static constexpr result_type max()
    {
        return std::numeric_limits<result_type>::max();
    }

    result_type operator()()
    {
        unsigned v;

        auto r = rand_s( &v );

        if( r != 0 )
        {
            BOOST_THROW_EXCEPTION( std::system_error( r, std::generic_category(), "rand_s" ) );
        }

        return v;
    }
};

} // detail
} // uuids
} // boost

#else

#include <random>

namespace boost {
namespace uuids {
namespace detail {

using std::random_device;

} // detail
} // uuids
} // boost

#endif

#endif // BOOST_UUID_DETAIL_RANDOM_DEVICE_HPP_INCLUDED
