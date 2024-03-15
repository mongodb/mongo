/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef debugger_Script_inl_h
#define debugger_Script_inl_h

#include "debugger/Script.h"  // for DebuggerScript

#include "mozilla/Assertions.h"  // for AssertionConditionType, MOZ_ASSERT
#include "mozilla/Variant.h"     // for AsVariant

#include <utility>  // for move

#include "jstypes.h"            // for JS_PUBLIC_API
#include "debugger/Debugger.h"  // for DebuggerScriptReferent
#include "gc/Cell.h"            // for Cell
#include "vm/JSScript.h"        // for BaseScript, JSScript
#include "vm/NativeObject.h"    // for NativeObject
#include "wasm/WasmJS.h"        // for WasmInstanceObject

#include "debugger/Debugger-inl.h"  // for Debugger::fromJSObject

class JS_PUBLIC_API JSObject;

inline js::Debugger* js::DebuggerScript::owner() const {
  JSObject* dbgobj = &getReservedSlot(OWNER_SLOT).toObject();
  return Debugger::fromJSObject(dbgobj);
}

js::gc::Cell* js::DebuggerScript::getReferentCell() const {
  return maybePtrFromReservedSlot<gc::Cell>(SCRIPT_SLOT);
}

js::DebuggerScriptReferent js::DebuggerScript::getReferent() const {
  if (gc::Cell* cell = getReferentCell()) {
    if (cell->is<BaseScript>()) {
      return mozilla::AsVariant(cell->as<BaseScript>());
    }
    MOZ_ASSERT(cell->is<JSObject>());
    return mozilla::AsVariant(
        &static_cast<NativeObject*>(cell)->as<WasmInstanceObject>());
  }
  return mozilla::AsVariant(static_cast<BaseScript*>(nullptr));
}

js::BaseScript* js::DebuggerScript::getReferentScript() const {
  gc::Cell* cell = getReferentCell();
  return cell->as<BaseScript>();
}

#endif /* debugger_Script_inl_h */
