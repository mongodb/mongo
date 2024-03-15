/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef util_NativeStack_h
#define util_NativeStack_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "js/Stack.h"  // JS::NativeStackBase

namespace js {

extern void* GetNativeStackBaseImpl();

inline JS::NativeStackBase GetNativeStackBase() {
  JS::NativeStackBase stackBase =
      reinterpret_cast<JS::NativeStackBase>(GetNativeStackBaseImpl());
  MOZ_ASSERT(stackBase != 0);
  MOZ_ASSERT(stackBase % sizeof(void*) == 0);
  return stackBase;
}

} /* namespace js */

#endif /* util_NativeStack_h */
