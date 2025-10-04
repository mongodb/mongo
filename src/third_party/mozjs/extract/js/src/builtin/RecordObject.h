/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_RecordObject_h
#define builtin_RecordObject_h

#include "vm/JSObject.h"
#include "vm/NativeObject.h"
#include "vm/RecordType.h"

namespace js {

class RecordObject : public NativeObject {
  enum { PrimitiveValueSlot, SlotCount };

 public:
  static const JSClass class_;

  static RecordObject* create(JSContext* cx, Handle<RecordType*> record);

  JS::RecordType* unbox() const;

  static bool maybeUnbox(JSObject* obj, MutableHandle<RecordType*> rrec);
};

}  // namespace js

#endif
