/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_WeakSetObject_h
#define builtin_WeakSetObject_h

#include "builtin/WeakMapObject.h"

namespace js {

class WeakSetObject : public WeakCollectionObject {
 public:
  static const JSClass class_;
  static const JSClass protoClass_;

 private:
  static const ClassSpec classSpec_;

  static const JSPropertySpec properties[];
  static const JSFunctionSpec methods[];

  static WeakSetObject* create(JSContext* cx, HandleObject proto = nullptr);
  [[nodiscard]] static bool construct(JSContext* cx, unsigned argc, Value* vp);

  [[nodiscard]] static MOZ_ALWAYS_INLINE bool is(HandleValue v);

  [[nodiscard]] static MOZ_ALWAYS_INLINE bool add_impl(JSContext* cx,
                                                       const CallArgs& args);
  [[nodiscard]] static bool add(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static MOZ_ALWAYS_INLINE bool delete_impl(JSContext* cx,
                                                          const CallArgs& args);
  [[nodiscard]] static bool delete_(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static MOZ_ALWAYS_INLINE bool has_impl(JSContext* cx,
                                                       const CallArgs& args);
  [[nodiscard]] static bool has(JSContext* cx, unsigned argc, Value* vp);

  static bool isBuiltinAdd(HandleValue add);
};

}  // namespace js

template <>
inline bool JSObject::is<js::WeakCollectionObject>() const {
  return is<js::WeakMapObject>() || is<js::WeakSetObject>();
}

#endif /* builtin_WeakSetObject_h */
