//  ratio_io
//
//  (C) Copyright Howard Hinnant
//  (C) Copyright 2010 Vicente J. Botet Escriba
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
// This code was adapted by Vicente from Howard Hinnant's experimental work
// on chrono i/o under lvm/libc++ to Boost

#ifndef BOOST_RATIO_RATIO_IO_HPP
#define BOOST_RATIO_RATIO_IO_HPP

/*

    ratio_io synopsis

#include <ratio>
#include <string>

namespace boost
{

template <class Ratio, class CharT>
struct ratio_string
{
    static basic_string<CharT> prefix();
    static basic_string<CharT> symbol();
};

}  // boost

*/

#include <boost/ratio/ratio.hpp>
#include <string>
#include <sstream>

namespace boost {

template <class Ratio, class CharT>
struct ratio_string
{
    static std::basic_string<CharT> symbol() {return prefix();}
    static std::basic_string<CharT> prefix();
};

template <class Ratio, class CharT>
std::basic_string<CharT>
ratio_string<Ratio, CharT>::prefix()
{
    std::basic_ostringstream<CharT> os;
    os << CharT('[') << Ratio::num << CharT('/')
                        << Ratio::den << CharT(']');
    return os.str();
}

// atto

template <>
struct ratio_string<atto, char>
{
    static std::string symbol() {return std::string(1, 'a');}
    static std::string prefix()  {return std::string("atto");}
};

template <>
struct ratio_string<atto, char16_t>
{
    static std::u16string symbol() {return std::u16string(1, u'a');}
    static std::u16string prefix()  {return std::u16string(u"atto");}
};

template <>
struct ratio_string<atto, char32_t>
{
    static std::u32string symbol() {return std::u32string(1, U'a');}
    static std::u32string prefix()  {return std::u32string(U"atto");}
};

template <>
struct ratio_string<atto, wchar_t>
{
    static std::wstring symbol() {return std::wstring(1, L'a');}
    static std::wstring prefix()  {return std::wstring(L"atto");}
};

// femto

template <>
struct ratio_string<femto, char>
{
    static std::string symbol() {return std::string(1, 'f');}
    static std::string prefix()  {return std::string("femto");}
};

template <>
struct ratio_string<femto, char16_t>
{
    static std::u16string symbol() {return std::u16string(1, u'f');}
    static std::u16string prefix()  {return std::u16string(u"femto");}
};

template <>
struct ratio_string<femto, char32_t>
{
    static std::u32string symbol() {return std::u32string(1, U'f');}
    static std::u32string prefix()  {return std::u32string(U"femto");}
};

template <>
struct ratio_string<femto, wchar_t>
{
    static std::wstring symbol() {return std::wstring(1, L'f');}
    static std::wstring prefix()  {return std::wstring(L"femto");}
};

// pico

template <>
struct ratio_string<pico, char>
{
    static std::string symbol() {return std::string(1, 'p');}
    static std::string prefix()  {return std::string("pico");}
};

template <>
struct ratio_string<pico, char16_t>
{
    static std::u16string symbol() {return std::u16string(1, u'p');}
    static std::u16string prefix()  {return std::u16string(u"pico");}
};

template <>
struct ratio_string<pico, char32_t>
{
    static std::u32string symbol() {return std::u32string(1, U'p');}
    static std::u32string prefix()  {return std::u32string(U"pico");}
};

template <>
struct ratio_string<pico, wchar_t>
{
    static std::wstring symbol() {return std::wstring(1, L'p');}
    static std::wstring prefix()  {return std::wstring(L"pico");}
};

// nano

template <>
struct ratio_string<nano, char>
{
    static std::string symbol() {return std::string(1, 'n');}
    static std::string prefix()  {return std::string("nano");}
};

template <>
struct ratio_string<nano, char16_t>
{
    static std::u16string symbol() {return std::u16string(1, u'n');}
    static std::u16string prefix()  {return std::u16string(u"nano");}
};

template <>
struct ratio_string<nano, char32_t>
{
    static std::u32string symbol() {return std::u32string(1, U'n');}
    static std::u32string prefix()  {return std::u32string(U"nano");}
};

template <>
struct ratio_string<nano, wchar_t>
{
    static std::wstring symbol() {return std::wstring(1, L'n');}
    static std::wstring prefix()  {return std::wstring(L"nano");}
};

// micro

template <>
struct ratio_string<micro, char>
{
    static std::string symbol() {return std::string("\xC2\xB5");}
    static std::string prefix()  {return std::string("micro");}
};

template <>
struct ratio_string<micro, char16_t>
{
    static std::u16string symbol() {return std::u16string(1, u'\xB5');}
    static std::u16string prefix()  {return std::u16string(u"micro");}
};

template <>
struct ratio_string<micro, char32_t>
{
    static std::u32string symbol() {return std::u32string(1, U'\xB5');}
    static std::u32string prefix()  {return std::u32string(U"micro");}
};

template <>
struct ratio_string<micro, wchar_t>
{
    static std::wstring symbol() {return std::wstring(1, L'\xB5');}
    static std::wstring prefix()  {return std::wstring(L"micro");}
};

// milli

template <>
struct ratio_string<milli, char>
{
    static std::string symbol() {return std::string(1, 'm');}
    static std::string prefix()  {return std::string("milli");}
};

template <>
struct ratio_string<milli, char16_t>
{
    static std::u16string symbol() {return std::u16string(1, u'm');}
    static std::u16string prefix()  {return std::u16string(u"milli");}
};

template <>
struct ratio_string<milli, char32_t>
{
    static std::u32string symbol() {return std::u32string(1, U'm');}
    static std::u32string prefix()  {return std::u32string(U"milli");}
};

template <>
struct ratio_string<milli, wchar_t>
{
    static std::wstring symbol() {return std::wstring(1, L'm');}
    static std::wstring prefix()  {return std::wstring(L"milli");}
};

// centi

template <>
struct ratio_string<centi, char>
{
    static std::string symbol() {return std::string(1, 'c');}
    static std::string prefix()  {return std::string("centi");}
};

template <>
struct ratio_string<centi, char16_t>
{
    static std::u16string symbol() {return std::u16string(1, u'c');}
    static std::u16string prefix()  {return std::u16string(u"centi");}
};

template <>
struct ratio_string<centi, char32_t>
{
    static std::u32string symbol() {return std::u32string(1, U'c');}
    static std::u32string prefix()  {return std::u32string(U"centi");}
};

template <>
struct ratio_string<centi, wchar_t>
{
    static std::wstring symbol() {return std::wstring(1, L'c');}
    static std::wstring prefix()  {return std::wstring(L"centi");}
};

// deci

template <>
struct ratio_string<deci, char>
{
    static std::string symbol() {return std::string(1, 'd');}
    static std::string prefix()  {return std::string("deci");}
};

template <>
struct ratio_string<deci, char16_t>
{
    static std::u16string symbol() {return std::u16string(1, u'd');}
    static std::u16string prefix()  {return std::u16string(u"deci");}
};

template <>
struct ratio_string<deci, char32_t>
{
    static std::u32string symbol() {return std::u32string(1, U'd');}
    static std::u32string prefix()  {return std::u32string(U"deci");}
};

template <>
struct ratio_string<deci, wchar_t>
{
    static std::wstring symbol() {return std::wstring(1, L'd');}
    static std::wstring prefix()  {return std::wstring(L"deci");}
};

// unit

// deca

template <>
struct ratio_string<deca, char>
{
    static std::string symbol() {return std::string("da");}
    static std::string prefix()  {return std::string("deca");}
};

template <>
struct ratio_string<deca, char16_t>
{
    static std::u16string symbol() {return std::u16string(u"da");}
    static std::u16string prefix()  {return std::u16string(u"deca");}
};

template <>
struct ratio_string<deca, char32_t>
{
    static std::u32string symbol() {return std::u32string(U"da");}
    static std::u32string prefix()  {return std::u32string(U"deca");}
};

template <>
struct ratio_string<deca, wchar_t>
{
    static std::wstring symbol() {return std::wstring(L"da");}
    static std::wstring prefix()  {return std::wstring(L"deca");}
};

// hecto

template <>
struct ratio_string<hecto, char>
{
    static std::string symbol() {return std::string(1, 'h');}
    static std::string prefix()  {return std::string("hecto");}
};

template <>
struct ratio_string<hecto, char16_t>
{
    static std::u16string symbol() {return std::u16string(1, u'h');}
    static std::u16string prefix()  {return std::u16string(u"hecto");}
};

template <>
struct ratio_string<hecto, char32_t>
{
    static std::u32string symbol() {return std::u32string(1, U'h');}
    static std::u32string prefix()  {return std::u32string(U"hecto");}
};

template <>
struct ratio_string<hecto, wchar_t>
{
    static std::wstring symbol() {return std::wstring(1, L'h');}
    static std::wstring prefix()  {return std::wstring(L"hecto");}
};

// kilo

template <>
struct ratio_string<kilo, char>
{
    static std::string symbol() {return std::string(1, 'k');}
    static std::string prefix()  {return std::string("kilo");}
};

template <>
struct ratio_string<kilo, char16_t>
{
    static std::u16string symbol() {return std::u16string(1, u'k');}
    static std::u16string prefix()  {return std::u16string(u"kilo");}
};

template <>
struct ratio_string<kilo, char32_t>
{
    static std::u32string symbol() {return std::u32string(1, U'k');}
    static std::u32string prefix()  {return std::u32string(U"kilo");}
};

template <>
struct ratio_string<kilo, wchar_t>
{
    static std::wstring symbol() {return std::wstring(1, L'k');}
    static std::wstring prefix()  {return std::wstring(L"kilo");}
};

// mega

template <>
struct ratio_string<mega, char>
{
    static std::string symbol() {return std::string(1, 'M');}
    static std::string prefix()  {return std::string("mega");}
};

template <>
struct ratio_string<mega, char16_t>
{
    static std::u16string symbol() {return std::u16string(1, u'M');}
    static std::u16string prefix()  {return std::u16string(u"mega");}
};

template <>
struct ratio_string<mega, char32_t>
{
    static std::u32string symbol() {return std::u32string(1, U'M');}
    static std::u32string prefix()  {return std::u32string(U"mega");}
};

template <>
struct ratio_string<mega, wchar_t>
{
    static std::wstring symbol() {return std::wstring(1, L'M');}
    static std::wstring prefix()  {return std::wstring(L"mega");}
};

// giga

template <>
struct ratio_string<giga, char>
{
    static std::string symbol() {return std::string(1, 'G');}
    static std::string prefix()  {return std::string("giga");}
};

template <>
struct ratio_string<giga, char16_t>
{
    static std::u16string symbol() {return std::u16string(1, u'G');}
    static std::u16string prefix()  {return std::u16string(u"giga");}
};

template <>
struct ratio_string<giga, char32_t>
{
    static std::u32string symbol() {return std::u32string(1, U'G');}
    static std::u32string prefix()  {return std::u32string(U"giga");}
};

template <>
struct ratio_string<giga, wchar_t>
{
    static std::wstring symbol() {return std::wstring(1, L'G');}
    static std::wstring prefix()  {return std::wstring(L"giga");}
};

// tera

template <>
struct ratio_string<tera, char>
{
    static std::string symbol() {return std::string(1, 'T');}
    static std::string prefix()  {return std::string("tera");}
};

template <>
struct ratio_string<tera, char16_t>
{
    static std::u16string symbol() {return std::u16string(1, u'T');}
    static std::u16string prefix()  {return std::u16string(u"tera");}
};

template <>
struct ratio_string<tera, char32_t>
{
    static std::u32string symbol() {return std::u32string(1, U'T');}
    static std::u32string prefix()  {return std::u32string(U"tera");}
};

template <>
struct ratio_string<tera, wchar_t>
{
    static std::wstring symbol() {return std::wstring(1, L'T');}
    static std::wstring prefix()  {return std::wstring(L"tera");}
};

// peta

template <>
struct ratio_string<peta, char>
{
    static std::string symbol() {return std::string(1, 'P');}
    static std::string prefix()  {return std::string("peta");}
};

template <>
struct ratio_string<peta, char16_t>
{
    static std::u16string symbol() {return std::u16string(1, u'P');}
    static std::u16string prefix()  {return std::u16string(u"peta");}
};

template <>
struct ratio_string<peta, char32_t>
{
    static std::u32string symbol() {return std::u32string(1, U'P');}
    static std::u32string prefix()  {return std::u32string(U"peta");}
};

template <>
struct ratio_string<peta, wchar_t>
{
    static std::wstring symbol() {return std::wstring(1, L'P');}
    static std::wstring prefix()  {return std::wstring(L"peta");}
};

// exa

template <>
struct ratio_string<exa, char>
{
    static std::string symbol() {return std::string(1, 'E');}
    static std::string prefix()  {return std::string("exa");}
};

template <>
struct ratio_string<exa, char16_t>
{
    static std::u16string symbol() {return std::u16string(1, u'E');}
    static std::u16string prefix()  {return std::u16string(u"exa");}
};

template <>
struct ratio_string<exa, char32_t>
{
    static std::u32string symbol() {return std::u32string(1, U'E');}
    static std::u32string prefix()  {return std::u32string(U"exa");}
};

template <>
struct ratio_string<exa, wchar_t>
{
    static std::wstring symbol() {return std::wstring(1, L'E');}
    static std::wstring prefix()  {return std::wstring(L"exa");}
};

}

#endif  // BOOST_RATIO_RATIO_IO_HPP
