//
// impl/defer.hpp
// ~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2019 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_IMPL_DEFER_HPP
#define BOOST_ASIO_IMPL_DEFER_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/detail/work_dispatcher.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace detail {

struct initiate_defer
{
  template <typename CompletionHandler>
  void operator()(BOOST_ASIO_MOVE_ARG(CompletionHandler) handler) const
  {
    typedef typename decay<CompletionHandler>::type DecayedHandler;

    typename associated_executor<DecayedHandler>::type ex(
        (get_associated_executor)(handler));

    typename associated_allocator<DecayedHandler>::type alloc(
        (get_associated_allocator)(handler));

    ex.defer(BOOST_ASIO_MOVE_CAST(CompletionHandler)(handler), alloc);
  }

  template <typename CompletionHandler, typename Executor>
  void operator()(BOOST_ASIO_MOVE_ARG(CompletionHandler) handler,
      BOOST_ASIO_MOVE_ARG(Executor) ex) const
  {
    typedef typename decay<CompletionHandler>::type DecayedHandler;

    typename associated_allocator<DecayedHandler>::type alloc(
        (get_associated_allocator)(handler));

    ex.defer(detail::work_dispatcher<DecayedHandler>(
          BOOST_ASIO_MOVE_CAST(CompletionHandler)(handler)), alloc);
  }
};

} // namespace detail

template <typename CompletionToken>
BOOST_ASIO_INITFN_RESULT_TYPE(CompletionToken, void()) defer(
    BOOST_ASIO_MOVE_ARG(CompletionToken) token)
{
  return async_initiate<CompletionToken, void()>(
      detail::initiate_defer(), token);
}

template <typename Executor, typename CompletionToken>
BOOST_ASIO_INITFN_RESULT_TYPE(CompletionToken, void()) defer(
    const Executor& ex, BOOST_ASIO_MOVE_ARG(CompletionToken) token,
    typename enable_if<is_executor<Executor>::value>::type*)
{
  return async_initiate<CompletionToken, void()>(
      detail::initiate_defer(), token, ex);
}

template <typename ExecutionContext, typename CompletionToken>
inline BOOST_ASIO_INITFN_RESULT_TYPE(CompletionToken, void()) defer(
    ExecutionContext& ctx, BOOST_ASIO_MOVE_ARG(CompletionToken) token,
    typename enable_if<is_convertible<
      ExecutionContext&, execution_context&>::value>::type*)
{
  return (defer)(ctx.get_executor(),
      BOOST_ASIO_MOVE_CAST(CompletionToken)(token));
}

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_IMPL_DEFER_HPP
