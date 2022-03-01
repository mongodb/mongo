/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Base class for readable and writable stream controllers. */

#ifndef builtin_streams_StreamController_h
#define builtin_streams_StreamController_h

#include "js/Value.h"         // JS::Value, JS::NumberValue
#include "vm/JSObject.h"      // JSObject
#include "vm/List.h"          // js::ListObject
#include "vm/NativeObject.h"  // js::NativeObject

namespace js {

/**
 * Common base class of both readable and writable stream controllers.
 */
class StreamController : public NativeObject {
 public:
  /**
   * Memory layout for stream controllers.
   *
   * Both ReadableStreamDefaultController and ReadableByteStreamController
   * are queue containers and must have these slots at identical offsets.
   *
   * The queue is guaranteed to be in the same compartment as the container,
   * but might contain wrappers for objects from other compartments.
   */
  enum Slots { Slot_Queue, Slot_TotalSize, SlotCount };

  ListObject* queue() const {
    return &getFixedSlot(Slot_Queue).toObject().as<ListObject>();
  }
  double queueTotalSize() const {
    return getFixedSlot(Slot_TotalSize).toNumber();
  }
  void setQueueTotalSize(double size) {
    setFixedSlot(Slot_TotalSize, JS::NumberValue(size));
  }
};

}  // namespace js

template <>
inline bool JSObject::is<js::StreamController>() const;

#endif  // builtin_streams_StreamController_h
