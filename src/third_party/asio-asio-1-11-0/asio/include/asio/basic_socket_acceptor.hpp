//
// basic_socket_acceptor.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_BASIC_SOCKET_ACCEPTOR_HPP
#define ASIO_BASIC_SOCKET_ACCEPTOR_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include "asio/basic_io_object.hpp"
#include "asio/basic_socket.hpp"
#include "asio/detail/handler_type_requirements.hpp"
#include "asio/detail/throw_error.hpp"
#include "asio/detail/type_traits.hpp"
#include "asio/error.hpp"
#include "asio/socket_acceptor_service.hpp"
#include "asio/socket_base.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {

/// Provides the ability to accept new connections.
/**
 * The basic_socket_acceptor class template is used for accepting new socket
 * connections.
 *
 * @par Thread Safety
 * @e Distinct @e objects: Safe.@n
 * @e Shared @e objects: Unsafe.
 *
 * @par Example
 * Opening a socket acceptor with the SO_REUSEADDR option enabled:
 * @code
 * asio::ip::tcp::acceptor acceptor(io_service);
 * asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), port);
 * acceptor.open(endpoint.protocol());
 * acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
 * acceptor.bind(endpoint);
 * acceptor.listen();
 * @endcode
 */
template <typename Protocol,
    typename SocketAcceptorService = socket_acceptor_service<Protocol> >
