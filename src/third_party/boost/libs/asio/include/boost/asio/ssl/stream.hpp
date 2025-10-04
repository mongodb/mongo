//
// ssl/stream.hpp
// ~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_SSL_STREAM_HPP
#define BOOST_ASIO_SSL_STREAM_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#include <boost/asio/async_result.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/detail/buffer_sequence_adapter.hpp>
#include <boost/asio/detail/handler_type_requirements.hpp>
#include <boost/asio/detail/non_const_lvalue.hpp>
#include <boost/asio/detail/noncopyable.hpp>
#include <boost/asio/detail/type_traits.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/detail/buffered_handshake_op.hpp>
#include <boost/asio/ssl/detail/handshake_op.hpp>
#include <boost/asio/ssl/detail/io.hpp>
#include <boost/asio/ssl/detail/read_op.hpp>
#include <boost/asio/ssl/detail/shutdown_op.hpp>
#include <boost/asio/ssl/detail/stream_core.hpp>
#include <boost/asio/ssl/detail/write_op.hpp>
#include <boost/asio/ssl/stream_base.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace ssl {

/// Provides stream-oriented functionality using SSL.
/**
 * The stream class template provides asynchronous and blocking stream-oriented
 * functionality using SSL.
 *
 * @par Thread Safety
 * @e Distinct @e objects: Safe.@n
 * @e Shared @e objects: Unsafe. The application must also ensure that all
 * asynchronous operations are performed within the same implicit or explicit
 * strand.
 *
 * @par Example
 * To use the SSL stream template with an ip::tcp::socket, you would write:
 * @code
 * boost::asio::io_context my_context;
 * boost::asio::ssl::context ctx(boost::asio::ssl::context::sslv23);
 * boost::asio::ssl::stream<boost::asio::ip::tcp::socket> sock(my_context, ctx);
 * @endcode
 *
 * @par Concepts:
 * AsyncReadStream, AsyncWriteStream, Stream, SyncReadStream, SyncWriteStream.
 */
