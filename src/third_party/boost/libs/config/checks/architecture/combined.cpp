// combined.cpp
//
// Copyright (c) 2012 Steven Watanabe
//               2014 Oliver Kowalke
//
// Distributed under the Boost Software License Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#if !defined(i386) && !defined(__i386__) && !defined(__i386) \
    && !defined(__i486__) && !defined(__i586__) && !defined(__i686__) \
    && !defined(_M_IX86) && !defined(__X86__) && !defined(_X86_) \
    && !defined(__THW_INTEL__) && !defined(__I86__) && !defined(__INTEL__) \
    && !defined(__amd64__) && !defined(__x86_64__) && !defined(__amd64) \
    && !defined(__x86_64) && !defined(_M_X64) \
    && !defined(__powerpc) && !defined(__powerpc__) && !defined(__ppc) \
    && !defined(__ppc__) && !defined(_M_PPC) && !defined(_ARCH_PPC) \
    && !defined(__POWERPC__) && !defined(__PPCGECKO__) \
    && !defined(__PPCBROADWAY) && !defined(_XENON)
#error "Not combined"
#endif
