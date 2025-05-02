// Copyright 2021-2023 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_HASH_DETAIL_HASH_INTEGRAL_HPP
#define BOOST_HASH_DETAIL_HASH_INTEGRAL_HPP

#include <boost/container_hash/detail/hash_mix.hpp>
#include <type_traits>
#include <cstddef>
#include <climits>

namespace boost
{
namespace hash_detail
{

// libstdc++ doesn't provide support for __int128 in the standard traits

template<class T> struct is_integral: public std::is_integral<T>
{
};

template<class T> struct is_unsigned: public std::is_unsigned<T>
{
};

template<class T> struct make_unsigned: public std::make_unsigned<T>
{
};

#if defined(__SIZEOF_INT128__)

template<> struct is_integral<__int128_t>: public std::true_type
{
};

template<> struct is_integral<__uint128_t>: public std::true_type
{
};

template<> struct is_unsigned<__int128_t>: public std::false_type
{
};

template<> struct is_unsigned<__uint128_t>: public std::true_type
{
};

template<> struct make_unsigned<__int128_t>
{
    typedef __uint128_t type;
};

template<> struct make_unsigned<__uint128_t>
{
    typedef __uint128_t type;
};

#endif

template<class T,
    bool bigger_than_size_t = (sizeof(T) > sizeof(std::size_t)),
    bool is_unsigned = is_unsigned<T>::value,
    std::size_t size_t_bits = sizeof(std::size_t) * CHAR_BIT,
    std::size_t type_bits = sizeof(T) * CHAR_BIT>
struct hash_integral_impl;

template<class T, bool is_unsigned, std::size_t size_t_bits, std::size_t type_bits> struct hash_integral_impl<T, false, is_unsigned, size_t_bits, type_bits>
{
    static std::size_t fn( T v )
    {
        return static_cast<std::size_t>( v );
    }
};

template<class T, std::size_t size_t_bits, std::size_t type_bits> struct hash_integral_impl<T, true, false, size_t_bits, type_bits>
{
    static std::size_t fn( T v )
    {
        typedef typename make_unsigned<T>::type U;

        if( v >= 0 )
        {
            return hash_integral_impl<U>::fn( static_cast<U>( v ) );
        }
        else
        {
            return ~hash_integral_impl<U>::fn( static_cast<U>( ~static_cast<U>( v ) ) );
        }
    }
};

template<class T> struct hash_integral_impl<T, true, true, 32, 64>
{
    static std::size_t fn( T v )
    {
        std::size_t seed = 0;

        seed = static_cast<std::size_t>( v >> 32 ) + hash_detail::hash_mix( seed );
        seed = static_cast<std::size_t>( v  & 0xFFFFFFFF ) + hash_detail::hash_mix( seed );

        return seed;
    }
};

template<class T> struct hash_integral_impl<T, true, true, 32, 128>
{
    static std::size_t fn( T v )
    {
        std::size_t seed = 0;

        seed = static_cast<std::size_t>( v >> 96 ) + hash_detail::hash_mix( seed );
        seed = static_cast<std::size_t>( v >> 64 ) + hash_detail::hash_mix( seed );
        seed = static_cast<std::size_t>( v >> 32 ) + hash_detail::hash_mix( seed );
        seed = static_cast<std::size_t>( v ) + hash_detail::hash_mix( seed );

        return seed;
    }
};

template<class T> struct hash_integral_impl<T, true, true, 64, 128>
{
    static std::size_t fn( T v )
    {
        std::size_t seed = 0;

        seed = static_cast<std::size_t>( v >> 64 ) + hash_detail::hash_mix( seed );
        seed = static_cast<std::size_t>( v ) + hash_detail::hash_mix( seed );

        return seed;
    }
};

} // namespace hash_detail

template <typename T>
typename std::enable_if<hash_detail::is_integral<T>::value, std::size_t>::type
    hash_value( T v )
{
    return hash_detail::hash_integral_impl<T>::fn( v );
}

} // namespace boost

#endif // #ifndef BOOST_HASH_DETAIL_HASH_INTEGRAL_HPP
