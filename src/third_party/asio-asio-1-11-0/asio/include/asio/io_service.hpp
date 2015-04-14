//
// io_service.hpp
// ~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_IO_SERVICE_HPP
#define ASIO_IO_SERVICE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include <cstddef>
#include <stdexcept>
#include <typeinfo>
#include "asio/async_result.hpp"
#include "asio/detail/noncopyable.hpp"
#include "asio/detail/wrapped_handler.hpp"
#include "asio/error_code.hpp"
#include "asio/execution_context.hpp"
#include "asio/is_executor.hpp"

#if defined(ASIO_WINDOWS) || defined(__CYGWIN__)
# include "asio/detail/winsock_init.hpp"
#elif defined(__sun) || defined(__QNX__) || defined(__hpux) || defined(_AIX) \
  || defined(__osf__)
# include "asio/detail/signal_init.hpp"
#endif

#include "asio/detail/push_options.hpp"

namespace asio {

namespace detail {
#if defined(ASIO_HAS_IOCP)
  typedef class win_iocp_io_service io_service_impl;
  class win_iocp_overlapped_ptr;
#else
  typedef class scheduler io_service_impl;
#endif
} // namespace detail

/// Provides core I/O functionality.
/**
 * The io_service class provides the core I/O functionality for users of the
 * asynchronous I/O objects, including:
 *
 * @li asio::ip::tcp::socket
 * @li asio::ip::tcp::acceptor
 * @li asio::ip::udp::socket
 * @li asio::deadline_timer.
 *
 * The io_service class also includes facilities intended for developers of
 * custom asynchronous services.
 *
 * @par Thread Safety
 * @e Distinct @e objects: Safe.@n
 * @e Shared @e objects: Safe, with the specific exceptions of the restart() and
 * notify_fork() functions. Calling restart() while there are unfinished run(),
 * run_one(), poll() or poll_one() calls results in undefined behaviour. The
 * notify_fork() function should not be called while any io_service function,
 * or any function on an I/O object that is associated with the io_service, is
 * being called in another thread.
 *
 * @par Concepts:
 * Dispatcher.
 *
 * @par Synchronous and asynchronous operations
 *
 * Synchronous operations on I/O objects implicitly run the io_service object
 * for an individual operation. The io_service functions run(), run_one(),
 * poll() or poll_one() must be called for the io_service to perform
 * asynchronous operations on behalf of a C++ program. Notification that an
 * asynchronous operation has completed is delivered by invocation of the
 * associated handler. Handlers are invoked only by a thread that is currently
 * calling any overload of run(), run_one(), poll() or poll_one() for the
 * io_service.
 *
 * @par Effect of exceptions thrown from handlers
 *
 * If an exception is thrown from a handler, the exception is allowed to
 * propagate through the throwing thread's invocation of run(), run_one(),
 * poll() or poll_one(). No other threads that are calling any of these
 * functions are affected. It is then the responsibility of the application to
 * catch the exception.
 *
 * After the exception has been caught, the run(), run_one(), poll() or
 * poll_one() call may be restarted @em without the need for an intervening
 * call to restart(). This allows the thread to rejoin the io_service object's
 * thread pool without impacting any other threads in the pool.
 *
 * For example:
 *
 * @code
 * asio::io_service io_service;
 * ...
 * for (;;)
 * {
 *   try
 *   {
 *     io_service.run();
 *     break; // run() exited normally
 *   }
 *   catch (my_exception& e)
 *   {
 *     // Deal with exception as appropriate.
 *   }
 * }
 * @endcode
 *
 * @par Stopping the io_service from running out of work
 *
 * Some applications may need to prevent an io_service object's run() call from
 * returning when there is no more work to do. For example, the io_service may
 * be being run in a background thread that is launched prior to the
 * application's asynchronous operations. The run() call may be kept running by
 * creating an object of type asio::io_service::work:
 *
 * @code asio::io_service io_service;
 * asio::io_service::work work(io_service);
 * ... @endcode
 *
 * To effect a shutdown, the application will then need to call the io_service
 * object's stop() member function. This will cause the io_service run() call
 * to return as soon as possible, abandoning unfinished operations and without
 * permitting ready handlers to be dispatched.
 *
 * Alternatively, if the application requires that all operations and handlers
 * be allowed to finish normally, the work object may be explicitly destroyed.
 *
 * @code asio::io_service io_service;
 * auto_ptr<asio::io_service::work> work(
 *     new asio::io_service::work(io_service));
 * ...
 * work.reset(); // Allow run() to exit. @endcode
 */
class io_service
  : public execution_context
{
private:
  typedef detail::io_service_impl impl_type;
#if defined(ASIO_HAS_IOCP)
  friend class detail::win_iocp_overlapped_ptr;
#endif

public:
  class executor_type;
  friend class executor_type;

  class work;
  friend class work;

  class service;

  class strand;

  /// Constructor.
  ASIO_DECL io_service();

  /// Constructor.
  /**
   * Construct with a hint about the required level of concurrency.
   *
   * @param concurrency_hint A suggestion to the implementation on how many
   * threads it should allow to run simultaneously.
   */
  ASIO_DECL explicit io_service(std::size_t concurrency_hint);

  /// Destructor.
  /**
   * On destruction, the io_service performs the following sequence of
   * operations:
   *
   * @li For each service object @c svc in the io_service set, in reverse order
   * of the beginning of service object lifetime, performs
   * @c svc->shutdown_service().
   *
   * @li Uninvoked handler objects that were scheduled for deferred invocation
   * on the io_service, or any associated strand, are destroyed.
   *
   * @li For each service object @c svc in the io_service set, in reverse order
   * of the beginning of service object lifetime, performs
   * <tt>delete static_cast<io_service::service*>(svc)</tt>.
   *
   * @note The destruction sequence described above permits programs to
   * simplify their resource management by using @c shared_ptr<>. Where an
   * object's lifetime is tied to the lifetime of a connection (or some other
   * sequence of asynchronous operations), a @c shared_ptr to the object would
   * be bound into the handlers for all asynchronous operations associated with
   * it. This works as follows:
   *
   * @li When a single connection ends, all associated asynchronous operations
   * complete. The corresponding handler objects are destroyed, and all
   * @c shared_ptr references to the objects are destroyed.
   *
   * @li To shut down the whole program, the io_service function stop() is
   * called to terminate any run() calls as soon as possible. The io_service
   * destructor defined above destroys all handlers, causing all @c shared_ptr
   * references to all connection objects to be destroyed.
   */
  ASIO_DECL ~io_service();

  /// Obtains the executor associated with the io_service.
  executor_type get_executor() ASIO_NOEXCEPT;

  /// Run the io_service object's event processing loop.
  /**
   * The run() function blocks until all work has finished and there are no
   * more handlers to be dispatched, or until the io_service has been stopped.
   *
   * Multiple threads may call the run() function to set up a pool of threads
   * from which the io_service may execute handlers. All threads that are
   * waiting in the pool are equivalent and the io_service may choose any one
   * of them to invoke a handler.
   *
   * A normal exit from the run() function implies that the io_service object
   * is stopped (the stopped() function returns @c true). Subsequent calls to
   * run(), run_one(), poll() or poll_one() will return immediately unless there
   * is a prior call to restart().
   *
   * @return The number of handlers that were executed.
   *
   * @throws asio::system_error Thrown on failure.
   *
   * @note The run() function must not be called from a thread that is currently
   * calling one of run(), run_one(), poll() or poll_one() on the same
   * io_service object.
   *
   * The poll() function may also be used to dispatch ready handlers, but
   * without blocking.
   */
  ASIO_DECL std::size_t run();

  /// Run the io_service object's event processing loop.
  /**
   * The run() function blocks until all work has finished and there are no
   * more handlers to be dispatched, or until the io_service has been stopped.
   *
   * Multiple threads may call the run() function to set up a pool of threads
   * from which the io_service may execute handlers. All threads that are
   * waiting in the pool are equivalent and the io_service may choose any one
   * of them to invoke a handler.
   *
   * A normal exit from the run() function implies that the io_service object
   * is stopped (the stopped() function returns @c true). Subsequent calls to
   * run(), run_one(), poll() or poll_one() will return immediately unless there
   * is a prior call to restart().
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @return The number of handlers that were executed.
   *
   * @note The run() function must not be called from a thread that is currently
   * calling one of run(), run_one(), poll() or poll_one() on the same
   * io_service object.
   *
   * The poll() function may also be used to dispatch ready handlers, but
   * without blocking.
   */
  ASIO_DECL std::size_t run(asio::error_code& ec);

  /// Run the io_service object's event processing loop to execute at most one
  /// handler.
  /**
   * The run_one() function blocks until one handler has been dispatched, or
   * until the io_service has been stopped.
   *
   * @return The number of handlers that were executed. A zero return value
   * implies that the io_service object is stopped (the stopped() function
   * returns @c true). Subsequent calls to run(), run_one(), poll() or
   * poll_one() will return immediately unless there is a prior call to
   * restart().
   *
   * @throws asio::system_error Thrown on failure.
   */
  ASIO_DECL std::size_t run_one();

  /// Run the io_service object's event processing loop to execute at most one
  /// handler.
  /**
   * The run_one() function blocks until one handler has been dispatched, or
   * until the io_service has been stopped.
   *
   * @return The number of handlers that were executed. A zero return value
   * implies that the io_service object is stopped (the stopped() function
   * returns @c true). Subsequent calls to run(), run_one(), poll() or
   * poll_one() will return immediately unless there is a prior call to
   * restart().
   *
   * @return The number of handlers that were executed.
   */
  ASIO_DECL std::size_t run_one(asio::error_code& ec);

  /// Run the io_service object's event processing loop to execute ready
  /// handlers.
  /**
   * The poll() function runs handlers that are ready to run, without blocking,
   * until the io_service has been stopped or there are no more ready handlers.
   *
   * @return The number of handlers that were executed.
   *
   * @throws asio::system_error Thrown on failure.
   */
  ASIO_DECL std::size_t poll();

  /// Run the io_service object's event processing loop to execute ready
  /// handlers.
  /**
   * The poll() function runs handlers that are ready to run, without blocking,
   * until the io_service has been stopped or there are no more ready handlers.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @return The number of handlers that were executed.
   */
  ASIO_DECL std::size_t poll(asio::error_code& ec);

  /// Run the io_service object's event processing loop to execute one ready
  /// handler.
  /**
   * The poll_one() function runs at most one handler that is ready to run,
   * without blocking.
   *
   * @return The number of handlers that were executed.
   *
   * @throws asio::system_error Thrown on failure.
   */
  ASIO_DECL std::size_t poll_one();

  /// Run the io_service object's event processing loop to execute one ready
  /// handler.
  /**
   * The poll_one() function runs at most one handler that is ready to run,
   * without blocking.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @return The number of handlers that were executed.
   */
  ASIO_DECL std::size_t poll_one(asio::error_code& ec);

  /// Stop the io_service object's event processing loop.
  /**
   * This function does not block, but instead simply signals the io_service to
   * stop. All invocations of its run() or run_one() member functions should
   * return as soon as possible. Subsequent calls to run(), run_one(), poll()
   * or poll_one() will return immediately until restart() is called.
   */
  ASIO_DECL void stop();

  /// Determine whether the io_service object has been stopped.
  /**
   * This function is used to determine whether an io_service object has been
   * stopped, either through an explicit call to stop(), or due to running out
   * of work. When an io_service object is stopped, calls to run(), run_one(),
   * poll() or poll_one() will return immediately without invoking any
   * handlers.
   *
   * @return @c true if the io_service object is stopped, otherwise @c false.
   */
  ASIO_DECL bool stopped() const;

  /// Restart the io_service in preparation for a subsequent run() invocation.
  /**
   * This function must be called prior to any second or later set of
   * invocations of the run(), run_one(), poll() or poll_one() functions when a
   * previous invocation of these functions returned due to the io_service
   * being stopped or running out of work. After a call to restart(), the
   * io_service object's stopped() function will return @c false.
   *
   * This function must not be called while there are any unfinished calls to
   * the run(), run_one(), poll() or poll_one() functions.
   */
  ASIO_DECL void restart();

#if !defined(ASIO_NO_DEPRECATED)
  /// (Deprecated: Use restart().) Reset the io_service in preparation for a
  /// subsequent run() invocation.
  /**
   * This function must be called prior to any second or later set of
   * invocations of the run(), run_one(), poll() or poll_one() functions when a
   * previous invocation of these functions returned due to the io_service
   * being stopped or running out of work. After a call to restart(), the
   * io_service object's stopped() function will return @c false.
   *
   * This function must not be called while there are any unfinished calls to
   * the run(), run_one(), poll() or poll_one() functions.
   */
  void reset();

  /// (Deprecated: Use asio::dispatch().) Request the io_service to
  /// invoke the given handler.
  /**
   * This function is used to ask the io_service to execute the given handler.
   *
   * The io_service guarantees that the handler will only be called in a thread
   * in which the run(), run_one(), poll() or poll_one() member functions is
   * currently being invoked. The handler may be executed inside this function
   * if the guarantee can be met.
   *
   * @param handler The handler to be called. The io_service will make
   * a copy of the handler object as required. The function signature of the
   * handler must be: @code void handler(); @endcode
   *
   * @note This function throws an exception only if:
   *
   * @li the handler's @c asio_handler_allocate function; or
   *
   * @li the handler's copy constructor
   *
   * throws an exception.
   */
  template <typename CompletionHandler>
  ASIO_INITFN_RESULT_TYPE(CompletionHandler, void ())
  dispatch(ASIO_MOVE_ARG(CompletionHandler) handler);

  /// (Deprecated: Use asio::post().) Request the io_service to invoke
  /// the given handler and return immediately.
  /**
   * This function is used to ask the io_service to execute the given handler,
   * but without allowing the io_service to call the handler from inside this
   * function.
   *
   * The io_service guarantees that the handler will only be called in a thread
   * in which the run(), run_one(), poll() or poll_one() member functions is
   * currently being invoked.
   *
   * @param handler The handler to be called. The io_service will make
   * a copy of the handler object as required. The function signature of the
   * handler must be: @code void handler(); @endcode
   *
   * @note This function throws an exception only if:
   *
   * @li the handler's @c asio_handler_allocate function; or
   *
   * @li the handler's copy constructor
   *
   * throws an exception.
   */
  template <typename CompletionHandler>
  ASIO_INITFN_RESULT_TYPE(CompletionHandler, void ())
  post(ASIO_MOVE_ARG(CompletionHandler) handler);

  /// (Deprecated: Use asio::wrap().) Create a new handler that
  /// automatically dispatches the wrapped handler on the io_service.
  /**
   * This function is used to create a new handler function object that, when
   * invoked, will automatically pass the wrapped handler to the io_service
   * object's dispatch function.
   *
   * @param handler The handler to be wrapped. The io_service will make a copy
   * of the handler object as required. The function signature of the handler
   * must be: @code void handler(A1 a1, ... An an); @endcode
   *
   * @return A function object that, when invoked, passes the wrapped handler to
   * the io_service object's dispatch function. Given a function object with the
   * signature:
   * @code R f(A1 a1, ... An an); @endcode
   * If this function object is passed to the wrap function like so:
   * @code io_service.wrap(f); @endcode
   * then the return value is a function object with the signature
   * @code void g(A1 a1, ... An an); @endcode
   * that, when invoked, executes code equivalent to:
   * @code io_service.dispatch(boost::bind(f, a1, ... an)); @endcode
   */
  template <typename Handler>
#if defined(GENERATING_DOCUMENTATION)
  unspecified
#else
  detail::wrapped_handler<io_service&, Handler>
#endif
  wrap(Handler handler);
#endif // !defined(ASIO_NO_DEPRECATED)

private:
  // Helper function to create the implementation.
  ASIO_DECL impl_type& create_impl(std::size_t concurrency_hint = 0);

  // Backwards compatible overload for use with services derived from
  // io_service::service.
  template <typename Service>
  friend Service& use_service(io_service& ios);

#if defined(ASIO_WINDOWS) || defined(__CYGWIN__)
  detail::winsock_init<> init_;
#elif defined(__sun) || defined(__QNX__) || defined(__hpux) || defined(_AIX) \
  || defined(__osf__)
  detail::signal_init<> init_;
#endif

  // The implementation.
  impl_type& impl_;
};

/// Executor used to submit functions to an io_service.
class io_service::executor_type
{
public:
  /// Obtain the underlying execution context.
  io_service& context() ASIO_NOEXCEPT;

