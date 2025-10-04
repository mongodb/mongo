//
// basic_signal_set.hpp
// ~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_BASIC_SIGNAL_SET_HPP
#define ASIO_BASIC_SIGNAL_SET_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"

#include "asio/any_io_executor.hpp"
#include "asio/async_result.hpp"
#include "asio/detail/handler_type_requirements.hpp"
#include "asio/detail/io_object_impl.hpp"
#include "asio/detail/non_const_lvalue.hpp"
#include "asio/detail/signal_set_service.hpp"
#include "asio/detail/throw_error.hpp"
#include "asio/detail/type_traits.hpp"
#include "asio/error.hpp"
#include "asio/execution_context.hpp"
#include "asio/signal_set_base.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {

/// Provides signal functionality.
/**
 * The basic_signal_set class provides the ability to perform an asynchronous
 * wait for one or more signals to occur.
 *
 * @par Thread Safety
 * @e Distinct @e objects: Safe.@n
 * @e Shared @e objects: Unsafe.
 *
 * @par Example
 * Performing an asynchronous wait:
 * @code
 * void handler(
 *     const asio::error_code& error,
 *     int signal_number)
 * {
 *   if (!error)
 *   {
 *     // A signal occurred.
 *   }
 * }
 *
 * ...
 *
 * // Construct a signal set registered for process termination.
 * asio::signal_set signals(my_context, SIGINT, SIGTERM);
 *
 * // Start an asynchronous wait for one of the signals to occur.
 * signals.async_wait(handler);
 * @endcode
 *
 * @par Queueing of signal notifications
 *
 * If a signal is registered with a signal_set, and the signal occurs when
 * there are no waiting handlers, then the signal notification is queued. The
 * next async_wait operation on that signal_set will dequeue the notification.
 * If multiple notifications are queued, subsequent async_wait operations
 * dequeue them one at a time. Signal notifications are dequeued in order of
 * ascending signal number.
 *
 * If a signal number is removed from a signal_set (using the @c remove or @c
 * erase member functions) then any queued notifications for that signal are
 * discarded.
 *
 * @par Multiple registration of signals
 *
 * The same signal number may be registered with different signal_set objects.
 * When the signal occurs, one handler is called for each signal_set object.
 *
 * Note that multiple registration only works for signals that are registered
 * using Asio. The application must not also register a signal handler using
 * functions such as @c signal() or @c sigaction().
 *
 * @par Signal masking on POSIX platforms
 *
 * POSIX allows signals to be blocked using functions such as @c sigprocmask()
 * and @c pthread_sigmask(). For signals to be delivered, programs must ensure
 * that any signals registered using signal_set objects are unblocked in at
 * least one thread.
 */
template <typename Executor = any_io_executor>
class basic_signal_set : public signal_set_base
{
private:
  class initiate_async_wait;

public:
  /// The type of the executor associated with the object.
  typedef Executor executor_type;

  /// Rebinds the signal set type to another executor.
  template <typename Executor1>
  struct rebind_executor
  {
    /// The signal set type when rebound to the specified executor.
    typedef basic_signal_set<Executor1> other;
  };

  /// Construct a signal set without adding any signals.
  /**
   * This constructor creates a signal set without registering for any signals.
   *
   * @param ex The I/O executor that the signal set will use, by default, to
   * dispatch handlers for any asynchronous operations performed on the
   * signal set.
   */
  explicit basic_signal_set(const executor_type& ex)
    : impl_(0, ex)
  {
  }

  /// Construct a signal set without adding any signals.
  /**
   * This constructor creates a signal set without registering for any signals.
   *
   * @param context An execution context which provides the I/O executor that
   * the signal set will use, by default, to dispatch handlers for any
   * asynchronous operations performed on the signal set.
   */
  template <typename ExecutionContext>
  explicit basic_signal_set(ExecutionContext& context,
      constraint_t<
        is_convertible<ExecutionContext&, execution_context&>::value,
        defaulted_constraint
      > = defaulted_constraint())
    : impl_(0, 0, context)
  {
  }

