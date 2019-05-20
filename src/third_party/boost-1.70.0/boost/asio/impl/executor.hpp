//
// impl/executor.hpp
// ~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2019 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_IMPL_EXECUTOR_HPP
#define BOOST_ASIO_IMPL_EXECUTOR_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/detail/atomic_count.hpp>
#include <boost/asio/detail/executor_function.hpp>
#include <boost/asio/detail/global.hpp>
#include <boost/asio/detail/memory.hpp>
#include <boost/asio/detail/recycling_allocator.hpp>
#include <boost/asio/executor.hpp>
#include <boost/asio/system_executor.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

#if !defined(GENERATING_DOCUMENTATION)

#if defined(BOOST_ASIO_HAS_MOVE)

// Lightweight, move-only function object wrapper.
class executor::function
{
public:
  template <typename F, typename Alloc>
  explicit function(F f, const Alloc& a)
  {
    // Allocate and construct an operation to wrap the function.
    typedef detail::executor_function<F, Alloc> func_type;
    typename func_type::ptr p = {
      detail::addressof(a), func_type::ptr::allocate(a), 0 };
    func_ = new (p.v) func_type(BOOST_ASIO_MOVE_CAST(F)(f), a);
    p.v = 0;
  }

  function(function&& other) BOOST_ASIO_NOEXCEPT
    : func_(other.func_)
  {
    other.func_ = 0;
  }

  ~function()
  {
    if (func_)
      func_->destroy();
  }

  void operator()()
  {
    if (func_)
    {
      detail::executor_function_base* func = func_;
      func_ = 0;
      func->complete();
    }
  }

private:
  detail::executor_function_base* func_;
};

#else // defined(BOOST_ASIO_HAS_MOVE)

// Not so lightweight, copyable function object wrapper.
class executor::function
{
public:
  template <typename F, typename Alloc>
  explicit function(const F& f, const Alloc&)
    : impl_(new impl<F>(f))
  {
  }

  void operator()()
  {
    impl_->invoke_(impl_.get());
  }

private:
  // Base class for polymorphic function implementations.
  struct impl_base
  {
    void (*invoke_)(impl_base*);
  };

  // Polymorphic function implementation.
  template <typename F>
  struct impl : impl_base
  {
    impl(const F& f)
      : function_(f)
    {
      invoke_ = &function::invoke<F>;
    }

    F function_;
  };

  // Helper to invoke a function.
  template <typename F>
  static void invoke(impl_base* i)
  {
    static_cast<impl<F>*>(i)->function_();
  }

  detail::shared_ptr<impl_base> impl_;
};

#endif // defined(BOOST_ASIO_HAS_MOVE)

// Default polymorphic allocator implementation.
template <typename Executor, typename Allocator>
class executor::impl
  : public executor::impl_base
{
public:
  typedef BOOST_ASIO_REBIND_ALLOC(Allocator, impl) allocator_type;

  static impl_base* create(const Executor& e, Allocator a = Allocator())
  {
    raw_mem mem(a);
    impl* p = new (mem.ptr_) impl(e, a);
    mem.ptr_ = 0;
    return p;
  }

  impl(const Executor& e, const Allocator& a) BOOST_ASIO_NOEXCEPT
    : impl_base(false),
      ref_count_(1),
      executor_(e),
      allocator_(a)
  {
  }

  impl_base* clone() const BOOST_ASIO_NOEXCEPT
  {
    ++ref_count_;
    return const_cast<impl_base*>(static_cast<const impl_base*>(this));
  }

  void destroy() BOOST_ASIO_NOEXCEPT
  {
    if (--ref_count_ == 0)
    {
      allocator_type alloc(allocator_);
      impl* p = this;
      p->~impl();
      alloc.deallocate(p, 1);
    }
  }

  void on_work_started() BOOST_ASIO_NOEXCEPT
  {
    executor_.on_work_started();
  }

  void on_work_finished() BOOST_ASIO_NOEXCEPT
  {
    executor_.on_work_finished();
  }

  execution_context& context() BOOST_ASIO_NOEXCEPT
  {
    return executor_.context();
  }

  void dispatch(BOOST_ASIO_MOVE_ARG(function) f)
  {
    executor_.dispatch(BOOST_ASIO_MOVE_CAST(function)(f), allocator_);
  }

  void post(BOOST_ASIO_MOVE_ARG(function) f)
  {
    executor_.post(BOOST_ASIO_MOVE_CAST(function)(f), allocator_);
  }

  void defer(BOOST_ASIO_MOVE_ARG(function) f)
  {
    executor_.defer(BOOST_ASIO_MOVE_CAST(function)(f), allocator_);
  }

  type_id_result_type target_type() const BOOST_ASIO_NOEXCEPT
  {
    return type_id<Executor>();
  }

  void* target() BOOST_ASIO_NOEXCEPT
  {
    return &executor_;
  }

  const void* target() const BOOST_ASIO_NOEXCEPT
  {
    return &executor_;
  }

  bool equals(const impl_base* e) const BOOST_ASIO_NOEXCEPT
  {
    if (this == e)
      return true;
    if (target_type() != e->target_type())
      return false;
    return executor_ == *static_cast<const Executor*>(e->target());
  }

private:
  mutable detail::atomic_count ref_count_;
  Executor executor_;
  Allocator allocator_;

  struct raw_mem
  {
    allocator_type allocator_;
    impl* ptr_;

    explicit raw_mem(const Allocator& a)
      : allocator_(a),
        ptr_(allocator_.allocate(1))
    {
    }

    ~raw_mem()
    {
      if (ptr_)
        allocator_.deallocate(ptr_, 1);
    }

  private:
    // Disallow copying and assignment.
    raw_mem(const raw_mem&);
    raw_mem operator=(const raw_mem&);
  };
};

