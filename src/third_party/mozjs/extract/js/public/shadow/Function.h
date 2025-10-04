/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Shadow definition of |JSFunction| innards.  Do not use this directly! */

#ifndef js_shadow_Function_h
#define js_shadow_Function_h

#include <stdint.h>  // uint16_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/CallArgs.h"       // JSNative
#include "js/shadow/Object.h"  // JS::shadow::Object
#include "js/Value.h"          // JS::Value

class JS_PUBLIC_API JSFunction;
class JSJitInfo;

namespace JS {

namespace shadow {

struct Function : shadow::Object {
  enum {
    FlagsAndArgCountSlot,
    NativeFuncOrInterpretedEnvSlot,
    NativeJitInfoOrInterpretedScriptSlot,
    AtomSlot
  };
  uint32_t flagsAndArgCount() const {
    return fixedSlots()[FlagsAndArgCountSlot].toPrivateUint32();
  }

  void* jitInfoOrScript() const {
    return fixedSlots()[NativeJitInfoOrInterpretedScriptSlot].toPrivate();
  }

  void setJitInfoOrScript(void* ptr) {
    fixedSlots()[NativeJitInfoOrInterpretedScriptSlot] = JS::PrivateValue(ptr);
  }
};

}  // namespace shadow

}  // namespace JS

#endif  // js_shadow_Function_h
