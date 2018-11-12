//
// impl/use_future.hpp
// ~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2018 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_IMPL_USE_FUTURE_HPP
#define BOOST_ASIO_IMPL_USE_FUTURE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <future>
#include <tuple>
#include <boost/asio/async_result.hpp>
#include <boost/asio/detail/memory.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/packaged_task.hpp>
#include <boost/system/system_error.hpp>
#include <boost/asio/system_executor.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace detail {

#if defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

template <typename T, typename F, typename... Args>
inline void promise_invoke_and_set(std::promise<T>& p,
    F& f, BOOST_ASIO_MOVE_ARG(Args)... args)
{
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
  try
#endif // !defined(BOOST_ASIO_NO_EXCEPTIONS)
  {
    p.set_value(f(BOOST_ASIO_MOVE_CAST(Args)(args)...));
  }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
  catch (...)
  {
    p.set_exception(std::current_exception());
  }
#endif // !defined(BOOST_ASIO_NO_EXCEPTIONS)
}

template <typename F, typename... Args>
inline void promise_invoke_and_set(std::promise<void>& p,
    F& f, BOOST_ASIO_MOVE_ARG(Args)... args)
{
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
  try
#endif // !defined(BOOST_ASIO_NO_EXCEPTIONS)
  {
    f(BOOST_ASIO_MOVE_CAST(Args)(args)...);
    p.set_value();
  }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
  catch (...)
  {
    p.set_exception(std::current_exception());
  }
#endif // !defined(BOOST_ASIO_NO_EXCEPTIONS)
}

#else // defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

template <typename T, typename F>
inline void promise_invoke_and_set(std::promise<T>& p, F& f)
{
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
  try
#endif // !defined(BOOST_ASIO_NO_EXCEPTIONS)
  {
    p.set_value(f());
  }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
  catch (...)
  {
    p.set_exception(std::current_exception());
  }
#endif // !defined(BOOST_ASIO_NO_EXCEPTIONS)
}

template <typename F, typename Args>
inline void promise_invoke_and_set(std::promise<void>& p, F& f)
{
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
  try
#endif // !defined(BOOST_ASIO_NO_EXCEPTIONS)
  {
    f();
    p.set_value();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
  }
  catch (...)
  {
    p.set_exception(std::current_exception());
  }
#endif // !defined(BOOST_ASIO_NO_EXCEPTIONS)
}

#if defined(BOOST_ASIO_NO_EXCEPTIONS)

#define BOOST_ASIO_PRIVATE_PROMISE_INVOKE_DEF(n) \
  template <typename T, typename F, BOOST_ASIO_VARIADIC_TPARAMS(n)> \
  inline void promise_invoke_and_set(std::promise<T>& p, \
      F& f, BOOST_ASIO_VARIADIC_MOVE_PARAMS(n)) \
  { \
    p.set_value(f(BOOST_ASIO_VARIADIC_MOVE_ARGS(n))); \
  } \
  \
  template <typename F, BOOST_ASIO_VARIADIC_TPARAMS(n)> \
  inline void promise_invoke_and_set(std::promise<void>& p, \
      F& f, BOOST_ASIO_VARIADIC_MOVE_PARAMS(n)) \
  { \
    f(BOOST_ASIO_VARIADIC_MOVE_ARGS(n)); \
    p.set_value(); \
  } \
  /**/
  BOOST_ASIO_VARIADIC_GENERATE(BOOST_ASIO_PRIVATE_PROMISE_INVOKE_DEF)
#undef BOOST_ASIO_PRIVATE_PROMISE_INVOKE_DEF

#else // defined(BOOST_ASIO_NO_EXCEPTIONS)

