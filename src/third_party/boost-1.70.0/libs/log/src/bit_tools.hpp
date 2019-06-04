/*
 *              Copyright Andrey Semashev 2016.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   bit_tools.hpp
 * \author Andrey Semashev
 * \date   16.01.2016
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_BIT_TOOLS_HPP_INCLUDED_
#define BOOST_LOG_BIT_TOOLS_HPP_INCLUDED_

#include <boost/log/detail/config.hpp>
#include <cstddef>
#include <boost/cstdint.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

//! Checks that the integer is a power of 2.
template< typename T >
inline BOOST_CONSTEXPR bool is_power_of_2(T n) BOOST_NOEXCEPT
{
    return n != (T)0 && (n & (n - (T)1)) == (T)0;
}

//! Returns an integer comprising the four characters
inline BOOST_CONSTEXPR uint32_t make_fourcc(unsigned char c1, unsigned char c2, unsigned char c3, unsigned char c4) BOOST_NOEXCEPT
{
    return (static_cast< uint32_t >(c1) << 24) | (static_cast< uint32_t >(c2) << 16) | (static_cast< uint32_t >(c3) << 8) | static_cast< uint32_t >(c4);
}

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_BIT_TOOLS_HPP_INCLUDED_
