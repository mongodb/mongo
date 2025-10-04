// Copyright 2022 Peter Dimov.
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#define _CRT_SECURE_NO_WARNINGS
#define _SILENCE_CXX20_CISO646_REMOVED_WARNING

#include <boost/container_hash/hash.hpp>
#include <boost/unordered_set.hpp>
#include <boost/core/detail/splitmix64.hpp>
#include <boost/core/type_name.hpp>
#include <boost/config.hpp>
#ifdef HAVE_ABSEIL
# include "absl/hash/hash.h"
#endif
#ifdef HAVE_ANKERL_UNORDERED_DENSE
# include "ankerl/unordered_dense.h"
#endif
#ifdef HAVE_MULXP_HASH
# include "mulxp_hash.hpp"
#endif
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <functional>

// mul31_hash

struct mul31_hash
{
    std::size_t operator()( std::string const& st ) const BOOST_NOEXCEPT
    {
        char const * p = st.data();
        std::size_t n = st.size();

#if SIZE_MAX > UINT32_MAX
        std::size_t h = 0xCBF29CE484222325ull;
#else
        std::size_t h = 0x811C9DC5u;
#endif

        for( std::size_t i = 0; i < n; ++i )
        {
            h = h * 31 + static_cast<unsigned char>( p[i] );
        }

        return h;
    }
};

// mul31_x4_hash

struct mul31_x4_hash
{
    std::size_t operator()( std::string const& st ) const BOOST_NOEXCEPT
    {
        char const * p = st.data();
        std::size_t n = st.size();

#if SIZE_MAX > UINT32_MAX
        std::size_t h = 0xCBF29CE484222325ull;
#else
        std::size_t h = 0x811C9DC5u;
#endif

        while( n >= 4 )
        {
            h = h * (31u * 31u * 31u * 31u)
                + static_cast<unsigned char>( p[0] ) * (31u * 31u * 31u)
                + static_cast<unsigned char>( p[1] ) * (31u * 31u)
                + static_cast<unsigned char>( p[2] ) * 31u
                + static_cast<unsigned char>( p[3] );

            p += 4;
            n -= 4;
        }

        while( n > 0 )
        {
            h = h * 31u + static_cast<unsigned char>( *p );

            ++p;
            --n;
        }

        return h;
    }
};

// mul31_x8_hash

struct mul31_x8_hash
{
    std::size_t operator()( std::string const& st ) const BOOST_NOEXCEPT
    {
        char const * p = st.data();
        std::size_t n = st.size();

#if SIZE_MAX > UINT32_MAX
        boost::uint64_t h = 0xCBF29CE484222325ull;
#else
        boost::uint64_t h = 0x811C9DC5u;
#endif

        while( n >= 8 )
        {
            h = h * (31ull * 31ull * 31ull * 31ull * 31ull * 31ull * 31ull * 31ull)
                + static_cast<unsigned char>( p[0] ) * (31ull * 31ull * 31ull * 31ull * 31ull * 31ull * 31ull)
                + static_cast<unsigned char>( p[1] ) * (31ull * 31ull * 31ull * 31ull * 31ull * 31ull)
                + static_cast<unsigned char>( p[2] ) * (31ull * 31ull * 31ull * 31ull * 31ull)
                + static_cast<unsigned char>( p[3] ) * (31ull * 31ull * 31ull * 31ull)
                + static_cast<unsigned char>( p[4] ) * (31ull * 31ull * 31ull)
                + static_cast<unsigned char>( p[5] ) * (31ull * 31ull)
                + static_cast<unsigned char>( p[6] ) * 31ull
                + static_cast<unsigned char>( p[7] );

            p += 8;
            n -= 8;
        }

        while( n > 0 )
        {
            h = h * 31u + static_cast<unsigned char>( *p );

            ++p;
            --n;
        }

        return static_cast<std::size_t>( h );
    }
};

// fnv1a_hash

template<int Bits> struct fnv1a_hash_impl;

template<> struct fnv1a_hash_impl<32>
{
    std::size_t operator()( std::string const& s ) const
    {
        std::size_t h = 0x811C9DC5u;

        char const * first = s.data();
        char const * last = first + s.size();

        for( ; first != last; ++first )
        {
            h ^= static_cast<unsigned char>( *first );
            h *= 0x01000193ul;
        }

        return h;
    }
};

