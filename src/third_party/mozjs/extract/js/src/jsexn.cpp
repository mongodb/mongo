/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS standard exception implementation.
 */

#include "jsexn.h"

#include "mozilla/Assertions.h"
#include "mozilla/Maybe.h"
#include "mozilla/ScopeExit.h"

#include <new>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <utility>

#include "jsapi.h"
#include "jsfriendapi.h"
#include "jstypes.h"

#include "frontend/FrontendContext.h"  // AutoReportFrontendContext
#include "js/CharacterEncoding.h"      // JS::UTF8Chars, JS::ConstUTF8CharsZ
#include "js/Class.h"
#include "js/ColumnNumber.h"  // JS::ColumnNumberOneOrigin, JS::TaggedColumnNumberOneOrigin
#include "js/Conversions.h"
#include "js/ErrorReport.h"             // JS::PrintError
#include "js/Exception.h"               // JS::ExceptionStack
#include "js/experimental/TypedData.h"  // JS_IsArrayBufferViewObject
#include "js/friend/ErrorMessages.h"  // JSErrNum, js::GetErrorMessage, JSMSG_*
#include "js/Object.h"                // JS::GetBuiltinClass
#include "js/PropertyAndElement.h"    // JS_GetProperty, JS_HasProperty
#include "js/SavedFrameAPI.h"
#include "js/Stack.h"
#include "js/UniquePtr.h"
#include "js/Value.h"
#include "js/Warnings.h"  // JS::{,Set}WarningReporter
#include "js/Wrapper.h"
#include "util/Memory.h"
#include "util/StringBuffer.h"
#include "vm/Compartment.h"
#include "vm/ErrorObject.h"
#include "vm/FrameIter.h"    // js::NonBuiltinFrameIter
#include "vm/JSAtomUtils.h"  // ClassName
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/Realm.h"
#include "vm/SavedFrame.h"
#include "vm/SavedStacks.h"
#include "vm/SelfHosting.h"
#include "vm/Stack.h"
#include "vm/StringType.h"
#include "vm/SymbolType.h"
#include "wasm/WasmJS.h"  // WasmExceptionObject

#include "vm/Compartment-inl.h"
#include "vm/ErrorObject-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/ObjectOperations-inl.h"  // js::GetProperty
#include "vm/SavedStacks-inl.h"

using namespace js;

using JS::SavedFrameSelfHosted;

size_t ExtraMallocSize(JSErrorReport* report) {
  if (report->linebuf()) {
    /*
     * Count with null terminator and alignment.
     * See CopyExtraData for the details about alignment.
     */
    return (report->linebufLength() + 1) * sizeof(char16_t) + 1;
  }

  return 0;
}

size_t ExtraMallocSize(JSErrorNotes::Note* note) { return 0; }

bool CopyExtraData(JSContext* cx, uint8_t** cursor, JSErrorReport* copy,
                   JSErrorReport* report) {
  if (report->linebuf()) {
    /*
     * Make sure cursor is properly aligned for char16_t for platforms
     * which need it and it's at the end of the buffer on exit.
     */
    size_t alignment_backlog = 0;
    if (size_t(*cursor) % 2) {
      (*cursor)++;
    } else {
      alignment_backlog = 1;
    }

    size_t linebufSize = (report->linebufLength() + 1) * sizeof(char16_t);
    const char16_t* linebufCopy = (const char16_t*)(*cursor);
    js_memcpy(*cursor, report->linebuf(), linebufSize);
    *cursor += linebufSize + alignment_backlog;
    copy->initBorrowedLinebuf(linebufCopy, report->linebufLength(),
                              report->tokenOffset());
  }

  /* Copy non-pointer members. */
  copy->isMuted = report->isMuted;
  copy->exnType = report->exnType;
  copy->isWarning_ = report->isWarning_;

  /* Deep copy notes. */
  if (report->notes) {
    auto copiedNotes = report->notes->copy(cx);
    if (!copiedNotes) {
      return false;
    }
    copy->notes = std::move(copiedNotes);
  } else {
    copy->notes.reset(nullptr);
  }

  return true;
}

bool CopyExtraData(JSContext* cx, uint8_t** cursor, JSErrorNotes::Note* copy,
                   JSErrorNotes::Note* report) {
  return true;
}

