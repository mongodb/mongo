// Copyright 2021 Peter Dimov.
// Copyright 2023-2024 Joaquin M Lopez Munoz.
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#define _SILENCE_CXX17_OLD_ALLOCATOR_MEMBERS_DEPRECATION_WARNING
#define _SILENCE_CXX20_CISO646_REMOVED_WARNING

#include <boost/unordered_map.hpp>
#include <boost/unordered/unordered_node_map.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/core/detail/splitmix64.hpp>
#include <boost/config.hpp>
#ifdef HAVE_ABSEIL
# include "absl/container/node_hash_map.h"
# include "absl/container/flat_hash_map.h"
#endif
#ifdef HAVE_ANKERL_UNORDERED_DENSE
# include "ankerl/unordered_dense.h"
#endif
#include <unordered_map>
#include <string_view>
#include <vector>
#include <memory>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <type_traits>

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

// counting allocator

static std::size_t s_alloc_bytes = 0;
static std::size_t s_alloc_count = 0;

template<class T> struct allocator
{
    using value_type = T;

    allocator() = default;

    template<class U> allocator( allocator<U> const & ) noexcept
    {
    }

    template<class U> bool operator==( allocator<U> const & ) const noexcept
    {
        return true;
    }

    template<class U> bool operator!=( allocator<U> const& ) const noexcept
    {
        return false;
    }

    T* allocate( std::size_t n ) const
    {
        s_alloc_bytes += n * sizeof(T);
        s_alloc_count++;

        return std::allocator<T>().allocate( n );
    }

    void deallocate( T* p, std::size_t n ) const noexcept
    {
        s_alloc_bytes -= n * sizeof(T);
        s_alloc_count--;

        std::allocator<T>().deallocate( p, n );
    }
};

//

struct record
{
    std::string label_;
    long long time_;
    std::size_t bytes_;
    std::size_t count_;
};

static std::vector<record> times;

template<template<class...> class Map> BOOST_NOINLINE void test( char const* label )
{
    std::cout << label << ":\n\n";

    s_alloc_bytes = 0;
    s_alloc_count = 0;

    Map<std::string_view, std::uint32_t> map;

    auto t0 = std::chrono::steady_clock::now();
    auto t1 = t0;

    test_insert( map, t1 );

    std::cout << "Memory: " << s_alloc_bytes << " bytes in " << s_alloc_count << " allocations\n\n";

    record rec = { label, 0, s_alloc_bytes, s_alloc_count };

    test_lookup( map, t1 );
    test_iteration( map, t1 );
    test_lookup( map, t1 );
    test_erase( map, t1 );

    auto tN = std::chrono::steady_clock::now();
    std::cout << "Total: " << ( tN - t0 ) / 1ms << " ms\n\n";

    rec.time_ = ( tN - t0 ) / 1ms;
    times.push_back( rec );
}

// aliases using the counting allocator

template<class K, class V> using allocator_for = ::allocator< std::pair<K const, V> >;

template<class K, class V> using std_unordered_map =
    std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, allocator_for<K, V>>;

template<class K, class V> using boost_unordered_map =
    boost::unordered_map<K, V, boost::hash<K>, std::equal_to<K>, allocator_for<K, V>>;

template<class K, class V> using boost_unordered_node_map =
    boost::unordered_node_map<K, V, boost::hash<K>, std::equal_to<K>, allocator_for<K, V>>;

template<class K, class V> using boost_unordered_flat_map =
    boost::unordered_flat_map<K, V, boost::hash<K>, std::equal_to<K>, allocator_for<K, V>>;

#ifdef HAVE_ABSEIL

template<class K, class V> using absl_node_hash_map =
    absl::node_hash_map<K, V, absl::container_internal::hash_default_hash<K>, absl::container_internal::hash_default_eq<K>, allocator_for<K, V>>;

template<class K, class V> using absl_flat_hash_map =
    absl::flat_hash_map<K, V, absl::container_internal::hash_default_hash<K>, absl::container_internal::hash_default_eq<K>, allocator_for<K, V>>;

#endif

#ifdef HAVE_ANKERL_UNORDERED_DENSE

