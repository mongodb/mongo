/*
Copyright Zhang Na 2022
Distributed under the Boost Software License, Version 1.0.
(See accompanying file LICENSE_1_0.txt or copy at
http://www.boost.org/LICENSE_1_0.txt)
*/

#ifndef BOOST_PREDEF_ARCHITECTURE_LOONGARCH_H
#define BOOST_PREDEF_ARCHITECTURE_LOONGARCH_H

#include <boost/predef/version_number.h>
#include <boost/predef/make.h>

/* tag::reference[]
= `BOOST_ARCH_LOONGARCH`

[options="header"]
|===
| {predef_symbol} | {predef_version}

| `+__loongarch__+` | {predef_detection}
|===
*/ // end::reference[]

#define BOOST_ARCH_LOONGARCH BOOST_VERSION_NUMBER_NOT_AVAILABLE

#if defined(__loongarch__)
#   undef BOOST_ARCH_LOONGARCH
#   define BOOST_ARCH_LOONGARCH BOOST_VERSION_NUMBER_AVAILABLE
#endif

#if BOOST_ARCH_LOONGARCH
#   define BOOST_ARCH_LOONGARCH_AVAILABLE
#endif

#define BOOST_ARCH_LOONGARCH_NAME "LoongArch"

#endif

#include <boost/predef/detail/test.h>
BOOST_PREDEF_DECLARE_TEST(BOOST_ARCH_LOONGARCH,BOOST_ARCH_LOONGARCH_NAME)
