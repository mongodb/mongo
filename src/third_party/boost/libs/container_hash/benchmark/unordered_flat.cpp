// Copyright 2021 Peter Dimov.
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#define _SILENCE_CXX17_OLD_ALLOCATOR_MEMBERS_DEPRECATION_WARNING
#define _SILENCE_CXX20_CISO646_REMOVED_WARNING

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/core/detail/splitmix64.hpp>
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
#include <vector>
#include <memory>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <chrono>

using namespace std::chrono_literals;

static void print_time( std::chrono::steady_clock::time_point & t1, char const* label, std::uint32_t s, std::size_t size )
{
    auto t2 = std::chrono::steady_clock::now();

    std::cout << label << ": " << ( t2 - t1 ) / 1ms << " ms (s=" << s << ", size=" << size << ")\n";

    t1 = t2;
}

constexpr unsigned N = 2'000'000;
constexpr int K = 10;

static std::vector<std::string> indices1, indices2;

static std::string make_index( unsigned x )
{
    char buffer[ 64 ];
    std::snprintf( buffer, sizeof(buffer), "pfx_%u_sfx", x );

    return buffer;
}

static std::string make_random_index( unsigned x )
{
    char buffer[ 64 ];
    std::snprintf( buffer, sizeof(buffer), "pfx_%0*d_%u_sfx", x % 8 + 1, 0, x );

    return buffer;
}

static void init_indices()
{
    indices1.reserve( N*2+1 );
    indices1.push_back( make_index( 0 ) );

    for( unsigned i = 1; i <= N*2; ++i )
    {
        indices1.push_back( make_index( i ) );
    }

    indices2.reserve( N*2+1 );
    indices2.push_back( make_index( 0 ) );

    {
        boost::detail::splitmix64 rng;

        for( unsigned i = 1; i <= N*2; ++i )
        {
            indices2.push_back( make_random_index( static_cast<std::uint32_t>( rng() ) ) );
        }
    }
}

template<class Map> BOOST_NOINLINE void test_insert( Map& map, std::chrono::steady_clock::time_point & t1 )
{
    for( unsigned i = 1; i <= N; ++i )
    {
        map.insert( { indices1[ i ], i } );
    }

    print_time( t1, "Consecutive insert",  0, map.size() );

    for( unsigned i = 1; i <= N; ++i )
    {
        map.insert( { indices2[ i ], i } );
    }

    print_time( t1, "Random insert",  0, map.size() );

    std::cout << std::endl;
}

template<class Map> BOOST_NOINLINE void test_lookup( Map& map, std::chrono::steady_clock::time_point & t1 )
{
    std::uint32_t s;
    
    s = 0;

    for( int j = 0; j < K; ++j )
    {
        for( unsigned i = 1; i <= N * 2; ++i )
        {
            auto it = map.find( indices1[ i ] );
            if( it != map.end() ) s += it->second;
        }
    }

    print_time( t1, "Consecutive lookup",  s, map.size() );

    s = 0;

    for( int j = 0; j < K; ++j )
    {
        for( unsigned i = 1; i <= N * 2; ++i )
        {
            auto it = map.find( indices2[ i ] );
            if( it != map.end() ) s += it->second;
        }
    }

    print_time( t1, "Random lookup",  s, map.size() );

    std::cout << std::endl;
}

template<class Map> BOOST_NOINLINE void test_iteration( Map& map, std::chrono::steady_clock::time_point & t1 )
{
    auto it = map.begin();

    while( it != map.end() )
    {
        if( it->second & 1 )
        {
            if constexpr( std::is_void_v< decltype( map.erase( it ) ) > )
            {
                map.erase( it++ );
            }
            else
            {
                it = map.erase( it );
            }
        }
        else
        {
            ++it;
        }
    }

    print_time( t1, "Iterate and erase odd elements",  0, map.size() );

    std::cout << std::endl;
}

