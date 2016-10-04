//
// socket_acceptor_service.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_SOCKET_ACCEPTOR_SERVICE_HPP
#define ASIO_SOCKET_ACCEPTOR_SERVICE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include "asio/basic_socket.hpp"
#include "asio/detail/type_traits.hpp"
#include "asio/error.hpp"
#include "asio/io_service.hpp"

#if defined(ASIO_WINDOWS_RUNTIME)
# include "asio/detail/null_socket_service.hpp"
#elif defined(ASIO_HAS_IOCP)
# include "asio/detail/win_iocp_socket_service.hpp"
#else
# include "asio/detail/reactive_socket_service.hpp"
#endif

#include "asio/detail/push_options.hpp"

namespace asio {

/// Default service implementation for a socket acceptor.
template <typename Protocol>
class socket_acceptor_service
#if defined(GENERATING_DOCUMENTATION)
  : public asio::io_service::service
#else
  : public asio::detail::service_base<socket_acceptor_service<Protocol> >
#endif
{
public:
#if defined(GENERATING_DOCUMENTATION)
  /// The unique service identifier.
  static asio::io_service::id id;
#endif

  /// The protocol type.
  typedef Protocol protocol_type;

  /// The endpoint type.
  typedef typename protocol_type::endpoint endpoint_type;

private:
  // The type of the platform-specific implementation.
#if defined(ASIO_WINDOWS_RUNTIME)
  typedef detail::null_socket_service<Protocol> service_impl_type;
#elif defined(ASIO_HAS_IOCP)
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

  /// Construct a new socket acceptor service for the specified io_service.
  explicit socket_acceptor_service(asio::io_service& io_service)
    : asio::detail::service_base<
        socket_acceptor_service<Protocol> >(io_service),
      service_impl_(io_service)
  {
  }

  /// Construct a new socket acceptor implementation.
  void construct(implementation_type& impl)
  {
    service_impl_.construct(impl);
  }

#if defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)
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

  /// Move-construct a new socket acceptor implementation from another protocol
  /// type.
  template <typename Protocol1>
  void converting_move_construct(implementation_type& impl,
      typename socket_acceptor_service<
        Protocol1>::implementation_type& other_impl,
      typename enable_if<is_convertible<
        Protocol1, Protocol>::value>::type* = 0)
  {
    service_impl_.template converting_move_construct<Protocol1>(
        impl, other_impl);
  }
#endif // defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)

  /// Destroy a socket acceptor implementation.
  void destroy(implementation_type& impl)
  {
    service_impl_.destroy(impl);
  }

  /// Open a new socket acceptor implementation.
  asio::error_code open(implementation_type& impl,
      const protocol_type& protocol, asio::error_code& ec)
  {
    return service_impl_.open(impl, protocol, ec);
  }

  /// Assign an existing native acceptor to a socket acceptor.
  asio::error_code assign(implementation_type& impl,
      const protocol_type& protocol, const native_handle_type& native_acceptor,
      asio::error_code& ec)
  {
    return service_impl_.assign(impl, protocol, native_acceptor, ec);
  }

  /// Determine whether the acceptor is open.
  bool is_open(const implementation_type& impl) const
  {
    return service_impl_.is_open(impl);
  }

  /// Cancel all asynchronous operations associated with the acceptor.
  asio::error_code cancel(implementation_type& impl,
      asio::error_code& ec)
  {
    return service_impl_.cancel(impl, ec);
  }

  /// Bind the socket acceptor to the specified local endpoint.
  asio::error_code bind(implementation_type& impl,
      const endpoint_type& endpoint, asio::error_code& ec)
  {
    return service_impl_.bind(impl, endpoint, ec);
  }

  /// Place the socket acceptor into the state where it will listen for new
  /// connections.
  asio::error_code listen(implementation_type& impl, int backlog,
      asio::error_code& ec)
  {
    return service_impl_.listen(impl, backlog, ec);
  }

  /// Close a socket acceptor implementation.
  asio::error_code close(implementation_type& impl,
      asio::error_code& ec)
  {
    return service_impl_.close(impl, ec);
  }

  /// Get the native acceptor implementation.
  native_handle_type native_handle(implementation_type& impl)
  {
    return service_impl_.native_handle(impl);
  }

  /// Set a socket option.
  template <typename SettableSocketOption>
  asio::error_code set_option(implementation_type& impl,
      const SettableSocketOption& option, asio::error_code& ec)
  {
    return service_impl_.set_option(impl, option, ec);
  }

  /// Get a socket option.
  template <typename GettableSocketOption>
  asio::error_code get_option(const implementation_type& impl,
      GettableSocketOption& option, asio::error_code& ec) const
  {
    return service_impl_.get_option(impl, option, ec);
  }

  /// Perform an IO control command on the socket.
  template <typename IoControlCommand>
  asio::error_code io_control(implementation_type& impl,
      IoControlCommand& command, asio::error_code& ec)
  {
    return service_impl_.io_control(impl, command, ec);
  }

  /// Gets the non-blocking mode of the acceptor.
  bool non_blocking(const implementation_type& impl) const
  {
    return service_impl_.non_blocking(impl);
  }

  /// Sets the non-blocking mode of the acceptor.
  asio::error_code non_blocking(implementation_type& impl,
      bool mode, asio::error_code& ec)
  {
    return service_impl_.non_blocking(impl, mode, ec);
  }

  /// Gets the non-blocking mode of the native acceptor implementation.
  bool native_non_blocking(const implementation_type& impl) const
  {
    return service_impl_.native_non_blocking(impl);
  }

  /// Sets the non-blocking mode of the native acceptor implementation.
  asio::error_code native_non_blocking(implementation_type& impl,
      bool mode, asio::error_code& ec)
  {
    return service_impl_.native_non_blocking(impl, mode, ec);
  }

  /// Get the local endpoint.
  endpoint_type local_endpoint(const implementation_type& impl,
      asio::error_code& ec) const
  {
    return service_impl_.local_endpoint(impl, ec);
  }

  /// Wait for the acceptor to become ready to read, ready to write, or to have
  /// pending error conditions.
  asio::error_code wait(implementation_type& impl,
      socket_base::wait_type w, asio::error_code& ec)
  {
    return service_impl_.wait(impl, w, ec);
  }

  /// Asynchronously wait for the acceptor to become ready to read, ready to
  /// write, or to have pending error conditions.
  template <typename WaitHandler>
  ASIO_INITFN_RESULT_TYPE(WaitHandler,
      void (asio::error_code))
  async_wait(implementation_type& impl, socket_base::wait_type w,
      ASIO_MOVE_ARG(WaitHandler) handler)
  {
    async_completion<WaitHandler,
      void (asio::error_code)> init(handler);

    service_impl_.async_wait(impl, w, init.handler);

    return init.result.get();
  }

  /// Accept a new connection.
  template <typename Protocol1, typename SocketService>
  asio::error_code accept(implementation_type& impl,
      basic_socket<Protocol1, SocketService>& peer,
      endpoint_type* peer_endpoint, asio::error_code& ec,
      typename enable_if<is_convertible<Protocol, Protocol1>::value>::type* = 0)
  {
    return service_impl_.accept(impl, peer, peer_endpoint, ec);
  }

  /// Start an asynchronous accept.
  template <typename Protocol1, typename SocketService, typename AcceptHandler>
  ASIO_INITFN_RESULT_TYPE(AcceptHandler,
      void (asio::error_code))
  async_accept(implementation_type& impl,
      basic_socket<Protocol1, SocketService>& peer,
      endpoint_type* peer_endpoint,
      ASIO_MOVE_ARG(AcceptHandler) handler,
      typename enable_if<is_convertible<Protocol, Protocol1>::value>::type* = 0)
  {
    async_completion<AcceptHandler,
      void (asio::error_code)> init(handler);

    service_impl_.async_accept(impl, peer, peer_endpoint, init.handler);

    return init.result.get();
  }

private:
  // Destroy all user-defined handler objects owned by the service.
  void shutdown_service()
  {
    service_impl_.shutdown_service();
  }

  // The platform-specific implementation.
  service_impl_type service_impl_;
};

} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_SOCKET_ACCEPTOR_SERVICE_HPP
