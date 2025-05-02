#ifndef BOOST_UUID_STRING_GENERATOR_HPP_INCLUDED
#define BOOST_UUID_STRING_GENERATOR_HPP_INCLUDED

// Copyright 2010 Andy Tompkins
// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/uuid/uuid.hpp>
#include <boost/throw_exception.hpp>
#include <boost/config.hpp>
#include <string>
#include <iterator>
#include <algorithm> // for find
#include <stdexcept>
#include <cstring> // for strlen, wcslen
#include <cstdio>

namespace boost {
namespace uuids {

// Generates a UUID from a string
//
// Accepts the following forms:
//
// 0123456789abcdef0123456789abcdef
// 01234567-89ab-cdef-0123-456789abcdef
// {01234567-89ab-cdef-0123-456789abcdef}
// {0123456789abcdef0123456789abcdef}

struct string_generator
{
    using result_type = uuid;

    template<class Ch, class Traits, class Alloc>
    uuid operator()( std::basic_string<Ch, Traits, Alloc> const& s ) const
    {
        return operator()(s.begin(), s.end());
    }

    uuid operator()( char const* s ) const
    {
        return operator()( s, s + std::strlen( s ) );
    }

    uuid operator()( wchar_t const* s ) const
    {
        return operator()( s, s + std::wcslen( s ) );
    }

    template<class CharIterator>
    uuid operator()( CharIterator begin, CharIterator end ) const
    {
        using char_type = typename std::iterator_traits<CharIterator>::value_type;

        int ipos = 0;

        // check open brace
        char_type c = get_next_char( begin, end, ipos );

        bool has_open_brace = is_open_brace( c );

        char_type open_brace_char = c;

        if( has_open_brace )
        {
            c = get_next_char( begin, end, ipos );
        }

        bool has_dashes = false;

        uuid u;

        int i = 0;

        for( uuid::iterator it_byte = u.begin(); it_byte != u.end(); ++it_byte, ++i )
        {
            if( it_byte != u.begin() )
            {
                c = get_next_char( begin, end, ipos );
            }

            if( i == 4 )
            {
                has_dashes = is_dash( c );

                if( has_dashes )
                {
                    c = get_next_char( begin, end, ipos );
                }
            }
            else if( i == 6 || i == 8 || i == 10 )
            {
                // if there are dashes, they must be in every slot
                if( has_dashes )
                {
                    if( is_dash( c ) )
                    {
                        c = get_next_char( begin, end, ipos );
                    }
                    else
                    {
                        throw_invalid( ipos - 1, "dash expected" );
                    }
                }
            }

            *it_byte = get_value( c, ipos - 1 );

            c = get_next_char( begin, end, ipos );

            *it_byte <<= 4;
            *it_byte |= get_value( c, ipos - 1 );
        }

        // check close brace
        if( has_open_brace )
        {
            c = get_next_char( begin, end, ipos );
            check_close_brace( c, open_brace_char, ipos - 1 );
        }

        // check end of string - any additional data is an invalid uuid
        if( begin != end )
        {
            throw_invalid( ipos, "unexpected extra input" );
        }

        return u;
    }

private:

    BOOST_NORETURN void throw_invalid( int ipos, char const* error ) const
    {
        char buffer[ 16 ];
        std::snprintf( buffer, sizeof( buffer ), "%d", ipos );

        BOOST_THROW_EXCEPTION( std::runtime_error( std::string( "Invalid UUID string at position " ) + buffer + ": " + error ) );
    }

    template <typename CharIterator>
    typename std::iterator_traits<CharIterator>::value_type
    get_next_char( CharIterator& begin, CharIterator end, int& ipos ) const
    {
        if( begin == end )
        {
            throw_invalid( ipos, "unexpected end of input" );
        }

        ++ipos;
        return *begin++;
    }

    unsigned char get_value( char c, int ipos ) const
    {
        static char const digits_begin[] = "0123456789abcdefABCDEF";
        static size_t digits_len = (sizeof(digits_begin) / sizeof(char)) - 1;
        static char const* const digits_end = digits_begin + digits_len;

        static unsigned char const values[] =
            { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,10,11,12,13,14,15 };

        size_t pos = std::find( digits_begin, digits_end, c ) - digits_begin;

        if( pos >= digits_len )
        {
            throw_invalid( ipos, "hex digit expected" );
        }

        return values[ pos ];
    }

    unsigned char get_value( wchar_t c, int ipos ) const
    {
        static wchar_t const digits_begin[] = L"0123456789abcdefABCDEF";
        static size_t digits_len = (sizeof(digits_begin) / sizeof(wchar_t)) - 1;
        static wchar_t const* const digits_end = digits_begin + digits_len;

        static unsigned char const values[] =
            { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,10,11,12,13,14,15 };

        size_t pos = std::find( digits_begin, digits_end, c ) - digits_begin;

        if( pos >= digits_len )
        {
            throw_invalid( ipos, "hex digit expected" );
        }

        return values[ pos ];
    }

    bool is_dash( char c ) const
    {
        return c == '-';
    }

    bool is_dash( wchar_t c ) const
    {
        return c == L'-';
    }

    // return closing brace
    bool is_open_brace( char c ) const
    {
        return c == '{';
    }

    bool is_open_brace( wchar_t c ) const
    {
        return c == L'{';
    }

    void check_close_brace( char c, char open_brace, int ipos ) const
    {
        if( open_brace == '{' && c == '}' )
        {
            //great
        }
        else
        {
            throw_invalid( ipos, "closing brace expected" );
        }
    }

    void check_close_brace( wchar_t c, wchar_t open_brace, int ipos ) const
    {
        if( open_brace == L'{' && c == L'}' )
        {
            // great
        }
        else
        {
            throw_invalid( ipos, "closing brace expected" );
        }
    }
};

}} // namespace boost::uuids

#endif // BOOST_UUID_STRING_GENERATOR_HPP_INCLUDED
