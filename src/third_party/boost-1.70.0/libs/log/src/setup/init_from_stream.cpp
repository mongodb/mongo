/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   init_from_stream.cpp
 * \author Andrey Semashev
 * \date   22.03.2008
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_WITHOUT_SETTINGS_PARSERS

#include <boost/log/detail/setup_config.hpp>
#include <boost/log/utility/setup/from_settings.hpp>
#include <boost/log/utility/setup/settings_parser.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

//! The function initializes the logging library from a stream containing logging settings
template< typename CharT >
BOOST_LOG_SETUP_API void init_from_stream(std::basic_istream< CharT >& strm)
{
    init_from_settings(parse_settings(strm));
}

#ifdef BOOST_LOG_USE_CHAR
template BOOST_LOG_SETUP_API void init_from_stream< char >(std::basic_istream< char >& strm);
#endif
#ifdef BOOST_LOG_USE_WCHAR_T
template BOOST_LOG_SETUP_API void init_from_stream< wchar_t >(std::basic_istream< wchar_t >& strm);
#endif

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_WITHOUT_SETTINGS_PARSERS
