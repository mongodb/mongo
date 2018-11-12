//
// experimental/impl/detached.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2018 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_EXPERIMENTAL_IMPL_DETACHED_HPP
#define BOOST_ASIO_EXPERIMENTAL_IMPL_DETACHED_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/detail/variadic_templates.hpp>
#include <boost/asio/handler_type.hpp>
#include <boost/system/system_error.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace experimental {
namespace detail {

  // Class to adapt a detached_t as a completion handler.
  class detached_handler
  {
  public:
    detached_handler(detached_t)
    {
    }

#if defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

    template <typename... Args>
    void operator()(Args...)
    {
    }

#else // defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

    void operator()()
    {
    }

#define BOOST_ASIO_PRIVATE_DETACHED_DEF(n) \
    template <BOOST_ASIO_VARIADIC_TPARAMS(n)> \
    void operator()(BOOST_ASIO_VARIADIC_BYVAL_PARAMS(n)) \
    { \
    } \
    /**/
    BOOST_ASIO_VARIADIC_GENERATE(BOOST_ASIO_PRIVATE_DETACHED_DEF)
#undef BOOST_ASIO_PRIVATE_DETACHED_DEF

#endif // defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)
  };

} // namespace detail
} // namespace experimental

#if !defined(GENERATING_DOCUMENTATION)

template <typename Signature>
struct async_result<experimental::detached_t, Signature>
{
  typedef boost::asio::experimental::detail::detached_handler
    completion_handler_type;

  typedef void return_type;

  explicit async_result(completion_handler_type&)
  {
  }

  void get()
  {
  }
};

#endif // !defined(GENERATING_DOCUMENTATION)

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_EXPERIMENTAL_IMPL_DETACHED_HPP
