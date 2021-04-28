/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   trivial.cpp
 * \author Andrey Semashev
 * \date   07.11.2009
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>

#if defined(BOOST_LOG_USE_CHAR)

#include <string>
#include <istream>
#include <boost/log/trivial.hpp>
#include <boost/log/sources/global_logger_storage.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace trivial {

//! Initialization routine
BOOST_LOG_API logger::logger_type logger::construct_logger()
{
    return logger_type(keywords::severity = info);
}

//! Returns a reference to the trivial logger instance
BOOST_LOG_API logger::logger_type& logger::get()
{
    return log::sources::aux::logger_singleton< logger >::get();
}

BOOST_LOG_ANONYMOUS_NAMESPACE {

BOOST_CONSTEXPR_OR_CONST unsigned int names_count = 6;

template< typename CharT >
struct severity_level_names
{
    static const CharT names[names_count][8];
};

template< typename CharT >
const CharT severity_level_names< CharT >::names[names_count][8] =
{
    { 't', 'r', 'a', 'c', 'e', 0 },
    { 'd', 'e', 'b', 'u', 'g', 0 },
    { 'i', 'n', 'f', 'o', 0 },
    { 'w', 'a', 'r', 'n', 'i', 'n', 'g', 0 },
    { 'e', 'r', 'r', 'o', 'r', 0 },
    { 'f', 'a', 't', 'a', 'l', 0 }
};

} // namespace

template< typename CharT >
BOOST_LOG_API const CharT* to_string(severity_level lvl)
{
    typedef severity_level_names< CharT > level_names;
    if (BOOST_LIKELY(static_cast< unsigned int >(lvl) < names_count))
        return level_names::names[static_cast< unsigned int >(lvl)];
    return NULL;
}

//! Parses enumeration value from string and returns \c true on success and \c false otherwise
template< typename CharT >
BOOST_LOG_API bool from_string(const CharT* str, std::size_t len, severity_level& lvl)
{
    typedef severity_level_names< CharT > level_names;
    typedef std::char_traits< CharT > char_traits;

    if (len == 5u)
    {
        if (char_traits::compare(str, level_names::names[0], len) == 0)
            lvl = static_cast< severity_level >(0);
        else if (char_traits::compare(str, level_names::names[1], len) == 0)
            lvl = static_cast< severity_level >(1);
        else if (char_traits::compare(str, level_names::names[4], len) == 0)
            lvl = static_cast< severity_level >(4);
        else if (char_traits::compare(str, level_names::names[5], len) == 0)
            lvl = static_cast< severity_level >(5);
        else
            goto no_match;
        return true;
    }
    else if (len == 4u)
    {
        if (char_traits::compare(str, level_names::names[2], len) == 0)
            lvl = static_cast< severity_level >(2);
        else
            goto no_match;
        return true;
    }
    else if (len == 7u)
    {
        if (char_traits::compare(str, level_names::names[3], len) == 0)
            lvl = static_cast< severity_level >(3);
        else
            goto no_match;
        return true;
    }

no_match:
    return false;
}

template< typename CharT, typename TraitsT >
BOOST_LOG_API std::basic_istream< CharT, TraitsT >& operator>> (
    std::basic_istream< CharT, TraitsT >& strm, severity_level& lvl)
{
    if (BOOST_LIKELY(strm.good()))
    {
        typedef std::basic_string< CharT, TraitsT > string_type;
        string_type str;
        strm >> str;
        if (BOOST_UNLIKELY(!boost::log::trivial::from_string(str.data(), str.size(), lvl)))
            strm.setstate(std::ios_base::failbit);
    }

    return strm;
}

template BOOST_LOG_API const char* to_string< char >(severity_level lvl);
template BOOST_LOG_API bool from_string< char >(const char* begin, std::size_t len, severity_level& lvl);
template BOOST_LOG_API std::basic_istream< char, std::char_traits< char > >&
    operator>> < char, std::char_traits< char > > (
        std::basic_istream< char, std::char_traits< char > >& strm, severity_level& lvl);
#ifdef BOOST_LOG_USE_WCHAR_T
template BOOST_LOG_API const wchar_t* to_string< wchar_t >(severity_level lvl);
template BOOST_LOG_API bool from_string< wchar_t >(const wchar_t* begin, std::size_t len, severity_level& lvl);
template BOOST_LOG_API std::basic_istream< wchar_t, std::char_traits< wchar_t > >&
    operator>> < wchar_t, std::char_traits< wchar_t > > (
        std::basic_istream< wchar_t, std::char_traits< wchar_t > >& strm, severity_level& lvl);
#endif

} // namespace trivial

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // defined(BOOST_LOG_USE_CHAR)
