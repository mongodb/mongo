#ifndef BOOST_UUID_TIME_GENERATOR_V7_HPP_INCLUDED
#define BOOST_UUID_TIME_GENERATOR_V7_HPP_INCLUDED

// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/detail/chacha20.hpp>
#include <boost/uuid/detail/random_provider.hpp>
#include <boost/uuid/detail/endian.hpp>
#include <random>
#include <cstdint>
#include <cstring>

namespace boost {
namespace uuids {

// time_generator_v7

class time_generator_v7
{
private:

    // Bit layout from high to low:
    // 48 bits: millisecond part of Unix epoch timestamp
    // 10 bits: microsecond part of Unix epoch timestamp
    //  6 bits: conflict resolution counter

    using state_type = std::uint64_t;

    state_type state_ = {};

    detail::chacha20_12 rng_;

public:

    using result_type = uuid;

    time_generator_v7();

    time_generator_v7( time_generator_v7 const& rhs );
    time_generator_v7( time_generator_v7&& rhs ) noexcept;

    time_generator_v7& operator=( time_generator_v7 const& rhs ) noexcept;
    time_generator_v7& operator=( time_generator_v7&& rhs ) noexcept;

    result_type operator()() noexcept;

private:

    static state_type get_new_state( state_type const& oldst ) noexcept;
};

// constructors

inline time_generator_v7::time_generator_v7()
{
    detail::random_provider seeder;
    rng_.seed( seeder );
}

inline time_generator_v7::time_generator_v7( time_generator_v7 const& rhs ): state_( rhs.state_ )
{
    detail::random_provider seeder;
    rng_.seed( seeder );
}

inline time_generator_v7::time_generator_v7( time_generator_v7&& rhs ) noexcept: state_( std::move( rhs.state_ ) ), rng_( std::move( rhs.rng_ ) )
{
    rhs.rng_.perturb();
}

// assignment

inline time_generator_v7& time_generator_v7::operator=( time_generator_v7 const& rhs ) noexcept
{
    state_ = rhs.state_;
    return *this;
}

inline time_generator_v7& time_generator_v7::operator=( time_generator_v7&& rhs ) noexcept
{
    state_ = std::move( rhs.state_ );
    rng_ = std::move( rhs.rng_ );

    rhs.rng_.perturb();

    return *this;
}

// get_new_state

inline time_generator_v7::state_type time_generator_v7::get_new_state( state_type const& oldst ) noexcept
{
    // `now()` in microseconds
    std::uint64_t now_in_us = std::chrono::time_point_cast< std::chrono::microseconds >( std::chrono::system_clock::now() ).time_since_epoch().count();

    std::uint64_t time_ms = now_in_us / 1000; // timestamp, ms part
    std::uint64_t time_us = now_in_us % 1000; // timestamp, us part

    std::uint64_t newst = ( time_ms << 16 ) | ( time_us << 6 );

    // if the time has advanced, reset counter to zero
    if( newst > oldst )
    {
        return newst;
    }

    // if time_in_ms has gone backwards, we can't be monotonic
    if( time_ms < ( oldst >> 16 ) )
    {
        return newst;
    }

    // otherwise, use the old value and increment the counter
    return oldst + 1;
}

// operator()

inline time_generator_v7::result_type time_generator_v7::operator()() noexcept
{
    uuid result;

    // set lower 64 bits to random values

    std::uniform_int_distribution<std::uint32_t> dist;

    detail::store_native_u32( result.data +  8, dist( rng_ ) );
    detail::store_native_u32( result.data + 12, dist( rng_ ) );

    // get new timestamp
    state_ = get_new_state( state_ );

    std::uint64_t time_ms = state_ >> 16; // timestamp, ms part
    std::uint64_t time_us = ( state_ & 0xFFFF ) >> 6; // timestamp, us part

    std::uint64_t timestamp = ( time_ms << 16 ) | 0x7000 | time_us;

    detail::store_big_u64( result.data + 0, timestamp );

    // set variant and counter

    result.data[ 8 ] = static_cast< std::uint8_t >( 0x80 | ( state_ & 0x3F ) );

    return result;
}

}} // namespace boost::uuids

#endif // BOOST_UUID_TIME_GENERATOR_V7_HPP_INCLUDED