  /// Inform the io_service that it has some outstanding work to do.
  /**
   * This function is used to inform the io_service that some work has begun.
   * This ensures that the io_service's run() and run_one() functions do not
   * exit while the work is underway.
   */
  void on_work_started() ASIO_NOEXCEPT;

  /// Inform the io_service that some work is no longer outstanding.
  /**
   * This function is used to inform the io_service that some work has
   * finished. Once the count of unfinished work reaches zero, the io_service
   * is stopped and the run() and run_one() functions may exit.
   */
  void on_work_finished() ASIO_NOEXCEPT;

  /// Request the io_service to invoke the given function object.
  /**
   * This function is used to ask the io_service to execute the given function
   * object. If the current thread is running the io_service, @c dispatch()
   * executes the function before returning. Otherwise, the function will be
   * scheduled to run on the io_service.
   *
   * @param f The function object to be called. The executor will make a copy
   * of the handler object as required. The function signature of the function
   * object must be: @code void function(); @endcode
   *
   * @param a An allocator that may be used by the executor to allocate the
   * internal storage needed for function invocation.
   */
  template <typename Function, typename Allocator>
  void dispatch(ASIO_MOVE_ARG(Function) f, const Allocator& a);

  /// Request the io_service to invoke the given function object.
  /**
   * This function is used to ask the io_service to execute the given function
   * object. The function object will never be executed inside @c post().
   * Instead, it will be scheduled to run on the io_service.
   *
   * @param f The function object to be called. The executor will make a copy
   * of the handler object as required. The function signature of the function
   * object must be: @code void function(); @endcode
   *
   * @param a An allocator that may be used by the executor to allocate the
   * internal storage needed for function invocation.
   */
  template <typename Function, typename Allocator>
  void post(ASIO_MOVE_ARG(Function) f, const Allocator& a);