#define BOOST_ASIO_PRIVATE_PROMISE_INVOKE_DEF(n) \
  template <typename T, typename F, BOOST_ASIO_VARIADIC_TPARAMS(n)> \
  inline void promise_invoke_and_set(std::promise<T>& p, \
      F& f, BOOST_ASIO_VARIADIC_MOVE_PARAMS(n)) \
  { \
    try \
    { \
      p.set_value(f(BOOST_ASIO_VARIADIC_MOVE_ARGS(n))); \
    } \
    catch (...) \
    { \
      p.set_exception(std::current_exception()); \
    } \
  } \
  \
  template <typename F, BOOST_ASIO_VARIADIC_TPARAMS(n)> \
  inline void promise_invoke_and_set(std::promise<void>& p, \
      F& f, BOOST_ASIO_VARIADIC_MOVE_PARAMS(n)) \
  { \
    try \
    { \
      f(BOOST_ASIO_VARIADIC_MOVE_ARGS(n)); \
      p.set_value(); \
    } \
    catch (...) \
    { \
      p.set_exception(std::current_exception()); \
    } \
  } \
  /**/
  BOOST_ASIO_VARIADIC_GENERATE(BOOST_ASIO_PRIVATE_PROMISE_INVOKE_DEF)
#undef BOOST_ASIO_PRIVATE_PROMISE_INVOKE_DEF

#endif // defined(BOOST_ASIO_NO_EXCEPTIONS)

#endif // defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

// A function object adapter to invoke a nullary function object and capture
// any exception thrown into a promise.
template <typename T, typename F>
class promise_invoker
{
public:
  promise_invoker(const shared_ptr<std::promise<T> >& p,
      BOOST_ASIO_MOVE_ARG(F) f)
    : p_(p), f_(BOOST_ASIO_MOVE_CAST(F)(f))
  {
  }

  void operator()()
  {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try
#endif // !defined(BOOST_ASIO_NO_EXCEPTIONS)
    {
      f_();
    }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    catch (...)
    {
      p_->set_exception(std::current_exception());
    }
#endif // !defined(BOOST_ASIO_NO_EXCEPTIONS)
  }

private:
  shared_ptr<std::promise<T> > p_;
  typename decay<F>::type f_;
};

// An executor that adapts the system_executor to capture any exeption thrown
// by a submitted function object and save it into a promise.
template <typename T>
class promise_executor
{
public:
  explicit promise_executor(const shared_ptr<std::promise<T> >& p)
    : p_(p)
  {
  }

  execution_context& context() const BOOST_ASIO_NOEXCEPT
  {
    return system_executor().context();
  }

  void on_work_started() const BOOST_ASIO_NOEXCEPT {}
  void on_work_finished() const BOOST_ASIO_NOEXCEPT {}

  template <typename F, typename A>
  void dispatch(BOOST_ASIO_MOVE_ARG(F) f, const A&) const
  {
    promise_invoker<T, F>(p_, BOOST_ASIO_MOVE_CAST(F)(f))();
  }

  template <typename F, typename A>
  void post(BOOST_ASIO_MOVE_ARG(F) f, const A& a) const
  {
    system_executor().post(
        promise_invoker<T, F>(p_, BOOST_ASIO_MOVE_CAST(F)(f)), a);
  }

  template <typename F, typename A>
  void defer(BOOST_ASIO_MOVE_ARG(F) f, const A& a) const
  {
    system_executor().defer(
        promise_invoker<T, F>(p_, BOOST_ASIO_MOVE_CAST(F)(f)), a);
  }

  friend bool operator==(const promise_executor& a,
      const promise_executor& b) BOOST_ASIO_NOEXCEPT
  {
    return a.p_ == b.p_;
  }

  friend bool operator!=(const promise_executor& a,
      const promise_executor& b) BOOST_ASIO_NOEXCEPT
  {
    return a.p_ != b.p_;
  }

private:
  shared_ptr<std::promise<T> > p_;
};

// The base class for all completion handlers that create promises.
template <typename T>
class promise_creator
{
public:
  typedef promise_executor<T> executor_type;

  executor_type get_executor() const BOOST_ASIO_NOEXCEPT
  {
    return executor_type(p_);
  }

  typedef std::future<T> future_type;

  future_type get_future()
  {
    return p_->get_future();
  }

protected:
  template <typename Allocator>
  void create_promise(const Allocator& a)
  {
    BOOST_ASIO_REBIND_ALLOC(Allocator, char) b(a);
    p_ = std::allocate_shared<std::promise<T>>(b, std::allocator_arg, b);
  }