template <typename T>
static UniquePtr<T> CopyErrorHelper(JSContext* cx, T* report) {
  /*
   * We use a single malloc block to make a deep copy of JSErrorReport or
   * JSErrorNotes::Note, except JSErrorNotes linked from JSErrorReport with
   * the following layout:
   *   JSErrorReport or JSErrorNotes::Note
   *   char array with characters for message_
   *   char array with characters for filename
   *   char16_t array with characters for linebuf (only for JSErrorReport)
   * Such layout together with the properties enforced by the following
   * asserts does not need any extra alignment padding.
   */
  static_assert(sizeof(T) % sizeof(const char*) == 0);
  static_assert(sizeof(const char*) % sizeof(char16_t) == 0);

  size_t filenameSize =
      report->filename ? strlen(report->filename.c_str()) + 1 : 0;
  size_t messageSize = 0;
  if (report->message()) {
    messageSize = strlen(report->message().c_str()) + 1;
  }

  /*
   * The mallocSize can not overflow since it represents the sum of the
   * sizes of already allocated objects.
   */
  size_t mallocSize =
      sizeof(T) + messageSize + filenameSize + ExtraMallocSize(report);
  uint8_t* cursor = cx->pod_calloc<uint8_t>(mallocSize);
  if (!cursor) {
    return nullptr;
  }

  UniquePtr<T> copy(new (cursor) T());
  cursor += sizeof(T);

  if (report->message()) {
    copy->initBorrowedMessage((const char*)cursor);
    js_memcpy(cursor, report->message().c_str(), messageSize);
    cursor += messageSize;
  }

  if (report->filename) {
    copy->filename = JS::ConstUTF8CharsZ((const char*)cursor);
    js_memcpy(cursor, report->filename.c_str(), filenameSize);
    cursor += filenameSize;
  }

  if (!CopyExtraData(cx, &cursor, copy.get(), report)) {
    return nullptr;
  }

  MOZ_ASSERT(cursor == (uint8_t*)copy.get() + mallocSize);

  // errorMessageName should be static.
  copy->errorMessageName = report->errorMessageName;

  /* Copy non-pointer members. */
  copy->sourceId = report->sourceId;
  copy->lineno = report->lineno;
  copy->column = report->column;
  copy->errorNumber = report->errorNumber;

  return copy;
}

UniquePtr<JSErrorNotes::Note> js::CopyErrorNote(JSContext* cx,
                                                JSErrorNotes::Note* note) {
  return CopyErrorHelper(cx, note);
}

UniquePtr<JSErrorReport> js::CopyErrorReport(JSContext* cx,
                                             JSErrorReport* report) {
  return CopyErrorHelper(cx, report);
}

struct SuppressErrorsGuard {
  JSContext* cx;
  JS::WarningReporter prevReporter;
  JS::AutoSaveExceptionState prevState;

  explicit SuppressErrorsGuard(JSContext* cx)
      : cx(cx),
        prevReporter(JS::SetWarningReporter(cx, nullptr)),
        prevState(cx) {}

  ~SuppressErrorsGuard() { JS::SetWarningReporter(cx, prevReporter); }
};

// Cut off the stack if it gets too deep (most commonly for infinite recursion
// errors).
static const size_t MAX_REPORTED_STACK_DEPTH = 1u << 7;

bool js::CaptureStack(JSContext* cx, MutableHandleObject stack) {
  return CaptureCurrentStack(
      cx, stack, JS::StackCapture(JS::MaxFrames(MAX_REPORTED_STACK_DEPTH)));
}

JSString* js::ComputeStackString(JSContext* cx) {
  SuppressErrorsGuard seg(cx);

  RootedObject stack(cx);
  if (!CaptureStack(cx, &stack)) {
    return nullptr;
  }

  RootedString str(cx);
  if (!BuildStackString(cx, cx->realm()->principals(), stack, &str)) {
    return nullptr;
  }

  return str.get();
}

JSErrorReport* js::ErrorFromException(JSContext* cx, HandleObject objArg) {
  // It's ok to UncheckedUnwrap here, since all we do is get the
  // JSErrorReport, and consumers are careful with the information they get
  // from that anyway.  Anyone doing things that would expose anything in the
  // JSErrorReport to page script either does a security check on the
  // JSErrorReport's principal or also tries to do toString on our object and
  // will fail if they can't unwrap it.
  RootedObject obj(cx, UncheckedUnwrap(objArg));
  if (!obj->is<ErrorObject>()) {
    return nullptr;
  }

  JSErrorReport* report = obj->as<ErrorObject>().getOrCreateErrorReport(cx);
  if (!report) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    cx->recoverFromOutOfMemory();
  }

  return report;
}

