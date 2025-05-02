#ifndef BOOST_UUID_DETAIL_HASH_MIX_INCLUDED
#define BOOST_UUID_DETAIL_HASH_MIX_INCLUDED

// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <cstdint>

namespace boost {
namespace uuids {
namespace detail {

// The multipliers are 32 bit, which makes the product
// easier to compute on 32 bit platforms.
//
// The mixing functions have been created with
// https://github.com/skeeto/hash-prospector

// prospector -p mul,xorr -t 1000
// score = 592.20293470138972
inline std::uint64_t hash_mix_mx( std::uint64_t x ) noexcept
{
    x *= 0xD96AAA55;
    x ^= x >> 16;
    return x;
}

// prospector -p mul:0xD96AAA55,xorr:16,mul,xorr -t 1000
// score = 79.5223047689704
// (with mx prepended)
inline std::uint64_t hash_mix_fmx( std::uint64_t x ) noexcept
{
    x *= 0x7DF954AB;
    x ^= x >> 16;
    return x;
}

} // detail
} // uuids
} // boost

#endif // #ifndef BOOST_UUID_DETAIL_HASH_MIX_INCLUDED
