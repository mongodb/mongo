/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2020 Andrey Semashev
 */

#define BOOST_USE_WINAPI_VERSION 0x0602

// Include Boost.Predef first so that windows.h is guaranteed to be not included
#include <boost/predef/os/windows.h>
#include <boost/predef/os/cygwin.h>
#if !BOOST_OS_WINDOWS && !BOOST_OS_CYGWIN
#error "This config test is for Windows only"
#endif

#include <boost/winapi/config.hpp>
#include <boost/predef/platform.h>
#if !(BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN8 && (BOOST_WINAPI_PARTITION_APP || BOOST_WINAPI_PARTITION_SYSTEM))
#error "No WaitOnAddress API"
#endif

#include <cstddef>
#include <boost/winapi/basic_types.hpp>
#include <boost/winapi/wait_on_address.hpp>

int main()
{
    unsigned int n = 0u, compare = 0u;
    boost::winapi::WaitOnAddress(&n, &compare, sizeof(n), 0);
    return 0;
}
