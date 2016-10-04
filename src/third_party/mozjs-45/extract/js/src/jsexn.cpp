/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS standard exception implementation.
 */

#include "jsexn.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/PodOperations.h"

#include <string.h>

#include "jsapi.h"
#include "jscntxt.h"
#include "jsfun.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsprf.h"
#include "jsscript.h"
#include "jstypes.h"
#include "jsutil.h"
#include "jswrapper.h"

#include "gc/Marking.h"
#include "vm/ErrorObject.h"
#include "vm/GlobalObject.h"
#include "vm/SavedStacks.h"
#include "vm/StringBuffer.h"

#include "jsobjinlines.h"

#include "vm/ErrorObject-inl.h"
#include "vm/SavedStacks-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::ArrayLength;
using mozilla::PodArrayZero;

static void
exn_finalize(FreeOp* fop, JSObject* obj);

bool
Error(JSContext* cx, unsigned argc, Value* vp);

static bool
exn_toSource(JSContext* cx, unsigned argc, Value* vp);

static const JSPropertySpec exception_properties[] = {
    JS_PSGS("stack", ErrorObject::getStack, ErrorObject::setStack, 0),
    JS_PS_END
};

static const JSFunctionSpec exception_methods[] = {
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str, exn_toSource, 0, 0),
#endif
    JS_SELF_HOSTED_FN(js_toString_str, "ErrorToString", 0,0),
    JS_FS_END
};

