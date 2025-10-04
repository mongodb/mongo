/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Shadow definition of |js::BaseShape| and |js::Shape| innards.  Do not use
 * this directly!
 */

#ifndef js_shadow_Shape_h
#define js_shadow_Shape_h

#include <stdint.h>  // uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/Id.h"  // JS::PropertyKey

struct JSClass;
class JS_PUBLIC_API JSObject;

namespace JS {

namespace shadow {

struct BaseShape {
  const JSClass* clasp;
  JS::Realm* realm;
};

class Shape {
 public:
  shadow::BaseShape* base;
  uint32_t immutableFlags;

  enum class Kind : uint8_t {
    // SharedShape or DictionaryShape for NativeObject.
    // (Only) these two kinds must have the low bit set, to allow for fast
    // is-native checking.
    Shared = 1,
    Dictionary = 3,
    // ProxyShape for ProxyObject.
    Proxy = 0,
    // WasmGCShape for WasmGCObject.
    WasmGC = 2,
  };

  static constexpr uint32_t KIND_SHIFT = 4;
  static constexpr uint32_t KIND_MASK = 0b11;

  static constexpr uint32_t FIXED_SLOTS_SHIFT = 6;
  static constexpr uint32_t FIXED_SLOTS_MASK = 0x1f << FIXED_SLOTS_SHIFT;

  bool isProxy() const {
    return Kind((immutableFlags >> KIND_SHIFT) & KIND_MASK) == Kind::Proxy;
  }
};

}  // namespace shadow

}  // namespace JS

#endif  // js_shadow_Shape_h
