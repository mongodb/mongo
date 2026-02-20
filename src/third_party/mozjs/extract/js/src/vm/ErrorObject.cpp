/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sw=2 et tw=80:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/ErrorObject-inl.h"

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Maybe.h"

#include <utility>

#include "jsexn.h"
#include "jspubtd.h"
#include "NamespaceImports.h"

#include "gc/AllocKind.h"
#include "gc/GCContext.h"
#include "js/CallArgs.h"
#include "js/CallNonGenericMethod.h"
#include "js/CharacterEncoding.h"  // JS::ConstUTF8CharsZ
#include "js/Class.h"
#include "js/ColumnNumber.h"  // JS::ColumnNumberOneOrigin
#include "js/Conversions.h"
#include "js/ErrorReport.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/StackLimits.h"    // js::AutoCheckRecursionLimit
#include "js/PropertySpec.h"
#include "js/RootingAPI.h"
#include "js/Stack.h"
#include "js/TypeDecls.h"
#include "js/Utility.h"
#include "js/Value.h"
#include "js/Wrapper.h"
#include "util/StringBuilder.h"
#include "vm/ErrorReporting.h"
#include "vm/GlobalObject.h"
#include "vm/Iteration.h"
#include "vm/JSAtomUtils.h"  // ClassName
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/NativeObject.h"
#include "vm/ObjectOperations.h"
#include "vm/SavedStacks.h"
#include "vm/SelfHosting.h"
#include "vm/Shape.h"
#include "vm/Stack.h"
#include "vm/StringType.h"
#include "vm/ToSource.h"  // js::ValueToSource

#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/ObjectOperations-inl.h"
#include "vm/Realm-inl.h"
#include "vm/SavedStacks-inl.h"
#include "vm/Shape-inl.h"

using namespace js;