#define IMPLEMENT_ERROR_SUBCLASS(name) \
    { \
        js_Error_str, /* yes, really */ \
        JSCLASS_HAS_CACHED_PROTO(JSProto_##name) | \
        JSCLASS_HAS_RESERVED_SLOTS(ErrorObject::RESERVED_SLOTS), \
        nullptr,                 /* addProperty */ \
        nullptr,                 /* delProperty */ \
        nullptr,                 /* getProperty */ \
        nullptr,                 /* setProperty */ \
        nullptr,                 /* enumerate */ \
        nullptr,                 /* resolve */ \
        nullptr,                 /* mayResolve */ \
        exn_finalize, \
        nullptr,                 /* call        */ \
        nullptr,                 /* hasInstance */ \
        nullptr,                 /* construct   */ \
        nullptr,                 /* trace       */ \
        { \
            ErrorObject::createConstructor, \
            ErrorObject::createProto, \
            nullptr, \
            nullptr, \
            exception_methods, \
            exception_properties, \
            nullptr, \
            JSProto_Error \
        } \
    }

const Class
ErrorObject::classes[JSEXN_LIMIT] = {
    {
        js_Error_str,
        JSCLASS_HAS_CACHED_PROTO(JSProto_Error) |
        JSCLASS_HAS_RESERVED_SLOTS(ErrorObject::RESERVED_SLOTS),
        nullptr,                 /* addProperty */
        nullptr,                 /* delProperty */
        nullptr,                 /* getProperty */
        nullptr,                 /* setProperty */
        nullptr,                 /* enumerate */
        nullptr,                 /* resolve */
        nullptr,                 /* mayResolve */
        exn_finalize,
        nullptr,                 /* call        */
        nullptr,                 /* hasInstance */
        nullptr,                 /* construct   */
        nullptr,                 /* trace       */
        {
            ErrorObject::createConstructor,
            ErrorObject::createProto,
            nullptr,
            nullptr,
            exception_methods,
            exception_properties,
            nullptr
        }
    },
    IMPLEMENT_ERROR_SUBCLASS(InternalError),
    IMPLEMENT_ERROR_SUBCLASS(EvalError),
    IMPLEMENT_ERROR_SUBCLASS(RangeError),
    IMPLEMENT_ERROR_SUBCLASS(ReferenceError),
    IMPLEMENT_ERROR_SUBCLASS(SyntaxError),
    IMPLEMENT_ERROR_SUBCLASS(TypeError),
    IMPLEMENT_ERROR_SUBCLASS(URIError)
};

JSErrorReport*
js::CopyErrorReport(JSContext* cx, JSErrorReport* report)
{
    /*
     * We use a single malloc block to make a deep copy of JSErrorReport with
     * the following layout:
     *   JSErrorReport
     *   array of copies of report->messageArgs
     *   char16_t array with characters for all messageArgs
     *   char16_t array with characters for ucmessage
     *   char16_t array with characters for uclinebuf and uctokenptr
     *   char array with characters for linebuf and tokenptr
     *   char array with characters for filename
     * Such layout together with the properties enforced by the following
     * asserts does not need any extra alignment padding.
     */
    JS_STATIC_ASSERT(sizeof(JSErrorReport) % sizeof(const char*) == 0);
    JS_STATIC_ASSERT(sizeof(const char*) % sizeof(char16_t) == 0);

    size_t filenameSize;
    size_t linebufSize;
    size_t uclinebufSize;
    size_t ucmessageSize;
    size_t i, argsArraySize, argsCopySize, argSize;
    size_t mallocSize;
    JSErrorReport* copy;
    uint8_t* cursor;

#define JS_CHARS_SIZE(chars) ((js_strlen(chars) + 1) * sizeof(char16_t))

    filenameSize = report->filename ? strlen(report->filename) + 1 : 0;
    linebufSize = report->linebuf ? strlen(report->linebuf) + 1 : 0;
    uclinebufSize = report->uclinebuf ? JS_CHARS_SIZE(report->uclinebuf) : 0;
    ucmessageSize = 0;
    argsArraySize = 0;
    argsCopySize = 0;
    if (report->ucmessage) {
        ucmessageSize = JS_CHARS_SIZE(report->ucmessage);
        if (report->messageArgs) {
            for (i = 0; report->messageArgs[i]; ++i)
                argsCopySize += JS_CHARS_SIZE(report->messageArgs[i]);

            /* Non-null messageArgs should have at least one non-null arg. */
            MOZ_ASSERT(i != 0);
            argsArraySize = (i + 1) * sizeof(const char16_t*);
        }
    }

    /*
     * The mallocSize can not overflow since it represents the sum of the
     * sizes of already allocated objects.
     */
    mallocSize = sizeof(JSErrorReport) + argsArraySize + argsCopySize +
                 ucmessageSize + uclinebufSize + linebufSize + filenameSize;
    cursor = cx->pod_malloc<uint8_t>(mallocSize);
    if (!cursor)
        return nullptr;

    copy = (JSErrorReport*)cursor;
    memset(cursor, 0, sizeof(JSErrorReport));
    cursor += sizeof(JSErrorReport);

    if (argsArraySize != 0) {
        copy->messageArgs = (const char16_t**)cursor;
        cursor += argsArraySize;
        for (i = 0; report->messageArgs[i]; ++i) {
            copy->messageArgs[i] = (const char16_t*)cursor;
            argSize = JS_CHARS_SIZE(report->messageArgs[i]);
            js_memcpy(cursor, report->messageArgs[i], argSize);
            cursor += argSize;
        }
        copy->messageArgs[i] = nullptr;
        MOZ_ASSERT(cursor == (uint8_t*)copy->messageArgs[0] + argsCopySize);
    }

    if (report->ucmessage) {
        copy->ucmessage = (const char16_t*)cursor;
        js_memcpy(cursor, report->ucmessage, ucmessageSize);
        cursor += ucmessageSize;
    }

    if (report->uclinebuf) {
        copy->uclinebuf = (const char16_t*)cursor;
        js_memcpy(cursor, report->uclinebuf, uclinebufSize);
        cursor += uclinebufSize;
        if (report->uctokenptr) {
            copy->uctokenptr = copy->uclinebuf + (report->uctokenptr -
                                                  report->uclinebuf);
        }
    }

    if (report->linebuf) {
        copy->linebuf = (const char*)cursor;
        js_memcpy(cursor, report->linebuf, linebufSize);
        cursor += linebufSize;
        if (report->tokenptr) {
            copy->tokenptr = copy->linebuf + (report->tokenptr -
                                              report->linebuf);
        }
    }

    if (report->filename) {
        copy->filename = (const char*)cursor;
        js_memcpy(cursor, report->filename, filenameSize);
    }
    MOZ_ASSERT(cursor + filenameSize == (uint8_t*)copy + mallocSize);

    /* Copy non-pointer members. */
    copy->isMuted = report->isMuted;
    copy->lineno = report->lineno;
    copy->column = report->column;
    copy->errorNumber = report->errorNumber;
    copy->exnType = report->exnType;

    /* Note that this is before it gets flagged with JSREPORT_EXCEPTION */
    copy->flags = report->flags;

#undef JS_CHARS_SIZE
    return copy;
}

struct SuppressErrorsGuard
{
    JSContext* cx;
    JSErrorReporter prevReporter;
    JS::AutoSaveExceptionState prevState;

    explicit SuppressErrorsGuard(JSContext* cx)
      : cx(cx),
        prevReporter(JS_SetErrorReporter(cx->runtime(), nullptr)),
        prevState(cx)
    {}

    ~SuppressErrorsGuard()
    {
        JS_SetErrorReporter(cx->runtime(), prevReporter);
    }
};

// Cut off the stack if it gets too deep (most commonly for infinite recursion
// errors).
static const size_t MAX_REPORTED_STACK_DEPTH = 1u << 7;

static bool
CaptureStack(JSContext* cx, MutableHandleObject stack)
{
    return CaptureCurrentStack(cx, stack, MAX_REPORTED_STACK_DEPTH);
}

JSString*
js::ComputeStackString(JSContext* cx)
{
    SuppressErrorsGuard seg(cx);

    RootedObject stack(cx);
    if (!CaptureStack(cx, &stack))
        return nullptr;

    RootedString str(cx);
    if (!BuildStackString(cx, stack, &str))
        return nullptr;

    return str.get();
}

static void
exn_finalize(FreeOp* fop, JSObject* obj)
{
    if (JSErrorReport* report = obj->as<ErrorObject>().getErrorReport())
        fop->free_(report);
}

JSErrorReport*
js::ErrorFromException(JSContext* cx, HandleObject objArg)
{
    // It's ok to UncheckedUnwrap here, since all we do is get the
    // JSErrorReport, and consumers are careful with the information they get
    // from that anyway.  Anyone doing things that would expose anything in the
    // JSErrorReport to page script either does a security check on the
    // JSErrorReport's principal or also tries to do toString on our object and
    // will fail if they can't unwrap it.
    RootedObject obj(cx, UncheckedUnwrap(objArg));
    if (!obj->is<ErrorObject>())
        return nullptr;

    return obj->as<ErrorObject>().getOrCreateErrorReport(cx);
}

JS_PUBLIC_API(JSObject*)
ExceptionStackOrNull(JSContext* cx, HandleObject objArg)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, objArg);
    RootedObject obj(cx, CheckedUnwrap(objArg));
    if (!obj || !obj->is<ErrorObject>()) {
      return nullptr;
    }

    return obj->as<ErrorObject>().stack();
}