  /// Construct a signal set and add one signal.
  /**
   * This constructor creates a signal set and registers for one signal.
   *
   * @param ex The I/O executor that the signal set will use, by default, to
   * dispatch handlers for any asynchronous operations performed on the
   * signal set.
   *
   * @param signal_number_1 The signal number to be added.
   *
   * @note This constructor is equivalent to performing:
   * @code asio::signal_set signals(ex);
   * signals.add(signal_number_1); @endcode
   */
  basic_signal_set(const executor_type& ex, int signal_number_1)
    : impl_(0, ex)
  {
    asio::error_code ec;
    impl_.get_service().add(impl_.get_implementation(), signal_number_1, ec);
    asio::detail::throw_error(ec, "add");
  }

  /// Construct a signal set and add one signal.
  /**
   * This constructor creates a signal set and registers for one signal.
   *
   * @param context An execution context which provides the I/O executor that
   * the signal set will use, by default, to dispatch handlers for any
   * asynchronous operations performed on the signal set.
   *
   * @param signal_number_1 The signal number to be added.
   *
   * @note This constructor is equivalent to performing:
   * @code asio::signal_set signals(context);
   * signals.add(signal_number_1); @endcode
   */
  template <typename ExecutionContext>
  basic_signal_set(ExecutionContext& context, int signal_number_1,
      constraint_t<
        is_convertible<ExecutionContext&, execution_context&>::value,
        defaulted_constraint
      > = defaulted_constraint())
    : impl_(0, 0, context)
  {
    asio::error_code ec;
    impl_.get_service().add(impl_.get_implementation(), signal_number_1, ec);
    asio::detail::throw_error(ec, "add");
  }

  /// Construct a signal set and add two signals.
  /**
   * This constructor creates a signal set and registers for two signals.
   *
   * @param ex The I/O executor that the signal set will use, by default, to
   * dispatch handlers for any asynchronous operations performed on the
   * signal set.
   *
   * @param signal_number_1 The first signal number to be added.
   *
   * @param signal_number_2 The second signal number to be added.
   *
   * @note This constructor is equivalent to performing:
   * @code asio::signal_set signals(ex);
   * signals.add(signal_number_1);
   * signals.add(signal_number_2); @endcode
   */
  basic_signal_set(const executor_type& ex, int signal_number_1,
      int signal_number_2)
    : impl_(0, ex)
  {
    asio::error_code ec;
    impl_.get_service().add(impl_.get_implementation(), signal_number_1, ec);
    asio::detail::throw_error(ec, "add");
    impl_.get_service().add(impl_.get_implementation(), signal_number_2, ec);
    asio::detail::throw_error(ec, "add");
  }

  /// Construct a signal set and add two signals.
  /**
   * This constructor creates a signal set and registers for two signals.
   *
   * @param context An execution context which provides the I/O executor that
   * the signal set will use, by default, to dispatch handlers for any
   * asynchronous operations performed on the signal set.
   *
   * @param signal_number_1 The first signal number to be added.
   *
   * @param signal_number_2 The second signal number to be added.
   *
   * @note This constructor is equivalent to performing:
   * @code asio::signal_set signals(context);
   * signals.add(signal_number_1);
   * signals.add(signal_number_2); @endcode
   */
  template <typename ExecutionContext>
  basic_signal_set(ExecutionContext& context, int signal_number_1,
      int signal_number_2,
      constraint_t<
        is_convertible<ExecutionContext&, execution_context&>::value,
        defaulted_constraint
      > = defaulted_constraint())
    : impl_(0, 0, context)
  {
    asio::error_code ec;
    impl_.get_service().add(impl_.get_implementation(), signal_number_1, ec);
    asio::detail::throw_error(ec, "add");
    impl_.get_service().add(impl_.get_implementation(), signal_number_2, ec);
    asio::detail::throw_error(ec, "add");
  }