JS_PUBLIC_API JSObject* JS::ExceptionStackOrNull(HandleObject objArg) {
  ErrorObject* errorObject = objArg->maybeUnwrapIf<ErrorObject>();
  if (errorObject) {
    return errorObject->stack();
  }

  WasmExceptionObject* wasmObject =
      objArg->maybeUnwrapIf<WasmExceptionObject>();
  if (wasmObject) {
    return wasmObject->stack();
  }

  return nullptr;
}

JS_PUBLIC_API JSLinearString* js::GetErrorTypeName(JSContext* cx,
                                                   int16_t exnType) {
  /*
   * JSEXN_INTERNALERR returns null to prevent that "InternalError: "
   * is prepended before "uncaught exception: "
   */
  if (exnType < 0 || exnType >= JSEXN_LIMIT || exnType == JSEXN_INTERNALERR ||
      exnType == JSEXN_WARN || exnType == JSEXN_NOTE) {
    return nullptr;
  }
  JSProtoKey key = GetExceptionProtoKey(JSExnType(exnType));
  return ClassName(key, cx);
}

bool js::ErrorToException(JSContext* cx, JSErrorReport* reportp,
                          JSErrorCallback callback, void* userRef) {
  MOZ_ASSERT(!reportp->isWarning());

  // Find the exception index associated with this error.
  JSErrNum errorNumber = static_cast<JSErrNum>(reportp->errorNumber);
  if (!callback) {
    callback = GetErrorMessage;
  }
  const JSErrorFormatString* errorString = callback(userRef, errorNumber);
  JSExnType exnType =
      errorString ? static_cast<JSExnType>(errorString->exnType) : JSEXN_ERR;
  MOZ_ASSERT(exnType < JSEXN_ERROR_LIMIT);

  // Prevent infinite recursion.
  if (cx->generatingError) {
    return false;
  }

  cx->generatingError = true;
  auto restore = mozilla::MakeScopeExit([cx] { cx->generatingError = false; });

  // Create an exception object.
  RootedString messageStr(cx, reportp->newMessageString(cx));
  if (!messageStr) {
    return false;
  }

  Rooted<JSString*> fileName(cx);
  if (const char* filename = reportp->filename.c_str()) {
    fileName =
        JS_NewStringCopyUTF8N(cx, JS::UTF8Chars(filename, strlen(filename)));
    if (!fileName) {
      return false;
    }
  } else {
    fileName = cx->emptyString();
  }

  uint32_t sourceId = reportp->sourceId;
  uint32_t lineNumber = reportp->lineno;
  JS::ColumnNumberOneOrigin columnNumber = reportp->column;

  // Error reports don't provide a |cause|, so we default to |Nothing| here.
  auto cause = JS::NothingHandleValue;

  RootedObject stack(cx);
  if (!CaptureStack(cx, &stack)) {
    return false;
  }

  UniquePtr<JSErrorReport> report = CopyErrorReport(cx, reportp);
  if (!report) {
    return false;
  }

  ErrorObject* errObject =
      ErrorObject::create(cx, exnType, stack, fileName, sourceId, lineNumber,
                          columnNumber, std::move(report), messageStr, cause);
  if (!errObject) {
    return false;
  }

  // Throw it.
  RootedValue errValue(cx, ObjectValue(*errObject));
  Rooted<SavedFrame*> nstack(cx);
  if (stack) {
    nstack = &stack->as<SavedFrame>();
  }
  cx->setPendingException(errValue, nstack);
  return true;
}

using SniffingBehavior = JS::ErrorReportBuilder::SniffingBehavior;

static bool IsDuckTypedErrorObject(JSContext* cx, HandleObject exnObject,
                                   const char** filename_strp) {
  /*
   * This function is called from ErrorReport::init and so should not generate
   * any new exceptions.
   */
  AutoClearPendingException acpe(cx);

  bool found;
  if (!JS_HasProperty(cx, exnObject, "message", &found) || !found) {
    return false;
  }

  // First try "filename".
  const char* filename_str = *filename_strp;
  if (!JS_HasProperty(cx, exnObject, filename_str, &found)) {
    return false;
  }
  if (!found) {
    // If that doesn't work, try "fileName".
    filename_str = "fileName";
    if (!JS_HasProperty(cx, exnObject, filename_str, &found) || !found) {
      return false;
    }
  }

  if (!JS_HasProperty(cx, exnObject, "lineNumber", &found) || !found) {
    return false;
  }

  *filename_strp = filename_str;
  return true;
}

