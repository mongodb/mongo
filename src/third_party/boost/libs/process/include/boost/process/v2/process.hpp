// Copyright (c) 2021 Klemens D. Morgenstern (klemens dot morgenstern at gmx dot net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//
// process.hpp
// ~~~~~~~~~~~~~~
//

#ifndef BOOST_PROCESS_V2_PROCESS_HPP
#define BOOST_PROCESS_V2_PROCESS_HPP

#include <boost/process/v2/detail/config.hpp>
#include <boost/process/v2/default_launcher.hpp>
#include <boost/process/v2/exit_code.hpp>
#include <boost/process/v2/pid.hpp>
#include <boost/process/v2/ext/exe.hpp>
#include <boost/process/v2/process_handle.hpp>

#if defined(BOOST_PROCESS_V2_STANDALONE)
#include <asio/any_io_executor.hpp>
#include <asio/post.hpp>
#include <utility>
#else
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/core/exchange.hpp>
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

/// A class managing a subprocess
/* A `basic_process` object manages a subprocess; it tracks the status and exit-code,
 * and will terminate the process on destruction if `detach` was not called.
*/
template<typename Executor = net::any_io_executor>
struct basic_process
{
  /// The executor of the process
  using executor_type = Executor;
  /// Get the executor of the process
  executor_type get_executor() {return process_handle_.get_executor();}

  /// The non-closing handle type
  using handle_type = basic_process_handle<executor_type>;

  /// Get the underlying non-closing handle
  handle_type & handle() { return process_handle_; }

  /// Get the underlying non-closing handle
  const handle_type & handle() const { return process_handle_; }

  /// Provides access to underlying operating system facilities
  using native_handle_type = typename handle_type::native_handle_type;

  /// Rebinds the process_handle to another executor.
  template <typename Executor1>
  struct rebind_executor
  {
    /// The socket type when rebound to the specified executor.
    typedef basic_process<Executor1> other;
  };

  /** An empty process is similar to a default constructed thread. It holds an empty
  handle and is a place holder for a process that is to be launched later. */
  basic_process() = default;

  basic_process(const basic_process&) = delete;
  basic_process& operator=(const basic_process&) = delete;

  /// Move construct the process. It will be detached from `lhs`.
  basic_process(basic_process&& lhs) = default;

  /// Move assign a process. It will be detached from `lhs`.
  basic_process& operator=(basic_process&& lhs) = default;

  /// Move construct and rebind the executor.
  template<typename Executor1>
  basic_process(basic_process<Executor1>&& lhs)
          : process_handle_(std::move(lhs.process_handle_)),
            exit_status_{lhs.exit_status_}
  {
  }

  /// Construct a child from a property list and launch it using the default launcher..
  template<typename ... Inits>
  explicit basic_process(
      executor_type executor,
      const filesystem::path& exe,
      std::initializer_list<string_view> args,
      Inits&&... inits)
      : basic_process(default_process_launcher()(std::move(executor), exe, args, std::forward<Inits>(inits)...))
  {
  }
  
  /// Construct a child from a property list and launch it using the default launcher..
  template<typename Args, typename ... Inits>
  explicit basic_process(
      executor_type executor,
      const filesystem::path& exe,
      Args&& args, Inits&&... inits)
      : basic_process(default_process_launcher()(std::move(executor), exe,
                                               std::forward<Args>(args), std::forward<Inits>(inits)...))
  {
  }

  /// Construct a child from a property list and launch it using the default launcher..
  template<typename ExecutionContext, typename ... Inits>
  explicit basic_process(
      ExecutionContext & context,
      typename std::enable_if<
          std::is_convertible<ExecutionContext&, 
                              net::execution_context&>::value,
          const filesystem::path&>::type exe,
      std::initializer_list<string_view> args,
      Inits&&... inits)
      : basic_process(default_process_launcher()(executor_type(context.get_executor()),
                                                 exe, args, std::forward<Inits>(inits)...))
  {
  }
  /// Construct a child from a property list and launch it using the default launcher.
  template<typename ExecutionContext, typename Args, typename ... Inits>
  explicit basic_process(
      ExecutionContext & context,
      typename std::enable_if<
          std::is_convertible<ExecutionContext&, 
                              net::execution_context&>::value,
          const filesystem::path&>::type exe,
      Args&& args, Inits&&... inits)
      : basic_process(default_process_launcher()(executor_type(context.get_executor()),
       exe, std::forward<Args>(args), std::forward<Inits>(inits)...))
  {
  }

