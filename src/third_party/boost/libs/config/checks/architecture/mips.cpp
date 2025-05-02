// mips.cpp
//
// Copyright (c) 2012 Steven Watanabe
//
// Distributed under the Boost Software License Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#if !(defined(__mips) || defined(_MIPS_ISA_MIPS1) || defined(_R3000))
#error "Not MIPS"
#endif
