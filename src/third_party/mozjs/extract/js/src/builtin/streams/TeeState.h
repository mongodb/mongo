/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Stream teeing state. */

#ifndef builtin_streams_TeeState_h
#define builtin_streams_TeeState_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include <stdint.h>  // uint32_t

#include "builtin/streams/ReadableStreamController.h"  // js::ReadableStreamDefaultController
#include "js/Class.h"                                  // JSClass
#include "js/Value.h"          // JS::{Int32,Object}Value
#include "vm/NativeObject.h"   // js::NativeObject
#include "vm/PromiseObject.h"  // js::PromiseObject

namespace js {

/**
 * TeeState objects implement the local variables in Streams spec 3.3.9
 * ReadableStreamTee, which are accessed by several algorithms.
 */
class TeeState : public NativeObject {
 public:
  /**
   * Memory layout for TeeState instances.
   *
   * The Reason1 and Reason2 slots store opaque values, which might be
   * wrapped objects from other compartments. Since we don't treat them as
   * objects in Streams-specific code, we don't have to worry about that
   * apart from ensuring that the values are properly wrapped before storing
   * them.
   *
   * CancelPromise is always created in TeeState::create below, so is
   * guaranteed to be in the same compartment as the TeeState instance
   * itself.
   *
   * Stream can be from another compartment. It is automatically wrapped
   * before storing it and unwrapped upon retrieval. That means that
   * TeeState consumers need to be able to deal with unwrapped
   * ReadableStream instances from non-current compartments.
   *
   * Branch1 and Branch2 are always created in the same compartment as the
   * TeeState instance, so cannot be from another compartment.
   */
  enum Slots {
    Slot_Flags = 0,
    Slot_Reason1,
    Slot_Reason2,
    Slot_CancelPromise,
    Slot_Stream,
    Slot_Branch1,
    Slot_Branch2,
    SlotCount
  };

 private:
  enum Flags {
    Flag_Reading = 1 << 0,
    Flag_Canceled1 = 1 << 1,
    Flag_Canceled2 = 1 << 2,

    // No internal user ever sets the cloneForBranch2 flag to true, and the
    // streams spec doesn't expose a way to set the flag to true.  So for the
    // moment, don't even reserve flag-space to store it.
    // Flag_CloneForBranch2 = 1 << 3,
  };
  uint32_t flags() const { return getFixedSlot(Slot_Flags).toInt32(); }
  void setFlags(uint32_t flags) {
    setFixedSlot(Slot_Flags, JS::Int32Value(flags));
  }

 public:
  static const JSClass class_;

  // Consistent with not even storing this always-false flag, expose it as
  // compile-time constant false.
  static constexpr bool cloneForBranch2() { return false; }

  bool reading() const { return flags() & Flag_Reading; }
  void setReading() {
    MOZ_ASSERT(!(flags() & Flag_Reading));
    setFlags(flags() | Flag_Reading);
  }
  void unsetReading() {
    MOZ_ASSERT(flags() & Flag_Reading);
    setFlags(flags() & ~Flag_Reading);
  }

  bool canceled1() const { return flags() & Flag_Canceled1; }
  void setCanceled1(HandleValue reason) {
    MOZ_ASSERT(!(flags() & Flag_Canceled1));
    setFlags(flags() | Flag_Canceled1);
    setFixedSlot(Slot_Reason1, reason);
  }

  bool canceled2() const { return flags() & Flag_Canceled2; }
  void setCanceled2(HandleValue reason) {
    MOZ_ASSERT(!(flags() & Flag_Canceled2));
    setFlags(flags() | Flag_Canceled2);
    setFixedSlot(Slot_Reason2, reason);
  }

  JS::Value reason1() const {
    MOZ_ASSERT(canceled1());
    return getFixedSlot(Slot_Reason1);
  }

  JS::Value reason2() const {
    MOZ_ASSERT(canceled2());
    return getFixedSlot(Slot_Reason2);
  }

  PromiseObject* cancelPromise() {
    return &getFixedSlot(Slot_CancelPromise).toObject().as<PromiseObject>();
  }

  ReadableStreamDefaultController* branch1() {
    ReadableStreamDefaultController* controller =
        &getFixedSlot(Slot_Branch1)
             .toObject()
             .as<ReadableStreamDefaultController>();
    MOZ_ASSERT(controller->isTeeBranch1());
    return controller;
  }
  void setBranch1(ReadableStreamDefaultController* controller) {
    MOZ_ASSERT(controller->isTeeBranch1());
    setFixedSlot(Slot_Branch1, JS::ObjectValue(*controller));
  }

  ReadableStreamDefaultController* branch2() {
    ReadableStreamDefaultController* controller =
        &getFixedSlot(Slot_Branch2)
             .toObject()
             .as<ReadableStreamDefaultController>();
    MOZ_ASSERT(controller->isTeeBranch2());
    return controller;
  }
  void setBranch2(ReadableStreamDefaultController* controller) {
    MOZ_ASSERT(controller->isTeeBranch2());
    setFixedSlot(Slot_Branch2, JS::ObjectValue(*controller));
  }

  static TeeState* create(JSContext* cx,
                          Handle<ReadableStream*> unwrappedStream);
};

}  // namespace js

#endif  // builtin_streams_TeeState_h
