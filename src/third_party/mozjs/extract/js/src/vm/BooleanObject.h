/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_BooleanObject_h
#define vm_BooleanObject_h

#include "vm/NativeObject.h"

namespace js {

class BooleanObject : public NativeObject {
  /* Stores this Boolean object's [[PrimitiveValue]]. */
  static const unsigned PRIMITIVE_VALUE_SLOT = 0;

  static const ClassSpec classSpec_;

 public:
  static const unsigned RESERVED_SLOTS = 1;

  static const JSClass class_;

  /*
   * Creates a new Boolean object boxing the given primitive bool.
   * If proto is nullptr, the [[Prototype]] will default to Boolean.prototype.
   */
  static inline BooleanObject* create(JSContext* cx, bool b,
                                      HandleObject proto = nullptr);

  bool unbox() const { return getFixedSlot(PRIMITIVE_VALUE_SLOT).toBoolean(); }

 private:
  static JSObject* createPrototype(JSContext* cx, JSProtoKey key);

  inline void setPrimitiveValue(bool b) {
    setFixedSlot(PRIMITIVE_VALUE_SLOT, BooleanValue(b));
  }
};

}  // namespace js

#endif /* vm_BooleanObject_h */
