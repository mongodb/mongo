//
// executor_work_guard.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2022 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_EXECUTOR_WORK_GUARD_HPP
#define BOOST_ASIO_EXECUTOR_WORK_GUARD_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#include <boost/asio/associated_executor.hpp>
#include <boost/asio/detail/type_traits.hpp>
#include <boost/asio/execution.hpp>
#include <boost/asio/is_executor.hpp>

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

#if !defined(BOOST_ASIO_EXECUTOR_WORK_GUARD_DECL)
#define BOOST_ASIO_EXECUTOR_WORK_GUARD_DECL

template <typename Executor, typename = void, typename = void>
class executor_work_guard;

#endif // !defined(BOOST_ASIO_EXECUTOR_WORK_GUARD_DECL)

#if defined(GENERATING_DOCUMENTATION)

/// An object of type @c executor_work_guard controls ownership of outstanding
/// executor work within a scope.
template <typename Executor>
class executor_work_guard
{
public:
  /// The underlying executor type.
  typedef Executor executor_type;

  /// Constructs a @c executor_work_guard object for the specified executor.
  /**
   * Stores a copy of @c e and calls <tt>on_work_started()</tt> on it.
   */
  explicit executor_work_guard(const executor_type& e) BOOST_ASIO_NOEXCEPT;

  /// Copy constructor.
  executor_work_guard(const executor_work_guard& other) BOOST_ASIO_NOEXCEPT;

  /// Move constructor.
  executor_work_guard(executor_work_guard&& other) BOOST_ASIO_NOEXCEPT;

  /// Destructor.
  /**
   * Unless the object has already been reset, or is in a moved-from state,
   * calls <tt>on_work_finished()</tt> on the stored executor.
   */
  ~executor_work_guard();

  /// Obtain the associated executor.
  executor_type get_executor() const BOOST_ASIO_NOEXCEPT;

  /// Whether the executor_work_guard object owns some outstanding work.
  bool owns_work() const BOOST_ASIO_NOEXCEPT;

  /// Indicate that the work is no longer outstanding.
  /**
   * Unless the object has already been reset, or is in a moved-from state,
   * calls <tt>on_work_finished()</tt> on the stored executor.
   */
  void reset() BOOST_ASIO_NOEXCEPT;
};

#endif // defined(GENERATING_DOCUMENTATION)

#if !defined(GENERATING_DOCUMENTATION)

#if !defined(BOOST_ASIO_NO_TS_EXECUTORS)

template <typename Executor>
class executor_work_guard<Executor,
    typename enable_if<
      is_executor<Executor>::value
    >::type>
{
public:
  typedef Executor executor_type;

  explicit executor_work_guard(const executor_type& e) BOOST_ASIO_NOEXCEPT
    : executor_(e),
      owns_(true)
  {
    executor_.on_work_started();
  }

  executor_work_guard(const executor_work_guard& other) BOOST_ASIO_NOEXCEPT
    : executor_(other.executor_),
      owns_(other.owns_)
  {
    if (owns_)
      executor_.on_work_started();
  }

#if defined(BOOST_ASIO_HAS_MOVE)
  executor_work_guard(executor_work_guard&& other) BOOST_ASIO_NOEXCEPT
    : executor_(BOOST_ASIO_MOVE_CAST(Executor)(other.executor_)),
      owns_(other.owns_)
  {
    other.owns_ = false;
  }
#endif // defined(BOOST_ASIO_HAS_MOVE)

  ~executor_work_guard()
  {
    if (owns_)
      executor_.on_work_finished();
  }

  executor_type get_executor() const BOOST_ASIO_NOEXCEPT
  {
    return executor_;
  }

  bool owns_work() const BOOST_ASIO_NOEXCEPT
  {
    return owns_;
  }

  void reset() BOOST_ASIO_NOEXCEPT
  {
    if (owns_)
    {
      executor_.on_work_finished();
      owns_ = false;
    }
  }

private:
  // Disallow assignment.
  executor_work_guard& operator=(const executor_work_guard&);

  executor_type executor_;
  bool owns_;
};

#endif // !defined(BOOST_ASIO_NO_TS_EXECUTORS)

template <typename Executor>
class executor_work_guard<Executor,
    typename enable_if<
      !is_executor<Executor>::value
    >::type,
    typename enable_if<
      execution::is_executor<Executor>::value
    >::type>
{
public:
  typedef Executor executor_type;

  explicit executor_work_guard(const executor_type& e) BOOST_ASIO_NOEXCEPT
    : executor_(e),
      owns_(true)
  {
    new (&work_) work_type(boost::asio::prefer(executor_,
          execution::outstanding_work.tracked));
  }

  executor_work_guard(const executor_work_guard& other) BOOST_ASIO_NOEXCEPT
    : executor_(other.executor_),
      owns_(other.owns_)
  {
    if (owns_)
    {
      new (&work_) work_type(boost::asio::prefer(executor_,
            execution::outstanding_work.tracked));
    }
  }

#if defined(BOOST_ASIO_HAS_MOVE)
  executor_work_guard(executor_work_guard&& other) BOOST_ASIO_NOEXCEPT
    : executor_(BOOST_ASIO_MOVE_CAST(Executor)(other.executor_)),
      owns_(other.owns_)
  {
    if (owns_)
    {
      new (&work_) work_type(
          BOOST_ASIO_MOVE_CAST(work_type)(
            *static_cast<work_type*>(
              static_cast<void*>(&other.work_))));
      other.owns_ = false;
    }
  }
#endif //  defined(BOOST_ASIO_HAS_MOVE)

  ~executor_work_guard()
  {
    if (owns_)
      static_cast<work_type*>(static_cast<void*>(&work_))->~work_type();
  }

  executor_type get_executor() const BOOST_ASIO_NOEXCEPT
  {
    return executor_;
  }

  bool owns_work() const BOOST_ASIO_NOEXCEPT
  {
    return owns_;
  }

  void reset() BOOST_ASIO_NOEXCEPT
  {
    if (owns_)
    {
      static_cast<work_type*>(static_cast<void*>(&work_))->~work_type();
      owns_ = false;
    }
  }

private:
  // Disallow assignment.
  executor_work_guard& operator=(const executor_work_guard&);

  typedef typename decay<
      typename prefer_result<
        const executor_type&,
        execution::outstanding_work_t::tracked_t
      >::type
    >::type work_type;

  executor_type executor_;
  typename aligned_storage<sizeof(work_type),
      alignment_of<work_type>::value>::type work_;
  bool owns_;
};

