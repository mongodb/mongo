/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* ReadableStream readers and generic reader operations. */

#ifndef builtin_streams_ReadableStreamReader_h
#define builtin_streams_ReadableStreamReader_h

#include "jstypes.h"          // JS_PUBLIC_API
#include "js/Class.h"         // JSClass, js::ClassSpec
#include "js/RootingAPI.h"    // JS::Handle
#include "js/Value.h"         // JS::{,Boolean,Object,Undefined}Value
#include "vm/JSObject.h"      // JSObject::is
#include "vm/List.h"          // js::ListObject
#include "vm/NativeObject.h"  // js::NativeObject

struct JS_PUBLIC_API JSContext;

namespace js {

class PromiseObject;
class ReadableStream;

/**
 * Tells whether or not read() result objects inherit from Object.prototype.
 * Generally, they should do so only if the reader was created by author code.
 * See <https://streams.spec.whatwg.org/#readable-stream-create-read-result>.
 */
enum class ForAuthorCodeBool { No, Yes };

class ReadableStreamReader : public NativeObject {
 public:
  /**
   * Memory layout of Stream Reader instances.
   *
   * See https://streams.spec.whatwg.org/#default-reader-internal-slots and
   * https://streams.spec.whatwg.org/#byob-reader-internal-slots for details.
   *
   * Note that [[readRequests]] and [[readIntoRequests]] are treated the same
   * in our implementation.
   *
   * Of the stored values, Stream and ClosedPromise might be
   * cross-compartment wrapper wrappers.
   *
   * For Stream, this can happen if the Reader was created by applying a
   * different compartment's ReadableStream.prototype.getReader method.
   *
   * For ClosedPromise, it can be caused by applying a different
   * compartment's ReadableStream*Reader.prototype.releaseLock method.
   *
   * Requests is guaranteed to be in the same compartment as the Reader, but
   * can contain wrapped request objects from other globals.
   */
  enum Slots {
    Slot_Stream,
    Slot_Requests,
    Slot_ClosedPromise,
    Slot_ForAuthorCode,
    SlotCount,
  };

  bool hasStream() const { return !getFixedSlot(Slot_Stream).isUndefined(); }
  void setStream(JSObject* stream) {
    setFixedSlot(Slot_Stream, JS::ObjectValue(*stream));
  }
  void clearStream() { setFixedSlot(Slot_Stream, JS::UndefinedValue()); }
  bool isClosed() { return !hasStream(); }

  /**
   * Tells whether this reader was created by author code.
   *
   * This returns Yes for readers created using `stream.getReader()`, and No
   * for readers created for the internal use of algorithms like
   * `stream.tee()` and `new Response(stream)`.
   *
   * The standard does not have this field. Instead, eight algorithms take a
   * forAuthorCode parameter, and a [[forAuthorCode]] field is part of each
   * read request. But the behavior is always equivalent to treating readers
   * created by author code as having a bit set on them. We implement it that
   * way for simplicity.
   */
  ForAuthorCodeBool forAuthorCode() const {
    return getFixedSlot(Slot_ForAuthorCode).toBoolean() ? ForAuthorCodeBool::Yes
                                                        : ForAuthorCodeBool::No;
  }
  void setForAuthorCode(ForAuthorCodeBool value) {
    setFixedSlot(Slot_ForAuthorCode,
                 JS::BooleanValue(value == ForAuthorCodeBool::Yes));
  }

  ListObject* requests() const {
    return &getFixedSlot(Slot_Requests).toObject().as<ListObject>();
  }
  void clearRequests() { setFixedSlot(Slot_Requests, JS::UndefinedValue()); }

  JSObject* closedPromise() const {
    return &getFixedSlot(Slot_ClosedPromise).toObject();
  }
  void setClosedPromise(JSObject* wrappedPromise) {
    setFixedSlot(Slot_ClosedPromise, JS::ObjectValue(*wrappedPromise));
  }

  static const JSClass class_;
};

class ReadableStreamDefaultReader : public ReadableStreamReader {
 public:
  static bool constructor(JSContext* cx, unsigned argc, JS::Value* vp);
  static const ClassSpec classSpec_;
  static const JSClass class_;
  static const ClassSpec protoClassSpec_;
  static const JSClass protoClass_;
};

[[nodiscard]] extern ReadableStreamDefaultReader*
CreateReadableStreamDefaultReader(JSContext* cx,
                                  JS::Handle<ReadableStream*> unwrappedStream,
                                  ForAuthorCodeBool forAuthorCode,
                                  JS::Handle<JSObject*> proto = nullptr);

[[nodiscard]] extern JSObject* ReadableStreamReaderGenericCancel(
    JSContext* cx, JS::Handle<ReadableStreamReader*> unwrappedReader,
    JS::Handle<JS::Value> reason);

[[nodiscard]] extern bool ReadableStreamReaderGenericInitialize(
    JSContext* cx, JS::Handle<ReadableStreamReader*> reader,
    JS::Handle<ReadableStream*> unwrappedStream,
    ForAuthorCodeBool forAuthorCode);

[[nodiscard]] extern bool ReadableStreamReaderGenericRelease(
    JSContext* cx, JS::Handle<ReadableStreamReader*> unwrappedReader);

[[nodiscard]] extern PromiseObject* ReadableStreamDefaultReaderRead(
    JSContext* cx, JS::Handle<ReadableStreamDefaultReader*> unwrappedReader);

}  // namespace js

template <>
inline bool JSObject::is<js::ReadableStreamReader>() const {
  return is<js::ReadableStreamDefaultReader>();
}

namespace js {

[[nodiscard]] extern JSObject* CreateReadableStreamBYOBReader(
    JSContext* cx, JS::Handle<ReadableStream*> unwrappedStream,
    ForAuthorCodeBool forAuthorCode, JS::Handle<JSObject*> proto = nullptr);

}  // namespace js

#endif  // builtin_streams_ReadableStreamReader_h
