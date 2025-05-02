#ifndef BOOST_UUID_UUID_CLOCK_HPP_INCLUDED
#define BOOST_UUID_UUID_CLOCK_HPP_INCLUDED

// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <chrono>
#include <ratio>
#include <cstdint>

namespace boost {
namespace uuids {

class uuid_clock
{
public:

    using rep = std::int64_t;
    using period = std::ratio<1, 10000000>; // 100ns
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<uuid_clock, duration>;

    static constexpr bool is_steady = false;

    static time_point now() noexcept;

    template<class Duration> static time_point from_sys( std::chrono::time_point<std::chrono::system_clock, Duration> const& tp ) noexcept;
    static std::chrono::time_point<std::chrono::system_clock, duration> to_sys( time_point const& tp ) noexcept;

    static time_point from_timestamp( std::uint64_t timestamp ) noexcept;
    static std::uint64_t to_timestamp( time_point const& tp ) noexcept;
};

inline uuid_clock::time_point uuid_clock::now() noexcept
{
    return from_sys( std::chrono::system_clock::now() );
}

template<class Duration>
inline uuid_clock::time_point uuid_clock::from_sys( std::chrono::time_point<std::chrono::system_clock, Duration> const& tp ) noexcept
{
    using days = std::chrono::duration< std::int32_t, std::ratio<86400> >;
    constexpr auto epoch_diff = days( 141427 );

    auto uuid_since = std::chrono::duration_cast<uuid_clock::duration>( tp.time_since_epoch() ) + epoch_diff;

    return uuid_clock::time_point( uuid_since );
}

inline std::chrono::time_point<std::chrono::system_clock, uuid_clock::duration> uuid_clock::to_sys( time_point const& tp ) noexcept
{
    using days = std::chrono::duration< std::int32_t, std::ratio<86400> >;
    constexpr auto epoch_diff = days( 141427 );

    return std::chrono::time_point<std::chrono::system_clock, duration>( tp.time_since_epoch() - epoch_diff );
}

inline uuid_clock::time_point uuid_clock::from_timestamp( std::uint64_t timestamp ) noexcept
{
    return time_point( duration( timestamp ) );
}

inline std::uint64_t uuid_clock::to_timestamp( time_point const& tp ) noexcept
{
    return static_cast<std::uint64_t>(tp.time_since_epoch().count());
}

} // uuids
} // boost

#endif // BOOST_UUID_UUID_CLOCK_HPP_INCLUDED
