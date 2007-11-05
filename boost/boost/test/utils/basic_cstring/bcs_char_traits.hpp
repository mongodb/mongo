//  (C) Copyright Gennadiy Rozental 2004-2005.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile: bcs_char_traits.hpp,v $
//
//  Version     : $Revision: 1.3 $
//
//  Description : generic char traits class; wraps std::char_traits
// ***************************************************************************

#ifndef BOOST_TEST_BCS_CHAR_TRAITS_HPP_071894GER
#define BOOST_TEST_BCS_CHAR_TRAITS_HPP_071894GER

// Boost
#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>
#include <boost/test/detail/config.hpp>
#include <boost/type_traits/add_const.hpp>

// STL
#include <string>                       // std::char_traits
#include <cstddef>                      // std::size_t

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

namespace ut_detail {

template<typename CharT> struct bcs_base_char           { typedef CharT type; };

template<> struct bcs_base_char<char const>             { typedef char type; };
template<> struct bcs_base_char<unsigned char>          { typedef char type; };
#if !BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x551))
template<> struct bcs_base_char<unsigned char const>    { typedef char type; };
#endif

template<> struct bcs_base_char<wchar_t const>          { typedef wchar_t type; };

// ************************************************************************** //
// **************               bcs_char_traits                ************** //
// ************************************************************************** //

template<typename CharT>
struct bcs_char_traits_impl
{
#if BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))
    typedef CharT const const_char;
#else
    typedef typename boost::add_const<CharT>::type const_char;
#endif
    static bool eq( CharT c1, CharT c2 )
    {
        return c1 == c2;
    }
    static bool lt( CharT c1, CharT c2 )
    {
        return c1 < c2;
    }

    static int compare( const_char* cstr1, const_char* cstr2, std::size_t n )
    {
        while( n > 0 ) {
            if( !eq( *cstr1, *cstr2 ) )
                return lt( *cstr1, *cstr2 ) ? -1 : 1;
            ++cstr1;
            ++cstr2;
            --n;
        }

        return 0;
    }

    static std::size_t length( const_char* cstr )
    {
        const_char null_char = CharT();

        const_char* ptr = cstr;
        while( !eq( *ptr, null_char ) )
            ++ptr;

        return ptr - cstr;
    }

    static const_char* find( const_char* s, std::size_t n, CharT c )
    {
        while( n > 0 ) {
            if( eq( *s, c ) )
                return s;

            ++s;
            --n;
        }
        return 0;
    }
};

#ifdef BOOST_CLASSIC_IOSTREAMS
template<typename CharT>
struct char_traits_with_find : std::string_char_traits<CharT> {
    static CharT const* find( CharT const* s, std::size_t n, CharT c )
    {
        while( n > 0 ) {
            if( eq( *s, c ) )
                return s;

            ++s;
            --n;
        }
        return 0;
    }
};

template<> struct bcs_char_traits_impl<char> : char_traits_with_find<char> {};
template<> struct bcs_char_traits_impl<wchar_t> : char_traits_with_find<wchar_t> {};
#else
template<> struct bcs_char_traits_impl<char> : std::char_traits<char> {};
template<> struct bcs_char_traits_impl<wchar_t> : std::char_traits<wchar_t> {};
#endif

template<typename CharT>
class bcs_char_traits : public bcs_char_traits_impl<CharT> {
    typedef typename ut_detail::bcs_base_char<CharT>::type                              the_base_char;
public:
#ifdef BOOST_CLASSIC_IOSTREAMS
    typedef std::basic_string<the_base_char, std::string_char_traits<the_base_char> >   std_string;
#else
    typedef std::basic_string<the_base_char, std::char_traits<the_base_char> >          std_string;
#endif
};

} // namespace ut_detail

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

// ***************************************************************************
//  Revision History :
//  
//  $Log: bcs_char_traits.hpp,v $
//  Revision 1.3  2005/02/20 08:27:09  rogeeff
//  This a major update for Boost.Test framework. See release docs for complete list of fixes/updates
//
//  Revision 1.2  2005/02/01 06:40:08  rogeeff
//  copyright update
//  old log entries removed
//  minor stilistic changes
//  depricated tools removed
//
//  Revision 1.1  2005/01/22 18:21:40  rogeeff
//  moved sharable staff into utils
//
// ***************************************************************************

#endif // BOOST_TEST_BCS_CHAR_TRAITS_HPP_071894GER
