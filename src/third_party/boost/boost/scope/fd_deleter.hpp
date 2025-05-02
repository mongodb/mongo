/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2023 Andrey Semashev
 */
/*!
 * \file scope/fd_deleter.hpp
 *
 * This header contains definition of a deleter function object for
 * POSIX-like file descriptors for use with \c unique_resource.
 */

#ifndef BOOST_SCOPE_FD_DELETER_HPP_INCLUDED_
#define BOOST_SCOPE_FD_DELETER_HPP_INCLUDED_

#include <boost/scope/detail/config.hpp>

#if !defined(BOOST_WINDOWS)
#include <unistd.h>
#if defined(hpux) || defined(_hpux) || defined(__hpux)
#include <cerrno>
#endif
#else // !defined(BOOST_WINDOWS)
#include <io.h>
#endif // !defined(BOOST_WINDOWS)

#include <boost/scope/detail/header.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace scope {

//! POSIX-like file descriptor deleter
struct fd_deleter
{
    using result_type = void;

    //! Closes the file descriptor
    result_type operator() (int fd) const noexcept
    {
#if !defined(BOOST_WINDOWS)
#if defined(hpux) || defined(_hpux) || defined(__hpux)
        // Some systems don't close the file descriptor in case if the thread is interrupted by a signal and close(2) returns EINTR.
        // Other (most) systems do close the file descriptor even when when close(2) returns EINTR, and attempting to close it
        // again could close a different file descriptor that was opened by a different thread.
        //
        // Future POSIX standards will likely fix this by introducing posix_close (see https://www.austingroupbugs.net/view.php?id=529)
        // and prohibiting returning EINTR from close(2), but we still have to support older systems where this new behavior is not available and close(2)
        // behaves differently between systems.
        int res;
        while (true)
        {
            res = ::close(fd);
            if (BOOST_UNLIKELY(res < 0))
            {
                int err = errno;
                if (err == EINTR)
                    continue;
            }

            break;
        }
#else
        ::close(fd);
#endif
#else // !defined(BOOST_WINDOWS)
        ::_close(fd);
#endif // !defined(BOOST_WINDOWS)
    }
};

} // namespace scope
} // namespace boost

#include <boost/scope/detail/footer.hpp>

#endif // BOOST_SCOPE_FD_DELETER_HPP_INCLUDED_
