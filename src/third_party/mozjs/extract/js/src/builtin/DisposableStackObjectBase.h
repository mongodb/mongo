/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_DisposableStackObjectBase_h
#define builtin_DisposableStackObjectBase_h

#include "vm/JSObject.h"
#include "vm/NativeObject.h"
#include "vm/UsingHint.h"

namespace js {

enum AdoptClosureSlots {
  AdoptClosureSlot_ValueSlot = 0,
  AdoptClosureSlot_OnDisposeSlot,
};

bool ThrowIfOnDisposeNotCallable(JSContext* cx,
                                 JS::Handle<JS::Value> onDispose);

bool AdoptClosure(JSContext* cx, unsigned argc, JS::Value* vp);

bool AddDisposableResource(JSContext* cx,
                           JS::Handle<ArrayObject*> disposeCapability,
                           JS::Handle<JS::Value> val, UsingHint hint);

bool AddDisposableResource(JSContext* cx,
                           JS::Handle<ArrayObject*> disposeCapability,
                           JS::Handle<JS::Value> val, UsingHint hint,
                           JS::Handle<JS::Value> methodVal);

bool CreateDisposableResource(JSContext* cx, JS::Handle<JS::Value> objVal,
                              UsingHint hint,
                              JS::MutableHandle<JS::Value> result);

bool CreateDisposableResource(JSContext* cx, JS::Handle<JS::Value> objVal,
                              UsingHint hint, JS::Handle<JS::Value> methodVal,
                              JS::MutableHandle<JS::Value> result);

bool GetDisposeMethod(JSContext* cx, JS::Handle<JS::Value> obj, UsingHint hint,
                      JS::MutableHandle<JS::Value> disposeMethod);

// This is a shared base class for common functionality between
// DisposableStackObject and AsyncDisposableStackObject.
class DisposableStackObjectBase : public NativeObject {
 public:
  enum DisposableState : uint8_t { Pending, Disposed };

  static constexpr uint32_t DISPOSABLE_RESOURCE_STACK_SLOT = 0;
  static constexpr uint32_t STATE_SLOT = 1;
  static constexpr uint32_t RESERVED_SLOTS = 2;

 protected:
  static ArrayObject* GetOrCreateDisposeCapability(
      JSContext* cx, JS::Handle<DisposableStackObjectBase*> obj);

  bool isDisposableResourceStackEmpty() const;
  void clearDisposableResourceStack();
  ArrayObject* nonEmptyDisposableResourceStack() const;
  DisposableState state() const;
  void setState(DisposableState state);
};

} /* namespace js */

#endif /* builtin_DisposableStackObjectBase_h */
