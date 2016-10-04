//
// wrap.hpp
// ~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_WRAP_HPP
#define ASIO_WRAP_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include "asio/detail/type_traits.hpp"
#include "asio/detail/variadic_templates.hpp"
#include "asio/associated_allocator.hpp"
#include "asio/async_result.hpp"
#include "asio/handler_type.hpp"
#include "asio/uses_executor.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace detail {

template <typename T>
struct executor_wrapper_check
{
  typedef void type;
};

// Helper to automatically define nested typedef result_type.

template <typename T, typename = void>
struct executor_wrapper_result_type
{
protected:
  typedef void result_type_or_void;
};

template <typename T>
struct executor_wrapper_result_type<T,
  typename executor_wrapper_check<typename T::result_type>::type>
{
  typedef typename T::result_type result_type;
protected:
  typedef result_type result_type_or_void;
};

template <typename R>
struct executor_wrapper_result_type<R(*)()>
{
  typedef R result_type;
protected:
  typedef result_type result_type_or_void;
};

template <typename R>
struct executor_wrapper_result_type<R(&)()>
{
  typedef R result_type;
protected:
  typedef result_type result_type_or_void;
};

template <typename R, typename A1>
struct executor_wrapper_result_type<R(*)(A1)>
{
  typedef R result_type;
protected:
  typedef result_type result_type_or_void;
};

template <typename R, typename A1>
struct executor_wrapper_result_type<R(&)(A1)>
{
  typedef R result_type;
protected:
  typedef result_type result_type_or_void;
};

template <typename R, typename A1, typename A2>
struct executor_wrapper_result_type<R(*)(A1, A2)>
{
  typedef R result_type;
protected:
  typedef result_type result_type_or_void;
};

template <typename R, typename A1, typename A2>
struct executor_wrapper_result_type<R(&)(A1, A2)>
{
  typedef R result_type;
protected:
  typedef result_type result_type_or_void;
};

// Helper to automatically define nested typedef argument_type.

template <typename T, typename = void>
struct executor_wrapper_argument_type {};

template <typename T>
struct executor_wrapper_argument_type<T,
  typename executor_wrapper_check<typename T::argument_type>::type>
{
  typedef typename T::argument_type argument_type;
};

template <typename R, typename A1>
struct executor_wrapper_argument_type<R(*)(A1)>
{
  typedef A1 argument_type;
};

template <typename R, typename A1>
struct executor_wrapper_argument_type<R(&)(A1)>
{
  typedef A1 argument_type;
};

// Helper to automatically define nested typedefs first_argument_type and
// second_argument_type.

template <typename T, typename = void>
struct executor_wrapper_argument_types {};

template <typename T>
struct executor_wrapper_argument_types<T,
  typename executor_wrapper_check<typename T::first_argument_type>::type>
{
  typedef typename T::first_argument_type first_argument_type;
  typedef typename T::second_argument_type second_argument_type;
};

template <typename R, typename A1, typename A2>
struct executor_wrapper_argument_type<R(*)(A1, A2)>
{
  typedef A1 first_argument_type;
  typedef A2 second_argument_type;
};

template <typename R, typename A1, typename A2>
struct executor_wrapper_argument_type<R(&)(A1, A2)>
{
  typedef A1 first_argument_type;
  typedef A2 second_argument_type;
};

// Helper to:
// - Apply the empty base optimisation to the executor.
// - Perform uses_executor construction of the wrapped type, if required.

template <typename T, typename Executor, bool UsesExecutor>
class executor_wrapper_base;

template <typename T, typename Executor>
class executor_wrapper_base<T, Executor, true>
  : protected Executor
{
protected:
  template <typename E, typename U>
  executor_wrapper_base(ASIO_MOVE_ARG(E) e, ASIO_MOVE_ARG(U) u)
    : Executor(ASIO_MOVE_CAST(E)(e)),
      wrapped_(executor_arg_t(), static_cast<const Executor&>(*this),
          ASIO_MOVE_CAST(U)(u))
  {
  }

  T wrapped_;
};

template <typename T, typename Executor>
class executor_wrapper_base<T, Executor, false>
  : protected Executor
{
protected:
  template <typename E, typename U>
  executor_wrapper_base(ASIO_MOVE_ARG(E) e, ASIO_MOVE_ARG(U) u)
    : Executor(ASIO_MOVE_CAST(E)(e)),
      wrapped_(ASIO_MOVE_CAST(U)(u))
  {
  }

  T wrapped_;
};