template<> struct fnv1a_hash_impl<64>
{
    std::size_t operator()( std::string const& s ) const
    {
        std::size_t h = 0xCBF29CE484222325ull;

        char const * first = s.data();
        char const * last = first + s.size();

        for( ; first != last; ++first )
        {
            h ^= static_cast<unsigned char>( *first );
            h *= 0x00000100000001B3ull;
        }

        return h;
    }
};

struct fnv1a_hash: fnv1a_hash_impl< std::numeric_limits<std::size_t>::digits > {};

// mulxp_hash

#ifdef HAVE_MULXP_HASH

struct mulxp1_hash_
{
    std::size_t operator()( std::string const& st ) const BOOST_NOEXCEPT
    {
        return mulxp1_hash( (unsigned char const*)st.data(), st.size(), 0 );
    }
};

struct mulxp3_hash_
{
    std::size_t operator()( std::string const& st ) const BOOST_NOEXCEPT
    {
        return mulxp3_hash( (unsigned char const*)st.data(), st.size(), 0 );
    }
};

struct mulxp1_hash32_
{
    std::size_t operator()( std::string const& st ) const BOOST_NOEXCEPT
    {
        return mulxp1_hash32( (unsigned char const*)st.data(), st.size(), 0 );
    }
};

struct mulxp3_hash32_
{
    std::size_t operator()( std::string const& st ) const BOOST_NOEXCEPT
    {
        return mulxp3_hash32( (unsigned char const*)st.data(), st.size(), 0 );
    }
};

#endif

// test_hash_speed

template<class H, class V> void test_hash_speed( int N, V const& v )
{
    typedef std::chrono::steady_clock clock_type;

    clock_type::time_point t1 = clock_type::now();

    std::size_t q = 0;

    H const h;

    for( int i = 0; i < N; ++i )
    {
        q += h( v[i] );
    }

    clock_type::time_point t2 = clock_type::now();

    long long ms1 = std::chrono::duration_cast<std::chrono::milliseconds>( t2 - t1 ).count();

    std::string hash = boost::core::type_name<H>();

#if defined( _MSC_VER )

    std::printf( "%57s : q=%20Iu, %lld ms\n", hash.c_str(), q, ms1 );

#else

    std::printf( "%57s : q=%20zu, %lld ms\n", hash.c_str(), q, ms1 );

#endif
}

// test_hash_collision

template<class H, class V> void test_hash_collision( int N, V const& v, std::size_t n )
{
    boost::unordered_set<std::size_t> s;
    H const h;

    for( int i = 0; i < N; ++i )
    {
        s.insert( h( v[i] ) );
    }

    std::string hash = boost::core::type_name<H>();

#if defined( _MSC_VER )

    std::printf( "%57s : c=%Iu\n", hash.c_str(), n - s.size() );

#else

    std::printf( "%57s : c=%zu\n", hash.c_str(), n - s.size() );

#endif
}

// test_container_speed

template<class V, class S> void test4( int N, V const& v, char const * hash, S s )
{
    typedef std::chrono::steady_clock clock_type;

    clock_type::time_point t1 = clock_type::now();

    for( int i = 0; i < N; ++i )
    {
        s.insert( v[ i * 16 ] );
    }

    clock_type::time_point t2 = clock_type::now();

    std::size_t q = 0;

    for( int i = 0; i < 16 * N; ++i )
    {
        q += s.count( v[ i ] );
    }

    clock_type::time_point t3 = clock_type::now();

    long long ms1 = std::chrono::duration_cast<std::chrono::milliseconds>( t2 - t1 ).count();
    long long ms2 = std::chrono::duration_cast<std::chrono::milliseconds>( t3 - t2 ).count();

    std::size_t n = s.bucket_count();
    std::size_t m = 0;
    std::size_t c = 0;

    for( std::size_t i = 0; i < n; ++i )
    {
        std::size_t k = s.bucket_size( i );

        if( k > 1 )
        {
            c += k - 1;
        }

        if( k > m )
        {
            m = k;
        }
    }

#if defined( _MSC_VER )

    std::printf( "%57s : n=%Iu, m=%Iu, c=%Iu, q=%Iu, %4lld + %4lld = %4lld ms\n", hash, n, m, c, q, ms1, ms2, ms1 + ms2 );

#else

    std::printf( "%57s : n=%zu, m=%zu, c=%zu, q=%zu, %4lld + %4lld = %4lld ms\n", hash, n, m, c, q, ms1, ms2, ms1 + ms2 );

#endif
}

template<class K, class H, class V> void test_container_speed( int N, V const& v )
{
    boost::unordered_set<K, H> s( 0 );
    test4( N, v, boost::core::type_name<H>().c_str(), s );
}