template <typename Stream>
class stream :
  public stream_base,
  private noncopyable
{
private:
  class initiate_async_handshake;
  class initiate_async_buffered_handshake;
  class initiate_async_shutdown;
  class initiate_async_write_some;
  class initiate_async_read_some;

public:
  /// The native handle type of the SSL stream.
  typedef SSL* native_handle_type;

  /// Structure for use with deprecated impl_type.
  struct impl_struct
  {
    SSL* ssl;
  };

  /// The type of the next layer.
  typedef remove_reference_t<Stream> next_layer_type;

  /// The type of the lowest layer.
  typedef typename next_layer_type::lowest_layer_type lowest_layer_type;

  /// The type of the executor associated with the object.
  typedef typename lowest_layer_type::executor_type executor_type;

  /// Construct a stream.
  /**
   * This constructor creates a stream and initialises the underlying stream
   * object.
   *
   * @param arg The argument to be passed to initialise the underlying stream.
   *
   * @param ctx The SSL context to be used for the stream.
   */
  template <typename Arg>
  stream(Arg&& arg, context& ctx)
    : next_layer_(static_cast<Arg&&>(arg)),
      core_(ctx.native_handle(), next_layer_.lowest_layer().get_executor())
  {
  }

  /// Construct a stream from an existing native implementation.
  /**
   * This constructor creates a stream and initialises the underlying stream
   * object. On success, ownership of the native implementation is transferred
   * to the stream, and it will be cleaned up when the stream is destroyed.
   *
   * @param arg The argument to be passed to initialise the underlying stream.
   *
   * @param handle An existing native SSL implementation.
   */
  template <typename Arg>
  stream(Arg&& arg, native_handle_type handle)
    : next_layer_(static_cast<Arg&&>(arg)),
      core_(handle, next_layer_.lowest_layer().get_executor())
  {
  }

  /// Move-construct a stream from another.
  /**
   * @param other The other stream object from which the move will occur. Must
   * have no outstanding asynchronous operations associated with it. Following
   * the move, @c other has a valid but unspecified state where the only safe
   * operation is destruction, or use as the target of a move assignment.
   */
  stream(stream&& other)
    : next_layer_(static_cast<Stream&&>(other.next_layer_)),
      core_(static_cast<detail::stream_core&&>(other.core_))
  {
  }

  /// Move-assign a stream from another.
  /**
   * @param other The other stream object from which the move will occur. Must
   * have no outstanding asynchronous operations associated with it. Following
   * the move, @c other has a valid but unspecified state where the only safe
   * operation is destruction, or use as the target of a move assignment.
   */
  stream& operator=(stream&& other)
  {
    if (this != &other)
    {
      next_layer_ = static_cast<Stream&&>(other.next_layer_);
      core_ = static_cast<detail::stream_core&&>(other.core_);
    }
    return *this;
  }

  /// Destructor.
  /**
   * @note A @c stream object must not be destroyed while there are pending
   * asynchronous operations associated with it.
   */
  ~stream()
  {
  }

  /// Get the executor associated with the object.
  /**
   * This function may be used to obtain the executor object that the stream
   * uses to dispatch handlers for asynchronous operations.
   *
   * @return A copy of the executor that stream will use to dispatch handlers.
   */
  executor_type get_executor() noexcept
  {
    return next_layer_.lowest_layer().get_executor();
  }

  /// Get the underlying implementation in the native type.
  /**
   * This function may be used to obtain the underlying implementation of the
   * context. This is intended to allow access to context functionality that is
   * not otherwise provided.
   *
   * @par Example
   * The native_handle() function returns a pointer of type @c SSL* that is
   * suitable for passing to functions such as @c SSL_get_verify_result and
   * @c SSL_get_peer_certificate:
   * @code
   * boost::asio::ssl::stream<boost::asio::ip::tcp::socket> sock(io_ctx, ctx);
   *
   * // ... establish connection and perform handshake ...
   *
   * if (X509* cert = SSL_get_peer_certificate(sock.native_handle()))
   * {
   *   if (SSL_get_verify_result(sock.native_handle()) == X509_V_OK)
   *   {
   *     // ...
   *   }
   * }
   * @endcode
   */
  native_handle_type native_handle()
  {
    return core_.engine_.native_handle();
  }

  /// Get a reference to the next layer.
  /**
   * This function returns a reference to the next layer in a stack of stream
   * layers.
   *
   * @return A reference to the next layer in the stack of stream layers.
   * Ownership is not transferred to the caller.
   */
  const next_layer_type& next_layer() const
  {
    return next_layer_;
  }

  /// Get a reference to the next layer.
  /**
   * This function returns a reference to the next layer in a stack of stream
   * layers.
   *
   * @return A reference to the next layer in the stack of stream layers.
   * Ownership is not transferred to the caller.
   */
  next_layer_type& next_layer()
  {
    return next_layer_;
  }

  /// Get a reference to the lowest layer.
  /**
   * This function returns a reference to the lowest layer in a stack of
   * stream layers.
   *
   * @return A reference to the lowest layer in the stack of stream layers.
   * Ownership is not transferred to the caller.
   */
  lowest_layer_type& lowest_layer()
  {
    return next_layer_.lowest_layer();
  }

  /// Get a reference to the lowest layer.
  /**
   * This function returns a reference to the lowest layer in a stack of
   * stream layers.
   *
   * @return A reference to the lowest layer in the stack of stream layers.
   * Ownership is not transferred to the caller.
   */
  const lowest_layer_type& lowest_layer() const
  {
    return next_layer_.lowest_layer();
  }

  /// Set the peer verification mode.
  /**
   * This function may be used to configure the peer verification mode used by
   * the stream. The new mode will override the mode inherited from the context.
   *
   * @param v A bitmask of peer verification modes. See @ref verify_mode for
   * available values.
   *
   * @throws boost::system::system_error Thrown on failure.
   *
   * @note Calls @c SSL_set_verify.
   */
  void set_verify_mode(verify_mode v)
  {
    boost::system::error_code ec;
    set_verify_mode(v, ec);
    boost::asio::detail::throw_error(ec, "set_verify_mode");
  }

  /// Set the peer verification mode.
  /**
   * This function may be used to configure the peer verification mode used by
   * the stream. The new mode will override the mode inherited from the context.
   *
   * @param v A bitmask of peer verification modes. See @ref verify_mode for
   * available values.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @note Calls @c SSL_set_verify.
   */
  BOOST_ASIO_SYNC_OP_VOID set_verify_mode(
      verify_mode v, boost::system::error_code& ec)
  {
    core_.engine_.set_verify_mode(v, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Set the peer verification depth.
  /**
   * This function may be used to configure the maximum verification depth
   * allowed by the stream.
   *
   * @param depth Maximum depth for the certificate chain verification that
   * shall be allowed.
   *
   * @throws boost::system::system_error Thrown on failure.
   *
   * @note Calls @c SSL_set_verify_depth.
   */
  void set_verify_depth(int depth)
  {
    boost::system::error_code ec;
    set_verify_depth(depth, ec);
    boost::asio::detail::throw_error(ec, "set_verify_depth");
  }

  /// Set the peer verification depth.
  /**
   * This function may be used to configure the maximum verification depth
   * allowed by the stream.
   *
   * @param depth Maximum depth for the certificate chain verification that
   * shall be allowed.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @note Calls @c SSL_set_verify_depth.
   */
  BOOST_ASIO_SYNC_OP_VOID set_verify_depth(
      int depth, boost::system::error_code& ec)
  {
    core_.engine_.set_verify_depth(depth, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Set the callback used to verify peer certificates.
  /**
   * This function is used to specify a callback function that will be called
   * by the implementation when it needs to verify a peer certificate.
   *
   * @param callback The function object to be used for verifying a certificate.
   * The function signature of the handler must be:
   * @code bool verify_callback(
   *   bool preverified, // True if the certificate passed pre-verification.
   *   verify_context& ctx // The peer certificate and other context.
   * ); @endcode
   * The return value of the callback is true if the certificate has passed
   * verification, false otherwise.
   *
   * @throws boost::system::system_error Thrown on failure.
   *
   * @note Calls @c SSL_set_verify.
   */
  template <typename VerifyCallback>
  void set_verify_callback(VerifyCallback callback)
  {
    boost::system::error_code ec;
    this->set_verify_callback(callback, ec);
    boost::asio::detail::throw_error(ec, "set_verify_callback");
  }

  /// Set the callback used to verify peer certificates.
  /**
   * This function is used to specify a callback function that will be called
   * by the implementation when it needs to verify a peer certificate.
   *
   * @param callback The function object to be used for verifying a certificate.
   * The function signature of the handler must be:
   * @code bool verify_callback(
   *   bool preverified, // True if the certificate passed pre-verification.
   *   verify_context& ctx // The peer certificate and other context.
   * ); @endcode
   * The return value of the callback is true if the certificate has passed
   * verification, false otherwise.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @note Calls @c SSL_set_verify.
   */
  template <typename VerifyCallback>
  BOOST_ASIO_SYNC_OP_VOID set_verify_callback(VerifyCallback callback,
      boost::system::error_code& ec)
  {
    core_.engine_.set_verify_callback(
        new detail::verify_callback<VerifyCallback>(callback), ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Perform SSL handshaking.
  /**
   * This function is used to perform SSL handshaking on the stream. The
   * function call will block until handshaking is complete or an error occurs.
   *
   * @param type The type of handshaking to be performed, i.e. as a client or as
   * a server.
   *
   * @throws boost::system::system_error Thrown on failure.
   */
  void handshake(handshake_type type)
  {
    boost::system::error_code ec;
    handshake(type, ec);
    boost::asio::detail::throw_error(ec, "handshake");
  }

  /// Perform SSL handshaking.
  /**
   * This function is used to perform SSL handshaking on the stream. The
   * function call will block until handshaking is complete or an error occurs.
   *
   * @param type The type of handshaking to be performed, i.e. as a client or as
   * a server.
   *
   * @param ec Set to indicate what error occurred, if any.
   */
  BOOST_ASIO_SYNC_OP_VOID handshake(handshake_type type,
      boost::system::error_code& ec)
  {
    detail::io(next_layer_, core_, detail::handshake_op(type), ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Perform SSL handshaking.
  /**
   * This function is used to perform SSL handshaking on the stream. The
   * function call will block until handshaking is complete or an error occurs.
   *
   * @param type The type of handshaking to be performed, i.e. as a client or as
   * a server.
   *
   * @param buffers The buffered data to be reused for the handshake.
   *
   * @throws boost::system::system_error Thrown on failure.
   */
  template <typename ConstBufferSequence>
  void handshake(handshake_type type, const ConstBufferSequence& buffers)
  {
    boost::system::error_code ec;
    handshake(type, buffers, ec);
    boost::asio::detail::throw_error(ec, "handshake");
  }

  /// Perform SSL handshaking.
  /**
   * This function is used to perform SSL handshaking on the stream. The
   * function call will block until handshaking is complete or an error occurs.
   *
   * @param type The type of handshaking to be performed, i.e. as a client or as
   * a server.
   *
   * @param buffers The buffered data to be reused for the handshake.
   *
   * @param ec Set to indicate what error occurred, if any.
   */
  template <typename ConstBufferSequence>
  BOOST_ASIO_SYNC_OP_VOID handshake(handshake_type type,
      const ConstBufferSequence& buffers, boost::system::error_code& ec)
  {
    detail::io(next_layer_, core_,
        detail::buffered_handshake_op<ConstBufferSequence>(type, buffers), ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Start an asynchronous SSL handshake.
  /**
   * This function is used to asynchronously perform an SSL handshake on the
   * stream. It is an initiating function for an @ref asynchronous_operation,
   * and always returns immediately.
   *
   * @param type The type of handshaking to be performed, i.e. as a client or as
   * a server.
   *
   * @param token The @ref completion_token that will be used to produce a
   * completion handler, which will be called when the handshake completes.
   * Potential completion tokens include @ref use_future, @ref use_awaitable,
   * @ref yield_context, or a function object with the correct completion
   * signature. The function signature of the completion handler must be:
   * @code void handler(
   *   const boost::system::error_code& error // Result of operation.
   * ); @endcode
   * Regardless of whether the asynchronous operation completes immediately or
   * not, the completion handler will not be invoked from within this function.
   * On immediate completion, invocation of the handler will be performed in a
   * manner equivalent to using boost::asio::async_immediate().
   *
   * @par Completion Signature
   * @code void(boost::system::error_code) @endcode
   *
   * @par Per-Operation Cancellation
   * This asynchronous operation supports cancellation for the following
   * boost::asio::cancellation_type values:
   *
   * @li @c cancellation_type::terminal
   *
   * @li @c cancellation_type::partial
   *
   * if they are also supported by the @c Stream type's @c async_read_some and
   * @c async_write_some operations.
   */
  template <
      BOOST_ASIO_COMPLETION_TOKEN_FOR(void (boost::system::error_code))
        HandshakeToken = default_completion_token_t<executor_type>>
  auto async_handshake(handshake_type type,
      HandshakeToken&& token = default_completion_token_t<executor_type>())
    -> decltype(
      async_initiate<HandshakeToken,
        void (boost::system::error_code)>(
          declval<initiate_async_handshake>(), token, type))
  {
    return async_initiate<HandshakeToken,
      void (boost::system::error_code)>(
        initiate_async_handshake(this), token, type);
  }

  /// Start an asynchronous SSL handshake.
  /**
   * This function is used to asynchronously perform an SSL handshake on the
   * stream. It is an initiating function for an @ref asynchronous_operation,
   * and always returns immediately.
   *
   * @param type The type of handshaking to be performed, i.e. as a client or as
   * a server.
   *
   * @param buffers The buffered data to be reused for the handshake. Although
   * the buffers object may be copied as necessary, ownership of the underlying
   * buffers is retained by the caller, which must guarantee that they remain
   * valid until the completion handler is called.
   *
   * @param token The @ref completion_token that will be used to produce a
   * completion handler, which will be called when the handshake completes.
   * Potential completion tokens include @ref use_future, @ref use_awaitable,
   * @ref yield_context, or a function object with the correct completion
   * signature. The function signature of the completion handler must be:
   * @code void handler(
   *   const boost::system::error_code& error, // Result of operation.
   *   std::size_t bytes_transferred // Amount of buffers used in handshake.
   * ); @endcode
   * Regardless of whether the asynchronous operation completes immediately or
   * not, the completion handler will not be invoked from within this function.
   * On immediate completion, invocation of the handler will be performed in a
   * manner equivalent to using boost::asio::async_immediate().
   *
   * @par Completion Signature
   * @code void(boost::system::error_code, std::size_t) @endcode
   *
   * @par Per-Operation Cancellation
   * This asynchronous operation supports cancellation for the following
   * boost::asio::cancellation_type values:
   *
   * @li @c cancellation_type::terminal
   *
   * @li @c cancellation_type::partial
   *
   * if they are also supported by the @c Stream type's @c async_read_some and
   * @c async_write_some operations.
   */
  template <typename ConstBufferSequence,
      BOOST_ASIO_COMPLETION_TOKEN_FOR(void (boost::system::error_code,
        std::size_t)) BufferedHandshakeToken
          = default_completion_token_t<executor_type>>
  auto async_handshake(handshake_type type, const ConstBufferSequence& buffers,
      BufferedHandshakeToken&& token
        = default_completion_token_t<executor_type>(),
      constraint_t<
        is_const_buffer_sequence<ConstBufferSequence>::value
      > = 0)
    -> decltype(
      async_initiate<BufferedHandshakeToken,
        void (boost::system::error_code, std::size_t)>(
          declval<initiate_async_buffered_handshake>(), token, type, buffers))
  {
    return async_initiate<BufferedHandshakeToken,
      void (boost::system::error_code, std::size_t)>(
        initiate_async_buffered_handshake(this), token, type, buffers);
  }

  /// Shut down SSL on the stream.
  /**
   * This function is used to shut down SSL on the stream. The function call
   * will block until SSL has been shut down or an error occurs.
   *
   * @throws boost::system::system_error Thrown on failure.
   */
  void shutdown()
  {
    boost::system::error_code ec;
    shutdown(ec);
    boost::asio::detail::throw_error(ec, "shutdown");
  }

  /// Shut down SSL on the stream.
  /**
   * This function is used to shut down SSL on the stream. The function call
   * will block until SSL has been shut down or an error occurs.
   *
   * @param ec Set to indicate what error occurred, if any.
   */
  BOOST_ASIO_SYNC_OP_VOID shutdown(boost::system::error_code& ec)
  {
    detail::io(next_layer_, core_, detail::shutdown_op(), ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Asynchronously shut down SSL on the stream.
  /**
   * This function is used to asynchronously shut down SSL on the stream. It is
   * an initiating function for an @ref asynchronous_operation, and always
   * returns immediately.
   *
   * @param token The @ref completion_token that will be used to produce a
   * completion handler, which will be called when the shutdown completes.
   * Potential completion tokens include @ref use_future, @ref use_awaitable,
   * @ref yield_context, or a function object with the correct completion
   * signature. The function signature of the completion handler must be:
   * @code void handler(
   *   const boost::system::error_code& error // Result of operation.
   * ); @endcode
   * Regardless of whether the asynchronous operation completes immediately or
   * not, the completion handler will not be invoked from within this function.
   * On immediate completion, invocation of the handler will be performed in a
   * manner equivalent to using boost::asio::async_immediate().
   *
   * @par Completion Signature
   * @code void(boost::system::error_code) @endcode
   *
   * @par Per-Operation Cancellation
   * This asynchronous operation supports cancellation for the following
   * boost::asio::cancellation_type values:
   *
   * @li @c cancellation_type::terminal
   *
   * @li @c cancellation_type::partial
   *
   * if they are also supported by the @c Stream type's @c async_read_some and
   * @c async_write_some operations.
   */
  template <
      BOOST_ASIO_COMPLETION_TOKEN_FOR(void (boost::system::error_code))
        ShutdownToken
          = default_completion_token_t<executor_type>>
  auto async_shutdown(
      ShutdownToken&& token = default_completion_token_t<executor_type>())
    -> decltype(
      async_initiate<ShutdownToken,
        void (boost::system::error_code)>(
          declval<initiate_async_shutdown>(), token))
  {
    return async_initiate<ShutdownToken,
      void (boost::system::error_code)>(
        initiate_async_shutdown(this), token);
  }

  /// Write some data to the stream.
  /**
   * This function is used to write data on the stream. The function call will
   * block until one or more bytes of data has been written successfully, or
   * until an error occurs.
   *
   * @param buffers The data to be written.
   *
   * @returns The number of bytes written.
   *
   * @throws boost::system::system_error Thrown on failure.
   *
   * @note The write_some operation may not transmit all of the data to the
   * peer. Consider using the @ref write function if you need to ensure that all
   * data is written before the blocking operation completes.
   */
  template <typename ConstBufferSequence>
  std::size_t write_some(const ConstBufferSequence& buffers)
  {
    boost::system::error_code ec;
    std::size_t n = write_some(buffers, ec);
    boost::asio::detail::throw_error(ec, "write_some");
    return n;
  }

  /// Write some data to the stream.
  /**
   * This function is used to write data on the stream. The function call will
   * block until one or more bytes of data has been written successfully, or
   * until an error occurs.
   *
   * @param buffers The data to be written to the stream.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @returns The number of bytes written. Returns 0 if an error occurred.
   *
   * @note The write_some operation may not transmit all of the data to the
   * peer. Consider using the @ref write function if you need to ensure that all
   * data is written before the blocking operation completes.
   */
  template <typename ConstBufferSequence>
  std::size_t write_some(const ConstBufferSequence& buffers,
      boost::system::error_code& ec)
  {
    return detail::io(next_layer_, core_,
        detail::write_op<ConstBufferSequence>(buffers), ec);
  }

  /// Start an asynchronous write.
  /**
   * This function is used to asynchronously write one or more bytes of data to
   * the stream. It is an initiating function for an @ref
   * asynchronous_operation, and always returns immediately.
   *
   * @param buffers The data to be written to the stream. Although the buffers
   * object may be copied as necessary, ownership of the underlying buffers is
   * retained by the caller, which must guarantee that they remain valid until
   * the completion handler is called.
   *
   * @param token The @ref completion_token that will be used to produce a
   * completion handler, which will be called when the write completes.
   * Potential completion tokens include @ref use_future, @ref use_awaitable,
   * @ref yield_context, or a function object with the correct completion
   * signature. The function signature of the completion handler must be:
   * @code void handler(
   *   const boost::system::error_code& error, // Result of operation.
   *   std::size_t bytes_transferred // Number of bytes written.
   * ); @endcode
   * Regardless of whether the asynchronous operation completes immediately or
   * not, the completion handler will not be invoked from within this function.
   * On immediate completion, invocation of the handler will be performed in a
   * manner equivalent to using boost::asio::async_immediate().
   *
   * @par Completion Signature
   * @code void(boost::system::error_code, std::size_t) @endcode
   *
   * @note The async_write_some operation may not transmit all of the data to
   * the peer. Consider using the @ref async_write function if you need to
   * ensure that all data is written before the asynchronous operation
   * completes.
   *
   * @par Per-Operation Cancellation
   * This asynchronous operation supports cancellation for the following
   * boost::asio::cancellation_type values:
   *
   * @li @c cancellation_type::terminal
   *
   * @li @c cancellation_type::partial
   *
   * if they are also supported by the @c Stream type's @c async_read_some and
   * @c async_write_some operations.
   */
  template <typename ConstBufferSequence,
      BOOST_ASIO_COMPLETION_TOKEN_FOR(void (boost::system::error_code,
        std::size_t)) WriteToken = default_completion_token_t<executor_type>>
  auto async_write_some(const ConstBufferSequence& buffers,
      WriteToken&& token = default_completion_token_t<executor_type>())
    -> decltype(
      async_initiate<WriteToken,
        void (boost::system::error_code, std::size_t)>(
          declval<initiate_async_write_some>(), token, buffers))
  {
    return async_initiate<WriteToken,
      void (boost::system::error_code, std::size_t)>(
        initiate_async_write_some(this), token, buffers);
  }

  /// Read some data from the stream.
  /**
   * This function is used to read data from the stream. The function call will
   * block until one or more bytes of data has been read successfully, or until
   * an error occurs.
   *
   * @param buffers The buffers into which the data will be read.
   *
   * @returns The number of bytes read.
   *
   * @throws boost::system::system_error Thrown on failure.
   *
   * @note The read_some operation may not read all of the requested number of
   * bytes. Consider using the @ref read function if you need to ensure that the
   * requested amount of data is read before the blocking operation completes.
   */
  template <typename MutableBufferSequence>
  std::size_t read_some(const MutableBufferSequence& buffers)
  {
    boost::system::error_code ec;
    std::size_t n = read_some(buffers, ec);
    boost::asio::detail::throw_error(ec, "read_some");
    return n;
  }

  /// Read some data from the stream.
  /**
   * This function is used to read data from the stream. The function call will
   * block until one or more bytes of data has been read successfully, or until
   * an error occurs.
   *
   * @param buffers The buffers into which the data will be read.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @returns The number of bytes read. Returns 0 if an error occurred.
   *
   * @note The read_some operation may not read all of the requested number of
   * bytes. Consider using the @ref read function if you need to ensure that the
   * requested amount of data is read before the blocking operation completes.
   */
  template <typename MutableBufferSequence>
  std::size_t read_some(const MutableBufferSequence& buffers,
      boost::system::error_code& ec)
  {
    return detail::io(next_layer_, core_,
        detail::read_op<MutableBufferSequence>(buffers), ec);
  }

  /// Start an asynchronous read.
  /**
   * This function is used to asynchronously read one or more bytes of data from
   * the stream. It is an initiating function for an @ref
   * asynchronous_operation, and always returns immediately.
   *
   * @param buffers The buffers into which the data will be read. Although the
   * buffers object may be copied as necessary, ownership of the underlying
   * buffers is retained by the caller, which must guarantee that they remain
   * valid until the completion handler is called.
   *
   * @param token The @ref completion_token that will be used to produce a
   * completion handler, which will be called when the read completes.
   * Potential completion tokens include @ref use_future, @ref use_awaitable,
   * @ref yield_context, or a function object with the correct completion
   * signature. The function signature of the completion handler must be:
   * @code void handler(
   *   const boost::system::error_code& error, // Result of operation.
   *   std::size_t bytes_transferred // Number of bytes read.
   * ); @endcode
   * Regardless of whether the asynchronous operation completes immediately or
   * not, the completion handler will not be invoked from within this function.
   * On immediate completion, invocation of the handler will be performed in a
   * manner equivalent to using boost::asio::async_immediate().
   *
   * @par Completion Signature
   * @code void(boost::system::error_code, std::size_t) @endcode
   *
   * @note The async_read_some operation may not read all of the requested
   * number of bytes. Consider using the @ref async_read function if you need to
   * ensure that the requested amount of data is read before the asynchronous
   * operation completes.
   *
   * @par Per-Operation Cancellation
   * This asynchronous operation supports cancellation for the following
   * boost::asio::cancellation_type values:
   *
   * @li @c cancellation_type::terminal
   *
   * @li @c cancellation_type::partial
   *
   * if they are also supported by the @c Stream type's @c async_read_some and
   * @c async_write_some operations.
   */
  template <typename MutableBufferSequence,
      BOOST_ASIO_COMPLETION_TOKEN_FOR(void (boost::system::error_code,
        std::size_t)) ReadToken = default_completion_token_t<executor_type>>
  auto async_read_some(const MutableBufferSequence& buffers,
      ReadToken&& token = default_completion_token_t<executor_type>())
    -> decltype(
      async_initiate<ReadToken,
        void (boost::system::error_code, std::size_t)>(
          declval<initiate_async_read_some>(), token, buffers))
  {
    return async_initiate<ReadToken,
      void (boost::system::error_code, std::size_t)>(
        initiate_async_read_some(this), token, buffers);
  }

private:
  class initiate_async_handshake
  {
  public:
    typedef typename stream::executor_type executor_type;

    explicit initiate_async_handshake(stream* self)
      : self_(self)
    {
    }

    executor_type get_executor() const noexcept
    {
      return self_->get_executor();
    }

    template <typename HandshakeHandler>
    void operator()(HandshakeHandler&& handler,
        handshake_type type) const
    {
      // If you get an error on the following line it means that your handler
      // does not meet the documented type requirements for a HandshakeHandler.
      BOOST_ASIO_HANDSHAKE_HANDLER_CHECK(HandshakeHandler, handler) type_check;

      boost::asio::detail::non_const_lvalue<HandshakeHandler> handler2(handler);
      detail::async_io(self_->next_layer_, self_->core_,
          detail::handshake_op(type), handler2.value);
    }

  private:
    stream* self_;
  };

  class initiate_async_buffered_handshake
  {
  public:
    typedef typename stream::executor_type executor_type;

    explicit initiate_async_buffered_handshake(stream* self)
      : self_(self)
    {
    }

    executor_type get_executor() const noexcept
    {
      return self_->get_executor();
    }

    template <typename BufferedHandshakeHandler, typename ConstBufferSequence>
    void operator()(BufferedHandshakeHandler&& handler,
        handshake_type type, const ConstBufferSequence& buffers) const
    {
      // If you get an error on the following line it means that your
      // handler does not meet the documented type requirements for a
      // BufferedHandshakeHandler.
      BOOST_ASIO_BUFFERED_HANDSHAKE_HANDLER_CHECK(
          BufferedHandshakeHandler, handler) type_check;

      boost::asio::detail::non_const_lvalue<
          BufferedHandshakeHandler> handler2(handler);
      detail::async_io(self_->next_layer_, self_->core_,
          detail::buffered_handshake_op<ConstBufferSequence>(type, buffers),
          handler2.value);
    }

  private:
    stream* self_;
  };

  class initiate_async_shutdown
  {
  public:
    typedef typename stream::executor_type executor_type;

    explicit initiate_async_shutdown(stream* self)
      : self_(self)
    {
    }

    executor_type get_executor() const noexcept
    {
      return self_->get_executor();
    }

    template <typename ShutdownHandler>
    void operator()(ShutdownHandler&& handler) const
    {
      // If you get an error on the following line it means that your handler
      // does not meet the documented type requirements for a ShutdownHandler.
      BOOST_ASIO_HANDSHAKE_HANDLER_CHECK(ShutdownHandler, handler) type_check;

      boost::asio::detail::non_const_lvalue<ShutdownHandler> handler2(handler);
      detail::async_io(self_->next_layer_, self_->core_,
          detail::shutdown_op(), handler2.value);
    }

  private:
    stream* self_;
  };

  class initiate_async_write_some
  {
  public:
    typedef typename stream::executor_type executor_type;

    explicit initiate_async_write_some(stream* self)
      : self_(self)
    {
    }

    executor_type get_executor() const noexcept
    {
      return self_->get_executor();
    }

    template <typename WriteHandler, typename ConstBufferSequence>
    void operator()(WriteHandler&& handler,
        const ConstBufferSequence& buffers) const
    {
      // If you get an error on the following line it means that your handler
      // does not meet the documented type requirements for a WriteHandler.
      BOOST_ASIO_WRITE_HANDLER_CHECK(WriteHandler, handler) type_check;

      boost::asio::detail::non_const_lvalue<WriteHandler> handler2(handler);
      detail::async_io(self_->next_layer_, self_->core_,
          detail::write_op<ConstBufferSequence>(buffers), handler2.value);
    }

  private:
    stream* self_;
  };

  class initiate_async_read_some
  {
  public:
    typedef typename stream::executor_type executor_type;

    explicit initiate_async_read_some(stream* self)
      : self_(self)
    {
    }

    executor_type get_executor() const noexcept
    {
      return self_->get_executor();
    }

    template <typename ReadHandler, typename MutableBufferSequence>
    void operator()(ReadHandler&& handler,
        const MutableBufferSequence& buffers) const
    {
      // If you get an error on the following line it means that your handler
      // does not meet the documented type requirements for a ReadHandler.
      BOOST_ASIO_READ_HANDLER_CHECK(ReadHandler, handler) type_check;

      boost::asio::detail::non_const_lvalue<ReadHandler> handler2(handler);
      detail::async_io(self_->next_layer_, self_->core_,
          detail::read_op<MutableBufferSequence>(buffers), handler2.value);
    }

  private:
    stream* self_;
  };

  Stream next_layer_;
  detail::stream_core core_;
};

} // namespace ssl
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_SSL_STREAM_HPP
