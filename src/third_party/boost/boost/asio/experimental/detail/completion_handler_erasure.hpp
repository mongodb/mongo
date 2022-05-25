//
// experimental/detail/completion_handler_erasure.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2021-2022 Klemens D. Morgenstern
//                         (klemens dot morgenstern at gmx dot net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_EXPERIMENTAL_DETAIL_COMPLETION_HANDLER_ERASURE_HPP
#define BOOST_ASIO_EXPERIMENTAL_DETAIL_COMPLETION_HANDLER_ERASURE_HPP

#include <new>
#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/dispatch.hpp>

namespace boost {
namespace asio {

class any_io_executor;

namespace experimental {
namespace detail {

template<typename Signature, typename Executor>
struct completion_handler_erasure_base;

template<typename Func, typename Signature, typename Executor>
struct completion_handler_erasure_impl;

template<typename Return, typename... Args, typename Executor>
struct completion_handler_erasure_base<Return(Args...), Executor>
{
  Executor executor;

  completion_handler_erasure_base(Executor&& executor)
    : executor(std::move(executor))
  {
  }

  virtual Return call(Args... args) = 0;
  virtual void destroy() = 0;
  virtual ~completion_handler_erasure_base() = default;
};

template<typename Func, typename Return, typename... Args, typename Executor>
struct completion_handler_erasure_impl<Func, Return(Args...), Executor> final
    : completion_handler_erasure_base<Return(Args...), Executor>
{
  using allocator_base = typename associated_allocator<Func>::type;
  using allocator_type =
    typename std::allocator_traits<allocator_base>::template rebind_alloc<
      completion_handler_erasure_impl>;

  completion_handler_erasure_impl(Executor&& exec, Func&& func)
    : completion_handler_erasure_base<Return(Args...), Executor>(
        std::move(exec)), func(std::move(func))
  {
  }

  struct uninit_deleter_t
  {
    allocator_type allocator;

    uninit_deleter_t(const Func& func)
      : allocator(get_associated_allocator(func))
    {
    }

    void operator()(completion_handler_erasure_impl* p)
    {
      std::allocator_traits<allocator_type>::deallocate(allocator, p, 1);
    }
  };

  static completion_handler_erasure_impl* make(Executor exec, Func&& func)
  {
    uninit_deleter_t deleter(func);
    std::unique_ptr<completion_handler_erasure_impl, uninit_deleter_t>
      uninit_ptr(std::allocator_traits<allocator_type>::allocate(
            deleter.allocator, 1), deleter);
    completion_handler_erasure_impl* ptr =
      new (uninit_ptr.get()) completion_handler_erasure_impl(
        std::move(exec), std::move(func));
    uninit_ptr.release();
    return ptr;
  }

  struct deleter_t
  {
    allocator_type allocator;

    deleter_t(const Func& func)
      : allocator(get_associated_allocator(func))
    {
    }

    void operator()(completion_handler_erasure_impl* p)
    {
      std::allocator_traits<allocator_type>::destroy(allocator, p);
      std::allocator_traits<allocator_type>::deallocate(allocator, p, 1);
    }
  };

  virtual Return call(Args... args) override
  {
    std::unique_ptr<completion_handler_erasure_impl,
      deleter_t> p(this, deleter_t(func));
    Func f(std::move(func));
    p.reset();
    std::move(f)(std::move(args)...);
  }

  virtual void destroy() override
  {
    std::unique_ptr<completion_handler_erasure_impl,
      deleter_t>(this, deleter_t(func));
  }

  Func func;
};

template<typename Signature, typename Executor = any_io_executor>
struct completion_handler_erasure;

template<typename Return, typename... Args, typename Executor>
struct completion_handler_erasure<Return(Args...), Executor>
{
  struct deleter_t
  {
    void operator()(
        completion_handler_erasure_base<Return(Args...), Executor>* p)
    {
      p->destroy();
    }
  };

  completion_handler_erasure(const completion_handler_erasure&) = delete;
  completion_handler_erasure(completion_handler_erasure&&) = default;
  completion_handler_erasure& operator=(
      const completion_handler_erasure&) = delete;
  completion_handler_erasure& operator=(
      completion_handler_erasure&&) = default;

  constexpr completion_handler_erasure() = default;

  constexpr completion_handler_erasure(nullptr_t)
    : completion_handler_erasure()
  {
  }

  template<typename Func>
  completion_handler_erasure(Executor exec, Func&& func)
    : impl_(completion_handler_erasure_impl<
        std::decay_t<Func>, Return(Args...), Executor>::make(
          std::move(exec), std::forward<Func>(func)))
  {
  }

  ~completion_handler_erasure()
  {
    if (impl_)
    {
      Executor executor(impl_->executor);
      boost::asio::dispatch(executor,
          [impl = std::move(impl_)]() mutable
          {
            impl.reset();
          });
    }
  }

  Return operator()(Args... args)
  {
    if (impl_)
      impl_.release()->call(std::move(args)...);
  }

  constexpr bool operator==(nullptr_t) const noexcept {return impl_ == nullptr;}
  constexpr bool operator!=(nullptr_t) const noexcept {return impl_ != nullptr;}
  constexpr bool operator!() const noexcept {return impl_ == nullptr;}

private:
  std::unique_ptr<
    completion_handler_erasure_base<Return(Args...), Executor>, deleter_t>
      impl_;
};

} // namespace detail
} // namespace experimental
} // namespace asio
} // namespace boost

#endif // BOOST_ASIO_EXPERIMENTAL_DETAIL_COMPLETION_HANDLER_ERASURE_HPP
