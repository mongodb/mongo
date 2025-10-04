/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Path charset agnostic wrappers for prlink.h. */

#ifndef mozilla_SharedLibrary_h
#define mozilla_SharedLibrary_h

#ifdef MOZILLA_INTERNAL_API

#  include "prlink.h"
#  include "mozilla/Char16.h"

namespace mozilla {

//
// Load the specified library.
//
// @param aPath  path to the library
// @param aFlags takes PR_LD_* flags (see prlink.h)
//
inline PRLibrary*
#  ifdef XP_WIN
LoadLibraryWithFlags(char16ptr_t aPath, PRUint32 aFlags = 0)
#  else
LoadLibraryWithFlags(const char* aPath, PRUint32 aFlags = 0)
#  endif
{
  PRLibSpec libSpec;
#  ifdef XP_WIN
  libSpec.type = PR_LibSpec_PathnameU;
  libSpec.value.pathname_u = aPath;
#  else
  libSpec.type = PR_LibSpec_Pathname;
  libSpec.value.pathname = aPath;
#  endif
  return PR_LoadLibraryWithFlags(libSpec, aFlags);
}

} /* namespace mozilla */

#endif /* MOZILLA_INTERNAL_API */

#endif /* mozilla_SharedLibrary_h */
