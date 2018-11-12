/*
Copyright Benjamin Worpitz 2018
Distributed under the Boost Software License, Version 1.0.
(See accompanying file LICENSE_1_0.txt or copy at
http://www.boost.org/LICENSE_1_0.txt)
*/

#ifndef BOOST_PREDEF_COMPILER_NVCC_H
#define BOOST_PREDEF_COMPILER_NVCC_H

#include <boost/predef/version_number.h>
#include <boost/predef/make.h>

/*`
[heading `BOOST_COMP_NVCC`]

[@https://en.wikipedia.org/wiki/NVIDIA_CUDA_Compiler NVCC] compiler.
Version number available as major, minor, and patch beginning with version 7.5.

[table
    [[__predef_symbol__] [__predef_version__]]

    [[`__NVCC__`] [__predef_detection__]]

    [[`__CUDACC_VER_MAJOR__`, `__CUDACC_VER_MINOR__`, `__CUDACC_VER_BUILD__`] [V.R.P]]
    ]
 */

#define BOOST_COMP_NVCC BOOST_VERSION_NUMBER_NOT_AVAILABLE

#if defined(__NVCC__)
#   if !defined(__CUDACC_VER_MAJOR__) || !defined(__CUDACC_VER_MINOR__) || !defined(__CUDACC_VER_BUILD__)
#       define BOOST_COMP_NVCC_DETECTION BOOST_VERSION_NUMBER_AVAILABLE
#   else
#       define BOOST_COMP_NVCC_DETECTION BOOST_VERSION_NUMBER(__CUDACC_VER_MAJOR__, __CUDACC_VER_MINOR__, __CUDACC_VER_BUILD__)
#   endif
#endif

#ifdef BOOST_COMP_NVCC_DETECTION
#   if defined(BOOST_PREDEF_DETAIL_COMP_DETECTED)
#       define BOOST_COMP_NVCC_EMULATED BOOST_COMP_NVCC_DETECTION
#   else
#       undef BOOST_COMP_NVCC
#       define BOOST_COMP_NVCC BOOST_COMP_NVCC_DETECTION
#   endif
#   define BOOST_COMP_NVCC_AVAILABLE
#   include <boost/predef/detail/comp_detected.h>
#endif

#define BOOST_COMP_NVCC_NAME "NVCC"

#endif

#include <boost/predef/detail/test.h>
BOOST_PREDEF_DECLARE_TEST(BOOST_COMP_NVCC,BOOST_COMP_NVCC_NAME)

#ifdef BOOST_COMP_NVCC_EMULATED
#include <boost/predef/detail/test.h>
BOOST_PREDEF_DECLARE_TEST(BOOST_COMP_NVCC_EMULATED,BOOST_COMP_NVCC_NAME)
#endif
