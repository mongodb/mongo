/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SavedStacksInl_h
#define vm_SavedStacksInl_h

#include "vm/SavedStacks.h"

// Assert that if the given object is not null, it's Class is the
// SavedFrame::class_ or the given object is a cross-compartment or Xray wrapper
// around such an object.
//
// We allow wrappers here because the JSAPI functions for working with
// SavedFrame objects and the SavedFrame accessors themselves handle wrappers
// and use the original caller's compartment's principals to determine what
// level of data to present. Unwrapping and entering the referent's compartment
// would mess that up. See the module level documentation in
// `js/src/vm/SavedStacks.h` as well as the comments in `js/src/jsapi.h`.
inline void
js::AssertObjectIsSavedFrameOrWrapper(JSContext* cx, HandleObject stack)
{
    if (stack)
        MOZ_RELEASE_ASSERT(js::SavedFrame::isSavedFrameOrWrapperAndNotProto(*stack));
}

#endif // vm_SavedStacksInl_h
