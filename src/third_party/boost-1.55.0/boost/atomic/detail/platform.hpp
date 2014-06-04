#ifndef BOOST_ATOMIC_DETAIL_PLATFORM_HPP
#define BOOST_ATOMIC_DETAIL_PLATFORM_HPP

//  Copyright (c) 2009 Helge Bahmann
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

// Platform selection file

#include <boost/atomic/detail/config.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

// Intel compiler does not support __atomic* intrinsics properly, although defines them (tested with 13.0.1 and 13.1.1 on Linux)
#if (defined(__GNUC__) && ((__GNUC__ * 100 + __GNUC_MINOR__) >= 407) && !defined(BOOST_INTEL_CXX_VERSION))\
    || (defined(BOOST_CLANG) && ((__clang_major__ * 100 + __clang_minor__) >= 302))

    #include <boost/atomic/detail/gcc-atomic.hpp>

#elif defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))

    #include <boost/atomic/detail/gcc-x86.hpp>

#elif 0 && defined(__GNUC__) && defined(__alpha__) /* currently does not work correctly */

    #include <boost/atomic/detail/base.hpp>
    #include <boost/atomic/detail/gcc-alpha.hpp>

#elif defined(__GNUC__) && (defined(__POWERPC__) || defined(__PPC__))

    #include <boost/atomic/detail/gcc-ppc.hpp>

// This list of ARM architecture versions comes from Apple's arm/arch.h header.
// I don't know how complete it is.
#elif defined(__GNUC__) && (defined(__ARM_ARCH_6__)  || defined(__ARM_ARCH_6J__) \
                         || defined(__ARM_ARCH_6Z__) || defined(__ARM_ARCH_6ZK__) \
                         || defined(__ARM_ARCH_6K__) \
                         || defined(__ARM_ARCH_7__) || defined(__ARM_ARCH_7A__) \
                         || defined(__ARM_ARCH_7R__) || defined(__ARM_ARCH_7M__) \
                         || defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_7S__))

    #include <boost/atomic/detail/gcc-armv6plus.hpp>

#elif defined(__linux__) && defined(__arm__)

    #include <boost/atomic/detail/linux-arm.hpp>

#elif defined(__GNUC__) && defined(__sparc_v9__)

    #include <boost/atomic/detail/gcc-sparcv9.hpp>

#elif defined(BOOST_WINDOWS) || defined(_WIN32_CE)

    #include <boost/atomic/detail/windows.hpp>

#elif 0 && defined(__GNUC__) /* currently does not work correctly */

    #include <boost/atomic/detail/base.hpp>
    #include <boost/atomic/detail/gcc-cas.hpp>

#else

#include <boost/atomic/detail/base.hpp>

#endif

#endif