class basic_socket_acceptor
  : public basic_io_object<SocketAcceptorService>,
    public socket_base
{
public:
  /// The native representation of an acceptor.
  typedef typename SocketAcceptorService::native_handle_type native_handle_type;

  /// The protocol type.
  typedef Protocol protocol_type;

  /// The endpoint type.
  typedef typename Protocol::endpoint endpoint_type;

  /// Construct an acceptor without opening it.
  /**
   * This constructor creates an acceptor without opening it to listen for new
   * connections. The open() function must be called before the acceptor can
   * accept new socket connections.
   *
   * @param io_service The io_service object that the acceptor will use to
   * dispatch handlers for any asynchronous operations performed on the
   * acceptor.
   */
  explicit basic_socket_acceptor(asio::io_service& io_service)
    : basic_io_object<SocketAcceptorService>(io_service)
  {
  }

  /// Construct an open acceptor.
  /**
   * This constructor creates an acceptor and automatically opens it.
   *
   * @param io_service The io_service object that the acceptor will use to
   * dispatch handlers for any asynchronous operations performed on the
   * acceptor.
   *
   * @param protocol An object specifying protocol parameters to be used.
   *
   * @throws asio::system_error Thrown on failure.
   */
  basic_socket_acceptor(asio::io_service& io_service,
      const protocol_type& protocol)
    : basic_io_object<SocketAcceptorService>(io_service)
  {
    asio::error_code ec;
    this->get_service().open(this->get_implementation(), protocol, ec);
    asio::detail::throw_error(ec, "open");
  }

  /// Construct an acceptor opened on the given endpoint.
  /**
   * This constructor creates an acceptor and automatically opens it to listen
   * for new connections on the specified endpoint.
   *
   * @param io_service The io_service object that the acceptor will use to
   * dispatch handlers for any asynchronous operations performed on the
   * acceptor.
   *
   * @param endpoint An endpoint on the local machine on which the acceptor
   * will listen for new connections.
   *
   * @param reuse_addr Whether the constructor should set the socket option
   * socket_base::reuse_address.
   *
   * @throws asio::system_error Thrown on failure.
   *
   * @note This constructor is equivalent to the following code:
   * @code
   * basic_socket_acceptor<Protocol> acceptor(io_service);
   * acceptor.open(endpoint.protocol());
   * if (reuse_addr)
   *   acceptor.set_option(socket_base::reuse_address(true));
   * acceptor.bind(endpoint);
   * acceptor.listen(listen_backlog);
   * @endcode
   */
  basic_socket_acceptor(asio::io_service& io_service,
      const endpoint_type& endpoint, bool reuse_addr = true)
    : basic_io_object<SocketAcceptorService>(io_service)
  {
    asio::error_code ec;
    const protocol_type protocol = endpoint.protocol();
    this->get_service().open(this->get_implementation(), protocol, ec);
    asio::detail::throw_error(ec, "open");
    if (reuse_addr)
    {
      this->get_service().set_option(this->get_implementation(),
          socket_base::reuse_address(true), ec);
      asio::detail::throw_error(ec, "set_option");
    }
    this->get_service().bind(this->get_implementation(), endpoint, ec);
    asio::detail::throw_error(ec, "bind");
    this->get_service().listen(this->get_implementation(),
        socket_base::max_connections, ec);
    asio::detail::throw_error(ec, "listen");
  }

  /// Construct a basic_socket_acceptor on an existing native acceptor.
  /**
   * This constructor creates an acceptor object to hold an existing native
   * acceptor.
   *
   * @param io_service The io_service object that the acceptor will use to
   * dispatch handlers for any asynchronous operations performed on the
   * acceptor.
   *
   * @param protocol An object specifying protocol parameters to be used.
   *
   * @param native_acceptor A native acceptor.
   *
   * @throws asio::system_error Thrown on failure.
   */
  basic_socket_acceptor(asio::io_service& io_service,
      const protocol_type& protocol, const native_handle_type& native_acceptor)
    : basic_io_object<SocketAcceptorService>(io_service)
  {
    asio::error_code ec;
    this->get_service().assign(this->get_implementation(),
        protocol, native_acceptor, ec);
    asio::detail::throw_error(ec, "assign");
  }

#if defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)
  /// Move-construct a basic_socket_acceptor from another.
  /**
   * This constructor moves an acceptor from one object to another.
   *
   * @param other The other basic_socket_acceptor object from which the move
   * will occur.
   *
   * @note Following the move, the moved-from object is in the same state as if
   * constructed using the @c basic_socket_acceptor(io_service&) constructor.
   */
  basic_socket_acceptor(basic_socket_acceptor&& other)
    : basic_io_object<SocketAcceptorService>(
        ASIO_MOVE_CAST(basic_socket_acceptor)(other))
  {
  }

  /// Move-assign a basic_socket_acceptor from another.
  /**
   * This assignment operator moves an acceptor from one object to another.
   *
   * @param other The other basic_socket_acceptor object from which the move
   * will occur.
   *
   * @note Following the move, the moved-from object is in the same state as if
   * constructed using the @c basic_socket_acceptor(io_service&) constructor.
   */
  basic_socket_acceptor& operator=(basic_socket_acceptor&& other)
  {
    basic_io_object<SocketAcceptorService>::operator=(
        ASIO_MOVE_CAST(basic_socket_acceptor)(other));
    return *this;
  }

  // All socket acceptors have access to each other's implementations.
  template <typename Protocol1, typename SocketAcceptorService1>
  friend class basic_socket_acceptor;

  /// Move-construct a basic_socket_acceptor from an acceptor of another
  /// protocol type.
  /**
   * This constructor moves an acceptor from one object to another.
   *
   * @param other The other basic_socket_acceptor object from which the move
   * will occur.
   *
   * @note Following the move, the moved-from object is in the same state as if
   * constructed using the @c basic_socket(io_service&) constructor.
   */
  template <typename Protocol1, typename SocketAcceptorService1>
  basic_socket_acceptor(
      basic_socket_acceptor<Protocol1, SocketAcceptorService1>&& other,
      typename enable_if<is_convertible<Protocol1, Protocol>::value>::type* = 0)
    : basic_io_object<SocketAcceptorService>(
        other.get_service().get_io_service())
  {
    this->get_service().template converting_move_construct<Protocol1>(
        this->get_implementation(), other.get_implementation());
  }

  /// Move-assign a basic_socket_acceptor from an acceptor of another protocol
  /// type.
  /**
   * This assignment operator moves an acceptor from one object to another.
   *
   * @param other The other basic_socket_acceptor object from which the move
   * will occur.
   *
   * @note Following the move, the moved-from object is in the same state as if
   * constructed using the @c basic_socket(io_service&) constructor.
   */
  template <typename Protocol1, typename SocketAcceptorService1>
  typename enable_if<is_convertible<Protocol1, Protocol>::value,
      basic_socket_acceptor>::type& operator=(
        basic_socket_acceptor<Protocol1, SocketAcceptorService1>&& other)
  {
    basic_socket_acceptor tmp(ASIO_MOVE_CAST2(basic_socket_acceptor<
            Protocol1, SocketAcceptorService1>)(other));
    basic_io_object<SocketAcceptorService>::operator=(
        ASIO_MOVE_CAST(basic_socket_acceptor)(tmp));
    return *this;
  }
#endif // defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)

  /// Open the acceptor using the specified protocol.
  /**
   * This function opens the socket acceptor so that it will use the specified
   * protocol.
   *
   * @param protocol An object specifying which protocol is to be used.
   *
   * @throws asio::system_error Thrown on failure.
   *
   * @par Example
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * acceptor.open(asio::ip::tcp::v4());
   * @endcode
   */
  void open(const protocol_type& protocol = protocol_type())
  {
    asio::error_code ec;
    this->get_service().open(this->get_implementation(), protocol, ec);
    asio::detail::throw_error(ec, "open");
  }

  /// Open the acceptor using the specified protocol.
  /**
   * This function opens the socket acceptor so that it will use the specified
   * protocol.
   *
   * @param protocol An object specifying which protocol is to be used.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @par Example
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * asio::error_code ec;
   * acceptor.open(asio::ip::tcp::v4(), ec);
   * if (ec)
   * {
   *   // An error occurred.
   * }
   * @endcode
   */
  asio::error_code open(const protocol_type& protocol,
      asio::error_code& ec)
  {
    return this->get_service().open(this->get_implementation(), protocol, ec);
  }

  /// Assigns an existing native acceptor to the acceptor.
  /*
   * This function opens the acceptor to hold an existing native acceptor.
   *
   * @param protocol An object specifying which protocol is to be used.
   *
   * @param native_acceptor A native acceptor.
   *
   * @throws asio::system_error Thrown on failure.
   */
  void assign(const protocol_type& protocol,
      const native_handle_type& native_acceptor)
  {
    asio::error_code ec;
    this->get_service().assign(this->get_implementation(),
        protocol, native_acceptor, ec);
    asio::detail::throw_error(ec, "assign");
  }

  /// Assigns an existing native acceptor to the acceptor.
  /*
   * This function opens the acceptor to hold an existing native acceptor.
   *
   * @param protocol An object specifying which protocol is to be used.
   *
   * @param native_acceptor A native acceptor.
   *
   * @param ec Set to indicate what error occurred, if any.
   */
  asio::error_code assign(const protocol_type& protocol,
      const native_handle_type& native_acceptor, asio::error_code& ec)
  {
    return this->get_service().assign(this->get_implementation(),
        protocol, native_acceptor, ec);
  }

  /// Determine whether the acceptor is open.
  bool is_open() const
  {
    return this->get_service().is_open(this->get_implementation());
  }

  /// Bind the acceptor to the given local endpoint.
  /**
   * This function binds the socket acceptor to the specified endpoint on the
   * local machine.
   *
   * @param endpoint An endpoint on the local machine to which the socket
   * acceptor will be bound.
   *
   * @throws asio::system_error Thrown on failure.
   *
   * @par Example
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), 12345);
   * acceptor.open(endpoint.protocol());
   * acceptor.bind(endpoint);
   * @endcode
   */
  void bind(const endpoint_type& endpoint)
  {
    asio::error_code ec;
    this->get_service().bind(this->get_implementation(), endpoint, ec);
    asio::detail::throw_error(ec, "bind");
  }

  /// Bind the acceptor to the given local endpoint.
  /**
   * This function binds the socket acceptor to the specified endpoint on the
   * local machine.
   *
   * @param endpoint An endpoint on the local machine to which the socket
   * acceptor will be bound.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @par Example
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), 12345);
   * acceptor.open(endpoint.protocol());
   * asio::error_code ec;
   * acceptor.bind(endpoint, ec);
   * if (ec)
   * {
   *   // An error occurred.
   * }
   * @endcode
   */
  asio::error_code bind(const endpoint_type& endpoint,
      asio::error_code& ec)
  {
    return this->get_service().bind(this->get_implementation(), endpoint, ec);
  }

  /// Place the acceptor into the state where it will listen for new
  /// connections.
  /**
   * This function puts the socket acceptor into the state where it may accept
   * new connections.
   *
   * @param backlog The maximum length of the queue of pending connections.
   *
   * @throws asio::system_error Thrown on failure.
   */
  void listen(int backlog = socket_base::max_connections)
  {
    asio::error_code ec;
    this->get_service().listen(this->get_implementation(), backlog, ec);
    asio::detail::throw_error(ec, "listen");
  }

  /// Place the acceptor into the state where it will listen for new
  /// connections.
  /**
   * This function puts the socket acceptor into the state where it may accept
   * new connections.
   *
   * @param backlog The maximum length of the queue of pending connections.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @par Example
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * ...
   * asio::error_code ec;
   * acceptor.listen(asio::socket_base::max_connections, ec);
   * if (ec)
   * {
   *   // An error occurred.
   * }
   * @endcode
   */
  asio::error_code listen(int backlog, asio::error_code& ec)
  {
    return this->get_service().listen(this->get_implementation(), backlog, ec);
  }

  /// Close the acceptor.
  /**
   * This function is used to close the acceptor. Any asynchronous accept
   * operations will be cancelled immediately.
   *
   * A subsequent call to open() is required before the acceptor can again be
   * used to again perform socket accept operations.
   *
   * @throws asio::system_error Thrown on failure.
   */
  void close()
  {
    asio::error_code ec;
    this->get_service().close(this->get_implementation(), ec);
    asio::detail::throw_error(ec, "close");
  }

  /// Close the acceptor.
  /**
   * This function is used to close the acceptor. Any asynchronous accept
   * operations will be cancelled immediately.
   *
   * A subsequent call to open() is required before the acceptor can again be
   * used to again perform socket accept operations.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @par Example
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * ...
   * asio::error_code ec;
   * acceptor.close(ec);
   * if (ec)
   * {
   *   // An error occurred.
   * }
   * @endcode
   */
  asio::error_code close(asio::error_code& ec)
  {
    return this->get_service().close(this->get_implementation(), ec);
  }

  /// Get the native acceptor representation.
  /**
   * This function may be used to obtain the underlying representation of the
   * acceptor. This is intended to allow access to native acceptor functionality
   * that is not otherwise provided.
   */
  native_handle_type native_handle()
  {
    return this->get_service().native_handle(this->get_implementation());
  }

  /// Cancel all asynchronous operations associated with the acceptor.
  /**
   * This function causes all outstanding asynchronous connect, send and receive
   * operations to finish immediately, and the handlers for cancelled operations
   * will be passed the asio::error::operation_aborted error.
   *
   * @throws asio::system_error Thrown on failure.
   */
  void cancel()
  {
    asio::error_code ec;
    this->get_service().cancel(this->get_implementation(), ec);
    asio::detail::throw_error(ec, "cancel");
  }

  /// Cancel all asynchronous operations associated with the acceptor.
  /**
   * This function causes all outstanding asynchronous connect, send and receive
   * operations to finish immediately, and the handlers for cancelled operations
   * will be passed the asio::error::operation_aborted error.
   *
   * @param ec Set to indicate what error occurred, if any.
   */
  asio::error_code cancel(asio::error_code& ec)
  {
    return this->get_service().cancel(this->get_implementation(), ec);
  }

  /// Set an option on the acceptor.
  /**
   * This function is used to set an option on the acceptor.
   *
   * @param option The new option value to be set on the acceptor.
   *
   * @throws asio::system_error Thrown on failure.
   *
   * @sa SettableSocketOption @n
   * asio::socket_base::reuse_address
   * asio::socket_base::enable_connection_aborted
   *
   * @par Example
   * Setting the SOL_SOCKET/SO_REUSEADDR option:
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * ...
   * asio::ip::tcp::acceptor::reuse_address option(true);
   * acceptor.set_option(option);
   * @endcode
   */
  template <typename SettableSocketOption>
  void set_option(const SettableSocketOption& option)
  {
    asio::error_code ec;
    this->get_service().set_option(this->get_implementation(), option, ec);
    asio::detail::throw_error(ec, "set_option");
  }

  /// Set an option on the acceptor.
  /**
   * This function is used to set an option on the acceptor.
   *
   * @param option The new option value to be set on the acceptor.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @sa SettableSocketOption @n
   * asio::socket_base::reuse_address
   * asio::socket_base::enable_connection_aborted
   *
   * @par Example
   * Setting the SOL_SOCKET/SO_REUSEADDR option:
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * ...
   * asio::ip::tcp::acceptor::reuse_address option(true);
   * asio::error_code ec;
   * acceptor.set_option(option, ec);
   * if (ec)
   * {
   *   // An error occurred.
   * }
   * @endcode
   */
  template <typename SettableSocketOption>
  asio::error_code set_option(const SettableSocketOption& option,
      asio::error_code& ec)
  {
    return this->get_service().set_option(
        this->get_implementation(), option, ec);
  }

  /// Get an option from the acceptor.
  /**
   * This function is used to get the current value of an option on the
   * acceptor.
   *
   * @param option The option value to be obtained from the acceptor.
   *
   * @throws asio::system_error Thrown on failure.
   *
   * @sa GettableSocketOption @n
   * asio::socket_base::reuse_address
   *
   * @par Example
   * Getting the value of the SOL_SOCKET/SO_REUSEADDR option:
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * ...
   * asio::ip::tcp::acceptor::reuse_address option;
   * acceptor.get_option(option);
   * bool is_set = option.get();
   * @endcode
   */
  template <typename GettableSocketOption>
  void get_option(GettableSocketOption& option)
  {
    asio::error_code ec;
    this->get_service().get_option(this->get_implementation(), option, ec);
    asio::detail::throw_error(ec, "get_option");
  }

  /// Get an option from the acceptor.
  /**
   * This function is used to get the current value of an option on the
   * acceptor.
   *
   * @param option The option value to be obtained from the acceptor.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @sa GettableSocketOption @n
   * asio::socket_base::reuse_address
   *
   * @par Example
   * Getting the value of the SOL_SOCKET/SO_REUSEADDR option:
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * ...
   * asio::ip::tcp::acceptor::reuse_address option;
   * asio::error_code ec;
   * acceptor.get_option(option, ec);
   * if (ec)
   * {
   *   // An error occurred.
   * }
   * bool is_set = option.get();
   * @endcode
   */
  template <typename GettableSocketOption>
  asio::error_code get_option(GettableSocketOption& option,
      asio::error_code& ec)
  {
    return this->get_service().get_option(
        this->get_implementation(), option, ec);
  }

  /// Perform an IO control command on the acceptor.
  /**
   * This function is used to execute an IO control command on the acceptor.
   *
   * @param command The IO control command to be performed on the acceptor.
   *
   * @throws asio::system_error Thrown on failure.
   *
   * @sa IoControlCommand @n
   * asio::socket_base::non_blocking_io
   *
   * @par Example
   * Getting the number of bytes ready to read:
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * ...
   * asio::ip::tcp::acceptor::non_blocking_io command(true);
   * socket.io_control(command);
   * @endcode
   */
  template <typename IoControlCommand>
  void io_control(IoControlCommand& command)
  {
    asio::error_code ec;
    this->get_service().io_control(this->get_implementation(), command, ec);
    asio::detail::throw_error(ec, "io_control");
  }

  /// Perform an IO control command on the acceptor.
  /**
   * This function is used to execute an IO control command on the acceptor.
   *
   * @param command The IO control command to be performed on the acceptor.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @sa IoControlCommand @n
   * asio::socket_base::non_blocking_io
   *
   * @par Example
   * Getting the number of bytes ready to read:
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * ...
   * asio::ip::tcp::acceptor::non_blocking_io command(true);
   * asio::error_code ec;
   * socket.io_control(command, ec);
   * if (ec)
   * {
   *   // An error occurred.
   * }
   * @endcode
   */
  template <typename IoControlCommand>
  asio::error_code io_control(IoControlCommand& command,
      asio::error_code& ec)
  {
    return this->get_service().io_control(
        this->get_implementation(), command, ec);
  }

  /// Gets the non-blocking mode of the acceptor.
  /**
   * @returns @c true if the acceptor's synchronous operations will fail with
   * asio::error::would_block if they are unable to perform the requested
   * operation immediately. If @c false, synchronous operations will block
   * until complete.
   *
   * @note The non-blocking mode has no effect on the behaviour of asynchronous
   * operations. Asynchronous operations will never fail with the error
   * asio::error::would_block.
   */
  bool non_blocking() const
  {
    return this->get_service().non_blocking(this->get_implementation());
  }

  /// Sets the non-blocking mode of the acceptor.
  /**
   * @param mode If @c true, the acceptor's synchronous operations will fail
   * with asio::error::would_block if they are unable to perform the
   * requested operation immediately. If @c false, synchronous operations will
   * block until complete.
   *
   * @throws asio::system_error Thrown on failure.
   *
   * @note The non-blocking mode has no effect on the behaviour of asynchronous
   * operations. Asynchronous operations will never fail with the error
   * asio::error::would_block.
   */
  void non_blocking(bool mode)
  {
    asio::error_code ec;
    this->get_service().non_blocking(this->get_implementation(), mode, ec);
    asio::detail::throw_error(ec, "non_blocking");
  }

  /// Sets the non-blocking mode of the acceptor.
  /**
   * @param mode If @c true, the acceptor's synchronous operations will fail
   * with asio::error::would_block if they are unable to perform the
   * requested operation immediately. If @c false, synchronous operations will
   * block until complete.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @note The non-blocking mode has no effect on the behaviour of asynchronous
   * operations. Asynchronous operations will never fail with the error
   * asio::error::would_block.
   */
  asio::error_code non_blocking(
      bool mode, asio::error_code& ec)
  {
    return this->get_service().non_blocking(
        this->get_implementation(), mode, ec);
  }

  /// Gets the non-blocking mode of the native acceptor implementation.
  /**
   * This function is used to retrieve the non-blocking mode of the underlying
   * native acceptor. This mode has no effect on the behaviour of the acceptor
   * object's synchronous operations.
   *
   * @returns @c true if the underlying acceptor is in non-blocking mode and
   * direct system calls may fail with asio::error::would_block (or the
   * equivalent system error).
   *
   * @note The current non-blocking mode is cached by the acceptor object.
   * Consequently, the return value may be incorrect if the non-blocking mode
   * was set directly on the native acceptor.
   */
  bool native_non_blocking() const
  {
    return this->get_service().native_non_blocking(this->get_implementation());
  }

  /// Sets the non-blocking mode of the native acceptor implementation.
  /**
   * This function is used to modify the non-blocking mode of the underlying
   * native acceptor. It has no effect on the behaviour of the acceptor object's
   * synchronous operations.
   *
   * @param mode If @c true, the underlying acceptor is put into non-blocking
   * mode and direct system calls may fail with asio::error::would_block
   * (or the equivalent system error).
   *
   * @throws asio::system_error Thrown on failure. If the @c mode is
   * @c false, but the current value of @c non_blocking() is @c true, this
   * function fails with asio::error::invalid_argument, as the
   * combination does not make sense.
   */
  void native_non_blocking(bool mode)
  {
    asio::error_code ec;
    this->get_service().native_non_blocking(
        this->get_implementation(), mode, ec);
    asio::detail::throw_error(ec, "native_non_blocking");
  }

  /// Sets the non-blocking mode of the native acceptor implementation.
  /**
   * This function is used to modify the non-blocking mode of the underlying
   * native acceptor. It has no effect on the behaviour of the acceptor object's
   * synchronous operations.
   *
   * @param mode If @c true, the underlying acceptor is put into non-blocking
   * mode and direct system calls may fail with asio::error::would_block
   * (or the equivalent system error).
   *
   * @param ec Set to indicate what error occurred, if any. If the @c mode is
   * @c false, but the current value of @c non_blocking() is @c true, this
   * function fails with asio::error::invalid_argument, as the
   * combination does not make sense.
   */
  asio::error_code native_non_blocking(
      bool mode, asio::error_code& ec)
  {
    return this->get_service().native_non_blocking(
        this->get_implementation(), mode, ec);
  }

  /// Get the local endpoint of the acceptor.
  /**
   * This function is used to obtain the locally bound endpoint of the acceptor.
   *
   * @returns An object that represents the local endpoint of the acceptor.
   *
   * @throws asio::system_error Thrown on failure.
   *
   * @par Example
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * ...
   * asio::ip::tcp::endpoint endpoint = acceptor.local_endpoint();
   * @endcode
   */
  endpoint_type local_endpoint() const
  {
    asio::error_code ec;
    endpoint_type ep = this->get_service().local_endpoint(
        this->get_implementation(), ec);
    asio::detail::throw_error(ec, "local_endpoint");
    return ep;
  }

  /// Get the local endpoint of the acceptor.
  /**
   * This function is used to obtain the locally bound endpoint of the acceptor.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @returns An object that represents the local endpoint of the acceptor.
   * Returns a default-constructed endpoint object if an error occurred and the
   * error handler did not throw an exception.
   *
   * @par Example
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * ...
   * asio::error_code ec;
   * asio::ip::tcp::endpoint endpoint = acceptor.local_endpoint(ec);
   * if (ec)
   * {
   *   // An error occurred.
   * }
   * @endcode
   */
  endpoint_type local_endpoint(asio::error_code& ec) const
  {
    return this->get_service().local_endpoint(this->get_implementation(), ec);
  }

  /// Wait for the acceptor to become ready to read, ready to write, or to have
  /// pending error conditions.
  /**
   * This function is used to perform a blocking wait for an acceptor to enter
   * a ready to read, write or error condition state.
   *
   * @param w Specifies the desired acceptor state.
   *
   * @par Example
   * Waiting for an acceptor to become readable.
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * ...
   * acceptor.wait(asio::ip::tcp::acceptor::wait_read);
   * @endcode
   */
  void wait(wait_type w)
  {
    asio::error_code ec;
    this->get_service().wait(this->get_implementation(), w, ec);
    asio::detail::throw_error(ec, "wait");
  }

  /// Wait for the acceptor to become ready to read, ready to write, or to have
  /// pending error conditions.
  /**
   * This function is used to perform a blocking wait for an acceptor to enter
   * a ready to read, write or error condition state.
   *
   * @param w Specifies the desired acceptor state.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @par Example
   * Waiting for an acceptor to become readable.
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * ...
   * asio::error_code ec;
   * acceptor.wait(asio::ip::tcp::acceptor::wait_read, ec);
   * @endcode
   */
  asio::error_code wait(wait_type w, asio::error_code& ec)
  {
    return this->get_service().wait(this->get_implementation(), w, ec);
  }

  /// Asynchronously wait for the acceptor to become ready to read, ready to
  /// write, or to have pending error conditions.
  /**
   * This function is used to perform an asynchronous wait for an acceptor to
   * enter a ready to read, write or error condition state.
   *
   * @param w Specifies the desired acceptor state.
   *
   * @param handler The handler to be called when the wait operation completes.
   * Copies will be made of the handler as required. The function signature of
   * the handler must be:
   * @code void handler(
   *   const asio::error_code& error // Result of operation
   * ); @endcode
   * Regardless of whether the asynchronous operation completes immediately or
   * not, the handler will not be invoked from within this function. Invocation
   * of the handler will be performed in a manner equivalent to using
   * asio::io_service::post().
   *
   * @par Example
   * @code
   * void wait_handler(const asio::error_code& error)
   * {
   *   if (!error)
   *   {
   *     // Wait succeeded.
   *   }
   * }
   *
   * ...
   *
   * asio::ip::tcp::acceptor acceptor(io_service);
   * ...
   * acceptor.async_wait(
   *     asio::ip::tcp::acceptor::wait_read,
   *     wait_handler);
   * @endcode
   */
  template <typename WaitHandler>
  ASIO_INITFN_RESULT_TYPE(WaitHandler,
      void (asio::error_code))
  async_wait(wait_type w, ASIO_MOVE_ARG(WaitHandler) handler)
  {
    // If you get an error on the following line it means that your handler does
    // not meet the documented type requirements for a WaitHandler.
    ASIO_WAIT_HANDLER_CHECK(WaitHandler, handler) type_check;

    return this->get_service().async_wait(this->get_implementation(),
        w, ASIO_MOVE_CAST(WaitHandler)(handler));
  }

  /// Accept a new connection.
  /**
   * This function is used to accept a new connection from a peer into the
   * given socket. The function call will block until a new connection has been
   * accepted successfully or an error occurs.
   *
   * @param peer The socket into which the new connection will be accepted.
   *
   * @throws asio::system_error Thrown on failure.
   *
   * @par Example
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * ...
   * asio::ip::tcp::socket socket(io_service);
   * acceptor.accept(socket);
   * @endcode
   */
  template <typename Protocol1, typename SocketService>
  void accept(basic_socket<Protocol1, SocketService>& peer,
      typename enable_if<is_convertible<Protocol, Protocol1>::value>::type* = 0)
  {
    asio::error_code ec;
    this->get_service().accept(this->get_implementation(),
        peer, static_cast<endpoint_type*>(0), ec);
    asio::detail::throw_error(ec, "accept");
  }

  /// Accept a new connection.
  /**
   * This function is used to accept a new connection from a peer into the
   * given socket. The function call will block until a new connection has been
   * accepted successfully or an error occurs.
   *
   * @param peer The socket into which the new connection will be accepted.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @par Example
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * ...
   * asio::ip::tcp::soocket socket(io_service);
   * asio::error_code ec;
   * acceptor.accept(socket, ec);
   * if (ec)
   * {
   *   // An error occurred.
   * }
   * @endcode
   */
  template <typename Protocol1, typename SocketService>
  asio::error_code accept(
      basic_socket<Protocol1, SocketService>& peer,
      asio::error_code& ec,
      typename enable_if<is_convertible<Protocol, Protocol1>::value>::type* = 0)
  {
    return this->get_service().accept(this->get_implementation(),
        peer, static_cast<endpoint_type*>(0), ec);
  }

  /// Start an asynchronous accept.
  /**
   * This function is used to asynchronously accept a new connection into a
   * socket. The function call always returns immediately.
   *
   * @param peer The socket into which the new connection will be accepted.
   * Ownership of the peer object is retained by the caller, which must
   * guarantee that it is valid until the handler is called.
   *
   * @param handler The handler to be called when the accept operation
   * completes. Copies will be made of the handler as required. The function
   * signature of the handler must be:
   * @code void handler(
   *   const asio::error_code& error // Result of operation.
   * ); @endcode
   * Regardless of whether the asynchronous operation completes immediately or
   * not, the handler will not be invoked from within this function. Invocation
   * of the handler will be performed in a manner equivalent to using
   * asio::io_service::post().
   *
   * @par Example
   * @code
   * void accept_handler(const asio::error_code& error)
   * {
   *   if (!error)
   *   {
   *     // Accept succeeded.
   *   }
   * }
   *
   * ...
   *
   * asio::ip::tcp::acceptor acceptor(io_service);
   * ...
   * asio::ip::tcp::socket socket(io_service);
   * acceptor.async_accept(socket, accept_handler);
   * @endcode
   */
  template <typename Protocol1, typename SocketService, typename AcceptHandler>
  ASIO_INITFN_RESULT_TYPE(AcceptHandler,
      void (asio::error_code))
  async_accept(basic_socket<Protocol1, SocketService>& peer,
      ASIO_MOVE_ARG(AcceptHandler) handler,
      typename enable_if<is_convertible<Protocol, Protocol1>::value>::type* = 0)
  {
    // If you get an error on the following line it means that your handler does
    // not meet the documented type requirements for a AcceptHandler.
    ASIO_ACCEPT_HANDLER_CHECK(AcceptHandler, handler) type_check;

    return this->get_service().async_accept(this->get_implementation(),
        peer, static_cast<endpoint_type*>(0),
        ASIO_MOVE_CAST(AcceptHandler)(handler));
  }

  /// Accept a new connection and obtain the endpoint of the peer
  /**
   * This function is used to accept a new connection from a peer into the
   * given socket, and additionally provide the endpoint of the remote peer.
   * The function call will block until a new connection has been accepted
   * successfully or an error occurs.
   *
   * @param peer The socket into which the new connection will be accepted.
   *
   * @param peer_endpoint An endpoint object which will receive the endpoint of
   * the remote peer.
   *
   * @throws asio::system_error Thrown on failure.
   *
   * @par Example
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * ...
   * asio::ip::tcp::socket socket(io_service);
   * asio::ip::tcp::endpoint endpoint;
   * acceptor.accept(socket, endpoint);
   * @endcode
   */
  template <typename SocketService>
  void accept(basic_socket<protocol_type, SocketService>& peer,
      endpoint_type& peer_endpoint)
  {
    asio::error_code ec;
    this->get_service().accept(this->get_implementation(),
        peer, &peer_endpoint, ec);
    asio::detail::throw_error(ec, "accept");
  }

  /// Accept a new connection and obtain the endpoint of the peer
  /**
   * This function is used to accept a new connection from a peer into the
   * given socket, and additionally provide the endpoint of the remote peer.
   * The function call will block until a new connection has been accepted
   * successfully or an error occurs.
   *
   * @param peer The socket into which the new connection will be accepted.
   *
   * @param peer_endpoint An endpoint object which will receive the endpoint of
   * the remote peer.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @par Example
   * @code
   * asio::ip::tcp::acceptor acceptor(io_service);
   * ...
   * asio::ip::tcp::socket socket(io_service);
   * asio::ip::tcp::endpoint endpoint;
   * asio::error_code ec;
   * acceptor.accept(socket, endpoint, ec);
   * if (ec)
   * {
   *   // An error occurred.
   * }
   * @endcode
   */
  template <typename SocketService>
  asio::error_code accept(
      basic_socket<protocol_type, SocketService>& peer,
      endpoint_type& peer_endpoint, asio::error_code& ec)
  {
    return this->get_service().accept(
        this->get_implementation(), peer, &peer_endpoint, ec);
  }

  /// Start an asynchronous accept.
  /**
   * This function is used to asynchronously accept a new connection into a
   * socket, and additionally obtain the endpoint of the remote peer. The
   * function call always returns immediately.
   *
   * @param peer The socket into which the new connection will be accepted.
   * Ownership of the peer object is retained by the caller, which must
   * guarantee that it is valid until the handler is called.
   *
   * @param peer_endpoint An endpoint object into which the endpoint of the
   * remote peer will be written. Ownership of the peer_endpoint object is
   * retained by the caller, which must guarantee that it is valid until the
   * handler is called.
   *
   * @param handler The handler to be called when the accept operation
   * completes. Copies will be made of the handler as required. The function
   * signature of the handler must be:
   * @code void handler(
   *   const asio::error_code& error // Result of operation.
   * ); @endcode
   * Regardless of whether the asynchronous operation completes immediately or
   * not, the handler will not be invoked from within this function. Invocation
   * of the handler will be performed in a manner equivalent to using
   * asio::io_service::post().
   */
  template <typename SocketService, typename AcceptHandler>
  ASIO_INITFN_RESULT_TYPE(AcceptHandler,
      void (asio::error_code))
  async_accept(basic_socket<protocol_type, SocketService>& peer,
      endpoint_type& peer_endpoint, ASIO_MOVE_ARG(AcceptHandler) handler)
  {
    // If you get an error on the following line it means that your handler does
    // not meet the documented type requirements for a AcceptHandler.
    ASIO_ACCEPT_HANDLER_CHECK(AcceptHandler, handler) type_check;

    return this->get_service().async_accept(this->get_implementation(), peer,
        &peer_endpoint, ASIO_MOVE_CAST(AcceptHandler)(handler));
  }
};

} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_BASIC_SOCKET_ACCEPTOR_HPP