bool
Error(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // ES6 19.5.1.1 mandates the .prototype lookup happens before the toString
    RootedObject proto(cx);
    if (!GetPrototypeFromCallableConstructor(cx, args, &proto))
        return false;

    /* Compute the error message, if any. */
    RootedString message(cx, nullptr);
    if (args.hasDefined(0)) {
        message = ToString<CanGC>(cx, args[0]);
        if (!message)
            return false;
    }

    /* Find the scripted caller, but only ones we're allowed to know about. */
    NonBuiltinFrameIter iter(cx, cx->compartment()->principals());

    /* Set the 'fileName' property. */
    RootedString fileName(cx);
    if (args.length() > 1) {
        fileName = ToString<CanGC>(cx, args[1]);
    } else {
        fileName = cx->runtime()->emptyString;
        if (!iter.done()) {
            if (const char* cfilename = iter.scriptFilename())
                fileName = JS_NewStringCopyZ(cx, cfilename);
        }
    }
    if (!fileName)
        return false;

    /* Set the 'lineNumber' property. */
    uint32_t lineNumber, columnNumber = 0;
    if (args.length() > 2) {
        if (!ToUint32(cx, args[2], &lineNumber))
            return false;
    } else {
        lineNumber = iter.done() ? 0 : iter.computeLine(&columnNumber);
        // XXX: Make the column 1-based as in other browsers, instead of 0-based
        // which is how SpiderMonkey stores it internally. This will be
        // unnecessary once bug 1144340 is fixed.
        ++columnNumber;
    }

    RootedObject stack(cx);
    if (!CaptureStack(cx, &stack))
        return false;

    /*
     * ECMA ed. 3, 15.11.1 requires Error, etc., to construct even when
     * called as functions, without operator new.  But as we do not give
     * each constructor a distinct JSClass, we must get the exception type
     * ourselves.
     */
    JSExnType exnType = JSExnType(args.callee().as<JSFunction>().getExtendedSlot(0).toInt32());

    RootedObject obj(cx, ErrorObject::create(cx, exnType, stack, fileName,
                                             lineNumber, columnNumber, nullptr, message, proto));
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

#if JS_HAS_TOSOURCE
/*
 * Return a string that may eval to something similar to the original object.
 */
static bool
exn_toSource(JSContext* cx, unsigned argc, Value* vp)
{
    JS_CHECK_RECURSION(cx, return false);
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    RootedValue nameVal(cx);
    RootedString name(cx);
    if (!GetProperty(cx, obj, obj, cx->names().name, &nameVal) ||
        !(name = ToString<CanGC>(cx, nameVal)))
    {
        return false;
    }

    RootedValue messageVal(cx);
    RootedString message(cx);
    if (!GetProperty(cx, obj, obj, cx->names().message, &messageVal) ||
        !(message = ValueToSource(cx, messageVal)))
    {
        return false;
    }

    RootedValue filenameVal(cx);
    RootedString filename(cx);
    if (!GetProperty(cx, obj, obj, cx->names().fileName, &filenameVal) ||
        !(filename = ValueToSource(cx, filenameVal)))
    {
        return false;
    }

    RootedValue linenoVal(cx);
    uint32_t lineno;
    if (!GetProperty(cx, obj, obj, cx->names().lineNumber, &linenoVal) ||
        !ToUint32(cx, linenoVal, &lineno))
    {
        return false;
    }

    StringBuffer sb(cx);
    if (!sb.append("(new ") || !sb.append(name) || !sb.append("("))
        return false;

    if (!sb.append(message))
        return false;

    if (!filename->empty()) {
        if (!sb.append(", ") || !sb.append(filename))
            return false;
    }
    if (lineno != 0) {
        /* We have a line, but no filename, add empty string */
        if (filename->empty() && !sb.append(", \"\""))
                return false;

        JSString* linenumber = ToString<CanGC>(cx, linenoVal);
        if (!linenumber)
            return false;
        if (!sb.append(", ") || !sb.append(linenumber))
            return false;
    }

    if (!sb.append("))"))
        return false;

    JSString* str = sb.finishString();
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}
#endif

/* static */ JSObject*
ErrorObject::createProto(JSContext* cx, JSProtoKey key)
{
    RootedObject errorProto(cx, GenericCreatePrototype(cx, key));
    if (!errorProto)
        return nullptr;

    Rooted<ErrorObject*> err(cx, &errorProto->as<ErrorObject>());
    RootedString emptyStr(cx, cx->names().empty);
    JSExnType type = ExnTypeFromProtoKey(key);
    if (!ErrorObject::init(cx, err, type, nullptr, emptyStr, nullptr, 0, 0, emptyStr))
        return nullptr;

    // The various prototypes also have .name in addition to the normal error
    // instance properties.
    RootedPropertyName name(cx, ClassName(key, cx));
    RootedValue nameValue(cx, StringValue(name));
    if (!DefineProperty(cx, err, cx->names().name, nameValue, nullptr, nullptr, 0))
        return nullptr;

    return errorProto;
}

/* static */ JSObject*
ErrorObject::createConstructor(JSContext* cx, JSProtoKey key)
{
    RootedObject ctor(cx);
    ctor = GenericCreateConstructor<Error, 1, gc::AllocKind::FUNCTION_EXTENDED>(cx, key);
    if (!ctor)
        return nullptr;

    ctor->as<JSFunction>().setExtendedSlot(0, Int32Value(ExnTypeFromProtoKey(key)));
    return ctor;
}

JS_FRIEND_API(JSFlatString*)
js::GetErrorTypeName(JSRuntime* rt, int16_t exnType)
{
    /*
     * JSEXN_INTERNALERR returns null to prevent that "InternalError: "
     * is prepended before "uncaught exception: "
     */
    if (exnType <= JSEXN_NONE || exnType >= JSEXN_LIMIT ||
        exnType == JSEXN_INTERNALERR)
    {
        return nullptr;
    }
    JSProtoKey key = GetExceptionProtoKey(JSExnType(exnType));
    return ClassName(key, rt);
}

bool
js::ErrorToException(JSContext* cx, const char* message, JSErrorReport* reportp,
                     JSErrorCallback callback, void* userRef)
{
    // Tell our caller to report immediately if this report is just a warning.
    MOZ_ASSERT(reportp);
    if (JSREPORT_IS_WARNING(reportp->flags))
        return false;

    // Similarly, we cannot throw a proper object inside the self-hosting
    // compartment, as we cannot construct the Error constructor without
    // self-hosted code. Tell our caller to report immediately.
    // Without self-hosted code, we cannot get started anyway.
    if (cx->runtime()->isSelfHostingCompartment(cx->compartment()))
        return false;

    // Find the exception index associated with this error.
    JSErrNum errorNumber = static_cast<JSErrNum>(reportp->errorNumber);
    if (!callback)
        callback = GetErrorMessage;
    const JSErrorFormatString* errorString = callback(userRef, errorNumber);
    JSExnType exnType = errorString ? static_cast<JSExnType>(errorString->exnType) : JSEXN_NONE;
    MOZ_ASSERT(exnType < JSEXN_LIMIT);

    // Return false (no exception raised) if no exception is associated
    // with the given error number.
    if (exnType == JSEXN_NONE)
        return false;

    // Prevent infinite recursion.
    if (cx->generatingError)
        return false;
    AutoScopedAssign<bool> asa(&cx->generatingError, true);

    // Create an exception object.
    RootedString messageStr(cx, reportp->ucmessage ? JS_NewUCStringCopyZ(cx, reportp->ucmessage)
                                                   : JS_NewStringCopyZ(cx, message));
    if (!messageStr)
        return cx->isExceptionPending();

    RootedString fileName(cx, JS_NewStringCopyZ(cx, reportp->filename));
    if (!fileName)
        return cx->isExceptionPending();

    uint32_t lineNumber = reportp->lineno;
    uint32_t columnNumber = reportp->column;

    RootedObject stack(cx);
    if (!CaptureStack(cx, &stack))
        return cx->isExceptionPending();

    js::ScopedJSFreePtr<JSErrorReport> report(CopyErrorReport(cx, reportp));
    if (!report)
        return cx->isExceptionPending();

    RootedObject errObject(cx, ErrorObject::create(cx, exnType, stack, fileName,
                                                   lineNumber, columnNumber, &report, messageStr));
    if (!errObject)
        return cx->isExceptionPending();

    // Throw it.
    RootedValue errValue(cx, ObjectValue(*errObject));
    JS_SetPendingException(cx, errValue);

    // Flag the error report passed in to indicate an exception was raised.
    reportp->flags |= JSREPORT_EXCEPTION;
    return true;
}

static bool
IsDuckTypedErrorObject(JSContext* cx, HandleObject exnObject, const char** filename_strp)
{
    /*
     * This function is called from ErrorReport::init and so should not generate
     * any new exceptions.
     */
    AutoClearPendingException acpe(cx);

    bool found;
    if (!JS_HasProperty(cx, exnObject, js_message_str, &found) || !found)
        return false;

    const char* filename_str = *filename_strp;
    if (!JS_HasProperty(cx, exnObject, filename_str, &found) || !found) {
        /* Now try "fileName", in case this quacks like an Error */
        filename_str = js_fileName_str;
        if (!JS_HasProperty(cx, exnObject, filename_str, &found) || !found)
            return false;
    }

    if (!JS_HasProperty(cx, exnObject, js_lineNumber_str, &found) || !found)
        return false;

    *filename_strp = filename_str;
    return true;
}

JS_FRIEND_API(JSString*)
js::ErrorReportToString(JSContext* cx, JSErrorReport* reportp)
{
    JSExnType type = static_cast<JSExnType>(reportp->exnType);
    RootedString str(cx, cx->runtime()->emptyString);
    if (type != JSEXN_NONE)
        str = ClassName(GetExceptionProtoKey(type), cx);
    RootedString toAppend(cx, JS_NewUCStringCopyN(cx, MOZ_UTF16(": "), 2));
    if (!str || !toAppend)
        return nullptr;
    str = ConcatStrings<CanGC>(cx, str, toAppend);
    if (!str)
        return nullptr;
    toAppend = JS_NewUCStringCopyZ(cx, reportp->ucmessage);
    if (toAppend)
        str = ConcatStrings<CanGC>(cx, str, toAppend);
    return str;
}

bool
js::ReportUncaughtException(JSContext* cx)
{
    if (!cx->isExceptionPending())
        return true;

    RootedValue exn(cx);
    if (!cx->getPendingException(&exn)) {
        cx->clearPendingException();
        return false;
    }

    cx->clearPendingException();

    ErrorReport err(cx);
    if (!err.init(cx, exn)) {
        cx->clearPendingException();
        return false;
    }

    cx->setPendingException(exn);
    CallErrorReporter(cx, err.message(), err.report());
    cx->clearPendingException();
    return true;
}

ErrorReport::ErrorReport(JSContext* cx)
  : reportp(nullptr),
    message_(nullptr),
    ownedMessage(nullptr),
    str(cx),
    strChars(cx),
    exnObject(cx)
{
}

ErrorReport::~ErrorReport()
{
    if (!ownedMessage)
        return;

    js_free(ownedMessage);
    if (ownedReport.messageArgs) {
        /*
         * ExpandErrorArgumentsVA owns its messageArgs only if it had to
         * inflate the arguments (from regular |char*|s), which is always in
         * our case.
         */
        size_t i = 0;
        while (ownedReport.messageArgs[i])
            js_free(const_cast<char16_t*>(ownedReport.messageArgs[i++]));
        js_free(ownedReport.messageArgs);
    }
    js_free(const_cast<char16_t*>(ownedReport.ucmessage));
}

void
ErrorReport::ReportAddonExceptionToTelementry(JSContext* cx)
{
    MOZ_ASSERT(exnObject);
    RootedObject unwrapped(cx, UncheckedUnwrap(exnObject));
    MOZ_ASSERT(unwrapped, "UncheckedUnwrap failed?");

    // There is not much we can report if the exception is not an ErrorObject, let's ignore those.
    if (!unwrapped->is<ErrorObject>())
        return;

    Rooted<ErrorObject*> errObj(cx, &unwrapped->as<ErrorObject>());
    RootedObject stack(cx, errObj->stack());

    // Let's ignore TOP level exceptions. For regular add-ons those will not be reported anyway,
    // for SDK based once it should not be a valid case either.
    // At this point the frame stack is unwound but the exception object stored the stack so let's
    // use that for getting the function name.
    if (!stack)
        return;

    JSCompartment* comp = stack->compartment();
    JSAddonId* addonId = comp->addonId;

    // We only want to send the report if the scope that just have thrown belongs to an add-on.
    // Let's check the compartment of the youngest function on the stack, to determine that.
    if (!addonId)
        return;

    RootedString funnameString(cx);
    JS::SavedFrameResult result = GetSavedFrameFunctionDisplayName(cx, stack, &funnameString);
    // AccessDenied should never be the case here for add-ons but let's not risk it.
    JSAutoByteString bytes;
    const char* funname = nullptr;
    bool denied = result == JS::SavedFrameResult::AccessDenied;
    funname = denied ? "unknown"
                     : funnameString ? AtomToPrintableString(cx,
                                                             &funnameString->asAtom(),
                                                             &bytes)
                                     : "anonymous";

    UniqueChars addonIdChars(JS_EncodeString(cx, addonId));

    const char* filename = nullptr;
    if (reportp && reportp->filename) {
        filename = strrchr(reportp->filename, '/');
        if (filename)
            filename++;
    }
    if (!filename) {
        filename = "FILE_NOT_FOUND";
    }
    char histogramKey[64];
    JS_snprintf(histogramKey, sizeof(histogramKey),
                "%s %s %s %u",
                addonIdChars.get(),
                funname,
                filename,
                (reportp ? reportp->lineno : 0) );
    cx->runtime()->addTelemetry(JS_TELEMETRY_ADDON_EXCEPTIONS, 1, histogramKey);
}

bool
ErrorReport::init(JSContext* cx, HandleValue exn)
{
    MOZ_ASSERT(!cx->isExceptionPending());

    if (exn.isObject()) {
        // Because ToString below could error and an exception object could become
        // unrooted, we must root our exception object, if any.
        exnObject = &exn.toObject();
        reportp = ErrorFromException(cx, exnObject);

        // Let's see if the exception is from add-on code, if so, it should be reported
        // to telementry.
        ReportAddonExceptionToTelementry(cx);
    }


    // Be careful not to invoke ToString if we've already successfully extracted
    // an error report, since the exception might be wrapped in a security
    // wrapper, and ToString-ing it might throw.
    if (reportp)
        str = ErrorReportToString(cx, reportp);
    else
        str = ToString<CanGC>(cx, exn);

    if (!str)
        cx->clearPendingException();

    // If ErrorFromException didn't get us a JSErrorReport, then the object
    // was not an ErrorObject, security-wrapped or otherwise. However, it might
    // still quack like one. Give duck-typing a chance.  We start by looking for
    // "filename" (all lowercase), since that's where DOMExceptions store their
    // filename.  Then we check "fileName", which is where Errors store it.  We
    // have to do it in that order, because DOMExceptions have Error.prototype
    // on their proto chain, and hence also have a "fileName" property, but its
    // value is "".
    const char* filename_str = "filename";
    if (!reportp && exnObject && IsDuckTypedErrorObject(cx, exnObject, &filename_str))
    {
        // Temporary value for pulling properties off of duck-typed objects.
        RootedValue val(cx);

        RootedString name(cx);
        if (JS_GetProperty(cx, exnObject, js_name_str, &val) && val.isString())
            name = val.toString();
        else
            cx->clearPendingException();

        RootedString msg(cx);
        if (JS_GetProperty(cx, exnObject, js_message_str, &val) && val.isString())
            msg = val.toString();
        else
            cx->clearPendingException();

        // If we have the right fields, override the ToString we performed on
        // the exception object above with something built out of its quacks
        // (i.e. as much of |NameQuack: MessageQuack| as we can make).
        //
        // It would be nice to use ErrorReportToString here, but we can't quite
        // do it - mostly because we'd need to figure out what JSExnType |name|
        // corresponds to, which may not be any JSExnType at all.
        if (name && msg) {
            RootedString colon(cx, JS_NewStringCopyZ(cx, ": "));
            if (!colon)
                return false;
            RootedString nameColon(cx, ConcatStrings<CanGC>(cx, name, colon));
            if (!nameColon)
                return false;
            str = ConcatStrings<CanGC>(cx, nameColon, msg);
            if (!str)
                return false;
        } else if (name) {
            str = name;
        } else if (msg) {
            str = msg;
        }

        if (JS_GetProperty(cx, exnObject, filename_str, &val)) {
            RootedString tmp(cx, ToString<CanGC>(cx, val));
            if (tmp)
                filename.encodeUtf8(cx, tmp);
            else
                cx->clearPendingException();
        } else {
            cx->clearPendingException();
        }

        uint32_t lineno;
        if (!JS_GetProperty(cx, exnObject, js_lineNumber_str, &val) ||
            !ToUint32(cx, val, &lineno))
        {
            cx->clearPendingException();
            lineno = 0;
        }

        uint32_t column;
        if (!JS_GetProperty(cx, exnObject, js_columnNumber_str, &val) ||
            !ToUint32(cx, val, &column))
        {
            cx->clearPendingException();
            column = 0;
        }

        reportp = &ownedReport;
        new (reportp) JSErrorReport();
        ownedReport.filename = filename.ptr();
        ownedReport.lineno = lineno;
        ownedReport.exnType = int16_t(JSEXN_NONE);
        ownedReport.column = column;
        if (str) {
            // Note that using |str| for |ucmessage| here is kind of wrong,
            // because |str| is supposed to be of the format
            // |ErrorName: ErrorMessage|, and |ucmessage| is supposed to
            // correspond to |ErrorMessage|. But this is what we've historically
            // done for duck-typed error objects.
            //
            // If only this stuff could get specced one day...
            if (str->ensureFlat(cx) && strChars.initTwoByte(cx, str))
                ownedReport.ucmessage = strChars.twoByteChars();
        }
    }

    if (str)
        message_ = bytesStorage.encodeUtf8(cx, str);
    if (!message_)
        message_ = "unknown (can't convert to string)";

    if (!reportp) {
        // This is basically an inlined version of
        //
        //   JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
        //                        JSMSG_UNCAUGHT_EXCEPTION, message_);
        //
        // but without the reporting bits.  Instead it just puts all
        // the stuff we care about in our ownedReport and message_.
        if (!populateUncaughtExceptionReport(cx, message_)) {
            // Just give up.  We're out of memory or something; not much we can
            // do here.
            return false;
        }
    } else {
        /* Flag the error as an exception. */
        reportp->flags |= JSREPORT_EXCEPTION;
    }

    return true;
}

bool
ErrorReport::populateUncaughtExceptionReport(JSContext* cx, ...)
{
    va_list ap;
    va_start(ap, cx);
    bool ok = populateUncaughtExceptionReportVA(cx, ap);
    va_end(ap);
    return ok;
}

bool
ErrorReport::populateUncaughtExceptionReportVA(JSContext* cx, va_list ap)
{
    new (&ownedReport) JSErrorReport();
    ownedReport.flags = JSREPORT_ERROR;
    ownedReport.errorNumber = JSMSG_UNCAUGHT_EXCEPTION;
    // XXXbz this assumes the stack we have right now is still
    // related to our exception object.  It would be better if we
    // could accept a passed-in stack of some sort instead.
    NonBuiltinFrameIter iter(cx, cx->compartment()->principals());
    if (!iter.done()) {
        ownedReport.filename = iter.scriptFilename();
        ownedReport.lineno = iter.computeLine(&ownedReport.column);
        // XXX: Make the column 1-based as in other browsers, instead of 0-based
        // which is how SpiderMonkey stores it internally. This will be
        // unnecessary once bug 1144340 is fixed.
        ++ownedReport.column;
        ownedReport.isMuted = iter.mutedErrors();
    }

    if (!ExpandErrorArgumentsVA(cx, GetErrorMessage, nullptr,
                                JSMSG_UNCAUGHT_EXCEPTION, &ownedMessage,
                                &ownedReport, ArgumentsAreASCII, ap)) {
        return false;
    }

    reportp = &ownedReport;
    message_ = ownedMessage;
    ownsMessageAndReport = true;
    return true;
}

JSObject*
js::CopyErrorObject(JSContext* cx, Handle<ErrorObject*> err)
{
    js::ScopedJSFreePtr<JSErrorReport> copyReport;
    if (JSErrorReport* errorReport = err->getErrorReport()) {
        copyReport = CopyErrorReport(cx, errorReport);
        if (!copyReport)
            return nullptr;
    }

    RootedString message(cx, err->getMessage());
    if (message && !cx->compartment()->wrap(cx, &message))
        return nullptr;
    RootedString fileName(cx, err->fileName(cx));
    if (!cx->compartment()->wrap(cx, &fileName))
        return nullptr;
    RootedObject stack(cx, err->stack());
    if (!cx->compartment()->wrap(cx, &stack))
        return nullptr;
    uint32_t lineNumber = err->lineNumber();
    uint32_t columnNumber = err->columnNumber();
    JSExnType errorType = err->type();

    // Create the Error object.
    return ErrorObject::create(cx, errorType, stack, fileName,
                               lineNumber, columnNumber, &copyReport, message);
}

JS_PUBLIC_API(bool)
JS::CreateError(JSContext* cx, JSExnType type, HandleObject stack, HandleString fileName,
                    uint32_t lineNumber, uint32_t columnNumber, JSErrorReport* report,
                    HandleString message, MutableHandleValue rval)
{
    assertSameCompartment(cx, stack, fileName, message);
    AssertObjectIsSavedFrameOrWrapper(cx, stack);

    js::ScopedJSFreePtr<JSErrorReport> rep;
    if (report)
        rep = CopyErrorReport(cx, report);

    RootedObject obj(cx,
        js::ErrorObject::create(cx, type, stack, fileName,
                                lineNumber, columnNumber, &rep, message));
    if (!obj)
        return false;

    rval.setObject(*obj);
    return true;
}

const char*
js::ValueToSourceForError(JSContext* cx, HandleValue val, JSAutoByteString& bytes)
{
    if (val.isUndefined()) {
        return "undefined";
    }
    if (val.isNull()) {
        return "null";
    }

    RootedString str(cx, JS_ValueToSource(cx, val));
    if (!str) {
        JS_ClearPendingException(cx);
        return "<<error converting value to string>>";
    }

    StringBuffer sb(cx);
    if (val.isObject()) {
        RootedObject valObj(cx, val.toObjectOrNull());
        ESClassValue cls;
        if (!GetBuiltinClass(cx, valObj, &cls)) {
            JS_ClearPendingException(cx);
            return "<<error determining class of value>>";
        }
        if (cls == ESClass_Array) {
            sb.append("the array ");
        } else if (cls == ESClass_ArrayBuffer) {
            sb.append("the array buffer ");
        } else if (JS_IsArrayBufferViewObject(valObj)) {
            sb.append("the typed array ");
        } else {
            sb.append("the object ");
        }
    } else if (val.isNumber()) {
        sb.append("the number ");
    } else if (val.isString()) {
        sb.append("the string ");
    } else {
        MOZ_ASSERT(val.isBoolean() || val.isSymbol());
        return bytes.encodeLatin1(cx, str);
    }
    sb.append(str);
    return bytes.encodeLatin1(cx, sb.finishString());
}
