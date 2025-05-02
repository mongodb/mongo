// Copyright (c) 2022 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PROCESS_V2_POSIX_FORK_AND_FORGET_LAUNCHER_HPP
#define BOOST_PROCESS_V2_POSIX_FORK_AND_FORGET_LAUNCHER_HPP

#include <boost/process/v2/posix/default_launcher.hpp>

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace posix
{

/// A posix fork launcher that ignores errors after `fork`.
struct fork_and_forget_launcher : default_launcher
{
    fork_and_forget_launcher() = default;

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
            v2::detail::throw_error(ec, "fork_and_forget_launcher");

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
            v2::detail::throw_error(ec, "fork_and_forget_launcher");

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
            ec = detail::on_setup(*this, executable, argv, inits ...);
            if (ec)
            {
                detail::on_error(*this, executable, argv, ec, inits...);
                return basic_process<Executor>(exec);
            }

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
                ec = detail::on_exec_setup(*this, executable, argv, inits...);
                if (!ec)
                    close_all_fds(ec);
                if (!ec)
                    ::execve(executable.c_str(), const_cast<char * const *>(argv), const_cast<char * const *>(env));

                BOOST_PROCESS_V2_ASSIGN_EC(ec, errno, system_category());
                detail::on_exec_error(*this, executable, argv, ec, inits...);
                ::exit(EXIT_FAILURE);
                return basic_process<Executor>{exec};
            }
#if !defined(BOOST_PROCESS_V2_DISABLE_NOTIFY_FORK)
            ctx.notify_fork(net::execution_context::fork_parent);
#endif
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
};


}

BOOST_PROCESS_V2_END_NAMESPACE


#endif //BOOST_PROCESS_V2_POSIX_FORK_AND_FORGET_LAUNCHER_HPP