  /// Request the io_service to invoke the given function object.
  /**
   * This function is used to ask the io_service to execute the given function
   * object. The function object will never be executed inside @c defer().
   * Instead, it will be scheduled to run on the io_service.
   *
   * If the current thread belongs to the io_service, @c defer() will delay
   * scheduling the function object until the current thread returns control to
   * the pool.
   *
   * @param f The function object to be called. The executor will make a copy
   * of the handler object as required. The function signature of the function
   * object must be: @code void function(); @endcode
   *
   * @param a An allocator that may be used by the executor to allocate the
   * internal storage needed for function invocation.
   */
  template <typename Function, typename Allocator>
  void defer(ASIO_MOVE_ARG(Function) f, const Allocator& a);

  /// Determine whether the io_service is running in the current thread.
  /**
   * @return @c true if the current thread is running the io_service. Otherwise
   * returns @c false.
   */
  bool running_in_this_thread() const ASIO_NOEXCEPT;

  /// Compare two executors for equality.
  /**
   * Two executors are equal if they refer to the same underlying io_service.
   */
  friend bool operator==(const executor_type& a,
      const executor_type& b) ASIO_NOEXCEPT
  {
    return &a.io_service_ == &b.io_service_;
  }

  /// Compare two executors for inequality.
  /**
   * Two executors are equal if they refer to the same underlying io_service.
   */
  friend bool operator!=(const executor_type& a,
      const executor_type& b) ASIO_NOEXCEPT
  {
    return &a.io_service_ != &b.io_service_;
  }

private:
  friend class io_service;

