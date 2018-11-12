//
// impl/io_context.hpp
// ~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2018 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_IMPL_IO_CONTEXT_HPP
#define BOOST_ASIO_IMPL_IO_CONTEXT_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/completion_handler.hpp>
#include <boost/asio/detail/executor_op.hpp>
#include <boost/asio/detail/fenced_block.hpp>
#include <boost/asio/detail/handler_type_requirements.hpp>
#include <boost/asio/detail/recycling_allocator.hpp>
#include <boost/asio/detail/service_registry.hpp>
#include <boost/asio/detail/throw_error.hpp>
#include <boost/asio/detail/type_traits.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

template <typename Service>
inline Service& use_service(io_context& ioc)
{
  // Check that Service meets the necessary type requirements.
  (void)static_cast<execution_context::service*>(static_cast<Service*>(0));
  (void)static_cast<const execution_context::id*>(&Service::id);

  return ioc.service_registry_->template use_service<Service>(ioc);
}

template <>
inline detail::io_context_impl& use_service<detail::io_context_impl>(
    io_context& ioc)
{
  return ioc.impl_;
}

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#if defined(BOOST_ASIO_HAS_IOCP)
# include <boost/asio/detail/win_iocp_io_context.hpp>
#else
# include <boost/asio/detail/scheduler.hpp>
#endif

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

inline io_context::executor_type
io_context::get_executor() BOOST_ASIO_NOEXCEPT
{
  return executor_type(*this);
}

#if defined(BOOST_ASIO_HAS_CHRONO)

template <typename Rep, typename Period>
std::size_t io_context::run_for(
    const chrono::duration<Rep, Period>& rel_time)
{
  return this->run_until(chrono::steady_clock::now() + rel_time);
}

template <typename Clock, typename Duration>
std::size_t io_context::run_until(
    const chrono::time_point<Clock, Duration>& abs_time)
{
  std::size_t n = 0;
  while (this->run_one_until(abs_time))
    if (n != (std::numeric_limits<std::size_t>::max)())
      ++n;
  return n;
}

template <typename Rep, typename Period>
std::size_t io_context::run_one_for(
    const chrono::duration<Rep, Period>& rel_time)
{
  return this->run_one_until(chrono::steady_clock::now() + rel_time);
}

template <typename Clock, typename Duration>
std::size_t io_context::run_one_until(
    const chrono::time_point<Clock, Duration>& abs_time)
{
  typename Clock::time_point now = Clock::now();
  while (now < abs_time)
  {
    typename Clock::duration rel_time = abs_time - now;
    if (rel_time > chrono::seconds(1))
      rel_time = chrono::seconds(1);

    boost::system::error_code ec;
    std::size_t s = impl_.wait_one(
        static_cast<long>(chrono::duration_cast<
          chrono::microseconds>(rel_time).count()), ec);
    boost::asio::detail::throw_error(ec);

    if (s || impl_.stopped())
      return s;

    now = Clock::now();
  }

  return 0;
}

#endif // defined(BOOST_ASIO_HAS_CHRONO)

#if !defined(BOOST_ASIO_NO_DEPRECATED)

inline void io_context::reset()
{
  restart();
}

template <typename LegacyCompletionHandler>
BOOST_ASIO_INITFN_RESULT_TYPE(LegacyCompletionHandler, void ())
io_context::dispatch(BOOST_ASIO_MOVE_ARG(LegacyCompletionHandler) handler)
{
  // If you get an error on the following line it means that your handler does
  // not meet the documented type requirements for a LegacyCompletionHandler.
  BOOST_ASIO_LEGACY_COMPLETION_HANDLER_CHECK(
      LegacyCompletionHandler, handler) type_check;

  async_completion<LegacyCompletionHandler, void ()> init(handler);

  if (impl_.can_dispatch())
  {
    detail::fenced_block b(detail::fenced_block::full);
    boost_asio_handler_invoke_helpers::invoke(
        init.completion_handler, init.completion_handler);
  }
  else
  {
    // Allocate and construct an operation to wrap the handler.
    typedef detail::completion_handler<
      typename handler_type<LegacyCompletionHandler, void ()>::type> op;
    typename op::ptr p = { detail::addressof(init.completion_handler),
      op::ptr::allocate(init.completion_handler), 0 };
    p.p = new (p.v) op(init.completion_handler);

    BOOST_ASIO_HANDLER_CREATION((*this, *p.p,
          "io_context", this, 0, "dispatch"));

    impl_.do_dispatch(p.p);
    p.v = p.p = 0;
  }

  return init.result.get();
}

template <typename LegacyCompletionHandler>
BOOST_ASIO_INITFN_RESULT_TYPE(LegacyCompletionHandler, void ())
io_context::post(BOOST_ASIO_MOVE_ARG(LegacyCompletionHandler) handler)
{
  // If you get an error on the following line it means that your handler does
  // not meet the documented type requirements for a LegacyCompletionHandler.
  BOOST_ASIO_LEGACY_COMPLETION_HANDLER_CHECK(
      LegacyCompletionHandler, handler) type_check;

  async_completion<LegacyCompletionHandler, void ()> init(handler);

  bool is_continuation =
    boost_asio_handler_cont_helpers::is_continuation(init.completion_handler);

  // Allocate and construct an operation to wrap the handler.
  typedef detail::completion_handler<
    typename handler_type<LegacyCompletionHandler, void ()>::type> op;
  typename op::ptr p = { detail::addressof(init.completion_handler),
      op::ptr::allocate(init.completion_handler), 0 };
  p.p = new (p.v) op(init.completion_handler);

  BOOST_ASIO_HANDLER_CREATION((*this, *p.p,
        "io_context", this, 0, "post"));

  impl_.post_immediate_completion(p.p, is_continuation);
  p.v = p.p = 0;

  return init.result.get();
}