#define IMPLEMENT_ERROR_PROTO_CLASS(name)                        \
  {#name ".prototype", JSCLASS_HAS_CACHED_PROTO(JSProto_##name), \
   JS_NULL_CLASS_OPS,                                            \
   &ErrorObject::classSpecs[JSProto_##name - JSProto_Error]}

const JSClass ErrorObject::protoClasses[JSEXN_ERROR_LIMIT] = {
    IMPLEMENT_ERROR_PROTO_CLASS(Error),

    IMPLEMENT_ERROR_PROTO_CLASS(InternalError),
    IMPLEMENT_ERROR_PROTO_CLASS(AggregateError),
    IMPLEMENT_ERROR_PROTO_CLASS(EvalError),
    IMPLEMENT_ERROR_PROTO_CLASS(RangeError),
    IMPLEMENT_ERROR_PROTO_CLASS(ReferenceError),
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
    IMPLEMENT_ERROR_PROTO_CLASS(SuppressedError),
#endif
    IMPLEMENT_ERROR_PROTO_CLASS(SyntaxError),
    IMPLEMENT_ERROR_PROTO_CLASS(TypeError),
    IMPLEMENT_ERROR_PROTO_CLASS(URIError),

    IMPLEMENT_ERROR_PROTO_CLASS(DebuggeeWouldRun),
    IMPLEMENT_ERROR_PROTO_CLASS(CompileError),
    IMPLEMENT_ERROR_PROTO_CLASS(LinkError),
    IMPLEMENT_ERROR_PROTO_CLASS(RuntimeError),
#ifdef ENABLE_WASM_JSPI
    IMPLEMENT_ERROR_PROTO_CLASS(SuspendError),
#endif
};

static bool exn_toSource(JSContext* cx, unsigned argc, Value* vp);

static const JSFunctionSpec error_methods[] = {
    JS_FN("toSource", exn_toSource, 0, 0),
    JS_SELF_HOSTED_FN("toString", "ErrorToString", 0, 0),
    JS_FS_END,
};

static bool exn_isError(JSContext* cx, unsigned argc, Value* vp);

static bool exn_captureStackTrace(JSContext* cx, unsigned argc, Value* vp);

static const JSFunctionSpec error_static_methods[] = {
    JS_FN("isError", exn_isError, 1, 0),
    JS_FN("captureStackTrace", exn_captureStackTrace, 2, 0),
    JS_FS_END,
};

// Error.prototype and NativeError.prototype have own .message and .name
// properties.
#define COMMON_ERROR_PROPERTIES(name) \
  JS_STRING_PS("message", "", 0), JS_STRING_PS("name", #name, 0)

static const JSPropertySpec error_properties[] = {
    COMMON_ERROR_PROPERTIES(Error),
    // Only Error.prototype has .stack!
    JS_PSGS("stack", ErrorObject::getStack, ErrorObject::setStack, 0),
    JS_PS_END,
};

#define IMPLEMENT_NATIVE_ERROR_PROPERTIES(name)       \
  static const JSPropertySpec name##_properties[] = { \
      COMMON_ERROR_PROPERTIES(name), JS_PS_END};

IMPLEMENT_NATIVE_ERROR_PROPERTIES(InternalError)
IMPLEMENT_NATIVE_ERROR_PROPERTIES(AggregateError)
IMPLEMENT_NATIVE_ERROR_PROPERTIES(EvalError)
IMPLEMENT_NATIVE_ERROR_PROPERTIES(RangeError)
IMPLEMENT_NATIVE_ERROR_PROPERTIES(ReferenceError)
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
IMPLEMENT_NATIVE_ERROR_PROPERTIES(SuppressedError)
#endif
IMPLEMENT_NATIVE_ERROR_PROPERTIES(SyntaxError)
IMPLEMENT_NATIVE_ERROR_PROPERTIES(TypeError)
IMPLEMENT_NATIVE_ERROR_PROPERTIES(URIError)
IMPLEMENT_NATIVE_ERROR_PROPERTIES(DebuggeeWouldRun)
IMPLEMENT_NATIVE_ERROR_PROPERTIES(CompileError)
IMPLEMENT_NATIVE_ERROR_PROPERTIES(LinkError)
IMPLEMENT_NATIVE_ERROR_PROPERTIES(RuntimeError)
#ifdef ENABLE_WASM_JSPI
IMPLEMENT_NATIVE_ERROR_PROPERTIES(SuspendError)
#endif

#define IMPLEMENT_NATIVE_ERROR_SPEC(name) \
  {ErrorObject::createConstructor,        \
   ErrorObject::createProto,              \
   nullptr,                               \
   nullptr,                               \
   nullptr,                               \
   name##_properties,                     \
   nullptr,                               \
   JSProto_Error}

#define IMPLEMENT_NONGLOBAL_ERROR_SPEC(name) \
  {ErrorObject::createConstructor,           \
   ErrorObject::createProto,                 \
   nullptr,                                  \
   nullptr,                                  \
   nullptr,                                  \
   name##_properties,                        \
   nullptr,                                  \
   JSProto_Error | ClassSpec::DontDefineConstructor}

const ClassSpec ErrorObject::classSpecs[JSEXN_ERROR_LIMIT] = {
    {ErrorObject::createConstructor, ErrorObject::createProto,
     error_static_methods, nullptr, error_methods, error_properties},

    IMPLEMENT_NATIVE_ERROR_SPEC(InternalError),
    IMPLEMENT_NATIVE_ERROR_SPEC(AggregateError),
    IMPLEMENT_NATIVE_ERROR_SPEC(EvalError),
    IMPLEMENT_NATIVE_ERROR_SPEC(RangeError),
    IMPLEMENT_NATIVE_ERROR_SPEC(ReferenceError),
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
    IMPLEMENT_NATIVE_ERROR_SPEC(SuppressedError),
#endif
    IMPLEMENT_NATIVE_ERROR_SPEC(SyntaxError),
    IMPLEMENT_NATIVE_ERROR_SPEC(TypeError),
    IMPLEMENT_NATIVE_ERROR_SPEC(URIError),

    IMPLEMENT_NONGLOBAL_ERROR_SPEC(DebuggeeWouldRun),
    IMPLEMENT_NONGLOBAL_ERROR_SPEC(CompileError),
    IMPLEMENT_NONGLOBAL_ERROR_SPEC(LinkError),
    IMPLEMENT_NONGLOBAL_ERROR_SPEC(RuntimeError),
#ifdef ENABLE_WASM_JSPI
    IMPLEMENT_NONGLOBAL_ERROR_SPEC(SuspendError),
#endif
};

#define IMPLEMENT_ERROR_CLASS_CORE(name, reserved_slots) \
  {#name,                                                \
   JSCLASS_HAS_CACHED_PROTO(JSProto_##name) |            \
       JSCLASS_HAS_RESERVED_SLOTS(reserved_slots) |      \
       JSCLASS_BACKGROUND_FINALIZE,                      \
   &ErrorObjectClassOps,                                 \
   &ErrorObject::classSpecs[JSProto_##name - JSProto_Error]}

#define IMPLEMENT_ERROR_CLASS(name) \
  IMPLEMENT_ERROR_CLASS_CORE(name, ErrorObject::RESERVED_SLOTS)

// Only used for classes that could be a Wasm trap. Classes that use this
// macro should be kept in sync with the exception types that mightBeWasmTrap()
// will return true for.
#define IMPLEMENT_ERROR_CLASS_MAYBE_WASM_TRAP(name) \
  IMPLEMENT_ERROR_CLASS_CORE(name, ErrorObject::RESERVED_SLOTS_MAYBE_WASM_TRAP)

static void exn_finalize(JS::GCContext* gcx, JSObject* obj);

static const JSClassOps ErrorObjectClassOps = {
    nullptr,       // addProperty
    nullptr,       // delProperty
    nullptr,       // enumerate
    nullptr,       // newEnumerate
    nullptr,       // resolve
    nullptr,       // mayResolve
    exn_finalize,  // finalize
    nullptr,       // call
    nullptr,       // construct
    nullptr,       // trace
};

const JSClass ErrorObject::classes[JSEXN_ERROR_LIMIT] = {
    IMPLEMENT_ERROR_CLASS(Error),
    IMPLEMENT_ERROR_CLASS_MAYBE_WASM_TRAP(InternalError),
    IMPLEMENT_ERROR_CLASS(AggregateError),
    IMPLEMENT_ERROR_CLASS(EvalError),
    IMPLEMENT_ERROR_CLASS(RangeError),
    IMPLEMENT_ERROR_CLASS(ReferenceError),
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
    IMPLEMENT_ERROR_CLASS(SuppressedError),
#endif
    IMPLEMENT_ERROR_CLASS(SyntaxError),
    IMPLEMENT_ERROR_CLASS(TypeError),
    IMPLEMENT_ERROR_CLASS(URIError),
    // These Error subclasses are not accessible via the global object:
    IMPLEMENT_ERROR_CLASS(DebuggeeWouldRun),
    IMPLEMENT_ERROR_CLASS(CompileError),
    IMPLEMENT_ERROR_CLASS(LinkError),
    IMPLEMENT_ERROR_CLASS_MAYBE_WASM_TRAP(RuntimeError),
#ifdef ENABLE_WASM_JSPI
    IMPLEMENT_ERROR_CLASS(SuspendError),
#endif
};

static void exn_finalize(JS::GCContext* gcx, JSObject* obj) {
  if (JSErrorReport* report = obj->as<ErrorObject>().getErrorReport()) {
    // Bug 1560019: This allocation is not currently tracked.
    gcx->deleteUntracked(report);
  }
}

static ErrorObject* CreateErrorObject(JSContext* cx, const CallArgs& args,
                                      unsigned messageArg, JSExnType exnType,
                                      HandleObject proto) {
  // Compute the error message, if any.
  RootedString message(cx, nullptr);
  if (args.hasDefined(messageArg)) {
    message = ToString<CanGC>(cx, args[messageArg]);
    if (!message) {
      return nullptr;
    }
  }

  // Don't interpret the two parameters following the message parameter as the
  // non-standard fileName and lineNumber arguments when we have an options
  // object argument.
  bool hasOptions = args.get(messageArg + 1).isObject();

  Rooted<mozilla::Maybe<Value>> cause(cx, mozilla::Nothing());
  if (hasOptions) {
    RootedObject options(cx, &args[messageArg + 1].toObject());

    bool hasCause = false;
    if (!HasProperty(cx, options, cx->names().cause, &hasCause)) {
      return nullptr;
    }

    if (hasCause) {
      RootedValue causeValue(cx);
      if (!GetProperty(cx, options, options, cx->names().cause, &causeValue)) {
        return nullptr;
      }
      cause = mozilla::Some(causeValue.get());
    }
  }

  // Find the scripted caller, but only ones we're allowed to know about.
  NonBuiltinFrameIter iter(cx, cx->realm()->principals());

  RootedString fileName(cx);
  uint32_t sourceId = 0;
  if (!hasOptions && args.length() > messageArg + 1) {
    fileName = ToString<CanGC>(cx, args[messageArg + 1]);
  } else {
    fileName = cx->runtime()->emptyString;
    if (!iter.done()) {
      if (const char* cfilename = iter.filename()) {
        fileName = JS_NewStringCopyUTF8Z(
            cx, JS::ConstUTF8CharsZ(cfilename, strlen(cfilename)));
      }
      if (iter.hasScript()) {
        sourceId = iter.script()->scriptSource()->id();
      }
    }
  }
  if (!fileName) {
    return nullptr;
  }

  uint32_t lineNumber;
  JS::ColumnNumberOneOrigin columnNumber;
  if (!hasOptions && args.length() > messageArg + 2) {
    if (!ToUint32(cx, args[messageArg + 2], &lineNumber)) {
      return nullptr;
    }
  } else {
    JS::TaggedColumnNumberOneOrigin tmp;
    lineNumber = iter.done() ? 0 : iter.computeLine(&tmp);
    columnNumber = JS::ColumnNumberOneOrigin(tmp.oneOriginValue());
  }

  RootedObject stack(cx);
  if (!CaptureStack(cx, &stack)) {
    return nullptr;
  }

  return ErrorObject::create(cx, exnType, stack, fileName, sourceId, lineNumber,
                             columnNumber, nullptr, message, cause, proto);
}

static bool Error(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // ECMA ed. 3, 15.11.1 requires Error, etc., to construct even when
  // called as functions, without operator new.  But as we do not give
  // each constructor a distinct JSClass, we must get the exception type
  // ourselves.
  JSExnType exnType =
      JSExnType(args.callee().as<JSFunction>().getExtendedSlot(0).toInt32());

  MOZ_ASSERT(exnType != JSEXN_AGGREGATEERR,
             "AggregateError has its own constructor function");

#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
  MOZ_ASSERT(exnType != JSEXN_SUPPRESSEDERR,
             "SuppressedError has its own constuctor function");
#endif

  JSProtoKey protoKey =
      JSCLASS_CACHED_PROTO_KEY(&ErrorObject::classes[exnType]);

  // ES6 19.5.1.1 mandates the .prototype lookup happens before the toString
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, protoKey, &proto)) {
    return false;
  }

  auto* obj = CreateErrorObject(cx, args, 0, exnType, proto);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

// AggregateError ( errors, message )
static bool AggregateError(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  mozilla::DebugOnly<JSExnType> exnType =
      JSExnType(args.callee().as<JSFunction>().getExtendedSlot(0).toInt32());

  MOZ_ASSERT(exnType == JSEXN_AGGREGATEERR);

  // Steps 1-2. (9.1.13 OrdinaryCreateFromConstructor, steps 1-2).
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_AggregateError,
                                          &proto)) {
    return false;
  }

  // TypeError anyway, but this gives a better error message.
  if (!args.requireAtLeast(cx, "AggregateError", 1)) {
    return false;
  }

  // 9.1.13 OrdinaryCreateFromConstructor, step 3.
  // Step 3.
  Rooted<ErrorObject*> obj(
      cx, CreateErrorObject(cx, args, 1, JSEXN_AGGREGATEERR, proto));
  if (!obj) {
    return false;
  }

  // Step 4.

  Rooted<ArrayObject*> errorsList(cx);
  if (!IterableToArray(cx, args.get(0), &errorsList)) {
    return false;
  }

  // Step 5.
  RootedValue errorsVal(cx, JS::ObjectValue(*errorsList));
  if (!NativeDefineDataProperty(cx, obj, cx->names().errors, errorsVal, 0)) {
    return false;
  }

  // Step 6.
  args.rval().setObject(*obj);
  return true;
}

#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
// Explicit Resource Management Proposal
// SuppressedError ( error, suppressed, message )
// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-suppressederror
static bool SuppressedError(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  mozilla::DebugOnly<JSExnType> exnType =
      JSExnType(args.callee().as<JSFunction>().getExtendedSlot(0).toInt32());

  MOZ_ASSERT(exnType == JSEXN_SUPPRESSEDERR);

  // Step 1. If NewTarget is undefined, let newTarget be the active function
  // object; else let newTarget be NewTarget.
  // Step 2. Let O be ? OrdinaryCreateFromConstructor(newTarget,
  // "%SuppressedError.prototype%", « [[ErrorData]] »).
  JS::Rooted<JSObject*> proto(cx);

  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_SuppressedError,
                                          &proto)) {
    return false;
  }

  // Step 3. If message is not undefined, then
  // Step 3.a. Let messageString be ? ToString(message).
  // Step 3.b. Perform CreateNonEnumerableDataPropertyOrThrow(O, "message",
  // messageString).
  JS::Rooted<ErrorObject*> obj(
      cx, CreateErrorObject(cx, args, 2, JSEXN_SUPPRESSEDERR, proto));

  if (!obj) {
    return false;
  }

  // Step 4. Perform CreateNonEnumerableDataPropertyOrThrow(O, "error", error).
  JS::Rooted<JS::Value> errorVal(cx, args.get(0));
  if (!NativeDefineDataProperty(cx, obj, cx->names().error, errorVal, 0)) {
    return false;
  }

  // Step 5. Perform CreateNonEnumerableDataPropertyOrThrow(O, "suppressed",
  // suppressed).
  JS::Rooted<JS::Value> suppressedVal(cx, args.get(1));
  if (!NativeDefineDataProperty(cx, obj, cx->names().suppressed, suppressedVal,
                                0)) {
    return false;
  }

  // Step 6. Return O.
  args.rval().setObject(*obj);
  return true;
}
#endif

