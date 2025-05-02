#ifndef BOOST_UUID_UUID_HPP_INCLUDED
#define BOOST_UUID_UUID_HPP_INCLUDED

// Copyright 2006 Andy Tompkins
// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/uuid/uuid_clock.hpp>
#include <boost/uuid/detail/endian.hpp>
#include <boost/uuid/detail/hash_mix.hpp>
#include <boost/uuid/detail/config.hpp>
#include <boost/type_traits/integral_constant.hpp> // for Serialization support
#include <boost/config.hpp>
#include <boost/config/workaround.hpp>
#include <array>
#include <chrono>
#include <typeindex> // cheapest std::hash
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__has_builtin)
# if __has_builtin(__builtin_is_constant_evaluated)
#  define BOOST_UUID_HAS_BUILTIN_ISCONSTEVAL
# endif
#endif

#if !defined(BOOST_UUID_HAS_BUILTIN_ISCONSTEVAL) && defined(BOOST_MSVC) && BOOST_MSVC >= 1925
# define BOOST_UUID_HAS_BUILTIN_ISCONSTEVAL
#endif

#if defined(__cpp_impl_three_way_comparison) && __cpp_impl_three_way_comparison >= 201907L && defined(__has_include)
# if __has_include(<compare>)
#  include <compare>
#  if defined(__cpp_lib_three_way_comparison) && __cpp_lib_three_way_comparison >= 201907L
#   define BOOST_UUID_HAS_THREE_WAY_COMPARISON __cpp_lib_three_way_comparison
#  elif defined(_LIBCPP_VERSION)
//  https://github.com/llvm/llvm-project/issues/73953
#   define BOOST_UUID_HAS_THREE_WAY_COMPARISON _LIBCPP_VERSION
#  endif
# endif
#endif

namespace boost {
namespace uuids {

struct uuid
{
private:

    using repr_type = std::uint8_t[ 16 ];

    struct data_type
    {
    private:

        union
        {
#if BOOST_WORKAROUND(BOOST_MSVC, < 1950)

            std::uint8_t repr_[ 16 ] = {};

#else

            std::uint8_t repr_[ 16 ];

#endif

#if !defined(BOOST_UUID_DISABLE_ALIGNMENT)

            std::uint64_t align_u64_;

#endif
        };

    public:

        BOOST_CXX14_CONSTEXPR operator repr_type& () noexcept { return repr_; }
        constexpr operator repr_type const& () const noexcept { return repr_; }

        BOOST_CXX14_CONSTEXPR std::uint8_t* operator()() noexcept { return repr_; }
        constexpr std::uint8_t const* operator()() const noexcept { return repr_; }

#if BOOST_WORKAROUND(BOOST_MSVC, < 1930)

        BOOST_CXX14_CONSTEXPR std::uint8_t* operator+( std::ptrdiff_t i ) noexcept { return repr_ + i; }
        constexpr std::uint8_t const* operator+( std::ptrdiff_t i ) const noexcept { return repr_ + i; }

        BOOST_CXX14_CONSTEXPR std::uint8_t& operator[]( std::ptrdiff_t i ) noexcept { return repr_[ i ]; }
        constexpr std::uint8_t const& operator[]( std::ptrdiff_t i ) const noexcept { return repr_[ i ]; }

#endif
    };

public:

    // data

#if BOOST_WORKAROUND(BOOST_MSVC, < 1950)

    data_type data;

#else

    data_type data = {};

#endif

public:

    // constructors

    uuid() = default;

#if defined(BOOST_NO_CXX14_CONSTEXPR)

    uuid( repr_type const& r ) noexcept
    {
        std::memcpy( data, r, 16 );
    }

#elif defined(BOOST_UUID_HAS_BUILTIN_ISCONSTEVAL)

    constexpr uuid( repr_type const& r ) noexcept
    {
        if( __builtin_is_constant_evaluated() )
        {
            for( int i = 0; i < 16; ++i ) data[ i ] = r[ i ];
        }
        else
        {
            std::memcpy( data, r, 16 );
        }
    }

#else

