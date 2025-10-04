#ifndef BOOST_UUID_DETAIL_CHACHA20_HPP_INCLUDED
#define BOOST_UUID_DETAIL_CHACHA20_HPP_INCLUDED

// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <limits>
#include <cstdint>
#include <cstddef>

namespace boost {
namespace uuids {
namespace detail {

class chacha20_12
{
private:

    std::uint32_t state_[ 16 ];
    std::uint32_t block_[ 16 ];
    std::size_t index_;

private:

    static inline std::uint32_t rotl( std::uint32_t x, int n ) noexcept
    {
        return ( x << n ) | ( x >> (32 - n) );
    }

    static inline void quarter_round( std::uint32_t (&x)[ 16 ], int a, int b, int c, int d ) noexcept
    {
        x[ a ] += x[ b ]; x[ d ] = rotl( x[d] ^ x[a], 16 );
        x[ c ] += x[ d ]; x[ b ] = rotl( x[b] ^ x[c], 12 );
        x[ a ] += x[ b ]; x[ d ] = rotl( x[d] ^ x[a],  8 );
        x[ c ] += x[ d ]; x[ b ] = rotl( x[b] ^ x[c],  7 );
    }

    void get_next_block() noexcept
    {
        for( int i = 0; i < 16; ++i )
        {
            block_[ i ] = state_[ i ];
        }

        for( int i = 0; i < 6; ++i )
        {
            quarter_round( block_, 0, 4,  8, 12 );
            quarter_round( block_, 1, 5,  9, 13 );
            quarter_round( block_, 2, 6, 10, 14 );
            quarter_round( block_, 3, 7, 11, 15 );
            quarter_round( block_, 0, 5, 10, 15 );
            quarter_round( block_, 1, 6, 11, 12 );
            quarter_round( block_, 2, 7,  8, 13 );
            quarter_round( block_, 3, 4,  9, 14 );
        }

        for( int i = 0; i < 16; ++i )
        {
            block_[ i ] += state_[ i ];
        }

        if( ++state_[ 12 ] == 0 ) ++state_[ 13 ];
    }

public:

    using result_type = std::uint32_t;

    chacha20_12() noexcept: index_( 16 )
    {
        state_[ 0 ] = 0x61707865;
        state_[ 1 ] = 0x3320646e;
        state_[ 2 ] = 0x79622d32;
        state_[ 3 ] = 0x6b206574;

        for( int i = 4; i < 16; ++i )
        {
            state_[ i ] = 0;
        }
    }

    chacha20_12( std::uint32_t const (&key)[ 8 ], std::uint32_t const (&nonce)[ 2 ] ) noexcept: index_( 16 )
    {
        state_[ 0 ] = 0x61707865;
        state_[ 1 ] = 0x3320646e;
        state_[ 2 ] = 0x79622d32;
        state_[ 3 ] = 0x6b206574;

        for( int i = 0; i < 8; ++i )
        {
            state_[ i + 4 ] = key[ i ];
        }

        state_[ 12 ] = 0;
        state_[ 13 ] = 0;

        state_[ 14 ] = nonce[ 0 ];
        state_[ 15 ] = nonce[ 1 ];
    }

    // only needed because basic_random_generator looks for it
    void seed() noexcept
    {
        index_ = 16;

        for( int i = 4; i < 16; ++i )
        {
            state_[ i ] = 0;
        }
    }

    template<class Seq> void seed( Seq& seq )
    {
        index_ = 16;

        seq.generate( state_ + 4, state_ + 16 );

        // reset counter
        state_[ 12 ] = 0;
        state_[ 13 ] = 0;
    }

    // perturbs the generator state so that it no longer generates
    // the same sequence; useful for e.g. moved from objects
    void perturb() noexcept
    {
        index_ = 16;

        for( int i = 12; i < 16; ++i )
        {
            ++state_[ i ];
        }
    }

    static constexpr result_type min() noexcept
    {
        return std::numeric_limits<result_type>::min();
    }

    static constexpr result_type max() noexcept
    {
        return std::numeric_limits<result_type>::max();
    }

    result_type operator()() noexcept
    {
        if( index_ == 16 )
        {
            get_next_block();
            index_ = 0;
        }

        return block_[ index_++ ];
    }
};

}}} // namespace boost::uuids::detail

#endif // #ifndef BOOST_UUID_DETAIL_SHA1_HPP_INCLUDED
