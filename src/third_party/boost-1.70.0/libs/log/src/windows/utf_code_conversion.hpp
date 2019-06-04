/*
 *              Copyright Andrey Semashev 2016.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   windows/utf_code_conversion.hpp
 * \author Andrey Semashev
 * \date   22.02.2016
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_WINDOWS_UTF_CODE_CONVERSION_HPP_INCLUDED_
#define BOOST_LOG_WINDOWS_UTF_CODE_CONVERSION_HPP_INCLUDED_

#include <boost/log/detail/config.hpp>
#include <string>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

//! Converts UTF-8 to UTF-16
std::wstring utf8_to_utf16(const char* str);
//! Converts UTF-16 to UTF-8
std::string utf16_to_utf8(const wchar_t* str);

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_WINDOWS_UTF_CODE_CONVERSION_HPP_INCLUDED_