    constexpr uuid( repr_type const& r ) noexcept
    {
        for( int i = 0; i < 16; ++i ) data[ i ] = r[ i ];
    }

#endif

    // iteration

    using value_type = std::uint8_t;
    using reference = std::uint8_t&;
    using const_reference = std::uint8_t const&;
    using iterator = std::uint8_t*;
    using const_iterator = std::uint8_t const*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    iterator begin() noexcept { return data; }
    const_iterator begin() const noexcept { return data; }

    iterator end() noexcept { return data() + size(); }
    const_iterator end() const noexcept { return data() + size(); }

    // size

    constexpr size_type size() const noexcept { return static_size(); }

    // This does not work on some compilers
    // They seem to want the variable defined in
    // a cpp file
    //BOOST_STATIC_CONSTANT(size_type, static_size = 16);
    static constexpr size_type static_size() noexcept { return 16; }

    // is_nil

    bool is_nil() const noexcept;

    // variant

    enum variant_type
    {
        variant_ncs, // NCS backward compatibility
        variant_rfc_4122, // defined in RFC 4122 document
        variant_microsoft, // Microsoft Corporation backward compatibility
        variant_future // future definition
    };

    variant_type variant() const noexcept
    {
        // variant is stored in octet 7
        // which is index 8, since indexes count backwards
        unsigned char octet7 = data[8]; // octet 7 is array index 8
        if ( (octet7 & 0x80) == 0x00 ) { // 0b0xxxxxxx
            return variant_ncs;
        } else if ( (octet7 & 0xC0) == 0x80 ) { // 0b10xxxxxx
            return variant_rfc_4122;
        } else if ( (octet7 & 0xE0) == 0xC0 ) { // 0b110xxxxx
            return variant_microsoft;
        } else {
            //assert( (octet7 & 0xE0) == 0xE0 ) // 0b111xxxx
            return variant_future;
        }
    }

    // version

    enum version_type
    {
        version_unknown = -1,
        version_time_based = 1,
        version_dce_security = 2,
        version_name_based_md5 = 3,
        version_random_number_based = 4,
        version_name_based_sha1 = 5,
        version_time_based_v6 = 6,
        version_time_based_v7 = 7,
        version_custom_v8 = 8
    };

    version_type version() const noexcept
    {
        // version is stored in octet 9
        // which is index 6, since indexes count backwards
        std::uint8_t octet9 = data[6];
        if ( (octet9 & 0xF0) == 0x10 ) {
            return version_time_based;
        } else if ( (octet9 & 0xF0) == 0x20 ) {
            return version_dce_security;
        } else if ( (octet9 & 0xF0) == 0x30 ) {
            return version_name_based_md5;
        } else if ( (octet9 & 0xF0) == 0x40 ) {
            return version_random_number_based;
        } else if ( (octet9 & 0xF0) == 0x50 ) {
            return version_name_based_sha1;
        } else if ( (octet9 & 0xF0) == 0x60 ) {
            return version_time_based_v6;
        } else if ( (octet9 & 0xF0) == 0x70 ) {
            return version_time_based_v7;
        } else if ( (octet9 & 0xF0) == 0x80 ) {
            return version_custom_v8;
        } else {
            return version_unknown;
        }
    }

    // timestamp

    using timestamp_type = std::uint64_t;

    timestamp_type timestamp_v1() const noexcept
    {
        std::uint32_t time_low = detail::load_big_u32( this->data + 0 );
        std::uint16_t time_mid = detail::load_big_u16( this->data + 4 );
        std::uint16_t time_hi = detail::load_big_u16( this->data + 6 ) & 0x0FFF;

        return time_low | static_cast<std::uint64_t>( time_mid ) << 32 | static_cast<std::uint64_t>( time_hi ) << 48;
    }