static bool GetPropertyNoException(JSContext* cx, HandleObject obj,
                                   SniffingBehavior behavior,
                                   Handle<PropertyName*> name,
                                   MutableHandleValue vp) {
  // This function has no side-effects so always use it.
  if (GetPropertyPure(cx, obj, NameToId(name), vp.address())) {
    return true;
  }

  if (behavior == SniffingBehavior::WithSideEffects) {
    AutoClearPendingException acpe(cx);
    return GetProperty(cx, obj, obj, name, vp);
  }

  return false;
}

// Create a new error message similar to what Error.prototype.toString would
// produce when called on an object with those property values for name and
// message.
static JSString* FormatErrorMessage(JSContext* cx, HandleString name,
                                    HandleString message) {
  if (name && message) {
    AutoClearPendingException acpe(cx);
    JSStringBuilder sb(cx);

    // Prefix the message with the error type, if it exists.
    if (!sb.append(name) || !sb.append(": ") || !sb.append(message)) {
      return nullptr;
    }

    return sb.finishString();
  }

  return name ? name : message;
}

static JSString* ErrorReportToString(JSContext* cx, HandleObject exn,
                                     JSErrorReport* reportp,
                                     SniffingBehavior behavior) {
  // The error object might have custom `name` overwriting the exnType in the
  // error report. Try getting that property and use the exnType as a fallback.
  RootedString name(cx);
  RootedValue nameV(cx);
  if (GetPropertyNoException(cx, exn, behavior, cx->names().name, &nameV) &&
      nameV.isString()) {
    name = nameV.toString();
  }

  // We do NOT want to use GetErrorTypeName() here because it will not do the
  // "right thing" for JSEXN_INTERNALERR.  That is, the caller of this API
  // expects that "InternalError: " will be prepended but GetErrorTypeName
  // goes out of its way to avoid this.
  if (!name) {
    JSExnType type = static_cast<JSExnType>(reportp->exnType);
    if (type != JSEXN_WARN && type != JSEXN_NOTE) {
      name = ClassName(GetExceptionProtoKey(type), cx);
    }
  }

  RootedString message(cx);
  RootedValue messageV(cx);
  if (GetPropertyNoException(cx, exn, behavior, cx->names().message,
                             &messageV) &&
      messageV.isString()) {
    message = messageV.toString();
  }

  if (!message) {
    message = reportp->newMessageString(cx);
    if (!message) {
      return nullptr;
    }
  }

  return FormatErrorMessage(cx, name, message);
}

JS::ErrorReportBuilder::ErrorReportBuilder(JSContext* cx)
    : reportp(nullptr), exnObject(cx) {}

JS::ErrorReportBuilder::~ErrorReportBuilder() = default;

