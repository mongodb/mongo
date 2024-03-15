/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_WeakRefObject_h
#define builtin_WeakRefObject_h

#include "vm/NativeObject.h"

namespace js {

class WeakRefObject : public NativeObject {
 public:
  enum { TargetSlot, SlotCount };

  static const JSClass class_;
  static const JSClass protoClass_;

  JSObject* target() { return maybePtrFromReservedSlot<JSObject>(TargetSlot); }

  void setTargetUnbarriered(JSObject* target);
  void clearTarget();

 private:
  static const JSClassOps classOps_;
  static const ClassSpec classSpec_;
  static const JSPropertySpec properties[];
  static const JSFunctionSpec methods[];

  [[nodiscard]] static bool construct(JSContext* cx, unsigned argc, Value* vp);
  static void trace(JSTracer* trc, JSObject* obj);
  static void finalize(JS::GCContext* gcx, JSObject* obj);

  static bool preserveDOMWrapper(JSContext* cx, HandleObject obj);

  static bool deref(JSContext* cx, unsigned argc, Value* vp);
  static void readBarrier(JSContext* cx, Handle<WeakRefObject*> self);
};

}  // namespace js
#endif /* builtin_WeakRefObject_h */
