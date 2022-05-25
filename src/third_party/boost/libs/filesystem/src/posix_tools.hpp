//  posix_tools.hpp  -------------------------------------------------------------------//

//  Copyright 2021 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#ifndef BOOST_FILESYSTEM_SRC_POSIX_TOOLS_HPP_
#define BOOST_FILESYSTEM_SRC_POSIX_TOOLS_HPP_

#include "platform_config.hpp"
#include <cerrno>
#include <boost/filesystem/config.hpp>
#ifdef BOOST_HAS_UNISTD_H
#include <unistd.h>
#endif

namespace boost {
namespace filesystem {
namespace detail {

/*!
 * Closes a file descriptor and returns the result, similar to close(2). Unlike close(2), guarantees that the file descriptor is closed even if EINTR error happens.
 *
 * Some systems don't close the file descriptor in case if the thread is interrupted by a signal and close(2) returns EINTR.
 * Other (most) systems do close the file descriptor even when when close(2) returns EINTR, and attempting to close it
 * again could close a different file descriptor that was opened by a different thread. This function hides this difference in behavior.
 *
 * Future POSIX standards will likely fix this by introducing posix_close (see https://www.austingroupbugs.net/view.php?id=529)
 * and prohibiting returning EINTR from close(2), but we still have to support older systems where this new behavior is not available and close(2)
 * behaves differently between systems.
 */
inline int close_fd(int fd)
{
#if defined(hpux) || defined(_hpux) || defined(__hpux)
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

    return res;
#else
    return ::close(fd);
#endif
}

} // namespace detail
} // namespace filesystem
} // namespace boost

#endif // BOOST_FILESYSTEM_SRC_POSIX_TOOLS_HPP_
