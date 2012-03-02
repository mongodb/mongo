//
// basic_socket_iostream.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2012 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_BASIC_SOCKET_IOSTREAM_HPP
#define BOOST_ASIO_BASIC_SOCKET_IOSTREAM_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#if !defined(BOOST_NO_IOSTREAM)

#include <boost/utility/base_from_member.hpp>
#include <boost/asio/basic_socket_streambuf.hpp>
#include <boost/asio/stream_socket_service.hpp>

#if !defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

# include <boost/preprocessor/arithmetic/inc.hpp>
# include <boost/preprocessor/repetition/enum_binary_params.hpp>
# include <boost/preprocessor/repetition/enum_params.hpp>
# include <boost/preprocessor/repetition/repeat_from_to.hpp>

# if !defined(BOOST_ASIO_SOCKET_IOSTREAM_MAX_ARITY)
#  define BOOST_ASIO_SOCKET_IOSTREAM_MAX_ARITY 5
# endif // !defined(BOOST_ASIO_SOCKET_IOSTREAM_MAX_ARITY)

// A macro that should expand to:
//   template <typename T1, ..., typename Tn>
//   explicit basic_socket_iostream(T1 x1, ..., Tn xn)
//     : basic_iostream<char>(&this->boost::base_from_member<
//         basic_socket_streambuf<Protocol, StreamSocketService,
//           Time, TimeTraits, TimerService> >::member)
//   {
//     if (rdbuf()->connect(x1, ..., xn) == 0)
//       this->setstate(std::ios_base::failbit);
//   }
// This macro should only persist within this file.

# define BOOST_ASIO_PRIVATE_CTR_DEF(z, n, data) \
  template <BOOST_PP_ENUM_PARAMS(n, typename T)> \
  explicit basic_socket_iostream(BOOST_PP_ENUM_BINARY_PARAMS(n, T, x)) \
    : std::basic_iostream<char>(&this->boost::base_from_member< \
        basic_socket_streambuf<Protocol, StreamSocketService, \
          Time, TimeTraits, TimerService> >::member) \
  { \
    tie(this); \
    if (rdbuf()->connect(BOOST_PP_ENUM_PARAMS(n, x)) == 0) \
      this->setstate(std::ios_base::failbit); \
  } \
  /**/

// A macro that should expand to:
//   template <typename T1, ..., typename Tn>
//   void connect(T1 x1, ..., Tn xn)
//   {
//     if (rdbuf()->connect(x1, ..., xn) == 0)
//       this->setstate(std::ios_base::failbit);
//   }
// This macro should only persist within this file.

# define BOOST_ASIO_PRIVATE_CONNECT_DEF(z, n, data) \
  template <BOOST_PP_ENUM_PARAMS(n, typename T)> \
  void connect(BOOST_PP_ENUM_BINARY_PARAMS(n, T, x)) \
  { \
    if (rdbuf()->connect(BOOST_PP_ENUM_PARAMS(n, x)) == 0) \
      this->setstate(std::ios_base::failbit); \
  } \
  /**/

#endif // !defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

/// Iostream interface for a socket.
template <typename Protocol,
    typename StreamSocketService = stream_socket_service<Protocol>,
    typename Time = boost::posix_time::ptime,
    typename TimeTraits = boost::asio::time_traits<Time>,
    typename TimerService = deadline_timer_service<Time, TimeTraits> >
