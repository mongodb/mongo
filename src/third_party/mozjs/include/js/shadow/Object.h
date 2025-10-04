/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Shadow definition of |JSObject| innards.  (|js::NativeObject| is more
 * accurate, but portions can sometimes be used in some non-native objects.)  Do
 * not use this directly!
 */

#ifndef js_shadow_Object_h
#define js_shadow_Object_h

#include <stddef.h>  // size_t

#include "js/shadow/Shape.h"  // JS::shadow::Shape
#include "js/Value.h"         // JS::Value

class JS_PUBLIC_API JSObject;

namespace JS {

class JS_PUBLIC_API Value;

namespace shadow {

/**
 * This layout is shared by all native objects. For non-native objects, the
 * shape may always be accessed safely, and other members may be as well,
 * depending on the object's specific layout.
 */
struct Object {
  shadow::Shape* shape;
#ifndef JS_64BIT
  uint32_t padding_;
#endif
  Value* slots;
  void* _1;

  static constexpr size_t MAX_FIXED_SLOTS = 16;

  size_t numFixedSlots() const {
    return (shape->immutableFlags & shadow::Shape::FIXED_SLOTS_MASK) >>
           shadow::Shape::FIXED_SLOTS_SHIFT;
  }

  Value* fixedSlots() const {
    auto address = reinterpret_cast<uintptr_t>(this);
    return reinterpret_cast<JS::Value*>(address + sizeof(shadow::Object));
  }

  Value& slotRef(size_t slot) const {
    size_t nfixed = numFixedSlots();
    if (slot < nfixed) {
      return fixedSlots()[slot];
    }
    return slots[slot - nfixed];
  }
};

}  // namespace shadow

}  // namespace JS

#endif  // js_shadow_Object_h
