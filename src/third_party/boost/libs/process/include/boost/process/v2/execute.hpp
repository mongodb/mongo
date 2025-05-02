// Copyright (c) 2022 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PROCESS_V2_EXECUTE_HPP
#define BOOST_PROCESS_V2_EXECUTE_HPP

#include <boost/process/v2/process.hpp>

#if defined(BOOST_PROCESS_V2_STANDALONE)
#include <asio/bind_cancellation_slot.hpp>
#else
#include <boost/asio/bind_cancellation_slot.hpp>
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE


/**
 * @brief Run a process and wait for it to complete.
 * 
 * @tparam Executor The asio executor of the process handle
 * @param proc The process to be run.
 * @return int The exit code of the process
 * @exception system_error An error that might have occurred during the wait.
 */
template<typename Executor>
inline int execute(basic_process<Executor> proc)
{
    return proc.wait();
}

/** \overload int execute(const basic_process<Executor> proc) */
template<typename Executor>
inline int execute(basic_process<Executor> proc, error_code & ec)
{
    return proc.wait(ec);
}

namespace detail
{

template<typename Executor>
struct execute_op
{
    std::unique_ptr<basic_process<Executor>> proc;

    struct cancel
    {
        using cancellation_type = net::cancellation_type;
        basic_process<Executor> * proc;
        cancel(basic_process<Executor> * proc) : proc(proc) {}

        void operator()(cancellation_type tp)
        {
            error_code ign;
            if ((tp & cancellation_type::total) != cancellation_type::none)
                proc->interrupt(ign);
            else if ((tp & cancellation_type::partial) != cancellation_type::none)
                proc->request_exit(ign);
            else if ((tp & cancellation_type::terminal) != cancellation_type::none)
                proc->terminate(ign);
        }
    };

    template<typename Self>
    void operator()(Self && self)
    {
        self.reset_cancellation_state(net::enable_total_cancellation());
        net::cancellation_slot s = self.get_cancellation_state().slot();
        if (s.is_connected())
            s.emplace<cancel>(proc.get());

        auto pro_ = proc.get();
        pro_->async_wait(
                net::bind_cancellation_slot(
                    net::cancellation_slot(),
                    std::move(self)));
    }

    template<typename Self>
    void operator()(Self && self, error_code ec, int res)
    { 
        self.get_cancellation_state().slot().clear();
        self.complete(ec, res);
    }
};

}

/// Execute a process asynchronously
/** This function asynchronously for a process to complete.
 * 
 * Cancelling the execution will signal the child process to exit
 * with the following interpretations:
 * 
 *  - cancellation_type::total    -> interrupt
 *  - cancellation_type::partial  -> request_exit
 *  - cancellation_type::terminal -> terminate
 * 
 * It is to note that `async_execute` will us the lowest selected cancellation
 * type. A subprocess might ignore anything not terminal.
 */
template<typename Executor = net::any_io_executor,
        BOOST_PROCESS_V2_COMPLETION_TOKEN_FOR(void (error_code, int))
            WaitHandler = net::default_completion_token_t<Executor>>
inline
auto async_execute(basic_process<Executor> proc,
                         WaitHandler && handler = net::default_completion_token_t<Executor>())
   -> decltype(net::async_compose<WaitHandler, void(error_code, int)>(
                  detail::execute_op<Executor>{nullptr}, handler, std::declval<Executor>()))
{
    std::unique_ptr<basic_process<Executor>> pro_(new basic_process<Executor>(std::move(proc)));
    auto exec = pro_->get_executor();
    return net::async_compose<WaitHandler, void(error_code, int)>(
            detail::execute_op<Executor>{std::move(pro_)}, handler, exec);
}

BOOST_PROCESS_V2_END_NAMESPACE

#endif //BOOST_PROCESS_V2_EXECUTE_HPP
