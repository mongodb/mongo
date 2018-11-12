//
// socket_acceptor_service.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2018 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_SOCKET_ACCEPTOR_SERVICE_HPP
#define BOOST_ASIO_SOCKET_ACCEPTOR_SERVICE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#if defined(BOOST_ASIO_ENABLE_OLD_SERVICES)

#include <boost/asio/basic_socket.hpp>
#include <boost/asio/detail/type_traits.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>

#if defined(BOOST_ASIO_WINDOWS_RUNTIME)
# include <boost/asio/detail/null_socket_service.hpp>
#elif defined(BOOST_ASIO_HAS_IOCP)
# include <boost/asio/detail/win_iocp_socket_service.hpp>
#else
# include <boost/asio/detail/reactive_socket_service.hpp>
#endif

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

/// Default service implementation for a socket acceptor.
template <typename Protocol>
class socket_acceptor_service
#if defined(GENERATING_DOCUMENTATION)
  : public boost::asio::io_context::service
#else
  : public boost::asio::detail::service_base<socket_acceptor_service<Protocol> >
#endif
{
public:
#if defined(GENERATING_DOCUMENTATION)
  /// The unique service identifier.
  static boost::asio::io_context::id id;
#endif

  /// The protocol type.
  typedef Protocol protocol_type;

  /// The endpoint type.
  typedef typename protocol_type::endpoint endpoint_type;

private:
  // The type of the platform-specific implementation.
#if defined(BOOST_ASIO_WINDOWS_RUNTIME)
  typedef detail::null_socket_service<Protocol> service_impl_type;
#elif defined(BOOST_ASIO_HAS_IOCP)
  typedef detail::win_iocp_socket_service<Protocol> service_impl_type;
#else
  typedef detail::reactive_socket_service<Protocol> service_impl_type;
#endif

public:
  /// The native type of the socket acceptor.
#if defined(GENERATING_DOCUMENTATION)
  typedef implementation_defined implementation_type;
#else
  typedef typename service_impl_type::implementation_type implementation_type;
#endif

  /// The native acceptor type.
#if defined(GENERATING_DOCUMENTATION)
  typedef implementation_defined native_handle_type;
#else
  typedef typename service_impl_type::native_handle_type native_handle_type;
#endif

  /// Construct a new socket acceptor service for the specified io_context.
  explicit socket_acceptor_service(boost::asio::io_context& io_context)
    : boost::asio::detail::service_base<
        socket_acceptor_service<Protocol> >(io_context),
      service_impl_(io_context)
  {
  }

  /// Construct a new socket acceptor implementation.
  void construct(implementation_type& impl)
  {
    service_impl_.construct(impl);
  }

#if defined(BOOST_ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)
  /// Move-construct a new socket acceptor implementation.
  void move_construct(implementation_type& impl,
      implementation_type& other_impl)
  {
    service_impl_.move_construct(impl, other_impl);
  }

  /// Move-assign from another socket acceptor implementation.
  void move_assign(implementation_type& impl,
      socket_acceptor_service& other_service,
      implementation_type& other_impl)
  {
    service_impl_.move_assign(impl, other_service.service_impl_, other_impl);
  }

  // All acceptor services have access to each other's implementations.
  template <typename Protocol1> friend class socket_acceptor_service;

  /// Move-construct a new socket acceptor implementation from another protocol
  /// type.
  template <typename Protocol1>
  void converting_move_construct(implementation_type& impl,
      socket_acceptor_service<Protocol1>& other_service,
      typename socket_acceptor_service<
        Protocol1>::implementation_type& other_impl,
      typename enable_if<is_convertible<
        Protocol1, Protocol>::value>::type* = 0)
  {
    service_impl_.template converting_move_construct<Protocol1>(
        impl, other_service.service_impl_, other_impl);
  }
#endif // defined(BOOST_ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)

  /// Destroy a socket acceptor implementation.
  void destroy(implementation_type& impl)
  {
    service_impl_.destroy(impl);
  }

  /// Open a new socket acceptor implementation.
  BOOST_ASIO_SYNC_OP_VOID open(implementation_type& impl,
      const protocol_type& protocol, boost::system::error_code& ec)
  {
    service_impl_.open(impl, protocol, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Assign an existing native acceptor to a socket acceptor.
  BOOST_ASIO_SYNC_OP_VOID assign(implementation_type& impl,
      const protocol_type& protocol, const native_handle_type& native_acceptor,
      boost::system::error_code& ec)
  {
    service_impl_.assign(impl, protocol, native_acceptor, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Determine whether the acceptor is open.
  bool is_open(const implementation_type& impl) const
  {
    return service_impl_.is_open(impl);
  }

  /// Cancel all asynchronous operations associated with the acceptor.
  BOOST_ASIO_SYNC_OP_VOID cancel(implementation_type& impl,
      boost::system::error_code& ec)
  {
    service_impl_.cancel(impl, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Bind the socket acceptor to the specified local endpoint.
  BOOST_ASIO_SYNC_OP_VOID bind(implementation_type& impl,
      const endpoint_type& endpoint, boost::system::error_code& ec)
  {
    service_impl_.bind(impl, endpoint, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Place the socket acceptor into the state where it will listen for new
  /// connections.
  BOOST_ASIO_SYNC_OP_VOID listen(implementation_type& impl, int backlog,
      boost::system::error_code& ec)
  {
    service_impl_.listen(impl, backlog, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Close a socket acceptor implementation.
  BOOST_ASIO_SYNC_OP_VOID close(implementation_type& impl,
      boost::system::error_code& ec)
  {
    service_impl_.close(impl, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Release ownership of the underlying acceptor.
  native_handle_type release(implementation_type& impl,
      boost::system::error_code& ec)
  {
    return service_impl_.release(impl, ec);
  }

  /// Get the native acceptor implementation.
  native_handle_type native_handle(implementation_type& impl)
  {
    return service_impl_.native_handle(impl);
  }

  /// Set a socket option.
  template <typename SettableSocketOption>
  BOOST_ASIO_SYNC_OP_VOID set_option(implementation_type& impl,
      const SettableSocketOption& option, boost::system::error_code& ec)
  {
    service_impl_.set_option(impl, option, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Get a socket option.
  template <typename GettableSocketOption>
  BOOST_ASIO_SYNC_OP_VOID get_option(const implementation_type& impl,
      GettableSocketOption& option, boost::system::error_code& ec) const
  {
    service_impl_.get_option(impl, option, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Perform an IO control command on the socket.
  template <typename IoControlCommand>
  BOOST_ASIO_SYNC_OP_VOID io_control(implementation_type& impl,
      IoControlCommand& command, boost::system::error_code& ec)
  {
    service_impl_.io_control(impl, command, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Gets the non-blocking mode of the acceptor.
  bool non_blocking(const implementation_type& impl) const
  {
    return service_impl_.non_blocking(impl);
  }

  /// Sets the non-blocking mode of the acceptor.
  BOOST_ASIO_SYNC_OP_VOID non_blocking(implementation_type& impl,
      bool mode, boost::system::error_code& ec)
  {
    service_impl_.non_blocking(impl, mode, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Gets the non-blocking mode of the native acceptor implementation.
  bool native_non_blocking(const implementation_type& impl) const
  {
    return service_impl_.native_non_blocking(impl);
  }

  /// Sets the non-blocking mode of the native acceptor implementation.
  BOOST_ASIO_SYNC_OP_VOID native_non_blocking(implementation_type& impl,
      bool mode, boost::system::error_code& ec)
  {
    service_impl_.native_non_blocking(impl, mode, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Get the local endpoint.
  endpoint_type local_endpoint(const implementation_type& impl,
      boost::system::error_code& ec) const
  {
    return service_impl_.local_endpoint(impl, ec);
  }

  /// Wait for the acceptor to become ready to read, ready to write, or to have
  /// pending error conditions.
  BOOST_ASIO_SYNC_OP_VOID wait(implementation_type& impl,
      socket_base::wait_type w, boost::system::error_code& ec)
  {
    service_impl_.wait(impl, w, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Asynchronously wait for the acceptor to become ready to read, ready to
  /// write, or to have pending error conditions.
  template <typename WaitHandler>
  BOOST_ASIO_INITFN_RESULT_TYPE(WaitHandler,
      void (boost::system::error_code))
  async_wait(implementation_type& impl, socket_base::wait_type w,
      BOOST_ASIO_MOVE_ARG(WaitHandler) handler)
  {
    async_completion<WaitHandler,
      void (boost::system::error_code)> init(handler);

    service_impl_.async_wait(impl, w, init.completion_handler);

    return init.result.get();
  }

  /// Accept a new connection.
  template <typename Protocol1, typename SocketService>
  BOOST_ASIO_SYNC_OP_VOID accept(implementation_type& impl,
      basic_socket<Protocol1, SocketService>& peer,
      endpoint_type* peer_endpoint, boost::system::error_code& ec,
      typename enable_if<is_convertible<Protocol, Protocol1>::value>::type* = 0)
  {
    service_impl_.accept(impl, peer, peer_endpoint, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

#if defined(BOOST_ASIO_HAS_MOVE)
  /// Accept a new connection.
  typename Protocol::socket accept(implementation_type& impl,
      io_context* peer_io_context, endpoint_type* peer_endpoint,
      boost::system::error_code& ec)
  {
    return service_impl_.accept(impl, peer_io_context, peer_endpoint, ec);
  }
#endif // defined(BOOST_ASIO_HAS_MOVE)

  /// Start an asynchronous accept.
  template <typename Protocol1, typename SocketService, typename AcceptHandler>
  BOOST_ASIO_INITFN_RESULT_TYPE(AcceptHandler,
      void (boost::system::error_code))
  async_accept(implementation_type& impl,
      basic_socket<Protocol1, SocketService>& peer,
      endpoint_type* peer_endpoint,
      BOOST_ASIO_MOVE_ARG(AcceptHandler) handler,
      typename enable_if<is_convertible<Protocol, Protocol1>::value>::type* = 0)
  {
    async_completion<AcceptHandler,
      void (boost::system::error_code)> init(handler);

    service_impl_.async_accept(impl,
        peer, peer_endpoint, init.completion_handler);

    return init.result.get();
  }

#if defined(BOOST_ASIO_HAS_MOVE)
  /// Start an asynchronous accept.
  template <typename MoveAcceptHandler>
  BOOST_ASIO_INITFN_RESULT_TYPE(MoveAcceptHandler,
      void (boost::system::error_code, typename Protocol::socket))
  async_accept(implementation_type& impl,
      boost::asio::io_context* peer_io_context, endpoint_type* peer_endpoint,
      BOOST_ASIO_MOVE_ARG(MoveAcceptHandler) handler)
  {
    async_completion<MoveAcceptHandler,
      void (boost::system::error_code,
        typename Protocol::socket)> init(handler);

    service_impl_.async_accept(impl,
        peer_io_context, peer_endpoint, init.completion_handler);

    return init.result.get();
  }
#endif // defined(BOOST_ASIO_HAS_MOVE)

private:
  // Destroy all user-defined handler objects owned by the service.
  void shutdown()
  {
    service_impl_.shutdown();
  }

  // The platform-specific implementation.
  service_impl_type service_impl_;
};

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // defined(BOOST_ASIO_ENABLE_OLD_SERVICES)

#endif // BOOST_ASIO_SOCKET_ACCEPTOR_SERVICE_HPP
