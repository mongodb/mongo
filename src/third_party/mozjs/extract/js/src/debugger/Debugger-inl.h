/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef debugger_Debugger_inl_h
#define debugger_Debugger_inl_h

#include "debugger/Debugger.h"  // for Debugger, ResumeMode

#include "mozilla/Assertions.h"  // for AssertionConditionType

#include "vm/JSObject.h"      // for JSObject
#include "vm/NativeObject.h"  // for NativeObject, JSObject::is

/* static */ inline js::Debugger* js::Debugger::fromJSObject(
    const JSObject* obj) {
  MOZ_ASSERT(obj->is<DebuggerInstanceObject>());
  return (Debugger*)obj->as<NativeObject>().getPrivate();
}

inline bool js::Debugger::isHookCallAllowed(JSContext* cx) const {
  // If we are evaluating inside of an eval on a debugger that has an
  // onNativeCall hook, we want to _only_ call the hooks attached to that
  // specific debugger.
  return !cx->insideDebuggerEvaluationWithOnNativeCallHook ||
         this == cx->insideDebuggerEvaluationWithOnNativeCallHook.ref();
}

#endif /* debugger_Debugger_inl_h */