/* static */
JSObject* ErrorObject::createProto(JSContext* cx, JSProtoKey key) {
  JSExnType type = ExnTypeFromProtoKey(key);

  if (type == JSEXN_ERR) {
    return GlobalObject::createBlankPrototype(
        cx, cx->global(), &ErrorObject::protoClasses[JSEXN_ERR]);
  }

  RootedObject protoProto(
      cx, GlobalObject::getOrCreateErrorPrototype(cx, cx->global()));
  if (!protoProto) {
    return nullptr;
  }

  return GlobalObject::createBlankPrototypeInheriting(
      cx, &ErrorObject::protoClasses[type], protoProto);
}

/* static */
JSObject* ErrorObject::createConstructor(JSContext* cx, JSProtoKey key) {
  JSExnType type = ExnTypeFromProtoKey(key);
  RootedObject ctor(cx);

  if (type == JSEXN_ERR) {
    ctor = GenericCreateConstructor<Error, 1, gc::AllocKind::FUNCTION_EXTENDED>(
        cx, key);
  } else {
    RootedFunction proto(
        cx, GlobalObject::getOrCreateErrorConstructor(cx, cx->global()));
    if (!proto) {
      return nullptr;
    }

    Native native;
    unsigned nargs;
    if (type == JSEXN_AGGREGATEERR) {
      native = AggregateError;
      nargs = 2;
    }
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
    else if (type == JSEXN_SUPPRESSEDERR) {
      native = SuppressedError;
      nargs = 3;
    }
#endif
    else {
      native = Error;
      nargs = 1;
    }

    ctor =
        NewFunctionWithProto(cx, native, nargs, FunctionFlags::NATIVE_CTOR,
                             nullptr, ClassName(key, cx), proto,
                             gc::AllocKind::FUNCTION_EXTENDED, TenuredObject);
  }

  if (!ctor) {
    return nullptr;
  }

  ctor->as<JSFunction>().setExtendedSlot(0, Int32Value(type));
  return ctor;
}

