#ifndef BOOST_UUID_DETAIL_BASIC_NAME_GENERATOR_HPP_INCLUDED
#define BOOST_UUID_DETAIL_BASIC_NAME_GENERATOR_HPP_INCLUDED

// Boost basic_name_generator.hpp header file  -----------------------//

// Copyright 2010 Andy Tompkins.
// Copyright 2017 James E. King III

// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
//  https://www.boost.org/LICENSE_1_0.txt)

#include <boost/uuid/namespaces.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/detail/static_assert.hpp>
#include <boost/config.hpp>
#include <string>
#include <cstdint>
#include <cstring> // for strlen, wcslen

namespace boost {
namespace uuids {
namespace detail {

template<class HashAlgo>
class basic_name_generator
{
private:

    uuid namespace_uuid_;

private:

    using digest_type = typename HashAlgo::digest_type;

public:

    using result_type = uuid;

    explicit basic_name_generator( uuid const& namespace_uuid ) noexcept
        : namespace_uuid_( namespace_uuid )
    {}

    template<class Ch> uuid operator()( Ch const* name ) const noexcept
    {
        HashAlgo hash;

        hash.process_bytes( namespace_uuid_.begin(), namespace_uuid_.size() );
        process_characters( hash, name, std::char_traits<Ch>().length( name ) );

        return hash_to_uuid( hash );
    }

    template<class Ch, class Traits, class Alloc>
    uuid operator()( std::basic_string<Ch, Traits, Alloc> const& name ) const noexcept
    {
        HashAlgo hash;

        hash.process_bytes( namespace_uuid_.begin(), namespace_uuid_.size() );
        process_characters( hash, name.c_str(), name.length() );

        return hash_to_uuid( hash );
    }

    uuid operator()( void const* buffer, std::size_t byte_count ) const noexcept
    {
        HashAlgo hash;

        hash.process_bytes( namespace_uuid_.begin(), namespace_uuid_.size() );
        hash.process_bytes( buffer, byte_count );

        return hash_to_uuid( hash );
    }

private:

    void process_characters( HashAlgo& hash, char const* p, std::size_t n ) const noexcept
    {
        hash.process_bytes( p, n );
    }

    // For portability, we convert all wide characters to uint32_t so that each
    // character is 4 bytes regardless of sizeof(wchar_t).

    void process_characters( HashAlgo& hash, wchar_t const* p, std::size_t n ) const noexcept
    {
        BOOST_UUID_STATIC_ASSERT( sizeof( std::uint32_t ) >= sizeof( wchar_t ) );

        for( std::size_t i = 0; i < n; ++i)
        {
            std::uint32_t ch = p[ i ];

            unsigned char bytes[ 4 ] =
            {
                static_cast<unsigned char>( ( ch >>  0 ) & 0xFF ),
                static_cast<unsigned char>( ( ch >>  8 ) & 0xFF ),
                static_cast<unsigned char>( ( ch >> 16 ) & 0xFF ),
                static_cast<unsigned char>( ( ch >> 24 ) & 0xFF )
            };

            hash.process_bytes( bytes, 4 );
        }
    }

    void process_characters( HashAlgo& hash, char32_t const* p, std::size_t n ) const noexcept
    {
        for( std::size_t i = 0; i < n; ++i)
        {
            process_utf32_codepoint( hash, p[ i ] );
        }
    }

    void process_characters( HashAlgo& hash, char16_t const* p, std::size_t n ) const noexcept
    {
        for( std::size_t i = 0; i < n; ++i)
        {
            char16_t ch = p[ i ];

            if( ch >= 0xD800 && ch <= 0xDBFF && i + 1 < n && p[ i+1 ] >= 0xDC00 && p[ i+1 ] <= 0xDFFF )
            {
                char16_t ch2 = p[ ++i ];

                std::uint32_t high = ch - 0xD800;
                std::uint32_t low = ch2 - 0xDC00;

                process_utf32_codepoint( hash, ( high << 10 ) + low + 0x10000 );
            }
            else
            {
                process_utf32_codepoint( hash, ch );
            }
        }
    }

    void process_utf32_codepoint( HashAlgo& hash, std::uint32_t cp ) const noexcept
    {
        if( ( cp >= 0xD800 && cp <= 0xDFFF ) || cp > 0x10FFFF )
        {
            cp = 0xFFFD; // Unicode replacement character
        }

        if( cp < 0x80 )
        {
            hash.process_byte( static_cast<unsigned char>( cp ) );
        }
        else if( cp < 0x800 )
        {
            unsigned char bytes[ 2 ] =
            {
                static_cast<unsigned char>( 0xC0 | (cp >>   6) ),
                static_cast<unsigned char>( 0x80 | (cp & 0x3F) )
            };

            hash.process_bytes( bytes, 2 );
        }
        else if( cp < 0x10000 )
        {
            unsigned char bytes[ 3 ] =
            {
                static_cast<unsigned char>( 0xE0 | (cp >> 12)         ),
                static_cast<unsigned char>( 0x80 | ((cp >> 6) & 0x3F) ),
                static_cast<unsigned char>( 0x80 | (cp & 0x3F)        )
            };

            hash.process_bytes( bytes, 3 );
        }
        else
        {
            unsigned char bytes[ 4 ] =
            {
                static_cast<unsigned char>( 0xF0 | ( cp >> 18 )          ),
                static_cast<unsigned char>( 0x80 | ((cp >> 12 ) & 0x3F ) ),
                static_cast<unsigned char>( 0x80 | ((cp >>  6 ) & 0x3F ) ),
                static_cast<unsigned char>( 0x80 | (cp & 0x3F)           )
            };

            hash.process_bytes( bytes, 4 );
        }
    }

#if defined(__cpp_char8_t) && __cpp_char8_t >= 201811L

    void process_characters( HashAlgo& hash, char8_t const* p, std::size_t n ) const noexcept
    {
        hash.process_bytes( p, n );
    }

#endif

    uuid hash_to_uuid( HashAlgo& hash ) const noexcept
    {
        digest_type digest;
        hash.get_digest(digest);

        BOOST_UUID_STATIC_ASSERT( sizeof(digest_type) >= 16 );

        uuid u;
        std::memcpy( u.data, digest, 16 );

        // set variant: must be 0b10xxxxxx
        *(u.begin()+8) &= 0x3F;
        *(u.begin()+8) |= 0x80;

        // set version
        unsigned char hashver = hash.get_version();
        *(u.begin()+6) &= 0x0F;             // clear out the relevant bits
        *(u.begin()+6) |= (hashver << 4);   // and apply them

        return u;
    }
};

} // namespace detail
} // uuids
} // boost

#endif // BOOST_UUID_DETAIL_BASIC_NAME_GENERATOR_HPP_INCLUDED
