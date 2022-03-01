/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Class WritableStreamDefaultWriter. */

#ifndef builtin_streams_WritableStreamDefaultWriter_h
#define builtin_streams_WritableStreamDefaultWriter_h

#include "jstypes.h"          // JS_PUBLIC_API
#include "js/Class.h"         // JSClass, js::ClassSpec
#include "js/Value.h"         // JS::{,Object,Undefined}Value
#include "vm/NativeObject.h"  // js::NativeObject

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

namespace js {

class PromiseObject;
class WritableStream;

class WritableStreamDefaultWriter : public NativeObject {
 public:
  /**
   * Memory layout of Stream Writer instances.
   *
   * See https://streams.spec.whatwg.org/#default-writer-internal-slots for
   * details.
   */
  enum Slots {
    /**
     * A promise that is resolved when the stream this writes to becomes closed.
     *
     * This promise is ordinarily created while this writer is being created; in
     * this case this promise is not a wrapper and is same-compartment with
     * this.  However, if the writer is closed and then this writer releases its
     * lock on the stream, this promise will be recreated within whatever realm
     * is in force when the lock is released:
     *
     *   var ws = new WritableStream({});
     *   var w = ws.getWriter();
     *   var c = w.closed;
     *   w.close().then(() => {
     *     w.releaseLock(); // changes this slot, and |w.closed|
     *     assertEq(c === w.closed, false);
     *   });
     *
     * So this field *may* potentially contain a wrapper around a promise.
     */
    Slot_ClosedPromise,

    /**
     * The stream that this writer writes to.  Because writers are created under
     * |WritableStream.prototype.getWriter| which may not be same-compartment
     * with the stream, this is potentially a wrapper.
     */
    Slot_Stream,

    /**
     * The promise returned by the |writer.ready| getter property, a promise
     * signaling that the related stream is accepting writes.
     *
     * This value repeatedly changes as the related stream changes back and
     * forth between being writable and temporarily filled (or, ultimately,
     * errored or aborted).  These changes are invoked by a number of user-
     * visible functions, so this may be a wrapper around a promise in another
     * realm.
     */
    Slot_ReadyPromise,

    SlotCount,
  };

  JSObject* closedPromise() const {
    return &getFixedSlot(Slot_ClosedPromise).toObject();
  }
  void setClosedPromise(JSObject* wrappedPromise) {
    setFixedSlot(Slot_ClosedPromise, JS::ObjectValue(*wrappedPromise));
  }

  bool hasStream() const { return !getFixedSlot(Slot_Stream).isUndefined(); }
  void setStream(JSObject* stream) {
    setFixedSlot(Slot_Stream, JS::ObjectValue(*stream));
  }
  void clearStream() { setFixedSlot(Slot_Stream, JS::UndefinedValue()); }

  JSObject* readyPromise() const {
    return &getFixedSlot(Slot_ReadyPromise).toObject();
  }
  void setReadyPromise(JSObject* wrappedPromise) {
    setFixedSlot(Slot_ReadyPromise, JS::ObjectValue(*wrappedPromise));
  }

  static bool constructor(JSContext* cx, unsigned argc, JS::Value* vp);
  static const ClassSpec classSpec_;
  static const JSClass class_;
  static const ClassSpec protoClassSpec_;
  static const JSClass protoClass_;
};

[[nodiscard]] extern WritableStreamDefaultWriter*
CreateWritableStreamDefaultWriter(JSContext* cx,
                                  JS::Handle<WritableStream*> unwrappedStream,
                                  JS::Handle<JSObject*> proto = nullptr);

}  // namespace js

#endif  // builtin_streams_WritableStreamDefaultWriter_h