/* static */
SharedShape* js::ErrorObject::assignInitialShape(JSContext* cx,
                                                 Handle<ErrorObject*> obj) {
  MOZ_ASSERT(obj->empty());

  constexpr PropertyFlags propFlags = {PropertyFlag::Configurable,
                                       PropertyFlag::Writable};

  if (!NativeObject::addPropertyInReservedSlot(cx, obj, cx->names().fileName,
                                               FILENAME_SLOT, propFlags)) {
    return nullptr;
  }

  if (!NativeObject::addPropertyInReservedSlot(cx, obj, cx->names().lineNumber,
                                               LINENUMBER_SLOT, propFlags)) {
    return nullptr;
  }

  if (!NativeObject::addPropertyInReservedSlot(
          cx, obj, cx->names().columnNumber, COLUMNNUMBER_SLOT, propFlags)) {
    return nullptr;
  }

  return obj->sharedShape();
}

/* static */
bool js::ErrorObject::init(JSContext* cx, Handle<ErrorObject*> obj,
                           JSExnType type, UniquePtr<JSErrorReport> errorReport,
                           HandleString fileName, HandleObject stack,
                           uint32_t sourceId, uint32_t lineNumber,
                           JS::ColumnNumberOneOrigin columnNumber,
                           HandleString message,
                           Handle<mozilla::Maybe<JS::Value>> cause) {
  MOZ_ASSERT(JSEXN_ERR <= type && type < JSEXN_ERROR_LIMIT);
  AssertObjectIsSavedFrameOrWrapper(cx, stack);
  cx->check(obj, stack);

  // Null out early in case of error, for exn_finalize's sake.
  obj->initReservedSlot(ERROR_REPORT_SLOT, PrivateValue(nullptr));

  if (!SharedShape::ensureInitialCustomShape<ErrorObject>(cx, obj)) {
    return false;
  }

  // The .message property isn't part of the initial shape because it's
  // present in some error objects -- |Error.prototype|, |new Error("f")|,
  // |new Error("")| -- but not in others -- |new Error(undefined)|,
  // |new Error()|.
  if (message) {
    constexpr PropertyFlags propFlags = {PropertyFlag::Configurable,
                                         PropertyFlag::Writable};
    if (!NativeObject::addPropertyInReservedSlot(cx, obj, cx->names().message,
                                                 MESSAGE_SLOT, propFlags)) {
      return false;
    }
  }

  // Similar to the .message property, .cause is present only in some error
  // objects -- |new Error("f", {cause: cause})| -- but not in other --
  // |Error.prototype|, |new Error()|, |new Error("f")|.
  if (cause.isSome()) {
    constexpr PropertyFlags propFlags = {PropertyFlag::Configurable,
                                         PropertyFlag::Writable};
    if (!NativeObject::addPropertyInReservedSlot(cx, obj, cx->names().cause,
                                                 CAUSE_SLOT, propFlags)) {
      return false;
    }
  }

  MOZ_ASSERT(obj->lookupPure(NameToId(cx->names().fileName))->slot() ==
             FILENAME_SLOT);
  MOZ_ASSERT(obj->lookupPure(NameToId(cx->names().lineNumber))->slot() ==
             LINENUMBER_SLOT);
  MOZ_ASSERT(obj->lookupPure(NameToId(cx->names().columnNumber))->slot() ==
             COLUMNNUMBER_SLOT);
  MOZ_ASSERT_IF(
      message,
      obj->lookupPure(NameToId(cx->names().message))->slot() == MESSAGE_SLOT);
  MOZ_ASSERT_IF(
      cause.isSome(),
      obj->lookupPure(NameToId(cx->names().cause))->slot() == CAUSE_SLOT);

  JSErrorReport* report = errorReport.release();
  obj->initReservedSlot(STACK_SLOT, ObjectOrNullValue(stack));
  obj->setReservedSlot(ERROR_REPORT_SLOT, PrivateValue(report));
  obj->initReservedSlot(FILENAME_SLOT, StringValue(fileName));
  obj->initReservedSlot(LINENUMBER_SLOT, Int32Value(lineNumber));
  obj->initReservedSlot(COLUMNNUMBER_SLOT,
                        Int32Value(columnNumber.oneOriginValue()));
  if (message) {
    obj->initReservedSlot(MESSAGE_SLOT, StringValue(message));
  }
  if (cause.isSome()) {
    obj->initReservedSlot(CAUSE_SLOT, *cause.get());
  } else {
    obj->initReservedSlot(CAUSE_SLOT, MagicValue(JS_ERROR_WITHOUT_CAUSE));
  }
  obj->initReservedSlot(SOURCEID_SLOT, Int32Value(sourceId));
  if (obj->mightBeWasmTrap()) {
    MOZ_ASSERT(JSCLASS_RESERVED_SLOTS(obj->getClass()) > WASM_TRAP_SLOT);
    obj->initReservedSlot(WASM_TRAP_SLOT, BooleanValue(false));
  }

  return true;
}

