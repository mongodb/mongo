/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ErrorObject_h_
#define vm_ErrorObject_h_

#include "mozilla/Assertions.h"
#include "mozilla/Maybe.h"

#include <iterator>
#include <stdint.h>

#include "jspubtd.h"
#include "NamespaceImports.h"

#include "js/Class.h"
#include "js/ColumnNumber.h"  // JS::ColumnNumberOneOrigin
#include "js/ErrorReport.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
#include "js/Value.h"
#include "vm/JSObject.h"
#include "vm/NativeObject.h"

namespace js {

class ErrorObject : public NativeObject {
  static JSObject* createProto(JSContext* cx, JSProtoKey key);

  static JSObject* createConstructor(JSContext* cx, JSProtoKey key);

  static bool init(JSContext* cx, Handle<ErrorObject*> obj, JSExnType type,
                   UniquePtr<JSErrorReport> errorReport, HandleString fileName,
                   HandleObject stack, uint32_t sourceId, uint32_t lineNumber,
                   JS::ColumnNumberOneOrigin columnNumber, HandleString message,
                   Handle<mozilla::Maybe<JS::Value>> cause);

  static const ClassSpec classSpecs[JSEXN_ERROR_LIMIT];
  static const JSClass protoClasses[JSEXN_ERROR_LIMIT];

 protected:
  static const uint32_t STACK_SLOT = 0;
  static const uint32_t ERROR_REPORT_SLOT = STACK_SLOT + 1;
  static const uint32_t FILENAME_SLOT = ERROR_REPORT_SLOT + 1;
  static const uint32_t LINENUMBER_SLOT = FILENAME_SLOT + 1;
  static const uint32_t COLUMNNUMBER_SLOT = LINENUMBER_SLOT + 1;
  static const uint32_t MESSAGE_SLOT = COLUMNNUMBER_SLOT + 1;
  static const uint32_t CAUSE_SLOT = MESSAGE_SLOT + 1;
  static const uint32_t SOURCEID_SLOT = CAUSE_SLOT + 1;

  static const uint32_t RESERVED_SLOTS = SOURCEID_SLOT + 1;

  // This slot is only used for errors that could be Wasm traps.
  static const uint32_t WASM_TRAP_SLOT = SOURCEID_SLOT + 1;
  static const uint32_t RESERVED_SLOTS_MAYBE_WASM_TRAP = WASM_TRAP_SLOT + 1;

 public:
  static const JSClass classes[JSEXN_ERROR_LIMIT];

  static const JSClass* classForType(JSExnType type) {
    MOZ_ASSERT(type < JSEXN_ERROR_LIMIT);
    return &classes[type];
  }

  static bool isErrorClass(const JSClass* clasp) {
    return &classes[0] <= clasp && clasp < &classes[0] + std::size(classes);
  }

  // Create an error of the given type corresponding to the provided location
  // info.  If |message| is non-null, then the error will have a .message
  // property with that value; otherwise the error will have no .message
  // property.
  static ErrorObject* create(JSContext* cx, JSExnType type, HandleObject stack,
                             HandleString fileName, uint32_t sourceId,
                             uint32_t lineNumber,
                             JS::ColumnNumberOneOrigin columnNumber,
                             UniquePtr<JSErrorReport> report,
                             HandleString message,
                             Handle<mozilla::Maybe<JS::Value>> cause,
                             HandleObject proto = nullptr);

  /*
   * Assign the initial error shape to the empty object.  (This shape does
   * *not* include .message, which must be added separately if needed; see
   * ErrorObject::init.)
   */
  static SharedShape* assignInitialShape(JSContext* cx,
                                         Handle<ErrorObject*> obj);

  JSExnType type() const {
    MOZ_ASSERT(isErrorClass(getClass()));
    return static_cast<JSExnType>(getClass() - &classes[0]);
  }

  JSErrorReport* getErrorReport() const {
    const Value& slot = getReservedSlot(ERROR_REPORT_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return static_cast<JSErrorReport*>(slot.toPrivate());
  }

  JSErrorReport* getOrCreateErrorReport(JSContext* cx);

  inline JSString* fileName(JSContext* cx) const;
  inline uint32_t sourceId() const;

  // Line number (1-origin).
  inline uint32_t lineNumber() const;

  // Column number in UTF-16 code units.
  inline JS::ColumnNumberOneOrigin columnNumber() const;

  // Returns nullptr or a (possibly wrapped) SavedFrame object.
  inline JSObject* stack() const;

  JSString* getMessage() const {
    Value val = getReservedSlot(MESSAGE_SLOT);
    return val.isString() ? val.toString() : nullptr;
  }

  /*
   * Return Nothing if the error was created without an initial cause or if the
   * initial cause data property has been redefined to an accessor property.
   */
  mozilla::Maybe<Value> getCause() const {
    const auto& value = getReservedSlot(CAUSE_SLOT);
    if (value.isMagic(JS_ERROR_WITHOUT_CAUSE) || value.isPrivateGCThing()) {
      return mozilla::Nothing();
    }
    return mozilla::Some(value);
  }

  void setStackSlot(const Value& stack) {
    MOZ_ASSERT(stack.isObjectOrNull());
    setReservedSlot(STACK_SLOT, stack);
  }

  void setCauseSlot(const Value& cause) {
    MOZ_ASSERT(!cause.isMagic());
    MOZ_ASSERT(getCause().isSome());
    setReservedSlot(CAUSE_SLOT, cause);
  }

  // Getter and setter for the Error.prototype.stack accessor.
  static bool getStack(JSContext* cx, unsigned argc, Value* vp);
  static bool getStack_impl(JSContext* cx, const CallArgs& args);
  static bool setStack(JSContext* cx, unsigned argc, Value* vp);
  static bool setStack_impl(JSContext* cx, const CallArgs& args);

  // Used to distinguish errors created from Wasm traps.
  bool mightBeWasmTrap() const {
    return type() == JSEXN_WASMRUNTIMEERROR || type() == JSEXN_INTERNALERR;
  }
  bool fromWasmTrap() const {
    if (!mightBeWasmTrap()) {
      return false;
    } else {
      MOZ_ASSERT(JSCLASS_RESERVED_SLOTS(getClass()) > WASM_TRAP_SLOT);
      return getReservedSlot(WASM_TRAP_SLOT).toBoolean();
    }
  }
  void setFromWasmTrap();
};

JSString* ErrorToSource(JSContext* cx, HandleObject obj);

}  // namespace js

template <>
inline bool JSObject::is<js::ErrorObject>() const {
  return js::ErrorObject::isErrorClass(getClass());
}

#endif  // vm_ErrorObject_h_