  shared_ptr<std::promise<T> > p_;
};

// For completion signature void().
class promise_handler_0
  : public promise_creator<void>
{
public:
  void operator()()
  {
    this->p_->set_value();
  }
};

// For completion signature void(error_code).
class promise_handler_ec_0
  : public promise_creator<void>
{
public:
  void operator()(const boost::system::error_code& ec)
  {
    if (ec)
    {
      this->p_->set_exception(
          std::make_exception_ptr(
            boost::system::system_error(ec)));
    }
    else
    {
      this->p_->set_value();
    }
  }
};

// For completion signature void(exception_ptr).
class promise_handler_ex_0
  : public promise_creator<void>
{
public:
  void operator()(const std::exception_ptr& ex)
  {
    if (ex)
    {
      this->p_->set_exception(ex);
    }
    else
    {
      this->p_->set_value();
    }
  }
};

// For completion signature void(T).
template <typename T>
class promise_handler_1
  : public promise_creator<T>
{
public:
  template <typename Arg>
  void operator()(BOOST_ASIO_MOVE_ARG(Arg) arg)
  {
    this->p_->set_value(BOOST_ASIO_MOVE_CAST(Arg)(arg));
  }
};

// For completion signature void(error_code, T).
template <typename T>
class promise_handler_ec_1
  : public promise_creator<T>
{
public:
  template <typename Arg>
  void operator()(const boost::system::error_code& ec,
      BOOST_ASIO_MOVE_ARG(Arg) arg)
  {
    if (ec)
    {
      this->p_->set_exception(
          std::make_exception_ptr(
            boost::system::system_error(ec)));
    }
    else
      this->p_->set_value(BOOST_ASIO_MOVE_CAST(Arg)(arg));
  }
};

// For completion signature void(exception_ptr, T).
template <typename T>
class promise_handler_ex_1
  : public promise_creator<T>
{
public:
  template <typename Arg>
  void operator()(const std::exception_ptr& ex,
      BOOST_ASIO_MOVE_ARG(Arg) arg)
  {
    if (ex)
      this->p_->set_exception(ex);
    else
      this->p_->set_value(BOOST_ASIO_MOVE_CAST(Arg)(arg));
  }
};

// For completion signature void(T1, ..., Tn);
template <typename T>
class promise_handler_n
  : public promise_creator<T>
{
public:
#if defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

  template <typename... Args>
  void operator()(BOOST_ASIO_MOVE_ARG(Args)... args)
  {
    this->p_->set_value(
        std::forward_as_tuple(
          BOOST_ASIO_MOVE_CAST(Args)(args)...));
  }

#else // defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

#define BOOST_ASIO_PRIVATE_CALL_OP_DEF(n) \
  template <BOOST_ASIO_VARIADIC_TPARAMS(n)> \
  void operator()(BOOST_ASIO_VARIADIC_MOVE_PARAMS(n)) \
  {\
    this->p_->set_value( \
        std::forward_as_tuple( \
          BOOST_ASIO_VARIADIC_MOVE_ARGS(n))); \
  } \
  /**/
  BOOST_ASIO_VARIADIC_GENERATE(BOOST_ASIO_PRIVATE_CALL_OP_DEF)
#undef BOOST_ASIO_PRIVATE_CALL_OP_DEF

#endif // defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)
};