/* static */
ErrorObject* js::ErrorObject::create(JSContext* cx, JSExnType errorType,
                                     HandleObject stack, HandleString fileName,
                                     uint32_t sourceId, uint32_t lineNumber,
                                     JS::ColumnNumberOneOrigin columnNumber,
                                     UniquePtr<JSErrorReport> report,
                                     HandleString message,
                                     Handle<mozilla::Maybe<JS::Value>> cause,
                                     HandleObject protoArg /* = nullptr */) {
  AssertObjectIsSavedFrameOrWrapper(cx, stack);

  RootedObject proto(cx, protoArg);
  if (!proto) {
    proto = GlobalObject::getOrCreateCustomErrorPrototype(cx, cx->global(),
                                                          errorType);
    if (!proto) {
      return nullptr;
    }
  }

  Rooted<ErrorObject*> errObject(cx);
  {
    const JSClass* clasp = ErrorObject::classForType(errorType);
    JSObject* obj = NewObjectWithGivenProto(cx, clasp, proto);
    if (!obj) {
      return nullptr;
    }
    errObject = &obj->as<ErrorObject>();
  }

  if (!ErrorObject::init(cx, errObject, errorType, std::move(report), fileName,
                         stack, sourceId, lineNumber, columnNumber, message,
                         cause)) {
    return nullptr;
  }

  return errObject;
}

