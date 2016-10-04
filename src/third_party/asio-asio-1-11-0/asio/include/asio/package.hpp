//
// package.hpp
// ~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_USE_PACKAGE_HPP
#define ASIO_USE_PACKAGE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include <future>
#include <memory>
#include "asio/async_result.hpp"
#include "asio/detail/type_traits.hpp"
#include "asio/detail/variadic_templates.hpp"
#include "asio/handler_type.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {

/// Class to enable lazy construction of a packaged_task from a completion
/// token.
/**
 * The packaged_token class is used to adapt a function object as a packaged
 * task. When this adapter is passed as a completion token to an asynchronous
 * operation, the result of the function object is retuned via a std::future.
 *
 * Use the @ref package function rather than using this class directly.
 */
template <typename Function, typename Allocator = std::allocator<void> >
class packaged_token
{
public:
  /// The allocator type. The allocator is used when constructing the
  /// @c std::promise object for a given asynchronous operation.
  typedef Allocator allocator_type;

  /// Construct using specified allocator.
  explicit packaged_token(Function f)
    : func_(std::move(f))
  {
  }

  /// Construct using specified allocator.
  packaged_token(Function f, const Allocator& allocator)
    : func_(std::move(f)),
      allocator_(allocator)
  {
  }

  /// Obtain allocator.
  allocator_type get_allocator() const ASIO_NOEXCEPT
  {
    return allocator_;
  }

private:
  template <class, class> friend class packaged_handler;
  Function func_;
  Allocator allocator_;
};

/// A packaged_task with an associated allocator.
template <typename Signature, typename Allocator>
class packaged_handler : public std::packaged_task<Signature>
{
public:
  /// The allocator type. The allocator is used when constructing the
  /// @c std::promise object for a given asynchronous operation.
  typedef Allocator allocator_type;

  /// Construct from a packaged token.
  template <typename Function>
  packaged_handler(
      packaged_token<Function, Allocator>&& token)
#if defined(_MSC_VER)
    : std::packaged_task<Signature>(std::move(token.func_)),
#elif defined(ASIO_HAS_CLANG_LIBCXX)
    : std::packaged_task<Signature>(std::allocator_arg,
        typename std::allocator_traits<
          Allocator>::template rebind_alloc<char>(token.allocator_),
        std::move(token.func_)),
#else
    : std::packaged_task<Signature>(std::allocator_arg,
        token.allocator_, std::move(token.func_)),
#endif
      allocator_(token.allocator_)
  {
  }

  /// Move construct from another packaged handler.
  packaged_handler(packaged_handler&& other)
    : std::packaged_task<Signature>(
        static_cast<std::packaged_task<Signature>&&>(other)),
      allocator_(other.allocator_)
  {
  }

  /// Obtain allocator.
  allocator_type get_allocator() const ASIO_NOEXCEPT
  {
    return allocator_;
  }

private:
  Allocator allocator_;
};

/// Wrap a function object in a packaged task.
/**
 * The @c package function is used to adapt a function object as a packaged
 * task. When this adapter is passed as a completion token to an asynchronous
 * operation, the result of the function object is retuned via a std::future.
 *
 * @par Example
 *
 * @code std::future<std::size_t> fut =
 *   my_socket.async_read_some(buffer,
 *     package([](asio::error_code ec, std::size_t n)
 *       {
 *         return ec ? 0 : n;
 *       }));
 * ...
 * std::size_t n = fut.get(); @endcode
 */
template <typename Function>
inline packaged_token<typename decay<Function>::type, std::allocator<void> >
package(ASIO_MOVE_ARG(Function) function)
{
  return packaged_token<typename decay<Function>::type, std::allocator<void> >(
      ASIO_MOVE_CAST(Function)(function), std::allocator<void>());
}

/// Wrap a function object in a packaged task.
/**
 * The @c package function is used to adapt a function object as a packaged
 * task. When this adapter is passed as a completion token to an asynchronous
 * operation, the result of the function object is retuned via a std::future.
 *
 * @par Example
 *
 * @code std::future<std::size_t> fut =
 *   my_socket.async_read_some(buffer,
 *     package([](asio::error_code ec, std::size_t n)
 *       {
 *         return ec ? 0 : n;
 *       }));
 * ...
 * std::size_t n = fut.get(); @endcode
 */
