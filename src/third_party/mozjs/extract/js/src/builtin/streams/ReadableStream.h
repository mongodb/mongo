/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Class ReadableStream. */

#ifndef builtin_streams_ReadableStream_h
#define builtin_streams_ReadableStream_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT{,_IF}

#include <stdint.h>  // uint32_t

#include "jstypes.h"        // JS_PUBLIC_API
#include "js/Class.h"       // JSClass, js::ClassSpec
#include "js/RootingAPI.h"  // JS::Handle
#include "js/Stream.h"      // JS::ReadableStream{Mode,UnderlyingSource}
#include "js/Value.h"  // JS::Int32Value, JS::ObjectValue, JS::UndefinedValue
#include "vm/NativeObject.h"  // js::NativeObject

class JS_PUBLIC_API JSObject;

namespace js {

class ReadableStreamController;

class ReadableStream : public NativeObject {
 public:
  /**
   * Memory layout of Stream instances.
   *
   * See https://streams.spec.whatwg.org/#rs-internal-slots for details on
   * the stored state. [[state]] and [[disturbed]] are stored in
   * StreamSlot_State as ReadableStream::State enum values.
   *
   * Of the stored values, Reader and StoredError might be cross-compartment
   * wrappers. This can happen if the Reader was created by applying a
   * different compartment's ReadableStream.prototype.getReader method.
   *
   * A stream's associated controller is always created from under the
   * stream's constructor and thus cannot be in a different compartment.
   */
  enum Slots {
    Slot_Controller,
    Slot_Reader,
    Slot_State,
    Slot_StoredError,
    SlotCount
  };

 private:
  enum StateBits {
    Readable = 0,
    Closed = 1,
    Errored = 2,
    StateMask = 0x000000ff,
    Disturbed = 0x00000100
  };

  uint32_t stateBits() const { return getFixedSlot(Slot_State).toInt32(); }
  void initStateBits(uint32_t stateBits) {
    MOZ_ASSERT((stateBits & ~Disturbed) <= Errored);
    setFixedSlot(Slot_State, JS::Int32Value(stateBits));
  }
  void setStateBits(uint32_t stateBits) {
#ifdef DEBUG
    bool wasDisturbed = disturbed();
    bool wasClosedOrErrored = closed() || errored();
#endif
    initStateBits(stateBits);
    MOZ_ASSERT_IF(wasDisturbed, disturbed());
    MOZ_ASSERT_IF(wasClosedOrErrored, !readable());
  }

  StateBits state() const { return StateBits(stateBits() & StateMask); }
  void setState(StateBits state) {
    MOZ_ASSERT(state <= Errored);
    uint32_t current = stateBits() & ~StateMask;
    setStateBits(current | state);
  }

 public:
  bool readable() const { return state() == Readable; }
  bool closed() const { return state() == Closed; }
  void setClosed() { setState(Closed); }
  bool errored() const { return state() == Errored; }
  void setErrored() { setState(Errored); }
  bool disturbed() const { return stateBits() & Disturbed; }
  void setDisturbed() { setStateBits(stateBits() | Disturbed); }

  bool hasController() const {
    return !getFixedSlot(Slot_Controller).isUndefined();
  }
  inline ReadableStreamController* controller() const;
  inline void setController(ReadableStreamController* controller);
  void clearController() {
    setFixedSlot(Slot_Controller, JS::UndefinedValue());
  }

  bool hasReader() const { return !getFixedSlot(Slot_Reader).isUndefined(); }
  void setReader(JSObject* reader) {
    setFixedSlot(Slot_Reader, JS::ObjectValue(*reader));
  }
  void clearReader() { setFixedSlot(Slot_Reader, JS::UndefinedValue()); }

  JS::Value storedError() const { return getFixedSlot(Slot_StoredError); }
  void setStoredError(JS::Handle<JS::Value> value) {
    setFixedSlot(Slot_StoredError, value);
  }

  JS::ReadableStreamMode mode() const;

  bool locked() const;

  [[nodiscard]] static ReadableStream* create(
      JSContext* cx, void* nsISupportsObject_alreadyAddreffed = nullptr,
      JS::Handle<JSObject*> proto = nullptr);
  static ReadableStream* createExternalSourceStream(
      JSContext* cx, JS::ReadableStreamUnderlyingSource* source,
      void* nsISupportsObject_alreadyAddreffed = nullptr,
      JS::Handle<JSObject*> proto = nullptr);

  static bool constructor(JSContext* cx, unsigned argc, Value* vp);
  static const ClassSpec classSpec_;
  static const JSClass class_;
  static const ClassSpec protoClassSpec_;
  static const JSClass protoClass_;
};

[[nodiscard]] extern bool SetUpExternalReadableByteStreamController(
    JSContext* cx, JS::Handle<ReadableStream*> stream,
    JS::ReadableStreamUnderlyingSource* source);

}  // namespace js

#endif  // builtin_streams_ReadableStream_h
