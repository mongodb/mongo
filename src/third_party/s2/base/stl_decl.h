// Copyright (C) 1999 and onwards Google, Inc.
//
// DEPRECATED: This file is deprecated.  Do not use in new code.
// Be careful about removing from old code, though, because your
// header file might be included by higher-level code that is
// accidentally depending on this. This file has no impact in linux
// compilations; you can safely remove dependencies from linux code.
//
// Original file level comment:
//   In most .h files, we would rather include a declaration of an stl
//   rather than including the appropriate stl h file (which brings in
//   lots of crap).  For many STL classes this is ok (eg pair), but for
//   some it's really annoying.  We define those here, so you can
//   just include this file instead of having to deal with the annoyance.
//
//   Most of the annoyance, btw, has to do with the default allocator.
//

#ifndef BASE_STL_DECL_H_
#define BASE_STL_DECL_H_

#include "base/definer.h"

#if defined(OS_WINDOWS)  /* If VC++'s STL */
#include "base/stl_decl_msvc.h"

#elif defined(__APPLE__) && defined(OS_MACOSX) /* gcc with a twist */
#include "base/stl_decl_osx.h"

#elif !defined(__GNUC__)
#error "Unknown C++ compiler"
#endif

#endif  // BASE_STL_DECL_H_
