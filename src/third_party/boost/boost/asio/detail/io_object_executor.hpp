//
// io_object_executor.hpp
// ~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2019 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_DETAIL_IO_OBJECT_EXECUTOR_HPP
#define BOOST_ASIO_DETAIL_IO_OBJECT_EXECUTOR_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/detail/handler_invoke_helpers.hpp>
#include <boost/asio/detail/type_traits.hpp>
#include <boost/asio/io_context.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace detail {

// Wrap the (potentially polymorphic) executor so that we can bypass it when
// dispatching on a target executor that has a native I/O implementation.
template <typename Executor>
class io_object_executor
{
public:
  io_object_executor(const Executor& ex,
      bool native_implementation) BOOST_ASIO_NOEXCEPT
    : executor_(ex),
      has_native_impl_(native_implementation)
  {
  }

  io_object_executor(const io_object_executor& other) BOOST_ASIO_NOEXCEPT
    : executor_(other.executor_),
      has_native_impl_(other.has_native_impl_)
  {
  }

  template <typename Executor1>
  io_object_executor(
      const io_object_executor<Executor1>& other) BOOST_ASIO_NOEXCEPT
    : executor_(other.inner_executor()),
      has_native_impl_(other.has_native_implementation())
  {
  }

#if defined(BOOST_ASIO_HAS_MOVE)
  io_object_executor(io_object_executor&& other) BOOST_ASIO_NOEXCEPT
    : executor_(BOOST_ASIO_MOVE_CAST(Executor)(other.executor_)),
      has_native_impl_(other.has_native_impl_)
  {
  }
#endif // defined(BOOST_ASIO_HAS_MOVE)

  const Executor& inner_executor() const BOOST_ASIO_NOEXCEPT
  {
    return executor_;
  }

  bool has_native_implementation() const BOOST_ASIO_NOEXCEPT
  {
    return has_native_impl_;
  }

  execution_context& context() const BOOST_ASIO_NOEXCEPT
  {
    return executor_.context();
  }

  void on_work_started() const BOOST_ASIO_NOEXCEPT
  {
    if (is_same<Executor, io_context::executor_type>::value
        || has_native_impl_)
    {
      // When using a native implementation, work is already counted by the
      // execution context.
    }
    else
    {
      executor_.on_work_started();
    }
  }

  void on_work_finished() const BOOST_ASIO_NOEXCEPT
  {
    if (is_same<Executor, io_context::executor_type>::value
        || has_native_impl_)
    {
      // When using a native implementation, work is already counted by the
      // execution context.
    }
    else
    {
      executor_.on_work_finished();
    }
  }

  template <typename F, typename A>
  void dispatch(BOOST_ASIO_MOVE_ARG(F) f, const A& a) const
  {
    if (is_same<Executor, io_context::executor_type>::value
        || has_native_impl_)
    {
      // When using a native implementation, I/O completion handlers are
      // already dispatched according to the execution context's executor's
      // rules. We can call the function directly.
      typename decay<F>::type function(BOOST_ASIO_MOVE_CAST(F)(f));
      boost_asio_handler_invoke_helpers::invoke(function, function);
    }
    else
    {
      executor_.dispatch(BOOST_ASIO_MOVE_CAST(F)(f), a);
    }
  }

  template <typename F, typename A>
  void post(BOOST_ASIO_MOVE_ARG(F) f, const A& a) const
  {
    executor_.post(BOOST_ASIO_MOVE_CAST(F)(f), a);
  }

  template <typename F, typename A>
  void defer(BOOST_ASIO_MOVE_ARG(F) f, const A& a) const
  {
    executor_.defer(BOOST_ASIO_MOVE_CAST(F)(f), a);
  }

  friend bool operator==(const io_object_executor& a,
      const io_object_executor& b) BOOST_ASIO_NOEXCEPT
  {
    return a.executor_ == b.executor_
      && a.has_native_impl_ == b.has_native_impl_;
  }

  friend bool operator!=(const io_object_executor& a,
      const io_object_executor& b) BOOST_ASIO_NOEXCEPT
  {
    return a.executor_ != b.executor_
      || a.has_native_impl_ != b.has_native_impl_;
  }

private:
  Executor executor_;
  const bool has_native_impl_;
};

} // namespace detail
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_DETAIL_IO_OBJECT_EXECUTOR_HPP
