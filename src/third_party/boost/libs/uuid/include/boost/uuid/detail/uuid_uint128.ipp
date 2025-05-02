#ifndef BOOST_UUID_DETAIL_UUID_UINT128_IPP_INCLUDED
#define BOOST_UUID_DETAIL_UUID_UINT128_IPP_INCLUDED

// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/uuid/detail/endian.hpp>

#if defined(BOOST_UUID_REPORT_IMPLEMENTATION)

#include <boost/config/pragma_message.hpp>
BOOST_PRAGMA_MESSAGE( "Using uuid_uint128.ipp" )

#endif

namespace boost {
namespace uuids {

inline bool uuid::is_nil() const noexcept
{
    __uint128_t v = detail::load_native_u128( this->data );
    return v == 0;
}

inline void uuid::swap( uuid& rhs ) noexcept
{
    __uint128_t v1 = detail::load_native_u128( this->data );
    __uint128_t v2 = detail::load_native_u128( rhs.data );

    detail::store_native_u128( this->data, v2 );
    detail::store_native_u128( rhs.data, v1 );
}

inline bool operator==( uuid const& lhs, uuid const& rhs ) noexcept
{
    __uint128_t v1 = detail::load_native_u128( lhs.data );
    __uint128_t v2 = detail::load_native_u128( rhs.data );

    return v1 == v2;
}

inline bool operator<( uuid const& lhs, uuid const& rhs ) noexcept
{
    __uint128_t v1 = detail::load_big_u128( lhs.data );
    __uint128_t v2 = detail::load_big_u128( rhs.data );

    return v1 < v2;
}

#if defined(BOOST_UUID_HAS_THREE_WAY_COMPARISON)

inline std::strong_ordering operator<=> (uuid const& lhs, uuid const& rhs) noexcept
{
    __uint128_t v1 = detail::load_big_u128( lhs.data );
    __uint128_t v2 = detail::load_big_u128( rhs.data );

    return v1 <=> v2;
}

#endif

} // namespace uuids
} // namespace boost

#endif // BOOST_UUID_DETAIL_UUID_UINT128_IPP_INCLUDED