JSErrorReport* js::ErrorObject::getOrCreateErrorReport(JSContext* cx) {
  if (JSErrorReport* r = getErrorReport()) {
    return r;
  }

  // We build an error report on the stack and then use CopyErrorReport to do
  // the nitty-gritty malloc stuff.
  JSErrorReport report;

  // Type.
  JSExnType type_ = type();
  report.exnType = type_;

  // Filename.
  RootedString filename(cx, fileName(cx));
  UniqueChars filenameStr = JS_EncodeStringToUTF8(cx, filename);
  if (!filenameStr) {
    return nullptr;
  }
  report.filename = JS::ConstUTF8CharsZ(filenameStr.get());

  // Coordinates.
  report.sourceId = sourceId();
  report.lineno = lineNumber();
  report.column = columnNumber();

  // Message. Note that |new Error()| will result in an undefined |message|
  // slot, so we need to explicitly substitute the empty string in that case.
  RootedString message(cx, getMessage());
  if (!message) {
    message = cx->runtime()->emptyString;
  }

  UniqueChars utf8 = StringToNewUTF8CharsZ(cx, *message);
  if (!utf8) {
    return nullptr;
  }
  report.initOwnedMessage(utf8.release());

  // Cache and return.
  UniquePtr<JSErrorReport> copy = CopyErrorReport(cx, &report);
  if (!copy) {
    return nullptr;
  }
  setReservedSlot(ERROR_REPORT_SLOT, PrivateValue(copy.get()));
  return copy.release();
}

static bool FindErrorInstanceOrPrototype(JSContext* cx, HandleObject obj,
                                         MutableHandleObject result) {
  // Walk up the prototype chain until we find an error object instance or
  // prototype object. This allows code like:
  //  Object.create(Error.prototype).stack
  // or
  //   function NYI() { }
  //   NYI.prototype = new Error;
  //   (new NYI).stack
  // to continue returning stacks that are useless, but at least don't throw.

  RootedObject curr(cx, obj);
  RootedObject target(cx);
  do {
    target = CheckedUnwrapStatic(curr);
    if (!target) {
      ReportAccessDenied(cx);
      return false;
    }
    if (IsErrorProtoKey(StandardProtoKeyOrNull(target))) {
      result.set(target);
      return true;
    }

    if (!GetPrototype(cx, curr, &curr)) {
      return false;
    }
  } while (curr);

  // We walked the whole prototype chain and did not find an Error
  // object.
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_INCOMPATIBLE_PROTO, "Error", "(get stack)",
                            obj->getClass()->name);
  return false;
}

static MOZ_ALWAYS_INLINE bool IsObject(HandleValue v) { return v.isObject(); }

// This is a helper method for telemetry to provide feedback for
// proposal-error-stack-accessor and can be removed (Bug 1943623).
// It is based upon the implementation of exn_isError.
static bool HasErrorDataSlot(JSContext* cx, HandleObject obj) {
  JSObject* unwrappedObject = CheckedUnwrapStatic(obj);
  if (!unwrappedObject) {
    return false;
  }

  if (JS_IsDeadWrapper(unwrappedObject)) {
    return false;
  }

  if (unwrappedObject->is<ErrorObject>()) {
    return true;
  }
  if (unwrappedObject->getClass()->isDOMClass()) {
    return cx->runtime()->DOMcallbacks->instanceClassIsError(
        unwrappedObject->getClass());
  }

  return false;
}

/* static */
bool js::ErrorObject::getStack(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  // We accept any object here, because of poor-man's subclassing of Error.
  return CallNonGenericMethod<IsObject, getStack_impl>(cx, args);
}

