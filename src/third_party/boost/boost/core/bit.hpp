#ifndef BOOST_CORE_BIT_HPP_INCLUDED
#define BOOST_CORE_BIT_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

// boost/core/bit.hpp
//
// A portable version of the C++20 standard header <bit>
//
// Copyright 2020 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/config.hpp>
#include <boost/static_assert.hpp>
#include <boost/cstdint.hpp>
#include <limits>
#include <cstring>
#include <cstdlib>

#if defined(_MSC_VER)

# include <intrin.h>
# pragma intrinsic(_BitScanForward)
# pragma intrinsic(_BitScanReverse)

# if defined(_M_X64)
#  pragma intrinsic(_BitScanForward64)
#  pragma intrinsic(_BitScanReverse64)
# endif

# pragma warning(push)
# pragma warning(disable: 4127) // conditional expression is constant
# pragma warning(disable: 4244) // conversion from int to T

#endif // defined(_MSC_VER)

#if defined(BOOST_MSVC) && BOOST_MSVC >= 1925
# define BOOST_CORE_HAS_BUILTIN_ISCONSTEVAL
#endif

#if defined(__has_builtin)
# if __has_builtin(__builtin_bit_cast)
#  define BOOST_CORE_HAS_BUILTIN_BIT_CAST
# endif
# if __has_builtin(__builtin_bswap16)
#  define BOOST_CORE_HAS_BUILTIN_BSWAP16
# endif
#endif

#if !defined(BOOST_CORE_HAS_BUILTIN_BIT_CAST) && (defined(BOOST_MSVC) && BOOST_MSVC >= 1926)
#  define BOOST_CORE_HAS_BUILTIN_BIT_CAST
#endif

#if !defined(BOOST_CORE_HAS_BUILTIN_BSWAP16) && (defined(BOOST_GCC) && BOOST_GCC >= 40800)
#  define BOOST_CORE_HAS_BUILTIN_BSWAP16
#endif