  /// Construct a signal set and add three signals.
  /**
   * This constructor creates a signal set and registers for three signals.
   *
   * @param ex The I/O executor that the signal set will use, by default, to
   * dispatch handlers for any asynchronous operations performed on the
   * signal set.
   *
   * @param signal_number_1 The first signal number to be added.
   *
   * @param signal_number_2 The second signal number to be added.
   *
   * @param signal_number_3 The third signal number to be added.
   *
   * @note This constructor is equivalent to performing:
   * @code asio::signal_set signals(ex);
   * signals.add(signal_number_1);
   * signals.add(signal_number_2);
   * signals.add(signal_number_3); @endcode
   */
  basic_signal_set(const executor_type& ex, int signal_number_1,
      int signal_number_2, int signal_number_3)
    : impl_(0, ex)
  {
    asio::error_code ec;
    impl_.get_service().add(impl_.get_implementation(), signal_number_1, ec);
    asio::detail::throw_error(ec, "add");
    impl_.get_service().add(impl_.get_implementation(), signal_number_2, ec);
    asio::detail::throw_error(ec, "add");
    impl_.get_service().add(impl_.get_implementation(), signal_number_3, ec);
    asio::detail::throw_error(ec, "add");
  }

  /// Construct a signal set and add three signals.
  /**
   * This constructor creates a signal set and registers for three signals.
   *
   * @param context An execution context which provides the I/O executor that
   * the signal set will use, by default, to dispatch handlers for any
   * asynchronous operations performed on the signal set.
   *
   * @param signal_number_1 The first signal number to be added.
   *
   * @param signal_number_2 The second signal number to be added.
   *
   * @param signal_number_3 The third signal number to be added.
   *
   * @note This constructor is equivalent to performing:
   * @code asio::signal_set signals(context);
   * signals.add(signal_number_1);
   * signals.add(signal_number_2);
   * signals.add(signal_number_3); @endcode
   */
  template <typename ExecutionContext>
  basic_signal_set(ExecutionContext& context, int signal_number_1,
      int signal_number_2, int signal_number_3,
      constraint_t<
        is_convertible<ExecutionContext&, execution_context&>::value,
        defaulted_constraint
      > = defaulted_constraint())
    : impl_(0, 0, context)
  {
    asio::error_code ec;
    impl_.get_service().add(impl_.get_implementation(), signal_number_1, ec);
    asio::detail::throw_error(ec, "add");
    impl_.get_service().add(impl_.get_implementation(), signal_number_2, ec);
    asio::detail::throw_error(ec, "add");
    impl_.get_service().add(impl_.get_implementation(), signal_number_3, ec);
    asio::detail::throw_error(ec, "add");
  }

  /// Destroys the signal set.
  /**
   * This function destroys the signal set, cancelling any outstanding
   * asynchronous wait operations associated with the signal set as if by
   * calling @c cancel.
   */
  ~basic_signal_set()
  {
  }

  /// Get the executor associated with the object.
  const executor_type& get_executor() noexcept
  {
    return impl_.get_executor();
  }

  /// Add a signal to a signal_set.
  /**
   * This function adds the specified signal to the set. It has no effect if the
   * signal is already in the set.
   *
   * @param signal_number The signal to be added to the set.
   *
   * @throws asio::system_error Thrown on failure.
   */
  void add(int signal_number)
  {
    asio::error_code ec;
    impl_.get_service().add(impl_.get_implementation(), signal_number, ec);
    asio::detail::throw_error(ec, "add");
  }

