//
// placeholders.hpp
// ~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_PLACEHOLDERS_HPP
#define BOOST_ASIO_PLACEHOLDERS_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/detail/functional.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace placeholders {

#if defined(GENERATING_DOCUMENTATION)

/// An argument placeholder, for use with std::bind() or boost::bind(), that
/// corresponds to the error argument of a handler for any of the asynchronous
/// functions.
unspecified error;

/// An argument placeholder, for use with std::bind() or boost::bind(), that
/// corresponds to the bytes_transferred argument of a handler for asynchronous
/// functions such as boost::asio::basic_stream_socket::async_write_some or
/// boost::asio::async_write.
unspecified bytes_transferred;

/// An argument placeholder, for use with std::bind() or boost::bind(), that
/// corresponds to the iterator argument of a handler for asynchronous functions
/// such as boost::asio::async_connect.
unspecified iterator;

/// An argument placeholder, for use with std::bind() or boost::bind(), that
/// corresponds to the results argument of a handler for asynchronous functions
/// such as boost::asio::basic_resolver::async_resolve.
unspecified results;

/// An argument placeholder, for use with std::bind() or boost::bind(), that
/// corresponds to the results argument of a handler for asynchronous functions
/// such as boost::asio::async_connect.
unspecified endpoint;

/// An argument placeholder, for use with std::bind() or boost::bind(), that
/// corresponds to the signal_number argument of a handler for asynchronous
/// functions such as boost::asio::signal_set::async_wait.
unspecified signal_number;

#else

static BOOST_ASIO_INLINE_VARIABLE constexpr auto& error
  = std::placeholders::_1;
static BOOST_ASIO_INLINE_VARIABLE constexpr auto& bytes_transferred
  = std::placeholders::_2;
static BOOST_ASIO_INLINE_VARIABLE constexpr auto& iterator
  = std::placeholders::_2;
static BOOST_ASIO_INLINE_VARIABLE constexpr auto& results
  = std::placeholders::_2;
static BOOST_ASIO_INLINE_VARIABLE constexpr auto& endpoint
  = std::placeholders::_2;
static BOOST_ASIO_INLINE_VARIABLE constexpr auto& signal_number
  = std::placeholders::_2;

#endif

} // namespace placeholders
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_PLACEHOLDERS_HPP
