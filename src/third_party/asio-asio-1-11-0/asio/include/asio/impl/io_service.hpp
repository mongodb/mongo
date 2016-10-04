//
// impl/io_service.hpp
// ~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_IMPL_IO_SERVICE_HPP
#define ASIO_IMPL_IO_SERVICE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/completion_handler.hpp"
#include "asio/detail/executor_op.hpp"
#include "asio/detail/fenced_block.hpp"
#include "asio/detail/handler_type_requirements.hpp"
#include "asio/detail/recycling_allocator.hpp"
#include "asio/detail/service_registry.hpp"
#include "asio/detail/type_traits.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {

template <typename Service>
inline Service& use_service(io_service& ios)
{
  // Check that Service meets the necessary type requirements.
  (void)static_cast<execution_context::service*>(static_cast<Service*>(0));
  (void)static_cast<const execution_context::id*>(&Service::id);

  return ios.service_registry_->template use_service<Service>(ios);
}

template <>
inline detail::io_service_impl& use_service<detail::io_service_impl>(
    io_service& ios)
{
  return ios.impl_;
}

} // namespace asio

#include "asio/detail/pop_options.hpp"

#if defined(ASIO_HAS_IOCP)
# include "asio/detail/win_iocp_io_service.hpp"
#else
# include "asio/detail/scheduler.hpp"
#endif

#include "asio/detail/push_options.hpp"

