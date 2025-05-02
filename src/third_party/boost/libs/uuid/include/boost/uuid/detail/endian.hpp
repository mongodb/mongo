#ifndef BOOST_UUID_DETAIL_ENDIAN_INCLUDED
#define BOOST_UUID_DETAIL_ENDIAN_INCLUDED

// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <cstring>
#include <cstdint>

#if defined(_MSC_VER) && !defined(__clang__)
# include <intrin.h>
#endif

namespace boost {
namespace uuids {
namespace detail {

// Byte order macros

#if defined(__BYTE_ORDER__)

#define BOOST_UUID_BYTE_ORDER __BYTE_ORDER__
#define BOOST_UUID_ORDER_LITTLE_ENDIAN __ORDER_LITTLE_ENDIAN__
#define BOOST_UUID_ORDER_BIG_ENDIAN __ORDER_BIG_ENDIAN__

#elif defined(__LITTLE_ENDIAN__) || defined(_MSC_VER) || defined(__i386__) || defined(__x86_64__)

#define BOOST_UUID_BYTE_ORDER 1234
#define BOOST_UUID_ORDER_LITTLE_ENDIAN 1234
#define BOOST_UUID_ORDER_BIG_ENDIAN 4321

#elif defined(__BIG_ENDIAN__)

#define BOOST_UUID_BYTE_ORDER 4321
#define BOOST_UUID_ORDER_LITTLE_ENDIAN 1234
#define BOOST_UUID_ORDER_BIG_ENDIAN 4321

#else

# error Unrecognized platform

#endif

// byteswap

#if defined(__GNUC__) || defined(__clang__)

inline std::uint16_t byteswap( std::uint16_t x ) noexcept
{
    return __builtin_bswap16( x );
}

inline std::uint32_t byteswap( std::uint32_t x ) noexcept
{
    return __builtin_bswap32( x );
}

inline std::uint64_t byteswap( std::uint64_t x ) noexcept
{
    return __builtin_bswap64( x );
}

#elif defined(_MSC_VER)

inline std::uint16_t byteswap( std::uint16_t x ) noexcept
{
    return _byteswap_ushort( x );
}

inline std::uint32_t byteswap( std::uint32_t x ) noexcept
{
    return _byteswap_ulong( x );
}

inline std::uint64_t byteswap( std::uint64_t x ) noexcept
{
    return _byteswap_uint64( x );
}

#else

inline std::uint16_t byteswap( std::uint16_t x ) noexcept
{
    return static_cast<std::uint16_t>( x << 8 | x >> 8 );
}

inline std::uint32_t byteswap( std::uint32_t x ) noexcept
{
    std::uint32_t step16 = x << 16 | x >> 16;
    return ((step16 << 8) & 0xff00ff00) | ((step16 >> 8) & 0x00ff00ff);
}

inline std::uint64_t byteswap( std::uint64_t x ) noexcept
{
    std::uint64_t step32 = x << 32 | x >> 32;
    std::uint64_t step16 = (step32 & 0x0000FFFF0000FFFFULL) << 16 | (step32 & 0xFFFF0000FFFF0000ULL) >> 16;
    return (step16 & 0x00FF00FF00FF00FFULL) << 8 | (step16 & 0xFF00FF00FF00FF00ULL) >> 8;
}

#endif

#if defined(__SIZEOF_INT128__)

inline __uint128_t byteswap( __uint128_t x ) noexcept
{
    return ( static_cast<__uint128_t>( detail::byteswap( static_cast<std::uint64_t>( x ) ) ) << 64 ) | detail::byteswap( static_cast<std::uint64_t>( x >> 64 ) );
}

#endif

// load_*_u16

inline std::uint16_t load_native_u16( void const* p ) noexcept
{
    std::uint16_t tmp;
    std::memcpy( &tmp, p, sizeof( tmp ) );
    return tmp;
}

inline std::uint16_t load_little_u16( void const* p ) noexcept
{
    std::uint16_t tmp;
    std::memcpy( &tmp, p, sizeof( tmp ) );

#if BOOST_UUID_BYTE_ORDER == BOOST_UUID_ORDER_LITTLE_ENDIAN

    return tmp;

#else

    return detail::byteswap( tmp );

#endif
}

inline std::uint16_t load_big_u16( void const* p ) noexcept
{
    std::uint16_t tmp;
    std::memcpy( &tmp, p, sizeof( tmp ) );

#if BOOST_UUID_BYTE_ORDER == BOOST_UUID_ORDER_BIG_ENDIAN

    return tmp;

#else

    return detail::byteswap( tmp );

#endif
}

// load_*_u32

inline std::uint32_t load_native_u32( void const* p ) noexcept
{
    std::uint32_t tmp;
    std::memcpy( &tmp, p, sizeof( tmp ) );
    return tmp;
}

inline std::uint32_t load_little_u32( void const* p ) noexcept
{
    std::uint32_t tmp;
    std::memcpy( &tmp, p, sizeof( tmp ) );

#if BOOST_UUID_BYTE_ORDER == BOOST_UUID_ORDER_LITTLE_ENDIAN

    return tmp;

#else

    return detail::byteswap( tmp );

#endif
}

inline std::uint32_t load_big_u32( void const* p ) noexcept
{
    std::uint32_t tmp;
    std::memcpy( &tmp, p, sizeof( tmp ) );

#if BOOST_UUID_BYTE_ORDER == BOOST_UUID_ORDER_BIG_ENDIAN

    return tmp;

#else

    return detail::byteswap( tmp );

#endif
}

// load_*_u64

inline std::uint64_t load_native_u64( void const* p ) noexcept
{
    std::uint64_t tmp;
    std::memcpy( &tmp, p, sizeof( tmp ) );
    return tmp;
}

inline std::uint64_t load_little_u64( void const* p ) noexcept
{
    std::uint64_t tmp;
    std::memcpy( &tmp, p, sizeof( tmp ) );

#if BOOST_UUID_BYTE_ORDER == BOOST_UUID_ORDER_LITTLE_ENDIAN

    return tmp;

#else

    return detail::byteswap( tmp );

#endif
}

inline std::uint64_t load_big_u64( void const* p ) noexcept
{
    std::uint64_t tmp;
    std::memcpy( &tmp, p, sizeof( tmp ) );

#if BOOST_UUID_BYTE_ORDER == BOOST_UUID_ORDER_BIG_ENDIAN

    return tmp;

#else

    return detail::byteswap( tmp );

#endif
}

// load_*_u128

#if defined(__SIZEOF_INT128__)

inline __uint128_t load_native_u128( void const* p ) noexcept
{
    __uint128_t tmp;
    std::memcpy( &tmp, p, sizeof( tmp ) );
    return tmp;
}

inline __uint128_t load_little_u128( void const* p ) noexcept
{
    __uint128_t tmp;
    std::memcpy( &tmp, p, sizeof( tmp ) );

#if BOOST_UUID_BYTE_ORDER == BOOST_UUID_ORDER_LITTLE_ENDIAN

    return tmp;

#else

    return detail::byteswap( tmp );

#endif
}

inline __uint128_t load_big_u128( void const* p ) noexcept
{
    __uint128_t tmp;
    std::memcpy( &tmp, p, sizeof( tmp ) );

#if BOOST_UUID_BYTE_ORDER == BOOST_UUID_ORDER_BIG_ENDIAN

    return tmp;

#else

    return detail::byteswap( tmp );

#endif
}

#endif

// store_*_u16

inline void store_native_u16( void* p, std::uint16_t v ) noexcept
{
    std::memcpy( p, &v, sizeof( v ) );
}

inline void store_little_u16( void* p, std::uint16_t v ) noexcept
{
#if BOOST_UUID_BYTE_ORDER != BOOST_UUID_ORDER_LITTLE_ENDIAN

    v = detail::byteswap( v );

#endif

    std::memcpy( p, &v, sizeof( v ) );
}

inline void store_big_u16( void* p, std::uint16_t v ) noexcept
{
#if BOOST_UUID_BYTE_ORDER != BOOST_UUID_ORDER_BIG_ENDIAN

    v = detail::byteswap( v );

#endif

    std::memcpy( p, &v, sizeof( v ) );
}

// store_*_u32

inline void store_native_u32( void* p, std::uint32_t v ) noexcept
{
    std::memcpy( p, &v, sizeof( v ) );
}

inline void store_little_u32( void* p, std::uint32_t v ) noexcept
{
#if BOOST_UUID_BYTE_ORDER != BOOST_UUID_ORDER_LITTLE_ENDIAN

    v = detail::byteswap( v );

#endif

    std::memcpy( p, &v, sizeof( v ) );
}

inline void store_big_u32( void* p, std::uint32_t v ) noexcept
{
#if BOOST_UUID_BYTE_ORDER != BOOST_UUID_ORDER_BIG_ENDIAN

    v = detail::byteswap( v );

#endif

    std::memcpy( p, &v, sizeof( v ) );
}

// store_*_u64

inline void store_native_u64( void* p, std::uint64_t v ) noexcept
{
    std::memcpy( p, &v, sizeof( v ) );
}

inline void store_little_u64( void* p, std::uint64_t v ) noexcept
{
#if BOOST_UUID_BYTE_ORDER != BOOST_UUID_ORDER_LITTLE_ENDIAN

    v = detail::byteswap( v );

#endif

    std::memcpy( p, &v, sizeof( v ) );
}

inline void store_big_u64( void* p, std::uint64_t v ) noexcept
{
#if BOOST_UUID_BYTE_ORDER != BOOST_UUID_ORDER_BIG_ENDIAN

    v = detail::byteswap( v );

#endif

    std::memcpy( p, &v, sizeof( v ) );
}

// store_*_u128

#if defined(__SIZEOF_INT128__)

inline void store_native_u128( void* p, __uint128_t v ) noexcept
{
    std::memcpy( p, &v, sizeof( v ) );
}

inline void store_little_u128( void* p, __uint128_t v ) noexcept
{
#if BOOST_UUID_BYTE_ORDER != BOOST_UUID_ORDER_LITTLE_ENDIAN

    v = detail::byteswap( v );

#endif

    std::memcpy( p, &v, sizeof( v ) );
}

inline void store_big_u128( void* p, __uint128_t v ) noexcept
{
#if BOOST_UUID_BYTE_ORDER != BOOST_UUID_ORDER_BIG_ENDIAN

    v = detail::byteswap( v );

#endif

    std::memcpy( p, &v, sizeof( v ) );
}

#endif

} // detail
} // uuids
} // boost

#endif // #ifndef BOOST_UUID_DETAIL_ENDIAN_INCLUDED
