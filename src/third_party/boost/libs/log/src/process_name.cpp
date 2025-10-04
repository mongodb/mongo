/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   process_name.cpp
 * \author Andrey Semashev
 * \date   29.07.2012
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 *
 * The code in this file is based on information on this page:
 *
 * http://stackoverflow.com/questions/1023306/finding-current-executables-path-without-proc-self-exe
 */

#include <boost/log/detail/config.hpp>
#include <climits> // PATH_MAX
#include <boost/log/attributes/current_process_name.hpp>
#include <boost/filesystem/path.hpp>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#if defined(BOOST_WINDOWS)

#include <windows.h>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

//! The function returns the current process name
BOOST_LOG_API std::string get_process_name()
{
    std::wstring buf;
    buf.resize(PATH_MAX);
    do
    {
        unsigned int len = GetModuleFileNameW(NULL, &buf[0], static_cast< unsigned int >(buf.size()));
        if (len < buf.size())
        {
            buf.resize(len);
            break;
        }

        buf.resize(buf.size() * 2);
    }
    while (buf.size() < 65536);

    return filesystem::path(buf).filename().string();
}

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#elif defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__)

#include <cstring>
#include <mach-o/dyld.h>
#include <boost/cstdint.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

//! The function returns the current process name
BOOST_LOG_API std::string get_process_name()
{
    std::string buf;
    buf.resize(PATH_MAX);
    while (true)
    {
        uint32_t size = static_cast< uint32_t >(buf.size());
        if (_NSGetExecutablePath(&buf[0], &size) == 0)
        {
            buf.resize(std::strlen(&buf[0]));
            break;
        }

        buf.resize(size);
    }

    return filesystem::path(buf).filename().string();
}

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#elif defined(__FreeBSD__)

#include <stddef.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <string>
#include <boost/filesystem/operations.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

//! The function returns the current process name
BOOST_LOG_API std::string get_process_name()
{
#if defined(KERN_PROC_PATHNAME)
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    char buf[PATH_MAX] = {};
    size_t cb = sizeof(buf);
    if (sysctl(mib, 4, buf, &cb, NULL, 0) == 0)
        return filesystem::path(buf).filename().string();
#endif

    if (filesystem::exists("/proc/curproc/file"))
        return filesystem::read_symlink("/proc/curproc/file").filename().string();

    return std::to_string(getpid());
}

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#else

#include <unistd.h>
#include <string>
#include <boost/filesystem/operations.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

//! The function returns the current process name
BOOST_LOG_API std::string get_process_name()
{
    if (filesystem::exists("/proc/self/exe"))
        return filesystem::read_symlink("/proc/self/exe").filename().string();

    if (filesystem::exists("/proc/curproc/file"))
        return filesystem::read_symlink("/proc/curproc/file").filename().string();

    if (filesystem::exists("/proc/curproc/exe"))
        return filesystem::read_symlink("/proc/curproc/exe").filename().string();

    return std::to_string(getpid());
}

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif
