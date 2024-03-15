/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef debugger_Environment_inl_h
#define debugger_Environment_inl_h

#include "debugger/Environment.h"  // for DebuggerEnvironment

#include "jstypes.h"            // for JS_PUBLIC_API
#include "NamespaceImports.h"   // for Value
#include "debugger/Debugger.h"  // for Debugger

#include "debugger/Debugger-inl.h"  // for Debugger::fromJSObject

class JS_PUBLIC_API JSObject;

inline js::Debugger* js::DebuggerEnvironment::owner() const {
  JSObject* dbgobj = &getReservedSlot(OWNER_SLOT).toObject();
  return Debugger::fromJSObject(dbgobj);
}

#endif /* debugger_Environment_inl_h */
