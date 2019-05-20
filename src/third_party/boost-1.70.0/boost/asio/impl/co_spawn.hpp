//
// impl/co_spawn.hpp
// ~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2019 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_IMPL_CO_SPAWN_HPP
#define BOOST_ASIO_IMPL_CO_SPAWN_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace detail {

template <typename T, typename Executor, typename F, typename Handler>
awaitable<void, Executor> co_spawn_entry_point(
    awaitable<T, Executor>*, Executor ex, F f, Handler handler)
{
  auto spawn_work = make_work_guard(ex);
  auto handler_work = make_work_guard(handler, ex);

  (void) co_await (post)(spawn_work.get_executor(),
      use_awaitable_t<Executor>{});

  bool done = false;
  try
  {
    T t = co_await f();

    done = true;

    (dispatch)(handler_work.get_executor(),
        [handler = std::move(handler), t = std::move(t)]() mutable
        {
          handler(std::exception_ptr(), std::move(t));
        });
  }
  catch (...)
  {
    if (done)
      throw;

    (dispatch)(handler_work.get_executor(),
        [handler = std::move(handler), e = std::current_exception()]() mutable
        {
          handler(e, T());
        });
  }
}

template <typename Executor, typename F, typename Handler>
awaitable<void, Executor> co_spawn_entry_point(
    awaitable<void, Executor>*, Executor ex, F f, Handler handler)
{
  auto spawn_work = make_work_guard(ex);
  auto handler_work = make_work_guard(handler, ex);

  (void) co_await (post)(spawn_work.get_executor(),
      use_awaitable_t<Executor>{});

  std::exception_ptr e = nullptr;
  try
  {
    co_await f();
  }
  catch (...)
  {
    e = std::current_exception();
  }

  (dispatch)(handler_work.get_executor(),
      [handler = std::move(handler), e]() mutable
      {
        handler(e);
      });
}

struct initiate_co_spawn
{
  template <typename Handler, typename Executor, typename F>
  void operator()(Handler&& handler, const Executor& ex, F&& f) const
  {
    typedef typename result_of<F()>::type awaitable_type;
    typedef typename awaitable_type::executor_type executor_type;

    executor_type ex2(ex);
    auto a = (co_spawn_entry_point)(static_cast<awaitable_type*>(nullptr),
        ex2, std::forward<F>(f), std::forward<Handler>(handler));
    awaitable_handler<executor_type, void>(std::move(a), ex2).launch();
  }
};

} // namespace detail

template <typename Executor, typename F, typename CompletionToken>
inline BOOST_ASIO_INITFN_RESULT_TYPE(CompletionToken,
    typename detail::awaitable_signature<typename result_of<F()>::type>::type)
co_spawn(const Executor& ex, F&& f, CompletionToken&& token,
    typename enable_if<
      is_executor<Executor>::value
    >::type*)
{
  return async_initiate<CompletionToken,
    typename detail::awaitable_signature<typename result_of<F()>::type>>(
      detail::initiate_co_spawn(), token, ex, std::forward<F>(f));
}

template <typename ExecutionContext, typename F, typename CompletionToken>
inline BOOST_ASIO_INITFN_RESULT_TYPE(CompletionToken,
    typename detail::awaitable_signature<typename result_of<F()>::type>::type)
co_spawn(ExecutionContext& ctx, F&& f, CompletionToken&& token,
    typename enable_if<
      is_convertible<ExecutionContext&, execution_context&>::value
    >::type*)
{
  return (co_spawn)(ctx.get_executor(), std::forward<F>(f),
      std::forward<CompletionToken>(token));
}

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_IMPL_CO_SPAWN_HPP
