// Copyright 2022 Peter Dimov.
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#define _SILENCE_CXX17_OLD_ALLOCATOR_MEMBERS_DEPRECATION_WARNING
#define _SILENCE_CXX20_CISO646_REMOVED_WARNING

#include <boost/unordered/unordered_flat_map.hpp>
#ifdef HAVE_ABSEIL
# include "absl/hash/hash.h"
#endif
#ifdef HAVE_ANKERL_UNORDERED_DENSE
# include "ankerl/unordered_dense.h"
#endif
#ifdef HAVE_MULXP_HASH
# include "mulxp_hash.hpp"
#endif
#include <boost/regex.hpp>
#include <vector>
#include <memory>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <fstream>
#include <string_view>
#include <string>

using namespace std::chrono_literals;

static void print_time( std::chrono::steady_clock::time_point & t1, char const* label, std::uint32_t s, std::size_t size )
{
    auto t2 = std::chrono::steady_clock::now();

    std::cout << label << ": " << ( t2 - t1 ) / 1ms << " ms (s=" << s << ", size=" << size << ")\n";

    t1 = t2;
}

static std::vector<std::string> words;

static void init_words()
{
#if SIZE_MAX > UINT32_MAX

    char const* fn = "enwik9"; // http://mattmahoney.net/dc/textdata

#else

    char const* fn = "enwik8"; // ditto

#endif

    auto t1 = std::chrono::steady_clock::now();

    std::ifstream is( fn );
    std::string in( std::istreambuf_iterator<char>( is ), std::istreambuf_iterator<char>{} );

    boost::regex re( "[a-zA-Z]+");
    boost::sregex_token_iterator it( in.begin(), in.end(), re, 0 ), end;

    words.assign( it, end );

    auto t2 = std::chrono::steady_clock::now();

    std::cout << fn << ": " << words.size() << " words, " << ( t2 - t1 ) / 1ms << " ms\n\n";
}

template<class Map> BOOST_NOINLINE void test_word_count( Map& map, std::chrono::steady_clock::time_point & t1 )
{
    std::size_t s = 0;

    for( auto const& word: words )
    {
        ++map[ word ];
        ++s;
    }

    print_time( t1, "Word count", s, map.size() );

    std::cout << std::endl;
}

template<class Map> BOOST_NOINLINE void test_contains( Map& map, std::chrono::steady_clock::time_point & t1 )
{
    std::size_t s = 0;

    for( auto const& word: words )
    {
        std::string_view w2( word );
        w2.remove_prefix( 1 );

        s += map.contains( w2 );
    }

    print_time( t1, "Contains", s, map.size() );

    std::cout << std::endl;
}

template<class Map> BOOST_NOINLINE void test_count( Map& map, std::chrono::steady_clock::time_point & t1 )
{
    std::size_t s = 0;

    for( auto const& word: words )
    {
        std::string_view w2( word );
        w2.remove_prefix( 1 );

        s += map.count( w2 );
    }

    print_time( t1, "Count", s, map.size() );

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

    boost::unordered_flat_map<std::string_view, std::size_t, Hash> map;

    auto t0 = std::chrono::steady_clock::now();
    auto t1 = t0;

    test_word_count( map, t1 );

    record rec = { label, 0 };

    test_contains( map, t1 );
    test_count( map, t1 );

    auto tN = std::chrono::steady_clock::now();
    std::cout << "Total: " << ( tN - t0 ) / 1ms << " ms\n\n";

    rec.time_ = ( tN - t0 ) / 1ms;
    times.push_back( rec );
}

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
    using is_avalanching = void;
};

// std_hash

struct std_hash: std::hash<std::string_view>
{
    using is_avalanching = void;
};

// absl_hash

#ifdef HAVE_ABSEIL

struct absl_hash: absl::Hash<std::string_view>
{
    using is_avalanching = void;
};

#endif

#ifdef HAVE_MULXP_HASH

struct mulxp1_hash_
{
    using is_avalanching = void;

    std::size_t operator()( std::string_view const& st ) const BOOST_NOEXCEPT
    {
        return mulxp1_hash( (unsigned char const*)st.data(), st.size(), 0 );
    }
};

struct mulxp3_hash_
{
    using is_avalanching = void;

    std::size_t operator()( std::string_view const& st ) const BOOST_NOEXCEPT
    {
        return mulxp3_hash( (unsigned char const*)st.data(), st.size(), 0 );
    }
};

struct mulxp3_hash32_
{
    using is_avalanching = void;

    std::size_t operator()( std::string_view const& st ) const BOOST_NOEXCEPT
    {
        std::size_t r = mulxp3_hash32( (unsigned char const*)st.data(), st.size(), 0 );

#if SIZE_MAX > UINT32_MAX

        r |= r << 32;

#endif

        return r;
    }
};

struct mulxp1_hash32_
{
    using is_avalanching = void;

    std::size_t operator()( std::string_view const& st ) const BOOST_NOEXCEPT
    {
        std::size_t r = mulxp1_hash32( (unsigned char const*)st.data(), st.size(), 0 );

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
    init_words();

    test< boost::hash<std::string_view> >( "boost::hash" );
    test< std_hash >( "std::hash" );
    test< fnv1a_hash >( "fnv1a_hash" );

#ifdef HAVE_ABSEIL

    test< absl_hash >( "absl::Hash" );

#endif

#ifdef HAVE_ANKERL_UNORDERED_DENSE

    test< ankerl::unordered_dense::hash<std::string_view> >( "ankerl::unordered_dense::hash" );

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
