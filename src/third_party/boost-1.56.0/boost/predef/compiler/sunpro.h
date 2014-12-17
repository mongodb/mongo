/*
Copyright Rene Rivera 2008-2014
Distributed under the Boost Software License, Version 1.0.
(See accompanying file LICENSE_1_0.txt or copy at
http://www.boost.org/LICENSE_1_0.txt)
*/

#ifndef BOOST_PREDEF_COMPILER_SUNPRO_H
#define BOOST_PREDEF_COMPILER_SUNPRO_H

#include <boost/predef/version_number.h>
#include <boost/predef/make.h>

/*`
[heading `BOOST_COMP_SUNPRO`]

[@http://en.wikipedia.org/wiki/Sun_Studio_%28software%29 Sun Studio] compiler.
Version number available as major, minor, and patch.

[table
    [[__predef_symbol__] [__predef_version__]]

    [[`__SUNPRO_CC`] [__predef_detection__]]
    [[`__SUNPRO_C`] [__predef_detection__]]

    [[`__SUNPRO_CC`] [V.R.P]]
    [[`__SUNPRO_C`] [V.R.P]]
    ]
 */

#define BOOST_COMP_SUNPRO BOOST_VERSION_NUMBER_NOT_AVAILABLE

#if defined(__SUNPRO_CC) || defined(__SUNPRO_C)
#   if !defined(BOOST_COMP_SUNPRO_DETECTION) && defined(__SUNPRO_CC)
#       define BOOST_COMP_SUNPRO_DETECTION BOOST_PREDEF_MAKE_0X_VRP(__SUNPRO_CC)
#   endif
#   if !defined(BOOST_COMP_SUNPRO_DETECTION) && defined(__SUNPRO_C)
#       define BOOST_COMP_SUNPRO_DETECTION BOOST_PREDEF_MAKE_0X_VRP(__SUNPRO_C)
#   endif
#   if !defined(BOOST_COMP_SUNPRO_DETECTION)
#       define BOOST_COMP_SUNPRO_DETECTION BOOST_VERSION_NUMBER_AVAILABLE
#   endif
#endif

#ifdef BOOST_COMP_SUNPRO_DETECTION
#   if defined(BOOST_PREDEF_DETAIL_COMP_DETECTED)
#       define BOOST_COMP_SUNPRO_EMULATED BOOST_COMP_SUNPRO_DETECTION
#   else
#       undef BOOST_COMP_SUNPRO
#       define BOOST_COMP_SUNPRO BOOST_COMP_SUNPRO_DETECTION
#   endif
#   define BOOST_COMP_SUNPRO_AVAILABLE
#   include <boost/predef/detail/comp_detected.h>
#endif

#define BOOST_COMP_SUNPRO_NAME "Sun Studio"

#include <boost/predef/detail/test.h>
BOOST_PREDEF_DECLARE_TEST(BOOST_COMP_SUNPRO,BOOST_COMP_SUNPRO_NAME)

#ifdef BOOST_COMP_SUNPRO_EMULATED
#include <boost/predef/detail/test.h>
BOOST_PREDEF_DECLARE_TEST(BOOST_COMP_SUNPRO_EMULATED,BOOST_COMP_SUNPRO_NAME)
#endif


#endif