// Helper to enable SFINAE on zero-argument operator() below.

template <typename T, typename = void>
struct executor_wrapper_result_of0
{
  typedef void type;
};

template <typename T>
struct executor_wrapper_result_of0<T,
  typename executor_wrapper_check<typename result_of<T()>::type>::type>
{
  typedef typename result_of<T()>::type type;
};

} // namespace detail

/// A call wrapper type to associate an object of type @c T with an executor of
/// type @c Executor.
template <typename T, typename Executor>
class executor_wrapper
#if !defined(GENERATING_DOCUMENTATION)
  : public detail::executor_wrapper_result_type<T>,
    public detail::executor_wrapper_argument_type<T>,
    public detail::executor_wrapper_argument_types<T>,
    private detail::executor_wrapper_base<
      T, Executor, uses_executor<T, Executor>::value>
#endif // !defined(GENERATING_DOCUMENTATION)
{
public:
  /// The type of the wrapped object.
  typedef T wrapped_type;

  /// The type of the associated executor.
  typedef Executor executor_type;

#if defined(GENERATING_DOCUMENTATION)
  /// The return type if a function.
  /**
   * The type of @c result_type is based on the type @c T of the wrapper's
   * target object:
   *
   * @li if @c T is a pointer to function type, @c result_type is a synonym for
   * the return type of @c T;
   *
   * @li if @c T is a class type with a member type @c result_type, then @c
   * result_type is a synonym for @c T::result_type;
   *
   * @li otherwise @c result_type is not defined.
   */
  typedef see_below result_type;

  /// The type of the function's argument.
  /**
   * The type of @c argument_type is based on the type @c T of the wrapper's
   * target object:
   *
   * @li if @c T is a pointer to a function type accepting a single argument,
   * @c argument_type is a synonym for the return type of @c T;
   *
   * @li if @c T is a class type with a member type @c argument_type, then @c
   * argument_type is a synonym for @c T::argument_type;
   *
   * @li otherwise @c argument_type is not defined.
   */
  typedef see_below argument_type;

  /// The type of the function's first argument.
  /**
   * The type of @c first_argument_type is based on the type @c T of the
   * wrapper's target object:
   *
   * @li if @c T is a pointer to a function type accepting two arguments, @c
   * first_argument_type is a synonym for the return type of @c T;
   *
   * @li if @c T is a class type with a member type @c first_argument_type,
   * then @c first_argument_type is a synonym for @c T::first_argument_type;
   *
   * @li otherwise @c first_argument_type is not defined.
   */
  typedef see_below first_argument_type;

  /// The type of the function's second argument.
  /**
   * The type of @c second_argument_type is based on the type @c T of the
   * wrapper's target object:
   *
   * @li if @c T is a pointer to a function type accepting two arguments, @c
   * second_argument_type is a synonym for the return type of @c T;
   *
   * @li if @c T is a class type with a member type @c first_argument_type,
   * then @c second_argument_type is a synonym for @c T::second_argument_type;
   *
   * @li otherwise @c second_argument_type is not defined.
   */
  typedef see_below second_argument_type;
#endif // defined(GENERATING_DOCUMENTATION)

  /// Construct an executor wrapper for the specified object.
  /**
   * This constructor is only valid if the type @c T is constructible from type
   * @c U.
   */
  template <typename U>
  executor_wrapper(executor_arg_t, const executor_type& e,
      ASIO_MOVE_ARG(U) u)
    : base_type(e, ASIO_MOVE_CAST(U)(u))
  {
  }

  /// Copy constructor.
  executor_wrapper(const executor_wrapper& other)
    : base_type(other.get_executor(), other.unwrap())
  {
  }

  /// Construct a copy, but specify a different executor.
  executor_wrapper(executor_arg_t, const executor_type& e,
      const executor_wrapper& other)
    : base_type(e, other.unwrap())
  {
  }

  /// Construct a copy of a different executor wrapper type.
  /**
   * This constructor is only valid if the @c Executor type is constructible
   * from type @c OtherExecutor, and the type @c T is constructible from type
   * @c U.
   */
  template <typename U, typename OtherExecutor>
  executor_wrapper(const executor_wrapper<U, OtherExecutor>& other)
    : base_type(other.get_executor(), other.unwrap())
  {
  }

  /// Construct a copy of a different executor wrapper type, but specify a
  /// different executor.
  /**
   * This constructor is only valid if the type @c T is constructible from type
   * @c U.
   */
  template <typename U, typename OtherExecutor>
  executor_wrapper(executor_arg_t, const executor_type& e,
      const executor_wrapper<U, OtherExecutor>& other)
    : base_type(e, other.unwrap())
  {
  }

#if defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)

  /// Move constructor.
  executor_wrapper(executor_wrapper&& other)
    : base_type(ASIO_MOVE_CAST(executor_type)(other.get_executor()),
        ASIO_MOVE_CAST(T)(other.unwrap()))
  {
  }

  /// Move construct the wrapped object, but specify a different executor.
  executor_wrapper(executor_arg_t, const executor_type& e,
      executor_wrapper&& other)
    : base_type(e, ASIO_MOVE_CAST(T)(other.unwrap()))
  {
  }

  /// Move construct from a different executor wrapper type.
  template <typename U, typename OtherExecutor>
  executor_wrapper(executor_wrapper<U, OtherExecutor>&& other)
    : base_type(ASIO_MOVE_CAST(OtherExecutor)(other.get_executor()),
        ASIO_MOVE_CAST(U)(other.unwrap()))
  {
  }

  /// Move construct from a different executor wrapper type, but specify a
  /// different executor.
  template <typename U, typename OtherExecutor>
  executor_wrapper(executor_arg_t, const executor_type& e,
      executor_wrapper<U, OtherExecutor>&& other)
    : base_type(e, ASIO_MOVE_CAST(U)(other.unwrap()))
  {
  }

#endif // defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)

  /// Destructor.
  ~executor_wrapper()
  {
  }

  /// Obtain a reference to the wrapped object.
  wrapped_type& unwrap() ASIO_NOEXCEPT
  {
    return this->wrapped_;
  }

  /// Obtain a reference to the wrapped object.
  const wrapped_type& unwrap() const ASIO_NOEXCEPT
  {
    return this->wrapped_;
  }

  /// Obtain the associated executor.
  executor_type get_executor() const ASIO_NOEXCEPT
  {
    return static_cast<const Executor&>(*this);
  }

#if defined(GENERATING_DOCUMENTATION)

  template <typename... Args> auto operator()(Args&& ...);
  template <typename... Args> auto operator()(Args&& ...) const;

#elif defined(ASIO_HAS_VARIADIC_TEMPLATES)

  /// Forwarding function call operator.
  template <typename... Args>
  typename result_of<T(Args...)>::type operator()(
      ASIO_MOVE_ARG(Args)... args)
  {
    return this->wrapped_(ASIO_MOVE_CAST(Args)(args)...);
  }

  /// Forwarding function call operator.
  template <typename... Args>
  typename result_of<T(Args...)>::type operator()(
      ASIO_MOVE_ARG(Args)... args) const
  {
    return this->wrapped_(ASIO_MOVE_CAST(Args)(args)...);
  }

#elif defined(ASIO_HAS_STD_TYPE_TRAITS) && !defined(_MSC_VER)

  typename detail::executor_wrapper_result_of0<T>::type operator()()
  {
    return this->wrapped_();
  }

  typename detail::executor_wrapper_result_of0<T>::type operator()() const
  {
    return this->wrapped_();
  }

#define ASIO_PRIVATE_WRAP_CALL_DEF(n) \
  template <ASIO_VARIADIC_TPARAMS(n)> \
  typename result_of<T(ASIO_VARIADIC_TARGS(n))>::type operator()( \
      ASIO_VARIADIC_MOVE_PARAMS(n)) \
  { \
    return this->wrapped_(ASIO_VARIADIC_MOVE_ARGS(n)); \
  } \
  \
  template <ASIO_VARIADIC_TPARAMS(n)> \
  typename result_of<T(ASIO_VARIADIC_TARGS(n))>::type operator()( \
      ASIO_VARIADIC_MOVE_PARAMS(n)) const \
  { \
    return this->wrapped_(ASIO_VARIADIC_MOVE_ARGS(n)); \
  } \
  /**/
  ASIO_VARIADIC_GENERATE(ASIO_PRIVATE_WRAP_CALL_DEF)
#undef ASIO_PRIVATE_WRAP_CALL_DEF

#else // defined(ASIO_HAS_STD_TYPE_TRAITS) && !defined(_MSC_VER)

  typedef typename detail::executor_wrapper_result_type<T>::result_type_or_void
    result_type_or_void;

  result_type_or_void operator()()
  {
    return this->wrapped_();
  }

  result_type_or_void operator()() const
  {
    return this->wrapped_();
  }

#define ASIO_PRIVATE_WRAP_CALL_DEF(n) \
  template <ASIO_VARIADIC_TPARAMS(n)> \
  result_type_or_void operator()( \
      ASIO_VARIADIC_MOVE_PARAMS(n)) \
  { \
    return this->wrapped_(ASIO_VARIADIC_MOVE_ARGS(n)); \
  } \
  \
  template <ASIO_VARIADIC_TPARAMS(n)> \
  result_type_or_void operator()( \
      ASIO_VARIADIC_MOVE_PARAMS(n)) const \
  { \
    return this->wrapped_(ASIO_VARIADIC_MOVE_ARGS(n)); \
  } \
  /**/
  ASIO_VARIADIC_GENERATE(ASIO_PRIVATE_WRAP_CALL_DEF)
#undef ASIO_PRIVATE_WRAP_CALL_DEF

#endif // defined(ASIO_HAS_STD_TYPE_TRAITS) && !defined(_MSC_VER)

private:
  typedef detail::executor_wrapper_base<T, Executor,
    uses_executor<T, Executor>::value> base_type;
};

