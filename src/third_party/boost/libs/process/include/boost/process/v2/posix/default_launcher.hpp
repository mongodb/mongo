// Copyright (c) 2022 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PROCESS_V2_POSIX_DEFAULT_LAUNCHER
#define BOOST_PROCESS_V2_POSIX_DEFAULT_LAUNCHER

#include <boost/process/v2/detail/config.hpp>
#include <boost/process/v2/cstring_ref.hpp>
#include <boost/process/v2/posix/detail/close_handles.hpp>
#include <boost/process/v2/detail/throw_error.hpp>
#include <boost/process/v2/detail/utf8.hpp>

#if defined(BOOST_PROCESS_V2_STANDALONE)
#include <asio/execution/executor.hpp>
#include <asio/is_executor.hpp>
#include <asio/execution_context.hpp>
#include <asio/execution/context.hpp>
#include <asio/query.hpp>
#else
#include <boost/asio/execution/executor.hpp>
#include <boost/asio/is_executor.hpp>
#include <boost/asio/execution_context.hpp>
#include <boost/asio/execution/context.hpp>
#include <boost/asio/query.hpp>
#endif

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
# include <crt_externs.h>
# if !defined(environ)
#  define environ (*_NSGetEnviron())
# endif
#elif defined(__MACH__) || defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__sun)
 extern "C" { extern char **environ; }
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

template<typename Executor>
struct basic_process;

