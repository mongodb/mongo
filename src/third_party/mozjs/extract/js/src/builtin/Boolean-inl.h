/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_Boolean_inl_h
#define builtin_Boolean_inl_h

#include "builtin/Boolean.h"

#include "vm/JSContext.h"
#include "vm/WrapperObject.h"

namespace js {

inline bool EmulatesUndefined(JSObject* obj) {
  // This may be called off the main thread. It's OK not to expose the object
  // here as it doesn't escape.
  AutoUnsafeCallWithABI unsafe(UnsafeABIStrictness::AllowPendingExceptions);
  JSObject* actual = MOZ_LIKELY(!obj->is<WrapperObject>())
                         ? obj
                         : UncheckedUnwrapWithoutExpose(obj);
  return actual->getClass()->emulatesUndefined();
}

} /* namespace js */

#endif /* builtin_Boolean_inl_h */