// For completion signature void(error_code, T1, ..., Tn);
template <typename T>
class promise_handler_ec_n
  : public promise_creator<T>
{
public:
#if defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

  template <typename... Args>
  void operator()(const boost::system::error_code& ec,
      BOOST_ASIO_MOVE_ARG(Args)... args)
  {
    if (ec)
    {
      this->p_->set_exception(
          std::make_exception_ptr(
            boost::system::system_error(ec)));
    }
    else
    {
      this->p_->set_value(
          std::forward_as_tuple(
            BOOST_ASIO_MOVE_CAST(Args)(args)...));
    }
  }

#else // defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

#define BOOST_ASIO_PRIVATE_CALL_OP_DEF(n) \
  template <BOOST_ASIO_VARIADIC_TPARAMS(n)> \
  void operator()(const boost::system::error_code& ec, \
      BOOST_ASIO_VARIADIC_MOVE_PARAMS(n)) \
  {\
    if (ec) \
    { \
      this->p_->set_exception( \
          std::make_exception_ptr( \
            boost::system::system_error(ec))); \
    } \
    else \
    { \
      this->p_->set_value( \
          std::forward_as_tuple( \
            BOOST_ASIO_VARIADIC_MOVE_ARGS(n))); \
    } \
  } \
  /**/
  BOOST_ASIO_VARIADIC_GENERATE(BOOST_ASIO_PRIVATE_CALL_OP_DEF)
#undef BOOST_ASIO_PRIVATE_CALL_OP_DEF

#endif // defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)
};

// For completion signature void(exception_ptr, T1, ..., Tn);
template <typename T>
class promise_handler_ex_n
  : public promise_creator<T>
{
public:
#if defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

  template <typename... Args>
  void operator()(const std::exception_ptr& ex,
      BOOST_ASIO_MOVE_ARG(Args)... args)
  {
    if (ex)
      this->p_->set_exception(ex);
    else
    {
      this->p_->set_value(
          std::forward_as_tuple(
            BOOST_ASIO_MOVE_CAST(Args)(args)...));
    }
  }

#else // defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

#define BOOST_ASIO_PRIVATE_CALL_OP_DEF(n) \
  template <BOOST_ASIO_VARIADIC_TPARAMS(n)> \
  void operator()(const std::exception_ptr& ex, \
      BOOST_ASIO_VARIADIC_MOVE_PARAMS(n)) \
  {\
    if (ex) \
      this->p_->set_exception(ex); \
    else \
    { \
      this->p_->set_value( \
          std::forward_as_tuple( \
            BOOST_ASIO_VARIADIC_MOVE_ARGS(n))); \
    } \
  } \
  /**/
  BOOST_ASIO_VARIADIC_GENERATE(BOOST_ASIO_PRIVATE_CALL_OP_DEF)
#undef BOOST_ASIO_PRIVATE_CALL_OP_DEF

#endif // defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)
};

// Helper template to choose the appropriate concrete promise handler
// implementation based on the supplied completion signature.
template <typename> class promise_handler_selector;

template <>
class promise_handler_selector<void()>
  : public promise_handler_0 {};

template <>
class promise_handler_selector<void(boost::system::error_code)>
  : public promise_handler_ec_0 {};

template <>
class promise_handler_selector<void(std::exception_ptr)>
  : public promise_handler_ex_0 {};

template <typename Arg>
class promise_handler_selector<void(Arg)>
  : public promise_handler_1<Arg> {};

template <typename Arg>
class promise_handler_selector<void(boost::system::error_code, Arg)>
  : public promise_handler_ec_1<Arg> {};

template <typename Arg>
class promise_handler_selector<void(std::exception_ptr, Arg)>
  : public promise_handler_ex_1<Arg> {};

#if defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

template <typename... Arg>
class promise_handler_selector<void(Arg...)>
  : public promise_handler_n<std::tuple<Arg...> > {};

template <typename... Arg>
class promise_handler_selector<void(boost::system::error_code, Arg...)>
  : public promise_handler_ec_n<std::tuple<Arg...> > {};

template <typename... Arg>
class promise_handler_selector<void(std::exception_ptr, Arg...)>
  : public promise_handler_ex_n<std::tuple<Arg...> > {};

#else // defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

