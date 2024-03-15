/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_TupleObject_h
#define builtin_TupleObject_h

#include "vm/NativeObject.h"
#include "vm/TupleType.h"

namespace js {

[[nodiscard]] mozilla::Maybe<TupleType&> ThisTupleValue(JSContext* cx,
                                                        HandleValue val);

class TupleObject : public NativeObject {
  enum { PrimitiveValueSlot, SlotCount };

 public:
  static const JSClass class_;

  static TupleObject* create(JSContext* cx, Handle<TupleType*> tuple);

  JS::TupleType& unbox() const;

  static mozilla::Maybe<TupleType&> maybeUnbox(JSObject* obj);
};

bool IsTuple(JSObject& obj);
}  // namespace js

#endif