namespace posix
{


namespace detail
{

struct base {};
struct derived : base {};

template<typename Launcher, typename Init>
inline error_code invoke_on_setup(Launcher & /*launcher*/, const filesystem::path &/*executable*/,
                                  const char * const * (&/*cmd_line*/),
                                  Init && /*init*/, base && )
{
    return error_code{};
}

template<typename Launcher, typename Init>
inline auto invoke_on_setup(Launcher & launcher, const filesystem::path &executable,
                            const char * const * (&cmd_line),
                            Init && init, derived && )
    -> decltype(init.on_setup(launcher, executable, cmd_line))
{
    return init.on_setup(launcher, executable, cmd_line);
}

template<typename Launcher>
inline error_code on_setup(Launcher & /*launcher*/, const filesystem::path &/*executable*/,
                           const char * const * (&/*cmd_line*/))
{
    return error_code{};
}

template<typename Launcher, typename Init1, typename ... Inits>
inline error_code on_setup(Launcher & launcher, const filesystem::path &executable,
                           const char * const * (&cmd_line),
                           Init1 && init1, Inits && ... inits)
{
    auto ec = invoke_on_setup(launcher, executable, cmd_line, init1, derived{});
    if (ec)
        return ec;
    else
        return on_setup(launcher, executable, cmd_line, inits...);
}


template<typename Launcher, typename Init>
inline void invoke_on_error(Launcher & /*launcher*/, const filesystem::path &/*executable*/,
                            const char * const * (&/*cmd_line*/),
                            const error_code & /*ec*/, Init && /*init*/, base && )
{
}

template<typename Launcher, typename Init>
inline auto invoke_on_error(Launcher & launcher, const filesystem::path &executable,
                            const char * const * (&cmd_line),
                            const error_code & ec, Init && init, derived && )
-> decltype(init.on_error(launcher, executable, cmd_line, ec))
{
    init.on_error(launcher, executable, cmd_line, ec);
}

template<typename Launcher>
inline void on_error(Launcher & /*launcher*/, const filesystem::path &/*executable*/,
                     const char * const * (&/*cmd_line*/),
                     const error_code & /*ec*/)
{
}

template<typename Launcher, typename Init1, typename ... Inits>
inline void on_error(Launcher & launcher, const filesystem::path &executable,
                     const char * const * (&cmd_line),
                     const error_code & ec,
                     Init1 && init1, Inits && ... inits)
{
    invoke_on_error(launcher, executable, cmd_line, ec, init1, derived{});
    on_error(launcher, executable, cmd_line, ec, inits...);
}

template<typename Launcher, typename Init>
inline void invoke_on_success(Launcher & /*launcher*/, const filesystem::path &/*executable*/,
                              const char * const * (&/*cmd_line*/),
                              Init && /*init*/, base && )
{
}

template<typename Launcher, typename Init>
inline auto invoke_on_success(Launcher & launcher, const filesystem::path &executable,
                              const char * const * (&cmd_line),
                              Init && init, derived && )
-> decltype(init.on_success(launcher, executable, cmd_line))
{
    init.on_success(launcher, executable, cmd_line);
}

template<typename Launcher>
inline void on_success(Launcher & /*launcher*/, const filesystem::path &/*executable*/,
                       const char * const * (&/*cmd_line*/))
{
}

template<typename Launcher, typename Init1, typename ... Inits>
inline void on_success(Launcher & launcher, const filesystem::path &executable,
                       const char * const * (&cmd_line),
                       Init1 && init1, Inits && ... inits)
{
    invoke_on_success(launcher, executable, cmd_line, init1, derived{});
    on_success(launcher, executable, cmd_line, inits...);
}

template<typename Launcher, typename Init>
inline void invoke_on_fork_error(Launcher & /*launcher*/, const filesystem::path &/*executable*/,
                                 const char * const * (&/*cmd_line*/),
                                 const error_code & /*ec*/, Init && /*init*/, base && )
{
}

template<typename Launcher, typename Init>
inline auto invoke_on_fork_error(Launcher & launcher, const filesystem::path &executable,
                                 const char * const * (&cmd_line),
                                 const error_code & ec, Init && init, derived && )
-> decltype(init.on_fork_error(launcher, executable, cmd_line, ec))
{
    init.on_fork_error(launcher, executable, cmd_line, ec);
}

template<typename Launcher>
inline void on_fork_error(Launcher & /*launcher*/, const filesystem::path &/*executable*/,
                          const char * const * (&/*cmd_line*/),
                          const error_code & /*ec*/)
{
}

template<typename Launcher, typename Init1, typename ... Inits>
inline void on_fork_error(Launcher & launcher, const filesystem::path &executable,
                          const char * const * (&cmd_line),
                          const error_code & ec,
                          Init1 && init1, Inits && ... inits)
{
    invoke_on_fork_error(launcher, executable, cmd_line, ec, init1, derived{});
    on_fork_error(launcher, executable, cmd_line, ec, inits...);
}

template<typename Launcher, typename Init>
inline error_code invoke_on_exec_setup(Launcher & /*launcher*/, const filesystem::path &/*executable*/,
                                       const char * const * (&/*cmd_line*/),
                                       Init && /*init*/, base && )
{
    return error_code{};
}

template<typename Launcher, typename Init>
inline auto invoke_on_exec_setup(Launcher & launcher, const filesystem::path &executable,
                                 const char * const * (&cmd_line),
                                 Init && init, derived && )
-> decltype(init.on_exec_setup(launcher, executable, cmd_line))
{
    return init.on_exec_setup(launcher, executable, cmd_line);
}

template<typename Launcher>
inline error_code on_exec_setup(Launcher & /*launcher*/, const filesystem::path &/*executable*/,
                                const char * const * (&/*cmd_line*/))
{
    return error_code{};
}

template<typename Launcher, typename Init1, typename ... Inits>
inline error_code on_exec_setup(Launcher & launcher, const filesystem::path &executable,
                                const char * const * (&cmd_line),
                                Init1 && init1, Inits && ... inits)
{
    auto ec = invoke_on_exec_setup(launcher, executable, cmd_line, init1, derived{});
    if (ec)
        return ec;
    else
        return on_exec_setup(launcher, executable, cmd_line, inits...);
}



template<typename Launcher, typename Init>
inline void invoke_on_exec_error(Launcher & /*launcher*/, const filesystem::path &/*executable*/,
                                 const char * const * (&/*cmd_line*/),
                                 const error_code & /*ec*/, Init && /*init*/, base && )
{
}

template<typename Launcher, typename Init>
inline auto invoke_on_exec_error(Launcher & launcher, const filesystem::path &executable,
                                 const char * const * (&cmd_line),
                                 const error_code & ec, Init && init, derived && )
-> decltype(init.on_exec_error(launcher, executable, cmd_line, ec))
{
    init.on_exec_error(launcher, executable, cmd_line, ec);
}

template<typename Launcher>
inline void on_exec_error(Launcher & /*launcher*/, const filesystem::path &/*executable*/,
                          const char * const * (&/*cmd_line*/),
                          const error_code & /*ec*/)
{
}

template<typename Launcher, typename Init1, typename ... Inits>
inline void on_exec_error(Launcher & launcher, const filesystem::path &executable,
                          const char * const * (&cmd_line),
                          const error_code & ec,
                          Init1 && init1, Inits && ... inits)
{
    invoke_on_exec_error(launcher, executable, cmd_line, ec, init1, derived{});
    on_exec_error(launcher, executable, cmd_line, ec, inits...);
}
}

/// The default launcher for processes on windows.
struct default_launcher
{
    /// The pointer to the environment forwarded to the subprocess.
    const char * const * env = environ;
    /// The pid of the subprocess - will be assigned after fork.
    int pid = -1;

