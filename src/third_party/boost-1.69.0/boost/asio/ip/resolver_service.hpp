//
// ip/resolver_service.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2018 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_IP_RESOLVER_SERVICE_HPP
#define BOOST_ASIO_IP_RESOLVER_SERVICE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#if defined(BOOST_ASIO_ENABLE_OLD_SERVICES)

#include <boost/asio/async_result.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/basic_resolver_iterator.hpp>
#include <boost/asio/ip/basic_resolver_query.hpp>
#include <boost/asio/ip/basic_resolver_results.hpp>

#if defined(BOOST_ASIO_WINDOWS_RUNTIME)
# include <boost/asio/detail/winrt_resolver_service.hpp>
#else
# include <boost/asio/detail/resolver_service.hpp>
#endif

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace ip {

/// Default service implementation for a resolver.
template <typename InternetProtocol>
class resolver_service
#if defined(GENERATING_DOCUMENTATION)
  : public boost::asio::io_context::service
#else
  : public boost::asio::detail::service_base<
      resolver_service<InternetProtocol> >
#endif
{
public:
#if defined(GENERATING_DOCUMENTATION)
  /// The unique service identifier.
  static boost::asio::io_context::id id;
#endif

  /// The protocol type.
  typedef InternetProtocol protocol_type;

  /// The endpoint type.
  typedef typename InternetProtocol::endpoint endpoint_type;

  /// The query type.
  typedef basic_resolver_query<InternetProtocol> query_type;

  /// The iterator type.
  typedef basic_resolver_iterator<InternetProtocol> iterator_type;

  /// The results type.
  typedef basic_resolver_results<InternetProtocol> results_type;

private:
  // The type of the platform-specific implementation.
#if defined(BOOST_ASIO_WINDOWS_RUNTIME)
  typedef boost::asio::detail::winrt_resolver_service<InternetProtocol>
    service_impl_type;
#else
  typedef boost::asio::detail::resolver_service<InternetProtocol>
    service_impl_type;
#endif

public:
  /// The type of a resolver implementation.
#if defined(GENERATING_DOCUMENTATION)
  typedef implementation_defined implementation_type;
#else
  typedef typename service_impl_type::implementation_type implementation_type;
#endif

  /// Construct a new resolver service for the specified io_context.
  explicit resolver_service(boost::asio::io_context& io_context)
    : boost::asio::detail::service_base<
        resolver_service<InternetProtocol> >(io_context),
      service_impl_(io_context)
  {
  }

  /// Construct a new resolver implementation.
  void construct(implementation_type& impl)
  {
    service_impl_.construct(impl);
  }

#if defined(BOOST_ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)
  /// Move-construct a new resolver implementation.
  void move_construct(implementation_type& impl,
      implementation_type& other_impl)
  {
    service_impl_.move_construct(impl, other_impl);
  }

  /// Move-assign from another resolver implementation.
  void move_assign(implementation_type& impl,
      resolver_service& other_service,
      implementation_type& other_impl)
  {
    service_impl_.move_assign(impl, other_service.service_impl_, other_impl);
  }
#endif // defined(BOOST_ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)

  /// Destroy a resolver implementation.
  void destroy(implementation_type& impl)
  {
    service_impl_.destroy(impl);
  }

  /// Cancel pending asynchronous operations.
  void cancel(implementation_type& impl)
  {
    service_impl_.cancel(impl);
  }

  /// Resolve a query to a list of entries.
  results_type resolve(implementation_type& impl, const query_type& query,
      boost::system::error_code& ec)
  {
    return service_impl_.resolve(impl, query, ec);
  }

  /// Asynchronously resolve a query to a list of entries.
  template <typename ResolveHandler>
  BOOST_ASIO_INITFN_RESULT_TYPE(ResolveHandler,
      void (boost::system::error_code, results_type))
  async_resolve(implementation_type& impl, const query_type& query,
      BOOST_ASIO_MOVE_ARG(ResolveHandler) handler)
  {
    boost::asio::async_completion<ResolveHandler,
      void (boost::system::error_code, results_type)> init(handler);

    service_impl_.async_resolve(impl, query, init.completion_handler);

    return init.result.get();
  }

  /// Resolve an endpoint to a list of entries.
  results_type resolve(implementation_type& impl,
      const endpoint_type& endpoint, boost::system::error_code& ec)
  {
    return service_impl_.resolve(impl, endpoint, ec);
  }

  /// Asynchronously resolve an endpoint to a list of entries.
  template <typename ResolveHandler>
  BOOST_ASIO_INITFN_RESULT_TYPE(ResolveHandler,
      void (boost::system::error_code, results_type))
  async_resolve(implementation_type& impl, const endpoint_type& endpoint,
      BOOST_ASIO_MOVE_ARG(ResolveHandler) handler)
  {
    boost::asio::async_completion<ResolveHandler,
      void (boost::system::error_code, results_type)> init(handler);

    service_impl_.async_resolve(impl, endpoint, init.completion_handler);

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
  service_impl_type service_impl_;
};

} // namespace ip
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // defined(BOOST_ASIO_ENABLE_OLD_SERVICES)

#endif // BOOST_ASIO_IP_RESOLVER_SERVICE_HPP