bool JS::ErrorReportBuilder::init(JSContext* cx,
                                  const JS::ExceptionStack& exnStack,
                                  SniffingBehavior sniffingBehavior) {
  MOZ_ASSERT(!cx->isExceptionPending());
  MOZ_ASSERT(!reportp);

  if (exnStack.exception().isObject()) {
    // Because ToString below could error and an exception object could become
    // unrooted, we must root our exception object, if any.
    exnObject = &exnStack.exception().toObject();
    reportp = ErrorFromException(cx, exnObject);
  }

  // Be careful not to invoke ToString if we've already successfully extracted
  // an error report, since the exception might be wrapped in a security
  // wrapper, and ToString-ing it might throw.
  RootedString str(cx);
  if (reportp) {
    str = ErrorReportToString(cx, exnObject, reportp, sniffingBehavior);
  } else if (exnStack.exception().isSymbol()) {
    RootedValue strVal(cx);
    if (js::SymbolDescriptiveString(cx, exnStack.exception().toSymbol(),
                                    &strVal)) {
      str = strVal.toString();
    } else {
      str = nullptr;
    }
  } else if (exnObject && sniffingBehavior == NoSideEffects) {
    str = cx->names().Object;
  } else {
    str = js::ToString<CanGC>(cx, exnStack.exception());
  }

  if (!str) {
    cx->clearPendingException();
  }

  // If ErrorFromException didn't get us a JSErrorReport, then the object
  // was not an ErrorObject, security-wrapped or otherwise. However, it might
  // still quack like one. Give duck-typing a chance.  We start by looking for
  // "filename" (all lowercase), since that's where DOMExceptions store their
  // filename.  Then we check "fileName", which is where Errors store it.  We
  // have to do it in that order, because DOMExceptions have Error.prototype
  // on their proto chain, and hence also have a "fileName" property, but its
  // value is "".
  const char* filename_str = "filename";
  if (!reportp && exnObject && sniffingBehavior == WithSideEffects &&
      IsDuckTypedErrorObject(cx, exnObject, &filename_str)) {
    // Temporary value for pulling properties off of duck-typed objects.
    RootedValue val(cx);

    RootedString name(cx);
    if (JS_GetProperty(cx, exnObject, "name", &val) && val.isString()) {
      name = val.toString();
    } else {
      cx->clearPendingException();
    }

    RootedString msg(cx);
    if (JS_GetProperty(cx, exnObject, "message", &val) && val.isString()) {
      msg = val.toString();
    } else {
      cx->clearPendingException();
    }

    // If we have the right fields, override the ToString we performed on
    // the exception object above with something built out of its quacks
    // (i.e. as much of |NameQuack: MessageQuack| as we can make).
    str = FormatErrorMessage(cx, name, msg);

    {
      AutoClearPendingException acpe(cx);
      if (JS_GetProperty(cx, exnObject, filename_str, &val)) {
        RootedString tmp(cx, js::ToString<CanGC>(cx, val));
        if (tmp) {
          filename = JS_EncodeStringToUTF8(cx, tmp);
        }
      }
    }
    if (!filename) {
      filename = DuplicateString("");
      if (!filename) {
        ReportOutOfMemory(cx);
        return false;
      }
    }

    uint32_t lineno;
    if (!JS_GetProperty(cx, exnObject, "lineNumber", &val) ||
        !ToUint32(cx, val, &lineno)) {
      cx->clearPendingException();
      lineno = 0;
    }

    uint32_t column;
    if (!JS_GetProperty(cx, exnObject, "columnNumber", &val) ||
        !ToUint32(cx, val, &column)) {
      cx->clearPendingException();
      column = 0;
    }

    reportp = &ownedReport;
    new (reportp) JSErrorReport();
    ownedReport.filename = JS::ConstUTF8CharsZ(filename.get());
    ownedReport.lineno = lineno;
    ownedReport.exnType = JSEXN_INTERNALERR;
    ownedReport.column = JS::ColumnNumberOneOrigin(column);

    if (str) {
      // Note that using |str| for |message_| here is kind of wrong,
      // because |str| is supposed to be of the format
      // |ErrorName: ErrorMessage|, and |message_| is supposed to
      // correspond to |ErrorMessage|. But this is what we've
      // historically done for duck-typed error objects.
      //
      // If only this stuff could get specced one day...
      if (auto utf8 = JS_EncodeStringToUTF8(cx, str)) {
        ownedReport.initOwnedMessage(utf8.release());
      } else {
        cx->clearPendingException();
        str = nullptr;
      }
    }
  }

  const char* utf8Message = nullptr;
  if (str) {
    toStringResultBytesStorage = JS_EncodeStringToUTF8(cx, str);
    utf8Message = toStringResultBytesStorage.get();
    if (!utf8Message) {
      cx->clearPendingException();
    }
  }
  if (!utf8Message) {
    utf8Message = "unknown (can't convert to string)";
  }

  if (!reportp) {
    // This is basically an inlined version of
    //
    //   JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
    //                            JSMSG_UNCAUGHT_EXCEPTION, utf8Message);
    //
    // but without the reporting bits.  Instead it just puts all
    // the stuff we care about in our ownedReport and message_.
    if (!populateUncaughtExceptionReportUTF8(cx, exnStack.stack(),
                                             utf8Message)) {
      // Just give up.  We're out of memory or something; not much we can
      // do here.
      return false;
    }
  } else {
    toStringResult_ = JS::ConstUTF8CharsZ(utf8Message, strlen(utf8Message));
  }

  return true;
}

