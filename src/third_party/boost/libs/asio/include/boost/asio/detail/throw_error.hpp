//
// detail/throw_error.hpp
// ~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_DETAIL_THROW_ERROR_HPP
#define BOOST_ASIO_DETAIL_THROW_ERROR_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/detail/throw_exception.hpp>
#include <boost/system/error_code.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace detail {

BOOST_ASIO_DECL void do_throw_error(
    const boost::system::error_code& err
    BOOST_ASIO_SOURCE_LOCATION_PARAM);

BOOST_ASIO_DECL void do_throw_error(
    const boost::system::error_code& err,
    const char* location
    BOOST_ASIO_SOURCE_LOCATION_PARAM);

inline void throw_error(
    const boost::system::error_code& err
    BOOST_ASIO_SOURCE_LOCATION_DEFAULTED_PARAM)
{
  if (err)
    do_throw_error(err BOOST_ASIO_SOURCE_LOCATION_ARG);
}

inline void throw_error(
    const boost::system::error_code& err,
    const char* location
    BOOST_ASIO_SOURCE_LOCATION_DEFAULTED_PARAM)
{
  if (err)
    do_throw_error(err, location BOOST_ASIO_SOURCE_LOCATION_ARG);
}

} // namespace detail
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#if defined(BOOST_ASIO_HEADER_ONLY)
# include <boost/asio/detail/impl/throw_error.ipp>
#endif // defined(BOOST_ASIO_HEADER_ONLY)

#endif // BOOST_ASIO_DETAIL_THROW_ERROR_HPP
