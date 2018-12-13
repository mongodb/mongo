//
// signal_set_service.hpp
// ~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2018 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_SIGNAL_SET_SERVICE_HPP
#define BOOST_ASIO_SIGNAL_SET_SERVICE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#if defined(BOOST_ASIO_ENABLE_OLD_SERVICES)

#include <boost/asio/async_result.hpp>
#include <boost/asio/detail/signal_set_service.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

/// Default service implementation for a signal set.
class signal_set_service
#if defined(GENERATING_DOCUMENTATION)
  : public boost::asio::io_context::service
#else
  : public boost::asio::detail::service_base<signal_set_service>
#endif
{
public:
#if defined(GENERATING_DOCUMENTATION)
  /// The unique service identifier.
  static boost::asio::io_context::id id;
#endif

public:
  /// The type of a signal set implementation.
#if defined(GENERATING_DOCUMENTATION)
  typedef implementation_defined implementation_type;
#else
  typedef detail::signal_set_service::implementation_type implementation_type;
#endif

  /// Construct a new signal set service for the specified io_context.
  explicit signal_set_service(boost::asio::io_context& io_context)
    : boost::asio::detail::service_base<signal_set_service>(io_context),
      service_impl_(io_context)
  {
  }

  /// Construct a new signal set implementation.
  void construct(implementation_type& impl)
  {
    service_impl_.construct(impl);
  }

  /// Destroy a signal set implementation.
  void destroy(implementation_type& impl)
  {
    service_impl_.destroy(impl);
  }

  /// Add a signal to a signal_set.
  BOOST_ASIO_SYNC_OP_VOID add(implementation_type& impl,
      int signal_number, boost::system::error_code& ec)
  {
    service_impl_.add(impl, signal_number, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Remove a signal to a signal_set.
  BOOST_ASIO_SYNC_OP_VOID remove(implementation_type& impl,
      int signal_number, boost::system::error_code& ec)
  {
    service_impl_.remove(impl, signal_number, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Remove all signals from a signal_set.
  BOOST_ASIO_SYNC_OP_VOID clear(implementation_type& impl,
      boost::system::error_code& ec)
  {
    service_impl_.clear(impl, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Cancel all operations associated with the signal set.
  BOOST_ASIO_SYNC_OP_VOID cancel(implementation_type& impl,
      boost::system::error_code& ec)
  {
    service_impl_.cancel(impl, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  // Start an asynchronous operation to wait for a signal to be delivered.
  template <typename SignalHandler>
  BOOST_ASIO_INITFN_RESULT_TYPE(SignalHandler,
      void (boost::system::error_code, int))
  async_wait(implementation_type& impl,
      BOOST_ASIO_MOVE_ARG(SignalHandler) handler)
  {
    async_completion<SignalHandler,
      void (boost::system::error_code, int)> init(handler);

    service_impl_.async_wait(impl, init.completion_handler);

    return init.result.get();
  }

private:
  // Destroy all user-defined handler objects owned by the service.
  void shutdown()
  {
    service_impl_.shutdown();
  }

  // Perform any fork-related housekeeping.
  void notify_fork(boost::asio::io_context::fork_event event)
  {
    service_impl_.notify_fork(event);
  }

  // The platform-specific implementation.
  detail::signal_set_service service_impl_;
};

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // defined(BOOST_ASIO_ENABLE_OLD_SERVICES)

#endif // BOOST_ASIO_SIGNAL_SET_SERVICE_HPP