bool JS::ErrorReportBuilder::populateUncaughtExceptionReportUTF8(
    JSContext* cx, HandleObject stack, ...) {
  va_list ap;
  va_start(ap, stack);
  bool ok = populateUncaughtExceptionReportUTF8VA(cx, stack, ap);
  va_end(ap);
  return ok;
}

bool JS::ErrorReportBuilder::populateUncaughtExceptionReportUTF8VA(
    JSContext* cx, HandleObject stack, va_list ap) {
  new (&ownedReport) JSErrorReport();
  ownedReport.isWarning_ = false;
  ownedReport.errorNumber = JSMSG_UNCAUGHT_EXCEPTION;

  bool skippedAsync;
  Rooted<SavedFrame*> frame(
      cx, UnwrapSavedFrame(cx, cx->realm()->principals(), stack,
                           SavedFrameSelfHosted::Exclude, skippedAsync));
  if (frame) {
    filename = StringToNewUTF8CharsZ(cx, *frame->getSource());
    if (!filename) {
      return false;
    }

    // |ownedReport.filename| inherits the lifetime of |ErrorReport::filename|.
    ownedReport.filename = JS::ConstUTF8CharsZ(filename.get());
    ownedReport.sourceId = frame->getSourceId();
    ownedReport.lineno = frame->getLine();
    ownedReport.column =
        JS::ColumnNumberOneOrigin(frame->getColumn().oneOriginValue());
    ownedReport.isMuted = frame->getMutedErrors();
  } else {
    // XXXbz this assumes the stack we have right now is still
    // related to our exception object.
    NonBuiltinFrameIter iter(cx, cx->realm()->principals());
    if (!iter.done()) {
      ownedReport.filename = JS::ConstUTF8CharsZ(iter.filename());
      JS::TaggedColumnNumberOneOrigin column;
      ownedReport.sourceId =
          iter.hasScript() ? iter.script()->scriptSource()->id() : 0;
      ownedReport.lineno = iter.computeLine(&column);
      ownedReport.column = JS::ColumnNumberOneOrigin(column.oneOriginValue());
      ownedReport.isMuted = iter.mutedErrors();
    }
  }

  AutoReportFrontendContext fc(cx);
  if (!ExpandErrorArgumentsVA(&fc, GetErrorMessage, nullptr,
                              JSMSG_UNCAUGHT_EXCEPTION, ArgumentsAreUTF8,
                              &ownedReport, ap)) {
    return false;
  }

  toStringResult_ = ownedReport.message();
  reportp = &ownedReport;
  return true;
}

JSObject* js::CopyErrorObject(JSContext* cx, Handle<ErrorObject*> err) {
  UniquePtr<JSErrorReport> copyReport;
  if (JSErrorReport* errorReport = err->getErrorReport()) {
    copyReport = CopyErrorReport(cx, errorReport);
    if (!copyReport) {
      return nullptr;
    }
  }

  RootedString message(cx, err->getMessage());
  if (message && !cx->compartment()->wrap(cx, &message)) {
    return nullptr;
  }
  RootedString fileName(cx, err->fileName(cx));
  if (!cx->compartment()->wrap(cx, &fileName)) {
    return nullptr;
  }
  RootedObject stack(cx, err->stack());
  if (!cx->compartment()->wrap(cx, &stack)) {
    return nullptr;
  }
  if (stack && JS_IsDeadWrapper(stack)) {
    // ErrorObject::create expects |stack| to be either nullptr or a (possibly
    // wrapped) SavedFrame instance.
    stack = nullptr;
  }
  Rooted<mozilla::Maybe<Value>> cause(cx, mozilla::Nothing());
  if (auto maybeCause = err->getCause()) {
    RootedValue errorCause(cx, maybeCause.value());
    if (!cx->compartment()->wrap(cx, &errorCause)) {
      return nullptr;
    }
    cause = mozilla::Some(errorCause.get());
  }
  uint32_t sourceId = err->sourceId();
  uint32_t lineNumber = err->lineNumber();
  JS::ColumnNumberOneOrigin columnNumber = err->columnNumber();
  JSExnType errorType = err->type();

  // Create the Error object.
  return ErrorObject::create(cx, errorType, stack, fileName, sourceId,
                             lineNumber, columnNumber, std::move(copyReport),
                             message, cause);
}

