#ifndef BOOST_UUID_TIME_GENERATOR_V1_HPP_INCLUDED
#define BOOST_UUID_TIME_GENERATOR_V1_HPP_INCLUDED

// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_clock.hpp>
#include <boost/uuid/detail/random_provider.hpp>
#include <boost/uuid/detail/endian.hpp>
#include <boost/config.hpp>
#include <boost/config/workaround.hpp>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace boost {
namespace uuids {

// time_generator_v1

class time_generator_v1
{
public:

    struct state_type
    {
        std::uint64_t timestamp;
        std::uint16_t clock_seq;

// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=114865
#if BOOST_WORKAROUND(BOOST_LIBSTDCXX_VERSION, >= 130000)
# if BOOST_CXX_VERSION >= 201402L

        std::uint16_t padding[ 3 ] = {};

# else

        std::uint16_t padding[ 3 ];

# endif
#endif
    };

private:

    uuid::node_type node_ = {{}};

    std::atomic<state_type>* ps_ = nullptr;

#if BOOST_WORKAROUND(BOOST_GCC, < 50000)

    // Avoid -Wmissing-field-initializers under GCC 4.x
    state_type state_ = { 0, 0 };

#else

    state_type state_ = {};

#endif

public:

    using result_type = uuid;

    time_generator_v1();
    time_generator_v1( uuid::node_type const& node, state_type const& state ) noexcept;
    time_generator_v1( uuid::node_type const& node, std::atomic<state_type>& state ) noexcept;

    result_type operator()() noexcept;

private:

    static state_type get_new_state( state_type const& oldst ) noexcept;
};

// constructors

inline time_generator_v1::time_generator_v1()
{
    detail::random_provider prov;

    // generate a pseudorandom node identifier

    std::uint32_t tmp[ 3 ];
    prov.generate( tmp, tmp + 3 );

    std::memcpy( node_.data(), tmp, node_.size() );
    node_[ 0 ] |= 0x01; // mark as multicast

    // generate a pseudorandom 14 bit clock sequence

    state_.clock_seq = static_cast<std::uint16_t>( tmp[ 2 ] & 0x3FFF );
}

inline time_generator_v1::time_generator_v1( uuid::node_type const& node, state_type const& state ) noexcept: node_( node ), state_( state )
{
}

inline time_generator_v1::time_generator_v1( uuid::node_type const& node, std::atomic<state_type>& state ) noexcept: node_( node ), ps_( &state )
{
}

// get_new_state

inline time_generator_v1::state_type time_generator_v1::get_new_state( state_type const& oldst ) noexcept
{
    state_type newst( oldst );

    std::uint64_t timestamp = uuid_clock::now().time_since_epoch().count();

    if( timestamp <= newst.timestamp )
    {
        newst.clock_seq = ( newst.clock_seq + 1 ) & 0x3FFF;
    }

    newst.timestamp = timestamp;

    return newst;
}

// operator()

inline time_generator_v1::result_type time_generator_v1::operator()() noexcept
{
    if( ps_ )
    {
        auto oldst = ps_->load( std::memory_order_relaxed );

        for( ;; )
        {
            auto newst = get_new_state( oldst );

            if( ps_->compare_exchange_strong( oldst, newst, std::memory_order_relaxed, std::memory_order_relaxed ) )
            {
                state_ = newst;
                break;
            }
        }
    }
    else
    {
        state_ = get_new_state( state_ );
    }

    uuid result;

    std::uint32_t time_low = static_cast< std::uint32_t >( state_.timestamp );

    detail::store_big_u32( result.data + 0, time_low );

    std::uint16_t time_mid = static_cast< std::uint16_t >( state_.timestamp >> 32 );

    detail::store_big_u16( result.data + 4, time_mid );

    std::uint16_t time_hi_and_version = static_cast< std::uint16_t >( state_.timestamp >> 48 ) | 0x1000;

    detail::store_big_u16( result.data + 6, time_hi_and_version );

    detail::store_big_u16( result.data + 8, state_.clock_seq | 0x8000 );

    std::memcpy( result.data + 10, node_.data(), 6 );

    return result;
}

}} // namespace boost::uuids

#endif // BOOST_UUID_TIME_GENERATOR_V1_HPP_INCLUDED
