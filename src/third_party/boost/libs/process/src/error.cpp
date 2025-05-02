// Copyright (c) 2021 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <boost/process/v2/detail/config.hpp>

#if defined(BOOST_PROCESS_V2_WINDOWS)
#include <boost/asio/windows/basic_object_handle.hpp>
#endif


#include <boost/process/v2/error.hpp>
#include <boost/process/v2/exit_code.hpp>

#include <cstdlib>

#if defined(BOOST_PROCESS_V2_POSIX)
#include <sys/wait.h>
#endif
BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace error
{

namespace detail
{

struct utf8_category final : public error_category
{
    utf8_category() : error_category(0xDAEDu) {}

    const char* name() const noexcept
    {
        return "process.v2.utf8";
    }
    std::string message(int value) const
    {
        switch (static_cast<utf8_conv_error>(value))
        {
            case utf8_conv_error::insufficient_buffer:
                return "A supplied buffer size was not large enough";
            case utf8_conv_error::invalid_character:
                return "Invalid characters were found in a string.";
            default:
                return "process.v2.utf8 error";
        }
    }
};


struct exit_code_category final : public error_category
{
    exit_code_category() : error_category(0xDAEEu) {}

    const char* name() const noexcept
    {
        return "process.v2.exit_code";
    }
    std::string message(int status) const
    {
        switch (evaluate_exit_code(status))
        {
            case v2::detail::still_active:
                return "still-active";
            case EXIT_SUCCESS:
                return "exit_success";
            case EXIT_FAILURE:
                return "exit_failure";
            default:
#if defined(BOOST_PROCESS_V2_POSIX)
                if (WIFCONTINUED(status))
                    return "continued";
                switch (WTERMSIG(status))
                {
#            if defined(SIGABRT)
                    case SIGABRT: return "SIGABRT:   Abort signal from abort(3)";
#            endif
#            if defined(SIGALRM)
                    case SIGALRM: return "SIGALRM:   Timer signal from alarm(2)";
#            endif
#            if defined(SIGBUS)
                    case SIGBUS: return "SIGBUS:    Bus error (bad memory access)";
#            endif
#            if defined(SIGCHLD)
                    case SIGCHLD: return "SIGCHLD:   Child stopped or terminated";
#            endif
#            if defined(SIGCONT)
                    case SIGCONT: return "SIGCONT:   Continue if stopped";
#            endif
#            if defined(SIGEMT)
                    case SIGEMT: return "SIGEMT:    Emulator trap";
#            endif
#            if defined(SIGFPE)
                    case SIGFPE: return "SIGFPE:    Floating-point exception";
#            endif
#            if defined(SIGHUP)
                    case SIGHUP: return "SIGHUP:    Hangup detected on controlling terminal";
#            endif
#            if defined(SIGILL)
                    case SIGILL: return "SIGILL:    Illegal Instruction";
#            endif
#            if defined(SIGINFO)
                    case SIGINFO: return "SIGINFO:   A synonym for SIGPWR";
#            endif
#            if defined(SIGINT)
                    case SIGINT: return "SIGINT:    Interrupt from keyboard";
#            endif
#            if defined(SIGIO)
                    case SIGIO: return "SIGIO:     I/O now possible (4.2BSD)";
#            endif
#            if defined(SIGKILL)
                    case SIGKILL: return "SIGKILL:   Kill signal";
#            endif
#            if defined(SIGLOST)
                    case SIGLOST: return "SIGLOST:   File lock lost (unused)";
#            endif
#            if defined(SIGPIPE)
                    case SIGPIPE: return "SIGPIPE:   Broken pipe: write to pipe with no";
#            endif
#            if defined(SIGPOLL) && !defined(SIGIO)
                    case SIGPOLL: return "SIGPOLL:   Pollable event (Sys V);";
#            endif
#            if defined(SIGPROF)
                    case SIGPROF: return "SIGPROF:   Profiling timer expired";
#            endif
#            if defined(SIGPWR)
                    case SIGPWR: return "SIGPWR:    Power failure (System V)";
#            endif
#            if defined(SIGQUIT)
                    case SIGQUIT: return "SIGQUIT:   Quit from keyboard";
#            endif
#            if defined(SIGSEGV)
                    case SIGSEGV: return "SIGSEGV:   Invalid memory reference";
#            endif
#            if defined(SIGSTKFLT)
                    case SIGSTKFLT: return "SIGSTKFLT: Stack fault on coprocessor (unused)";
#            endif
#            if defined(SIGSTOP)
                    case SIGSTOP: return "SIGSTOP:   Stop process";
#            endif
#            if defined(SIGTSTP)
                    case SIGTSTP: return "SIGTSTP:   Stop typed at terminal";
#            endif
#            if defined(SIGSYS)
                    case SIGSYS: return "SIGSYS:    Bad system call (SVr4);";
#            endif
#            if defined(SIGTERM)
                    case SIGTERM: return "SIGTERM:   Termination signal";
#            endif
#            if defined(SIGTRAP)
                    case SIGTRAP: return "SIGTRAP:   Trace/breakpoint trap";
#            endif
#            if defined(SIGTTIN)
                    case SIGTTIN: return "SIGTTIN:   Terminal input for background process";
#            endif
#            if defined(SIGTTOU)
                    case SIGTTOU: return "SIGTTOU:   Terminal output for background process";
#            endif
#            if defined(SIGURG)
                    case SIGURG: return "SIGURG:    Urgent condition on socket (4.2BSD)";
#            endif
#            if defined(SIGUSR1)
                    case SIGUSR1: return "SIGUSR1:   User-defined signal 1";
#            endif
#            if defined(SIGUSR2)
                    case SIGUSR2: return "SIGUSR2:   User-defined signal 2";
#            endif
#            if defined(SIGVTALRM)
                    case SIGVTALRM: return "SIGVTALRM: Virtual alarm clock (4.2BSD)";
#            endif
#            if defined(SIGXCPU)
                    case SIGXCPU: return "SIGXCPU:   CPU time limit exceeded (4.2BSD);";
#            endif
#            if defined(SIGXFSZ)
                    case SIGXFSZ: return "SIGXFSZ:   File size limit exceeded (4.2BSD);";
#            endif
#            if defined(SIGWINCH)
                    case SIGWINCH: return "SIGWINCH:  Window resize signal (4.3BSD, Sun)";
#            endif
                    default: return "Unknown signal";
                }
#endif
                return "exited with other error";
        }
    }
};




} // namespace detail

BOOST_PROCESS_V2_DECL const error_category& get_utf8_category()
{
    static detail::utf8_category instance;
    return instance;
}

BOOST_PROCESS_V2_DECL const error_category& get_exit_code_category()
{
    static detail::exit_code_category instance;
    return instance;
}

}

BOOST_PROCESS_V2_END_NAMESPACE
