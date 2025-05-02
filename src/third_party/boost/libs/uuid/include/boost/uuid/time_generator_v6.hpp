#ifndef BOOST_UUID_TIME_GENERATOR_V6_HPP_INCLUDED
#define BOOST_UUID_TIME_GENERATOR_V6_HPP_INCLUDED

// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/uuid/time_generator_v1.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/detail/endian.hpp>

namespace boost {
namespace uuids {

// time_generator_v6

class time_generator_v6: public time_generator_v1
{
public:

    using result_type = uuid;

    using time_generator_v1::time_generator_v1;

    result_type operator()() noexcept;
};

// operator()

inline time_generator_v6::result_type time_generator_v6::operator()() noexcept
{
    uuid result = time_generator_v1::operator()();

    std::uint64_t timestamp = result.timestamp_v1();

    std::uint32_t time_high = static_cast< std::uint32_t >( timestamp >> 28 );

    detail::store_big_u32( result.data + 0, time_high );

    std::uint16_t time_mid = static_cast< std::uint16_t >( timestamp >> 12 );

    detail::store_big_u16( result.data + 4, time_mid );

    std::uint16_t time_low_and_version = static_cast< std::uint16_t >( timestamp & 0xFFF ) | 0x6000;

    detail::store_big_u16( result.data + 6, time_low_and_version );

    return result;
}

}} // namespace boost::uuids

#endif // BOOST_UUID_TIME_GENERATOR_V1_HPP_INCLUDED
