// arm.cpp
//
// Copyright (c) 2012 Steven Watanabe
//
// Distributed under the Boost Software License Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#if !defined(__arm__) && !defined(__thumb__) && \
    !defined(__TARGET_ARCH_ARM) && !defined(__TARGET_ARCH_THUMB) && \
    !defined(_ARM) && !defined(_M_ARM) && \
    !defined(__aarch64__) 
#error "Not ARM"
#endif