#define BOOST_ASIO_PRIVATE_PROMISE_SELECTOR_DEF(n) \
  template <typename Arg, BOOST_ASIO_VARIADIC_TPARAMS(n)> \
  class promise_handler_selector< \
    void(Arg, BOOST_ASIO_VARIADIC_TARGS(n))> \
      : public promise_handler_n< \
        std::tuple<Arg, BOOST_ASIO_VARIADIC_TARGS(n)> > {}; \
  \
  template <typename Arg, BOOST_ASIO_VARIADIC_TPARAMS(n)> \
  class promise_handler_selector< \
    void(boost::system::error_code, Arg, BOOST_ASIO_VARIADIC_TARGS(n))> \
      : public promise_handler_ec_n< \
        std::tuple<Arg, BOOST_ASIO_VARIADIC_TARGS(n)> > {}; \
  \
  template <typename Arg, BOOST_ASIO_VARIADIC_TPARAMS(n)> \
  class promise_handler_selector< \
    void(std::exception_ptr, Arg, BOOST_ASIO_VARIADIC_TARGS(n))> \
      : public promise_handler_ex_n< \
        std::tuple<Arg, BOOST_ASIO_VARIADIC_TARGS(n)> > {}; \
  /**/
  BOOST_ASIO_VARIADIC_GENERATE(BOOST_ASIO_PRIVATE_PROMISE_SELECTOR_DEF)
#undef BOOST_ASIO_PRIVATE_PROMISE_SELECTOR_DEF

#endif // defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

// Completion handlers produced from the use_future completion token, when not
// using use_future::operator().
template <typename Signature, typename Allocator>
class promise_handler
  : public promise_handler_selector<Signature>
{
public:
  typedef Allocator allocator_type;
  typedef void result_type;

  promise_handler(use_future_t<Allocator> u)
    : allocator_(u.get_allocator())
  {
    this->create_promise(allocator_);
  }

  allocator_type get_allocator() const BOOST_ASIO_NOEXCEPT
  {
    return allocator_;
  }

private:
  Allocator allocator_;
};

template <typename Function, typename Signature, typename Allocator>
inline void asio_handler_invoke(Function& f,
    promise_handler<Signature, Allocator>* h)
{
  typename promise_handler<Signature, Allocator>::executor_type
    ex(h->get_executor());
  ex.dispatch(BOOST_ASIO_MOVE_CAST(Function)(f), std::allocator<void>());
}

template <typename Function, typename Signature, typename Allocator>
inline void asio_handler_invoke(const Function& f,
    promise_handler<Signature, Allocator>* h)
{
  typename promise_handler<Signature, Allocator>::executor_type
    ex(h->get_executor());
  ex.dispatch(f, std::allocator<void>());
}

// Helper base class for async_result specialisation.
template <typename Signature, typename Allocator>
class promise_async_result
{
public:
  typedef promise_handler<Signature, Allocator> completion_handler_type;
  typedef typename completion_handler_type::future_type return_type;

  explicit promise_async_result(completion_handler_type& h)
    : future_(h.get_future())
  {
  }

  return_type get()
  {
    return BOOST_ASIO_MOVE_CAST(return_type)(future_);
  }

private:
  return_type future_;
};

// Return value from use_future::operator().
template <typename Function, typename Allocator>
class packaged_token
{
public:
  packaged_token(Function f, const Allocator& a)
    : function_(BOOST_ASIO_MOVE_CAST(Function)(f)),
      allocator_(a)
  {
  }

//private:
  Function function_;
  Allocator allocator_;
};

// Completion handlers produced from the use_future completion token, when
// using use_future::operator().
template <typename Function, typename Allocator, typename Result>
class packaged_handler
  : public promise_creator<Result>
{
public:
  typedef Allocator allocator_type;
  typedef void result_type;

  packaged_handler(packaged_token<Function, Allocator> t)
    : function_(BOOST_ASIO_MOVE_CAST(Function)(t.function_)),
      allocator_(t.allocator_)
  {
    this->create_promise(allocator_);
  }

  allocator_type get_allocator() const BOOST_ASIO_NOEXCEPT
  {
    return allocator_;
  }

#if defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

  template <typename... Args>
  void operator()(BOOST_ASIO_MOVE_ARG(Args)... args)
  {
    (promise_invoke_and_set)(*this->p_,
        function_, BOOST_ASIO_MOVE_CAST(Args)(args)...);
  }

#else // defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

  void operator()()
  {
    (promise_invoke_and_set)(*this->p_, function_);
  }

#define BOOST_ASIO_PRIVATE_CALL_OP_DEF(n) \
  template <BOOST_ASIO_VARIADIC_TPARAMS(n)> \
  void operator()(BOOST_ASIO_VARIADIC_MOVE_PARAMS(n)) \
  {\
    (promise_invoke_and_set)(*this->p_, \
        function_, BOOST_ASIO_VARIADIC_MOVE_ARGS(n)); \
  } \
  /**/
  BOOST_ASIO_VARIADIC_GENERATE(BOOST_ASIO_PRIVATE_CALL_OP_DEF)
#undef BOOST_ASIO_PRIVATE_CALL_OP_DEF

#endif // defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

private:
  Function function_;
  Allocator allocator_;
};