class basic_socket_iostream
  : public boost::base_from_member<
      basic_socket_streambuf<Protocol, StreamSocketService,
        Time, TimeTraits, TimerService> >,
    public std::basic_iostream<char>
{
public:
  /// The endpoint type.
  typedef typename Protocol::endpoint endpoint_type;

  /// The time type.
  typedef typename TimeTraits::time_type time_type;

  /// The duration type.
  typedef typename TimeTraits::duration_type duration_type;

  /// Construct a basic_socket_iostream without establishing a connection.
  basic_socket_iostream()
    : std::basic_iostream<char>(&this->boost::base_from_member<
        basic_socket_streambuf<Protocol, StreamSocketService,
          Time, TimeTraits, TimerService> >::member)
  {
    tie(this);
  }

#if defined(GENERATING_DOCUMENTATION)
  /// Establish a connection to an endpoint corresponding to a resolver query.
  /**
   * This constructor automatically establishes a connection based on the
   * supplied resolver query parameters. The arguments are used to construct
   * a resolver query object.
   */
  template <typename T1, ..., typename TN>
  explicit basic_socket_iostream(T1 t1, ..., TN tn);
#elif defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)
  template <typename... T>
  explicit basic_socket_iostream(T... x)
    : std::basic_iostream<char>(&this->boost::base_from_member<
        basic_socket_streambuf<Protocol, StreamSocketService,
          Time, TimeTraits, TimerService> >::member)
  {
    tie(this);
    if (rdbuf()->connect(x...) == 0)
      this->setstate(std::ios_base::failbit);
  }
#else
  BOOST_PP_REPEAT_FROM_TO(
      1, BOOST_PP_INC(BOOST_ASIO_SOCKET_IOSTREAM_MAX_ARITY),
      BOOST_ASIO_PRIVATE_CTR_DEF, _ )
#endif

#if defined(GENERATING_DOCUMENTATION)
  /// Establish a connection to an endpoint corresponding to a resolver query.
  /**
   * This function automatically establishes a connection based on the supplied
   * resolver query parameters. The arguments are used to construct a resolver
   * query object.
   */
  template <typename T1, ..., typename TN>
  void connect(T1 t1, ..., TN tn);
#elif defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)
  template <typename... T>
  void connect(T... x)
  {
    if (rdbuf()->connect(x...) == 0)
      this->setstate(std::ios_base::failbit);
  }
#else
  BOOST_PP_REPEAT_FROM_TO(
      1, BOOST_PP_INC(BOOST_ASIO_SOCKET_IOSTREAM_MAX_ARITY),
      BOOST_ASIO_PRIVATE_CONNECT_DEF, _ )
#endif

  /// Close the connection.
  void close()
  {
    if (rdbuf()->close() == 0)
      this->setstate(std::ios_base::failbit);
  }

  /// Return a pointer to the underlying streambuf.
  basic_socket_streambuf<Protocol, StreamSocketService,
    Time, TimeTraits, TimerService>* rdbuf() const
  {
    return const_cast<basic_socket_streambuf<Protocol, StreamSocketService,
      Time, TimeTraits, TimerService>*>(
        &this->boost::base_from_member<
          basic_socket_streambuf<Protocol, StreamSocketService,
            Time, TimeTraits, TimerService> >::member);
  }

  /// Get the last error associated with the stream.
  /**
   * @return An \c error_code corresponding to the last error from the stream.
   *
   * @par Example
   * To print the error associated with a failure to establish a connection:
   * @code tcp::iostream s("www.boost.org", "http");
   * if (!s)
   * {
   *   std::cout << "Error: " << s.error().message() << std::endl;
   * } @endcode
   */
  const boost::system::error_code& error() const
  {
    return rdbuf()->puberror();
  }

  /// Get the stream's expiry time as an absolute time.
  /**
   * @return An absolute time value representing the stream's expiry time.
   */
  time_type expires_at() const
  {
    return rdbuf()->expires_at();
  }

  /// Set the stream's expiry time as an absolute time.
  /**
   * This function sets the expiry time associated with the stream. Stream
   * operations performed after this time (where the operations cannot be
   * completed using the internal buffers) will fail with the error
   * boost::asio::error::operation_aborted.
   *
   * @param expiry_time The expiry time to be used for the stream.
   */
  void expires_at(const time_type& expiry_time)
  {
    rdbuf()->expires_at(expiry_time);
  }

  /// Get the timer's expiry time relative to now.
  /**
   * @return A relative time value representing the stream's expiry time.
   */
  duration_type expires_from_now() const
  {
    return rdbuf()->expires_from_now();
  }

  /// Set the stream's expiry time relative to now.
  /**
   * This function sets the expiry time associated with the stream. Stream
   * operations performed after this time (where the operations cannot be
   * completed using the internal buffers) will fail with the error
   * boost::asio::error::operation_aborted.
   *
   * @param expiry_time The expiry time to be used for the timer.
   */
  void expires_from_now(const duration_type& expiry_time)
  {
    rdbuf()->expires_from_now(expiry_time);
  }
};

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#if !defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)
# undef BOOST_ASIO_PRIVATE_CTR_DEF
# undef BOOST_ASIO_PRIVATE_CONNECT_DEF
#endif // !defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

#endif // defined(BOOST_NO_IOSTREAM)

#endif // BOOST_ASIO_BASIC_SOCKET_IOSTREAM_HPP
