//
// async_result.hpp
// ~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_ASYNC_RESULT_HPP
#define ASIO_ASYNC_RESULT_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include "asio/detail/type_traits.hpp"
#include "asio/handler_type.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {

/// An interface for customising the behaviour of an initiating function.
/**
 * This template may be specialised for user-defined handler types.
 */
template <typename Handler>
class async_result
{
public:
  /// The return type of the initiating function.
  typedef void type;

  /// Construct an async result from a given handler.
  /**
   * When using a specalised async_result, the constructor has an opportunity
   * to initialise some state associated with the handler, which is then
   * returned from the initiating function.
   */
  explicit async_result(Handler&)
  {
  }

  /// Obtain the value to be returned from the initiating function.
  type get()
  {
  }
};

/// Helper template to deduce the real type of a handler, capture a local copy
/// of the handler, and then create an async_result for the handler.
template <typename Handler, typename Signature>
struct async_completion
{
  /// The real handler type to be used for the asynchronous operation.
  typedef typename asio::handler_type<
    Handler, Signature>::type handler_type;

  /// Constructor.
  /**
   * The constructor creates the concrete handler and makes the link between
   * the handler and the asynchronous result.
   */
#if defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)
  explicit async_completion(
      typename remove_reference<Handler>::type& orig_handler)
    : handler(static_cast<typename conditional<
        is_same<Handler, handler_type>::value,
        handler_type&, Handler&&>::type>(orig_handler)),
      result(handler)
  {
  }
#else // defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)
  explicit async_completion(const Handler& orig_handler)
    : handler(orig_handler),
      result(handler)
  {
  }
#endif // defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)

  /// A copy of, or reference to, a real handler object.
#if defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)
  typename conditional<
    is_same<Handler, handler_type>::value,
    handler_type&, handler_type>::type handler;
#else // defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)
  typename asio::handler_type<Handler, Signature>::type handler;
#endif // defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)

  /// The result of the asynchronous operation's initiating function.
  async_result<typename asio::handler_type<
    Handler, Signature>::type> result;
};

namespace detail {

template <typename Handler, typename Signature>
struct async_result_type_helper
{
  typedef typename async_result<
      typename handler_type<Handler, Signature>::type
    >::type type;
};

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#if defined(GENERATING_DOCUMENTATION)
# define ASIO_INITFN_RESULT_TYPE(h, sig) \
  void_or_deduced
#elif defined(_MSC_VER) && (_MSC_VER < 1500)
# define ASIO_INITFN_RESULT_TYPE(h, sig) \
  typename ::asio::detail::async_result_type_helper<h, sig>::type
#else
# define ASIO_INITFN_RESULT_TYPE(h, sig) \
  typename ::asio::async_result< \
    typename ::asio::handler_type<h, sig>::type>::type
#endif

#endif // ASIO_ASYNC_RESULT_HPP
