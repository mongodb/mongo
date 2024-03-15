/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef debugger_Object_inl_h
#define debugger_Object_inl_h

#include "debugger/Object.h"  // for DebuggerObject

#include "mozilla/Assertions.h"  // for AssertionConditionType, MOZ_ASSERT

#include "NamespaceImports.h"  // for Value

#include "debugger/Debugger.h"  // for Debugger
#include "js/Wrapper.h"         // for CheckedUnwrapStatic
#include "vm/JSObject.h"        // for JSObject
#include "vm/PromiseObject.h"   // for js::PromiseObject

#include "debugger/Debugger-inl.h"  // for Debugger::fromJSObject

inline js::Debugger* js::DebuggerObject::owner() const {
  JSObject* dbgobj = &getReservedSlot(OWNER_SLOT).toObject();
  return Debugger::fromJSObject(dbgobj);
}

inline js::PromiseObject* js::DebuggerObject::promise() const {
  MOZ_ASSERT(isPromise());

  JSObject* referent = this->referent();
  if (IsCrossCompartmentWrapper(referent)) {
    // We know we have a Promise here, so CheckedUnwrapStatic is fine.
    referent = CheckedUnwrapStatic(referent);
    MOZ_ASSERT(referent);
  }

  return &referent->as<PromiseObject>();
}

#endif /* debugger_Object_inl_h */