namespace boost
{
namespace core
{

// bit_cast

#if defined(BOOST_CORE_HAS_BUILTIN_BIT_CAST)

template<class To, class From>
BOOST_CONSTEXPR To bit_cast( From const & from ) BOOST_NOEXCEPT
{
    return __builtin_bit_cast( To, from );
}

#else

template<class To, class From>
To bit_cast( From const & from ) BOOST_NOEXCEPT
{
    BOOST_STATIC_ASSERT( sizeof(To) == sizeof(From) );

    To to;
    std::memcpy( &to, &from, sizeof(To) );
    return to;
}

#endif

// countl

#if defined(__GNUC__) || defined(__clang__)

namespace detail
{

BOOST_CONSTEXPR inline int countl_impl( unsigned char x ) BOOST_NOEXCEPT
{
    return x? __builtin_clz( x ) - ( std::numeric_limits<unsigned int>::digits - std::numeric_limits<unsigned char>::digits ): std::numeric_limits<unsigned char>::digits;
}

BOOST_CONSTEXPR inline int countl_impl( unsigned short x ) BOOST_NOEXCEPT
{
    return x? __builtin_clz( x ) - ( std::numeric_limits<unsigned int>::digits - std::numeric_limits<unsigned short>::digits ): std::numeric_limits<unsigned short>::digits;
}

BOOST_CONSTEXPR inline int countl_impl( unsigned int x ) BOOST_NOEXCEPT
{
    return x? __builtin_clz( x ): std::numeric_limits<unsigned int>::digits;
}

BOOST_CONSTEXPR inline int countl_impl( unsigned long x ) BOOST_NOEXCEPT
{
    return x? __builtin_clzl( x ): std::numeric_limits<unsigned long>::digits;
}

BOOST_CONSTEXPR inline int countl_impl( boost::ulong_long_type x ) BOOST_NOEXCEPT
{
    return x? __builtin_clzll( x ): std::numeric_limits<boost::ulong_long_type>::digits;
}

} // namespace detail

template<class T>
BOOST_CONSTEXPR int countl_zero( T x ) BOOST_NOEXCEPT
{
    BOOST_STATIC_ASSERT( std::numeric_limits<T>::is_integer && !std::numeric_limits<T>::is_signed );

    return boost::core::detail::countl_impl( x );
}

#else // defined(__GNUC__) || defined(__clang__)

namespace detail
{

#if defined(_MSC_VER) && defined(BOOST_CORE_HAS_BUILTIN_ISCONSTEVAL)

BOOST_CXX14_CONSTEXPR inline int countl_impl( boost::uint32_t x ) BOOST_NOEXCEPT
{
    if( __builtin_is_constant_evaluated() )
    {
        constexpr unsigned char mod37[ 37 ] = { 32, 31, 6, 30, 9, 5, 0, 29, 16, 8, 2, 4, 21, 0, 19, 28, 25, 15, 0, 7, 10, 1, 17, 3, 22, 20, 26, 0, 11, 18, 23, 27, 12, 24, 13, 14, 0 };

        x |= x >> 1;
        x |= x >> 2;
        x |= x >> 4;
        x |= x >> 8;
        x |= x >> 16;

        return mod37[ x % 37 ];
    }
    else
    {
        unsigned long r;

        if( _BitScanReverse( &r, x ) )
        {
            return 31 - static_cast<int>( r );
        }
        else
        {
            return 32;
        }
    }
}

BOOST_CXX14_CONSTEXPR inline int countl_impl( boost::uint8_t x ) BOOST_NOEXCEPT
{
    return boost::core::detail::countl_impl( static_cast<boost::uint32_t>( x ) ) - 24;
}

BOOST_CXX14_CONSTEXPR inline int countl_impl( boost::uint16_t x ) BOOST_NOEXCEPT
{
    return boost::core::detail::countl_impl( static_cast<boost::uint32_t>( x ) ) - 16;
}

#elif defined(_MSC_VER)

inline int countl_impl( boost::uint32_t x ) BOOST_NOEXCEPT
{
    unsigned long r;

    if( _BitScanReverse( &r, x ) )
    {
        return 31 - static_cast<int>( r );
    }
    else
    {
        return 32;
    }
}

inline int countl_impl( boost::uint8_t x ) BOOST_NOEXCEPT
{
    return boost::core::detail::countl_impl( static_cast<boost::uint32_t>( x ) ) - 24;
}

inline int countl_impl( boost::uint16_t x ) BOOST_NOEXCEPT
{
    return boost::core::detail::countl_impl( static_cast<boost::uint32_t>( x ) ) - 16;
}

#else

inline int countl_impl( boost::uint32_t x ) BOOST_NOEXCEPT
{
    static unsigned char const mod37[ 37 ] = { 32, 31, 6, 30, 9, 5, 0, 29, 16, 8, 2, 4, 21, 0, 19, 28, 25, 15, 0, 7, 10, 1, 17, 3, 22, 20, 26, 0, 11, 18, 23, 27, 12, 24, 13, 14, 0 };

    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;

    return mod37[ x % 37 ];
}

inline int countl_impl( boost::uint8_t x ) BOOST_NOEXCEPT
{
    return boost::core::detail::countl_impl( static_cast<boost::uint32_t>( x ) ) - 24;
}

inline int countl_impl( boost::uint16_t x ) BOOST_NOEXCEPT
{
    return boost::core::detail::countl_impl( static_cast<boost::uint32_t>( x ) ) - 16;
}

#endif

#if defined(_MSC_VER) && defined(_M_X64) && defined(BOOST_CORE_HAS_BUILTIN_ISCONSTEVAL)

BOOST_CXX14_CONSTEXPR inline int countl_impl( boost::uint64_t x ) BOOST_NOEXCEPT
{
    if( __builtin_is_constant_evaluated() )
    {
        return static_cast<boost::uint32_t>( x >> 32 ) != 0?
            boost::core::detail::countl_impl( static_cast<boost::uint32_t>( x >> 32 ) ):
            boost::core::detail::countl_impl( static_cast<boost::uint32_t>( x ) ) + 32;
    }
    else
    {
        unsigned long r;

        if( _BitScanReverse64( &r, x ) )
        {
            return 63 - static_cast<int>( r );
        }
        else
        {
            return 64;
        }
    }
}

#elif defined(_MSC_VER) && defined(_M_X64)

inline int countl_impl( boost::uint64_t x ) BOOST_NOEXCEPT
{
    unsigned long r;

    if( _BitScanReverse64( &r, x ) )
    {
        return 63 - static_cast<int>( r );
    }
    else
    {
        return 64;
    }
}

#elif defined(_MSC_VER) && defined(BOOST_CORE_HAS_BUILTIN_ISCONSTEVAL)

BOOST_CXX14_CONSTEXPR inline int countl_impl( boost::uint64_t x ) BOOST_NOEXCEPT
{
    return static_cast<boost::uint32_t>( x >> 32 ) != 0?
        boost::core::detail::countl_impl( static_cast<boost::uint32_t>( x >> 32 ) ):
        boost::core::detail::countl_impl( static_cast<boost::uint32_t>( x ) ) + 32;
}

#else

inline int countl_impl( boost::uint64_t x ) BOOST_NOEXCEPT
{
    return static_cast<boost::uint32_t>( x >> 32 ) != 0?
        boost::core::detail::countl_impl( static_cast<boost::uint32_t>( x >> 32 ) ):
        boost::core::detail::countl_impl( static_cast<boost::uint32_t>( x ) ) + 32;
}

#endif

} // namespace detail

template<class T>
BOOST_CXX14_CONSTEXPR int countl_zero( T x ) BOOST_NOEXCEPT
{
    BOOST_STATIC_ASSERT( std::numeric_limits<T>::is_integer && !std::numeric_limits<T>::is_signed );

    BOOST_STATIC_ASSERT( sizeof(T) == sizeof(boost::uint8_t) || sizeof(T) == sizeof(boost::uint16_t) || sizeof(T) == sizeof(boost::uint32_t) || sizeof(T) == sizeof(boost::uint64_t) );

    BOOST_IF_CONSTEXPR ( sizeof(T) == sizeof(boost::uint8_t) )
    {
        return boost::core::detail::countl_impl( static_cast<boost::uint8_t>( x ) );
    }
    else BOOST_IF_CONSTEXPR ( sizeof(T) == sizeof(boost::uint16_t) )
    {
        return boost::core::detail::countl_impl( static_cast<boost::uint16_t>( x ) );
    }
    else BOOST_IF_CONSTEXPR ( sizeof(T) == sizeof(boost::uint32_t) )
    {
        return boost::core::detail::countl_impl( static_cast<boost::uint32_t>( x ) );
    }
    else
    {
        return boost::core::detail::countl_impl( static_cast<boost::uint64_t>( x ) );
    }
}

#endif // defined(__GNUC__) || defined(__clang__)

template<class T>
BOOST_CONSTEXPR int countl_one( T x ) BOOST_NOEXCEPT
{
    BOOST_STATIC_ASSERT( std::numeric_limits<T>::is_integer && !std::numeric_limits<T>::is_signed );

    return boost::core::countl_zero( static_cast<T>( ~x ) );
}

// countr

#if defined(__GNUC__) || defined(__clang__)

namespace detail
{

BOOST_CONSTEXPR inline int countr_impl( unsigned char x ) BOOST_NOEXCEPT
{
    return x? __builtin_ctz( x ): std::numeric_limits<unsigned char>::digits;
}

BOOST_CONSTEXPR inline int countr_impl( unsigned short x ) BOOST_NOEXCEPT
{
    return x? __builtin_ctz( x ): std::numeric_limits<unsigned short>::digits;
}

BOOST_CONSTEXPR inline int countr_impl( unsigned int x ) BOOST_NOEXCEPT
{
    return x? __builtin_ctz( x ): std::numeric_limits<unsigned int>::digits;
}

BOOST_CONSTEXPR inline int countr_impl( unsigned long x ) BOOST_NOEXCEPT
{
    return x? __builtin_ctzl( x ): std::numeric_limits<unsigned long>::digits;
}

BOOST_CONSTEXPR inline int countr_impl( boost::ulong_long_type x ) BOOST_NOEXCEPT
{
    return x? __builtin_ctzll( x ): std::numeric_limits<boost::ulong_long_type>::digits;
}

} // namespace detail

template<class T>
BOOST_CONSTEXPR int countr_zero( T x ) BOOST_NOEXCEPT
{
    BOOST_STATIC_ASSERT( std::numeric_limits<T>::is_integer && !std::numeric_limits<T>::is_signed );

    return boost::core::detail::countr_impl( x );
}

#else // defined(__GNUC__) || defined(__clang__)

namespace detail
{

#if defined(_MSC_VER) && defined(BOOST_CORE_HAS_BUILTIN_ISCONSTEVAL)

BOOST_CXX14_CONSTEXPR inline int countr_impl( boost::uint32_t x ) BOOST_NOEXCEPT
{
    if( __builtin_is_constant_evaluated() )
    {
        constexpr unsigned char mod37[ 37 ] = { 32, 0, 1, 26, 2, 23, 27, 0, 3, 16, 24, 30, 28, 11, 0, 13, 4, 7, 17, 0, 25, 22, 31, 15, 29, 10, 12, 6, 0, 21, 14, 9, 5, 20, 8, 19, 18 };
        return mod37[ ( -(boost::int32_t)x & x ) % 37 ];
    }
    else
    {
        unsigned long r;

        if( _BitScanForward( &r, x ) )
        {
            return static_cast<int>( r );
        }
        else
        {
            return 32;
        }
    }
}

BOOST_CXX14_CONSTEXPR inline int countr_impl( boost::uint8_t x ) BOOST_NOEXCEPT
{
    return boost::core::detail::countr_impl( static_cast<boost::uint32_t>( x ) | 0x100 );
}

BOOST_CXX14_CONSTEXPR inline int countr_impl( boost::uint16_t x ) BOOST_NOEXCEPT
{
    return boost::core::detail::countr_impl( static_cast<boost::uint32_t>( x ) | 0x10000 );
}

#elif defined(_MSC_VER)

inline int countr_impl( boost::uint32_t x ) BOOST_NOEXCEPT
{
    unsigned long r;

    if( _BitScanForward( &r, x ) )
    {
        return static_cast<int>( r );
    }
    else
    {
        return 32;
    }
}

inline int countr_impl( boost::uint8_t x ) BOOST_NOEXCEPT
{
    return boost::core::detail::countr_impl( static_cast<boost::uint32_t>( x ) | 0x100 );
}

inline int countr_impl( boost::uint16_t x ) BOOST_NOEXCEPT
{
    return boost::core::detail::countr_impl( static_cast<boost::uint32_t>( x ) | 0x10000 );
}

#else

inline int countr_impl( boost::uint32_t x ) BOOST_NOEXCEPT
{
    static unsigned char const mod37[ 37 ] = { 32, 0, 1, 26, 2, 23, 27, 0, 3, 16, 24, 30, 28, 11, 0, 13, 4, 7, 17, 0, 25, 22, 31, 15, 29, 10, 12, 6, 0, 21, 14, 9, 5, 20, 8, 19, 18 };
    return mod37[ ( -(boost::int32_t)x & x ) % 37 ];
}

inline int countr_impl( boost::uint8_t x ) BOOST_NOEXCEPT
{
    return boost::core::detail::countr_impl( static_cast<boost::uint32_t>( x ) | 0x100 );
}

inline int countr_impl( boost::uint16_t x ) BOOST_NOEXCEPT
{
    return boost::core::detail::countr_impl( static_cast<boost::uint32_t>( x ) | 0x10000 );
}

#endif

#if defined(_MSC_VER) && defined(_M_X64) && defined(BOOST_CORE_HAS_BUILTIN_ISCONSTEVAL)

BOOST_CXX14_CONSTEXPR inline int countr_impl( boost::uint64_t x ) BOOST_NOEXCEPT
{
    if( __builtin_is_constant_evaluated() )
    {
        return static_cast<boost::uint32_t>( x ) != 0?
            boost::core::detail::countr_impl( static_cast<boost::uint32_t>( x ) ):
            boost::core::detail::countr_impl( static_cast<boost::uint32_t>( x >> 32 ) ) + 32;
    }
    else
    {
        unsigned long r;

        if( _BitScanForward64( &r, x ) )
        {
            return static_cast<int>( r );
        }
        else
        {
            return 64;
        }
    }
}

#elif defined(_MSC_VER) && defined(_M_X64)

inline int countr_impl( boost::uint64_t x ) BOOST_NOEXCEPT
{
    unsigned long r;

    if( _BitScanForward64( &r, x ) )
    {
        return static_cast<int>( r );
    }
    else
    {
        return 64;
    }
}

#elif defined(_MSC_VER) && defined(BOOST_CORE_HAS_BUILTIN_ISCONSTEVAL)

BOOST_CXX14_CONSTEXPR inline int countr_impl( boost::uint64_t x ) BOOST_NOEXCEPT
{
    return static_cast<boost::uint32_t>( x ) != 0?
        boost::core::detail::countr_impl( static_cast<boost::uint32_t>( x ) ):
        boost::core::detail::countr_impl( static_cast<boost::uint32_t>( x >> 32 ) ) + 32;
}

#else

inline int countr_impl( boost::uint64_t x ) BOOST_NOEXCEPT
{
    return static_cast<boost::uint32_t>( x ) != 0?
        boost::core::detail::countr_impl( static_cast<boost::uint32_t>( x ) ):
        boost::core::detail::countr_impl( static_cast<boost::uint32_t>( x >> 32 ) ) + 32;
}

#endif

} // namespace detail

template<class T>
BOOST_CXX14_CONSTEXPR int countr_zero( T x ) BOOST_NOEXCEPT
{
    BOOST_STATIC_ASSERT( std::numeric_limits<T>::is_integer && !std::numeric_limits<T>::is_signed );

    BOOST_STATIC_ASSERT( sizeof(T) == sizeof(boost::uint8_t) || sizeof(T) == sizeof(boost::uint16_t) || sizeof(T) == sizeof(boost::uint32_t) || sizeof(T) == sizeof(boost::uint64_t) );

    BOOST_IF_CONSTEXPR ( sizeof(T) == sizeof(boost::uint8_t) )
    {
        return boost::core::detail::countr_impl( static_cast<boost::uint8_t>( x ) );
    }
    else BOOST_IF_CONSTEXPR ( sizeof(T) == sizeof(boost::uint16_t) )
    {
        return boost::core::detail::countr_impl( static_cast<boost::uint16_t>( x ) );
    }
    else BOOST_IF_CONSTEXPR ( sizeof(T) == sizeof(boost::uint32_t) )
    {
        return boost::core::detail::countr_impl( static_cast<boost::uint32_t>( x ) );
    }
    else
    {
        return boost::core::detail::countr_impl( static_cast<boost::uint64_t>( x ) );
    }
}

#endif // defined(__GNUC__) || defined(__clang__)

template<class T>
BOOST_CONSTEXPR int countr_one( T x ) BOOST_NOEXCEPT
{
    BOOST_STATIC_ASSERT( std::numeric_limits<T>::is_integer && !std::numeric_limits<T>::is_signed );

    return boost::core::countr_zero( static_cast<T>( ~x ) );
}

// popcount

#if defined(__GNUC__) || defined(__clang__)

#if defined(__clang__) && __clang_major__ * 100 + __clang_minor__ < 304
# define BOOST_CORE_POPCOUNT_CONSTEXPR
#else
# define BOOST_CORE_POPCOUNT_CONSTEXPR BOOST_CONSTEXPR
#endif

namespace detail
{

BOOST_CORE_POPCOUNT_CONSTEXPR inline int popcount_impl( unsigned char x ) BOOST_NOEXCEPT
{
    return __builtin_popcount( x );
}

BOOST_CORE_POPCOUNT_CONSTEXPR inline int popcount_impl( unsigned short x ) BOOST_NOEXCEPT
{
    return __builtin_popcount( x );
}

BOOST_CORE_POPCOUNT_CONSTEXPR inline int popcount_impl( unsigned int x ) BOOST_NOEXCEPT
{
    return __builtin_popcount( x );
}

BOOST_CORE_POPCOUNT_CONSTEXPR inline int popcount_impl( unsigned long x ) BOOST_NOEXCEPT
{
    return __builtin_popcountl( x );
}

BOOST_CORE_POPCOUNT_CONSTEXPR inline int popcount_impl( boost::ulong_long_type x ) BOOST_NOEXCEPT
{
    return __builtin_popcountll( x );
}

} // namespace detail

#undef BOOST_CORE_POPCOUNT_CONSTEXPR

template<class T>
BOOST_CONSTEXPR int popcount( T x ) BOOST_NOEXCEPT
{
    BOOST_STATIC_ASSERT( std::numeric_limits<T>::is_integer && !std::numeric_limits<T>::is_signed );

    return boost::core::detail::popcount_impl( x );
}

#else // defined(__GNUC__) || defined(__clang__)

namespace detail
{

BOOST_CXX14_CONSTEXPR inline int popcount_impl( boost::uint32_t x ) BOOST_NOEXCEPT
{
    x = x - ( ( x >> 1 ) & 0x55555555 );
    x = ( x & 0x33333333 ) + ( ( x >> 2 ) & 0x33333333 );
    x = ( x + ( x >> 4 ) ) & 0x0F0F0F0F;

    return static_cast<unsigned>( ( x * 0x01010101 ) >> 24 );
}

BOOST_CXX14_CONSTEXPR inline int popcount_impl( boost::uint64_t x ) BOOST_NOEXCEPT
{
    x = x - ( ( x >> 1 ) & 0x5555555555555555 );
    x = ( x & 0x3333333333333333 ) + ( ( x >> 2 ) & 0x3333333333333333 );
    x = ( x + ( x >> 4 ) ) & 0x0F0F0F0F0F0F0F0F;

    return static_cast<unsigned>( ( x * 0x0101010101010101 ) >> 56 );
}

} // namespace detail

template<class T>
BOOST_CXX14_CONSTEXPR int popcount( T x ) BOOST_NOEXCEPT
{
    BOOST_STATIC_ASSERT( std::numeric_limits<T>::is_integer && !std::numeric_limits<T>::is_signed );

    BOOST_STATIC_ASSERT( sizeof(T) <= sizeof(boost::uint64_t) );

    BOOST_IF_CONSTEXPR ( sizeof(T) <= sizeof(boost::uint32_t) )
    {
        return boost::core::detail::popcount_impl( static_cast<boost::uint32_t>( x ) );
    }
    else
    {
        return boost::core::detail::popcount_impl( static_cast<boost::uint64_t>( x ) );
    }
}

#endif // defined(__GNUC__) || defined(__clang__)

// rotating

template<class T>
BOOST_CXX14_CONSTEXPR T rotl( T x, int s ) BOOST_NOEXCEPT
{
    BOOST_STATIC_ASSERT( std::numeric_limits<T>::is_integer && !std::numeric_limits<T>::is_signed );

    unsigned const mask = std::numeric_limits<T>::digits - 1;
    return static_cast<T>( x << (static_cast<unsigned>( s ) & mask) | x >> (static_cast<unsigned>( -s ) & mask) );
}

template<class T>
BOOST_CXX14_CONSTEXPR T rotr( T x, int s ) BOOST_NOEXCEPT
{
    BOOST_STATIC_ASSERT( std::numeric_limits<T>::is_integer && !std::numeric_limits<T>::is_signed );

    unsigned const mask = std::numeric_limits<T>::digits - 1;
    return static_cast<T>( x >> (static_cast<unsigned>( s ) & mask) | x << (static_cast<unsigned>( -s ) & mask) );
}

// integral powers of 2

template<class T>
BOOST_CONSTEXPR bool has_single_bit( T x ) BOOST_NOEXCEPT
{
    BOOST_STATIC_ASSERT( std::numeric_limits<T>::is_integer && !std::numeric_limits<T>::is_signed );

    return x != 0 && ( x & ( x - 1 ) ) == 0;
}

// bit_width returns `int` now, https://cplusplus.github.io/LWG/issue3656
// has been applied to C++20 as a DR

template<class T>
BOOST_CONSTEXPR int bit_width( T x ) BOOST_NOEXCEPT
{
    BOOST_STATIC_ASSERT( std::numeric_limits<T>::is_integer && !std::numeric_limits<T>::is_signed );

    return std::numeric_limits<T>::digits - boost::core::countl_zero( x );
}

template<class T>
BOOST_CONSTEXPR T bit_floor( T x ) BOOST_NOEXCEPT
{
    BOOST_STATIC_ASSERT( std::numeric_limits<T>::is_integer && !std::numeric_limits<T>::is_signed );

    return x == 0? T(0): static_cast<T>( T(1) << ( boost::core::bit_width( x ) - 1 ) );
}

namespace detail
{

BOOST_CXX14_CONSTEXPR inline boost::uint32_t bit_ceil_impl( boost::uint32_t x ) BOOST_NOEXCEPT
{
    if( x == 0 )
    {
        return 0;
    }

    --x;

    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;

    ++x;

    return x;
}

BOOST_CXX14_CONSTEXPR inline boost::uint64_t bit_ceil_impl( boost::uint64_t x ) BOOST_NOEXCEPT
{
    if( x == 0 )
    {
        return 0;
    }

    --x;

    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x |= x >> 32;

    ++x;

    return x;
}

} // namespace detail

template<class T>
BOOST_CXX14_CONSTEXPR T bit_ceil( T x ) BOOST_NOEXCEPT
{
    BOOST_STATIC_ASSERT( std::numeric_limits<T>::is_integer && !std::numeric_limits<T>::is_signed );

    BOOST_STATIC_ASSERT( sizeof(T) <= sizeof(boost::uint64_t) );

    BOOST_IF_CONSTEXPR ( sizeof(T) <= sizeof(boost::uint32_t) )
    {
        return static_cast<T>( boost::core::detail::bit_ceil_impl( static_cast<boost::uint32_t>( x ) ) );
    }
    else
    {
        return static_cast<T>( boost::core::detail::bit_ceil_impl( static_cast<boost::uint64_t>( x ) ) );
    }
}

// endian

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

# define BOOST_CORE_BIT_NATIVE_INITIALIZER =little

#elif defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__

# define BOOST_CORE_BIT_NATIVE_INITIALIZER =big

#elif defined(__BYTE_ORDER__) && defined(__ORDER_PDP_ENDIAN__) && __BYTE_ORDER__ == __ORDER_PDP_ENDIAN__

# define BOOST_CORE_BIT_NATIVE_INITIALIZER

#elif defined(__LITTLE_ENDIAN__)

# define BOOST_CORE_BIT_NATIVE_INITIALIZER =little

#elif defined(__BIG_ENDIAN__)

# define BOOST_CORE_BIT_NATIVE_INITIALIZER =big

#elif defined(_MSC_VER) || defined(__i386__) || defined(__x86_64__)

# define BOOST_CORE_BIT_NATIVE_INITIALIZER =little

#else

# define BOOST_CORE_BIT_NATIVE_INITIALIZER

#endif

#if !defined(BOOST_NO_CXX11_SCOPED_ENUMS)

enum class endian
{
    big,
    little,
    native BOOST_CORE_BIT_NATIVE_INITIALIZER
};

typedef endian endian_type;

#else

namespace endian
{

enum type
{
    big,
    little,
    native BOOST_CORE_BIT_NATIVE_INITIALIZER
};

} // namespace endian

typedef endian::type endian_type;

#endif

#undef BOOST_CORE_BIT_NATIVE_INITIALIZER

// byteswap

namespace detail
{

BOOST_CONSTEXPR inline boost::uint8_t byteswap_impl( boost::uint8_t x ) BOOST_NOEXCEPT
{
    return x;
}

#if defined(BOOST_CORE_HAS_BUILTIN_BSWAP16)

BOOST_CONSTEXPR inline boost::uint16_t byteswap_impl( boost::uint16_t x ) BOOST_NOEXCEPT
{
    return __builtin_bswap16( x );
}

#else

BOOST_CONSTEXPR inline boost::uint16_t byteswap_impl( boost::uint16_t x ) BOOST_NOEXCEPT
{
    return static_cast<boost::uint16_t>( x << 8 | x >> 8 );
}

#endif

#if defined(__GNUC__) || defined(__clang__)

BOOST_CXX14_CONSTEXPR inline boost::uint32_t byteswap_impl( boost::uint32_t x ) BOOST_NOEXCEPT
{
    return __builtin_bswap32( x );
}

BOOST_CXX14_CONSTEXPR inline boost::uint64_t byteswap_impl( boost::uint64_t x ) BOOST_NOEXCEPT
{
    return __builtin_bswap64( x );
}

#elif defined(_MSC_VER) && defined(BOOST_CORE_HAS_BUILTIN_ISCONSTEVAL)

BOOST_CXX14_CONSTEXPR inline boost::uint32_t byteswap_impl( boost::uint32_t x ) BOOST_NOEXCEPT
{
    if( __builtin_is_constant_evaluated() )
    {
        boost::uint32_t step16 = x << 16 | x >> 16;
        return ((step16 << 8) & 0xff00ff00) | ((step16 >> 8) & 0x00ff00ff);
    }
    else
    {
        return _byteswap_ulong( x );
    }
}

BOOST_CXX14_CONSTEXPR inline boost::uint64_t byteswap_impl( boost::uint64_t x ) BOOST_NOEXCEPT
{
    if( __builtin_is_constant_evaluated() )
    {
        boost::uint64_t step32 = x << 32 | x >> 32;
        boost::uint64_t step16 = (step32 & 0x0000FFFF0000FFFFULL) << 16 | (step32 & 0xFFFF0000FFFF0000ULL) >> 16;
        return (step16 & 0x00FF00FF00FF00FFULL) << 8 | (step16 & 0xFF00FF00FF00FF00ULL) >> 8;
    }
    else
    {
        return _byteswap_uint64( x );
    }
}

#elif defined(_MSC_VER)

inline boost::uint32_t byteswap_impl( boost::uint32_t x ) BOOST_NOEXCEPT
{
    return _byteswap_ulong( x );
}

inline boost::uint64_t byteswap_impl( boost::uint64_t x ) BOOST_NOEXCEPT
{
    return _byteswap_uint64( x );
}

#else

BOOST_CXX14_CONSTEXPR inline boost::uint32_t byteswap_impl( boost::uint32_t x ) BOOST_NOEXCEPT
{
    boost::uint32_t step16 = x << 16 | x >> 16;
    return ((step16 << 8) & 0xff00ff00) | ((step16 >> 8) & 0x00ff00ff);
}

BOOST_CXX14_CONSTEXPR inline boost::uint64_t byteswap_impl( boost::uint64_t x ) BOOST_NOEXCEPT
{
    boost::uint64_t step32 = x << 32 | x >> 32;
    boost::uint64_t step16 = (step32 & 0x0000FFFF0000FFFFULL) << 16 | (step32 & 0xFFFF0000FFFF0000ULL) >> 16;
    return (step16 & 0x00FF00FF00FF00FFULL) << 8 | (step16 & 0xFF00FF00FF00FF00ULL) >> 8;
}

#endif

} // namespace detail

template<class T> BOOST_CXX14_CONSTEXPR T byteswap( T x ) BOOST_NOEXCEPT
{
    BOOST_STATIC_ASSERT( std::numeric_limits<T>::is_integer );

    BOOST_STATIC_ASSERT( sizeof(T) == sizeof(boost::uint8_t) || sizeof(T) == sizeof(boost::uint16_t) || sizeof(T) == sizeof(boost::uint32_t) || sizeof(T) == sizeof(boost::uint64_t) );

    BOOST_IF_CONSTEXPR ( sizeof(T) == sizeof(boost::uint8_t) )
    {
        return static_cast<T>( boost::core::detail::byteswap_impl( static_cast<boost::uint8_t>( x ) ) );
    }
    else BOOST_IF_CONSTEXPR ( sizeof(T) == sizeof(boost::uint16_t) )
    {
        return static_cast<T>( boost::core::detail::byteswap_impl( static_cast<boost::uint16_t>( x ) ) );
    }
    else BOOST_IF_CONSTEXPR ( sizeof(T) == sizeof(boost::uint32_t) )
    {
        return static_cast<T>( boost::core::detail::byteswap_impl( static_cast<boost::uint32_t>( x ) ) );
    }
    else
    {
        return static_cast<T>( boost::core::detail::byteswap_impl( static_cast<boost::uint64_t>( x ) ) );
    }
}

} // namespace core
} // namespace boost

#if defined(_MSC_VER)
# pragma warning(pop)
#endif

#endif  // #ifndef BOOST_CORE_BIT_HPP_INCLUDED
