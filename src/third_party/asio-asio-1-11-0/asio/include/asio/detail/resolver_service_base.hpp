//
// detail/resolver_service_base.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_DETAIL_RESOLVER_SERVICE_BASE_HPP
#define ASIO_DETAIL_RESOLVER_SERVICE_BASE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include "asio/error.hpp"
#include "asio/io_service.hpp"
#include "asio/detail/mutex.hpp"
#include "asio/detail/noncopyable.hpp"
#include "asio/detail/operation.hpp"
#include "asio/detail/socket_ops.hpp"
#include "asio/detail/socket_types.hpp"
#include "asio/detail/scoped_ptr.hpp"
#include "asio/detail/thread.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace detail {

class resolver_service_base
{
public:
  // The implementation type of the resolver. A cancellation token is used to
  // indicate to the background thread that the operation has been cancelled.
  typedef socket_ops::shared_cancel_token_type implementation_type;

  // Constructor.
  ASIO_DECL resolver_service_base(asio::io_service& io_service);

  // Destructor.
  ASIO_DECL ~resolver_service_base();

  // Destroy all user-defined handler objects owned by the service.
  ASIO_DECL void shutdown_service();

  // Perform any fork-related housekeeping.
  ASIO_DECL void fork_service(
      asio::io_service::fork_event fork_ev);

  // Construct a new resolver implementation.
  ASIO_DECL void construct(implementation_type& impl);

  // Destroy a resolver implementation.
  ASIO_DECL void destroy(implementation_type&);

  // Cancel pending asynchronous operations.
  ASIO_DECL void cancel(implementation_type& impl);

protected:
  // Helper function to start an asynchronous resolve operation.
  ASIO_DECL void start_resolve_op(operation* op);

#if !defined(ASIO_WINDOWS_RUNTIME)
  // Helper class to perform exception-safe cleanup of addrinfo objects.
  class auto_addrinfo
    : private asio::detail::noncopyable
  {
  public:
    explicit auto_addrinfo(asio::detail::addrinfo_type* ai)
      : ai_(ai)
    {
    }

    ~auto_addrinfo()
    {
      if (ai_)
        socket_ops::freeaddrinfo(ai_);
    }

    operator asio::detail::addrinfo_type*()
    {
      return ai_;
    }

  private:
    asio::detail::addrinfo_type* ai_;
  };
#endif // !defined(ASIO_WINDOWS_RUNTIME)

  // Helper class to run the work io_service in a thread.
  class work_io_service_runner;

  // Start the work thread if it's not already running.
  ASIO_DECL void start_work_thread();

  // The io_service implementation used to post completions.
  io_service_impl& io_service_impl_;

private:
  // Mutex to protect access to internal data.
  asio::detail::mutex mutex_;

  // Private io_service used for performing asynchronous host resolution.
  asio::detail::scoped_ptr<asio::io_service> work_io_service_;

  // The work io_service implementation used to post completions.
  io_service_impl& work_io_service_impl_;

  // Work for the private io_service to perform.
  asio::detail::scoped_ptr<asio::io_service::work> work_;

  // Thread used for running the work io_service's run loop.
  asio::detail::scoped_ptr<asio::detail::thread> work_thread_;
};

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#if defined(ASIO_HEADER_ONLY)
# include "asio/detail/impl/resolver_service_base.ipp"
#endif // defined(ASIO_HEADER_ONLY)

#endif // ASIO_DETAIL_RESOLVER_SERVICE_BASE_HPP