    timestamp_type timestamp_v6() const noexcept
    {
        std::uint32_t time_high = detail::load_big_u32( this->data + 0 );
        std::uint16_t time_mid = detail::load_big_u16( this->data + 4 );
        std::uint16_t time_low = detail::load_big_u16( this->data + 6 ) & 0x0FFF;

        return time_low | static_cast<std::uint64_t>( time_mid ) << 12 | static_cast<std::uint64_t>( time_high ) << 28;
    }

    timestamp_type timestamp_v7() const noexcept
    {
        std::uint64_t time_and_version = detail::load_big_u64( this->data + 0 );
        return time_and_version >> 16;
    }

    // time_point

    uuid_clock::time_point time_point_v1() const noexcept
    {
        return uuid_clock::from_timestamp( timestamp_v1() );
    }

    uuid_clock::time_point time_point_v6() const noexcept
    {
        return uuid_clock::from_timestamp( timestamp_v6() );
    }

    std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds> time_point_v7() const noexcept
    {
        return std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds>( std::chrono::milliseconds( timestamp_v7() ) );
    }

    // clock_seq

    using clock_seq_type = std::uint16_t;

    clock_seq_type clock_seq() const noexcept
    {
        return detail::load_big_u16( this->data + 8 ) & 0x3FFF;
    }

    // node_identifier

    using node_type = std::array<std::uint8_t, 6>;

    node_type node_identifier() const noexcept
    {
        node_type node = {{}};

        std::memcpy( node.data(), this->data + 10, 6 );
        return node;
    }

    // swap

    void swap( uuid& rhs ) noexcept;
};

// operators

inline bool operator==( uuid const& lhs, uuid const& rhs ) noexcept;
inline bool operator< ( uuid const& lhs, uuid const& rhs ) noexcept;

inline bool operator!=( uuid const& lhs, uuid const& rhs ) noexcept
{
    return !(lhs == rhs);
}

inline bool operator>( uuid const& lhs, uuid const& rhs ) noexcept
{
    return rhs < lhs;
}
inline bool operator<=( uuid const& lhs, uuid const& rhs ) noexcept
{
    return !(rhs < lhs);
}

inline bool operator>=( uuid const& lhs, uuid const& rhs ) noexcept
{
    return !(lhs < rhs);
}

#if defined(BOOST_UUID_HAS_THREE_WAY_COMPARISON)

inline std::strong_ordering operator<=>( uuid const& lhs, uuid const& rhs ) noexcept;

#endif

// swap

inline void swap( uuid& lhs, uuid& rhs ) noexcept
{
    lhs.swap( rhs );
}

// hash_value

inline std::size_t hash_value( uuid const& u ) noexcept
{
    std::uint64_t r = 0;

    r = detail::hash_mix_mx( r + detail::load_little_u32( u.data +  0 ) );
    r = detail::hash_mix_mx( r + detail::load_little_u32( u.data +  4 ) );
    r = detail::hash_mix_mx( r + detail::load_little_u32( u.data +  8 ) );
    r = detail::hash_mix_mx( r + detail::load_little_u32( u.data + 12 ) );

    return static_cast<std::size_t>( detail::hash_mix_fmx( r ) );
}

}} //namespace boost::uuids

// Boost.Serialization support

// BOOST_CLASS_IMPLEMENTATION(boost::uuids::uuid, boost::serialization::primitive_type)

namespace boost
{
namespace serialization
{

template<class T> struct implementation_level_impl;
template<> struct implementation_level_impl<const uuids::uuid>: boost::integral_constant<int, 1> {};

} // namespace serialization
} // namespace boost

// std::hash support

namespace std
{

template<> struct hash<boost::uuids::uuid>
{
    std::size_t operator()( boost::uuids::uuid const& value ) const noexcept
    {
        return boost::uuids::hash_value( value );
    }
};

} // namespace std

#if defined(BOOST_UUID_USE_SSE2)
# include <boost/uuid/detail/uuid_x86.ipp>
#elif defined(__SIZEOF_INT128__)
# include <boost/uuid/detail/uuid_uint128.ipp>
#else
# include <boost/uuid/detail/uuid_generic.ipp>
#endif

#endif // BOOST_UUID_UUID_HPP_INCLUDED
