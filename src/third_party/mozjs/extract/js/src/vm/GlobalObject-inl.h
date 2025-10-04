/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_GlobalObject_inl_h
#define vm_GlobalObject_inl_h

#include "vm/GlobalObject.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "vm/JSContext.h"             // JSContext
#include "vm/ObjectOperations-inl.h"  // js::SetProperty

/* static */ inline bool js::GlobalObject::setIntrinsicValue(
    JSContext* cx, Handle<GlobalObject*> global, Handle<PropertyName*> name,
    HandleValue value) {
  Rooted<NativeObject*> holder(cx, global->getComputedIntrinsicsHolder());
  MOZ_ASSERT(holder->lookupPure(name).isNothing(),
             "SetIntrinsic tried to redefine existing intrinsic");
  return SetProperty(cx, holder, name, value);
}

#endif /* vm_GlobalObject_inl_h */