  /// Attach to an existing process
  explicit basic_process(executor_type exec, pid_type pid) : process_handle_(std::move(exec), pid) {}

  /// Attach to an existing process and the internal handle
  explicit basic_process(executor_type exec, pid_type pid, native_handle_type native_handle)
        : process_handle_(std::move(exec), pid, native_handle) {}

  /// Create an invalid handle
  explicit basic_process(executor_type exec) : process_handle_{std::move(exec)} {}

  /// Attach to an existing process
  template <typename ExecutionContext>
  explicit basic_process(ExecutionContext & context, pid_type pid,
                         typename std::enable_if<
                             std::is_convertible<ExecutionContext&,
                                net::execution_context&>::value, void *>::type = nullptr)
       : process_handle_(context, pid) {}

  /// Attach to an existing process and the internal handle
  template <typename ExecutionContext>
  explicit basic_process(ExecutionContext & context, pid_type pid, native_handle_type native_handle,
                         typename std::enable_if<
                            std::is_convertible<ExecutionContext&, 
                                net::execution_context&>::value, void *>::type = nullptr)
      : process_handle_(context.get_executor(), pid, native_handle) {}

  /// Create an invalid handle
  template <typename ExecutionContext>
  explicit basic_process(ExecutionContext & context,
                         typename std::enable_if<
                             is_convertible<ExecutionContext&, 
                                net::execution_context&>::value, void *>::type = nullptr)
     : process_handle_(context.get_executor()) {}



  /// Destruct the handle and terminate the process if it wasn't detached.
  ~basic_process()
  {
    process_handle_.terminate_if_running();
  }

  /// Sends the process a signal to ask for an interrupt, which the process may interpret as a shutdown.
  /** Maybe be ignored by the subprocess. */
  void interrupt()
  {
    error_code ec;
    interrupt(ec);
    if (ec)
      throw system_error(ec, "interrupt failed");

  }
  /// Throwing @overload void interrupt()
  void interrupt(error_code & ec)
  {
    process_handle_.interrupt(ec);
  }

  /// Throwing @overload void request_exit(error_code & ec)
  void request_exit()
  {
    error_code ec;
    request_exit(ec);
    if (ec)
      throw system_error(ec, "request_exit failed");
  }
  /// Sends the process a signal to ask for a graceful shutdown. Maybe be ignored by the subprocess.
  void request_exit(error_code & ec)
  {
    process_handle_.request_exit(ec);
  }

  /// Send the process a signal requesting it to stop. This may rely on undocumented functions.
  void suspend(error_code &ec)
  {
    process_handle_.suspend(ec);
  }

  /// Send the process a signal requesting it to stop. This may rely on undocumented functions.
  void suspend()
  {
    error_code ec;
    suspend(ec);
    if (ec)
        detail::throw_error(ec, "suspend");
  }


  /// Send the process a signal requesting it to resume. This may rely on undocumented functions.
  void resume(error_code &ec)
  {
    process_handle_.resume(ec);  
  }

  /// Send the process a signal requesting it to resume. This may rely on undocumented functions.
  void resume()
  {
      error_code ec;
      suspend(ec);
      if (ec)
          detail::throw_error(ec, "resume");
  }

  /// Throwing @overload void terminate(native_exit_code_type &exit_code, error_code & ec)
  void terminate()
  {
    error_code ec;
    terminate(ec);
    if (ec)
      detail::throw_error(ec, "terminate failed");
  }
  /// Unconditionally terminates the process and stores the exit code in exit_status.
  void terminate(error_code & ec)
  {
    process_handle_.terminate(exit_status_, ec);
  }

