/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   id_formatting.hpp
 * \author Andrey Semashev
 * \date   25.01.2015
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_ID_FORMATTING_HPP_INCLUDED_
#define BOOST_LOG_ID_FORMATTING_HPP_INCLUDED_

#include <boost/log/detail/config.hpp>
#include <cstddef>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

// Defined in dump.cpp
extern const char g_hex_char_table[2][16];

template< std::size_t IdSize, typename CharT, typename IdT >
inline void format_id(CharT* buf, std::size_t size, IdT id, bool uppercase) BOOST_NOEXCEPT
{
    const char* const char_table = g_hex_char_table[uppercase];

    // Input buffer is assumed to be always larger than 2 chars
    *buf++ = static_cast< CharT >(char_table[0]);  // '0'
    *buf++ = static_cast< CharT >(char_table[10] + ('x' - 'a')); // 'x'

    size -= 3; // reserve space for the terminating 0
    std::size_t i = 0;
    const std::size_t n = (size > (IdSize * 2u)) ? IdSize * 2u : size;
    for (std::size_t shift = n * 4u - 4u; i < n; ++i, shift -= 4u)
    {
        buf[i] = static_cast< CharT >(char_table[(id >> shift) & 15u]);
    }

    buf[i] = static_cast< CharT >('\0');
}

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_ID_FORMATTING_HPP_INCLUDED_