  // Constructor.
  explicit executor_type(io_service& i) : io_service_(i) {}

  // The underlying io_service.
  io_service& io_service_;
};

#if !defined(GENERATING_DOCUMENTATION)
template <> struct is_executor<io_service::executor_type> : true_type {};
#endif // !defined(GENERATING_DOCUMENTATION)


/// (Deprecated: Use executor_work.) Class to inform the io_service when it has
/// work to do.
/**
 * The work class is used to inform the io_service when work starts and
 * finishes. This ensures that the io_service object's run() function will not
 * exit while work is underway, and that it does exit when there is no
 * unfinished work remaining.
 *
 * The work class is copy-constructible so that it may be used as a data member
 * in a handler class. It is not assignable.
 */
class io_service::work
{
public:
  /// Constructor notifies the io_service that work is starting.
  /**
   * The constructor is used to inform the io_service that some work has begun.
   * This ensures that the io_service object's run() function will not exit
   * while the work is underway.
   */
  explicit work(asio::io_service& io_service);

  /// Copy constructor notifies the io_service that work is starting.
  /**
   * The constructor is used to inform the io_service that some work has begun.
   * This ensures that the io_service object's run() function will not exit
   * while the work is underway.
   */
  work(const work& other);

  /// Destructor notifies the io_service that the work is complete.
  /**
   * The destructor is used to inform the io_service that some work has
   * finished. Once the count of unfinished work reaches zero, the io_service
   * object's run() function is permitted to exit.
   */
  ~work();

