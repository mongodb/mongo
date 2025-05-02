//
// local/seq_packet_protocol.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_LOCAL_SEQ_PACKET_PROTOCOL_HPP
#define BOOST_ASIO_LOCAL_SEQ_PACKET_PROTOCOL_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS) \
  || defined(GENERATING_DOCUMENTATION)

#include <boost/asio/basic_socket_acceptor.hpp>
#include <boost/asio/basic_seq_packet_socket.hpp>
#include <boost/asio/detail/socket_types.hpp>
#include <boost/asio/local/basic_endpoint.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace local {

/// Encapsulates the flags needed for seq_packet UNIX sockets.
/**
 * The boost::asio::local::seq_packet_protocol class contains flags necessary
 * for sequenced packet UNIX domain sockets.
 *
 * @par Thread Safety
 * @e Distinct @e objects: Safe.@n
 * @e Shared @e objects: Safe.
 *
 * @par Concepts:
 * Protocol.
 */
class seq_packet_protocol
{
public:
  /// Obtain an identifier for the type of the protocol.
  int type() const noexcept
  {
    return SOCK_SEQPACKET;
  }

  /// Obtain an identifier for the protocol.
  int protocol() const noexcept
  {
    return 0;
  }

  /// Obtain an identifier for the protocol family.
  int family() const noexcept
  {
    return AF_UNIX;
  }

  /// The type of a UNIX domain endpoint.
  typedef basic_endpoint<seq_packet_protocol> endpoint;

  /// The UNIX domain socket type.
  typedef basic_seq_packet_socket<seq_packet_protocol> socket;

  /// The UNIX domain acceptor type.
  typedef basic_socket_acceptor<seq_packet_protocol> acceptor;
};

} // namespace local
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
       //   || defined(GENERATING_DOCUMENTATION)

#endif // BOOST_ASIO_LOCAL_SEQ_PACKET_PROTOCOL_HPP
