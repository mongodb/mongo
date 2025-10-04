/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef util_WindowsWrapper_h
#define util_WindowsWrapper_h

/*
 * This file is a wrapper around <windows.h> to prevent the mangling of
 * various function names throughout the codebase.
 */

#ifdef XP_WIN
#  include <windows.h>
#  undef assert
#  undef GetProp
#  undef MemoryBarrier
#  undef SetProp
#  undef CONST
#  undef STRICT
#  undef LEGACY
#  undef THIS
#  undef PASSTHROUGH
#endif

#endif /* util_WindowsWrapper_h */
