//
// experimental/co_spawn.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2021-2023 Klemens D. Morgenstern
//                         (klemens dot morgenstern at gmx dot net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#ifndef BOOST_ASIO_EXPERIMENTAL_CO_SPAWN_HPP
#define BOOST_ASIO_EXPERIMENTAL_CO_SPAWN_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <utility>
#include <boost/asio/compose.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/detail/type_traits.hpp>
#include <boost/asio/experimental/coro.hpp>
#include <boost/asio/prepend.hpp>
#include <boost/asio/redirect_error.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace experimental {
namespace detail {

template <typename T, typename U, typename Executor>
struct coro_spawn_op
{
  coro<T, U, Executor> c;

  void operator()(auto& self)
  {
    auto op = c.async_resume(deferred);
    std::move(op)((prepend)(std::move(self), 0));
  }

  void operator()(auto& self, int, auto... res)
  {
    self.complete(std::move(res)...);
  }
};

} // namespace detail

/// Spawn a resumable coroutine.
/**
 * This function spawns the coroutine for execution on its executor. It binds
 * the lifetime of the coroutine to the executor.
 *
 * @param c The coroutine
 *
 * @param token The completion token
 *
 * @returns Implementation defined
 */
template <typename T, typename Executor, typename CompletionToken>
BOOST_ASIO_INITFN_AUTO_RESULT_TYPE(
    CompletionToken, void(std::exception_ptr, T))
co_spawn(coro<void, T, Executor> c, CompletionToken&& token)
{
  auto exec = c.get_executor();
  return async_compose<CompletionToken, void(std::exception_ptr, T)>(
      detail::coro_spawn_op<void, T, Executor>{std::move(c)},
      token, exec);
}

/// Spawn a resumable coroutine.
/**
 * This function spawns the coroutine for execution on its executor. It binds
 * the lifetime of the coroutine to the executor.
 *
 * @param c The coroutine
 *
 * @param token The completion token
 *
 * @returns Implementation defined
 */
template <typename T, typename Executor, typename CompletionToken>
BOOST_ASIO_INITFN_AUTO_RESULT_TYPE(
    CompletionToken, void(std::exception_ptr, T))
co_spawn(coro<void(), T, Executor> c, CompletionToken&& token)
{
  auto exec = c.get_executor();
  return async_compose<CompletionToken, void(std::exception_ptr, T)>(
      detail::coro_spawn_op<void(), T, Executor>{std::move(c)},
      token, exec);
}

/// Spawn a resumable coroutine.
/**
 * This function spawns the coroutine for execution on its executor. It binds
 * the lifetime of the coroutine to the executor.
 *
 * @param c The coroutine
 *
 * @param token The completion token
 *
 * @returns Implementation defined
 */
template <typename T, typename Executor, typename CompletionToken>
BOOST_ASIO_INITFN_AUTO_RESULT_TYPE(CompletionToken, void(T))
co_spawn(coro<void() noexcept, T, Executor> c, CompletionToken&& token)
{
  auto exec = c.get_executor();
  return async_compose<CompletionToken, void(T)>(
      detail::coro_spawn_op<void() noexcept, T, Executor>{std::move(c)},
      token, exec);
}

/// Spawn a resumable coroutine.
/**
 * This function spawns the coroutine for execution on its executor. It binds
 * the lifetime of the coroutine to the executor.
 *
 * @param c The coroutine
 *
 * @param token The completion token
 *
 * @returns Implementation defined
 */
template <typename Executor, typename CompletionToken>
BOOST_ASIO_INITFN_AUTO_RESULT_TYPE(
    CompletionToken, void(std::exception_ptr))
co_spawn(coro<void, void, Executor> c, CompletionToken&& token)
{
  auto exec = c.get_executor();
  return async_compose<CompletionToken, void(std::exception_ptr)>(
      detail::coro_spawn_op<void, void, Executor>{std::move(c)},
      token, exec);
}

/// Spawn a resumable coroutine.
/**
 * This function spawns the coroutine for execution on its executor. It binds
 * the lifetime of the coroutine to the executor.
 *
 * @param c The coroutine
 *
 * @param token The completion token
 *
 * @returns Implementation defined
 */
template <typename Executor, typename CompletionToken>
BOOST_ASIO_INITFN_AUTO_RESULT_TYPE(
    CompletionToken, void(std::exception_ptr))
co_spawn(coro<void(), void, Executor> c, CompletionToken&& token)
{
  auto exec = c.get_executor();
  return async_compose<CompletionToken, void(std::exception_ptr)>(
      detail::coro_spawn_op<void(), void, Executor>{std::move(c)},
      token, exec);
}

/// Spawn a resumable coroutine.
/**
 * This function spawns the coroutine for execution on its executor. It binds
 * the lifetime of the coroutine to the executor.
 *
 * @param c The coroutine
 *
 * @param token The completion token
 *
 * @returns Implementation defined
 */
template <typename Executor, typename CompletionToken>
BOOST_ASIO_INITFN_AUTO_RESULT_TYPE(CompletionToken, void())
co_spawn(coro<void() noexcept, void, Executor> c, CompletionToken&& token)
{
  auto exec = c.get_executor();
  return async_compose<CompletionToken, void()>(
      detail::coro_spawn_op<void() noexcept, void, Executor>{std::move(c)},
      token, exec);
}

} // namespace detail
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif //BOOST_ASIO_EXPERIMENTAL_CO_SPAWN_HPP
