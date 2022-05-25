//
// experimental/detail/channel_operation.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2022 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_EXPERIMENTAL_DETAIL_CHANNEL_OPERATION_HPP
#define BOOST_ASIO_EXPERIMENTAL_DETAIL_CHANNEL_OPERATION_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/detail/op_queue.hpp>
#include <boost/asio/execution/executor.hpp>
#include <boost/asio/execution/outstanding_work.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/prefer.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace experimental {
namespace detail {

// Base class for all channel operations. A function pointer is used instead of
// virtual functions to avoid the associated overhead.
class channel_operation BOOST_ASIO_INHERIT_TRACKED_HANDLER
{
public:
  template <typename Executor, typename = void>
  class handler_work_base;

  template <typename Handler, typename IoExecutor, typename = void>
  class handler_work;

  void destroy()
  {
    func_(this, destroy_op, 0);
  }

protected:
  enum action
  {
    destroy_op = 0,
    complete_op = 1,
    cancel_op = 2,
    close_op = 3
  };

  typedef void (*func_type)(channel_operation*, action, void*);

  channel_operation(func_type func)
    : next_(0),
      func_(func),
      cancellation_key_(0)
  {
  }

  // Prevents deletion through this type.
  ~channel_operation()
  {
  }

  friend class boost::asio::detail::op_queue_access;
  channel_operation* next_;
  func_type func_;

public:
  // The operation key used for targeted cancellation.
  void* cancellation_key_;
};

template <typename Executor, typename>
class channel_operation::handler_work_base
{
public:
  handler_work_base(int, const Executor& ex)
    : executor_(boost::asio::prefer(ex, execution::outstanding_work.tracked))
  {
  }

  template <typename Function, typename Handler>
  void post(Function& function, Handler& handler)
  {
    typename associated_allocator<Handler>::type allocator =
      (get_associated_allocator)(handler);

    execution::execute(
        boost::asio::prefer(
          boost::asio::require(executor_, execution::blocking.never),
          execution::allocator(allocator)),
        BOOST_ASIO_MOVE_CAST(Function)(function));
  }

private:
  typename decay<
      typename prefer_result<Executor,
        execution::outstanding_work_t::tracked_t
      >::type
    >::type executor_;
};

#if !defined(BOOST_ASIO_NO_TS_EXECUTORS)

template <typename Executor>
class channel_operation::handler_work_base<Executor,
    typename enable_if<
      !execution::is_executor<Executor>::value
    >::type>
{
public:
  handler_work_base(int, const Executor& ex)
    : work_(ex)
  {
  }

  template <typename Function, typename Handler>
  void post(Function& function, Handler& handler)
  {
    typename associated_allocator<Handler>::type allocator =
      (get_associated_allocator)(handler);

    work_.get_executor().post(
        BOOST_ASIO_MOVE_CAST(Function)(function), allocator);
  }

private:
  executor_work_guard<Executor> work_;
};

#endif // !defined(BOOST_ASIO_NO_TS_EXECUTORS)

template <typename Handler, typename IoExecutor, typename>
class channel_operation::handler_work :
  channel_operation::handler_work_base<IoExecutor>,
  channel_operation::handler_work_base<
      typename associated_executor<Handler, IoExecutor>::type, IoExecutor>
{
public:
  typedef channel_operation::handler_work_base<IoExecutor> base1_type;

  typedef channel_operation::handler_work_base<
      typename associated_executor<Handler, IoExecutor>::type, IoExecutor>
    base2_type;

  handler_work(Handler& handler, const IoExecutor& io_ex) BOOST_ASIO_NOEXCEPT
    : base1_type(0, io_ex),
      base2_type(0, (get_associated_executor)(handler, io_ex))
  {
  }

  template <typename Function>
  void complete(Function& function, Handler& handler)
  {
    base2_type::post(function, handler);
  }
};

template <typename Handler, typename IoExecutor>
class channel_operation::handler_work<
    Handler, IoExecutor,
    typename enable_if<
      is_same<
        typename associated_executor<Handler,
          IoExecutor>::asio_associated_executor_is_unspecialised,
        void
      >::value
    >::type> : handler_work_base<IoExecutor>
{
public:
  typedef channel_operation::handler_work_base<IoExecutor> base1_type;

  handler_work(Handler&, const IoExecutor& io_ex) BOOST_ASIO_NOEXCEPT
    : base1_type(0, io_ex)
  {
  }

  template <typename Function>
  void complete(Function& function, Handler& handler)
  {
    base1_type::post(function, handler);
  }
};

} // namespace detail
} // namespace experimental
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_EXPERIMENTAL_DETAIL_CHANNEL_OPERATION_HPP