#endif // !defined(GENERATING_DOCUMENTATION)

/// Create an @ref executor_work_guard object.
/**
 * @param ex An executor.
 *
 * @returns A work guard constructed with the specified executor.
 */
template <typename Executor>
BOOST_ASIO_NODISCARD inline executor_work_guard<Executor>
make_work_guard(const Executor& ex,
    typename constraint<
      is_executor<Executor>::value || execution::is_executor<Executor>::value
    >::type = 0)
{
  return executor_work_guard<Executor>(ex);
}

/// Create an @ref executor_work_guard object.
/**
 * @param ctx An execution context, from which an executor will be obtained.
 *
 * @returns A work guard constructed with the execution context's executor,
 * obtained by performing <tt>ctx.get_executor()</tt>.
 */
template <typename ExecutionContext>
BOOST_ASIO_NODISCARD inline
executor_work_guard<typename ExecutionContext::executor_type>
make_work_guard(ExecutionContext& ctx,
    typename constraint<
      is_convertible<ExecutionContext&, execution_context&>::value
    >::type = 0)
{
  return executor_work_guard<typename ExecutionContext::executor_type>(
      ctx.get_executor());
}

/// Create an @ref executor_work_guard object.
/**
 * @param t An arbitrary object, such as a completion handler, for which the
 * associated executor will be obtained.
 *
 * @returns A work guard constructed with the associated executor of the object
 * @c t, which is obtained as if by calling <tt>get_associated_executor(t)</tt>.
 */
template <typename T>
BOOST_ASIO_NODISCARD inline
executor_work_guard<typename associated_executor<T>::type>
make_work_guard(const T& t,
    typename constraint<
      !is_executor<T>::value
    >::type = 0,
    typename constraint<
      !execution::is_executor<T>::value
    >::type = 0,
    typename constraint<
      !is_convertible<T&, execution_context&>::value
    >::type = 0)
{
  return executor_work_guard<typename associated_executor<T>::type>(
      associated_executor<T>::get(t));
}

/// Create an @ref executor_work_guard object.
/**
 * @param t An arbitrary object, such as a completion handler, for which the
 * associated executor will be obtained.
 *
 * @param ex An executor to be used as the candidate object when determining the
 * associated executor.
 *
 * @returns A work guard constructed with the associated executor of the object
 * @c t, which is obtained as if by calling <tt>get_associated_executor(t,
 * ex)</tt>.
 */
template <typename T, typename Executor>
BOOST_ASIO_NODISCARD inline
executor_work_guard<typename associated_executor<T, Executor>::type>
make_work_guard(const T& t, const Executor& ex,
    typename constraint<
      is_executor<Executor>::value || execution::is_executor<Executor>::value
    >::type = 0)
{
  return executor_work_guard<typename associated_executor<T, Executor>::type>(
      associated_executor<T, Executor>::get(t, ex));
}

/// Create an @ref executor_work_guard object.
/**
 * @param t An arbitrary object, such as a completion handler, for which the
 * associated executor will be obtained.
 *
 * @param ctx An execution context, from which an executor is obtained to use as
 * the candidate object for determining the associated executor.
 *
 * @returns A work guard constructed with the associated executor of the object
 * @c t, which is obtained as if by calling <tt>get_associated_executor(t,
 * ctx.get_executor())</tt>.
 */
template <typename T, typename ExecutionContext>
BOOST_ASIO_NODISCARD inline executor_work_guard<typename associated_executor<T,
  typename ExecutionContext::executor_type>::type>
make_work_guard(const T& t, ExecutionContext& ctx,
    typename constraint<
      !is_executor<T>::value
    >::type = 0,
    typename constraint<
      !execution::is_executor<T>::value
    >::type = 0,
    typename constraint<
      !is_convertible<T&, execution_context&>::value
    >::type = 0,
    typename constraint<
      is_convertible<ExecutionContext&, execution_context&>::value
    >::type = 0)
{
  return executor_work_guard<typename associated_executor<T,
    typename ExecutionContext::executor_type>::type>(
      associated_executor<T, typename ExecutionContext::executor_type>::get(
        t, ctx.get_executor()));
}

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_EXECUTOR_WORK_GUARD_HPP
