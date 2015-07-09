/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/ErrorObject-inl.h"

#include "jsexn.h"

#include "vm/GlobalObject.h"

#include "jsobjinlines.h"

#include "vm/NativeObject-inl.h"
#include "vm/Shape-inl.h"

using namespace js;

/* static */ Shape*
js::ErrorObject::assignInitialShape(ExclusiveContext* cx, Handle<ErrorObject*> obj)
{
    MOZ_ASSERT(obj->empty());

    if (!obj->addDataProperty(cx, cx->names().fileName, FILENAME_SLOT, 0))
        return nullptr;
    if (!obj->addDataProperty(cx, cx->names().lineNumber, LINENUMBER_SLOT, 0))
        return nullptr;
    if (!obj->addDataProperty(cx, cx->names().columnNumber, COLUMNNUMBER_SLOT, 0))
        return nullptr;
    return obj->addDataProperty(cx, cx->names().stack, STACK_SLOT, 0);
}

/* static */ bool
js::ErrorObject::init(JSContext* cx, Handle<ErrorObject*> obj, JSExnType type,
                      ScopedJSFreePtr<JSErrorReport>* errorReport, HandleString fileName,
                      HandleString stack, uint32_t lineNumber, uint32_t columnNumber,
                      HandleString message)
{
    // Null out early in case of error, for exn_finalize's sake.
    obj->initReservedSlot(ERROR_REPORT_SLOT, PrivateValue(nullptr));

    if (!EmptyShape::ensureInitialCustomShape<ErrorObject>(cx, obj))
        return false;

    // The .message property isn't part of the initial shape because it's
    // present in some error objects -- |Error.prototype|, |new Error("f")|,
    // |new Error("")| -- but not in others -- |new Error(undefined)|,
    // |new Error()|.
    RootedShape messageShape(cx);
    if (message) {
        messageShape = obj->addDataProperty(cx, cx->names().message, MESSAGE_SLOT, 0);
        if (!messageShape)
            return false;
        MOZ_ASSERT(messageShape->slot() == MESSAGE_SLOT);
    }

    MOZ_ASSERT(obj->lookupPure(NameToId(cx->names().fileName))->slot() == FILENAME_SLOT);
    MOZ_ASSERT(obj->lookupPure(NameToId(cx->names().lineNumber))->slot() == LINENUMBER_SLOT);
    MOZ_ASSERT(obj->lookupPure(NameToId(cx->names().columnNumber))->slot() ==
               COLUMNNUMBER_SLOT);
    MOZ_ASSERT(obj->lookupPure(NameToId(cx->names().stack))->slot() == STACK_SLOT);
    MOZ_ASSERT_IF(message,
                  obj->lookupPure(NameToId(cx->names().message))->slot() == MESSAGE_SLOT);

    MOZ_ASSERT(JSEXN_ERR <= type && type < JSEXN_LIMIT);

    JSErrorReport* report = errorReport ? errorReport->forget() : nullptr;
    obj->initReservedSlot(EXNTYPE_SLOT, Int32Value(type));
    obj->setReservedSlot(ERROR_REPORT_SLOT, PrivateValue(report));
    obj->initReservedSlot(FILENAME_SLOT, StringValue(fileName));
    obj->initReservedSlot(LINENUMBER_SLOT, Int32Value(lineNumber));
    obj->initReservedSlot(COLUMNNUMBER_SLOT, Int32Value(columnNumber));
    obj->initReservedSlot(STACK_SLOT, StringValue(stack));
    if (message)
        obj->setSlotWithType(cx, messageShape, StringValue(message));

    return true;
}

/* static */ ErrorObject*
js::ErrorObject::create(JSContext* cx, JSExnType errorType, HandleString stack,
                        HandleString fileName, uint32_t lineNumber, uint32_t columnNumber,
                        ScopedJSFreePtr<JSErrorReport>* report, HandleString message)
{
    Rooted<JSObject*> proto(cx, GlobalObject::getOrCreateCustomErrorPrototype(cx, cx->global(), errorType));
    if (!proto)
        return nullptr;

    Rooted<ErrorObject*> errObject(cx);
    {
        const Class* clasp = ErrorObject::classForType(errorType);
        JSObject* obj = NewObjectWithGivenProto(cx, clasp, proto, NullPtr());
        if (!obj)
            return nullptr;
        errObject = &obj->as<ErrorObject>();
    }

    if (!ErrorObject::init(cx, errObject, errorType, report, fileName, stack,
                           lineNumber, columnNumber, message))
    {
        return nullptr;
    }

    return errObject;
}

JSErrorReport*
js::ErrorObject::getOrCreateErrorReport(JSContext* cx)
{
    if (JSErrorReport* r = getErrorReport())
        return r;

    // We build an error report on the stack and then use CopyErrorReport to do
    // the nitty-gritty malloc stuff.
    JSErrorReport report;

    // Type.
    JSExnType type_ = type();
    report.exnType = type_;

    // Filename.
    JSAutoByteString filenameStr;
    if (!filenameStr.encodeLatin1(cx, fileName(cx)))
        return nullptr;
    report.filename = filenameStr.ptr();

    // Coordinates.
    report.lineno = lineNumber();
    report.column = columnNumber();

    // Message. Note that |new Error()| will result in an undefined |message|
    // slot, so we need to explicitly substitute the empty string in that case.
    RootedString message(cx, getMessage());
    if (!message)
        message = cx->runtime()->emptyString;
    if (!message->ensureFlat(cx))
        return nullptr;
    AutoStableStringChars chars(cx);
    if (!chars.initTwoByte(cx, message))
        return nullptr;
    report.ucmessage = chars.twoByteRange().start().get();

    // Cache and return.
    JSErrorReport* copy = CopyErrorReport(cx, &report);
    if (!copy)
        return nullptr;
    setReservedSlot(ERROR_REPORT_SLOT, PrivateValue(copy));
    return copy;
}
