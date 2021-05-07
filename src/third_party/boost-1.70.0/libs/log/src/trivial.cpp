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

const unsigned int names_count = 6;

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

BOOST_LOG_API const char* to_string(severity_level lvl)
{
    typedef severity_level_names< char > level_names;
    if (static_cast< unsigned int >(lvl) < names_count)
        return level_names::names[static_cast< unsigned int >(lvl)];
    else
        return NULL;
}

template< typename CharT, typename TraitsT >
BOOST_LOG_API std::basic_istream< CharT, TraitsT >& operator>> (
    std::basic_istream< CharT, TraitsT >& strm, severity_level& lvl)
{
    if (strm.good())
    {
        typedef severity_level_names< CharT > level_names;
        typedef std::basic_string< CharT, TraitsT > string_type;
        string_type str;
        strm >> str;
        for (unsigned int i = 0; i < names_count; ++i)
        {
            if (str == level_names::names[i])
            {
                lvl = static_cast< severity_level >(i);
                return strm;
            }
        }
        strm.setstate(std::ios_base::failbit);
    }

    return strm;
}

template BOOST_LOG_API std::basic_istream< char, std::char_traits< char > >&
    operator>> < char, std::char_traits< char > > (
        std::basic_istream< char, std::char_traits< char > >& strm, severity_level& lvl);
#ifdef BOOST_LOG_USE_WCHAR_T
template BOOST_LOG_API std::basic_istream< wchar_t, std::char_traits< wchar_t > >&
    operator>> < wchar_t, std::char_traits< wchar_t > > (
        std::basic_istream< wchar_t, std::char_traits< wchar_t > >& strm, severity_level& lvl);
#endif

} // namespace trivial

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // defined(BOOST_LOG_USE_CHAR)