namespace asio {

inline io_service::executor_type
io_service::get_executor() ASIO_NOEXCEPT
{
  return executor_type(*this);
}

#if !defined(ASIO_NO_DEPRECATED)

inline void io_service::reset()
{
  restart();
}

template <typename CompletionHandler>
ASIO_INITFN_RESULT_TYPE(CompletionHandler, void ())
io_service::dispatch(ASIO_MOVE_ARG(CompletionHandler) handler)
{
  // If you get an error on the following line it means that your handler does
  // not meet the documented type requirements for a CompletionHandler.
  ASIO_COMPLETION_HANDLER_CHECK(CompletionHandler, handler) type_check;

  async_completion<CompletionHandler, void ()> init(handler);

  if (impl_.can_dispatch())
  {
    detail::fenced_block b(detail::fenced_block::full);
    asio_handler_invoke_helpers::invoke(init.handler, init.handler);
  }
  else
  {
    // Allocate and construct an operation to wrap the handler.
    typedef detail::completion_handler<
      typename handler_type<CompletionHandler, void ()>::type> op;
    typename op::ptr p = { detail::addressof(init.handler),
      op::ptr::allocate(init.handler), 0 };
    p.p = new (p.v) op(init.handler);

    ASIO_HANDLER_CREATION((p.p, "io_service", this, "dispatch"));

    impl_.do_dispatch(p.p);
    p.v = p.p = 0;
  }

  return init.result.get();
}

template <typename CompletionHandler>
ASIO_INITFN_RESULT_TYPE(CompletionHandler, void ())
io_service::post(ASIO_MOVE_ARG(CompletionHandler) handler)
{
  // If you get an error on the following line it means that your handler does
  // not meet the documented type requirements for a CompletionHandler.
  ASIO_COMPLETION_HANDLER_CHECK(CompletionHandler, handler) type_check;

  async_completion<CompletionHandler, void ()> init(handler);

  bool is_continuation =
    asio_handler_cont_helpers::is_continuation(init.handler);

  // Allocate and construct an operation to wrap the handler.
  typedef detail::completion_handler<
    typename handler_type<CompletionHandler, void ()>::type> op;
  typename op::ptr p = { detail::addressof(init.handler),
      op::ptr::allocate(init.handler), 0 };
  p.p = new (p.v) op(init.handler);

  ASIO_HANDLER_CREATION((p.p, "io_service", this, "post"));

  impl_.post_immediate_completion(p.p, is_continuation);
  p.v = p.p = 0;

  return init.result.get();
}

template <typename Handler>
#if defined(GENERATING_DOCUMENTATION)
unspecified
#else
inline detail::wrapped_handler<io_service&, Handler>
#endif
io_service::wrap(Handler handler)
{
  return detail::wrapped_handler<io_service&, Handler>(*this, handler);
}

#endif // !defined(ASIO_NO_DEPRECATED)

inline io_service&
io_service::executor_type::context() ASIO_NOEXCEPT
{
  return io_service_;
}

inline void io_service::executor_type::on_work_started() ASIO_NOEXCEPT
{
  io_service_.impl_.work_started();
}

inline void io_service::executor_type::on_work_finished() ASIO_NOEXCEPT
{
  io_service_.impl_.work_finished();
}

template <typename Function, typename Allocator>
void io_service::executor_type::dispatch(
    ASIO_MOVE_ARG(Function) f, const Allocator& a)
{
  // Make a local, non-const copy of the function.
  typedef typename decay<Function>::type function_type;
  function_type tmp(ASIO_MOVE_CAST(Function)(f));

  // Invoke immediately if we are already inside the thread pool.
  if (io_service_.impl_.can_dispatch())
  {
    detail::fenced_block b(detail::fenced_block::full);
    asio_handler_invoke_helpers::invoke(tmp, tmp);
    return;
  }

  // Construct an allocator to be used for the operation.
  typedef typename detail::get_recycling_allocator<Allocator>::type alloc_type;
  alloc_type allocator(detail::get_recycling_allocator<Allocator>::get(a));

  // Allocate and construct an operation to wrap the function.
  typedef detail::executor_op<function_type, alloc_type, detail::operation> op;
  typename op::ptr p = { allocator, 0, 0 };
  p.v = p.a.allocate(1);
  p.p = new (p.v) op(tmp, allocator);

  ASIO_HANDLER_CREATION((p.p, "io_service", this, "post"));

  io_service_.impl_.post_immediate_completion(p.p, false);
  p.v = p.p = 0;
}

template <typename Function, typename Allocator>
void io_service::executor_type::post(
    ASIO_MOVE_ARG(Function) f, const Allocator& a)
{
  // Make a local, non-const copy of the function.
  typedef typename decay<Function>::type function_type;
  function_type tmp(ASIO_MOVE_CAST(Function)(f));

  // Construct an allocator to be used for the operation.
  typedef typename detail::get_recycling_allocator<Allocator>::type alloc_type;
  alloc_type allocator(detail::get_recycling_allocator<Allocator>::get(a));

  // Allocate and construct an operation to wrap the function.
  typedef detail::executor_op<function_type, alloc_type, detail::operation> op;
  typename op::ptr p = { allocator, 0, 0 };
  p.v = p.a.allocate(1);
  p.p = new (p.v) op(tmp, allocator);

  ASIO_HANDLER_CREATION((p.p, "io_service", this, "post"));

  io_service_.impl_.post_immediate_completion(p.p, false);
  p.v = p.p = 0;
}

template <typename Function, typename Allocator>
void io_service::executor_type::defer(
    ASIO_MOVE_ARG(Function) f, const Allocator& a)
{
  // Make a local, non-const copy of the function.
  typedef typename decay<Function>::type function_type;
  function_type tmp(ASIO_MOVE_CAST(Function)(f));

  // Construct an allocator to be used for the operation.
  typedef typename detail::get_recycling_allocator<Allocator>::type alloc_type;
  alloc_type allocator(detail::get_recycling_allocator<Allocator>::get(a));

  // Allocate and construct an operation to wrap the function.
  typedef detail::executor_op<function_type, alloc_type, detail::operation> op;
  typename op::ptr p = { allocator, 0, 0 };
  p.v = p.a.allocate(1);
  p.p = new (p.v) op(tmp, allocator);

  ASIO_HANDLER_CREATION((p.p, "io_service", this, "defer"));

  io_service_.impl_.post_immediate_completion(p.p, true);
  p.v = p.p = 0;
}

inline bool
io_service::executor_type::running_in_this_thread() const ASIO_NOEXCEPT
{
  return io_service_.impl_.can_dispatch();
}

inline io_service::work::work(asio::io_service& io_service)
  : io_service_impl_(io_service.impl_)
{
  io_service_impl_.work_started();
}

inline io_service::work::work(const work& other)
  : io_service_impl_(other.io_service_impl_)
{
  io_service_impl_.work_started();
}

inline io_service::work::~work()
{
  io_service_impl_.work_finished();
}

inline asio::io_service& io_service::work::get_io_service()
{
  return static_cast<asio::io_service&>(io_service_impl_.context());
}

inline asio::io_service& io_service::service::get_io_service()
{
  return static_cast<asio::io_service&>(context());
}

} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_IMPL_IO_SERVICE_HPP
