/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_WeakMapObject_h
#define builtin_WeakMapObject_h

#include "gc/WeakMap.h"
#include "vm/NativeObject.h"

namespace js {

// Abstract base class for WeakMapObject and WeakSetObject.
class WeakCollectionObject : public NativeObject {
 public:
  enum { DataSlot, SlotCount };

  ObjectValueWeakMap* getMap() {
    return maybePtrFromReservedSlot<ObjectValueWeakMap>(DataSlot);
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf aMallocSizeOf);

  [[nodiscard]] static bool nondeterministicGetKeys(
      JSContext* cx, Handle<WeakCollectionObject*> obj,
      MutableHandleObject ret);

 protected:
  static const JSClassOps classOps_;
};

class WeakMapObject : public WeakCollectionObject {
 public:
  static const JSClass class_;
  static const JSClass protoClass_;

 private:
  static const ClassSpec classSpec_;

  static const JSPropertySpec properties[];
  static const JSFunctionSpec methods[];

  [[nodiscard]] static bool construct(JSContext* cx, unsigned argc, Value* vp);

  [[nodiscard]] static MOZ_ALWAYS_INLINE bool is(HandleValue v);

  [[nodiscard]] static MOZ_ALWAYS_INLINE bool has_impl(JSContext* cx,
                                                       const CallArgs& args);
  [[nodiscard]] static bool has(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static MOZ_ALWAYS_INLINE bool get_impl(JSContext* cx,
                                                       const CallArgs& args);
  [[nodiscard]] static bool get(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static MOZ_ALWAYS_INLINE bool delete_impl(JSContext* cx,
                                                          const CallArgs& args);
  [[nodiscard]] static bool delete_(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static MOZ_ALWAYS_INLINE bool set_impl(JSContext* cx,
                                                       const CallArgs& args);
  [[nodiscard]] static bool set(JSContext* cx, unsigned argc, Value* vp);
};

}  // namespace js

#endif /* builtin_WeakMapObject_h */
