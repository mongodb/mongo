/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* ReadableStream.prototype.pipeTo state. */

#ifndef builtin_streams_PipeToState_h
#define builtin_streams_PipeToState_h

#include "mozilla/Assertions.h"          // MOZ_ASSERT
#include "mozilla/WrappingOperations.h"  // mozilla::WrapToSigned

#include <stdint.h>  // uint32_t

#include "builtin/streams/ReadableStreamReader.h"  // js::ReadableStreamDefaultReader
#include "builtin/streams/WritableStreamDefaultWriter.h"  // js::WritableStreamDefaultWriter
#include "js/Class.h"                                     // JSClass
#include "js/RootingAPI.h"                                // JS::Handle
#include "js/Value.h"          // JS::Int32Value, JS::ObjectValue
#include "vm/NativeObject.h"   // js::NativeObject
#include "vm/PromiseObject.h"  // js::PromiseObject

class JS_PUBLIC_API JSObject;

namespace js {

class ReadableStream;
class WritableStream;

/**
 * PipeToState objects implement the local variables in Streams spec 3.4.11
 * ReadableStreamPipeTo across all sub-operations that occur in that algorithm.
 */
class PipeToState : public NativeObject {
 public:
  /**
   * Memory layout for PipeToState instances.
   */
  enum Slots {
    /** Integer bit field of various flags. */
    Slot_Flags = 0,

    /**
     * The promise resolved or rejected when the overall pipe-to operation
     * completes.
     *
     * This promise is created directly under |ReadableStreamPipeTo|, at the
     * same time the corresponding |PipeToState| is created, so it is always
     * same-compartment with this and is guaranteed to hold a |PromiseObject*|
     * if initialization succeeded.
     */
    Slot_Promise,

    /**
     * A |ReadableStreamDefaultReader| used to read from the readable stream
     * being piped from.
     *
     * This reader is created at the same time as its |PipeToState|, so this
     * reader is same-compartment with this and is guaranteed to be a
     * |ReadableStreamDefaultReader*| if initialization succeeds.
     */
    Slot_Reader,

    /**
     * A |WritableStreamDefaultWriter| used to write to the writable stream
     * being piped to.
     *
     * This writer is created at the same time as its |PipeToState|, so this
     * writer is same-compartment with this and is guaranteed to be a
     * |WritableStreamDefaultWriter*| if initialization succeeds.
     */
    Slot_Writer,

    /**
     * The |PromiseObject*| of the last write performed to the destinationg
     * |WritableStream| using the writer in |Slot_Writer|.  If no writes have
     * yet been performed, this slot contains |undefined|.
     *
     * This promise is created inside a handler function in the same compartment
     * and realm as this |PipeToState|, so it is always a |PromiseObject*| and
     * never a wrapper around one.
     */
    Slot_LastWriteRequest,

    /**
     * Either |undefined| or an |AbortSignal| instance specified by the user,
     * whose controller may be used to externally abort the piping algorithm.
     *
     * This signal is user-provided, so it may be a wrapper around an
     * |AbortSignal| not from the same compartment as this.
     */
    Slot_Signal,

    SlotCount,
  };

  // The set of possible actions to be passed to the "shutdown with an action"
  // algorithm.
  //
  // We store actions as numbers because 1) handler functions already devote
  // their extra slots to target and extra value; and 2) storing a full function
  // pointer would require an extra slot, while storing as number packs into
  // existing flag storage.
  enum class ShutdownAction {
    /** The action used during |abortAlgorithm|.*/
    AbortAlgorithm,

    /**
     * The action taken when |source| errors and aborting is not prevented, to
     * abort |dest| with |source|'s error.
     */
    AbortDestStream,

    /**
     * The action taken when |dest| becomes errored or closed and canceling is
     * not prevented, to cancel |source| with |dest|'s error.
     */
    CancelSource,

    /**
     * The action taken when |source| closes and closing is not prevented, to
     * close the writer while propagating any error in it.
     */
    CloseWriterWithErrorPropagation,

  };

 private:
  enum Flags : uint32_t {
    /**
     * The action passed to the "shutdown with an action" algorithm.
     *
     * Note that because only the first "shutdown" and "shutdown with an action"
     * operation has any effect, we can store this action in |PipeToState| in
     * the first invocation of either operation without worrying about it being
     * overwritten.
     *
     * Purely for convenience, we encode this in the lowest bits so that the
     * result of a mask is the underlying value of the correct |ShutdownAction|.
     */
    Flag_ShutdownActionBits = 0b0000'0011,

    Flag_ShuttingDown = 0b0000'0100,

    Flag_PendingRead = 0b0000'1000,
#ifdef DEBUG
    Flag_PendingReadWouldBeRejected = 0b0001'0000,
#endif

    Flag_PreventClose = 0b0010'0000,
    Flag_PreventAbort = 0b0100'0000,
    Flag_PreventCancel = 0b1000'0000,
  };

