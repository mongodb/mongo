//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2012-2013. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////


#define DLMALLOC_VERSION 286

#ifndef DLMALLOC_VERSION
   #error "DLMALLOC_VERSION undefined"
#endif

#if DLMALLOC_VERSION == 286
   #include "dlmalloc_ext_2_8_6.c"
#else
   #error "Unsupported boost_cont_VERSION version"
#endif
