// Copyright (c) 2022 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PROCESS_V2_PROCESS_HANDLE_HPP
#define BOOST_PROCESS_V2_PROCESS_HANDLE_HPP

#include <boost/process/v2/detail/config.hpp>

#if defined(BOOST_PROCESS_V2_WINDOWS)
#include <boost/process/v2/detail/process_handle_windows.hpp>
#else

#if defined(BOOST_PROCESS_V2_PIDFD_OPEN)
#include <boost/process/v2/detail/process_handle_fd.hpp>
#elif defined(BOOST_PROCESS_V2_PDFORK)
#include <boost/process/v2/detail/process_handle_fd_or_signal.hpp>
#else
// with asio support we could use EVFILT_PROC:NOTE_EXIT as well.
#include <boost/process/v2/detail/process_handle_signal.hpp>
#endif
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE


#if defined(GENERATING_DOCUMENTATION)
/** A process handle is an unmanaged version of a process.
 * This means it does not terminate the process on destruction and
 * will not keep track of the exit-code. 
 * 
 * Note that the exit code might be discovered early, during a call to `running`.
 * Thus it can only be discovered that process has exited already.
 */
template<typename Executor = net::any_io_executor>
struct basic_process_handle
{
    /// The native handle of the process. 
    /** This might be undefined on posix systems that only support signals */
    using native_handle_type = implementation_defined;

    /// The executor_type of the process_handle
    using executor_type =  Executor;

    /// Getter for the executor
    executor_type get_executor();

    /// Rebinds the process_handle to another executor.
    template<typename Executor1>
    struct rebind_executor
    {
        /// The socket type when rebound to the specified executor.
        typedef basic_process_handle<Executor1> other;
    };


    /// Construct a basic_process_handle from an execution_context.
    /**
    * @tparam ExecutionContext The context must fulfill the asio::execution_context requirements
    */
    template<typename ExecutionContext>
    basic_process_handle(ExecutionContext &context);

    /// Construct an empty process_handle from an executor.
    basic_process_handle(executor_type executor);

    /// Construct an empty process_handle from an executor and bind it to a pid.
    /** On NON-linux posix systems this call is not able to obtain a file-descriptor and will thus 
     * rely on signals.
     */
    basic_process_handle(executor_type executor, pid_type pid);

    /// Construct an empty process_handle from an executor and bind it to a pid and the native-handle
    /** On some non-linux posix systems this overload is not present.
     */
    basic_process_handle(executor_type executor, pid_type pid, native_handle_type process_handle);

    /// Move construct and rebind the executor.
    template<typename Executor1>
    basic_process_handle(basic_process_handle<Executor1> &&handle);

    /// Get the id of the process
    pid_type id() const
    { return pid_; }

    /// Terminate the process if it's still running and ignore the result
    void terminate_if_running(error_code &);

    /// Throwing @overload void terminate_if_running(error_code & ec;
    void terminate_if_running();
    /// wait for the process to exit and store the exit code in exit_status.
    void wait(native_exit_code_type &exit_status, error_code &ec);
    /// Throwing @overload wait(native_exit_code_type &exit_code, error_code & ec)
    void wait(native_exit_code_type &exit_status);

    /// Sends the process a signal to ask for an interrupt, which the process may interpret as a shutdown.
    /** Maybe be ignored by the subprocess. */
    void interrupt(error_code &ec);

    /// Throwing @overload void interrupt()
    void interrupt();

    /// Sends the process a signal to ask for a graceful shutdown. Maybe be ignored by the subprocess.
    void request_exit(error_code &ec);

    /// Throwing @overload void request_exit(error_code & ec)
    void request_exit()

    /// Unconditionally terminates the process and stores the exit code in exit_status.
    void terminate(native_exit_code_type &exit_status, error_code &ec);\
    /// Throwing @overload void terminate(native_exit_code_type &exit_code, error_code & ec)
    void terminate(native_exit_code_type &exit_status);/

    /// Checks if the current process is running. 
    /**If it has already completed, it assigns the exit code to `exit_code`.
     */ 
    bool running(native_exit_code_type &exit_code, error_code &ec);
    /// Throwing @overload bool running(native_exit_code_type &exit_code, error_code & ec)
    bool running(native_exit_code_type &exit_code);

    /// Check if the process handle is referring to an existing process.
    bool is_open() const;

    /// Asynchronously wait for the process to exit and deliver the native exit-code in the completion handler.
    template<BOOST_PROCESS_V2_COMPLETION_TOKEN_FOR(void(error_code, native_exit_code_type))
             WaitHandler = net::default_completion_token_t<executor_type>>
    auto async_wait(WaitHandler &&handler = net::default_completion_token_t<executor_type>());
};


#else
#if defined(BOOST_PROCESS_V2_WINDOWS)
template<typename Executor = net::any_io_executor>
using basic_process_handle = detail::basic_process_handle_win<Executor>;
#else

#if defined(BOOST_PROCESS_V2_PIDFD_OPEN)
template<typename Executor = net::any_io_executor>
using basic_process_handle = detail::basic_process_handle_fd<Executor>;
#elif defined(BOOST_PROCESS_V2_PDFORK) || defined(BOOST_PROCESS_V2_PIPE_LAUNCHER)
template<typename Executor = net::any_io_executor>
using basic_process_handle = detail::basic_process_handle_fd_or_signal<Executor>;
#else

template<typename Executor = net::any_io_executor>
using basic_process_handle = detail::basic_process_handle_signal<Executor>;

#endif
#endif

/// Process handle with the default executor.
using process_handle = basic_process_handle<>;

#endif

BOOST_PROCESS_V2_END_NAMESPACE

#endif //BOOST_PROCESS_V2_PROCESS_HANDLE_HPP