  /// Throwing @overload wait(error_code & ec)
  int wait()
  {
    error_code ec;
    if (running(ec))
      process_handle_.wait(exit_status_, ec);
    if (ec)
      detail::throw_error(ec, "wait failed");
    return exit_code();
  }
  /// Waits for the process to exit, store the exit code internally and return it.
  int wait(error_code & ec)
  {
    if (running(ec))
        process_handle_.wait(exit_status_, ec);
    return exit_code();
  }

  /// Detach the process.
  handle_type detach()
  {
#if defined(BOOST_PROCESS_V2_STANDALONE)
    return std::exchange(process_handle_, get_executor());
#else
    return boost::exchange(process_handle_, get_executor());
#endif
  }
  /// Get the native
  native_handle_type native_handle() {return process_handle_.native_handle(); }
  /// Return the evaluated exit_code.
  int exit_code() const
  {
    return evaluate_exit_code(exit_status_);
  }

  /// Get the id of the process;
  pid_type id() const {return process_handle_.id();}

  /// The native handle of the process. 
  /** This might be undefined on posix systems that only support signals */
  native_exit_code_type native_exit_code() const
  {
    return exit_status_;
  }
  /// Checks if the current process is running. 
  /** If it has already completed the exit code will be stored internally 
   * and can be obtained by calling `exit_code.
   */
  bool running()
  {
    error_code ec;
    native_exit_code_type exit_code{};
    auto r =  process_handle_.running(exit_code, ec);
    if (!ec && !r)
      exit_status_ = exit_code;
    else
      detail::throw_error(ec, "running failed");

    return r;
  }

  /// Throwing @overload bool running(error_code & ec)
  bool running(error_code & ec) noexcept
  {
    native_exit_code_type exit_code{};
    auto r =  process_handle_.running(exit_code, ec);
    if (!ec && !r)
      exit_status_ = exit_code;
    return r;
  }
  
  /// Check if the process is referring to an existing process.
  /** Note that this might be a process that already exited.*/
  bool is_open() const { return process_handle_.is_open(); }
  


private:
  template<typename Executor1>
  friend struct basic_process;

  basic_process_handle<Executor> process_handle_;
  native_exit_code_type exit_status_{detail::still_active};

  
  struct async_wait_op_
  {
    basic_process_handle<Executor> & handle;
    native_exit_code_type & res;

    template<typename Self>
    void operator()(Self && self)
    {
      if (!process_is_running(res))
      {
        struct completer
        {
            int code;
            typename std::decay<Self>::type self;
            void operator()()
            {
                self.complete(error_code{}, evaluate_exit_code(code));
            }
        };

        net::post(handle.get_executor(),
                                              completer{static_cast<int>(res), std::move(self)});
      }
      else
        handle.async_wait(std::move(self));
    }

    template<typename Self>
    void operator()(Self && self, error_code ec, native_exit_code_type code)
    {
      if (!ec && process_is_running(code))
        handle.async_wait(std::move(self));
      else
      {
        if (!ec)
          res = code;
        std::move(self).complete(ec, evaluate_exit_code(code));
      }
    }
  };

 public:
  /// Asynchronously wait for the process to exit and deliver the native exit-code in the completion handler.
  template <BOOST_PROCESS_V2_COMPLETION_TOKEN_FOR(void (error_code, int))
  WaitHandler = net::default_completion_token_t<executor_type>>
  auto async_wait(WaitHandler && handler = net::default_completion_token_t<executor_type>())
    -> decltype(net::async_compose<WaitHandler, void (error_code, int)>(
        async_wait_op_{process_handle_, exit_status_}, handler, process_handle_))
  {
    return net::async_compose<WaitHandler, void (error_code, int)>(
        async_wait_op_{process_handle_, exit_status_}, handler, process_handle_);
  }
};

/// Process with the default executor.
typedef basic_process<> process;

BOOST_PROCESS_V2_END_NAMESPACE


#endif //BOOST_PROCESS_V2_PROCESS_HPP
