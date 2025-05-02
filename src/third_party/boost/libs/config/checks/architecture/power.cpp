// power.cpp
//
// Copyright (c) 2012 Steven Watanabe
//
// Distributed under the Boost Software License Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#if !defined(__powerpc) && !defined(__powerpc__) && !defined(__ppc) \
    && !defined(__ppc__) && !defined(_M_PPC) && !defined(_ARCH_PPC) \
    && !defined(__POWERPC__) && !defined(__PPCGECKO__) \
    && !defined(__PPCBROADWAY) && !defined(_XENON)
#error "Not PPC"
#endif
