//
// basic_socket_streambuf.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_BASIC_SOCKET_STREAMBUF_HPP
#define ASIO_BASIC_SOCKET_STREAMBUF_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"

#if !defined(ASIO_NO_IOSTREAM)

#include <streambuf>
#include "asio/basic_socket.hpp"
#include "asio/deadline_timer_service.hpp"
#include "asio/detail/array.hpp"
#include "asio/detail/throw_error.hpp"
#include "asio/io_service.hpp"
#include "asio/stream_socket_service.hpp"

#if defined(ASIO_HAS_BOOST_DATE_TIME)
# include "asio/deadline_timer.hpp"
#else
# include "asio/steady_timer.hpp"
#endif

#if !defined(ASIO_HAS_VARIADIC_TEMPLATES)

# include "asio/detail/variadic_templates.hpp"

// A macro that should expand to:
//   template <typename T1, ..., typename Tn>
//   basic_socket_streambuf<Protocol, StreamSocketService,
//     Time, TimeTraits, TimerService>* connect(
//       T1 x1, ..., Tn xn)
//   {
//     init_buffers();
//     this->basic_socket<Protocol, StreamSocketService>::close(ec_);
//     typedef typename Protocol::resolver resolver_type;
//     typedef typename resolver_type::query resolver_query;
//     resolver_query query(x1, ..., xn);
//     resolve_and_connect(query);
//     return !ec_ ? this : 0;
//   }
// This macro should only persist within this file.

# define ASIO_PRIVATE_CONNECT_DEF(n) \
  template <ASIO_VARIADIC_TPARAMS(n)> \
  basic_socket_streambuf<Protocol, StreamSocketService, \
    Time, TimeTraits, TimerService>* connect( \
      ASIO_VARIADIC_BYVAL_PARAMS(n)) \
  { \
    init_buffers(); \
    this->basic_socket<Protocol, StreamSocketService>::close(ec_); \
    typedef typename Protocol::resolver resolver_type; \
    typedef typename resolver_type::query resolver_query; \
    resolver_query query(ASIO_VARIADIC_BYVAL_ARGS(n)); \
    resolve_and_connect(query); \
    return !ec_ ? this : 0; \
  } \
  /**/

#endif // !defined(ASIO_HAS_VARIADIC_TEMPLATES)

#include "asio/detail/push_options.hpp"

