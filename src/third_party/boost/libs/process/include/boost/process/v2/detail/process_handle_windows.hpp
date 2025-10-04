// Copyright (c) 2022 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PROCESS_V2_DETAIL_PROCESS_HANDLE_WINDOWS_HPP
#define BOOST_PROCESS_V2_DETAIL_PROCESS_HANDLE_WINDOWS_HPP

#include <boost/process/v2/detail/config.hpp>

#include <boost/process/v2/exit_code.hpp>
#include <boost/process/v2/pid.hpp>
#include <boost/process/v2/detail/throw_error.hpp>

#if defined(BOOST_PROCESS_V2_STANDALONE)
#include <asio/any_io_executor.hpp>
#include <asio/compose.hpp>
#include <asio/windows/basic_object_handle.hpp>
#else
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/compose.hpp>
#include <boost/asio/windows/basic_object_handle.hpp>
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace detail
{
  
BOOST_PROCESS_V2_DECL void get_exit_code_( void * handle, native_exit_code_type & exit_code, error_code & ec);
BOOST_PROCESS_V2_DECL void * open_process_(pid_type pid);
BOOST_PROCESS_V2_DECL void terminate_if_running_(void * handle);
BOOST_PROCESS_V2_DECL bool check_handle_(void* handle, error_code & ec);
BOOST_PROCESS_V2_DECL bool check_pid_(pid_type pid_, error_code & ec);
BOOST_PROCESS_V2_DECL void interrupt_(pid_type pid_, error_code & ec);
BOOST_PROCESS_V2_DECL void suspend_(void * handle, error_code & ec);
BOOST_PROCESS_V2_DECL void resume_(void * handle, error_code & ec);
BOOST_PROCESS_V2_DECL void terminate_(void * handle, error_code & ec, native_exit_code_type & exit_code);
BOOST_PROCESS_V2_DECL void request_exit_(pid_type pid_, error_code & ec);
BOOST_PROCESS_V2_DECL void check_running_(void* handle, error_code & ec, native_exit_code_type & exit_status);

template<typename Executor = net::any_io_executor>
struct basic_process_handle_win
{
    typedef net::windows::basic_object_handle<Executor> handle_type;
    typedef typename handle_type::native_handle_type native_handle_type;

    typedef Executor executor_type;

    executor_type get_executor()
    { return handle_.get_executor(); }

    /// Rebinds the process_handle to another executor.
    template<typename Executor1>
    struct rebind_executor
    {
        /// The socket type when rebound to the specified executor.
        typedef basic_process_handle_win<Executor1> other;
    };

    template<typename ExecutionContext>
    basic_process_handle_win(ExecutionContext &context,
                         typename std::enable_if<
                                 std::is_convertible<ExecutionContext &,
                                         net::execution_context &>::value
                         >::type = 0)
            : pid_(0), handle_(context)
    {
    }

    basic_process_handle_win(Executor executor)
            : pid_(0), handle_(executor)
    {
    }

    basic_process_handle_win(Executor executor, pid_type pid)
            : pid_(pid), handle_(executor, detail::open_process_(pid))
    {
    }

    basic_process_handle_win(Executor executor, pid_type pid, native_handle_type process_handle)
            : pid_(pid), handle_(executor, process_handle)
    {
    }


    template<typename Executor1>
    basic_process_handle_win(basic_process_handle_win<Executor1> && other)
            : pid_(other.pid_), handle_(std::move(other.handle_))
    {
      other.pid_ = static_cast<DWORD>(-1);
    }

    basic_process_handle_win(basic_process_handle_win && other)
        :  pid_(other.pid_), handle_(std::move(other.handle_))
    {
      other.pid_ = static_cast<DWORD>(-1);
    }

    basic_process_handle_win& operator=(basic_process_handle_win && other)
    {
        pid_ = other.pid_;
        handle_ = std::move(other.handle_);
        other.pid_ = static_cast<DWORD>(-1);
        return *this;
    }

    template<typename Executor1>
    basic_process_handle_win& operator=(basic_process_handle_win<Executor1> && other)
    {
        pid_ = other.pid_;
        handle_ = std::move(other.handle_);
        other.pid_ = static_cast<DWORD>(-1);
        return *this;
    }

    ~basic_process_handle_win()
    {
        if (handle_.is_open())
        {
            error_code ec;
            handle_.close(ec);
        }
    }

    native_handle_type native_handle()
    { return handle_.native_handle(); }

    pid_type id() const
    { return pid_; }

    void terminate_if_running(error_code &)
    {
        detail::terminate_if_running_(handle_.native_handle());
    }

    void terminate_if_running()
    {
        detail::terminate_if_running_(handle_.native_handle());
    }

    void wait(native_exit_code_type &exit_status, error_code &ec)
    {
        if (!detail::check_handle_(handle_.native_handle(), ec))
            return;

        handle_.wait(ec);
        if (!ec)
            detail::get_exit_code_(handle_.native_handle(), exit_status, ec);
    }


    void wait(native_exit_code_type &exit_status)
    {
        error_code ec;
        wait(exit_status, ec);
        if (ec)
            detail::throw_error(ec, "wait(pid)");
    }

    void interrupt(error_code &ec)
    {
        if (!detail::check_pid_(pid_, ec))
            return;

        detail::interrupt_(pid_, ec);
    }

    void interrupt()
    {
        error_code ec;
        interrupt(ec);
        if (ec)
            detail::throw_error(ec, "interrupt");
    }

    void request_exit(error_code &ec)
    {
        if (!detail::check_pid_(pid_, ec))
            return;
        detail::request_exit_(pid_, ec);
    }

    void request_exit()
    {
        error_code ec;
        request_exit(ec);
        if (ec)
            detail::throw_error(ec, "request_exit");
    }

    void suspend(error_code &ec)
    {
        detail::suspend_(handle_.native_handle(), ec);
    }

    void suspend()
    {
        error_code ec;
        suspend(ec);
        if (ec)
            detail::throw_error(ec, "suspend");
    }

    void resume(error_code &ec)
    {
        detail::resume_(handle_.native_handle(), ec);
    }

    void resume()
    {
        error_code ec;
        resume(ec);
        if (ec)
            detail::throw_error(ec, "resume");
    }

    void terminate(native_exit_code_type &exit_status, error_code &ec)
    {
        if (!detail::check_handle_(handle_.native_handle(), ec))
            return;

        detail::terminate_(handle_.native_handle(), ec, exit_status);
        if (!ec)
            wait(exit_status, ec);

    }

    void terminate(native_exit_code_type &exit_status)
    {
        error_code ec;
        terminate(exit_status, ec);
        if (ec)
            detail::throw_error(ec, "terminate");
    }

    bool running(native_exit_code_type &exit_code, error_code & ec)
    {
        if (!detail::check_handle_(handle_.native_handle(), ec))
            return false;

        native_exit_code_type code;
        //single value, not needed in the winapi.
        detail::check_running_(handle_.native_handle(), ec, code);
        if (ec)
            return false;

        if (process_is_running(code))
            return true;
        else
            exit_code = code;

        return false;
    }

    bool running(native_exit_code_type &exit_code)
    {
        error_code ec;
        bool res = running(exit_code, ec);
        if (ec)
            detail::throw_error(ec, "is_running");
        return res;
    }

    bool is_open() const
    {
        return handle_.is_open();
    }

    template<typename>
    friend struct basic_process_handle_win;
  private:
    pid_type pid_;
    handle_type handle_;

    struct async_wait_op_
    {
        handle_type &handle;

        template<typename Self>
        void operator()(Self &&self)
        {

            self.reset_cancellation_state(asio::enable_total_cancellation());
            auto sl = self.get_cancellation_state().slot();
            auto & h = handle;
            if (sl.is_connected())
                sl.assign(
                    [&h](asio::cancellation_type ct)
                    {
                      error_code ec;
                      h.cancel(ec);
                    });
            handle.async_wait(std::move(self));
        }

        template<typename Self>
        void operator()(Self &&self, error_code ec)
        {
            native_exit_code_type exit_code{};
            if (ec == asio::error::operation_aborted && !self.get_cancellation_state().cancelled())
              return handle.async_wait(std::move(self));

            if (!ec)
                detail::get_exit_code_(handle.native_handle(), exit_code, ec);
            std::move(self).complete(ec, exit_code);
        }
    };
 public:
    template<BOOST_PROCESS_V2_COMPLETION_TOKEN_FOR(void(error_code, native_exit_code_type))
             WaitHandler = net::default_completion_token_t<executor_type>>
    auto async_wait(WaitHandler &&handler = net::default_completion_token_t<executor_type>())
        -> decltype(net::async_compose<WaitHandler, void(error_code, native_exit_code_type)>(
                    async_wait_op_{handle_}, handler, handle_))
    {
        return net::async_compose<WaitHandler, void(error_code, native_exit_code_type)>(
                async_wait_op_{handle_}, handler, handle_
        );
    }
};

}

BOOST_PROCESS_V2_END_NAMESPACE


#endif //BOOST_PROCESS_V2_DETAIL_PROCESS_HANDLE_WINDOWS_HPP
