/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef util_NativeStack_h
#define util_NativeStack_h

#include "js/Utility.h"

namespace js {

extern void*
GetNativeStackBaseImpl();

inline uintptr_t
GetNativeStackBase()
{
    uintptr_t stackBase = reinterpret_cast<uintptr_t>(GetNativeStackBaseImpl());
    MOZ_ASSERT(stackBase != 0);
    MOZ_ASSERT(stackBase % sizeof(void*) == 0);
    return stackBase;
}

} /* namespace js */

#endif /* util_NativeStack_h */