namespace asio {
namespace detail {

// A separate base class is used to ensure that the io_service is initialised
// prior to the basic_socket_streambuf's basic_socket base class.
class socket_streambuf_base
{
protected:
  io_service io_service_;
};

} // namespace detail

/// Iostream streambuf for a socket.
template <typename Protocol,
    typename StreamSocketService = stream_socket_service<Protocol>,
#if defined(ASIO_HAS_BOOST_DATE_TIME) \
  || defined(GENERATING_DOCUMENTATION)
    typename Time = boost::posix_time::ptime,
    typename TimeTraits = asio::time_traits<Time>,
    typename TimerService = deadline_timer_service<Time, TimeTraits> >
#else
    typename Time = steady_timer::clock_type,
    typename TimeTraits = steady_timer::traits_type,
    typename TimerService = steady_timer::service_type>
#endif
class basic_socket_streambuf
  : public std::streambuf,
    private detail::socket_streambuf_base,
    public basic_socket<Protocol, StreamSocketService>
{
private:
  // These typedefs are intended keep this class's implementation independent
  // of whether it's using Boost.DateTime, Boost.Chrono or std::chrono.
#if defined(ASIO_HAS_BOOST_DATE_TIME)
  typedef TimeTraits traits_helper;
#else
  typedef detail::chrono_time_traits<Time, TimeTraits> traits_helper;
#endif

public:
  /// The endpoint type.
  typedef typename Protocol::endpoint endpoint_type;

#if defined(GENERATING_DOCUMENTATION)
  /// (Deprecated: Use time_point.) The time type.
  typedef typename TimeTraits::time_type time_type;

  /// The time type.
  typedef typename TimeTraits::time_point time_point;

  /// (Deprecated: Use duration.) The duration type.
  typedef typename TimeTraits::duration_type duration_type;

  /// The duration type.
  typedef typename TimeTraits::duration duration;
#else
# if !defined(ASIO_NO_DEPRECATED)
  typedef typename traits_helper::time_type time_type;
  typedef typename traits_helper::duration_type duration_type;
# endif // !defined(ASIO_NO_DEPRECATED)
  typedef typename traits_helper::time_type time_point;
  typedef typename traits_helper::duration_type duration;
#endif

  /// Construct a basic_socket_streambuf without establishing a connection.
  basic_socket_streambuf()
    : basic_socket<Protocol, StreamSocketService>(
        this->detail::socket_streambuf_base::io_service_),
      unbuffered_(false),
      timer_service_(0),
      timer_state_(no_timer)
  {
    init_buffers();
  }

  /// Destructor flushes buffered data.
  virtual ~basic_socket_streambuf()
  {
    if (pptr() != pbase())
      overflow(traits_type::eof());

    destroy_timer();
  }

  /// Establish a connection.
  /**
   * This function establishes a connection to the specified endpoint.
   *
   * @return \c this if a connection was successfully established, a null
   * pointer otherwise.
   */
  basic_socket_streambuf<Protocol, StreamSocketService,
    Time, TimeTraits, TimerService>* connect(
      const endpoint_type& endpoint)
  {
    init_buffers();

    this->basic_socket<Protocol, StreamSocketService>::close(ec_);

    if (timer_state_ == timer_has_expired)
    {
      ec_ = asio::error::operation_aborted;
      return 0;
    }

    io_handler handler = { this };
    this->basic_socket<Protocol, StreamSocketService>::async_connect(
        endpoint, handler);

    ec_ = asio::error::would_block;
    this->get_service().get_io_service().restart();
    do this->get_service().get_io_service().run_one();
    while (ec_ == asio::error::would_block);

    return !ec_ ? this : 0;
  }

#if defined(GENERATING_DOCUMENTATION)
  /// Establish a connection.
  /**
   * This function automatically establishes a connection based on the supplied
   * resolver query parameters. The arguments are used to construct a resolver
   * query object.
   *
   * @return \c this if a connection was successfully established, a null
   * pointer otherwise.
   */
  template <typename T1, ..., typename TN>
  basic_socket_streambuf<Protocol, StreamSocketService>* connect(
      T1 t1, ..., TN tn);
#elif defined(ASIO_HAS_VARIADIC_TEMPLATES)
  template <typename... T>
  basic_socket_streambuf<Protocol, StreamSocketService,
    Time, TimeTraits, TimerService>* connect(T... x)
  {
    init_buffers();
    this->basic_socket<Protocol, StreamSocketService>::close(ec_);
    typedef typename Protocol::resolver resolver_type;
    typedef typename resolver_type::query resolver_query;
    resolver_query query(x...);
    resolve_and_connect(query);
    return !ec_ ? this : 0;
  }
#else
  ASIO_VARIADIC_GENERATE(ASIO_PRIVATE_CONNECT_DEF)
#endif

  /// Close the connection.
  /**
   * @return \c this if a connection was successfully established, a null
   * pointer otherwise.
   */
  basic_socket_streambuf<Protocol, StreamSocketService,
    Time, TimeTraits, TimerService>* close()
  {
    sync();
    this->basic_socket<Protocol, StreamSocketService>::close(ec_);
    if (!ec_)
      init_buffers();
    return !ec_ ? this : 0;
  }

  /// Get the last error associated with the stream buffer.
  /**
   * @return An \c error_code corresponding to the last error from the stream
   * buffer.
   */
  const asio::error_code& puberror() const
  {
    return error();
  }

#if !defined(ASIO_NO_DEPRECATED)
  /// (Deprecated: Use expiry().) Get the stream buffer's expiry time as an
  /// absolute time.
  /**
   * @return An absolute time value representing the stream buffer's expiry
   * time.
   */
  time_point expires_at() const
  {
    return timer_service_
      ? timer_service_->expires_at(timer_implementation_)
      : time_point();
  }
#endif // !defined(ASIO_NO_DEPRECATED)

  /// Get the stream buffer's expiry time as an absolute time.
  /**
   * @return An absolute time value representing the stream buffer's expiry
   * time.
   */
  time_point expiry() const
  {
    return timer_service_
#if defined(ASIO_HAS_BOOST_DATE_TIME)
      ? timer_service_->expires_at(timer_implementation_)
#else // defined(ASIO_HAS_BOOST_DATE_TIME)
      ? timer_service_->expiry(timer_implementation_)
#endif // defined(ASIO_HAS_BOOST_DATE_TIME)
      : time_point();
  }

  /// Set the stream buffer's expiry time as an absolute time.
  /**
   * This function sets the expiry time associated with the stream. Stream
   * operations performed after this time (where the operations cannot be
   * completed using the internal buffers) will fail with the error
   * asio::error::operation_aborted.
   *
   * @param expiry_time The expiry time to be used for the stream.
   */
  void expires_at(const time_point& expiry_time)
  {
    construct_timer();

    asio::error_code ec;
    timer_service_->expires_at(timer_implementation_, expiry_time, ec);
    asio::detail::throw_error(ec, "expires_at");

    start_timer();
  }

  /// Set the stream buffer's expiry time relative to now.
  /**
   * This function sets the expiry time associated with the stream. Stream
   * operations performed after this time (where the operations cannot be
   * completed using the internal buffers) will fail with the error
   * asio::error::operation_aborted.
   *
   * @param expiry_time The expiry time to be used for the timer.
   */
  void expires_at(const duration& expiry_time)
  {
    construct_timer();

    asio::error_code ec;
    timer_service_->expires_from_now(timer_implementation_, expiry_time, ec);
    asio::detail::throw_error(ec, "expires_from_now");

    start_timer();
  }

  /// Set the stream buffer's expiry time relative to now.
  /**
   * This function sets the expiry time associated with the stream. Stream
   * operations performed after this time (where the operations cannot be
   * completed using the internal buffers) will fail with the error
   * asio::error::operation_aborted.
   *
   * @param expiry_time The expiry time to be used for the timer.
   */
  void expires_after(const duration& expiry_time)
  {
    construct_timer();

    asio::error_code ec;
#if defined(ASIO_HAS_BOOST_DATE_TIME)
    timer_service_->expires_from_now(timer_implementation_, expiry_time, ec);
#else // defined(ASIO_HAS_BOOST_DATE_TIME)
    timer_service_->expires_after(timer_implementation_, expiry_time, ec);
#endif // defined(ASIO_HAS_BOOST_DATE_TIME)
    asio::detail::throw_error(ec, "after");

    start_timer();
  }

#if !defined(ASIO_NO_DEPRECATED)
  /// (Deprecated: Use expiry().) Get the stream buffer's expiry time relative
  /// to now.
  /**
   * @return A relative time value representing the stream buffer's expiry time.
   */
  duration expires_from_now() const
  {
    return traits_helper::subtract(expires_at(), traits_helper::now());
  }

  /// (Deprecated: Use expires_after().) Set the stream buffer's expiry time
  /// relative to now.
  /**
   * This function sets the expiry time associated with the stream. Stream
   * operations performed after this time (where the operations cannot be
   * completed using the internal buffers) will fail with the error
   * asio::error::operation_aborted.
   *
   * @param expiry_time The expiry time to be used for the timer.
   */
  void expires_from_now(const duration& expiry_time)
  {
    construct_timer();

    asio::error_code ec;
    timer_service_->expires_from_now(timer_implementation_, expiry_time, ec);
    asio::detail::throw_error(ec, "expires_from_now");

    start_timer();
  }
#endif // !defined(ASIO_NO_DEPRECATED)

protected:
  int_type underflow()
  {
    if (gptr() == egptr())
    {
      if (timer_state_ == timer_has_expired)
      {
        ec_ = asio::error::operation_aborted;
        return traits_type::eof();
      }

      io_handler handler = { this };
      this->get_service().async_receive(this->get_implementation(),
          asio::buffer(asio::buffer(get_buffer_) + putback_max),
          0, handler);

      ec_ = asio::error::would_block;
      this->get_service().get_io_service().restart();
      do this->get_service().get_io_service().run_one();
      while (ec_ == asio::error::would_block);
      if (ec_)
        return traits_type::eof();

      setg(&get_buffer_[0], &get_buffer_[0] + putback_max,
          &get_buffer_[0] + putback_max + bytes_transferred_);
      return traits_type::to_int_type(*gptr());
    }
    else
    {
      return traits_type::eof();
    }
  }

  int_type overflow(int_type c)
  {
    if (unbuffered_)
    {
      if (traits_type::eq_int_type(c, traits_type::eof()))
      {
        // Nothing to do.
        return traits_type::not_eof(c);
      }
      else
      {
        if (timer_state_ == timer_has_expired)
        {
          ec_ = asio::error::operation_aborted;
          return traits_type::eof();
        }

        // Send the single character immediately.
        char_type ch = traits_type::to_char_type(c);
        io_handler handler = { this };
        this->get_service().async_send(this->get_implementation(),
            asio::buffer(&ch, sizeof(char_type)), 0, handler);

        ec_ = asio::error::would_block;
        this->get_service().get_io_service().restart();
        do this->get_service().get_io_service().run_one();
        while (ec_ == asio::error::would_block);
        if (ec_)
          return traits_type::eof();

        return c;
      }
    }
    else
    {
      // Send all data in the output buffer.
      asio::const_buffer buffer =
        asio::buffer(pbase(), pptr() - pbase());
      while (asio::buffer_size(buffer) > 0)
      {
        if (timer_state_ == timer_has_expired)
        {
          ec_ = asio::error::operation_aborted;
          return traits_type::eof();
        }

        io_handler handler = { this };
        this->get_service().async_send(this->get_implementation(),
            asio::buffer(buffer), 0, handler);

        ec_ = asio::error::would_block;
        this->get_service().get_io_service().restart();
        do this->get_service().get_io_service().run_one();
        while (ec_ == asio::error::would_block);
        if (ec_)
          return traits_type::eof();

        buffer = buffer + bytes_transferred_;
      }
      setp(&put_buffer_[0], &put_buffer_[0] + put_buffer_.size());

      // If the new character is eof then our work here is done.
      if (traits_type::eq_int_type(c, traits_type::eof()))
        return traits_type::not_eof(c);

      // Add the new character to the output buffer.
      *pptr() = traits_type::to_char_type(c);
      pbump(1);
      return c;
    }
  }

  int sync()
  {
    return overflow(traits_type::eof());
  }

  std::streambuf* setbuf(char_type* s, std::streamsize n)
  {
    if (pptr() == pbase() && s == 0 && n == 0)
    {
      unbuffered_ = true;
      setp(0, 0);
      return this;
    }

    return 0;
  }

  /// Get the last error associated with the stream buffer.
  /**
   * @return An \c error_code corresponding to the last error from the stream
   * buffer.
   */
  virtual const asio::error_code& error() const
  {
    return ec_;
  }

private:
  void init_buffers()
  {
    setg(&get_buffer_[0],
        &get_buffer_[0] + putback_max,
        &get_buffer_[0] + putback_max);
    if (unbuffered_)
      setp(0, 0);
    else
      setp(&put_buffer_[0], &put_buffer_[0] + put_buffer_.size());
  }

  template <typename ResolverQuery>
  void resolve_and_connect(const ResolverQuery& query)
  {
    typedef typename Protocol::resolver resolver_type;
    typedef typename resolver_type::iterator iterator_type;
    resolver_type resolver(detail::socket_streambuf_base::io_service_);
    iterator_type i = resolver.resolve(query, ec_);
    if (!ec_)
    {
      iterator_type end;
      ec_ = asio::error::host_not_found;
      while (ec_ && i != end)
      {
        this->basic_socket<Protocol, StreamSocketService>::close(ec_);

        if (timer_state_ == timer_has_expired)
        {
          ec_ = asio::error::operation_aborted;
          return;
        }

        io_handler handler = { this };
        this->basic_socket<Protocol, StreamSocketService>::async_connect(
            *i, handler);

        ec_ = asio::error::would_block;
        this->get_service().get_io_service().restart();
        do this->get_service().get_io_service().run_one();
        while (ec_ == asio::error::would_block);

        ++i;
      }
    }
  }

  struct io_handler;
  friend struct io_handler;
  struct io_handler
  {
    basic_socket_streambuf* this_;

    void operator()(const asio::error_code& ec,
        std::size_t bytes_transferred = 0)
    {
      this_->ec_ = ec;
      this_->bytes_transferred_ = bytes_transferred;
    }
  };

  struct timer_handler;
  friend struct timer_handler;
  struct timer_handler
  {
    basic_socket_streambuf* this_;

    void operator()(const asio::error_code&)
    {
      time_point now = traits_helper::now();

#if defined(ASIO_HAS_BOOST_DATE_TIME)
      time_point expiry_time = this_->timer_service_->expires_at(
            this_->timer_implementation_);
#else // defined(ASIO_HAS_BOOST_DATE_TIME)
      time_point expiry_time = this_->timer_service_->expiry(
            this_->timer_implementation_);
#endif // defined(ASIO_HAS_BOOST_DATE_TIME)

      if (traits_helper::less_than(now, expiry_time))
      {
        this_->timer_state_ = timer_is_pending;
        this_->timer_service_->async_wait(this_->timer_implementation_, *this);
      }
      else
      {
        this_->timer_state_ = timer_has_expired;
        asio::error_code ec;
        this_->basic_socket<Protocol, StreamSocketService>::close(ec);
      }
    }
  };

  void construct_timer()
  {
    if (timer_service_ == 0)
    {
      TimerService& timer_service = use_service<TimerService>(
          detail::socket_streambuf_base::io_service_);
      timer_service.construct(timer_implementation_);
      timer_service_ = &timer_service;
    }
  }

  void destroy_timer()
  {
    if (timer_service_)
      timer_service_->destroy(timer_implementation_);
  }

  void start_timer()
  {
    if (timer_state_ != timer_is_pending)
    {
      timer_handler handler = { this };
      handler(asio::error_code());
    }
  }

  enum { putback_max = 8 };
  enum { buffer_size = 512 };
  asio::detail::array<char, buffer_size> get_buffer_;
  asio::detail::array<char, buffer_size> put_buffer_;
  bool unbuffered_;
  asio::error_code ec_;
  std::size_t bytes_transferred_;
  TimerService* timer_service_;
  typename TimerService::implementation_type timer_implementation_;
  enum state { no_timer, timer_is_pending, timer_has_expired } timer_state_;
};

} // namespace asio

#include "asio/detail/pop_options.hpp"

#if !defined(ASIO_HAS_VARIADIC_TEMPLATES)
# undef ASIO_PRIVATE_CONNECT_DEF
#endif // !defined(ASIO_HAS_VARIADIC_TEMPLATES)

#endif // !defined(ASIO_NO_IOSTREAM)

#endif // ASIO_BASIC_SOCKET_STREAMBUF_HPP