// Polymorphic allocator specialisation for system_executor.
template <typename Allocator>
class executor::impl<system_executor, Allocator>
  : public executor::impl_base
{
public:
  static impl_base* create(const system_executor&,
      const Allocator& = Allocator())
  {
    return &detail::global<impl<system_executor, std::allocator<void> > >();
  }

  impl()
    : impl_base(true)
  {
  }

  impl_base* clone() const BOOST_ASIO_NOEXCEPT
  {
    return const_cast<impl_base*>(static_cast<const impl_base*>(this));
  }

  void destroy() BOOST_ASIO_NOEXCEPT
  {
  }

  void on_work_started() BOOST_ASIO_NOEXCEPT
  {
    executor_.on_work_started();
  }

  void on_work_finished() BOOST_ASIO_NOEXCEPT
  {
    executor_.on_work_finished();
  }

  execution_context& context() BOOST_ASIO_NOEXCEPT
  {
    return executor_.context();
  }

  void dispatch(BOOST_ASIO_MOVE_ARG(function) f)
  {
    executor_.dispatch(BOOST_ASIO_MOVE_CAST(function)(f), allocator_);
  }

  void post(BOOST_ASIO_MOVE_ARG(function) f)
  {
    executor_.post(BOOST_ASIO_MOVE_CAST(function)(f), allocator_);
  }

  void defer(BOOST_ASIO_MOVE_ARG(function) f)
  {
    executor_.defer(BOOST_ASIO_MOVE_CAST(function)(f), allocator_);
  }

  type_id_result_type target_type() const BOOST_ASIO_NOEXCEPT
  {
    return type_id<system_executor>();
  }

  void* target() BOOST_ASIO_NOEXCEPT
  {
    return &executor_;
  }

  const void* target() const BOOST_ASIO_NOEXCEPT
  {
    return &executor_;
  }

  bool equals(const impl_base* e) const BOOST_ASIO_NOEXCEPT
  {
    return this == e;
  }

private:
  system_executor executor_;
  Allocator allocator_;
};

template <typename Executor>
executor::executor(Executor e)
  : impl_(impl<Executor, std::allocator<void> >::create(e))
{
}

template <typename Executor, typename Allocator>
executor::executor(allocator_arg_t, const Allocator& a, Executor e)
  : impl_(impl<Executor, Allocator>::create(e, a))
{
}

template <typename Function, typename Allocator>
void executor::dispatch(BOOST_ASIO_MOVE_ARG(Function) f,
    const Allocator& a) const
{
  impl_base* i = get_impl();
  if (i->fast_dispatch_)
    system_executor().dispatch(BOOST_ASIO_MOVE_CAST(Function)(f), a);
  else
    i->dispatch(function(BOOST_ASIO_MOVE_CAST(Function)(f), a));
}

template <typename Function, typename Allocator>
void executor::post(BOOST_ASIO_MOVE_ARG(Function) f,
    const Allocator& a) const
{
  get_impl()->post(function(BOOST_ASIO_MOVE_CAST(Function)(f), a));
}

template <typename Function, typename Allocator>
void executor::defer(BOOST_ASIO_MOVE_ARG(Function) f,
    const Allocator& a) const
{
  get_impl()->defer(function(BOOST_ASIO_MOVE_CAST(Function)(f), a));
}

template <typename Executor>
Executor* executor::target() BOOST_ASIO_NOEXCEPT
{
  return impl_ && impl_->target_type() == type_id<Executor>()
    ? static_cast<Executor*>(impl_->target()) : 0;
}

template <typename Executor>
const Executor* executor::target() const BOOST_ASIO_NOEXCEPT
{
  return impl_ && impl_->target_type() == type_id<Executor>()
    ? static_cast<Executor*>(impl_->target()) : 0;
}

#endif // !defined(GENERATING_DOCUMENTATION)

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_IMPL_EXECUTOR_HPP
