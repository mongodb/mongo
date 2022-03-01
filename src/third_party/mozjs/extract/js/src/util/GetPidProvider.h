/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef util_GetPidProvider_h
#define util_GetPidProvider_h

#ifdef XP_WIN
#  include <process.h>
#  define getpid _getpid
#elif defined(__wasi__)
#  define getpid() 1
#else
#  include <unistd.h>
#endif

#endif /* util_GetPidProvider_h */
