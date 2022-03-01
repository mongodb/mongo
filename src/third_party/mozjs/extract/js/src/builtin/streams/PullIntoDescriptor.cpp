/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Pull descriptor objects for tracking byte stream pull-into requests. */

#include "builtin/streams/PullIntoDescriptor.h"

#include <stdint.h>  // uint32_t

#include "js/Class.h"       // JSClass, JSCLASS_HAS_RESERVED_SLOTS
#include "js/RootingAPI.h"  // JS::Handle, JS::Rooted

#include "vm/JSObject-inl.h"  // js::NewBuiltinClassInstance

using js::PullIntoDescriptor;

using JS::Handle;
using JS::Int32Value;
using JS::ObjectOrNullValue;
using JS::ObjectValue;
using JS::Rooted;

/* static */ PullIntoDescriptor* PullIntoDescriptor::create(
    JSContext* cx, Handle<ArrayBufferObject*> buffer, uint32_t byteOffset,
    uint32_t byteLength, uint32_t bytesFilled, uint32_t elementSize,
    Handle<JSObject*> ctor, ReaderType readerType) {
  Rooted<PullIntoDescriptor*> descriptor(
      cx, NewBuiltinClassInstance<PullIntoDescriptor>(cx));
  if (!descriptor) {
    return nullptr;
  }

  descriptor->setFixedSlot(Slot_buffer, ObjectValue(*buffer));
  descriptor->setFixedSlot(Slot_Ctor, ObjectOrNullValue(ctor));
  descriptor->setFixedSlot(Slot_ByteOffset, Int32Value(byteOffset));
  descriptor->setFixedSlot(Slot_ByteLength, Int32Value(byteLength));
  descriptor->setFixedSlot(Slot_BytesFilled, Int32Value(bytesFilled));
  descriptor->setFixedSlot(Slot_ElementSize, Int32Value(elementSize));
  descriptor->setFixedSlot(Slot_ReaderType,
                           Int32Value(static_cast<int32_t>(readerType)));
  return descriptor;
}

const JSClass PullIntoDescriptor::class_ = {
    "PullIntoDescriptor", JSCLASS_HAS_RESERVED_SLOTS(SlotCount)};