template<class Map> BOOST_NOINLINE void test_erase( Map& map, std::chrono::steady_clock::time_point & t1 )
{
    for( unsigned i = 1; i <= N; ++i )
    {
        map.erase( indices1[ i ] );
    }

    print_time( t1, "Consecutive erase",  0, map.size() );

    for( unsigned i = 1; i <= N; ++i )
    {
        map.erase( indices2[ i ] );
    }

    print_time( t1, "Random erase",  0, map.size() );

    std::cout << std::endl;
}

//

struct record
{
    std::string label_;
    long long time_;
};

static std::vector<record> times;

template<class Hash> BOOST_NOINLINE void test( char const* label )
{
    std::cout << label << ":\n\n";

    boost::unordered_flat_map<std::string, std::uint32_t, Hash> map;

    auto t0 = std::chrono::steady_clock::now();
    auto t1 = t0;

    test_insert( map, t1 );

    record rec = { label, 0 };

    test_lookup( map, t1 );
    test_iteration( map, t1 );
    test_lookup( map, t1 );
    test_erase( map, t1 );

    auto tN = std::chrono::steady_clock::now();
    std::cout << "Total: " << ( tN - t0 ) / 1ms << " ms\n\n";

    rec.time_ = ( tN - t0 ) / 1ms;
    times.push_back( rec );
}

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

struct fnv1a_hash: fnv1a_hash_impl< std::numeric_limits<std::size_t>::digits >
{
    using is_avalanching = void;
};

// std_hash

struct std_hash: std::hash<std::string>
{
    using is_avalanching = void;
};

// absl_hash

#ifdef HAVE_ABSEIL

struct absl_hash: absl::Hash<std::string>
{
    using is_avalanching = void;
};

#endif

// mulxp_hash

#ifdef HAVE_MULXP_HASH

struct mulxp1_hash_
{
    using is_avalanching = void;

    std::size_t operator()( std::string const& st ) const BOOST_NOEXCEPT
    {
        return mulxp1_hash( (unsigned char const*)st.data(), st.size(), 0 );
    }
};

struct mulxp3_hash_
{
    using is_avalanching = void;

    std::size_t operator()( std::string const& st ) const BOOST_NOEXCEPT
    {
        return mulxp3_hash( (unsigned char const*)st.data(), st.size(), 0 );
    }
};

struct mulxp1_hash32_
{
    using is_avalanching = void;

    std::size_t operator()( std::string const& st ) const BOOST_NOEXCEPT
    {
        std::size_t r = mulxp1_hash32( (unsigned char const*)st.data(), st.size(), 0 );

#if SIZE_MAX > UINT32_MAX

        r |= r << 32;

#endif

        return r;
    }
};

struct mulxp3_hash32_
{
    using is_avalanching = void;

    std::size_t operator()( std::string const& st ) const BOOST_NOEXCEPT
    {
        std::size_t r = mulxp3_hash32( (unsigned char const*)st.data(), st.size(), 0 );

#if SIZE_MAX > UINT32_MAX

        r |= r << 32;

#endif

        return r;
    }
};

#endif

//

int main()
{
    init_indices();

    test< boost::hash<std::string> >( "boost::hash" );
    test< std_hash >( "std::hash" );
    test< fnv1a_hash >( "fnv1a_hash" );

#ifdef HAVE_ABSEIL

    test< absl_hash >( "absl::Hash" );

#endif

#ifdef HAVE_ANKERL_UNORDERED_DENSE

    test< ankerl::unordered_dense::hash<std::string> >( "ankerl::unordered_dense::hash" );

#endif

#ifdef HAVE_MULXP_HASH

    test< mulxp1_hash_ >( "mulxp1_hash" );
    test< mulxp3_hash_ >( "mulxp3_hash" );
    test< mulxp1_hash32_ >( "mulxp1_hash32" );
    test< mulxp3_hash32_ >( "mulxp3_hash32" );

#endif

    std::cout << "---\n\n";

    for( auto const& x: times )
    {
        std::cout << std::setw( 32 ) << ( x.label_ + ": " ) << std::setw( 5 ) << x.time_ << " ms\n";
    }
}

#ifdef HAVE_ABSEIL
# include "absl/hash/internal/hash.cc"
# include "absl/hash/internal/low_level_hash.cc"
# include "absl/hash/internal/city.cc"
#endif