  /// Add a signal to a signal_set.
  /**
   * This function adds the specified signal to the set. It has no effect if the
   * signal is already in the set.
   *
   * @param signal_number The signal to be added to the set.
   *
   * @param ec Set to indicate what error occurred, if any.
   */
  ASIO_SYNC_OP_VOID add(int signal_number,
      asio::error_code& ec)
  {
    impl_.get_service().add(impl_.get_implementation(), signal_number, ec);
    ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Add a signal to a signal_set with the specified flags.
  /**
   * This function adds the specified signal to the set. It has no effect if the
   * signal is already in the set.
   *
   * Flags other than flags::dont_care require OS support for the @c sigaction
   * call, and this function will fail with @c error::operation_not_supported if
   * this is unavailable.
   *
   * The specified flags will conflict with a prior, active registration of the
   * same signal, if either specified a flags value other than flags::dont_care.
   * In this case, the @c add will fail with @c error::invalid_argument.
   *
   * @param signal_number The signal to be added to the set.
   *
   * @param f Flags to modify the behaviour of the specified signal.
   *
   * @throws asio::system_error Thrown on failure.
   */
  void add(int signal_number, flags_t f)
  {
    asio::error_code ec;
    impl_.get_service().add(impl_.get_implementation(), signal_number, f, ec);
    asio::detail::throw_error(ec, "add");
  }

  /// Add a signal to a signal_set with the specified flags.
  /**
   * This function adds the specified signal to the set. It has no effect if the
   * signal is already in the set.
   *
   * Flags other than flags::dont_care require OS support for the @c sigaction
   * call, and this function will fail with @c error::operation_not_supported if
   * this is unavailable.
   *
   * The specified flags will conflict with a prior, active registration of the
   * same signal, if either specified a flags value other than flags::dont_care.
   * In this case, the @c add will fail with @c error::invalid_argument.
   *
   * @param signal_number The signal to be added to the set.
   *
   * @param f Flags to modify the behaviour of the specified signal.
   *
   * @param ec Set to indicate what error occurred, if any.
   */
  ASIO_SYNC_OP_VOID add(int signal_number, flags_t f,
      asio::error_code& ec)
  {
    impl_.get_service().add(impl_.get_implementation(), signal_number, f, ec);
    ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Remove a signal from a signal_set.
  /**
   * This function removes the specified signal from the set. It has no effect
   * if the signal is not in the set.
   *
   * @param signal_number The signal to be removed from the set.
   *
   * @throws asio::system_error Thrown on failure.
   *
   * @note Removes any notifications that have been queued for the specified
   * signal number.
   */
  void remove(int signal_number)
  {
    asio::error_code ec;
    impl_.get_service().remove(impl_.get_implementation(), signal_number, ec);
    asio::detail::throw_error(ec, "remove");
  }

  /// Remove a signal from a signal_set.
  /**
   * This function removes the specified signal from the set. It has no effect
   * if the signal is not in the set.
   *
   * @param signal_number The signal to be removed from the set.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @note Removes any notifications that have been queued for the specified
   * signal number.
   */
  ASIO_SYNC_OP_VOID remove(int signal_number,
      asio::error_code& ec)
  {
    impl_.get_service().remove(impl_.get_implementation(), signal_number, ec);
    ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Remove all signals from a signal_set.
  /**
   * This function removes all signals from the set. It has no effect if the set
   * is already empty.
   *
   * @throws asio::system_error Thrown on failure.
   *
   * @note Removes all queued notifications.
   */
  void clear()
  {
    asio::error_code ec;
    impl_.get_service().clear(impl_.get_implementation(), ec);
    asio::detail::throw_error(ec, "clear");
  }

  /// Remove all signals from a signal_set.
  /**
   * This function removes all signals from the set. It has no effect if the set
   * is already empty.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @note Removes all queued notifications.
   */
  ASIO_SYNC_OP_VOID clear(asio::error_code& ec)
  {
    impl_.get_service().clear(impl_.get_implementation(), ec);
    ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Cancel all operations associated with the signal set.
  /**
   * This function forces the completion of any pending asynchronous wait
   * operations against the signal set. The handler for each cancelled
   * operation will be invoked with the asio::error::operation_aborted
   * error code.
   *
   * Cancellation does not alter the set of registered signals.
   *
   * @throws asio::system_error Thrown on failure.
   *
   * @note If a registered signal occurred before cancel() is called, then the
   * handlers for asynchronous wait operations will:
   *
   * @li have already been invoked; or
   *
   * @li have been queued for invocation in the near future.
   *
   * These handlers can no longer be cancelled, and therefore are passed an
   * error code that indicates the successful completion of the wait operation.
   */
  void cancel()
  {
    asio::error_code ec;
    impl_.get_service().cancel(impl_.get_implementation(), ec);
    asio::detail::throw_error(ec, "cancel");
  }

  /// Cancel all operations associated with the signal set.
  /**
   * This function forces the completion of any pending asynchronous wait
   * operations against the signal set. The handler for each cancelled
   * operation will be invoked with the asio::error::operation_aborted
   * error code.
   *
   * Cancellation does not alter the set of registered signals.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @note If a registered signal occurred before cancel() is called, then the
   * handlers for asynchronous wait operations will:
   *
   * @li have already been invoked; or
   *
   * @li have been queued for invocation in the near future.
   *
   * These handlers can no longer be cancelled, and therefore are passed an
   * error code that indicates the successful completion of the wait operation.
   */
  ASIO_SYNC_OP_VOID cancel(asio::error_code& ec)
  {
    impl_.get_service().cancel(impl_.get_implementation(), ec);
    ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Start an asynchronous operation to wait for a signal to be delivered.
  /**
   * This function may be used to initiate an asynchronous wait against the
   * signal set. It is an initiating function for an @ref
   * asynchronous_operation, and always returns immediately.
   *
   * For each call to async_wait(), the completion handler will be called
   * exactly once. The completion handler will be called when:
   *
   * @li One of the registered signals in the signal set occurs; or
   *
   * @li The signal set was cancelled, in which case the handler is passed the
   * error code asio::error::operation_aborted.
   *
   * @param token The @ref completion_token that will be used to produce a
   * completion handler, which will be called when the wait completes.
   * Potential completion tokens include @ref use_future, @ref use_awaitable,
   * @ref yield_context, or a function object with the correct completion
   * signature. The function signature of the completion handler must be:
   * @code void handler(
   *   const asio::error_code& error, // Result of operation.
   *   int signal_number // Indicates which signal occurred.
   * ); @endcode
   * Regardless of whether the asynchronous operation completes immediately or
   * not, the completion handler will not be invoked from within this function.
   * On immediate completion, invocation of the handler will be performed in a
   * manner equivalent to using asio::async_immediate().
   *
   * @par Completion Signature
   * @code void(asio::error_code, int) @endcode
   *
   * @par Per-Operation Cancellation
   * This asynchronous operation supports cancellation for the following
   * asio::cancellation_type values:
   *
   * @li @c cancellation_type::terminal
   *
   * @li @c cancellation_type::partial
   *
   * @li @c cancellation_type::total
   */
  template <
    ASIO_COMPLETION_TOKEN_FOR(void (asio::error_code, int))
      SignalToken = default_completion_token_t<executor_type>>
  auto async_wait(
      SignalToken&& token = default_completion_token_t<executor_type>())
    -> decltype(
      async_initiate<SignalToken, void (asio::error_code, int)>(
        declval<initiate_async_wait>(), token))
  {
    return async_initiate<SignalToken, void (asio::error_code, int)>(
        initiate_async_wait(this), token);
  }

private:
  // Disallow copying and assignment.
  basic_signal_set(const basic_signal_set&) = delete;
  basic_signal_set& operator=(const basic_signal_set&) = delete;

  class initiate_async_wait
  {
  public:
    typedef Executor executor_type;

    explicit initiate_async_wait(basic_signal_set* self)
      : self_(self)
    {
    }

    const executor_type& get_executor() const noexcept
    {
      return self_->get_executor();
    }

    template <typename SignalHandler>
    void operator()(SignalHandler&& handler) const
    {
      // If you get an error on the following line it means that your handler
      // does not meet the documented type requirements for a SignalHandler.
      ASIO_SIGNAL_HANDLER_CHECK(SignalHandler, handler) type_check;

      detail::non_const_lvalue<SignalHandler> handler2(handler);
      self_->impl_.get_service().async_wait(
          self_->impl_.get_implementation(),
          handler2.value, self_->impl_.get_executor());
    }

  private:
    basic_signal_set* self_;
  };

  detail::io_object_impl<detail::signal_set_service, Executor> impl_;
};

} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_BASIC_SIGNAL_SET_HPP
