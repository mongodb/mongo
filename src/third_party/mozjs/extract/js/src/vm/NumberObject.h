/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_NumberObject_h
#define vm_NumberObject_h

#include "vm/NativeObject.h"

namespace js {

class NumberObject : public NativeObject {
  /* Stores this Number object's [[PrimitiveValue]]. */
  static const unsigned PRIMITIVE_VALUE_SLOT = 0;

  static const ClassSpec classSpec_;

 public:
  static const unsigned RESERVED_SLOTS = 1;

  static const JSClass class_;

  /*
   * Creates a new Number object boxing the given number.
   * If proto is nullptr, then Number.prototype will be used instead.
   */
  static inline NumberObject* create(JSContext* cx, double d,
                                     HandleObject proto = nullptr);

  double unbox() const { return getFixedSlot(PRIMITIVE_VALUE_SLOT).toNumber(); }

 private:
  static JSObject* createPrototype(JSContext* cx, JSProtoKey key);

  inline void setPrimitiveValue(double d) {
    setFixedSlot(PRIMITIVE_VALUE_SLOT, NumberValue(d));
  }
};

}  // namespace js

#endif /* vm_NumberObject_h */