template <typename Function,
    typename Function1, typename Allocator, typename Result>
inline void asio_handler_invoke(Function& f,
    packaged_handler<Function1, Allocator, Result>* h)
{
  typename packaged_handler<Function1, Allocator, Result>::executor_type
    ex(h->get_executor());
  ex.dispatch(BOOST_ASIO_MOVE_CAST(Function)(f), std::allocator<void>());
}

template <typename Function,
    typename Function1, typename Allocator, typename Result>
inline void asio_handler_invoke(const Function& f,
    packaged_handler<Function1, Allocator, Result>* h)
{
  typename packaged_handler<Function1, Allocator, Result>::executor_type
    ex(h->get_executor());
  ex.dispatch(f, std::allocator<void>());
}

// Helper base class for async_result specialisation.
template <typename Function, typename Allocator, typename Result>
class packaged_async_result
{
public:
  typedef packaged_handler<Function, Allocator, Result> completion_handler_type;
  typedef typename completion_handler_type::future_type return_type;

  explicit packaged_async_result(completion_handler_type& h)
    : future_(h.get_future())
  {
  }

  return_type get()
  {
    return BOOST_ASIO_MOVE_CAST(return_type)(future_);
  }

private:
  return_type future_;
};

} // namespace detail

template <typename Allocator> template <typename Function>
inline detail::packaged_token<typename decay<Function>::type, Allocator>
use_future_t<Allocator>::operator()(BOOST_ASIO_MOVE_ARG(Function) f) const
{
  return detail::packaged_token<typename decay<Function>::type, Allocator>(
      BOOST_ASIO_MOVE_CAST(Function)(f), allocator_);
}

#if !defined(GENERATING_DOCUMENTATION)

#if defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

template <typename Allocator, typename Result, typename... Args>
class async_result<use_future_t<Allocator>, Result(Args...)>
  : public detail::promise_async_result<
      void(typename decay<Args>::type...), Allocator>
{
public:
  explicit async_result(
    typename detail::promise_async_result<void(typename decay<Args>::type...),
      Allocator>::completion_handler_type& h)
    : detail::promise_async_result<
        void(typename decay<Args>::type...), Allocator>(h)
  {
  }
};

template <typename Function, typename Allocator,
    typename Result, typename... Args>
class async_result<detail::packaged_token<Function, Allocator>, Result(Args...)>
  : public detail::packaged_async_result<Function, Allocator,
      typename result_of<Function(Args...)>::type>
{
public:
  explicit async_result(
    typename detail::packaged_async_result<Function, Allocator,
      typename result_of<Function(Args...)>::type>::completion_handler_type& h)
    : detail::packaged_async_result<Function, Allocator,
        typename result_of<Function(Args...)>::type>(h)
  {
  }
};

#else // defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

template <typename Allocator, typename Result>
class async_result<use_future_t<Allocator>, Result()>
  : public detail::promise_async_result<void(), Allocator>
{
public:
  explicit async_result(
    typename detail::promise_async_result<
      void(), Allocator>::completion_handler_type& h)
    : detail::promise_async_result<void(), Allocator>(h)
  {
  }
};

