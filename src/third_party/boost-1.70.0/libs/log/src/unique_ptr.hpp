/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   unique_ptr.hpp
 * \author Andrey Semashev
 * \date   18.07.2015
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_UNIQUE_PTR_HPP_INCLUDED_
#define BOOST_LOG_UNIQUE_PTR_HPP_INCLUDED_

#include <boost/log/detail/config.hpp>

#if !defined(BOOST_NO_CXX11_SMART_PTR)

#include <memory>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

using std::unique_ptr;

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#else // !defined(BOOST_NO_CXX11_SMART_PTR)

#include <boost/move/unique_ptr.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

using boost::movelib::unique_ptr;

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#endif // !defined(BOOST_NO_CXX11_SMART_PTR)

#endif // BOOST_LOG_UNIQUE_PTR_HPP_INCLUDED_