template <typename Handler>
#if defined(GENERATING_DOCUMENTATION)
unspecified
#else
inline detail::wrapped_handler<io_context&, Handler>
#endif
io_context::wrap(Handler handler)
{
  return detail::wrapped_handler<io_context&, Handler>(*this, handler);
}

#endif // !defined(BOOST_ASIO_NO_DEPRECATED)

inline io_context&
io_context::executor_type::context() const BOOST_ASIO_NOEXCEPT
{
  return io_context_;
}

inline void
io_context::executor_type::on_work_started() const BOOST_ASIO_NOEXCEPT
{
  io_context_.impl_.work_started();
}

inline void
io_context::executor_type::on_work_finished() const BOOST_ASIO_NOEXCEPT
{
  io_context_.impl_.work_finished();
}

template <typename Function, typename Allocator>
void io_context::executor_type::dispatch(
    BOOST_ASIO_MOVE_ARG(Function) f, const Allocator& a) const
{
  typedef typename decay<Function>::type function_type;

  // Invoke immediately if we are already inside the thread pool.
  if (io_context_.impl_.can_dispatch())
  {
    // Make a local, non-const copy of the function.
    function_type tmp(BOOST_ASIO_MOVE_CAST(Function)(f));

    detail::fenced_block b(detail::fenced_block::full);
    boost_asio_handler_invoke_helpers::invoke(tmp, tmp);
    return;
  }

  // Allocate and construct an operation to wrap the function.
  typedef detail::executor_op<function_type, Allocator, detail::operation> op;
  typename op::ptr p = { detail::addressof(a), op::ptr::allocate(a), 0 };
  p.p = new (p.v) op(BOOST_ASIO_MOVE_CAST(Function)(f), a);

  BOOST_ASIO_HANDLER_CREATION((this->context(), *p.p,
        "io_context", &this->context(), 0, "post"));

  io_context_.impl_.post_immediate_completion(p.p, false);
  p.v = p.p = 0;
}

template <typename Function, typename Allocator>
void io_context::executor_type::post(
    BOOST_ASIO_MOVE_ARG(Function) f, const Allocator& a) const
{
  typedef typename decay<Function>::type function_type;

  // Allocate and construct an operation to wrap the function.
  typedef detail::executor_op<function_type, Allocator, detail::operation> op;
  typename op::ptr p = { detail::addressof(a), op::ptr::allocate(a), 0 };
  p.p = new (p.v) op(BOOST_ASIO_MOVE_CAST(Function)(f), a);

  BOOST_ASIO_HANDLER_CREATION((this->context(), *p.p,
        "io_context", &this->context(), 0, "post"));

  io_context_.impl_.post_immediate_completion(p.p, false);
  p.v = p.p = 0;
}

template <typename Function, typename Allocator>
void io_context::executor_type::defer(
    BOOST_ASIO_MOVE_ARG(Function) f, const Allocator& a) const
{
  typedef typename decay<Function>::type function_type;

  // Allocate and construct an operation to wrap the function.
  typedef detail::executor_op<function_type, Allocator, detail::operation> op;
  typename op::ptr p = { detail::addressof(a), op::ptr::allocate(a), 0 };
  p.p = new (p.v) op(BOOST_ASIO_MOVE_CAST(Function)(f), a);

  BOOST_ASIO_HANDLER_CREATION((this->context(), *p.p,
        "io_context", &this->context(), 0, "defer"));

  io_context_.impl_.post_immediate_completion(p.p, true);
  p.v = p.p = 0;
}

inline bool
io_context::executor_type::running_in_this_thread() const BOOST_ASIO_NOEXCEPT
{
  return io_context_.impl_.can_dispatch();
}

#if !defined(BOOST_ASIO_NO_DEPRECATED)
inline io_context::work::work(boost::asio::io_context& io_context)
  : io_context_impl_(io_context.impl_)
{
  io_context_impl_.work_started();
}

inline io_context::work::work(const work& other)
  : io_context_impl_(other.io_context_impl_)
{
  io_context_impl_.work_started();
}

inline io_context::work::~work()
{
  io_context_impl_.work_finished();
}

inline boost::asio::io_context& io_context::work::get_io_context()
{
  return static_cast<boost::asio::io_context&>(io_context_impl_.context());
}

inline boost::asio::io_context& io_context::work::get_io_service()
{
  return static_cast<boost::asio::io_context&>(io_context_impl_.context());
}
#endif // !defined(BOOST_ASIO_NO_DEPRECATED)

inline boost::asio::io_context& io_context::service::get_io_context()
{
  return static_cast<boost::asio::io_context&>(context());
}

#if !defined(BOOST_ASIO_NO_DEPRECATED)
inline boost::asio::io_context& io_context::service::get_io_service()
{
  return static_cast<boost::asio::io_context&>(context());
}
#endif // !defined(BOOST_ASIO_NO_DEPRECATED)

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_IMPL_IO_CONTEXT_HPP