/* static */
bool js::ErrorObject::getStack_impl(JSContext* cx, const CallArgs& args) {
  RootedObject thisObj(cx, &args.thisv().toObject());

  // This telemetry to provide feedback for proposal-error-stack-accessor and
  // can later be removed (Bug 1943623).
  cx->runtime()->setUseCounter(cx->global(), JSUseCounter::ERRORSTACK_GETTER);
  if (!HasErrorDataSlot(cx, thisObj)) {
    cx->runtime()->setUseCounter(cx->global(),
                                 JSUseCounter::ERRORSTACK_GETTER_NO_ERRORDATA);
  }

  RootedObject obj(cx);
  if (!FindErrorInstanceOrPrototype(cx, thisObj, &obj)) {
    return false;
  }

  if (!obj->is<ErrorObject>()) {
    args.rval().setString(cx->runtime()->emptyString);
    return true;
  }

  // Do frame filtering based on the ErrorObject's principals. This ensures we
  // don't see chrome frames when chrome code accesses .stack over Xrays.
  JSPrincipals* principals = obj->as<ErrorObject>().realm()->principals();

  RootedObject savedFrameObj(cx, obj->as<ErrorObject>().stack());
  RootedString stackString(cx);
  if (!BuildStackString(cx, principals, savedFrameObj, &stackString)) {
    return false;
  }

  if (cx->runtime()->stackFormat() == js::StackFormat::V8) {
    // When emulating V8 stack frames, we also need to prepend the
    // stringified Error to the stack string.
    Handle<PropertyName*> name = cx->names().ErrorToStringWithTrailingNewline;
    FixedInvokeArgs<0> args2(cx);
    RootedValue rval(cx);
    if (!CallSelfHostedFunction(cx, name, args.thisv(), args2, &rval)) {
      return false;
    }

    if (!rval.isString()) {
      args.rval().setString(cx->runtime()->emptyString);
      return true;
    }

    RootedString stringified(cx, rval.toString());
    stackString = ConcatStrings<CanGC>(cx, stringified, stackString);
  }

  args.rval().setString(stackString);
  return true;
}

/* static */
bool js::ErrorObject::setStack(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  // We accept any object here, because of poor-man's subclassing of Error.
  return CallNonGenericMethod<IsObject, setStack_impl>(cx, args);
}

/* static */
bool js::ErrorObject::setStack_impl(JSContext* cx, const CallArgs& args) {
  RootedObject thisObj(cx, &args.thisv().toObject());

  if (!args.requireAtLeast(cx, "(set stack)", 1)) {
    return false;
  }
  RootedValue val(cx, args[0]);

  // This telemetry to provide feedback for proposal-error-stack-accessor and
  // can later be removed (Bug 1943623).
  cx->runtime()->setUseCounter(cx->global(), JSUseCounter::ERRORSTACK_SETTER);
  if (!val.isString()) {
    cx->runtime()->setUseCounter(cx->global(),
                                 JSUseCounter::ERRORSTACK_SETTER_NONSTRING);
  }
  if (!HasErrorDataSlot(cx, thisObj)) {
    cx->runtime()->setUseCounter(cx->global(),
                                 JSUseCounter::ERRORSTACK_SETTER_NO_ERRORDATA);
  }
  return DefineDataProperty(cx, thisObj, cx->names().stack, val);
}

void js::ErrorObject::setFromWasmTrap() {
  MOZ_ASSERT(mightBeWasmTrap());
  MOZ_ASSERT(JSCLASS_RESERVED_SLOTS(getClass()) > WASM_TRAP_SLOT);
  setReservedSlot(WASM_TRAP_SLOT, BooleanValue(true));
}

JSString* js::ErrorToSource(JSContext* cx, HandleObject obj) {
  AutoCycleDetector detector(cx, obj);
  if (!detector.init()) {
    return nullptr;
  }
  if (detector.foundCycle()) {
    return NewStringCopyZ<CanGC>(cx, "{}");
  }

  RootedValue nameVal(cx);
  RootedString name(cx);
  if (!GetProperty(cx, obj, obj, cx->names().name, &nameVal) ||
      !(name = ToString<CanGC>(cx, nameVal))) {
    return nullptr;
  }

  RootedValue messageVal(cx);
  RootedString message(cx);
  if (!GetProperty(cx, obj, obj, cx->names().message, &messageVal) ||
      !(message = ValueToSource(cx, messageVal))) {
    return nullptr;
  }

  RootedValue filenameVal(cx);
  RootedString filename(cx);
  if (!GetProperty(cx, obj, obj, cx->names().fileName, &filenameVal) ||
      !(filename = ValueToSource(cx, filenameVal))) {
    return nullptr;
  }

  RootedValue errorsVal(cx);
  RootedString errors(cx);
  bool isAggregateError = obj->is<ErrorObject>() &&
                          obj->as<ErrorObject>().type() == JSEXN_AGGREGATEERR;
  if (isAggregateError) {
    if (!GetProperty(cx, obj, obj, cx->names().errors, &errorsVal) ||
        !(errors = ValueToSource(cx, errorsVal))) {
      return nullptr;
    }
  }

  RootedValue linenoVal(cx);
  uint32_t lineno;
  if (!GetProperty(cx, obj, obj, cx->names().lineNumber, &linenoVal) ||
      !ToUint32(cx, linenoVal, &lineno)) {
    return nullptr;
  }

  JSStringBuilder sb(cx);
  if (!sb.append("(new ") || !sb.append(name) || !sb.append("(")) {
    return nullptr;
  }

  if (isAggregateError) {
    if (!sb.append(errors) || !sb.append(", ")) {
      return nullptr;
    }
  }

  if (!sb.append(message)) {
    return nullptr;
  }

  if (!filename->empty()) {
    if (!sb.append(", ") || !sb.append(filename)) {
      return nullptr;
    }
  }
  if (lineno != 0) {
    /* We have a line, but no filename, add empty string */
    if (filename->empty() && !sb.append(", \"\"")) {
      return nullptr;
    }

    JSString* linenumber = ToString<CanGC>(cx, linenoVal);
    if (!linenumber) {
      return nullptr;
    }
    if (!sb.append(", ") || !sb.append(linenumber)) {
      return nullptr;
    }
  }

  if (!sb.append("))")) {
    return nullptr;
  }

  return sb.finishString();
}

