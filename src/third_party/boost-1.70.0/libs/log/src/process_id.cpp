/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   process_id.cpp
 * \author Andrey Semashev
 * \date   12.09.2009
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <iostream>
#include <boost/log/detail/process_id.hpp>
#include "id_formatting.hpp"

#if defined(BOOST_WINDOWS)

#include <windows.h>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

enum { pid_size = sizeof(GetCurrentProcessId()) };

namespace this_process {

    //! The function returns current process identifier
    BOOST_LOG_API process::id get_id()
    {
        return process::id(GetCurrentProcessId());
    }

} // namespace this_process

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#else // defined(BOOST_WINDOWS)

#include <unistd.h>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

namespace this_process {

    //! The function returns current process identifier
    BOOST_LOG_API process::id get_id()
    {
        // According to POSIX, pid_t should always be an integer type:
        // http://pubs.opengroup.org/onlinepubs/009695399/basedefs/sys/types.h.html
        return process::id(getpid());
    }

} // namespace this_process

enum { pid_size = sizeof(pid_t) };

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#endif // defined(BOOST_WINDOWS)


namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

template< typename CharT, typename TraitsT >
BOOST_LOG_API std::basic_ostream< CharT, TraitsT >&
operator<< (std::basic_ostream< CharT, TraitsT >& strm, process::id const& pid)
{
    if (strm.good())
    {
        CharT buf[pid_size * 2 + 3]; // 2 chars per byte + 3 chars for the leading 0x and terminating zero
        format_id< pid_size >(buf, sizeof(buf) / sizeof(*buf), pid.native_id(), (strm.flags() & std::ios_base::uppercase) != 0);

        strm << buf;
    }

    return strm;
}

#if defined(BOOST_LOG_USE_CHAR)
template BOOST_LOG_API
std::basic_ostream< char, std::char_traits< char > >&
operator<< (std::basic_ostream< char, std::char_traits< char > >& strm, process::id const& pid);
#endif // defined(BOOST_LOG_USE_CHAR)

#if defined(BOOST_LOG_USE_WCHAR_T)
template BOOST_LOG_API
std::basic_ostream< wchar_t, std::char_traits< wchar_t > >&
operator<< (std::basic_ostream< wchar_t, std::char_traits< wchar_t > >& strm, process::id const& pid);
#endif // defined(BOOST_LOG_USE_WCHAR_T)

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
