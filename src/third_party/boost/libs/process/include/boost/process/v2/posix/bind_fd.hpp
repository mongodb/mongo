// Copyright (c) 2022 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PROCESS_V2_POSIX_BIND_FD_HPP
#define BOOST_PROCESS_V2_POSIX_BIND_FD_HPP

#include <boost/process/v2/detail/config.hpp>
#include <boost/process/v2/default_launcher.hpp>

#include <fcntl.h>

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace posix
{

/// Utility class to bind a file descriptor to an explicit file descriptor for the child process.
struct bind_fd
{
    int target;
    int fd;
    bool fd_needs_closing{false};

    ~bind_fd()
    {
        if (fd_needs_closing)
            ::close(fd);
    }

    bind_fd() = delete;
    /// Inherit file descriptor with the same value.
    /**
     * This will pass descriptor 42 as 42 to the child process:
     * @code
     * process p{"test", {},  posix::bind_fd(42)};
     * @endcode
     */
    bind_fd(int target) : target(target), fd(target) {}

    /// Inherit an asio io-object as a given file descriptor to the child process.
    /**
     * This will pass the tcp::socket, as 42 to the child process:
     * @code
     * extern tcp::socket sock; 
     * process p{"test", {},  posix::bind_fd(42, sock)};
     * @endcode
     */

    template<typename Stream>
    bind_fd(int target, Stream && str, decltype(std::declval<Stream>().native_handle()) = -1)
        : bind_fd(target, str.native_handle())
    {}

    /// Inherit a `FILE` as a given file descriptor to the child process.
    /**
     * This will pass the given `FILE*`, as 42 to the child process:
     * @code
     * process p{"test", {},  posix::bind_fd(42, stderr)};
     * @endcode
     */
    bind_fd(int target, FILE * f) : bind_fd(target, fileno(f)) {}
    
    /// Inherit a file descriptor with as a different value.
    /**
     * This will pass 24 as 42 to the child process:
     * @code
     * process p{"test", {},  posix::bind_fd(42, 24)};
     * @endcode
     */
    bind_fd(int target, int fd) : target(target), fd(fd) {}
    
    /// Inherit a null device as a set descriptor.
    /**
     * This will pass 24 as 42 to the child process:
     * @code
     * process p{"test", {},  posix::bind_fd(42, nullptr)};
     * @endcode
     */
    bind_fd(int target, std::nullptr_t) : bind_fd(target, filesystem::path("/dev/null")) {}

    /// Inherit a newly opened-file as a set descriptor.
    /**
     * This will pass 24 as 42 to the child process:
     * @code
     * process p{"test", {},  posix::bind_fd(42, "extra-output.txt")};
     * @endcode
     */
    bind_fd(int target, const filesystem::path & pth, int flags = O_RDWR | O_CREAT)
            : target(target), fd(::open(pth.c_str(), flags, 0660)), fd_needs_closing(true)
    {
    }

    error_code on_setup(posix::default_launcher & launcher, const filesystem::path &, const char * const *)
    {
        launcher.fd_whitelist.push_back(target);
        return {};
    }

    /// Implementation of the initialization function.
    error_code on_exec_setup(posix::default_launcher & /*launcher*/, const filesystem::path &, const char * const *)
    {
        if (::dup2(fd, target) == -1)
            return error_code(errno, system_category());
        return error_code ();
    }
};

}

BOOST_PROCESS_V2_END_NAMESPACE

#endif //BOOST_PROCESS_V2_POSIX_BIND_FD_HPP