template <typename Function, typename Allocator, typename Result>
class async_result<detail::packaged_token<Function, Allocator>, Result()>
  : public detail::packaged_async_result<Function, Allocator,
      typename result_of<Function()>::type>
{
public:
  explicit async_result(
    typename detail::packaged_async_result<Function, Allocator,
      typename result_of<Function()>::type>::completion_handler_type& h)
    : detail::packaged_async_result<Function, Allocator,
        typename result_of<Function()>::type>(h)
  {
  }
};

#define BOOST_ASIO_PRIVATE_ASYNC_RESULT_DEF(n) \
  template <typename Allocator, \
      typename Result, BOOST_ASIO_VARIADIC_TPARAMS(n)> \
  class async_result<use_future_t<Allocator>, \
      Result(BOOST_ASIO_VARIADIC_TARGS(n))> \
    : public detail::promise_async_result< \
        void(BOOST_ASIO_VARIADIC_DECAY(n)), Allocator> \
  { \
  public: \
    explicit async_result( \
      typename detail::promise_async_result< \
        void(BOOST_ASIO_VARIADIC_DECAY(n)), \
        Allocator>::completion_handler_type& h) \
      : detail::promise_async_result< \
          void(BOOST_ASIO_VARIADIC_DECAY(n)), Allocator>(h) \
    { \
    } \
  }; \
  \
  template <typename Function, typename Allocator, \
      typename Result, BOOST_ASIO_VARIADIC_TPARAMS(n)> \
  class async_result<detail::packaged_token<Function, Allocator>, \
      Result(BOOST_ASIO_VARIADIC_TARGS(n))> \
    : public detail::packaged_async_result<Function, Allocator, \
        typename result_of<Function(BOOST_ASIO_VARIADIC_TARGS(n))>::type> \
  { \
  public: \
    explicit async_result( \
      typename detail::packaged_async_result<Function, Allocator, \
        typename result_of<Function(BOOST_ASIO_VARIADIC_TARGS(n))>::type \
        >::completion_handler_type& h) \
      : detail::packaged_async_result<Function, Allocator, \
          typename result_of<Function(BOOST_ASIO_VARIADIC_TARGS(n))>::type>(h) \
    { \
    } \
  }; \
  /**/
  BOOST_ASIO_VARIADIC_GENERATE(BOOST_ASIO_PRIVATE_ASYNC_RESULT_DEF)
#undef BOOST_ASIO_PRIVATE_ASYNC_RESULT_DEF

#endif // defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

#if !defined(BOOST_ASIO_NO_DEPRECATED)

template <typename Allocator, typename Signature>
struct handler_type<use_future_t<Allocator>, Signature>
{
  typedef typename async_result<use_future_t<Allocator>,
    Signature>::completion_handler_type type;
};

template <typename Signature, typename Allocator>
class async_result<detail::promise_handler<Signature, Allocator> >
  : public detail::promise_async_result<Signature, Allocator>
{
public:
  typedef typename detail::promise_async_result<
    Signature, Allocator>::return_type type;

  explicit async_result(
    typename detail::promise_async_result<
      Signature, Allocator>::completion_handler_type& h)
    : detail::promise_async_result<Signature, Allocator>(h)
  {
  }
};

template <typename Function, typename Allocator, typename Signature>
struct handler_type<detail::packaged_token<Function, Allocator>, Signature>
{
  typedef typename async_result<detail::packaged_token<Function, Allocator>,
    Signature>::completion_handler_type type;
};

template <typename Function, typename Allocator, typename Result>
class async_result<detail::packaged_handler<Function, Allocator, Result> >
  : public detail::packaged_async_result<Function, Allocator, Result>
{
public:
  typedef typename detail::packaged_async_result<
    Function, Allocator, Result>::return_type type;

  explicit async_result(
    typename detail::packaged_async_result<
      Function, Allocator, Result>::completion_handler_type& h)
    : detail::packaged_async_result<Function, Allocator, Result>(h)
  {
  }
};

#endif // !defined(BOOST_ASIO_NO_DEPRECATED)

#endif // !defined(GENERATING_DOCUMENTATION)

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_IMPL_USE_FUTURE_HPP
