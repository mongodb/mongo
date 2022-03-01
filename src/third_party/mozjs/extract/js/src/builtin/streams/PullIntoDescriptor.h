/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Pull descriptor objects for tracking byte stream pull-into requests. */

#ifndef builtin_streams_PullIntoDescriptor_h
#define builtin_streams_PullIntoDescriptor_h

#include <stdint.h>  // int32_t, uint32_t

#include "js/Class.h"              // JSClass
#include "vm/ArrayBufferObject.h"  // js::ArrayBufferObject;
#include "vm/NativeObject.h"       // js::NativeObject

namespace js {

enum class ReaderType : int32_t { Default = 0, BYOB = 1 };

class PullIntoDescriptor : public NativeObject {
 private:
  enum Slots {
    Slot_buffer,
    Slot_ByteOffset,
    Slot_ByteLength,
    Slot_BytesFilled,
    Slot_ElementSize,
    Slot_Ctor,
    Slot_ReaderType,
    SlotCount
  };

 public:
  static const JSClass class_;

  ArrayBufferObject* buffer() {
    return &getFixedSlot(Slot_buffer).toObject().as<ArrayBufferObject>();
  }
  void setBuffer(ArrayBufferObject* buffer) {
    setFixedSlot(Slot_buffer, ObjectValue(*buffer));
  }
  JSObject* ctor() { return getFixedSlot(Slot_Ctor).toObjectOrNull(); }
  uint32_t byteOffset() const {
    return getFixedSlot(Slot_ByteOffset).toInt32();
  }
  uint32_t byteLength() const {
    return getFixedSlot(Slot_ByteLength).toInt32();
  }
  uint32_t bytesFilled() const {
    return getFixedSlot(Slot_BytesFilled).toInt32();
  }
  void setBytesFilled(int32_t bytes) {
    setFixedSlot(Slot_BytesFilled, Int32Value(bytes));
  }
  uint32_t elementSize() const {
    return getFixedSlot(Slot_ElementSize).toInt32();
  }
  ReaderType readerType() const {
    int32_t n = getFixedSlot(Slot_ReaderType).toInt32();
    MOZ_ASSERT(n == int32_t(ReaderType::Default) ||
               n == int32_t(ReaderType::BYOB));
    return ReaderType(n);
  }

  static PullIntoDescriptor* create(JSContext* cx,
                                    JS::Handle<ArrayBufferObject*> buffer,
                                    uint32_t byteOffset, uint32_t byteLength,
                                    uint32_t bytesFilled, uint32_t elementSize,
                                    JS::Handle<JSObject*> ctor,
                                    ReaderType readerType);
};

}  // namespace js

#endif  // builtin_streams_PullIntoDescriptor_h
