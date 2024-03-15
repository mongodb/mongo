/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef debugger_DebuggerMemory_h
#define debugger_DebuggerMemory_h

#include "js/Class.h"
#include "js/Value.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"

namespace js {

class DebuggerMemory : public NativeObject {
  friend class Debugger;

  static DebuggerMemory* checkThis(JSContext* cx, CallArgs& args);

  Debugger* getDebugger();

 public:
  static DebuggerMemory* create(JSContext* cx, Debugger* dbg);

  enum { JSSLOT_DEBUGGER, JSSLOT_COUNT };

  static bool construct(JSContext* cx, unsigned argc, Value* vp);
  static const JSClass class_;
  static const JSPropertySpec properties[];
  static const JSFunctionSpec methods[];

  struct CallData;
};

} /* namespace js */

#endif /* debugger_DebuggerMemory_h */
