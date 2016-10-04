//
// detail/impl/strand_executor_service.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_DETAIL_IMPL_STRAND_EXECUTOR_SERVICE_HPP
#define ASIO_DETAIL_IMPL_STRAND_EXECUTOR_SERVICE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/call_stack.hpp"
#include "asio/detail/fenced_block.hpp"
#include "asio/detail/handler_invoke_helpers.hpp"
#include "asio/detail/recycling_allocator.hpp"
#include "asio/executor_work.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace detail {

template <typename Executor>
class strand_executor_service::invoker
{
public:
  invoker(const implementation_type& impl, Executor& ex)
    : impl_(impl),
      work_(ex)
  {
  }

  invoker(const invoker& other)
    : impl_(other.impl_),
      work_(other.work_)
  {
  }

#if defined(ASIO_HAS_MOVE)
  invoker(invoker&& other)
    : impl_(ASIO_MOVE_CAST(implementation_type)(other.impl_)),
      work_(ASIO_MOVE_CAST(executor_work<Executor>)(other.work_))
  {
  }
#endif // defined(ASIO_HAS_MOVE)

  struct on_invoker_exit
  {
    invoker* this_;

    ~on_invoker_exit()
    {
      this_->impl_->mutex_->lock();
      this_->impl_->ready_queue_.push(this_->impl_->waiting_queue_);
      bool more_handlers = this_->impl_->locked_ =
        !this_->impl_->ready_queue_.empty();
      this_->impl_->mutex_->unlock();

      if (more_handlers)
      {
        Executor ex(this_->work_.get_executor());
        recycling_allocator<void> allocator;
        ex.post(ASIO_MOVE_CAST(invoker)(*this_), allocator);
      }
    }
  };

  void operator()()
  {
    // Indicate that this strand is executing on the current thread.
    call_stack<strand_impl>::context ctx(impl_.get());

    // Ensure the next handler, if any, is scheduled on block exit.
    on_invoker_exit on_exit = { this };
    (void)on_exit;

    // Run all ready handlers. No lock is required since the ready queue is
    // accessed only within the strand.
    asio::error_code ec;
    while (scheduler_operation* o = impl_->ready_queue_.front())
    {
      impl_->ready_queue_.pop();
      o->complete(impl_.get(), ec, 0);
    }
  }

private:
  implementation_type impl_;
  executor_work<Executor> work_;
};

template <typename Executor, typename Function, typename Allocator>
void strand_executor_service::dispatch(const implementation_type& impl,
    Executor& ex, ASIO_MOVE_ARG(Function) function, const Allocator& a)
{
  // Make a local, non-const copy of the function.
  typedef typename decay<Function>::type function_type;
  function_type tmp(ASIO_MOVE_CAST(Function)(function));

  // If we are already in the strand then the function can run immediately.
  if (call_stack<strand_impl>::contains(impl.get()))
  {
    fenced_block b(fenced_block::full);
    asio_handler_invoke_helpers::invoke(tmp, tmp);
    return;
  }

  // Construct an allocator to be used for the operation.
  typedef typename detail::get_recycling_allocator<Allocator>::type alloc_type;
  alloc_type allocator(detail::get_recycling_allocator<Allocator>::get(a));

  // Allocate and construct an operation to wrap the function.
  typedef executor_op<function_type, alloc_type> op;
  typename op::ptr p = { allocator, 0, 0 };
  p.v = p.a.allocate(1);
  p.p = new (p.v) op(tmp, allocator);

  ASIO_HANDLER_CREATION((p.p, "strand_executor", this, "dispatch"));

  // Add the function to the strand and schedule the strand if required.
  bool first = enqueue(impl, p.p);
  p.v = p.p = 0;
  if (first)
    ex.dispatch(invoker<Executor>(impl, ex), allocator);
}

// Request invocation of the given function and return immediately.
template <typename Executor, typename Function, typename Allocator>
void strand_executor_service::post(const implementation_type& impl,
    Executor& ex, ASIO_MOVE_ARG(Function) function, const Allocator& a)
{
  // Make a local, non-const copy of the function.
  typedef typename decay<Function>::type function_type;
  function_type tmp(ASIO_MOVE_CAST(Function)(function));

  // Construct an allocator to be used for the operation.
  typedef typename detail::get_recycling_allocator<Allocator>::type alloc_type;
  alloc_type allocator(detail::get_recycling_allocator<Allocator>::get(a));

  // Allocate and construct an operation to wrap the function.
  typedef executor_op<function_type, alloc_type> op;
  typename op::ptr p = { allocator, 0, 0 };
  p.v = p.a.allocate(1);
  p.p = new (p.v) op(tmp, allocator);

  ASIO_HANDLER_CREATION((p.p, "strand_executor", this, "post"));

  // Add the function to the strand and schedule the strand if required.
  bool first = enqueue(impl, p.p);
  p.v = p.p = 0;
  if (first)
    ex.post(invoker<Executor>(impl, ex), allocator);
}

// Request invocation of the given function and return immediately.
template <typename Executor, typename Function, typename Allocator>
void strand_executor_service::defer(const implementation_type& impl,
    Executor& ex, ASIO_MOVE_ARG(Function) function, const Allocator& a)
{
  // Make a local, non-const copy of the function.
  typedef typename decay<Function>::type function_type;
  function_type tmp(ASIO_MOVE_CAST(Function)(function));

  // Construct an allocator to be used for the operation.
  typedef typename detail::get_recycling_allocator<Allocator>::type alloc_type;
  alloc_type allocator(detail::get_recycling_allocator<Allocator>::get(a));

  // Allocate and construct an operation to wrap the function.
  typedef executor_op<function_type, alloc_type> op;
  typename op::ptr p = { allocator, 0, 0 };
  p.v = p.a.allocate(1);
  p.p = new (p.v) op(tmp, allocator);

  ASIO_HANDLER_CREATION((p.p, "strand_executor", this, "defer"));

  // Add the function to the strand and schedule the strand if required.
  bool first = enqueue(impl, p.p);
  p.v = p.p = 0;
  if (first)
    ex.defer(invoker<Executor>(impl, ex), allocator);
}

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_DETAIL_IMPL_STRAND_EXECUTOR_SERVICE_HPP
