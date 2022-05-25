//
// connect_pipe.hpp
// ~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2022 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_CONNECT_PIPE_HPP
#define BOOST_ASIO_CONNECT_PIPE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#if defined(BOOST_ASIO_HAS_PIPE) \
  || defined(GENERATING_DOCUMENTATION)

#include <boost/asio/basic_readable_pipe.hpp>
#include <boost/asio/basic_writable_pipe.hpp>
#include <boost/asio/error.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace detail {

#if defined(BOOST_ASIO_HAS_IOCP)
typedef HANDLE native_pipe_handle;
#else // defined(BOOST_ASIO_HAS_IOCP)
typedef int native_pipe_handle;
#endif // defined(BOOST_ASIO_HAS_IOCP)

BOOST_ASIO_DECL void create_pipe(native_pipe_handle p[2],
    boost::system::error_code& ec);

BOOST_ASIO_DECL void close_pipe(native_pipe_handle p);

} // namespace detail

/// Connect two pipe ends using an anonymous pipe.
/**
 * @param read_end The read end of the pipe.
 *
 * @param write_end The write end of the pipe.
 *
 * @throws boost::system::system_error Thrown on failure.
 */
template <typename Executor1, typename Executor2>
void connect_pipe(basic_readable_pipe<Executor1>& read_end,
    basic_writable_pipe<Executor2>& write_end);

/// Connect two pipe ends using an anonymous pipe.
/**
 * @param read_end The read end of the pipe.
 *
 * @param write_end The write end of the pipe.
 *
 * @throws boost::system::system_error Thrown on failure.
 *
 * @param ec Set to indicate what error occurred, if any.
 */
template <typename Executor1, typename Executor2>
BOOST_ASIO_SYNC_OP_VOID connect_pipe(basic_readable_pipe<Executor1>& read_end,
    basic_writable_pipe<Executor2>& write_end, boost::system::error_code& ec);

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#include <boost/asio/impl/connect_pipe.hpp>
#if defined(BOOST_ASIO_HEADER_ONLY)
# include <boost/asio/impl/connect_pipe.ipp>
#endif // defined(BOOST_ASIO_HEADER_ONLY)

#endif // defined(BOOST_ASIO_HAS_PIPE)
       //   || defined(GENERATING_DOCUMENTATION)

#endif // BOOST_ASIO_CONNECT_PIPE_HPP
