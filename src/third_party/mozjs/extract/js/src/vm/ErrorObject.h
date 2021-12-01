/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ErrorObject_h_
#define vm_ErrorObject_h_

#include "mozilla/ArrayUtils.h"

#include "vm/NativeObject.h"
#include "vm/SavedStacks.h"
#include "vm/Shape.h"

namespace js {

/*
 * Initialize the exception constructor/prototype hierarchy.
 */
extern JSObject*
InitExceptionClasses(JSContext* cx, HandleObject obj);

class ErrorObject : public NativeObject
{
    static JSObject*
    createProto(JSContext* cx, JSProtoKey key);

    static JSObject*
    createConstructor(JSContext* cx, JSProtoKey key);

    /* For access to createProto. */
    friend JSObject*
    js::InitExceptionClasses(JSContext* cx, HandleObject global);

    static bool
    init(JSContext* cx, Handle<ErrorObject*> obj, JSExnType type,
         ScopedJSFreePtr<JSErrorReport>* errorReport, HandleString fileName, HandleObject stack,
         uint32_t lineNumber, uint32_t columnNumber, HandleString message);

    static const ClassSpec classSpecs[JSEXN_ERROR_LIMIT];
    static const Class protoClasses[JSEXN_ERROR_LIMIT];

  protected:
    static const uint32_t EXNTYPE_SLOT          = 0;
    static const uint32_t STACK_SLOT            = EXNTYPE_SLOT + 1;
    static const uint32_t ERROR_REPORT_SLOT     = STACK_SLOT + 1;
    static const uint32_t FILENAME_SLOT         = ERROR_REPORT_SLOT + 1;
    static const uint32_t LINENUMBER_SLOT       = FILENAME_SLOT + 1;
    static const uint32_t COLUMNNUMBER_SLOT     = LINENUMBER_SLOT + 1;
    static const uint32_t MESSAGE_SLOT          = COLUMNNUMBER_SLOT + 1;

    static const uint32_t RESERVED_SLOTS = MESSAGE_SLOT + 1;

  public:
    static const Class classes[JSEXN_ERROR_LIMIT];

    static const Class * classForType(JSExnType type) {
        MOZ_ASSERT(type < JSEXN_WARN);
        return &classes[type];
    }

    static bool isErrorClass(const Class* clasp) {
        return &classes[0] <= clasp && clasp < &classes[0] + mozilla::ArrayLength(classes);
    }

    // Create an error of the given type corresponding to the provided location
    // info.  If |message| is non-null, then the error will have a .message
    // property with that value; otherwise the error will have no .message
    // property.
    static ErrorObject*
    create(JSContext* cx, JSExnType type, HandleObject stack, HandleString fileName,
           uint32_t lineNumber, uint32_t columnNumber, ScopedJSFreePtr<JSErrorReport>* report,
           HandleString message, HandleObject proto = nullptr);

    /*
     * Assign the initial error shape to the empty object.  (This shape does
     * *not* include .message, which must be added separately if needed; see
     * ErrorObject::init.)
     */
    static Shape*
    assignInitialShape(JSContext* cx, Handle<ErrorObject*> obj);

    JSExnType type() const {
        return JSExnType(getReservedSlot(EXNTYPE_SLOT).toInt32());
    }

    JSErrorReport * getErrorReport() const {
        const Value& slot = getReservedSlot(ERROR_REPORT_SLOT);
        if (slot.isUndefined())
            return nullptr;
        return static_cast<JSErrorReport*>(slot.toPrivate());
    }

    JSErrorReport * getOrCreateErrorReport(JSContext* cx);

    inline JSString * fileName(JSContext* cx) const;
    inline uint32_t lineNumber() const;
    inline uint32_t columnNumber() const;
    inline JSObject * stack() const;

    JSString * getMessage() const {
        const HeapSlot& slot = getReservedSlotRef(MESSAGE_SLOT);
        return slot.isString() ? slot.toString() : nullptr;
    }

    // Getter and setter for the Error.prototype.stack accessor.
    static bool getStack(JSContext* cx, unsigned argc, Value* vp);
    static bool getStack_impl(JSContext* cx, const CallArgs& args);
    static bool setStack(JSContext* cx, unsigned argc, Value* vp);
    static bool setStack_impl(JSContext* cx, const CallArgs& args);
};

} // namespace js

template<>
inline bool
JSObject::is<js::ErrorObject>() const
{
    return js::ErrorObject::isErrorClass(getClass());
}

#endif // vm_ErrorObject_h_