  /// Get the io_service associated with the work.
  asio::io_service& get_io_service();

private:
  // Prevent assignment.
  void operator=(const work& other);

  // The io_service implementation.
  detail::io_service_impl& io_service_impl_;
};

/// Base class for all io_service services.
class io_service::service
  : public execution_context::service
{
public:
  /// Get the io_service object that owns the service.
  asio::io_service& get_io_service();

protected:
  /// Constructor.
  /**
   * @param owner The io_service object that owns the service.
   */
  ASIO_DECL service(asio::io_service& owner);

  /// Destructor.
  ASIO_DECL virtual ~service();
};

namespace detail {

// Special service base class to keep classes header-file only.
template <typename Type>
class service_base
  : public asio::io_service::service
{
public:
  static asio::detail::service_id<Type> id;

  // Constructor.
  service_base(asio::io_service& io_service)
    : asio::io_service::service(io_service)
  {
  }
};

template <typename Type>
asio::detail::service_id<Type> service_base<Type>::id;

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#include "asio/impl/io_service.hpp"
#if defined(ASIO_HEADER_ONLY)
# include "asio/impl/io_service.ipp"
#endif // defined(ASIO_HEADER_ONLY)

// If both io_service.hpp and strand.hpp have been included, automatically
// include the header file needed for the io_service::strand class.
#if defined(ASIO_STRAND_HPP)
# include "asio/io_service_strand.hpp"
#endif // defined(ASIO_STRAND_HPP)

#endif // ASIO_IO_SERVICE_HPP
