//  ratio_io
//
//  (C) Copyright 2010 Vicente J. Botet Escriba
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
// This code was adapted by Vicente from Howard Hinnant's experimental work
// on chrono i/o under lvm/libc++ to Boost

#ifndef BOOST_RATIO_RATIO_STATIC_STRING_HPP
#define BOOST_RATIO_RATIO_STATIC_STRING_HPP

/*

    ratio_static_string synopsis

#include <ratio>
#include <string>

namespace boost
{

template <class Ratio, class CharT>
struct ratio_static_string
{
    typedef basic_str<CharT, ...> short_name;
    typedef basic_str<CharT, ...> long_name;
};

}  // boost

*/

#include <boost/config.hpp>
#include <boost/ratio/ratio.hpp>
#include <boost/static_string/basic_str.hpp>
//#include <sstream>


#if defined(BOOST_NO_CXX11_UNICODE_LITERALS) || defined(BOOST_NO_CXX11_CHAR16_T) || defined(BOOST_NO_CXX11_CHAR32_T)
//~ #define BOOST_RATIO_HAS_UNICODE_SUPPORT
#else
#define BOOST_RATIO_HAS_UNICODE_SUPPORT 1
#endif

namespace boost {

template <class Ratio, class CharT>
struct ratio_static_string;


// atto

template <>
struct ratio_static_string<atto, char>
{
    typedef static_string::str_1<'a'>::type short_name;
    typedef static_string::str_4<'a','t','t','o'>::type long_name;
};

#ifdef BOOST_RATIO_HAS_UNICODE_SUPPORT

template <>
struct ratio_static_string<atto, char16_t>
{
    typedef static_string::u16str_1<u'a'>::type short_name;
    typedef static_string::u16str_4<u'a',u't',u't',u'o'>::type long_name;
};

template <>
struct ratio_static_string<atto, char32_t>
{
    typedef static_string::u32str_1<U'a'>::type short_name;
    typedef static_string::u32str_4<U'a',U't',U't',U'o'>::type long_name;
};

#endif

#ifndef BOOST_NO_STD_WSTRING
template <>
struct ratio_static_string<atto, wchar_t>
{
    typedef static_string::wstr_1<L'a'>::type short_name;
    typedef static_string::wstr_4<L'a',L't',L't',L'o'>::type long_name;
};
#endif

// femto

template <>
struct ratio_static_string<femto, char>
{
    typedef static_string::str_1<'f'>::type short_name;
    typedef static_string::str_5<'f','e','m','t','o'>::type long_name;
};

#ifdef BOOST_RATIO_HAS_UNICODE_SUPPORT

template <>
struct ratio_static_string<femto, char16_t>
{
    typedef static_string::u16str_1<u'f'>::type short_name;
    typedef static_string::u16str_5<u'f',u'e',u'm',u't',u'o'>::type long_name;
};

template <>
struct ratio_static_string<femto, char32_t>
{
    typedef static_string::u32str_1<U'f'>::type short_name;
    typedef static_string::u32str_5<U'f',U'e',U'm',U't',U'o'>::type long_name;
};

#endif

#ifndef BOOST_NO_STD_WSTRING
template <>
struct ratio_static_string<femto, wchar_t>
{
    typedef static_string::wstr_1<L'f'>::type short_name;
    typedef static_string::wstr_5<L'f',L'e',L'm',L't',L'o'>::type long_name;
};
#endif

// pico

template <>
struct ratio_static_string<pico, char>
{
    typedef static_string::str_1<'p'>::type short_name;
    typedef static_string::str_4<'p','i','c','o'>::type long_name;
};

#ifdef BOOST_RATIO_HAS_UNICODE_SUPPORT

template <>
struct ratio_static_string<pico, char16_t>
{
    typedef static_string::u16str_1<u'p'>::type short_name;
    typedef static_string::u16str_4<u'p',u'i',u'c',u'o'>::type long_name;
};

template <>
struct ratio_static_string<pico, char32_t>
{
    typedef static_string::u32str_1<U'p'>::type short_name;
    typedef static_string::u32str_4<U'p',U'i',U'c',U'o'>::type long_name;
};

#endif

#ifndef BOOST_NO_STD_WSTRING
template <>
struct ratio_static_string<pico, wchar_t>
{
    typedef static_string::wstr_1<L'p'>::type short_name;
    typedef static_string::wstr_4<L'p',L'i',L'c',L'o'>::type long_name;
};
#endif

// nano

template <>
struct ratio_static_string<nano, char>
{
    typedef static_string::str_1<'n'>::type short_name;
    typedef static_string::str_4<'n','a','n','o'>::type long_name;
};

#ifdef BOOST_RATIO_HAS_UNICODE_SUPPORT

template <>
struct ratio_static_string<nano, char16_t>
{
    typedef static_string::u16str_1<u'n'>::type short_name;
    typedef static_string::u16str_4<u'n',u'a',u'n',u'o'>::type long_name;
};

template <>
struct ratio_static_string<nano, char32_t>
{
    typedef static_string::u32str_1<U'n'>::type short_name;
    typedef static_string::u32str_4<U'n',U'a',U'n',U'o'>::type long_name;
};

#endif

#ifndef BOOST_NO_STD_WSTRING
template <>
struct ratio_static_string<nano, wchar_t>
{
    typedef static_string::wstr_1<L'n'>::type short_name;
    typedef static_string::wstr_4<L'n',L'a',L'n',L'o'>::type long_name;
};
#endif

// micro

template <>
struct ratio_static_string<micro, char>
{
    typedef static_string::str_2<'\xC2','\xB5'>::type short_name;
    typedef static_string::str_5<'m','i','c','r','o'>::type long_name;
};

#ifdef BOOST_RATIO_HAS_UNICODE_SUPPORT

template <>
struct ratio_static_string<micro, char16_t>
{
    typedef static_string::u16str_1<u'\xB5'>::type short_name;
    typedef static_string::u16str_5<u'm',u'i',u'c',u'r',u'o'>::type long_name;
};

template <>
struct ratio_static_string<micro, char32_t>
{
    typedef static_string::u32str_1<U'\xB5'>::type short_name;
    typedef static_string::u32str_5<U'm',U'i',U'c',U'r',U'o'>::type long_name;
};

#endif

#ifndef BOOST_NO_STD_WSTRING
template <>
struct ratio_static_string<micro, wchar_t>
{
    typedef static_string::wstr_1<L'\xB5'>::type short_name;
    typedef static_string::wstr_5<L'm',L'i',L'c',L'r',L'o'>::type long_name;
};
#endif

// milli

template <>
struct ratio_static_string<milli, char>
{
    typedef static_string::str_1<'m'>::type short_name;
    typedef static_string::str_5<'m','i','l','l','i'>::type long_name;
};

#ifdef BOOST_RATIO_HAS_UNICODE_SUPPORT

template <>
struct ratio_static_string<milli, char16_t>
{
    typedef static_string::u16str_1<u'm'>::type short_name;
    typedef static_string::u16str_5<u'm',u'i',u'l',u'l',u'i'>::type long_name;
};

template <>
struct ratio_static_string<milli, char32_t>
{
    typedef static_string::u32str_1<U'm'>::type short_name;
    typedef static_string::u32str_5<U'm',U'i',U'l',U'l',U'i'>::type long_name;
};

#endif

#ifndef BOOST_NO_STD_WSTRING
template <>
struct ratio_static_string<milli, wchar_t>
{
    typedef static_string::wstr_1<L'm'>::type short_name;
    typedef static_string::wstr_5<L'm',L'i',L'l',L'l',L'i'>::type long_name;
};
#endif

// centi

template <>
struct ratio_static_string<centi, char>
{
    typedef static_string::str_1<'c'>::type short_name;
    typedef static_string::str_5<'c','e','n','t','i'>::type long_name;
};

#ifdef BOOST_RATIO_HAS_UNICODE_SUPPORT

template <>
struct ratio_static_string<centi, char16_t>
{
    typedef static_string::u16str_1<u'c'>::type short_name;
    typedef static_string::u16str_5<u'c',u'e',u'n',u't',u'i'>::type long_name;
};

template <>
struct ratio_static_string<centi, char32_t>
{
    typedef static_string::u32str_1<U'c'>::type short_name;
    typedef static_string::u32str_5<U'c',U'e',U'n',U't',U'i'>::type long_name;
};

#endif

#ifndef BOOST_NO_STD_WSTRING
template <>
struct ratio_static_string<centi, wchar_t>
{
    typedef static_string::wstr_1<L'c'>::type short_name;
    typedef static_string::wstr_5<L'c',L'e',L'n',L't',L'i'>::type long_name;
};
#endif

// deci

template <>
struct ratio_static_string<deci, char>
{
    typedef static_string::str_1<'d'>::type short_name;
    typedef static_string::str_4<'d','e','c','i'>::type long_name;
};

#ifdef BOOST_RATIO_HAS_UNICODE_SUPPORT

template <>
struct ratio_static_string<deci, char16_t>
{
    typedef static_string::u16str_1<u'd'>::type short_name;
    typedef static_string::u16str_4<u'd',u'e',u'c',u'i'>::type long_name;
};

template <>
struct ratio_static_string<deci, char32_t>
{
    typedef static_string::u32str_1<U'd'>::type short_name;
    typedef static_string::u32str_4<U'd',U'e',U'c',U'i'>::type long_name;
};

#endif

#ifndef BOOST_NO_STD_WSTRING
template <>
struct ratio_static_string<deci, wchar_t>
{
    typedef static_string::wstr_1<L'd'>::type short_name;
    typedef static_string::wstr_4<L'd',L'e',L'c',L'i'>::type long_name;
};
#endif

// deca

template <>
struct ratio_static_string<deca, char>
{
    typedef static_string::str_2<'d','a'>::type short_name;
    typedef static_string::str_4<'d','e','c','a'>::type long_name;
};

#ifdef BOOST_RATIO_HAS_UNICODE_SUPPORT

template <>
struct ratio_static_string<deca, char16_t>
{
    typedef static_string::u16str_2<u'd',u'a'>::type short_name;
    typedef static_string::u16str_4<u'd',u'e',u'c',u'a'>::type long_name;
};

template <>
struct ratio_static_string<deca, char32_t>
{
    typedef static_string::u32str_2<U'd',U'a'>::type short_name;
    typedef static_string::u32str_4<U'd',U'e',U'c',U'a'>::type long_name;
};

#endif

#ifndef BOOST_NO_STD_WSTRING
template <>
struct ratio_static_string<deca, wchar_t>
{
    typedef static_string::wstr_2<L'd',L'a'>::type short_name;
    typedef static_string::wstr_4<L'd',L'e',L'c',L'a'>::type long_name;
};
#endif

// hecto

template <>
struct ratio_static_string<hecto, char>
{
    typedef static_string::str_1<'h'>::type short_name;
    typedef static_string::str_5<'h','e','c','t','o'>::type long_name;
};

#ifdef BOOST_RATIO_HAS_UNICODE_SUPPORT

template <>
struct ratio_static_string<hecto, char16_t>
{
    typedef static_string::u16str_1<u'h'>::type short_name;
    typedef static_string::u16str_5<u'h',u'e',u'c',u't',u'o'>::type long_name;
};

template <>
struct ratio_static_string<hecto, char32_t>
{
    typedef static_string::u32str_1<U'h'>::type short_name;
    typedef static_string::u32str_5<U'h',U'e',U'c',U't',U'o'>::type long_name;
};

#endif

#ifndef BOOST_NO_STD_WSTRING
template <>
struct ratio_static_string<hecto, wchar_t>
{
    typedef static_string::wstr_1<L'h'>::type short_name;
    typedef static_string::wstr_5<L'h',L'e',L'c',L't',L'o'>::type long_name;
};
#endif

// kilo

template <>
struct ratio_static_string<kilo, char>
{
    typedef static_string::str_1<'k'>::type short_name;
    typedef static_string::str_4<'k','i','l','o'>::type long_name;
};

#ifdef BOOST_RATIO_HAS_UNICODE_SUPPORT

template <>
struct ratio_static_string<kilo, char16_t>
{
    typedef static_string::u16str_1<u'k'>::type short_name;
    typedef static_string::u16str_4<u'k',u'i',u'l',u'o'>::type long_name;
};

template <>
struct ratio_static_string<kilo, char32_t>
{
    typedef static_string::u32str_1<U'k'>::type short_name;
    typedef static_string::u32str_4<U'k',U'i',U'l',U'o'>::type long_name;
};

#endif

#ifndef BOOST_NO_STD_WSTRING
template <>
struct ratio_static_string<kilo, wchar_t>
{
    typedef static_string::wstr_1<L'k'>::type short_name;
    typedef static_string::wstr_4<L'k',L'i',L'l',L'o'>::type long_name;
};
#endif

// mega

template <>
struct ratio_static_string<mega, char>
{
    typedef static_string::str_1<'M'>::type short_name;
    typedef static_string::str_4<'m','e','g','a'>::type long_name;
};

#ifdef BOOST_RATIO_HAS_UNICODE_SUPPORT

template <>
struct ratio_static_string<mega, char16_t>
{
    typedef static_string::u16str_1<u'M'>::type short_name;
    typedef static_string::u16str_4<u'm',u'e',u'g',u'a'>::type long_name;
};

template <>
struct ratio_static_string<mega, char32_t>
{
    typedef static_string::u32str_1<U'M'>::type short_name;
    typedef static_string::u32str_4<U'm',U'e',U'g',U'a'>::type long_name;
};

#endif

#ifndef BOOST_NO_STD_WSTRING
template <>
struct ratio_static_string<mega, wchar_t>
{
    typedef static_string::wstr_1<L'M'>::type short_name;
    typedef static_string::wstr_4<L'm',L'e',L'g',L'a'>::type long_name;
};
#endif

// giga

template <>
struct ratio_static_string<giga, char>
{
    typedef static_string::str_1<'G'>::type short_name;
    typedef static_string::str_4<'g','i','g','a'>::type long_name;
};

#ifdef BOOST_RATIO_HAS_UNICODE_SUPPORT

template <>
struct ratio_static_string<giga, char16_t>
{
    typedef static_string::u16str_1<u'G'>::type short_name;
    typedef static_string::u16str_4<u'g',u'i',u'g',u'a'>::type long_name;
};

template <>
struct ratio_static_string<giga, char32_t>
{
    typedef static_string::u32str_1<U'G'>::type short_name;
    typedef static_string::u32str_4<U'g',U'i',U'g',U'a'>::type long_name;
};

#endif

#ifndef BOOST_NO_STD_WSTRING
template <>
struct ratio_static_string<giga, wchar_t>
{
    typedef static_string::wstr_1<L'G'>::type short_name;
    typedef static_string::wstr_4<L'g',L'i',L'g',L'a'>::type long_name;
};
#endif

// tera

template <>
struct ratio_static_string<tera, char>
{
    typedef static_string::str_1<'T'>::type short_name;
    typedef static_string::str_4<'t','e','r','a'>::type long_name;
};

#ifdef BOOST_RATIO_HAS_UNICODE_SUPPORT

template <>
struct ratio_static_string<tera, char16_t>
{
    typedef static_string::u16str_1<u'T'>::type short_name;
    typedef static_string::u16str_4<u't',u'e',u'r',u'a'>::type long_name;
};

template <>
struct ratio_static_string<tera, char32_t>
{
    typedef static_string::u32str_1<U'T'>::type short_name;
    typedef static_string::u32str_4<U't',U'e',U'r',U'a'>::type long_name;
};

#endif

#ifndef BOOST_NO_STD_WSTRING
template <>
struct ratio_static_string<tera, wchar_t>
{
    typedef static_string::wstr_1<L'T'>::type short_name;
    typedef static_string::wstr_4<L'r',L'e',L'r',L'a'>::type long_name;
};
#endif

// peta

template <>
struct ratio_static_string<peta, char>
{
    typedef static_string::str_1<'P'>::type short_name;
    typedef static_string::str_4<'p','e','t','a'>::type long_name;
};

#ifdef BOOST_RATIO_HAS_UNICODE_SUPPORT

template <>
struct ratio_static_string<peta, char16_t>
{
    typedef static_string::u16str_1<u'P'>::type short_name;
    typedef static_string::u16str_4<u'p',u'e',u't',u'a'>::type long_name;
};

template <>
struct ratio_static_string<peta, char32_t>
{
    typedef static_string::u32str_1<U'P'>::type short_name;
    typedef static_string::u32str_4<U'p',U'e',U't',U'a'>::type long_name;
};

#endif

#ifndef BOOST_NO_STD_WSTRING
template <>
struct ratio_static_string<peta, wchar_t>
{
    typedef static_string::wstr_1<L'P'>::type short_name;
    typedef static_string::wstr_4<L'p',L'e',L't',L'a'>::type long_name;
};
#endif

// exa

template <>
struct ratio_static_string<exa, char>
{
    typedef static_string::str_1<'E'>::type short_name;
    typedef static_string::str_3<'e','x','a'>::type long_name;
};

#ifdef BOOST_RATIO_HAS_UNICODE_SUPPORT

template <>
struct ratio_static_string<exa, char16_t>
{
    typedef static_string::u16str_1<u'E'>::type short_name;
    typedef static_string::u16str_3<u'e',u'x',u'a'>::type long_name;
};

template <>
struct ratio_static_string<exa, char32_t>
{
    typedef static_string::u32str_1<U'E'>::type short_name;
    typedef static_string::u32str_3<U'e',U'x',U'a'>::type long_name;
};

#endif

#ifndef BOOST_NO_STD_WSTRING
template <>
struct ratio_static_string<exa, wchar_t>
{
    typedef static_string::wstr_1<L'E'>::type short_name;
    typedef static_string::wstr_3<L'e',L'x',L'a'>::type long_name;
};
#endif

}

#endif  // BOOST_RATIO_RATIO_STATIC_STRING_HPP
