/*
Copyright Redshift Software, Inc. 2008-2013
Distributed under the Boost Software License, Version 1.0.
(See accompanying file LICENSE_1_0.txt or copy at
http://www.boost.org/LICENSE_1_0.txt)
*/

#ifndef BOOST_PREDEF_OS_MACOS_H
#define BOOST_PREDEF_OS_MACOS_H

#include <boost/predef/version_number.h>
#include <boost/predef/make.h>

/*`
[heading `BOOST_OS_MACOS`]

[@http://en.wikipedia.org/wiki/Mac_OS Mac OS] operating system.

[table
    [[__predef_symbol__] [__predef_version__]]

    [[`macintosh`] [__predef_detection__]]
    [[`Macintosh`] [__predef_detection__]]
    [[`__APPLE__`] [__predef_detection__]]
    [[`__MACH__`] [__predef_detection__]]

    [[`__APPLE__`, `__MACH__`] [10.0.0]]
    [[ /otherwise/ ] [9.0.0]]
    ]
 */

#define BOOST_OS_MACOS BOOST_VERSION_NUMBER_NOT_AVAILABLE

#if !BOOST_PREDEF_DETAIL_OS_DETECTED && ( \
    defined(macintosh) || defined(Macintosh) || \
    (defined(__APPLE__) && defined(__MACH__)) \
    )
#   undef BOOST_OS_MACOS
#   if !defined(BOOST_OS_MACOS) && defined(__APPLE__) && defined(__MACH__)
#       define BOOST_OS_MACOS BOOST_VERSION_NUMBER(10,0,0)
#   endif
#   if !defined(BOOST_OS_MACOS)
#       define BOOST_OS_MACOS BOOST_VERSION_NUMBER(9,0,0)
#   endif
#endif

#if BOOST_OS_MACOS
#   define BOOST_OS_MACOS_AVAILABLE
#   include <boost/predef/detail/os_detected.h>
#endif

#define BOOST_OS_MACOS_NAME "Mac OS"

#include <boost/predef/detail/test.h>
BOOST_PREDEF_DECLARE_TEST(BOOST_OS_MACOS,BOOST_OS_MACOS_NAME)


#endif