JS_PUBLIC_API bool JS::CreateError(JSContext* cx, JSExnType type,
                                   HandleObject stack, HandleString fileName,
                                   uint32_t lineNumber,
                                   JS::ColumnNumberOneOrigin columnNumber,
                                   JSErrorReport* report, HandleString message,
                                   Handle<mozilla::Maybe<Value>> cause,
                                   MutableHandleValue rval) {
  cx->check(stack, fileName, message);
  AssertObjectIsSavedFrameOrWrapper(cx, stack);

  js::UniquePtr<JSErrorReport> rep;
  if (report) {
    rep = CopyErrorReport(cx, report);
    if (!rep) {
      return false;
    }
  }

  JSObject* obj =
      js::ErrorObject::create(cx, type, stack, fileName, 0, lineNumber,
                              columnNumber, std::move(rep), message, cause);
  if (!obj) {
    return false;
  }

  rval.setObject(*obj);
  return true;
}

const char* js::ValueToSourceForError(JSContext* cx, HandleValue val,
                                      UniqueChars& bytes) {
  if (val.isUndefined()) {
    return "undefined";
  }

  if (val.isNull()) {
    return "null";
  }

  AutoClearPendingException acpe(cx);

  RootedString str(cx, JS_ValueToSource(cx, val));
  if (!str) {
    return "<<error converting value to string>>";
  }

  JSStringBuilder sb(cx);
  if (val.hasObjectPayload()) {
    RootedObject valObj(cx, &val.getObjectPayload());
    ESClass cls;
    if (!JS::GetBuiltinClass(cx, valObj, &cls)) {
      return "<<error determining class of value>>";
    }
    const char* s;
    if (cls == ESClass::Array) {
      s = "the array ";
    } else if (cls == ESClass::ArrayBuffer) {
      s = "the array buffer ";
    } else if (JS_IsArrayBufferViewObject(valObj)) {
      s = "the typed array ";
#ifdef ENABLE_RECORD_TUPLE
    } else if (cls == ESClass::Record) {
      s = "the record ";
    } else if (cls == ESClass::Tuple) {
      s = "the tuple ";
#endif
    } else {
      s = "the object ";
    }
    if (!sb.append(s, strlen(s))) {
      return "<<error converting value to string>>";
    }
  } else if (val.isNumber()) {
    if (!sb.append("the number ")) {
      return "<<error converting value to string>>";
    }
  } else if (val.isString()) {
    if (!sb.append("the string ")) {
      return "<<error converting value to string>>";
    }
  } else if (val.isBigInt()) {
    if (!sb.append("the BigInt ")) {
      return "<<error converting value to string>>";
    }
  } else {
    MOZ_ASSERT(val.isBoolean() || val.isSymbol());
    bytes = StringToNewUTF8CharsZ(cx, *str);
    return bytes.get();
  }
  if (!sb.append(str)) {
    return "<<error converting value to string>>";
  }
  str = sb.finishString();
  if (!str) {
    return "<<error converting value to string>>";
  }
  bytes = StringToNewUTF8CharsZ(cx, *str);
  return bytes.get();
}

bool js::GetInternalError(JSContext* cx, unsigned errorNumber,
                          MutableHandleValue error) {
  FixedInvokeArgs<1> args(cx);
  args[0].set(Int32Value(errorNumber));
  return CallSelfHostedFunction(cx, cx->names().GetInternalError,
                                NullHandleValue, args, error);
}

bool js::GetTypeError(JSContext* cx, unsigned errorNumber,
                      MutableHandleValue error) {
  FixedInvokeArgs<1> args(cx);
  args[0].set(Int32Value(errorNumber));
  return CallSelfHostedFunction(cx, cx->names().GetTypeError, NullHandleValue,
                                args, error);
}

bool js::GetAggregateError(JSContext* cx, unsigned errorNumber,
                           MutableHandleValue error) {
  FixedInvokeArgs<1> args(cx);
  args[0].set(Int32Value(errorNumber));
  return CallSelfHostedFunction(cx, cx->names().GetAggregateError,
                                NullHandleValue, args, error);
}

JS_PUBLIC_API mozilla::Maybe<Value> JS::GetExceptionCause(JSObject* exc) {
  if (!exc->is<ErrorObject>()) {
    return mozilla::Nothing();
  }
  auto& error = exc->as<ErrorObject>();
  return error.getCause();
}