int main()
{
    int const N = 1048576 / 2; // 1048576 is too much for 32 bit

    std::vector<std::string> v;

    {
        v.reserve( N * 16 );

        boost::detail::splitmix64 rnd;

        for( int i = 0; i < 16 * N; ++i )
        {
            char buffer[ 64 ];

            unsigned long long k = rnd();

            if( k & 1 )
            {
                sprintf( buffer, "prefix_%llu_suffix", k );
            }
            else
            {
                sprintf( buffer, "{%u}", static_cast<unsigned>( k ) );
            }

            v.push_back( buffer );
        }
    }

    std::puts( "Hash speed test:\n" );

    test_hash_speed<mul31_hash>( N * 16, v );
    test_hash_speed<mul31_x4_hash>( N * 16, v );
    test_hash_speed<mul31_x8_hash>( N * 16, v );
    test_hash_speed<fnv1a_hash>( N * 16, v );
    test_hash_speed<boost::hash<std::string> >( N * 16, v );
    test_hash_speed<std::hash<std::string> >( N * 16, v );
#ifdef HAVE_ABSEIL
    test_hash_speed<absl::Hash<std::string> >( N * 16, v );
#endif
#ifdef HAVE_ANKERL_UNORDERED_DENSE
    test_hash_speed<ankerl::unordered_dense::hash<std::string> >( N * 16, v );
#endif
#ifdef HAVE_MULXP_HASH
    test_hash_speed<mulxp1_hash_>( N * 16, v );
    test_hash_speed<mulxp3_hash_>( N * 16, v );
    test_hash_speed<mulxp1_hash32_>( N * 16, v );
    test_hash_speed<mulxp3_hash32_>( N * 16, v );
#endif

    std::puts( "" );

    std::puts( "Hash collision test:\n" );

    {
        std::size_t n = 0;

        {
            boost::unordered_set<std::string> s;

            for( int i = 0; i < N * 16; ++i )
            {
                s.insert( v[i] );
            }

            n = s.size();
        }

        test_hash_collision<mul31_hash>( N * 16, v, n );
        test_hash_collision<mul31_x4_hash>( N * 16, v, n );
        test_hash_collision<mul31_x8_hash>( N * 16, v, n );
        test_hash_collision<fnv1a_hash>( N * 16, v, n );
        test_hash_collision<boost::hash<std::string> >( N * 16, v, n );
        test_hash_collision<std::hash<std::string> >( N * 16, v, n );
#ifdef HAVE_ABSEIL
        test_hash_collision<absl::Hash<std::string> >( N * 16, v, n );
#endif
#ifdef HAVE_ANKERL_UNORDERED_DENSE
        test_hash_collision<ankerl::unordered_dense::hash<std::string> >( N * 16, v, n );
#endif
#ifdef HAVE_MULXP_HASH
        test_hash_collision<mulxp1_hash_>( N * 16, v, n );
        test_hash_collision<mulxp3_hash_>( N * 16, v, n );
        test_hash_collision<mulxp1_hash32_>( N * 16, v, n );
        test_hash_collision<mulxp3_hash32_>( N * 16, v, n );
#endif
    }

    std::puts( "" );

    typedef std::string K;

    std::puts( "Container speed test:\n---\n" );

    test_container_speed<K, mul31_hash>( N, v );
    test_container_speed<K, mul31_x4_hash>( N, v );
    test_container_speed<K, mul31_x8_hash>( N, v );
    test_container_speed<K, fnv1a_hash>( N, v );
    test_container_speed<K, boost::hash<std::string> >( N, v );
    test_container_speed<K, std::hash<std::string> >( N, v );
#ifdef HAVE_ABSEIL
    test_container_speed<K, absl::Hash<std::string> >( N, v );
#endif
#ifdef HAVE_ANKERL_UNORDERED_DENSE
    test_container_speed<K, ankerl::unordered_dense::hash<std::string> >( N, v );
#endif
#ifdef HAVE_MULXP_HASH
    test_container_speed<K, mulxp1_hash_>( N, v );
    test_container_speed<K, mulxp3_hash_>( N, v );
    test_container_speed<K, mulxp1_hash32_>( N, v );
    test_container_speed<K, mulxp3_hash32_>( N, v );
#endif

    std::puts( "" );
}

#ifdef HAVE_ABSEIL
# include "absl/hash/internal/hash.cc"
# include "absl/hash/internal/low_level_hash.cc"
# include "absl/hash/internal/city.cc"
#endif
