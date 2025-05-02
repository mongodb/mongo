// Copyright (c) 2022 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PROCESS_V2_POSIX_PDFORK_LAUNCHER_HPP
#define BOOST_PROCESS_V2_POSIX_PDFORK_LAUNCHER_HPP

#include <boost/process/v2/posix/default_launcher.hpp>

#include <unistd.h>
#include <sys/procdesc.h>

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace posix
{

/// A launcher using `pdfork`. Default on FreeBSD
struct pdfork_launcher : default_launcher
{
    /// The file descriptor of the subprocess. Set after fork.
    int fd;
    pdfork_launcher() = default;

    template<typename ExecutionContext, typename Args, typename ... Inits>
    auto operator()(ExecutionContext & context,
                    const typename std::enable_if<is_convertible<
                            ExecutionContext&, net::execution_context&>::value,
                            filesystem::path >::type & executable,
                    Args && args,
                    Inits && ... inits ) -> basic_process<typename ExecutionContext::executor_type>
    {
        error_code ec;
        auto proc =  (*this)(context, ec, executable, std::forward<Args>(args), std::forward<Inits>(inits)...);

        if (ec)
            v2::detail::throw_error(ec, "pdfork_launcher");

        return proc;
    }


    template<typename ExecutionContext, typename Args, typename ... Inits>
    auto operator()(ExecutionContext & context,
                    error_code & ec,
                    const typename std::enable_if<is_convertible<
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
            v2::detail::throw_error(ec, "pdfork_launcher");

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
            pid = ::pdfork(&fd, PD_DAEMON | PD_CLOEXEC);
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
#if !defined(BOOST_PROCESS_V2_DISABLE_NOTIFY_FORK)
                ctx.notify_fork(net::execution_context::fork_child);
#endif
                ::close(pg.p[0]);

                ec = detail::on_exec_setup(*this, executable, argv, inits...);
                if (!ec)
                {
                    close_all_fds(ec);
                }                
                if (!ec)
                    ::execve(executable.c_str(), const_cast<char * const *>(argv), const_cast<char * const *>(env));

                default_launcher::ignore_unused(::write(pg.p[1], &errno, sizeof(int)));
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
        basic_process<Executor> proc(exec, pid, fd);
        detail::on_success(*this, executable, argv, ec, inits...);
        return proc;
    }
};


}

BOOST_PROCESS_V2_END_NAMESPACE


#endif //BOOST_PROCESS_V2_POSIX_PDFORK_LAUNCHER_HPP