/*
 * Return a string that may eval to something similar to the original object.
 */
static bool exn_toSource(JSContext* cx, unsigned argc, Value* vp) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  JSString* str = ErrorToSource(cx, obj);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Error.isError Proposal
 * Error.isError ( arg )
 * https://tc39.es/proposal-is-error/#sec-error.iserror
 * IsError ( argument )
 * https://tc39.es/proposal-is-error/#sec-iserror
 */
static bool exn_isError(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Error.isError ( arg )
  // Step 1. Return IsError(arg).

  // IsError ( argument )
  // Step 1. If argument is not an Object, return false.
  if (!args.get(0).isObject()) {
    args.rval().setBoolean(false);
    return true;
  }

  JSObject* unwrappedObject = CheckedUnwrapStatic(&args.get(0).toObject());
  if (!unwrappedObject) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_OBJECT_ACCESS_DENIED);
    return false;
  }

  if (JS_IsDeadWrapper(unwrappedObject)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
  }

  // Step 2. If argument has an [[ErrorData]] internal slot, return true.
  if (unwrappedObject->is<ErrorObject>()) {
    args.rval().setBoolean(true);
    return true;
  }
  if (unwrappedObject->getClass()->isDOMClass()) {
    args.rval().setBoolean(cx->runtime()->DOMcallbacks->instanceClassIsError(
        unwrappedObject->getClass()));
    return true;
  }

  // Step 3. Return false
  args.rval().setBoolean(false);
  return true;
}

// The below is the "documentation" from https://v8.dev/docs/stack-trace-api
//
//  ## Stack trace collection for custom exceptions
//
//  The stack trace mechanism used for built-in errors is implemented using a
//  general stack trace collection API that is also available to user scripts.
//  The function
//
//   Error.captureStackTrace(error, constructorOpt)
//
//  adds a stack property to the given error object that yields the stack trace
//  at the time captureStackTrace was called. Stack traces collected through
//  Error.captureStackTrace are immediately collected, formatted, and attached
//  to the given error object.
//
//  The optional constructorOpt parameter allows you to pass in a function
//  value. When collecting the stack trace all frames above the topmost call to
//  this function, including that call, are left out of the stack trace. This
//  can be useful to hide implementation details that won’t be useful to the
//  user. The usual way of defining a custom error that captures a stack trace
//  would be:
//
//   function MyError() {
//     Error.captureStackTrace(this, MyError);
//     // Any other initialization goes here.
//   }
//
//  Passing in MyError as a second argument means that the constructor call to
//  MyError won’t show up in the stack trace.

static bool exn_captureStackTrace(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  const char* callerName = "Error.captureStackTrace";

  if (!args.requireAtLeast(cx, callerName, 1)) {
    return false;
  }

  Rooted<JSObject*> obj(cx,
                        RequireObjectArg(cx, "`target`", callerName, args[0]));
  if (!obj) {
    return false;
  }

  Rooted<JSObject*> caller(cx, nullptr);
  if (args.length() > 1 && args[1].isObject() &&
      args[1].toObject().isCallable()) {
    caller = CheckedUnwrapStatic(&args[1].toObject());
    if (!caller) {
      ReportAccessDenied(cx);
      return false;
    }
  }

  RootedObject stack(cx);
  if (!CaptureCurrentStack(
          cx, &stack, JS::StackCapture(JS::MaxFrames(MAX_REPORTED_STACK_DEPTH)),
          caller)) {
    return false;
  }

  RootedString stackString(cx);

  // Do frame filtering based on the current realm, to filter out any
  // chrome frames which could exist on the stack.
  JSPrincipals* principals = cx->realm()->principals();
  if (!BuildStackString(cx, principals, stack, &stackString)) {
    return false;
  }

  // V8 installs a non-enumerable, configurable getter-setter on the object.
  // JSC installs a non-enumerable, configurable, writable value on the
  // object. We are following JSC here, not V8.
  RootedValue string(cx, StringValue(stackString));
  if (!DefineDataProperty(cx, obj, cx->names().stack, string, 0)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}