template<class K, class V> using ankerl_unordered_dense_map =
    ankerl::unordered_dense::map<K, V, ankerl::unordered_dense::hash<K>, std::equal_to<K>, ::allocator< std::pair<K, V> >>;

#endif

// fnv1a_hash

template<int Bits> struct fnv1a_hash_impl;

template<> struct fnv1a_hash_impl<32>
{
    std::size_t operator()( std::string_view const& s ) const
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
    std::size_t operator()( std::string_view const& s ) const
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
    using is_avalanching = std::true_type;
};

template<class K, class V> using std_unordered_map_fnv1a =
std::unordered_map<K, V, fnv1a_hash, std::equal_to<K>, allocator_for<K, V>>;

template<class K, class V> using boost_unordered_map_fnv1a =
    boost::unordered_map<K, V, fnv1a_hash, std::equal_to<K>, allocator_for<K, V>>;

template<class K, class V> using boost_unordered_node_map_fnv1a =
    boost::unordered_node_map<K, V, fnv1a_hash, std::equal_to<K>, allocator_for<K, V>>;

template<class K, class V> using boost_unordered_flat_map_fnv1a =
    boost::unordered_flat_map<K, V, fnv1a_hash, std::equal_to<K>, allocator_for<K, V>>;

#ifdef HAVE_ABSEIL

template<class K, class V> using absl_node_hash_map_fnv1a =
    absl::node_hash_map<K, V, fnv1a_hash, absl::container_internal::hash_default_eq<K>, allocator_for<K, V>>;

template<class K, class V> using absl_flat_hash_map_fnv1a =
    absl::flat_hash_map<K, V, fnv1a_hash, absl::container_internal::hash_default_eq<K>, allocator_for<K, V>>;

#endif

#ifdef HAVE_ANKERL_UNORDERED_DENSE

template<class K, class V> using ankerl_unordered_dense_map_fnv1a =
    ankerl::unordered_dense::map<K, V, fnv1a_hash, std::equal_to<K>, ::allocator< std::pair<K, V> >>;

#endif

//

int main()
{
    init_indices();

    test<std_unordered_map>( "std::unordered_map" );
    test<boost_unordered_map>( "boost::unordered_map" );
    test<boost_unordered_node_map>( "boost::unordered_node_map" );
    test<boost_unordered_flat_map>( "boost::unordered_flat_map" );

#ifdef HAVE_ANKERL_UNORDERED_DENSE

    test<ankerl_unordered_dense_map>( "ankerl::unordered_dense::map" );

#endif

#ifdef HAVE_ABSEIL

    test<absl_node_hash_map>( "absl::node_hash_map" );
    test<absl_flat_hash_map>( "absl::flat_hash_map" );

#endif

    test<std_unordered_map_fnv1a>( "std::unordered_map, FNV-1a" );
    test<boost_unordered_map_fnv1a>( "boost::unordered_map, FNV-1a" );
    test<boost_unordered_node_map_fnv1a>( "boost::unordered_node_map, FNV-1a" );
    test<boost_unordered_flat_map_fnv1a>( "boost::unordered_flat_map, FNV-1a" );

#ifdef HAVE_ANKERL_UNORDERED_DENSE

    test<ankerl_unordered_dense_map_fnv1a>( "ankerl::unordered_dense::map, FNV-1a" );

#endif

#ifdef HAVE_ABSEIL

    test<absl_node_hash_map_fnv1a>( "absl::node_hash_map, FNV-1a" );
    test<absl_flat_hash_map_fnv1a>( "absl::flat_hash_map, FNV-1a" );

#endif

    std::cout << "---\n\n";

    for( auto const& x: times )
    {
        std::cout << std::setw( 38 ) << ( x.label_ + ": " ) << std::setw( 5 ) << x.time_ << " ms, " << std::setw( 9 ) << x.bytes_ << " bytes in " << x.count_ << " allocations\n";
    }
}

#ifdef HAVE_ABSEIL
# include "absl/container/internal/raw_hash_set.cc"
# include "absl/hash/internal/hash.cc"
# include "absl/hash/internal/low_level_hash.cc"
# include "absl/hash/internal/city.cc"
#endif
