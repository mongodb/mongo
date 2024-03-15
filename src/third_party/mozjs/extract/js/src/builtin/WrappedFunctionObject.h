/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_WrappedFunctionObject_h
#define builtin_WrappedFunctionObject_h

#include "js/Value.h"
#include "vm/NativeObject.h"

namespace js {

// Implementing Wrapped Function Exotic Objects from the ShadowRealms proposal
// https://tc39.es/proposal-shadowrealm/#sec-wrapped-function-exotic-objects
//
// These are produced as callables are passed across ShadowRealm boundaries,
// preventing functions from piercing the shadow realm barrier.
class WrappedFunctionObject : public NativeObject {
 public:
  static const JSClass class_;

  enum { WrappedTargetFunctionSlot, SlotCount };

  JSObject* getTargetFunction() const {
    return &getFixedSlot(WrappedTargetFunctionSlot).toObject();
  }

  void setTargetFunction(JSObject& obj) {
    setFixedSlot(WrappedTargetFunctionSlot, ObjectValue(obj));
  }
};

bool WrappedFunctionCreate(JSContext* cx, Realm* callerRealm,
                           Handle<JSObject*> target, MutableHandle<Value> res);

bool GetWrappedValue(JSContext* cx, Realm* callerRealm, Handle<Value> value,
                     MutableHandle<Value> res);

}  // namespace js

#endif
