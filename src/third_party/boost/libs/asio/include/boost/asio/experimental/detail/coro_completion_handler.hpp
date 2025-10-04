//
// experimental/detail/coro_completion_handler.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2021-2023 Klemens D. Morgenstern
//                         (klemens dot morgenstern at gmx dot net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_EXPERIMENTAL_DETAIL_CORO_COMPLETION_HANDLER_HPP
#define BOOST_ASIO_EXPERIMENTAL_DETAIL_CORO_COMPLETION_HANDLER_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/coro.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace experimental {
namespace detail {

template <typename Promise, typename... Args>
struct coro_completion_handler
{
  coro_completion_handler(coroutine_handle<Promise> h,
      std::optional<std::tuple<Args...>>& result)
    : self(h),
      result(result)
  {
  }

  coro_completion_handler(coro_completion_handler&&) = default;

  coroutine_handle<Promise> self;

  std::optional<std::tuple<Args...>>& result;

  using promise_type = Promise;

  void operator()(Args... args)
  {
    result.emplace(std::move(args)...);
    self.resume();
  }

  using allocator_type = typename promise_type::allocator_type;
  allocator_type get_allocator() const noexcept
  {
    return self.promise().get_allocator();
  }

  using executor_type = typename promise_type::executor_type;
  executor_type get_executor() const noexcept
  {
    return self.promise().get_executor();
  }

  using cancellation_slot_type = typename promise_type::cancellation_slot_type;
  cancellation_slot_type get_cancellation_slot() const noexcept
  {
    return self.promise().get_cancellation_slot();
  }
};

template <typename Signature>
struct coro_completion_handler_type;

template <typename... Args>
struct coro_completion_handler_type<void(Args...)>
{
  using type = std::tuple<Args...>;

  template <typename Promise>
  using completion_handler = coro_completion_handler<Promise, Args...>;
};

template <typename Signature>
using coro_completion_handler_type_t =
  typename coro_completion_handler_type<Signature>::type;

inline void coro_interpret_result(std::tuple<>&&)
{
}

template <typename... Args>
inline auto coro_interpret_result(std::tuple<Args...>&& args)
{
  return std::move(args);
}

template <typename... Args>
auto coro_interpret_result(std::tuple<std::exception_ptr, Args...>&& args)
{
  if (std::get<0>(args))
    std::rethrow_exception(std::get<0>(args));

  return std::apply(
      [](auto, auto&&... rest)
      {
        return std::make_tuple(std::move(rest)...);
      }, std::move(args));
}

template <typename... Args>
auto coro_interpret_result(
    std::tuple<boost::system::error_code, Args...>&& args)
{
  if (std::get<0>(args))
    boost::asio::detail::throw_exception(
        boost::system::system_error(std::get<0>(args)));

  return std::apply(
      [](auto, auto&&... rest)
      {
        return std::make_tuple(std::move(rest)...);
      }, std::move(args));
}

template <typename  Arg>
inline auto coro_interpret_result(std::tuple<Arg>&& args)
{
  return std::get<0>(std::move(args));
}

template <typename Arg>
auto coro_interpret_result(std::tuple<std::exception_ptr, Arg>&& args)
{
  if (std::get<0>(args))
    std::rethrow_exception(std::get<0>(args));
  return std::get<1>(std::move(args));
}

inline auto coro_interpret_result(
    std::tuple<boost::system::error_code>&& args)
{
  if (std::get<0>(args))
    boost::asio::detail::throw_exception(
        boost::system::system_error(std::get<0>(args)));
}

inline auto coro_interpret_result(std::tuple<std::exception_ptr>&& args)
{
  if (std::get<0>(args))
    std::rethrow_exception(std::get<0>(args));
}

template <typename Arg>
auto coro_interpret_result(std::tuple<boost::system::error_code, Arg>&& args)
{
  if (std::get<0>(args))
    boost::asio::detail::throw_exception(
        boost::system::system_error(std::get<0>(args)));
  return std::get<1>(std::move(args));
}

} // namespace detail
} // namespace experimental
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_EXPERIMENTAL_DETAIL_CORO_COMPLETION_HANDLER_HPP