    /// The whitelist for file descriptors.
    std::vector<int> fd_whitelist = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};

    default_launcher() = default;

    template<typename ExecutionContext, typename Args, typename ... Inits>
    auto operator()(ExecutionContext & context,
                    const typename std::enable_if<std::is_convertible<
                            ExecutionContext&, net::execution_context&>::value,
                            filesystem::path >::type & executable,
                    Args && args,
                    Inits && ... inits ) -> basic_process<typename ExecutionContext::executor_type>
    {
        error_code ec;
        auto proc =  (*this)(context, ec, executable, std::forward<Args>(args), std::forward<Inits>(inits)...);

        if (ec)
            v2::detail::throw_error(ec, "default_launcher");

        return proc;
    }


    template<typename ExecutionContext, typename Args, typename ... Inits>
    auto operator()(ExecutionContext & context,
                    error_code & ec,
                    const typename std::enable_if<std::is_convertible<
                            ExecutionContext&, net::execution_context&>::value,
                            filesystem::path >::type & executable,
                    Args && args,
                    Inits && ... inits ) -> basic_process<typename ExecutionContext::executor_type>
    {
        return (*this)(context.get_executor(), ec, executable, std::forward<Args>(args), std::forward<Inits>(inits)...);
    }

    template<typename Executor, typename Args, typename ... Inits>
    auto operator()(Executor exec,
                    const typename std::enable_if<
                            net::execution::is_executor<Executor>::value ||
                            net::is_executor<Executor>::value,
                            filesystem::path >::type & executable,
                    Args && args,
                    Inits && ... inits ) -> basic_process<Executor>
    {
        error_code ec;
        auto proc =  (*this)(std::move(exec), ec, executable, std::forward<Args>(args), std::forward<Inits>(inits)...);

        if (ec)
            v2::detail::throw_error(ec, "default_launcher");

        return proc;
    }

    template<typename Executor, typename Args, typename ... Inits>
    auto operator()(Executor exec,
                    error_code & ec,
                    const typename std::enable_if<
                            net::execution::is_executor<Executor>::value ||
                            net::is_executor<Executor>::value,
                            filesystem::path >::type & executable,
                    Args && args,
                    Inits && ... inits ) -> basic_process<Executor>
    {
        auto argv = this->build_argv_(executable, std::forward<Args>(args));
        {
            pipe_guard pg;
            if (::pipe(pg.p))
            {
                BOOST_PROCESS_V2_ASSIGN_EC(ec, errno, system_category());
                return basic_process<Executor>{exec};
            }
            if (::fcntl(pg.p[1], F_SETFD, FD_CLOEXEC))
            {
                BOOST_PROCESS_V2_ASSIGN_EC(ec, errno, system_category());
                return basic_process<Executor>{exec};
            }
            ec = detail::on_setup(*this, executable, argv, inits ...);
            if (ec)
            {
                detail::on_error(*this, executable, argv, ec, inits...);
                return basic_process<Executor>(exec);
            }
            fd_whitelist.push_back(pg.p[1]);

            auto & ctx = net::query(
                    exec, net::execution::context);
#if !defined(BOOST_PROCESS_V2_DISABLE_NOTIFY_FORK)
            ctx.notify_fork(net::execution_context::fork_prepare);
#endif
            pid = ::fork();
            if (pid == -1)
            {
#if !defined(BOOST_PROCESS_V2_DISABLE_NOTIFY_FORK)
                ctx.notify_fork(net::execution_context::fork_parent);
#endif
                detail::on_fork_error(*this, executable, argv, ec, inits...);
                detail::on_error(*this, executable, argv, ec, inits...);

                BOOST_PROCESS_V2_ASSIGN_EC(ec, errno, system_category());
                return basic_process<Executor>{exec};
            }
            else if (pid == 0)
            {
                ::close(pg.p[0]);
#if !defined(BOOST_PROCESS_V2_DISABLE_NOTIFY_FORK)
                ctx.notify_fork(net::execution_context::fork_child);
#endif
                ec = detail::on_exec_setup(*this, executable, argv, inits...);
                if (!ec)
                {
                    close_all_fds(ec);
                }                
                if (!ec)
                    ::execve(executable.c_str(), const_cast<char * const *>(argv), const_cast<char * const *>(env));

                ignore_unused(::write(pg.p[1], &errno, sizeof(int)));
                BOOST_PROCESS_V2_ASSIGN_EC(ec, errno, system_category());
                detail::on_exec_error(*this, executable, argv, ec, inits...);
                ::exit(EXIT_FAILURE);
                return basic_process<Executor>{exec};
            }
#if !defined(BOOST_PROCESS_V2_DISABLE_NOTIFY_FORK)
            ctx.notify_fork(net::execution_context::fork_parent);
#endif
            ::close(pg.p[1]);
            pg.p[1] = -1;
            int child_error{0};
            int count = -1;
            while ((count = ::read(pg.p[0], &child_error, sizeof(child_error))) == -1)
            {
                int err = errno;
                if ((err != EAGAIN) && (err != EINTR))
                {
                    BOOST_PROCESS_V2_ASSIGN_EC(ec, err, system_category());
                    break;
                }
            }
            if (count != 0)
                BOOST_PROCESS_V2_ASSIGN_EC(ec, child_error, system_category());

            if (ec)
            {
                detail::on_error(*this, executable, argv, ec, inits...);
                do { ::waitpid(pid, nullptr, 0); } while (errno == EINTR);
                return basic_process<Executor>{exec};
            }
        }
        basic_process<Executor> proc(exec, pid);
        detail::on_success(*this, executable, argv, ec, inits...);
        return proc;

    }
  protected:

    void ignore_unused(std::size_t ) {}
    void close_all_fds(error_code & ec)
    {
        std::sort(fd_whitelist.begin(), fd_whitelist.end());
        detail::close_all(fd_whitelist, ec);
        fd_whitelist = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};
    }

    struct pipe_guard
    {
        int p[2];
        pipe_guard() : p{-1,-1} {}

        ~pipe_guard()
        {
            if (p[0] != -1)
                ::close(p[0]);
            if (p[1] != -1)
                ::close(p[1]);
        }
    };

    //if we need to allocate something
    std::vector<std::string> argv_buffer_;
    std::vector<const char *> argv_;

    template<typename Args>
    const char * const * build_argv_(const filesystem::path & pt, const Args & args,
                                     typename std::enable_if<
                                             std::is_convertible<
                                                     decltype(*std::begin(std::declval<Args>())),
                                                     cstring_ref>::value>::type * = nullptr)
    {
        const auto arg_cnt = std::distance(std::begin(args), std::end(args));
        argv_.reserve(arg_cnt + 2);
        argv_.push_back(pt.native().data());
        for (auto && arg : args)
            argv_.push_back(arg.c_str());

        argv_.push_back(nullptr);
        return argv_.data();
    }

    const char * const * build_argv_(const filesystem::path &, const char ** argv)
    {
        return argv;
    }

    template<typename Args>
    const char * const *  build_argv_(const filesystem::path & pt, const Args & args,
                                      typename std::enable_if<
                                              !std::is_convertible<
                                                      decltype(*std::begin(std::declval<Args>())),
                                                      cstring_ref>::value>::type * = nullptr)
    {
        const auto arg_cnt = std::distance(std::begin(args), std::end(args));
        argv_.reserve(arg_cnt + 2);
        argv_buffer_.reserve(arg_cnt);
        argv_.push_back(pt.native().data());

        using char_type = typename decay<decltype((*std::begin(std::declval<Args>()))[0])>::type;

        for (basic_string_view<char_type>  arg : args)
            argv_buffer_.push_back(v2::detail::conv_string<char>(arg.data(), arg.size()));

        for (auto && arg : argv_buffer_)
            argv_.push_back(arg.c_str());

        argv_.push_back(nullptr);
        return argv_.data();
    }
};


}

BOOST_PROCESS_V2_END_NAMESPACE

#endif //BOOST_PROCESS_V2_POSIX_DEFAULT_LAUNCHER