template <typename Function, typename Allocator>
inline packaged_token<typename decay<Function>::type, Allocator> package(
    ASIO_MOVE_ARG(Function) function, const Allocator& a)
{
  return packaged_token<typename decay<Function>::type, Allocator>(
      ASIO_MOVE_CAST(Function)(function), a);
}

#if !defined(GENERATING_DOCUMENTATION)

#if defined(ASIO_HAS_VARIADIC_TEMPLATES)

template <typename Function, typename Allocator, typename R, typename... Args>
struct handler_type<packaged_token<Function, Allocator>, R(Args...)>
{
  typedef packaged_handler<
    typename result_of<Function(Args...)>::type(Args...),
      Allocator> type;
};

#else // defined(ASIO_HAS_VARIADIC_TEMPLATES)

template <typename Function, typename Allocator, typename R>
struct handler_type<packaged_token<Function, Allocator>, R()>
{
  typedef packaged_handler<
    typename result_of<Function()>::type(),
      Allocator> type;
};

#define ASIO_PRIVATE_HANDLER_TYPE_DEF(n) \
  template <typename Function, typename Allocator, \
    typename R, ASIO_VARIADIC_TPARAMS(n)> \
  struct handler_type< \
    packaged_token<Function, Allocator>, R(ASIO_VARIADIC_TARGS(n))> \
  { \
    typedef packaged_handler< \
      typename result_of< \
        Function(ASIO_VARIADIC_TARGS(n))>::type( \
          ASIO_VARIADIC_TARGS(n)), \
            Allocator> type; \
  }; \
  /**/
  ASIO_VARIADIC_GENERATE(ASIO_PRIVATE_HANDLER_TYPE_DEF)
#undef ASIO_PRIVATE_HANDLER_TYPE_DEF

#endif // defined(ASIO_HAS_VARIADIC_TEMPLATES)

#if defined(ASIO_HAS_VARIADIC_TEMPLATES)

template <typename R, typename... Args>
class async_result<std::packaged_task<R(Args...)> >
{
public:
  typedef std::future<R> type;

  explicit async_result(std::packaged_task<R(Args...)>& h)
    : future_(h.get_future())
  {
  }

  type get()
  {
    return std::move(future_);
  }

private:
  type future_;
};

#else // defined(ASIO_HAS_VARIADIC_TEMPLATES)

template <typename R>
class async_result<std::packaged_task<R()> >
{
public:
  typedef std::future<R> type;

  explicit async_result(std::packaged_task<R()>& h)
    : future_(h.get_future())
  {
  }

  type get()
  {
    return std::move(future_);
  }

private:
  type future_;
};

#define ASIO_PRIVATE_ASYNC_RESULT_DEF(n) \
  template <typename R, ASIO_VARIADIC_TPARAMS(n)> \
  class async_result<std::packaged_task<R(ASIO_VARIADIC_TARGS(n))> > \
  { \
  public: \
    typedef std::future<R> type; \
  \
    explicit async_result( \
        std::packaged_task<R(ASIO_VARIADIC_TARGS(n))>& h) \
      : future_(h.get_future()) \
    { \
    } \
  \
    type get() \
    { \
      return std::move(future_); \
    } \
  \
  private: \
    type future_; \
  }; \
  /**/
  ASIO_VARIADIC_GENERATE(ASIO_PRIVATE_ASYNC_RESULT_DEF)
#undef ASIO_PRIVATE_ASYNC_RESULT_DEF

#endif // defined(ASIO_HAS_VARIADIC_TEMPLATES)

template <typename Signature, typename Allocator>
class async_result<packaged_handler<Signature, Allocator>>
  : public async_result<std::packaged_task<Signature>>
{
public:
  explicit async_result(packaged_handler<Signature, Allocator>& h)
    : async_result<std::packaged_task<Signature>>(h) {}
};

#endif // !defined(GENERATING_DOCUMENTATION)

} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_USE_PACKAGE_HPP
