// Copyright 2022 Peter Dimov.
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#define _CRT_SECURE_NO_WARNINGS

#include <boost/container_hash/hash.hpp>
#include <boost/core/detail/splitmix64.hpp>
#include <boost/core/type_name.hpp>
#include <boost/config.hpp>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <list>
#include <chrono>

// test_hash_speed

template<class T, class V> void test_hash_speed( int N, V const& v )
{
    std::vector<T> w;

    w.reserve( N );

    for( int i = 0; i < N; ++i )
    {
        w.emplace_back( v[i].begin(), v[i].end() );
    }

    typedef std::chrono::steady_clock clock_type;

    clock_type::time_point t1 = clock_type::now();

    std::size_t q = 0;

    boost::hash<T> const h;

    for( int i = 0; i < N; ++i )
    {
        q += h( w[i] );
    }

    clock_type::time_point t2 = clock_type::now();

    long long ms1 = std::chrono::duration_cast<std::chrono::milliseconds>( t2 - t1 ).count();

    std::string type = boost::core::type_name<T>();

#if defined( _MSC_VER )

    std::printf( "%25s : q=%20Iu, %lld ms\n", type.c_str(), q, ms1 );

#else

    std::printf( "%25s : q=%20zu, %lld ms\n", type.c_str(), q, ms1 );

#endif
}

int main()
{
    int const N = 1048576 * 8;

    std::vector<std::string> v;

    {
        v.reserve( N );

        boost::detail::splitmix64 rnd;

        for( int i = 0; i < N; ++i )
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

    std::puts( "Char sequence hashing test:\n" );

    test_hash_speed< std::string >( N, v );
    test_hash_speed< std::vector<char> >( N, v );
    test_hash_speed< std::deque<char> >( N, v );
    test_hash_speed< std::list<char> >( N, v );

    std::puts( "" );
}
