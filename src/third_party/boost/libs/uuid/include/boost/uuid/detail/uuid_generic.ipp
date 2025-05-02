#ifndef BOOST_UUID_DETAIL_UUID_GENERIC_IPP_INCLUDED
#define BOOST_UUID_DETAIL_UUID_GENERIC_IPP_INCLUDED

// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/uuid/detail/endian.hpp>
#include <cstdint>

#if defined(BOOST_UUID_REPORT_IMPLEMENTATION)

#include <boost/config/pragma_message.hpp>
BOOST_PRAGMA_MESSAGE( "Using uuid_generic.ipp" )

#endif

namespace boost {
namespace uuids {

inline bool uuid::is_nil() const noexcept
{
    std::uint64_t v = detail::load_native_u64( this->data + 0 );
    std::uint64_t w = detail::load_native_u64( this->data + 8 );

    return v == 0 && w == 0;
}

inline void uuid::swap( uuid& rhs ) noexcept
{
    std::uint64_t v1 = detail::load_native_u64( this->data + 0 );
    std::uint64_t w1 = detail::load_native_u64( this->data + 8 );

    std::uint64_t v2 = detail::load_native_u64( rhs.data + 0 );
    std::uint64_t w2 = detail::load_native_u64( rhs.data + 8 );

    detail::store_native_u64( this->data + 0, v2 );
    detail::store_native_u64( this->data + 8, w2 );

    detail::store_native_u64( rhs.data + 0, v1 );
    detail::store_native_u64( rhs.data + 8, w1 );
}

inline bool operator==( uuid const& lhs, uuid const& rhs ) noexcept
{
    std::uint64_t v1 = detail::load_native_u64( lhs.data + 0 );
    std::uint64_t w1 = detail::load_native_u64( lhs.data + 8 );

    std::uint64_t v2 = detail::load_native_u64( rhs.data + 0 );
    std::uint64_t w2 = detail::load_native_u64( rhs.data + 8 );

    return v1 == v2 && w1 == w2;
}

inline bool operator<( uuid const& lhs, uuid const& rhs ) noexcept
{
    std::uint64_t v1 = detail::load_big_u64( lhs.data + 0 );
    std::uint64_t w1 = detail::load_big_u64( lhs.data + 8 );

    std::uint64_t v2 = detail::load_big_u64( rhs.data + 0 );
    std::uint64_t w2 = detail::load_big_u64( rhs.data + 8 );

    return v1 < v2 || ( !( v2 < v1 ) && w1 < w2 );
}

#if defined(BOOST_UUID_HAS_THREE_WAY_COMPARISON)

inline std::strong_ordering operator<=> (uuid const& lhs, uuid const& rhs) noexcept
{
    std::uint64_t v1 = detail::load_big_u64( lhs.data + 0 );
    std::uint64_t w1 = detail::load_big_u64( lhs.data + 8 );

    std::uint64_t v2 = detail::load_big_u64( rhs.data + 0 );
    std::uint64_t w2 = detail::load_big_u64( rhs.data + 8 );

    if( v1 < v2 ) return std::strong_ordering::less;
    if( v1 > v2 ) return std::strong_ordering::greater;

    return w1 <=> w2;
}

#endif

} // namespace uuids
} // namespace boost

#endif // BOOST_UUID_DETAIL_UUID_GENERIC_IPP_INCLUDED
