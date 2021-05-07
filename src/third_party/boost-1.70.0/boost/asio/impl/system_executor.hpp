//
// impl/system_executor.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2019 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_IMPL_SYSTEM_EXECUTOR_HPP
#define BOOST_ASIO_IMPL_SYSTEM_EXECUTOR_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/executor_op.hpp>
#include <boost/asio/detail/global.hpp>
#include <boost/asio/detail/recycling_allocator.hpp>
#include <boost/asio/detail/type_traits.hpp>
#include <boost/asio/system_context.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

inline system_context& system_executor::context() const BOOST_ASIO_NOEXCEPT
{
  return detail::global<system_context>();
}

template <typename Function, typename Allocator>
void system_executor::dispatch(
    BOOST_ASIO_MOVE_ARG(Function) f, const Allocator&) const
{
  typename decay<Function>::type tmp(BOOST_ASIO_MOVE_CAST(Function)(f));
  boost_asio_handler_invoke_helpers::invoke(tmp, tmp);
}

template <typename Function, typename Allocator>
void system_executor::post(
    BOOST_ASIO_MOVE_ARG(Function) f, const Allocator& a) const
{
  typedef typename decay<Function>::type function_type;

  system_context& ctx = detail::global<system_context>();

  // Allocate and construct an operation to wrap the function.
  typedef detail::executor_op<function_type, Allocator> op;
  typename op::ptr p = { detail::addressof(a), op::ptr::allocate(a), 0 };
  p.p = new (p.v) op(BOOST_ASIO_MOVE_CAST(Function)(f), a);

  BOOST_ASIO_HANDLER_CREATION((ctx, *p.p,
        "system_executor", &this->context(), 0, "post"));

  ctx.scheduler_.post_immediate_completion(p.p, false);
  p.v = p.p = 0;
}

template <typename Function, typename Allocator>
void system_executor::defer(
    BOOST_ASIO_MOVE_ARG(Function) f, const Allocator& a) const
{
  typedef typename decay<Function>::type function_type;

  system_context& ctx = detail::global<system_context>();

  // Allocate and construct an operation to wrap the function.
  typedef detail::executor_op<function_type, Allocator> op;
  typename op::ptr p = { detail::addressof(a), op::ptr::allocate(a), 0 };
  p.p = new (p.v) op(BOOST_ASIO_MOVE_CAST(Function)(f), a);

  BOOST_ASIO_HANDLER_CREATION((ctx, *p.p,
        "system_executor", &this->context(), 0, "defer"));

  ctx.scheduler_.post_immediate_completion(p.p, true);
  p.v = p.p = 0;
}

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_IMPL_SYSTEM_EXECUTOR_HPP