/// Associate an object of type @c T with an executor of type @c Executor.
template <typename Executor, typename T>
inline executor_wrapper<typename decay<T>::type, Executor>
wrap(const Executor& ex, ASIO_MOVE_ARG(T) t,
    typename enable_if<is_executor<Executor>::value>::type* = 0)
{
  return executor_wrapper<typename decay<T>::type, Executor>(
      executor_arg_t(), ex, ASIO_MOVE_CAST(T)(t));
}

/// Associate an object of type @c T with an execution context's executor.
template <typename ExecutionContext, typename T>
inline executor_wrapper<typename decay<T>::type,
  typename ExecutionContext::executor_type>
wrap(ExecutionContext& ctx, ASIO_MOVE_ARG(T) t,
    typename enable_if<is_convertible<
      ExecutionContext&, execution_context&>::value>::type* = 0)
{
  return executor_wrapper<typename decay<T>::type,
    typename ExecutionContext::executor_type>(
      executor_arg_t(), ctx.get_executor(), ASIO_MOVE_CAST(T)(t));
}

#if !defined(GENERATING_DOCUMENTATION)

template <typename T, typename Executor>
struct uses_executor<executor_wrapper<T, Executor>, Executor>
  : true_type {};

template <typename T, typename Executor, typename Signature>
struct handler_type<executor_wrapper<T, Executor>, Signature>
{
  typedef executor_wrapper<
    typename handler_type<T, Signature>::type, Executor> type;
};

template <typename T, typename Executor>
class async_result<executor_wrapper<T, Executor> >
{
public:
  typedef typename async_result<T>::type type;

  explicit async_result(executor_wrapper<T, Executor>& w)
    : wrapped_(w.unwrap())
  {
  }

  type get()
  {
    return wrapped_.get();
  }

private:
  async_result<T> wrapped_;
};

template <typename T, typename Executor, typename Allocator>
struct associated_allocator<executor_wrapper<T, Executor>, Allocator>
{
  typedef typename associated_allocator<T, Allocator>::type type;

  static type get(const executor_wrapper<T, Executor>& w,
      const Allocator& a = Allocator()) ASIO_NOEXCEPT
  {
    return associated_allocator<T, Allocator>::get(w.unwrap(), a);
  }
};

template <typename T, typename Executor, typename Executor1>
struct associated_executor<executor_wrapper<T, Executor>, Executor1>
{
  typedef Executor type;

  static type get(const executor_wrapper<T, Executor>& w,
      const Executor1& = Executor1()) ASIO_NOEXCEPT
  {
    return w.get_executor();
  }
};

#endif // !defined(GENERATING_DOCUMENTATION)

} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_WRAP_HPP