  uint32_t flags() const { return getFixedSlot(Slot_Flags).toInt32(); }
  void setFlags(uint32_t flags) {
    setFixedSlot(Slot_Flags, JS::Int32Value(mozilla::WrapToSigned(flags)));
  }

  // Flags start out zeroed, so the initially-stored shutdown action value will
  // be this value.  (This is also the value of an *initialized* shutdown
  // action, but it doesn't seem worth the trouble to store an extra bit to
  // detect this specific action being recorded multiple times, purely for
  // assertions.)
  static constexpr ShutdownAction UninitializedAction =
      ShutdownAction::AbortAlgorithm;

  static_assert(Flag_ShutdownActionBits & 1,
                "shutdown action bits must be low-order bits so that we can "
                "cast ShutdownAction values directly to bits to store");

  static constexpr uint32_t MaxAction =
      static_cast<uint32_t>(ShutdownAction::CloseWriterWithErrorPropagation);

  static_assert(MaxAction <= Flag_ShutdownActionBits,
                "max action shouldn't overflow available bits to store it");

 public:
  static const JSClass class_;

  PromiseObject* promise() const {
    return &getFixedSlot(Slot_Promise).toObject().as<PromiseObject>();
  }

  ReadableStreamDefaultReader* reader() const {
    return &getFixedSlot(Slot_Reader)
                .toObject()
                .as<ReadableStreamDefaultReader>();
  }

  WritableStreamDefaultWriter* writer() const {
    return &getFixedSlot(Slot_Writer)
                .toObject()
                .as<WritableStreamDefaultWriter>();
  }

  PromiseObject* lastWriteRequest() const {
    const auto& slot = getFixedSlot(Slot_LastWriteRequest);
    if (slot.isUndefined()) {
      return nullptr;
    }

    return &slot.toObject().as<PromiseObject>();
  }

  void updateLastWriteRequest(PromiseObject* writeRequest) {
    MOZ_ASSERT(writeRequest != nullptr);
    setFixedSlot(Slot_LastWriteRequest, JS::ObjectValue(*writeRequest));
  }

  bool hasSignal() const {
    JS::Value v = getFixedSlot(Slot_Signal);
    MOZ_ASSERT(v.isObject() || v.isUndefined());
    return v.isObject();
  }

  bool shuttingDown() const { return flags() & Flag_ShuttingDown; }
  void setShuttingDown() {
    MOZ_ASSERT(!shuttingDown());
    setFlags(flags() | Flag_ShuttingDown);
  }

  ShutdownAction shutdownAction() const {
    MOZ_ASSERT(shuttingDown(),
               "must be shutting down to have a shutdown action");

    uint32_t bits = flags() & Flag_ShutdownActionBits;
    static_assert(Flag_ShutdownActionBits & 1,
                  "shutdown action bits are assumed to be low-order bits that "
                  "don't have to be shifted down to ShutdownAction's range");

    MOZ_ASSERT(bits <= MaxAction, "bits must encode a valid action");

    return static_cast<ShutdownAction>(bits);
  }

  void setShutdownAction(ShutdownAction action) {
    MOZ_ASSERT(shuttingDown(),
               "must be protected by the |shuttingDown| boolean to save the "
               "shutdown action");
    MOZ_ASSERT(shutdownAction() == UninitializedAction,
               "should only set shutdown action once");

    setFlags(flags() | static_cast<uint32_t>(action));
  }

  bool preventClose() const { return flags() & Flag_PreventClose; }
  bool preventAbort() const { return flags() & Flag_PreventAbort; }
  bool preventCancel() const { return flags() & Flag_PreventCancel; }

  bool hasPendingRead() const { return flags() & Flag_PendingRead; }
  void setPendingRead() {
    MOZ_ASSERT(!hasPendingRead());
    setFlags(flags() | Flag_PendingRead);
  }
  void clearPendingRead() {
    MOZ_ASSERT(hasPendingRead());
    setFlags(flags() & ~Flag_PendingRead);
  }

#ifdef DEBUG
  bool pendingReadWouldBeRejected() const {
    return flags() & Flag_PendingReadWouldBeRejected;
  }
  void setPendingReadWouldBeRejected() {
    MOZ_ASSERT(!pendingReadWouldBeRejected());
    setFlags(flags() | Flag_PendingReadWouldBeRejected);
  }
#endif

  void initFlags(bool preventClose, bool preventAbort, bool preventCancel) {
    MOZ_ASSERT(getFixedSlot(Slot_Flags).isUndefined());

    uint32_t flagBits = (preventClose ? Flag_PreventClose : 0) |
                        (preventAbort ? Flag_PreventAbort : 0) |
                        (preventCancel ? Flag_PreventCancel : 0);
    setFlags(flagBits);
  }

  static PipeToState* create(JSContext* cx, JS::Handle<PromiseObject*> promise,
                             JS::Handle<ReadableStream*> unwrappedSource,
                             JS::Handle<WritableStream*> unwrappedDest,
                             bool preventClose, bool preventAbort,
                             bool preventCancel, JS::Handle<JSObject*> signal);
};

}  // namespace js

#endif  // builtin_streams_PipeToState_h
