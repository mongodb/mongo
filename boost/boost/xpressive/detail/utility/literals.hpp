///////////////////////////////////////////////////////////////////////////////
// literals.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_UTILITY_LITERALS_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_UTILITY_LITERALS_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/config.hpp> // for BOOST_STATIC_CONSTANT
#include <boost/detail/workaround.hpp>

namespace boost { namespace xpressive { namespace detail
{

///////////////////////////////////////////////////////////////////////////////
// char_literal
//
template<typename Char, char Ch, wchar_t Wch>
struct char_literal;

template<char Ch, wchar_t Wch>
struct char_literal<char, Ch, Wch>
{
    BOOST_STATIC_CONSTANT(char, value = Ch);
};

template<char Ch, wchar_t Wch>
struct char_literal<wchar_t, Ch, Wch>
{
    BOOST_STATIC_CONSTANT(wchar_t, value = Wch);
};

#ifndef BOOST_NO_INCLASS_MEMBER_INITIALIZATION
template<char Ch, wchar_t Wch>
char const char_literal<char, Ch, Wch>::value;

template<char Ch, wchar_t Wch>
wchar_t const char_literal<wchar_t, Ch, Wch>::value;
#endif

template<typename Ch>
struct string_literal;

template<>
struct string_literal<char>
{
    static char const *pick(char const *cstr, wchar_t const *)
    {
        return cstr;
    }

    static char pick(char ch, wchar_t)
    {
        return ch;
    }
};

template<>
struct string_literal<wchar_t>
{
    static wchar_t const *pick(char const *, wchar_t const *cstr)
    {
        return cstr;
    }

    static wchar_t pick(char, wchar_t ch)
    {
        return ch;
    }
};

#if BOOST_WORKAROUND(__MWERKS__, BOOST_TESTED_AT(0x3206))

# define BOOST_XPR_CHAR_(Char, ch) ch
# define BOOST_XPR_CSTR_(Char, st) boost::xpressive::detail::string_literal<Char>::pick(st, L##st)

#else

# define BOOST_XPR_CHAR_(Char, ch) boost::xpressive::detail::char_literal<Char, ch, L##ch>::value
# define BOOST_XPR_CSTR_(Char, st) boost::xpressive::detail::string_literal<Char>::pick(st, L##st)

#endif

}}} // namespace boost::xpressive::detail

#endif
