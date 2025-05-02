#ifndef BOOST_UUID_DETAIL_TO_CHARS_HPP_INCLUDED
#define BOOST_UUID_DETAIL_TO_CHARS_HPP_INCLUDED

// Copyright 2009 Andy Tompkins
// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/uuid/uuid.hpp>

namespace boost {
namespace uuids {
namespace detail {

constexpr char const* digits( char const* ) noexcept
{
    return "0123456789abcdef-";
}

constexpr wchar_t const* digits( wchar_t const* ) noexcept
{
    return L"0123456789abcdef-";
}

constexpr char16_t const* digits( char16_t const* ) noexcept
{
    return u"0123456789abcdef-";
}

constexpr char32_t const* digits( char32_t const* ) noexcept
{
    return U"0123456789abcdef-";
}

#if defined(__cpp_char8_t) && __cpp_char8_t >= 201811L

constexpr char8_t const* digits( char8_t const* ) noexcept
{
    return u8"0123456789abcdef-";
}

#endif

template<class Ch> inline Ch* to_chars( uuid const& u, Ch* out ) noexcept
{
    constexpr Ch const* p = digits( static_cast<Ch const*>( nullptr ) );

    for( std::size_t i = 0; i < 16; ++i )
    {
        std::uint8_t ch = u.data()[ i ];

        *out++ = p[ (ch >> 4) & 0x0F ];
        *out++ = p[ ch & 0x0F ];

        if( i == 3 || i == 5 || i == 7 || i == 9 )
        {
            *out++ = p[ 16 ];
        }
    }

    return out;
}

} // namespace detail
}} //namespace boost::uuids

#endif // BOOST_UUID_DETAIL_TO_CHARS_HPP_INCLUDED
