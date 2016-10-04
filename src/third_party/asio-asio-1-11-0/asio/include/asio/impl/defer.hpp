//
// impl/defer.hpp
// ~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_IMPL_DEFER_HPP
#define ASIO_IMPL_DEFER_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include "asio/associated_allocator.hpp"
#include "asio/associated_executor.hpp"
#include "asio/detail/work_dispatcher.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {

template <typename CompletionToken>
ASIO_INITFN_RESULT_TYPE(CompletionToken, void()) defer(
    ASIO_MOVE_ARG(CompletionToken) token)
{
  typedef typename handler_type<CompletionToken, void()>::type handler;
  async_completion<CompletionToken, void()> completion(token);

  typename associated_executor<handler>::type ex(
      (get_associated_executor)(completion.handler));

  typename associated_allocator<handler>::type alloc(
      (get_associated_allocator)(completion.handler));

  ex.defer(ASIO_MOVE_CAST(handler)(completion.handler), alloc);

  return completion.result.get();
}

template <typename Executor, typename CompletionToken>
ASIO_INITFN_RESULT_TYPE(CompletionToken, void()) defer(
    const Executor& ex, ASIO_MOVE_ARG(CompletionToken) token,
    typename enable_if<is_executor<Executor>::value>::type*)
{
  typedef typename handler_type<CompletionToken, void()>::type handler;
  async_completion<CompletionToken, void()> completion(token);

  Executor ex1(ex);

  typename associated_allocator<handler>::type alloc(
      (get_associated_allocator)(completion.handler));

  ex1.defer(detail::work_dispatcher<handler>(completion.handler), alloc);

  return completion.result.get();
}

template <typename ExecutionContext, typename CompletionToken>
inline ASIO_INITFN_RESULT_TYPE(CompletionToken, void()) defer(
    ExecutionContext& ctx, ASIO_MOVE_ARG(CompletionToken) token,
    typename enable_if<is_convertible<
      ExecutionContext&, execution_context&>::value>::type*)
{
  return (defer)(ctx.get_executor(),
      ASIO_MOVE_CAST(CompletionToken)(token));
}

} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_IMPL_DEFER_HPP
