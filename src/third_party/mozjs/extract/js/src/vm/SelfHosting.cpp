/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/SelfHosting.h"

#include "mozilla/Casting.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Maybe.h"
#include "mozilla/Utf8.h"  // mozilla::Utf8Unit

#include <algorithm>
#include <iterator>

#include "jsdate.h"
#include "jsfriendapi.h"
#include "jsmath.h"
#include "jsnum.h"
#include "selfhosted.out.h"

#include "builtin/Array.h"
#include "builtin/BigInt.h"
#ifdef JS_HAS_INTL_API
#  include "builtin/intl/Collator.h"
#  include "builtin/intl/DateTimeFormat.h"
#  include "builtin/intl/DisplayNames.h"
#  include "builtin/intl/IntlObject.h"
#  include "builtin/intl/ListFormat.h"
#  include "builtin/intl/Locale.h"
#  include "builtin/intl/NumberFormat.h"
#  include "builtin/intl/PluralRules.h"
#  include "builtin/intl/RelativeTimeFormat.h"
#endif
#include "builtin/MapObject.h"
#include "builtin/ModuleObject.h"
#include "builtin/Object.h"
#include "builtin/Promise.h"
#include "builtin/Reflect.h"
#include "builtin/RegExp.h"
#include "builtin/SelfHostingDefines.h"
#include "builtin/String.h"
#include "builtin/WeakMapObject.h"
#include "frontend/BytecodeCompilation.h"  // CompileGlobalScriptToStencil
#include "frontend/CompilationStencil.h"   // js::frontend::CompilationStencil
#include "gc/Marking.h"
#include "gc/Policy.h"
#include "jit/AtomicOperations.h"
#include "jit/InlinableNatives.h"
#include "js/CharacterEncoding.h"
#include "js/CompilationAndEvaluation.h"
#include "js/Conversions.h"
#include "js/Date.h"
#include "js/ErrorReport.h"  // JS::PrintError
#include "js/Exception.h"
#include "js/experimental/TypedData.h"  // JS_GetArrayBufferViewType
#include "js/friend/ErrorMessages.h"    // js::GetErrorMessage, JSMSG_*
#include "js/Modules.h"                 // JS::GetModulePrivate
#include "js/PropertySpec.h"
#include "js/ScalarType.h"  // js::Scalar::Type
#include "js/SourceText.h"  // JS::SourceText
#include "js/StableStringChars.h"
#include "js/Transcoding.h"
#include "js/Warnings.h"  // JS::{,Set}WarningReporter
#include "js/Wrapper.h"
#include "util/StringBuffer.h"
#include "vm/ArgumentsObject.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/BigIntType.h"
#include "vm/BytecodeIterator.h"
#include "vm/BytecodeLocation.h"
#include "vm/Compression.h"
#include "vm/DateObject.h"
#include "vm/ErrorReporting.h"  // js::MaybePrintAndClearPendingException
#include "vm/FrameIter.h"       // js::ScriptFrameIter
#include "vm/FunctionFlags.h"   // js::FunctionFlags
#include "vm/GeneratorObject.h"
#include "vm/Interpreter.h"
#include "vm/Iteration.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/PIC.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/Printer.h"
#include "vm/Realm.h"
#include "vm/RegExpObject.h"
#include "vm/StringType.h"
#include "vm/ToSource.h"  // js::ValueToSource
#include "vm/TypedArrayObject.h"
#include "vm/Uint8Clamped.h"
#include "vm/WrapperObject.h"

#include "gc/GC-inl.h"
#include "vm/BooleanObject-inl.h"
#include "vm/BytecodeIterator-inl.h"
#include "vm/BytecodeLocation-inl.h"
#include "vm/Compartment-inl.h"
#include "vm/JSAtom-inl.h"
#include "vm/JSFunction-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/NumberObject-inl.h"
#include "vm/StringObject-inl.h"
#include "vm/TypedArrayObject-inl.h"

using namespace js;
using namespace js::selfhosted;

using JS::AutoCheckCannotGC;
using JS::AutoStableStringChars;
using JS::CompileOptions;
using mozilla::Maybe;

static void selfHosting_WarningReporter(JSContext* cx, JSErrorReport* report) {
  MOZ_ASSERT(report->isWarning());

  JS::PrintError(stderr, report, true);
}

static bool intrinsic_ToObject(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  JSObject* obj = ToObject(cx, args[0]);
  if (!obj) {
    return false;
  }
  args.rval().setObject(*obj);
  return true;
}

static bool intrinsic_IsObject(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  Value val = args[0];
  bool isObject = val.isObject();
  args.rval().setBoolean(isObject);
  return true;
}

static bool intrinsic_IsArray(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  RootedValue val(cx, args[0]);
  if (val.isObject()) {
    RootedObject obj(cx, &val.toObject());
    bool isArray = false;
    if (!IsArray(cx, obj, &isArray)) {
      return false;
    }
    args.rval().setBoolean(isArray);
  } else {
    args.rval().setBoolean(false);
  }
  return true;
}

static bool intrinsic_IsCrossRealmArrayConstructor(JSContext* cx, unsigned argc,
                                                   Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isObject());

  bool result = false;
  if (!IsCrossRealmArrayConstructor(cx, &args[0].toObject(), &result)) {
    return false;
  }
  args.rval().setBoolean(result);
  return true;
}

static bool intrinsic_ToLength(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);

  // Inline fast path for the common case.
  if (args[0].isInt32()) {
    int32_t i = args[0].toInt32();
    args.rval().setInt32(i < 0 ? 0 : i);
    return true;
  }

  uint64_t length = 0;
  if (!ToLength(cx, args[0], &length)) {
    return false;
  }

  args.rval().setNumber(double(length));
  return true;
}

static bool intrinsic_ToInteger(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  double result;
  if (!ToInteger(cx, args[0], &result)) {
    return false;
  }
  args.rval().setNumber(result);
  return true;
}

static bool intrinsic_ToSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  JSString* str = ValueToSource(cx, args[0]);
  if (!str) {
    return false;
  }
  args.rval().setString(str);
  return true;
}

static bool intrinsic_ToPropertyKey(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedId id(cx);
  if (!ToPropertyKey(cx, args[0], &id)) {
    return false;
  }

  args.rval().set(IdToValue(id));
  return true;
}

static bool intrinsic_IsCallable(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setBoolean(IsCallable(args[0]));
  return true;
}

static bool intrinsic_IsConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  args.rval().setBoolean(IsConstructor(args[0]));
  return true;
}

template <typename T>
static bool intrinsic_IsInstanceOfBuiltin(JSContext* cx, unsigned argc,
                                          Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isObject());

  args.rval().setBoolean(args[0].toObject().is<T>());
  return true;
}

template <typename T>
static bool intrinsic_GuardToBuiltin(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isObject());

  if (args[0].toObject().is<T>()) {
    args.rval().setObject(args[0].toObject());
    return true;
  }
  args.rval().setNull();
  return true;
}

template <typename T>
static bool intrinsic_IsWrappedInstanceOfBuiltin(JSContext* cx, unsigned argc,
                                                 Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isObject());

  JSObject* obj = &args[0].toObject();
  if (!obj->is<WrapperObject>()) {
    args.rval().setBoolean(false);
    return true;
  }

  JSObject* unwrapped = CheckedUnwrapDynamic(obj, cx);
  if (!unwrapped) {
    ReportAccessDenied(cx);
    return false;
  }

  args.rval().setBoolean(unwrapped->is<T>());
  return true;
}

template <typename T>
static bool intrinsic_IsPossiblyWrappedInstanceOfBuiltin(JSContext* cx,
                                                         unsigned argc,
                                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isObject());

  JSObject* obj = CheckedUnwrapDynamic(&args[0].toObject(), cx);
  if (!obj) {
    ReportAccessDenied(cx);
    return false;
  }

  args.rval().setBoolean(obj->is<T>());
  return true;
}

static bool intrinsic_SubstringKernel(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args[0].isString());
  MOZ_RELEASE_ASSERT(args[1].isInt32());
  MOZ_RELEASE_ASSERT(args[2].isInt32());

  RootedString str(cx, args[0].toString());
  int32_t begin = args[1].toInt32();
  int32_t length = args[2].toInt32();

  JSString* substr = SubstringKernel(cx, str, begin, length);
  if (!substr) {
    return false;
  }

  args.rval().setString(substr);
  return true;
}

static void ThrowErrorWithType(JSContext* cx, JSExnType type,
                               const CallArgs& args) {
  MOZ_RELEASE_ASSERT(args[0].isInt32());
  uint32_t errorNumber = args[0].toInt32();

#ifdef DEBUG
  const JSErrorFormatString* efs = GetErrorMessage(nullptr, errorNumber);
  MOZ_ASSERT(efs->argCount == args.length() - 1);
  MOZ_ASSERT(efs->exnType == type,
             "error-throwing intrinsic and error number are inconsistent");
#endif

  UniqueChars errorArgs[3];
  for (unsigned i = 1; i < 4 && i < args.length(); i++) {
    HandleValue val = args[i];
    if (val.isInt32() || val.isString()) {
      JSString* str = ToString<CanGC>(cx, val);
      if (!str) {
        return;
      }
      errorArgs[i - 1] = QuoteString(cx, str);
    } else {
      errorArgs[i - 1] =
          DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, val, nullptr);
    }
    if (!errorArgs[i - 1]) {
      return;
    }
  }

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, errorNumber,
                           errorArgs[0].get(), errorArgs[1].get(),
                           errorArgs[2].get());
}

static bool intrinsic_ThrowRangeError(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() >= 1);

  ThrowErrorWithType(cx, JSEXN_RANGEERR, args);
  return false;
}

static bool intrinsic_ThrowTypeError(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() >= 1);

  ThrowErrorWithType(cx, JSEXN_TYPEERR, args);
  return false;
}

static bool intrinsic_ThrowSyntaxError(JSContext* cx, unsigned argc,
                                       Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() >= 1);

  ThrowErrorWithType(cx, JSEXN_SYNTAXERR, args);
  return false;
}

static bool intrinsic_ThrowAggregateError(JSContext* cx, unsigned argc,
                                          Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() >= 1);

  ThrowErrorWithType(cx, JSEXN_AGGREGATEERR, args);
  return false;
}

static bool intrinsic_ThrowInternalError(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() >= 1);

  ThrowErrorWithType(cx, JSEXN_INTERNALERR, args);
  return false;
}

static bool intrinsic_GetErrorMessage(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_RELEASE_ASSERT(args[0].isInt32());

  const JSErrorFormatString* errorString =
      GetErrorMessage(nullptr, args[0].toInt32());
  MOZ_ASSERT(errorString);

  MOZ_ASSERT(errorString->argCount == 0);
  RootedString message(cx, JS_NewStringCopyZ(cx, errorString->format));
  if (!message) {
    return false;
  }

  args.rval().setString(message);
  return true;
}

static bool intrinsic_CreateModuleSyntaxError(JSContext* cx, unsigned argc,
                                              Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 4);
  MOZ_ASSERT(args[0].isObject());
  MOZ_RELEASE_ASSERT(args[1].isInt32());
  MOZ_RELEASE_ASSERT(args[2].isInt32());
  MOZ_ASSERT(args[3].isString());

  RootedModuleObject module(cx, &args[0].toObject().as<ModuleObject>());
  RootedString filename(cx,
                        JS_NewStringCopyZ(cx, module->script()->filename()));
  if (!filename) {
    return false;
  }

  RootedString message(cx, args[3].toString());

  RootedValue error(cx);
  if (!JS::CreateError(cx, JSEXN_SYNTAXERR, nullptr, filename,
                       args[1].toInt32(), args[2].toInt32(), nullptr, message,
                       &error)) {
    return false;
  }

  args.rval().set(error);
  return true;
}

/**
 * Handles an assertion failure in self-hosted code just like an assertion
 * failure in C++ code. Information about the failure can be provided in
 * args[0].
 */
static bool intrinsic_AssertionFailed(JSContext* cx, unsigned argc, Value* vp) {
#ifdef DEBUG
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() > 0) {
    // try to dump the informative string
    JSString* str = ToString<CanGC>(cx, args[0]);
    if (str) {
      js::Fprinter out(stderr);
      out.put("Self-hosted JavaScript assertion info: ");
      str->dumpCharsNoNewline(out);
      out.putChar('\n');
    }
  }
#endif
  MOZ_ASSERT(false);
  return false;
}

/**
 * Dumps a message to stderr, after stringifying it. Doesn't append a newline.
 */
static bool intrinsic_DumpMessage(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
#ifdef DEBUG
  if (args.length() > 0) {
    // try to dump the informative string
    js::Fprinter out(stderr);
    JSString* str = ToString<CanGC>(cx, args[0]);
    if (str) {
      str->dumpCharsNoNewline(out);
      out.putChar('\n');
    } else {
      cx->recoverFromOutOfMemory();
    }
  }
#endif
  args.rval().setUndefined();
  return true;
}

static bool intrinsic_FinishBoundFunctionInit(JSContext* cx, unsigned argc,
                                              Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);
  MOZ_ASSERT(IsCallable(args[1]));
  MOZ_RELEASE_ASSERT(args[2].isInt32());

  RootedFunction bound(cx, &args[0].toObject().as<JSFunction>());
  RootedObject targetObj(cx, &args[1].toObject());
  int32_t argCount = args[2].toInt32();

  args.rval().setUndefined();
  return JSFunction::finishBoundFunctionInit(cx, bound, targetObj, argCount);
}

/*
 * Used to decompile values in the nearest non-builtin stack frame, falling
 * back to decompiling in the current frame. Helpful for printing higher-order
 * function arguments.
 *
 * The user must supply the argument number of the value in question; it
 * _cannot_ be automatically determined.
 */
static bool intrinsic_DecompileArg(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  MOZ_RELEASE_ASSERT(args[0].isInt32());

  HandleValue value = args[1];
  JSString* str = DecompileArgument(cx, args[0].toInt32(), value);
  if (!str) {
    return false;
  }
  args.rval().setString(str);
  return true;
}

static bool intrinsic_DefineDataProperty(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // When DefineDataProperty is called with 3 arguments, it's compiled to
  // JSOp::InitElem in the bytecode emitter so we shouldn't get here.
  MOZ_ASSERT(args.length() == 4);
  MOZ_ASSERT(args[0].isObject());
  MOZ_RELEASE_ASSERT(args[3].isInt32());

  RootedObject obj(cx, &args[0].toObject());
  RootedId id(cx);
  if (!ToPropertyKey(cx, args[1], &id)) {
    return false;
  }
  RootedValue value(cx, args[2]);

  JS::PropertyAttributes attrs;
  unsigned attributes = args[3].toInt32();

  MOZ_ASSERT(bool(attributes & ATTR_ENUMERABLE) !=
                 bool(attributes & ATTR_NONENUMERABLE),
             "DefineDataProperty must receive either ATTR_ENUMERABLE xor "
             "ATTR_NONENUMERABLE");
  if (attributes & ATTR_ENUMERABLE) {
    attrs += JS::PropertyAttribute::Enumerable;
  }

  MOZ_ASSERT(bool(attributes & ATTR_CONFIGURABLE) !=
                 bool(attributes & ATTR_NONCONFIGURABLE),
             "DefineDataProperty must receive either ATTR_CONFIGURABLE xor "
             "ATTR_NONCONFIGURABLE");
  if (attributes & ATTR_CONFIGURABLE) {
    attrs += JS::PropertyAttribute::Configurable;
  }

  MOZ_ASSERT(
      bool(attributes & ATTR_WRITABLE) != bool(attributes & ATTR_NONWRITABLE),
      "DefineDataProperty must receive either ATTR_WRITABLE xor "
      "ATTR_NONWRITABLE");
  if (attributes & ATTR_WRITABLE) {
    attrs += JS::PropertyAttribute::Writable;
  }

  Rooted<PropertyDescriptor> desc(cx, PropertyDescriptor::Data(value, attrs));
  if (!DefineProperty(cx, obj, id, desc)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

static bool intrinsic_DefineProperty(JSContext* cx, unsigned argc, Value* vp) {
  // _DefineProperty(object, propertyKey, attributes,
  //                 valueOrGetter, setter, strict)
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 6);
  MOZ_ASSERT(args[0].isObject());
  MOZ_ASSERT(args[1].isString() || args[1].isNumber() || args[1].isSymbol());
  MOZ_RELEASE_ASSERT(args[2].isInt32());
  MOZ_ASSERT(args[5].isBoolean());

  RootedObject obj(cx, &args[0].toObject());
  RootedId id(cx);
  if (!PrimitiveValueToId<CanGC>(cx, args[1], &id)) {
    return false;
  }

  Rooted<PropertyDescriptor> desc(cx, PropertyDescriptor::Empty());

  unsigned attributes = args[2].toInt32();
  if (attributes & (ATTR_ENUMERABLE | ATTR_NONENUMERABLE)) {
    desc.setEnumerable(attributes & ATTR_ENUMERABLE);
  }

  if (attributes & (ATTR_CONFIGURABLE | ATTR_NONCONFIGURABLE)) {
    desc.setConfigurable(attributes & ATTR_CONFIGURABLE);
  }

  if (attributes & (ATTR_WRITABLE | ATTR_NONWRITABLE)) {
    desc.setWritable(attributes & ATTR_WRITABLE);
  }

  // When args[4] is |null|, the data descriptor has a value component.
  if ((attributes & DATA_DESCRIPTOR_KIND) && args[4].isNull()) {
    desc.setValue(args[3]);
  }

  if (attributes & ACCESSOR_DESCRIPTOR_KIND) {
    Value getter = args[3];
    if (getter.isObject()) {
      desc.setGetter(&getter.toObject());
    } else if (getter.isUndefined()) {
      desc.setGetter(nullptr);
    } else {
      MOZ_ASSERT(getter.isNull());
    }

    Value setter = args[4];
    if (setter.isObject()) {
      desc.setSetter(&setter.toObject());
    } else if (setter.isUndefined()) {
      desc.setSetter(nullptr);
    } else {
      MOZ_ASSERT(setter.isNull());
    }
  }

  desc.assertValid();

  ObjectOpResult result;
  if (!DefineProperty(cx, obj, id, desc, result)) {
    return false;
  }

  bool strict = args[5].toBoolean();
  if (strict && !result.ok()) {
    // We need to tell our caller Object.defineProperty,
    // that this operation failed, without actually throwing
    // for web-compatibility reasons.
    if (result.failureCode() == JSMSG_CANT_DEFINE_WINDOW_NC) {
      args.rval().setBoolean(false);
      return true;
    }

    return result.reportError(cx, obj, id);
  }

  args.rval().setBoolean(result.ok());
  return true;
}

static bool intrinsic_ObjectHasPrototype(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);

  // Self-hosted code calls this intrinsic with builtin prototypes. These are
  // always native objects.
  auto* obj = &args[0].toObject().as<NativeObject>();
  auto* proto = &args[1].toObject().as<NativeObject>();

  JSObject* actualProto = obj->staticPrototype();
  args.rval().setBoolean(actualProto == proto);
  return true;
}

static bool intrinsic_UnsafeSetReservedSlot(JSContext* cx, unsigned argc,
                                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);
  MOZ_ASSERT(args[0].isObject());
  MOZ_RELEASE_ASSERT(args[1].isInt32());
  MOZ_ASSERT(args[1].toInt32() >= 0);

  uint32_t slot = uint32_t(args[1].toInt32());
  args[0].toObject().as<NativeObject>().setReservedSlot(slot, args[2]);
  args.rval().setUndefined();
  return true;
}

static bool intrinsic_UnsafeGetReservedSlot(JSContext* cx, unsigned argc,
                                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  MOZ_ASSERT(args[0].isObject());
  MOZ_RELEASE_ASSERT(args[1].isInt32());
  MOZ_ASSERT(args[1].toInt32() >= 0);

  uint32_t slot = uint32_t(args[1].toInt32());
  args.rval().set(args[0].toObject().as<NativeObject>().getReservedSlot(slot));
  return true;
}

static bool intrinsic_UnsafeGetObjectFromReservedSlot(JSContext* cx,
                                                      unsigned argc,
                                                      Value* vp) {
  if (!intrinsic_UnsafeGetReservedSlot(cx, argc, vp)) {
    return false;
  }
  MOZ_ASSERT(vp->isObject());
  return true;
}

static bool intrinsic_UnsafeGetInt32FromReservedSlot(JSContext* cx,
                                                     unsigned argc, Value* vp) {
  if (!intrinsic_UnsafeGetReservedSlot(cx, argc, vp)) {
    return false;
  }
  MOZ_ASSERT(vp->isInt32());
  return true;
}

static bool intrinsic_UnsafeGetStringFromReservedSlot(JSContext* cx,
                                                      unsigned argc,
                                                      Value* vp) {
  if (!intrinsic_UnsafeGetReservedSlot(cx, argc, vp)) {
    return false;
  }
  MOZ_ASSERT(vp->isString());
  return true;
}

static bool intrinsic_UnsafeGetBooleanFromReservedSlot(JSContext* cx,
                                                       unsigned argc,
                                                       Value* vp) {
  if (!intrinsic_UnsafeGetReservedSlot(cx, argc, vp)) {
    return false;
  }
  MOZ_ASSERT(vp->isBoolean());
  return true;
}

static bool intrinsic_ThisTimeValue(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isInt32());

  const char* name = nullptr;

  int32_t method = args[0].toInt32();
  if (method == DATE_METHOD_LOCALE_TIME_STRING) {
    name = "toLocaleTimeString";
  } else if (method == DATE_METHOD_LOCALE_DATE_STRING) {
    name = "toLocaleDateString";
  } else {
    MOZ_ASSERT(method == DATE_METHOD_LOCALE_STRING);
    name = "toLocaleString";
  }

  auto* unwrapped = UnwrapAndTypeCheckThis<DateObject>(cx, args, name);
  if (!unwrapped) {
    return false;
  }

  args.rval().set(unwrapped->UTCTime());
  return true;
}

static bool intrinsic_IsPackedArray(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isObject());
  args.rval().setBoolean(IsPackedArray(&args[0].toObject()));
  return true;
}

bool js::intrinsic_NewArrayIterator(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 0);

  JSObject* obj = NewArrayIterator(cx);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

static bool intrinsic_ArrayIteratorPrototypeOptimizable(JSContext* cx,
                                                        unsigned argc,
                                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 0);

  ForOfPIC::Chain* stubChain = ForOfPIC::getOrCreate(cx);
  if (!stubChain) {
    return false;
  }

  bool optimized;
  if (!stubChain->tryOptimizeArrayIteratorNext(cx, &optimized)) {
    return false;
  }
  args.rval().setBoolean(optimized);
  return true;
}

static bool intrinsic_GetNextMapEntryForIterator(JSContext* cx, unsigned argc,
                                                 Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  MOZ_ASSERT(args[0].toObject().is<MapIteratorObject>());
  MOZ_ASSERT(args[1].isObject());

  MapIteratorObject* mapIterator = &args[0].toObject().as<MapIteratorObject>();
  ArrayObject* result = &args[1].toObject().as<ArrayObject>();

  args.rval().setBoolean(MapIteratorObject::next(mapIterator, result));
  return true;
}

static bool intrinsic_CreateMapIterationResultPair(JSContext* cx, unsigned argc,
                                                   Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 0);

  JSObject* result = MapIteratorObject::createResultPair(cx);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

static bool intrinsic_GetNextSetEntryForIterator(JSContext* cx, unsigned argc,
                                                 Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  MOZ_ASSERT(args[0].toObject().is<SetIteratorObject>());
  MOZ_ASSERT(args[1].isObject());

  SetIteratorObject* setIterator = &args[0].toObject().as<SetIteratorObject>();
  ArrayObject* result = &args[1].toObject().as<ArrayObject>();

  args.rval().setBoolean(SetIteratorObject::next(setIterator, result));
  return true;
}

static bool intrinsic_CreateSetIterationResult(JSContext* cx, unsigned argc,
                                               Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 0);

  JSObject* result = SetIteratorObject::createResult(cx);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

bool js::intrinsic_NewStringIterator(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 0);

  JSObject* obj = NewStringIterator(cx);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

bool js::intrinsic_NewRegExpStringIterator(JSContext* cx, unsigned argc,
                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 0);

  JSObject* obj = NewRegExpStringIterator(cx);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

static js::PropertyName* GetUnclonedSelfHostedCanonicalName(JSFunction* fun) {
  if (!fun->isExtended()) {
    return nullptr;
  }
  Value name = fun->getExtendedSlot(CANONICAL_FUNCTION_NAME_SLOT);
  if (!name.isString()) {
    return nullptr;
  }
  return name.toString()->asAtom().asPropertyName();
}

js::PropertyName* js::GetClonedSelfHostedFunctionName(const JSFunction* fun) {
  if (!fun->isExtended()) {
    return nullptr;
  }
  Value name = fun->getExtendedSlot(LAZY_FUNCTION_NAME_SLOT);
  if (!name.isString()) {
    return nullptr;
  }
  return name.toString()->asAtom().asPropertyName();
}

js::PropertyName* js::GetClonedSelfHostedFunctionNameOffMainThread(
    JSFunction* fun) {
  Value name = fun->getExtendedSlotOffMainThread(LAZY_FUNCTION_NAME_SLOT);
  if (!name.isString()) {
    return nullptr;
  }
  return name.toString()->asAtom().asPropertyName();
}

bool js::IsExtendedUnclonedSelfHostedFunctionName(JSAtom* name) {
  if (name->length() < 2) {
    return false;
  }
  return name->latin1OrTwoByteChar(0) ==
         ExtendedUnclonedSelfHostedFunctionNamePrefix;
}

void js::SetUnclonedSelfHostedCanonicalName(JSFunction* fun, JSAtom* name) {
  fun->setExtendedSlot(CANONICAL_FUNCTION_NAME_SLOT, StringValue(name));
}

static void SetClonedSelfHostedFunctionName(JSFunction* fun, JSAtom* name) {
  fun->setExtendedSlot(LAZY_FUNCTION_NAME_SLOT, StringValue(name));
}

static bool intrinsic_GeneratorObjectIsClosed(JSContext* cx, unsigned argc,
                                              Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isObject());

  GeneratorObject* genObj = &args[0].toObject().as<GeneratorObject>();
  args.rval().setBoolean(genObj->isClosed());
  return true;
}

static bool intrinsic_IsSuspendedGenerator(JSContext* cx, unsigned argc,
                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);

  if (!args[0].isObject() || !args[0].toObject().is<GeneratorObject>()) {
    args.rval().setBoolean(false);
    return true;
  }

  GeneratorObject& genObj = args[0].toObject().as<GeneratorObject>();
  args.rval().setBoolean(!genObj.isClosed() && genObj.isSuspended());
  return true;
}

static bool intrinsic_GeneratorIsRunning(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isObject());

  GeneratorObject* genObj = &args[0].toObject().as<GeneratorObject>();
  args.rval().setBoolean(genObj->isRunning());
  return true;
}

static bool intrinsic_GeneratorSetClosed(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isObject());

  GeneratorObject* genObj = &args[0].toObject().as<GeneratorObject>();
  genObj->setClosed();
  return true;
}

template <typename T>
static bool intrinsic_ArrayBufferByteLength(JSContext* cx, unsigned argc,
                                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isObject());
  MOZ_ASSERT(args[0].toObject().is<T>());

  size_t byteLength = args[0].toObject().as<T>().byteLength();
  args.rval().setNumber(byteLength);
  return true;
}

template <typename T>
static bool intrinsic_PossiblyWrappedArrayBufferByteLength(JSContext* cx,
                                                           unsigned argc,
                                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);

  T* obj = args[0].toObject().maybeUnwrapAs<T>();
  if (!obj) {
    ReportAccessDenied(cx);
    return false;
  }

  size_t byteLength = obj->byteLength();
  args.rval().setNumber(byteLength);
  return true;
}

static void AssertNonNegativeInteger(const Value& v) {
  MOZ_ASSERT(v.isNumber());
  MOZ_ASSERT(v.toNumber() >= 0);
  MOZ_ASSERT(v.toNumber() < DOUBLE_INTEGRAL_PRECISION_LIMIT);
  MOZ_ASSERT(JS::ToInteger(v.toNumber()) == v.toNumber());
}

template <typename T>
static bool intrinsic_ArrayBufferCopyData(JSContext* cx, unsigned argc,
                                          Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 6);
  AssertNonNegativeInteger(args[1]);
  AssertNonNegativeInteger(args[3]);
  AssertNonNegativeInteger(args[4]);

  bool isWrapped = args[5].toBoolean();
  Rooted<T*> toBuffer(cx);
  if (!isWrapped) {
    toBuffer = &args[0].toObject().as<T>();
  } else {
    JSObject* wrapped = &args[0].toObject();
    MOZ_ASSERT(wrapped->is<WrapperObject>());
    toBuffer = wrapped->maybeUnwrapAs<T>();
    if (!toBuffer) {
      ReportAccessDenied(cx);
      return false;
    }
  }
  size_t toIndex = size_t(args[1].toNumber());
  Rooted<T*> fromBuffer(cx, &args[2].toObject().as<T>());
  size_t fromIndex = size_t(args[3].toNumber());
  size_t count = size_t(args[4].toNumber());

  T::copyData(toBuffer, toIndex, fromBuffer, fromIndex, count);

  args.rval().setUndefined();
  return true;
}

// Arguments must both be SharedArrayBuffer or wrapped SharedArrayBuffer.
static bool intrinsic_SharedArrayBuffersMemorySame(JSContext* cx, unsigned argc,
                                                   Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);

  auto* lhs = args[0].toObject().maybeUnwrapAs<SharedArrayBufferObject>();
  if (!lhs) {
    ReportAccessDenied(cx);
    return false;
  }
  auto* rhs = args[1].toObject().maybeUnwrapAs<SharedArrayBufferObject>();
  if (!rhs) {
    ReportAccessDenied(cx);
    return false;
  }

  args.rval().setBoolean(lhs->rawBufferObject() == rhs->rawBufferObject());
  return true;
}

static bool intrinsic_GetTypedArrayKind(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isObject());

  static_assert(TYPEDARRAY_KIND_INT8 == Scalar::Type::Int8,
                "TYPEDARRAY_KIND_INT8 doesn't match the scalar type");
  static_assert(TYPEDARRAY_KIND_UINT8 == Scalar::Type::Uint8,
                "TYPEDARRAY_KIND_UINT8 doesn't match the scalar type");
  static_assert(TYPEDARRAY_KIND_INT16 == Scalar::Type::Int16,
                "TYPEDARRAY_KIND_INT16 doesn't match the scalar type");
  static_assert(TYPEDARRAY_KIND_UINT16 == Scalar::Type::Uint16,
                "TYPEDARRAY_KIND_UINT16 doesn't match the scalar type");
  static_assert(TYPEDARRAY_KIND_INT32 == Scalar::Type::Int32,
                "TYPEDARRAY_KIND_INT32 doesn't match the scalar type");
  static_assert(TYPEDARRAY_KIND_UINT32 == Scalar::Type::Uint32,
                "TYPEDARRAY_KIND_UINT32 doesn't match the scalar type");
  static_assert(TYPEDARRAY_KIND_FLOAT32 == Scalar::Type::Float32,
                "TYPEDARRAY_KIND_FLOAT32 doesn't match the scalar type");
  static_assert(TYPEDARRAY_KIND_FLOAT64 == Scalar::Type::Float64,
                "TYPEDARRAY_KIND_FLOAT64 doesn't match the scalar type");
  static_assert(TYPEDARRAY_KIND_UINT8CLAMPED == Scalar::Type::Uint8Clamped,
                "TYPEDARRAY_KIND_UINT8CLAMPED doesn't match the scalar type");
  static_assert(TYPEDARRAY_KIND_BIGINT64 == Scalar::Type::BigInt64,
                "TYPEDARRAY_KIND_BIGINT64 doesn't match the scalar type");
  static_assert(TYPEDARRAY_KIND_BIGUINT64 == Scalar::Type::BigUint64,
                "TYPEDARRAY_KIND_BIGUINT64 doesn't match the scalar type");

  JSObject* obj = &args[0].toObject();
  Scalar::Type type = JS_GetArrayBufferViewType(obj);

  args.rval().setInt32(static_cast<int32_t>(type));
  return true;
}

static bool intrinsic_IsTypedArrayConstructor(JSContext* cx, unsigned argc,
                                              Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isObject());

  args.rval().setBoolean(js::IsTypedArrayConstructor(&args[0].toObject()));
  return true;
}

static bool intrinsic_TypedArrayBuffer(JSContext* cx, unsigned argc,
                                       Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(TypedArrayObject::is(args[0]));

  Rooted<TypedArrayObject*> tarray(cx,
                                   &args[0].toObject().as<TypedArrayObject>());
  if (!TypedArrayObject::ensureHasBuffer(cx, tarray)) {
    return false;
  }

  args.rval().set(tarray->bufferValue());
  return true;
}

static bool intrinsic_TypedArrayByteOffset(JSContext* cx, unsigned argc,
                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(TypedArrayObject::is(args[0]));

  auto* tarr = &args[0].toObject().as<TypedArrayObject>();
  args.rval().set(tarr->byteOffsetValue());
  return true;
}

static bool intrinsic_TypedArrayElementSize(JSContext* cx, unsigned argc,
                                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(TypedArrayObject::is(args[0]));

  unsigned size =
      TypedArrayElemSize(args[0].toObject().as<TypedArrayObject>().type());
  MOZ_ASSERT(size == 1 || size == 2 || size == 4 || size == 8);

  args.rval().setInt32(mozilla::AssertedCast<int32_t>(size));
  return true;
}

// Return the value of [[ArrayLength]] internal slot of the TypedArray
static bool intrinsic_TypedArrayLength(JSContext* cx, unsigned argc,
                                       Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(TypedArrayObject::is(args[0]));

  auto* tarr = &args[0].toObject().as<TypedArrayObject>();
  args.rval().set(tarr->lengthValue());
  return true;
}

static bool intrinsic_PossiblyWrappedTypedArrayLength(JSContext* cx,
                                                      unsigned argc,
                                                      Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isObject());

  TypedArrayObject* obj = args[0].toObject().maybeUnwrapAs<TypedArrayObject>();
  if (!obj) {
    ReportAccessDenied(cx);
    return false;
  }

  args.rval().set(obj->lengthValue());
  return true;
}

static bool intrinsic_PossiblyWrappedTypedArrayHasDetachedBuffer(JSContext* cx,
                                                                 unsigned argc,
                                                                 Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isObject());

  TypedArrayObject* obj = args[0].toObject().maybeUnwrapAs<TypedArrayObject>();
  if (!obj) {
    ReportAccessDenied(cx);
    return false;
  }

  bool detached = obj->hasDetachedBuffer();
  args.rval().setBoolean(detached);
  return true;
}

// Extract the TypedArrayObject* underlying |obj| and return it.  This method,
// in a TOTALLY UNSAFE manner, completely violates the normal compartment
// boundaries, returning an object not necessarily in the current compartment
// or in |obj|'s compartment.
//
// All callers of this method are expected to sigil this TypedArrayObject*, and
// all values and information derived from it, with an "unsafe" prefix, to
// indicate the extreme caution required when dealing with such values.
//
// If calling code discipline ever fails to be maintained, it's gonna have a
// bad time.
static TypedArrayObject* DangerouslyUnwrapTypedArray(JSContext* cx,
                                                     JSObject* obj) {
  // An unwrapped pointer to an object potentially on the other side of a
  // compartment boundary!  Isn't this such fun?
  TypedArrayObject* unwrapped = obj->maybeUnwrapAs<TypedArrayObject>();
  if (!unwrapped) {
    ReportAccessDenied(cx);
    return nullptr;
  }

  // Be super-duper careful using this, as we've just punched through
  // the compartment boundary, and things like buffer() on this aren't
  // same-compartment with anything else in the calling method.
  return unwrapped;
}

// The specification requires us to perform bitwise copying when |sourceType|
// and |targetType| are the same (ES2017, §22.2.3.24, step 15). Additionally,
// as an optimization, we can also perform bitwise copying when |sourceType|
// and |targetType| have compatible bit-level representations.
static bool IsTypedArrayBitwiseSlice(Scalar::Type sourceType,
                                     Scalar::Type targetType) {
  switch (sourceType) {
    case Scalar::Int8:
      return targetType == Scalar::Int8 || targetType == Scalar::Uint8;

    case Scalar::Uint8:
    case Scalar::Uint8Clamped:
      return targetType == Scalar::Int8 || targetType == Scalar::Uint8 ||
             targetType == Scalar::Uint8Clamped;

    case Scalar::Int16:
    case Scalar::Uint16:
      return targetType == Scalar::Int16 || targetType == Scalar::Uint16;

    case Scalar::Int32:
    case Scalar::Uint32:
      return targetType == Scalar::Int32 || targetType == Scalar::Uint32;

    case Scalar::Float32:
      return targetType == Scalar::Float32;

    case Scalar::Float64:
      return targetType == Scalar::Float64;

    case Scalar::BigInt64:
    case Scalar::BigUint64:
      return targetType == Scalar::BigInt64 || targetType == Scalar::BigUint64;

    default:
      MOZ_CRASH("IsTypedArrayBitwiseSlice with a bogus typed array type");
  }
}

static bool intrinsic_TypedArrayBitwiseSlice(JSContext* cx, unsigned argc,
                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 4);
  MOZ_ASSERT(args[0].isObject());
  MOZ_ASSERT(args[1].isObject());
  AssertNonNegativeInteger(args[2]);
  AssertNonNegativeInteger(args[3]);

  Rooted<TypedArrayObject*> source(cx,
                                   &args[0].toObject().as<TypedArrayObject>());
  MOZ_ASSERT(!source->hasDetachedBuffer());

  // As directed by |DangerouslyUnwrapTypedArray|, sigil this pointer and all
  // variables derived from it to counsel extreme caution here.
  Rooted<TypedArrayObject*> unsafeTypedArrayCrossCompartment(cx);
  unsafeTypedArrayCrossCompartment =
      DangerouslyUnwrapTypedArray(cx, &args[1].toObject());
  if (!unsafeTypedArrayCrossCompartment) {
    return false;
  }
  MOZ_ASSERT(!unsafeTypedArrayCrossCompartment->hasDetachedBuffer());

  Scalar::Type sourceType = source->type();
  if (!IsTypedArrayBitwiseSlice(sourceType,
                                unsafeTypedArrayCrossCompartment->type())) {
    args.rval().setBoolean(false);
    return true;
  }

  size_t sourceOffset = size_t(args[2].toNumber());
  size_t count = size_t(args[3].toNumber());

  MOZ_ASSERT(count > 0 && count <= source->length());
  MOZ_ASSERT(sourceOffset <= source->length() - count);
  MOZ_ASSERT(count <= unsafeTypedArrayCrossCompartment->length());

  size_t elementSize = TypedArrayElemSize(sourceType);
  MOZ_ASSERT(elementSize ==
             TypedArrayElemSize(unsafeTypedArrayCrossCompartment->type()));

  SharedMem<uint8_t*> sourceData =
      source->dataPointerEither().cast<uint8_t*>() + sourceOffset * elementSize;

  SharedMem<uint8_t*> unsafeTargetDataCrossCompartment =
      unsafeTypedArrayCrossCompartment->dataPointerEither().cast<uint8_t*>();

  size_t byteLength = count * elementSize;

  // The same-type case requires exact copying preserving the bit-level
  // encoding of the source data, so use memcpy if possible. If source and
  // target are the same buffer, we can't use memcpy (or memmove), because
  // the specification requires sequential copying of the values. This case
  // is only possible if a @@species constructor created a specifically
  // crafted typed array. It won't happen in normal code and hence doesn't
  // need to be optimized.
  if (!TypedArrayObject::sameBuffer(source, unsafeTypedArrayCrossCompartment)) {
    if (source->isSharedMemory() ||
        unsafeTypedArrayCrossCompartment->isSharedMemory()) {
      jit::AtomicOperations::memcpySafeWhenRacy(
          unsafeTargetDataCrossCompartment, sourceData, byteLength);
    } else {
      memcpy(unsafeTargetDataCrossCompartment.unwrapUnshared(),
             sourceData.unwrapUnshared(), byteLength);
    }
  } else {
    using namespace jit;

    for (; byteLength > 0; byteLength--) {
      AtomicOperations::storeSafeWhenRacy(
          unsafeTargetDataCrossCompartment++,
          AtomicOperations::loadSafeWhenRacy(sourceData++));
    }
  }

  args.rval().setBoolean(true);
  return true;
}

static bool intrinsic_TypedArrayInitFromPackedArray(JSContext* cx,
                                                    unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  MOZ_ASSERT(args[0].isObject());
  MOZ_ASSERT(args[1].isObject());

  Rooted<TypedArrayObject*> target(cx,
                                   &args[0].toObject().as<TypedArrayObject>());
  MOZ_ASSERT(!target->hasDetachedBuffer());
  MOZ_ASSERT(!target->isSharedMemory());

  RootedArrayObject source(cx, &args[1].toObject().as<ArrayObject>());
  MOZ_ASSERT(IsPackedArray(source));
  MOZ_ASSERT(source->length() == target->length());

  switch (target->type()) {
#define INIT_TYPED_ARRAY(T, N)                                         \
  case Scalar::N: {                                                    \
    if (!ElementSpecific<T, UnsharedOps>::initFromIterablePackedArray( \
            cx, target, source)) {                                     \
      return false;                                                    \
    }                                                                  \
    break;                                                             \
  }
    JS_FOR_EACH_TYPED_ARRAY(INIT_TYPED_ARRAY)
#undef INIT_TYPED_ARRAY

    default:
      MOZ_CRASH(
          "TypedArrayInitFromPackedArray with a typed array with bogus type");
  }

  args.rval().setUndefined();
  return true;
}

static bool intrinsic_RegExpCreate(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  MOZ_ASSERT(args.length() == 1 || args.length() == 2);
  MOZ_ASSERT_IF(args.length() == 2,
                args[1].isString() || args[1].isUndefined());
  MOZ_ASSERT(!args.isConstructing());

  return RegExpCreate(cx, args[0], args.get(1), args.rval());
}

static bool intrinsic_RegExpGetSubstitution(JSContext* cx, unsigned argc,
                                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 6);

  RootedArrayObject matchResult(cx, &args[0].toObject().as<ArrayObject>());

  RootedLinearString string(cx, args[1].toString()->ensureLinear(cx));
  if (!string) {
    return false;
  }

  int32_t position = int32_t(args[2].toNumber());
  MOZ_ASSERT(position >= 0);

  RootedLinearString replacement(cx, args[3].toString()->ensureLinear(cx));
  if (!replacement) {
    return false;
  }

  int32_t firstDollarIndex = int32_t(args[4].toNumber());
  MOZ_ASSERT(firstDollarIndex >= 0);

  RootedValue namedCaptures(cx, args[5]);
  MOZ_ASSERT(namedCaptures.isUndefined() || namedCaptures.isObject());

  return RegExpGetSubstitution(cx, matchResult, string, size_t(position),
                               replacement, size_t(firstDollarIndex),
                               namedCaptures, args.rval());
}

static bool intrinsic_StringReplaceString(JSContext* cx, unsigned argc,
                                          Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);

  RootedString string(cx, args[0].toString());
  RootedString pattern(cx, args[1].toString());
  RootedString replacement(cx, args[2].toString());
  JSString* result = str_replace_string_raw(cx, string, pattern, replacement);
  if (!result) {
    return false;
  }

  args.rval().setString(result);
  return true;
}

static bool intrinsic_StringReplaceAllString(JSContext* cx, unsigned argc,
                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);

  RootedString string(cx, args[0].toString());
  RootedString pattern(cx, args[1].toString());
  RootedString replacement(cx, args[2].toString());
  JSString* result =
      str_replaceAll_string_raw(cx, string, pattern, replacement);
  if (!result) {
    return false;
  }

  args.rval().setString(result);
  return true;
}

static bool intrinsic_StringSplitString(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);

  RootedString string(cx, args[0].toString());
  RootedString sep(cx, args[1].toString());

  JSObject* aobj = StringSplitString(cx, string, sep, INT32_MAX);
  if (!aobj) {
    return false;
  }

  args.rval().setObject(*aobj);
  return true;
}

static bool intrinsic_StringSplitStringLimit(JSContext* cx, unsigned argc,
                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);

  RootedString string(cx, args[0].toString());
  RootedString sep(cx, args[1].toString());

  // args[2] should be already in UInt32 range, but it could be double typed,
  // because of Ion optimization.
  uint32_t limit = uint32_t(args[2].toNumber());
  MOZ_ASSERT(limit > 0,
             "Zero limit case is already handled in self-hosted code.");

  JSObject* aobj = StringSplitString(cx, string, sep, limit);
  if (!aobj) {
    return false;
  }

  args.rval().setObject(*aobj);
  return true;
}

bool CallSelfHostedNonGenericMethod(JSContext* cx, const CallArgs& args) {
  // This function is called when a self-hosted method is invoked on a
  // wrapper object, like a CrossCompartmentWrapper. The last argument is
  // the name of the self-hosted function. The other arguments are the
  // arguments to pass to this function.

  MOZ_ASSERT(args.length() > 0);
  RootedPropertyName name(
      cx, args[args.length() - 1].toString()->asAtom().asPropertyName());

  InvokeArgs args2(cx);
  if (!args2.init(cx, args.length() - 1)) {
    return false;
  }

  for (size_t i = 0; i < args.length() - 1; i++) {
    args2[i].set(args[i]);
  }

  return CallSelfHostedFunction(cx, name, args.thisv(), args2, args.rval());
}

#ifdef DEBUG
bool js::CallSelfHostedFunction(JSContext* cx, const char* name,
                                HandleValue thisv, const AnyInvokeArgs& args,
                                MutableHandleValue rval) {
  JSAtom* funAtom = Atomize(cx, name, strlen(name));
  if (!funAtom) {
    return false;
  }
  RootedPropertyName funName(cx, funAtom->asPropertyName());
  return CallSelfHostedFunction(cx, funName, thisv, args, rval);
}
#endif

bool js::CallSelfHostedFunction(JSContext* cx, HandlePropertyName name,
                                HandleValue thisv, const AnyInvokeArgs& args,
                                MutableHandleValue rval) {
  RootedValue fun(cx);
  if (!GlobalObject::getIntrinsicValue(cx, cx->global(), name, &fun)) {
    return false;
  }
  MOZ_ASSERT(fun.toObject().is<JSFunction>());

  return Call(cx, fun, thisv, args, rval);
}

template <typename T>
bool Is(HandleValue v) {
  return v.isObject() && v.toObject().is<T>();
}

template <IsAcceptableThis Test>
static bool CallNonGenericSelfhostedMethod(JSContext* cx, unsigned argc,
                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<Test, CallSelfHostedNonGenericMethod>(cx, args);
}

bool js::IsCallSelfHostedNonGenericMethod(NativeImpl impl) {
  return impl == CallSelfHostedNonGenericMethod;
}

bool js::ReportIncompatibleSelfHostedMethod(JSContext* cx,
                                            const CallArgs& args) {
  // The contract for this function is the same as
  // CallSelfHostedNonGenericMethod. The normal ReportIncompatible function
  // doesn't work for selfhosted functions, because they always call the
  // different CallXXXMethodIfWrapped methods, which would be reported as the
  // called function instead.

  // Lookup the selfhosted method that was invoked.  But skip over
  // internal self-hosted function frames, because those are never the
  // actual self-hosted callee from external code.  We can't just skip
  // self-hosted things until we find a non-self-hosted one because of cases
  // like array.sort(somethingSelfHosted), where we want to report the error
  // in the somethingSelfHosted, not in the sort() call.

  static const char* const internalNames[] = {
      "IsTypedArrayEnsuringArrayBuffer",
      "UnwrapAndCallRegExpBuiltinExec",
      "RegExpBuiltinExec",
      "RegExpExec",
      "RegExpSearchSlowPath",
      "RegExpReplaceSlowPath",
      "RegExpMatchSlowPath",
  };

  ScriptFrameIter iter(cx);
  MOZ_ASSERT(iter.isFunctionFrame());

  while (!iter.done()) {
    MOZ_ASSERT(iter.callee(cx)->isSelfHostedOrIntrinsic() &&
               !iter.callee(cx)->isBoundFunction());
    UniqueChars funNameBytes;
    const char* funName =
        GetFunctionNameBytes(cx, iter.callee(cx), &funNameBytes);
    if (!funName) {
      return false;
    }
    if (std::all_of(
            std::begin(internalNames), std::end(internalNames),
            [funName](auto* name) { return strcmp(funName, name) != 0; })) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_INCOMPATIBLE_METHOD, funName, "method",
                               InformalValueTypeName(args.thisv()));
      return false;
    }
    ++iter;
  }

  MOZ_ASSERT_UNREACHABLE("How did we not find a useful self-hosted frame?");
  return false;
}

#ifdef JS_HAS_INTL_API
/**
 * Returns the default locale as a well-formed, but not necessarily
 * canonicalized, BCP-47 language tag.
 */
static bool intrinsic_RuntimeDefaultLocale(JSContext* cx, unsigned argc,
                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 0);

  const char* locale = cx->runtime()->getDefaultLocale();
  if (!locale) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEFAULT_LOCALE_ERROR);
    return false;
  }

  JSString* jslocale = NewStringCopyZ<CanGC>(cx, locale);
  if (!jslocale) {
    return false;
  }

  args.rval().setString(jslocale);
  return true;
}

static bool intrinsic_IsRuntimeDefaultLocale(JSContext* cx, unsigned argc,
                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isString() || args[0].isUndefined());

  // |undefined| is the default value when the Intl runtime caches haven't
  // yet been initialized. Handle it the same way as a cache miss.
  if (args[0].isUndefined()) {
    args.rval().setBoolean(false);
    return true;
  }

  const char* locale = cx->runtime()->getDefaultLocale();
  if (!locale) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEFAULT_LOCALE_ERROR);
    return false;
  }

  JSLinearString* str = args[0].toString()->ensureLinear(cx);
  if (!str) {
    return false;
  }

  bool equals = StringEqualsAscii(str, locale);
  args.rval().setBoolean(equals);
  return true;
}
#endif  // JS_HAS_INTL_API

static bool intrinsic_ThrowArgTypeNotObject(JSContext* cx, unsigned argc,
                                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  MOZ_ASSERT(args[0].isNumber());
  MOZ_ASSERT(!args[1].isObject());
  if (args[0].toNumber() == NOT_OBJECT_KIND_DESCRIPTOR) {
    ReportNotObject(cx, JSMSG_OBJECT_REQUIRED_PROP_DESC, args[1]);
  } else {
    MOZ_CRASH("unexpected kind");
  }

  return false;
}

static bool intrinsic_ConstructFunction(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);
  MOZ_ASSERT(IsConstructor(args[0]));
  MOZ_ASSERT(IsConstructor(args[1]));
  MOZ_ASSERT(args[2].toObject().is<ArrayObject>());

  RootedArrayObject argsList(cx, &args[2].toObject().as<ArrayObject>());
  uint32_t len = argsList->length();
  ConstructArgs constructArgs(cx);
  if (!constructArgs.init(cx, len)) {
    return false;
  }
  for (uint32_t index = 0; index < len; index++) {
    constructArgs[index].set(argsList->getDenseElement(index));
  }

  RootedObject res(cx);
  if (!Construct(cx, args[0], constructArgs, args[1], &res)) {
    return false;
  }

  args.rval().setObject(*res);
  return true;
}

static bool intrinsic_IsConstructing(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 0);

  ScriptFrameIter iter(cx);
  bool isConstructing = iter.isConstructing();
  args.rval().setBoolean(isConstructing);
  return true;
}

static bool intrinsic_ConstructorForTypedArray(JSContext* cx, unsigned argc,
                                               Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isObject());

  auto* object = UnwrapAndDowncastValue<TypedArrayObject>(cx, args[0]);
  if (!object) {
    return false;
  }

  JSProtoKey protoKey = StandardProtoKeyOrNull(object);
  MOZ_ASSERT(protoKey);

  // While it may seem like an invariant that in any compartment,
  // seeing a typed array object implies that the TypedArray constructor
  // for that type is initialized on the compartment's global, this is not
  // the case. When we construct a typed array given a cross-compartment
  // ArrayBuffer, we put the constructed TypedArray in the same compartment
  // as the ArrayBuffer. Since we use the prototype from the initial
  // compartment, and never call the constructor in the ArrayBuffer's
  // compartment from script, we are not guaranteed to have initialized
  // the constructor.
  JSObject* ctor = GlobalObject::getOrCreateConstructor(cx, protoKey);
  if (!ctor) {
    return false;
  }

  args.rval().setObject(*ctor);
  return true;
}

static bool intrinsic_HostResolveImportedModule(JSContext* cx, unsigned argc,
                                                Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  RootedModuleObject module(cx, &args[0].toObject().as<ModuleObject>());
  RootedObject moduleRequest(cx, &args[1].toObject());

  RootedValue referencingPrivate(cx, JS::GetModulePrivate(module));
  RootedObject result(
      cx, CallModuleResolveHook(cx, referencingPrivate, moduleRequest));
  if (!result) {
    return false;
  }

  if (!result->is<ModuleObject>()) {
    JS_ReportErrorASCII(cx, "Module resolve hook did not return Module object");
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

static bool intrinsic_CreateImportBinding(JSContext* cx, unsigned argc,
                                          Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 4);
  RootedModuleEnvironmentObject environment(
      cx, &args[0].toObject().as<ModuleEnvironmentObject>());
  RootedAtom importedName(cx, &args[1].toString()->asAtom());
  RootedModuleObject module(cx, &args[2].toObject().as<ModuleObject>());
  RootedAtom localName(cx, &args[3].toString()->asAtom());
  if (!environment->createImportBinding(cx, importedName, module, localName)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

static bool intrinsic_CreateNamespaceBinding(JSContext* cx, unsigned argc,
                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);
  RootedModuleEnvironmentObject environment(
      cx, &args[0].toObject().as<ModuleEnvironmentObject>());
  RootedId name(cx, AtomToId(&args[1].toString()->asAtom()));
  MOZ_ASSERT(args[2].toObject().is<ModuleNamespaceObject>());
  // The property already exists in the evironment but is not writable, so set
  // the slot directly.
  mozilla::Maybe<PropertyInfo> prop = environment->lookup(cx, name);
  MOZ_ASSERT(prop.isSome());
  environment->setSlot(prop->slot(), args[2]);
  args.rval().setUndefined();
  return true;
}

static bool intrinsic_EnsureModuleEnvironmentNamespace(JSContext* cx,
                                                       unsigned argc,
                                                       Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  RootedModuleObject module(cx, &args[0].toObject().as<ModuleObject>());
  MOZ_ASSERT(args[1].toObject().is<ModuleNamespaceObject>());
  RootedModuleEnvironmentObject environment(cx, &module->initialEnvironment());
  // The property already exists in the evironment but is not writable, so set
  // the slot directly.
  mozilla::Maybe<PropertyInfo> prop =
      environment->lookup(cx, cx->names().starNamespaceStar);
  MOZ_ASSERT(prop.isSome());
  environment->setSlot(prop->slot(), args[1]);
  args.rval().setUndefined();
  return true;
}

static bool intrinsic_InstantiateModuleFunctionDeclarations(JSContext* cx,
                                                            unsigned argc,
                                                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  RootedModuleObject module(cx, &args[0].toObject().as<ModuleObject>());
  args.rval().setUndefined();
  return ModuleObject::instantiateFunctionDeclarations(cx, module);
}

static bool intrinsic_ExecuteModule(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  RootedModuleObject module(cx, &args[0].toObject().as<ModuleObject>());
  return ModuleObject::execute(cx, module, args.rval());
}

static bool intrinsic_IsTopLevelAwaitEnabled(JSContext* cx, unsigned argc,
                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 0);
  bool topLevelAwait = cx->options().topLevelAwait();
  args.rval().setBoolean(topLevelAwait);
  return true;
}

static bool intrinsic_SetCycleRoot(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  RootedModuleObject module(cx, &args[0].toObject().as<ModuleObject>());
  RootedModuleObject cycleRoot(cx, &args[1].toObject().as<ModuleObject>());
  module->setCycleRoot(cycleRoot);
  args.rval().setUndefined();
  return true;
}

static bool intrinsic_GetCycleRoot(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  RootedModuleObject module(cx, &args[0].toObject().as<ModuleObject>());
  JSObject* result = module->getCycleRoot();
  if (!result) {
    return false;
  }
  args.rval().setObject(*result);
  return true;
}

static bool intrinsic_AppendAsyncParentModule(JSContext* cx, unsigned argc,
                                              Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  RootedModuleObject self(cx, &args[0].toObject().as<ModuleObject>());
  RootedModuleObject parent(cx, &args[1].toObject().as<ModuleObject>());
  return ModuleObject::appendAsyncParentModule(cx, self, parent);
}

static bool intrinsic_InitAsyncEvaluating(JSContext* cx, unsigned argc,
                                          Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  RootedModuleObject module(cx, &args[0].toObject().as<ModuleObject>());
  if (!module->initAsyncEvaluatingSlot()) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

static bool intrinsic_IsAsyncEvaluating(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  RootedModuleObject module(cx, &args[0].toObject().as<ModuleObject>());
  bool isAsyncEvaluating = module->isAsyncEvaluating();
  args.rval().setBoolean(isAsyncEvaluating);
  return true;
}

static bool intrinsic_CreateTopLevelCapability(JSContext* cx, unsigned argc,
                                               Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  RootedModuleObject self(cx, &args[0].toObject().as<ModuleObject>());
  PromiseObject* result = ModuleObject::createTopLevelCapability(cx, self);
  if (!result) {
    return false;
  }
  args.rval().setObject(*result);
  return true;
}

static bool intrinsic_ModuleTopLevelCapabilityResolve(JSContext* cx,
                                                      unsigned argc,
                                                      Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  RootedModuleObject module(cx, &args[0].toObject().as<ModuleObject>());
  ModuleObject::topLevelCapabilityResolve(cx, module);
  args.rval().setUndefined();
  return true;
}

static bool intrinsic_ModuleTopLevelCapabilityReject(JSContext* cx,
                                                     unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  RootedModuleObject module(cx, &args[0].toObject().as<ModuleObject>());
  HandleValue error = args[1];
  ModuleObject::topLevelCapabilityReject(cx, module, error);
  args.rval().setUndefined();
  return true;
}

static bool intrinsic_NewModuleNamespace(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  RootedModuleObject module(cx, &args[0].toObject().as<ModuleObject>());
  RootedObject exports(cx, &args[1].toObject());
  JSObject* namespace_ = ModuleObject::createNamespace(cx, module, exports);
  if (!namespace_) {
    return false;
  }

  args.rval().setObject(*namespace_);
  return true;
}

static bool intrinsic_AddModuleNamespaceBinding(JSContext* cx, unsigned argc,
                                                Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 4);
  RootedModuleNamespaceObject namespace_(
      cx, &args[0].toObject().as<ModuleNamespaceObject>());
  RootedAtom exportedName(cx, &args[1].toString()->asAtom());
  RootedModuleObject targetModule(cx, &args[2].toObject().as<ModuleObject>());
  RootedAtom targetName(cx, &args[3].toString()->asAtom());
  if (!namespace_->addBinding(cx, exportedName, targetModule, targetName)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

static bool intrinsic_PromiseResolve(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);

  RootedObject constructor(cx, &args[0].toObject());
  JSObject* promise = js::PromiseResolve(cx, constructor, args[1]);
  if (!promise) {
    return false;
  }

  args.rval().setObject(*promise);
  return true;
}

static bool intrinsic_CopyDataPropertiesOrGetOwnKeys(JSContext* cx,
                                                     unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);
  MOZ_ASSERT(args[0].isObject());
  MOZ_ASSERT(args[1].isObject());
  MOZ_ASSERT(args[2].isObjectOrNull());

  RootedObject target(cx, &args[0].toObject());
  RootedObject from(cx, &args[1].toObject());
  RootedObject excludedItems(cx, args[2].toObjectOrNull());

  if (from->is<NativeObject>() && target->is<PlainObject>() &&
      (!excludedItems || excludedItems->is<PlainObject>())) {
    bool optimized;
    if (!CopyDataPropertiesNative(
            cx, target.as<PlainObject>(), from.as<NativeObject>(),
            (excludedItems ? excludedItems.as<PlainObject>() : nullptr),
            &optimized)) {
      return false;
    }

    if (optimized) {
      args.rval().setNull();
      return true;
    }
  }

  return GetOwnPropertyKeys(
      cx, from, JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS, args.rval());
}

static bool intrinsic_ToBigInt(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  BigInt* res = ToBigInt(cx, args[0]);
  if (!res) {
    return false;
  }
  args.rval().setBigInt(res);
  return true;
}

static bool intrinsic_NewWrapForValidIterator(JSContext* cx, unsigned argc,
                                              Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 0);

  JSObject* obj = NewWrapForValidIterator(cx);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

static bool intrinsic_NewPrivateName(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);

  RootedString desc(cx, args[0].toString());
  auto* symbol = JS::Symbol::new_(cx, JS::SymbolCode::PrivateNameSymbol, desc);
  if (!symbol) {
    return false;
  }
  args.rval().setSymbol(symbol);
  return true;
}

static bool intrinsic_NewIteratorHelper(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 0);

  JSObject* obj = NewIteratorHelper(cx);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

static bool intrinsic_NewAsyncIteratorHelper(JSContext* cx, unsigned argc,
                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 0);

  JSObject* obj = NewAsyncIteratorHelper(cx);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

static bool intrinsic_NoPrivateGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 0);

  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_PRIVATE_SETTER_ONLY);

  args.rval().setUndefined();
  return false;
}

static const JSFunctionSpec intrinsic_functions[] = {
    // Intrinsic helper functions
    JS_FN("AddModuleNamespaceBinding", intrinsic_AddModuleNamespaceBinding, 4,
          0),
    JS_FN("AppendAsyncParentModule", intrinsic_AppendAsyncParentModule, 2, 0),
    JS_INLINABLE_FN("ArrayBufferByteLength",
                    intrinsic_ArrayBufferByteLength<ArrayBufferObject>, 1, 0,
                    IntrinsicArrayBufferByteLength),
    JS_FN("ArrayBufferCopyData",
          intrinsic_ArrayBufferCopyData<ArrayBufferObject>, 6, 0),
    JS_INLINABLE_FN("ArrayIteratorPrototypeOptimizable",
                    intrinsic_ArrayIteratorPrototypeOptimizable, 0, 0,
                    IntrinsicArrayIteratorPrototypeOptimizable),
    JS_FN("ArrayNativeSort", intrinsic_ArrayNativeSort, 1, 0),
    JS_FN("AssertionFailed", intrinsic_AssertionFailed, 1, 0),
    JS_FN("CallArrayBufferMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<ArrayBufferObject>>, 2, 0),
    JS_FN("CallArrayIteratorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<ArrayIteratorObject>>, 2, 0),
    JS_FN("CallAsyncIteratorHelperMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<AsyncIteratorHelperObject>>, 2, 0),
    JS_FN("CallGeneratorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<GeneratorObject>>, 2, 0),
    JS_FN("CallIteratorHelperMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<IteratorHelperObject>>, 2, 0),
    JS_FN("CallMapIteratorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<MapIteratorObject>>, 2, 0),
    JS_FN("CallMapMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<MapObject>>, 2, 0),
    JS_FN("CallModuleMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<ModuleObject>>, 2, 0),
    JS_FN("CallRegExpMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<RegExpObject>>, 2, 0),
    JS_FN("CallRegExpStringIteratorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<RegExpStringIteratorObject>>, 2, 0),
    JS_FN("CallSetIteratorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<SetIteratorObject>>, 2, 0),
    JS_FN("CallSetMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<SetObject>>, 2, 0),
    JS_FN("CallSharedArrayBufferMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<SharedArrayBufferObject>>, 2, 0),
    JS_FN("CallStringIteratorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<StringIteratorObject>>, 2, 0),
    JS_FN("CallTypedArrayMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<TypedArrayObject>>, 2, 0),
    JS_FN("CallWrapForValidIteratorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<WrapForValidIteratorObject>>, 2, 0),
    JS_FN("ConstructFunction", intrinsic_ConstructFunction, 2, 0),
    JS_FN("ConstructorForTypedArray", intrinsic_ConstructorForTypedArray, 1, 0),
    JS_FN("CopyDataPropertiesOrGetOwnKeys",
          intrinsic_CopyDataPropertiesOrGetOwnKeys, 3, 0),
    JS_FN("CreateImportBinding", intrinsic_CreateImportBinding, 4, 0),
    JS_FN("CreateMapIterationResultPair",
          intrinsic_CreateMapIterationResultPair, 0, 0),
    JS_FN("CreateModuleSyntaxError", intrinsic_CreateModuleSyntaxError, 4, 0),
    JS_FN("CreateNamespaceBinding", intrinsic_CreateNamespaceBinding, 3, 0),
    JS_FN("CreateSetIterationResult", intrinsic_CreateSetIterationResult, 0, 0),
    JS_FN("CreateTopLevelCapability", intrinsic_CreateTopLevelCapability, 1, 0),
    JS_FN("DecompileArg", intrinsic_DecompileArg, 2, 0),
    JS_FN("DefineDataProperty", intrinsic_DefineDataProperty, 4, 0),
    JS_FN("DefineProperty", intrinsic_DefineProperty, 6, 0),
    JS_FN("DumpMessage", intrinsic_DumpMessage, 1, 0),
    JS_FN("EnsureModuleEnvironmentNamespace",
          intrinsic_EnsureModuleEnvironmentNamespace, 1, 0),
    JS_FN("ExecuteModule", intrinsic_ExecuteModule, 1, 0),
    JS_INLINABLE_FN("FinishBoundFunctionInit",
                    intrinsic_FinishBoundFunctionInit, 3, 0,
                    IntrinsicFinishBoundFunctionInit),
    JS_FN("FlatStringMatch", FlatStringMatch, 2, 0),
    JS_FN("FlatStringSearch", FlatStringSearch, 2, 0),
    JS_FN("GeneratorIsRunning", intrinsic_GeneratorIsRunning, 1, 0),
    JS_FN("GeneratorObjectIsClosed", intrinsic_GeneratorObjectIsClosed, 1, 0),
    JS_FN("GeneratorSetClosed", intrinsic_GeneratorSetClosed, 1, 0),
    JS_FN("GetCycleRoot", intrinsic_GetCycleRoot, 1, 0),
    JS_FN("GetElemBaseForLambda", intrinsic_GetElemBaseForLambda, 1, 0),
    JS_FN("GetErrorMessage", intrinsic_GetErrorMessage, 1, 0),
    JS_INLINABLE_FN("GetFirstDollarIndex", GetFirstDollarIndex, 1, 0,
                    GetFirstDollarIndex),
    JS_INLINABLE_FN("GetNextMapEntryForIterator",
                    intrinsic_GetNextMapEntryForIterator, 2, 0,
                    IntrinsicGetNextMapEntryForIterator),
    JS_INLINABLE_FN("GetNextSetEntryForIterator",
                    intrinsic_GetNextSetEntryForIterator, 2, 0,
                    IntrinsicGetNextSetEntryForIterator),
    JS_FN("GetOwnPropertyDescriptorToArray", GetOwnPropertyDescriptorToArray, 2,
          0),
    JS_FN("GetStringDataProperty", intrinsic_GetStringDataProperty, 2, 0),
    JS_FN("GetTypedArrayKind", intrinsic_GetTypedArrayKind, 1, 0),
    JS_INLINABLE_FN("GuardToArrayBuffer",
                    intrinsic_GuardToBuiltin<ArrayBufferObject>, 1, 0,
                    IntrinsicGuardToArrayBuffer),
    JS_INLINABLE_FN("GuardToArrayIterator",
                    intrinsic_GuardToBuiltin<ArrayIteratorObject>, 1, 0,
                    IntrinsicGuardToArrayIterator),
    JS_INLINABLE_FN("GuardToAsyncIteratorHelper",
                    intrinsic_GuardToBuiltin<AsyncIteratorHelperObject>, 1, 0,
                    IntrinsicGuardToAsyncIteratorHelper),
    JS_INLINABLE_FN("GuardToIteratorHelper",
                    intrinsic_GuardToBuiltin<IteratorHelperObject>, 1, 0,
                    IntrinsicGuardToIteratorHelper),
    JS_INLINABLE_FN("GuardToMapIterator",
                    intrinsic_GuardToBuiltin<MapIteratorObject>, 1, 0,
                    IntrinsicGuardToMapIterator),
    JS_INLINABLE_FN("GuardToMapObject", intrinsic_GuardToBuiltin<MapObject>, 1,
                    0, IntrinsicGuardToMapObject),
    JS_INLINABLE_FN("GuardToRegExpStringIterator",
                    intrinsic_GuardToBuiltin<RegExpStringIteratorObject>, 1, 0,
                    IntrinsicGuardToRegExpStringIterator),
    JS_INLINABLE_FN("GuardToSetIterator",
                    intrinsic_GuardToBuiltin<SetIteratorObject>, 1, 0,
                    IntrinsicGuardToSetIterator),
    JS_INLINABLE_FN("GuardToSetObject", intrinsic_GuardToBuiltin<SetObject>, 1,
                    0, IntrinsicGuardToSetObject),
    JS_INLINABLE_FN("GuardToSharedArrayBuffer",
                    intrinsic_GuardToBuiltin<SharedArrayBufferObject>, 1, 0,
                    IntrinsicGuardToSharedArrayBuffer),
    JS_INLINABLE_FN("GuardToStringIterator",
                    intrinsic_GuardToBuiltin<StringIteratorObject>, 1, 0,
                    IntrinsicGuardToStringIterator),
    JS_INLINABLE_FN("GuardToWrapForValidIterator",
                    intrinsic_GuardToBuiltin<WrapForValidIteratorObject>, 1, 0,
                    IntrinsicGuardToWrapForValidIterator),
    JS_FN("HostResolveImportedModule", intrinsic_HostResolveImportedModule, 2,
          0),
    JS_FN("InitAsyncEvaluating", intrinsic_InitAsyncEvaluating, 1, 0),
    JS_FN("InstantiateModuleFunctionDeclarations",
          intrinsic_InstantiateModuleFunctionDeclarations, 1, 0),
    JS_FN("IntrinsicAsyncGeneratorNext", AsyncGeneratorNext, 1, 0),
    JS_FN("IntrinsicAsyncGeneratorReturn", AsyncGeneratorReturn, 1, 0),
    JS_FN("IntrinsicAsyncGeneratorThrow", AsyncGeneratorThrow, 1, 0),
    JS_INLINABLE_FN("IsArray", intrinsic_IsArray, 1, 0, ArrayIsArray),
    JS_FN("IsAsyncEvaluating", intrinsic_IsAsyncEvaluating, 1, 0),
    JS_FN("IsAsyncFunctionGeneratorObject",
          intrinsic_IsInstanceOfBuiltin<AsyncFunctionGeneratorObject>, 1, 0),
    JS_FN("IsAsyncGeneratorObject",
          intrinsic_IsInstanceOfBuiltin<AsyncGeneratorObject>, 1, 0),
    JS_INLINABLE_FN("IsCallable", intrinsic_IsCallable, 1, 0,
                    IntrinsicIsCallable),
    JS_INLINABLE_FN("IsConstructing", intrinsic_IsConstructing, 0, 0,
                    IntrinsicIsConstructing),
    JS_INLINABLE_FN("IsConstructor", intrinsic_IsConstructor, 1, 0,
                    IntrinsicIsConstructor),
    JS_INLINABLE_FN("IsCrossRealmArrayConstructor",
                    intrinsic_IsCrossRealmArrayConstructor, 1, 0,
                    IntrinsicIsCrossRealmArrayConstructor),
    JS_FN("IsGeneratorObject", intrinsic_IsInstanceOfBuiltin<GeneratorObject>,
          1, 0),
    JS_FN("IsModule", intrinsic_IsInstanceOfBuiltin<ModuleObject>, 1, 0),
    JS_FN("IsModuleEnvironment",
          intrinsic_IsInstanceOfBuiltin<ModuleEnvironmentObject>, 1, 0),
    JS_INLINABLE_FN("IsObject", intrinsic_IsObject, 1, 0, IntrinsicIsObject),
    JS_INLINABLE_FN("IsPackedArray", intrinsic_IsPackedArray, 1, 0,
                    IntrinsicIsPackedArray),
    JS_INLINABLE_FN("IsPossiblyWrappedRegExpObject",
                    intrinsic_IsPossiblyWrappedInstanceOfBuiltin<RegExpObject>,
                    1, 0, IsPossiblyWrappedRegExpObject),
    JS_INLINABLE_FN(
        "IsPossiblyWrappedTypedArray",
        intrinsic_IsPossiblyWrappedInstanceOfBuiltin<TypedArrayObject>, 1, 0,
        IntrinsicIsPossiblyWrappedTypedArray),
    JS_INLINABLE_FN("IsRegExpObject",
                    intrinsic_IsInstanceOfBuiltin<RegExpObject>, 1, 0,
                    IsRegExpObject),
    JS_INLINABLE_FN("IsSuspendedGenerator", intrinsic_IsSuspendedGenerator, 1,
                    0, IntrinsicIsSuspendedGenerator),
    JS_FN("IsTopLevelAwaitEnabled", intrinsic_IsTopLevelAwaitEnabled, 0, 0),
    JS_INLINABLE_FN("IsTypedArray",
                    intrinsic_IsInstanceOfBuiltin<TypedArrayObject>, 1, 0,
                    IntrinsicIsTypedArray),
    JS_INLINABLE_FN("IsTypedArrayConstructor",
                    intrinsic_IsTypedArrayConstructor, 1, 0,
                    IntrinsicIsTypedArrayConstructor),
    JS_FN("IsWrappedArrayBuffer",
          intrinsic_IsWrappedInstanceOfBuiltin<ArrayBufferObject>, 1, 0),
    JS_FN("IsWrappedSharedArrayBuffer",
          intrinsic_IsWrappedInstanceOfBuiltin<SharedArrayBufferObject>, 1, 0),
    JS_FN("ModuleTopLevelCapabilityReject",
          intrinsic_ModuleTopLevelCapabilityReject, 2, 0),
    JS_FN("ModuleTopLevelCapabilityResolve",
          intrinsic_ModuleTopLevelCapabilityResolve, 1, 0),
    JS_INLINABLE_FN("NewArrayIterator", intrinsic_NewArrayIterator, 0, 0,
                    IntrinsicNewArrayIterator),
    JS_FN("NewAsyncIteratorHelper", intrinsic_NewAsyncIteratorHelper, 0, 0),
    JS_FN("NewIteratorHelper", intrinsic_NewIteratorHelper, 0, 0),
    JS_FN("NewModuleNamespace", intrinsic_NewModuleNamespace, 2, 0),
    JS_FN("NewPrivateName", intrinsic_NewPrivateName, 1, 0),
    JS_INLINABLE_FN("NewRegExpStringIterator",
                    intrinsic_NewRegExpStringIterator, 0, 0,
                    IntrinsicNewRegExpStringIterator),
    JS_INLINABLE_FN("NewStringIterator", intrinsic_NewStringIterator, 0, 0,
                    IntrinsicNewStringIterator),
    JS_FN("NewWrapForValidIterator", intrinsic_NewWrapForValidIterator, 0, 0),
    JS_FN("NoPrivateGetter", intrinsic_NoPrivateGetter, 1, 0),
    JS_INLINABLE_FN("ObjectHasPrototype", intrinsic_ObjectHasPrototype, 2, 0,
                    IntrinsicObjectHasPrototype),
    JS_INLINABLE_FN(
        "PossiblyWrappedArrayBufferByteLength",
        intrinsic_PossiblyWrappedArrayBufferByteLength<ArrayBufferObject>, 1, 0,
        IntrinsicPossiblyWrappedArrayBufferByteLength),
    JS_FN(
        "PossiblyWrappedSharedArrayBufferByteLength",
        intrinsic_PossiblyWrappedArrayBufferByteLength<SharedArrayBufferObject>,
        1, 0),
    JS_FN("PossiblyWrappedTypedArrayHasDetachedBuffer",
          intrinsic_PossiblyWrappedTypedArrayHasDetachedBuffer, 1, 0),
    JS_INLINABLE_FN("PossiblyWrappedTypedArrayLength",
                    intrinsic_PossiblyWrappedTypedArrayLength, 1, 0,
                    IntrinsicPossiblyWrappedTypedArrayLength),
    JS_FN("PromiseResolve", intrinsic_PromiseResolve, 2, 0),
    JS_FN("RegExpConstructRaw", regexp_construct_raw_flags, 2, 0),
    JS_FN("RegExpCreate", intrinsic_RegExpCreate, 2, 0),
    JS_FN("RegExpGetSubstitution", intrinsic_RegExpGetSubstitution, 5, 0),
    JS_INLINABLE_FN("RegExpInstanceOptimizable", RegExpInstanceOptimizable, 1,
                    0, RegExpInstanceOptimizable),
    JS_INLINABLE_FN("RegExpMatcher", RegExpMatcher, 3, 0, RegExpMatcher),
    JS_INLINABLE_FN("RegExpPrototypeOptimizable", RegExpPrototypeOptimizable, 1,
                    0, RegExpPrototypeOptimizable),
    JS_INLINABLE_FN("RegExpSearcher", RegExpSearcher, 3, 0, RegExpSearcher),
    JS_INLINABLE_FN("RegExpTester", RegExpTester, 3, 0, RegExpTester),
    JS_INLINABLE_FN("SameValue", js::obj_is, 2, 0, ObjectIs),
    JS_FN("SetCycleRoot", intrinsic_SetCycleRoot, 2, 0),
    JS_FN("SharedArrayBufferByteLength",
          intrinsic_ArrayBufferByteLength<SharedArrayBufferObject>, 1, 0),
    JS_FN("SharedArrayBufferCopyData",
          intrinsic_ArrayBufferCopyData<SharedArrayBufferObject>, 6, 0),
    JS_FN("SharedArrayBuffersMemorySame",
          intrinsic_SharedArrayBuffersMemorySame, 2, 0),
    JS_FN("StringReplaceAllString", intrinsic_StringReplaceAllString, 3, 0),
    JS_INLINABLE_FN("StringReplaceString", intrinsic_StringReplaceString, 3, 0,
                    IntrinsicStringReplaceString),
    JS_INLINABLE_FN("StringSplitString", intrinsic_StringSplitString, 2, 0,
                    IntrinsicStringSplitString),
    JS_FN("StringSplitStringLimit", intrinsic_StringSplitStringLimit, 3, 0),
    JS_INLINABLE_FN("SubstringKernel", intrinsic_SubstringKernel, 3, 0,
                    IntrinsicSubstringKernel),
    JS_FN("ThisNumberValueForToLocaleString", ThisNumberValueForToLocaleString,
          0, 0),
    JS_FN("ThisTimeValue", intrinsic_ThisTimeValue, 1, 0),
    JS_FN("ThrowAggregateError", intrinsic_ThrowAggregateError, 4, 0),
    JS_FN("ThrowArgTypeNotObject", intrinsic_ThrowArgTypeNotObject, 2, 0),
    JS_FN("ThrowInternalError", intrinsic_ThrowInternalError, 4, 0),
    JS_FN("ThrowRangeError", intrinsic_ThrowRangeError, 4, 0),
    JS_FN("ThrowSyntaxError", intrinsic_ThrowSyntaxError, 4, 0),
    JS_FN("ThrowTypeError", intrinsic_ThrowTypeError, 4, 0),
    JS_FN("ToBigInt", intrinsic_ToBigInt, 1, 0),
    JS_INLINABLE_FN("ToInteger", intrinsic_ToInteger, 1, 0, IntrinsicToInteger),
    JS_INLINABLE_FN("ToLength", intrinsic_ToLength, 1, 0, IntrinsicToLength),
    JS_INLINABLE_FN("ToObject", intrinsic_ToObject, 1, 0, IntrinsicToObject),
    JS_FN("ToPropertyKey", intrinsic_ToPropertyKey, 1, 0),
    JS_FN("ToSource", intrinsic_ToSource, 1, 0),
    JS_FN("TypedArrayBitwiseSlice", intrinsic_TypedArrayBitwiseSlice, 4, 0),
    JS_FN("TypedArrayBuffer", intrinsic_TypedArrayBuffer, 1, 0),
    JS_INLINABLE_FN("TypedArrayByteOffset", intrinsic_TypedArrayByteOffset, 1,
                    0, IntrinsicTypedArrayByteOffset),
    JS_INLINABLE_FN("TypedArrayElementSize", intrinsic_TypedArrayElementSize, 1,
                    0, IntrinsicTypedArrayElementSize),
    JS_FN("TypedArrayInitFromPackedArray",
          intrinsic_TypedArrayInitFromPackedArray, 2, 0),
    JS_INLINABLE_FN("TypedArrayLength", intrinsic_TypedArrayLength, 1, 0,
                    IntrinsicTypedArrayLength),
    JS_INLINABLE_FN("UnsafeGetBooleanFromReservedSlot",
                    intrinsic_UnsafeGetBooleanFromReservedSlot, 2, 0,
                    IntrinsicUnsafeGetBooleanFromReservedSlot),
    JS_INLINABLE_FN("UnsafeGetInt32FromReservedSlot",
                    intrinsic_UnsafeGetInt32FromReservedSlot, 2, 0,
                    IntrinsicUnsafeGetInt32FromReservedSlot),
    JS_INLINABLE_FN("UnsafeGetObjectFromReservedSlot",
                    intrinsic_UnsafeGetObjectFromReservedSlot, 2, 0,
                    IntrinsicUnsafeGetObjectFromReservedSlot),
    JS_INLINABLE_FN("UnsafeGetReservedSlot", intrinsic_UnsafeGetReservedSlot, 2,
                    0, IntrinsicUnsafeGetReservedSlot),
    JS_INLINABLE_FN("UnsafeGetStringFromReservedSlot",
                    intrinsic_UnsafeGetStringFromReservedSlot, 2, 0,
                    IntrinsicUnsafeGetStringFromReservedSlot),
    JS_INLINABLE_FN("UnsafeSetReservedSlot", intrinsic_UnsafeSetReservedSlot, 3,
                    0, IntrinsicUnsafeSetReservedSlot),

    // Intrinsics and standard functions used by Intl API implementation.
#ifdef JS_HAS_INTL_API
    JS_FN("intl_BestAvailableLocale", intl_BestAvailableLocale, 3, 0),
    JS_FN("intl_CallCollatorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<CollatorObject>>, 2, 0),
    JS_FN("intl_CallDateTimeFormatMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<DateTimeFormatObject>>, 2, 0),
    JS_FN("intl_CallDisplayNamesMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<DisplayNamesObject>>, 2, 0),
    JS_FN("intl_CallListFormatMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<ListFormatObject>>, 2, 0),
    JS_FN("intl_CallNumberFormatMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<NumberFormatObject>>, 2, 0),
    JS_FN("intl_CallPluralRulesMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<PluralRulesObject>>, 2, 0),
    JS_FN("intl_CallRelativeTimeFormatMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<RelativeTimeFormatObject>>, 2, 0),
    JS_FN("intl_Collator", intl_Collator, 2, 0),
    JS_FN("intl_CompareStrings", intl_CompareStrings, 3, 0),
    JS_FN("intl_ComputeDisplayName", intl_ComputeDisplayName, 6, 0),
    JS_FN("intl_ComputeDisplayNames", intl_ComputeDisplayNames, 3, 0),
    JS_FN("intl_DateTimeFormat", intl_DateTimeFormat, 2, 0),
    JS_FN("intl_FormatDateTime", intl_FormatDateTime, 2, 0),
    JS_FN("intl_FormatDateTimeRange", intl_FormatDateTimeRange, 4, 0),
    JS_FN("intl_FormatList", intl_FormatList, 3, 0),
    JS_FN("intl_FormatNumber", intl_FormatNumber, 3, 0),
    JS_FN("intl_FormatRelativeTime", intl_FormatRelativeTime, 4, 0),
    JS_FN("intl_GetCalendarInfo", intl_GetCalendarInfo, 1, 0),
    JS_FN("intl_GetLocaleInfo", intl_GetLocaleInfo, 1, 0),
    JS_FN("intl_GetPluralCategories", intl_GetPluralCategories, 1, 0),
    JS_INLINABLE_FN("intl_GuardToCollator",
                    intrinsic_GuardToBuiltin<CollatorObject>, 1, 0,
                    IntlGuardToCollator),
    JS_INLINABLE_FN("intl_GuardToDateTimeFormat",
                    intrinsic_GuardToBuiltin<DateTimeFormatObject>, 1, 0,
                    IntlGuardToDateTimeFormat),
    JS_INLINABLE_FN("intl_GuardToDisplayNames",
                    intrinsic_GuardToBuiltin<DisplayNamesObject>, 1, 0,
                    IntlGuardToDisplayNames),
    JS_INLINABLE_FN("intl_GuardToListFormat",
                    intrinsic_GuardToBuiltin<ListFormatObject>, 1, 0,
                    IntlGuardToListFormat),
    JS_INLINABLE_FN("intl_GuardToNumberFormat",
                    intrinsic_GuardToBuiltin<NumberFormatObject>, 1, 0,
                    IntlGuardToNumberFormat),
    JS_INLINABLE_FN("intl_GuardToPluralRules",
                    intrinsic_GuardToBuiltin<PluralRulesObject>, 1, 0,
                    IntlGuardToPluralRules),
    JS_INLINABLE_FN("intl_GuardToRelativeTimeFormat",
                    intrinsic_GuardToBuiltin<RelativeTimeFormatObject>, 1, 0,
                    IntlGuardToRelativeTimeFormat),
    JS_FN("intl_IsRuntimeDefaultLocale", intrinsic_IsRuntimeDefaultLocale, 1,
          0),
    JS_FN("intl_IsValidTimeZoneName", intl_IsValidTimeZoneName, 1, 0),
    JS_FN("intl_IsWrappedDateTimeFormat",
          intrinsic_IsWrappedInstanceOfBuiltin<DateTimeFormatObject>, 1, 0),
    JS_FN("intl_IsWrappedNumberFormat",
          intrinsic_IsWrappedInstanceOfBuiltin<NumberFormatObject>, 1, 0),
    JS_FN("intl_NumberFormat", intl_NumberFormat, 2, 0),
    JS_FN("intl_RuntimeDefaultLocale", intrinsic_RuntimeDefaultLocale, 0, 0),
    JS_FN("intl_SelectPluralRule", intl_SelectPluralRule, 2, 0),
    JS_FN("intl_TryValidateAndCanonicalizeLanguageTag",
          intl_TryValidateAndCanonicalizeLanguageTag, 1, 0),
    JS_FN("intl_ValidateAndCanonicalizeLanguageTag",
          intl_ValidateAndCanonicalizeLanguageTag, 2, 0),
    JS_FN("intl_ValidateAndCanonicalizeUnicodeExtensionType",
          intl_ValidateAndCanonicalizeUnicodeExtensionType, 3, 0),
    JS_FN("intl_availableCalendars", intl_availableCalendars, 1, 0),
    JS_FN("intl_availableCollations", intl_availableCollations, 1, 0),
#  if DEBUG || MOZ_SYSTEM_ICU
    JS_FN("intl_availableMeasurementUnits", intl_availableMeasurementUnits, 0,
          0),
#  endif
    JS_FN("intl_canonicalizeTimeZone", intl_canonicalizeTimeZone, 1, 0),
    JS_FN("intl_defaultCalendar", intl_defaultCalendar, 1, 0),
    JS_FN("intl_defaultTimeZone", intl_defaultTimeZone, 0, 0),
    JS_FN("intl_defaultTimeZoneOffset", intl_defaultTimeZoneOffset, 0, 0),
    JS_FN("intl_isDefaultTimeZone", intl_isDefaultTimeZone, 1, 0),
    JS_FN("intl_isUpperCaseFirst", intl_isUpperCaseFirst, 1, 0),
    JS_FN("intl_numberingSystem", intl_numberingSystem, 1, 0),
    JS_FN("intl_patternForSkeleton", intl_patternForSkeleton, 3, 0),
    JS_FN("intl_patternForStyle", intl_patternForStyle, 6, 0),
    JS_FN("intl_skeletonForPattern", intl_skeletonForPattern, 1, 0),
    JS_FN("intl_supportedLocaleOrFallback", intl_supportedLocaleOrFallback, 1,
          0),
    JS_FN("intl_toLocaleLowerCase", intl_toLocaleLowerCase, 2, 0),
    JS_FN("intl_toLocaleUpperCase", intl_toLocaleUpperCase, 2, 0),
#endif  // JS_HAS_INTL_API

    // Standard builtins used by self-hosting.
    JS_INLINABLE_FN("std_Array", array_construct, 1, 0, Array),
    JS_INLINABLE_FN("std_Array_pop", array_pop, 0, 0, ArrayPop),
    JS_INLINABLE_FN("std_Array_push", array_push, 1, 0, ArrayPush),
    JS_FN("std_BigInt_valueOf", BigIntObject::valueOf, 0, 0),
    JS_FN("std_Date_now", date_now, 0, 0),
    JS_FN("std_Function_apply", fun_apply, 2, 0),
    JS_FN("std_Map_entries", MapObject::entries, 0, 0),
    JS_INLINABLE_FN("std_Math_abs", math_abs, 1, 0, MathAbs),
    JS_INLINABLE_FN("std_Math_floor", math_floor, 1, 0, MathFloor),
    JS_INLINABLE_FN("std_Math_max", math_max, 2, 0, MathMax),
    JS_INLINABLE_FN("std_Math_min", math_min, 2, 0, MathMin),
    JS_INLINABLE_FN("std_Math_trunc", math_trunc, 1, 0, MathTrunc),
    JS_INLINABLE_FN("std_Object_create", obj_create, 2, 0, ObjectCreate),
    JS_INLINABLE_FN("std_Object_isPrototypeOf", obj_isPrototypeOf, 1, 0,
                    ObjectIsPrototypeOf),
    JS_FN("std_Object_propertyIsEnumerable", obj_propertyIsEnumerable, 1, 0),
    JS_FN("std_Object_setProto", obj_setProto, 1, 0),
    JS_FN("std_Object_toString", obj_toString, 0, 0),
    JS_INLINABLE_FN("std_Reflect_getPrototypeOf", Reflect_getPrototypeOf, 1, 0,
                    ReflectGetPrototypeOf),
    JS_FN("std_Reflect_isExtensible", Reflect_isExtensible, 1, 0),
    JS_FN("std_Reflect_ownKeys", Reflect_ownKeys, 1, 0),
    JS_FN("std_Set_values", SetObject::values, 0, 0),
    JS_INLINABLE_FN("std_String_charCodeAt", str_charCodeAt, 1, 0,
                    StringCharCodeAt),
    JS_FN("std_String_endsWith", str_endsWith, 1, 0),
    JS_INLINABLE_FN("std_String_fromCharCode", str_fromCharCode, 1, 0,
                    StringFromCharCode),
    JS_INLINABLE_FN("std_String_fromCodePoint", str_fromCodePoint, 1, 0,
                    StringFromCodePoint),
    JS_FN("std_String_includes", str_includes, 1, 0),
    JS_FN("std_String_indexOf", str_indexOf, 1, 0),
    JS_FN("std_String_startsWith", str_startsWith, 1, 0),
    JS_FN("std_TypedArray_buffer", js::TypedArray_bufferGetter, 1, 0),

    JS_FS_END};

void js::FillSelfHostingCompileOptions(CompileOptions& options) {
  /*
   * In self-hosting mode, scripts use JSOp::GetIntrinsic instead of
   * JSOp::GetName or JSOp::GetGName to access unbound variables.
   * JSOp::GetIntrinsic does a name lookup on a special object, whose
   * properties are filled in lazily upon first access for a given global.
   *
   * As that object is inaccessible to client code, the lookups are
   * guaranteed to return the original objects, ensuring safe implementation
   * of self-hosted builtins.
   *
   * Additionally, the special syntax callFunction(fun, receiver, ...args)
   * is supported, for which bytecode is emitted that invokes |fun| with
   * |receiver| as the this-object and ...args as the arguments.
   */
  options.setIntroductionType("self-hosted");
  options.setFileAndLine("self-hosted", 1);
  options.setSkipFilenameValidation(true);
  options.setSelfHostingMode(true);
  options.setForceFullParse();
  options.setForceStrictMode();
  options.setDiscardSource();
  options.setIsRunOnce(true);
  options.setNoScriptRval(true);
}

GlobalObject* JSRuntime::createSelfHostingGlobal(JSContext* cx) {
  MOZ_ASSERT(!cx->isExceptionPending());
  MOZ_ASSERT(!cx->realm());

  JS::RealmOptions options;
  options.creationOptions().setNewCompartmentInSelfHostingZone();
  // Debugging the selfHosted zone is not supported because CCWs are not
  // allowed in that zone.
  options.creationOptions().setInvisibleToDebugger(true);

  Realm* realm = NewRealm(cx, nullptr, options);
  if (!realm) {
    return nullptr;
  }

  static const JSClassOps shgClassOps = {
      nullptr,                   // addProperty
      nullptr,                   // delProperty
      nullptr,                   // enumerate
      nullptr,                   // newEnumerate
      nullptr,                   // resolve
      nullptr,                   // mayResolve
      nullptr,                   // finalize
      nullptr,                   // call
      nullptr,                   // hasInstance
      nullptr,                   // construct
      JS_GlobalObjectTraceHook,  // trace
  };

  static const JSClass shgClass = {"self-hosting-global", JSCLASS_GLOBAL_FLAGS,
                                   &shgClassOps};

  AutoRealmUnchecked ar(cx, realm);
  Rooted<GlobalObject*> shg(cx, GlobalObject::createInternal(cx, &shgClass));
  if (!shg) {
    return nullptr;
  }

  cx->runtime()->selfHostingGlobal_ = shg;
  MOZ_ASSERT(realm->zone()->isSelfHostingZone());
  realm->setIsSelfHostingRealm();

  if (!GlobalObject::initSelfHostingBuiltins(cx, shg, intrinsic_functions)) {
    return nullptr;
  }

  JS_FireOnNewGlobalObject(cx, shg);

  return shg;
}

class MOZ_STACK_CLASS AutoSelfHostingErrorReporter {
  JSContext* cx_;
  JS::WarningReporter oldReporter_;

 public:
  explicit AutoSelfHostingErrorReporter(JSContext* cx) : cx_(cx) {
    oldReporter_ = JS::SetWarningReporter(cx_, selfHosting_WarningReporter);
  }
  ~AutoSelfHostingErrorReporter() {
    JS::SetWarningReporter(cx_, oldReporter_);

    // Exceptions in self-hosted code will usually be printed to stderr in
    // ErrorToException, but not all exceptions are handled there. For
    // instance, ReportOutOfMemory will throw the "out of memory" string
    // without going through ErrorToException. We handle these other
    // exceptions here.
    MaybePrintAndClearPendingException(cx_);
  }
};

static bool VerifyGlobalNames(JSContext* cx, Handle<GlobalObject*> shg) {
#ifdef DEBUG
  // The `intrinsic_functions` list must be sorted so that we can use
  // mozilla::BinarySearch to do lookups on demand.
  const char* prev = "";
  for (JSFunctionSpec spec : intrinsic_functions) {
    if (spec.name.string()) {
      MOZ_ASSERT(strcmp(prev, spec.name.string()) < 0,
                 "Self-hosted intrinsics must be sorted");
      prev = spec.name.string();
    }
  }

  RootedId id(cx);
  bool nameMissing = false;

  for (auto base = cx->zone()->cellIter<BaseScript>();
       !base.done() && !nameMissing; base.next()) {
    if (!base->hasBytecode()) {
      continue;
    }
    JSScript* script = base->asJSScript();

    for (BytecodeLocation loc : AllBytecodesIterable(script)) {
      JSOp op = loc.getOp();

      if (op == JSOp::GetIntrinsic) {
        PropertyName* name = loc.getPropertyName(script);
        id = NameToId(name);

        if (shg->lookupPure(id).isNothing()) {
          // cellIter disallows GCs, but error reporting wants to
          // have them, so we need to move it out of the loop.
          nameMissing = true;
          break;
        }
      }
    }
  }

  if (nameMissing) {
    return Throw(cx, id, JSMSG_NO_SUCH_SELF_HOSTED_PROP);
  }
#endif  // DEBUG

  return true;
}

[[nodiscard]] bool InitSelfHostingFromStencil(
    JSContext* cx, Handle<GlobalObject*> shg, frontend::CompilationInput& input,
    const frontend::CompilationStencil& stencil) {
  // Instantiate the stencil and run the script.
  // NOTE: Use a block here so the GC roots are dropped before freezing the Zone
  //       below.
  {
    Rooted<frontend::CompilationGCOutput> output(cx);
    if (!frontend::CompilationStencil::instantiateStencils(cx, input, stencil,
                                                           output.get())) {
      return false;
    }

    // Run the script
    RootedScript script(cx, output.get().script);
    RootedValue rval(cx);
    if (!JS_ExecuteScript(cx, script, &rval)) {
      return false;
    }
  }

  if (!VerifyGlobalNames(cx, shg)) {
    return false;
  }

  // Garbage collect the self hosting zone once when it is created. It should
  // not be modified after this point.
  cx->runtime()->gc.freezeSelfHostingZone();

  return true;
}

bool JSRuntime::initSelfHosting(JSContext* cx, JS::SelfHostedCache xdrCache,
                                JS::SelfHostedWriter xdrWriter) {
  MOZ_ASSERT(!selfHostingGlobal_);

  if (cx->runtime()->parentRuntime) {
    selfHostingGlobal_ = cx->runtime()->parentRuntime->selfHostingGlobal_;
    return true;
  }

  Rooted<GlobalObject*> shg(cx, JSRuntime::createSelfHostingGlobal(cx));
  if (!shg) {
    return false;
  }

  JSAutoRealm ar(cx, shg);

  /*
   * Set a temporary error reporter printing to stderr because it is too
   * early in the startup process for any other reporter to be registered
   * and we don't want errors in self-hosted code to be silently swallowed.
   *
   * This class also overrides the warning reporter to print warnings to
   * stderr. See selfHosting_WarningReporter.
   */
  AutoSelfHostingErrorReporter errorReporter(cx);

  // Variables used to instantiate scripts.
  CompileOptions options(cx);
  FillSelfHostingCompileOptions(options);

  // Try initializing from Stencil XDR.
  bool decodeOk = false;
  Rooted<frontend::CompilationGCOutput> output(cx);
  if (xdrCache.Length() > 0) {
    // Allow the VM to directly use bytecode from the XDR buffer without
    // copying it. The buffer must outlive all runtimes (including workers).
    options.usePinnedBytecode = true;

    Rooted<frontend::CompilationInput> input(
        cx, frontend::CompilationInput(options));
    if (!input.get().initForSelfHostingGlobal(cx)) {
      return false;
    }

    frontend::CompilationStencil stencil(input.get().source);
    if (!stencil.deserializeStencils(cx, input.get(), xdrCache, &decodeOk)) {
      return false;
    }

    if (decodeOk) {
      return InitSelfHostingFromStencil(cx, shg, input.get(), stencil);
    }
  }

  // If script wasn't generated, it means XDR was either not provided or that it
  // failed the decoding phase. Parse from text as before.
  uint32_t srcLen = GetRawScriptsSize();
  const unsigned char* compressed = compressedSources;
  uint32_t compressedLen = GetCompressedSize();
  auto src = cx->make_pod_array<char>(srcLen);
  if (!src) {
    return false;
  }
  if (!DecompressString(compressed, compressedLen,
                        reinterpret_cast<unsigned char*>(src.get()), srcLen)) {
    return false;
  }

  JS::SourceText<mozilla::Utf8Unit> srcBuf;
  if (!srcBuf.init(cx, std::move(src), srcLen)) {
    return false;
  }

  Rooted<frontend::CompilationInput> input(cx,
                                           frontend::CompilationInput(options));
  auto stencil = frontend::CompileGlobalScriptToStencil(cx, input.get(), srcBuf,
                                                        ScopeKind::Global);
  if (!stencil) {
    return false;
  }

  // Serialize the stencil to XDR.
  if (xdrWriter) {
    JS::TranscodeBuffer xdrBuffer;
    if (!stencil->serializeStencils(cx, input.get(), xdrBuffer)) {
      return false;
    }

    if (!xdrWriter(cx, xdrBuffer)) {
      return false;
    }
  }

  return InitSelfHostingFromStencil(cx, shg, input.get(), *stencil);
}

void JSRuntime::finishSelfHosting() { selfHostingGlobal_ = nullptr; }

void JSRuntime::traceSelfHostingGlobal(JSTracer* trc) {
  if (selfHostingGlobal_ && !parentRuntime) {
    TraceRoot(trc, const_cast<NativeObject**>(&selfHostingGlobal_.ref()),
              "self-hosting global");
  }
}

GeneratorKind JSRuntime::getSelfHostedFunctionGeneratorKind(JSAtom* name) {
  JSFunction* fun = getUnclonedSelfHostedFunction(name->asPropertyName());
  return fun->generatorKind();
}

static bool CloneValue(JSContext* cx, HandleValue selfHostedValue,
                       MutableHandleValue vp);

static void GetUnclonedValue(NativeObject* selfHostedObject,
                             const PropertyKey& id, Value* vp) {
  if (JSID_IS_INT(id)) {
    size_t index = JSID_TO_INT(id);
    if (index < selfHostedObject->getDenseInitializedLength() &&
        !selfHostedObject->getDenseElement(index).isMagic(JS_ELEMENTS_HOLE)) {
      *vp = selfHostedObject->getDenseElement(JSID_TO_INT(id));
      return;
    }
  }

  // Since all atoms used by self-hosting are marked as permanent, the only
  // reason we'd see a non-permanent atom here is code looking for
  // properties on the self hosted global which aren't present.
  // Since we ensure that that can't happen during startup, encountering
  // non-permanent atoms here should be impossible.
  MOZ_ASSERT_IF(JSID_IS_STRING(id), JSID_TO_STRING(id)->isPermanentAtom());

  mozilla::Maybe<PropertyInfo> prop = selfHostedObject->lookupPure(id);
  MOZ_ASSERT(prop.isSome());
  MOZ_ASSERT(prop->isDataProperty());
  *vp = selfHostedObject->getSlot(prop->slot());
}

static bool CloneProperties(JSContext* cx, HandleNativeObject selfHostedObject,
                            HandleObject clone) {
  RootedIdVector ids(cx);
  Vector<uint8_t, 16> attrs(cx);

  for (size_t i = 0; i < selfHostedObject->getDenseInitializedLength(); i++) {
    if (!selfHostedObject->getDenseElement(i).isMagic(JS_ELEMENTS_HOLE)) {
      if (!ids.append(INT_TO_JSID(i))) {
        return false;
      }
      if (!attrs.append(JSPROP_ENUMERATE)) {
        return false;
      }
    }
  }

  Rooted<PropertyInfoWithKeyVector> props(cx, PropertyInfoWithKeyVector(cx));
  for (ShapePropertyIter<NoGC> iter(selfHostedObject->shape()); !iter.done();
       iter++) {
    if (iter->enumerable() && !props.append(*iter)) {
      return false;
    }
  }

  // Now our properties are in last-to-first order, so....
  std::reverse(props.begin(), props.end());
  for (size_t i = 0; i < props.length(); ++i) {
    MOZ_ASSERT(props[i].isDataProperty(),
               "Can't handle cloning accessors here yet.");
    if (!ids.append(props[i].key())) {
      return false;
    }
    PropertyInfo prop = props[i];
    uint8_t propAttrs = 0;
    if (prop.enumerable()) {
      propAttrs |= JSPROP_ENUMERATE;
    }
    if (!prop.configurable()) {
      propAttrs |= JSPROP_PERMANENT;
    }
    if (!prop.writable()) {
      propAttrs |= JSPROP_READONLY;
    }
    if (!attrs.append(propAttrs)) {
      return false;
    }
  }

  RootedId id(cx);
  RootedValue val(cx);
  RootedValue selfHostedValue(cx);
  for (uint32_t i = 0; i < ids.length(); i++) {
    id = ids[i];
    GetUnclonedValue(selfHostedObject, id, selfHostedValue.address());
    if (!CloneValue(cx, selfHostedValue, &val) ||
        !JS_DefinePropertyById(cx, clone, id, val, attrs[i])) {
      return false;
    }
  }

  return true;
}

static JSString* CloneString(JSContext* cx, JSLinearString* selfHostedString) {
  size_t len = selfHostedString->length();
  {
    JS::AutoCheckCannotGC nogc;
    JSString* clone;
    if (selfHostedString->hasLatin1Chars()) {
      clone =
          NewStringCopyN<NoGC>(cx, selfHostedString->latin1Chars(nogc), len);
    } else {
      clone = NewStringCopyNDontDeflate<NoGC>(
          cx, selfHostedString->twoByteChars(nogc), len);
    }
    if (clone) {
      return clone;
    }
  }

  AutoStableStringChars chars(cx);
  if (!chars.init(cx, selfHostedString)) {
    return nullptr;
  }

  return chars.isLatin1()
             ? NewStringCopyN<CanGC>(cx, chars.latin1Range().begin().get(), len)
             : NewStringCopyNDontDeflate<CanGC>(
                   cx, chars.twoByteRange().begin().get(), len);
}

// Returns the ScriptSourceObject to use for cloned self-hosted scripts in the
// current realm.
static ScriptSourceObject* SelfHostingScriptSourceObject(JSContext* cx) {
  if (ScriptSourceObject* sso = cx->realm()->selfHostingScriptSource) {
    return sso;
  }

  CompileOptions options(cx);
  FillSelfHostingCompileOptions(options);

  RefPtr<ScriptSource> source(cx->new_<ScriptSource>());
  if (!source) {
    return nullptr;
  }

  if (!source->initFromOptions(cx, options)) {
    return nullptr;
  }

  RootedScriptSourceObject sourceObject(
      cx, ScriptSourceObject::create(cx, source.get()));
  if (!sourceObject) {
    return nullptr;
  }

  if (!ScriptSourceObject::initFromOptions(cx, sourceObject, options)) {
    return nullptr;
  }

  cx->realm()->selfHostingScriptSource.set(sourceObject);
  return sourceObject;
}

static JSObject* CloneObject(JSContext* cx,
                             HandleNativeObject selfHostedObject) {
#ifdef DEBUG
  // Object hash identities are owned by the hashed object, which may be on a
  // different thread than the clone target. In theory, these objects are all
  // tenured and will not be compacted; however, we simply avoid the issue
  // altogether by skipping the cycle-detection when off thread.
  mozilla::Maybe<AutoCycleDetector> detect;
  if (js::CurrentThreadCanAccessZone(selfHostedObject->zoneFromAnyThread())) {
    detect.emplace(cx, selfHostedObject);
    if (!detect->init()) {
      return nullptr;
    }
    if (detect->foundCycle()) {
      MOZ_CRASH("SelfHosted cloning cannot handle cyclic object graphs.");
    }
  }
#endif

  RootedObject clone(cx);
  if (selfHostedObject->is<JSFunction>()) {
    RootedFunction selfHostedFunction(cx, &selfHostedObject->as<JSFunction>());
    if (selfHostedFunction->isInterpreted()) {
      // Arrow functions use the first extended slot for their lexical |this|
      // value. And methods use the first extended slot for their home-object.
      // We only expect to see normal functions here.
      MOZ_ASSERT(selfHostedFunction->kind() == FunctionFlags::NormalFunction);
      MOZ_ASSERT(selfHostedFunction->isLambda() == false);

      Handle<GlobalObject*> global = cx->global();
      Rooted<GlobalLexicalEnvironmentObject*> globalLexical(
          cx, &global->lexicalEnvironment());
      RootedScope emptyGlobalScope(cx, &global->emptyGlobalScope());
      Rooted<ScriptSourceObject*> sourceObject(
          cx, SelfHostingScriptSourceObject(cx));
      if (!sourceObject) {
        return nullptr;
      }
      MOZ_ASSERT(
          !CanReuseScriptForClone(cx->realm(), selfHostedFunction, global));
      clone = CloneFunctionAndScript(cx, selfHostedFunction, globalLexical,
                                     emptyGlobalScope, sourceObject,
                                     gc::AllocKind::FUNCTION_EXTENDED);
      if (!clone) {
        return nullptr;
      }

      // Save the original function name that we are cloning from. This allows
      // the function to potentially be relazified in the future.
      SetClonedSelfHostedFunctionName(&clone->as<JSFunction>(),
                                      selfHostedFunction->explicitName());

      // If |_SetCanonicalName| was called on the function, the function name to
      // use is stored in the extended slot.
      if (JSAtom* name =
              GetUnclonedSelfHostedCanonicalName(selfHostedFunction)) {
        clone->as<JSFunction>().setAtom(name);
      }
    } else {
      clone = CloneSelfHostingIntrinsic(cx, selfHostedFunction);
    }
  } else if (selfHostedObject->is<RegExpObject>()) {
    RegExpObject& reobj = selfHostedObject->as<RegExpObject>();
    RootedAtom source(cx, reobj.getSource());
    MOZ_ASSERT(source->isPermanentAtom());
    clone = RegExpObject::create(cx, source, reobj.getFlags(), TenuredObject);
  } else if (selfHostedObject->is<DateObject>()) {
    clone =
        JS::NewDateObject(cx, selfHostedObject->as<DateObject>().clippedTime());
  } else if (selfHostedObject->is<BooleanObject>()) {
    clone = BooleanObject::create(
        cx, selfHostedObject->as<BooleanObject>().unbox());
  } else if (selfHostedObject->is<NumberObject>()) {
    clone =
        NumberObject::create(cx, selfHostedObject->as<NumberObject>().unbox());
  } else if (selfHostedObject->is<StringObject>()) {
    JSString* selfHostedString = selfHostedObject->as<StringObject>().unbox();
    if (!selfHostedString->isLinear()) {
      MOZ_CRASH();
    }
    RootedString str(cx, CloneString(cx, &selfHostedString->asLinear()));
    if (!str) {
      return nullptr;
    }
    clone = StringObject::create(cx, str);
  } else if (selfHostedObject->is<ArrayObject>()) {
    clone = NewTenuredDenseEmptyArray(cx, nullptr);
  } else {
    MOZ_ASSERT(selfHostedObject->is<NativeObject>());
    clone = NewObjectWithGivenProto(
        cx, selfHostedObject->getClass(), nullptr,
        selfHostedObject->asTenured().getAllocKind(), TenuredObject);
  }
  if (!clone) {
    return nullptr;
  }

  if (!CloneProperties(cx, selfHostedObject, clone)) {
    return nullptr;
  }
  return clone;
}

static bool CloneValue(JSContext* cx, HandleValue selfHostedValue,
                       MutableHandleValue vp) {
  if (selfHostedValue.isObject()) {
    RootedNativeObject selfHostedObject(
        cx, &selfHostedValue.toObject().as<NativeObject>());
    JSObject* clone = CloneObject(cx, selfHostedObject);
    if (!clone) {
      return false;
    }
    vp.setObject(*clone);
  } else if (selfHostedValue.isBoolean() || selfHostedValue.isNumber() ||
             selfHostedValue.isNullOrUndefined()) {
    // Nothing to do here: these are represented inline in the value.
    vp.set(selfHostedValue);
  } else if (selfHostedValue.isString()) {
    if (!selfHostedValue.toString()->isLinear()) {
      MOZ_CRASH();
    }
    JSLinearString* selfHostedString = &selfHostedValue.toString()->asLinear();
    JSString* clone = CloneString(cx, selfHostedString);
    if (!clone) {
      return false;
    }
    vp.setString(clone);
  } else if (selfHostedValue.isSymbol()) {
    // Well-known symbols are shared.
    mozilla::DebugOnly<JS::Symbol*> sym = selfHostedValue.toSymbol();
    MOZ_ASSERT(sym->isWellKnownSymbol());
    MOZ_ASSERT(cx->wellKnownSymbols().get(sym->code()) == sym);
    vp.set(selfHostedValue);
  } else {
    MOZ_CRASH("Self-hosting CloneValue can't clone given value.");
  }
  return true;
}

bool JSRuntime::createLazySelfHostedFunctionClone(
    JSContext* cx, HandlePropertyName selfHostedName, HandleAtom name,
    unsigned nargs, NewObjectKind newKind, MutableHandleFunction fun) {
  MOZ_ASSERT(newKind != GenericObject);

  RootedAtom funName(cx, name);
  JSFunction* selfHostedFun = getUnclonedSelfHostedFunction(selfHostedName);
  if (!selfHostedFun) {
    return false;
  }

  // If there is a a canonical name set, use that instead.
  if (JSAtom* name = GetUnclonedSelfHostedCanonicalName(selfHostedFun)) {
    funName = name;
  }

  RootedObject proto(cx);
  if (!GetFunctionPrototype(cx, selfHostedFun->generatorKind(),
                            selfHostedFun->asyncKind(), &proto)) {
    return false;
  }

  fun.set(NewScriptedFunction(cx, nargs, FunctionFlags::BASESCRIPT, funName,
                              proto, gc::AllocKind::FUNCTION_EXTENDED,
                              newKind));
  if (!fun) {
    return false;
  }
  fun->setIsSelfHostedBuiltin();
  fun->initSelfHostedLazyScript(&cx->runtime()->selfHostedLazyScript.ref());
  SetClonedSelfHostedFunctionName(fun, selfHostedName);
  return true;
}

bool JSRuntime::cloneSelfHostedFunctionScript(JSContext* cx,
                                              HandlePropertyName name,
                                              HandleFunction targetFun) {
  RootedFunction sourceFun(cx, getUnclonedSelfHostedFunction(name));
  if (!sourceFun) {
    return false;
  }
  MOZ_ASSERT(targetFun->isExtended());
  MOZ_ASSERT(targetFun->hasSelfHostedLazyScript());

  RootedScript sourceScript(cx, JSFunction::getOrCreateScript(cx, sourceFun));
  if (!sourceScript) {
    return false;
  }

  Rooted<ScriptSourceObject*> sourceObject(cx,
                                           SelfHostingScriptSourceObject(cx));
  if (!sourceObject) {
    return false;
  }

  // Assert that there are no intervening scopes between the global scope
  // and the self-hosted script. Toplevel lexicals are explicitly forbidden
  // by the parser when parsing self-hosted code. The fact they have the
  // global lexical scope on the scope chain is for uniformity and engine
  // invariants.
  MOZ_ASSERT(sourceScript->outermostScope()->enclosing()->kind() ==
             ScopeKind::Global);
  RootedScope emptyGlobalScope(cx, &cx->global()->emptyGlobalScope());
  if (!CloneScriptIntoFunction(cx, emptyGlobalScope, targetFun, sourceScript,
                               sourceObject)) {
    return false;
  }

  MOZ_ASSERT(targetFun->hasBytecode());
  RootedScript targetScript(cx, targetFun->nonLazyScript());

  // Relazifiable self-hosted function may be relazified later into a
  // SelfHostedLazyScript. It is important to note that this only applies to
  // named self-hosted entry points (that use this clone method). Inner
  // functions clones used by self-hosted are never relazified, even if they
  // would be able to in normal script.
  if (targetScript->isRelazifiable()) {
    targetScript->setAllowRelazify();
  }

  MOZ_ASSERT(sourceFun->nargs() == targetFun->nargs());
  MOZ_ASSERT(sourceScript->hasRest() == targetScript->hasRest());
  MOZ_ASSERT(targetFun->strict(), "Self-hosted builtins must be strict");

  // The target function might have been relazified after its flags changed.
  targetFun->setFlags(targetFun->flags().toRaw() | sourceFun->flags().toRaw());
  return true;
}

void JSRuntime::getUnclonedSelfHostedValue(PropertyName* name, Value* vp) {
  PropertyKey id = NameToId(name);
  GetUnclonedValue(selfHostingGlobal_, id, vp);
}

JSFunction* JSRuntime::getUnclonedSelfHostedFunction(PropertyName* name) {
  Value selfHostedValue;
  getUnclonedSelfHostedValue(name, &selfHostedValue);
  return &selfHostedValue.toObject().as<JSFunction>();
}

bool JSRuntime::cloneSelfHostedValue(JSContext* cx, HandlePropertyName name,
                                     MutableHandleValue vp) {
  RootedValue selfHostedValue(cx);
  getUnclonedSelfHostedValue(name, selfHostedValue.address());

  /*
   * We don't clone if we're operating in the self-hosting global, as that
   * means we're currently executing the self-hosting script while
   * initializing the runtime (see JSRuntime::initSelfHosting).
   */
  if (cx->global() == selfHostingGlobal_) {
    vp.set(selfHostedValue);
    return true;
  }

  return CloneValue(cx, selfHostedValue, vp);
}

void JSRuntime::assertSelfHostedFunctionHasCanonicalName(
    JSContext* cx, HandlePropertyName name) {
#ifdef DEBUG
  JSFunction* selfHostedFun = getUnclonedSelfHostedFunction(name);
  MOZ_ASSERT(selfHostedFun);
  MOZ_ASSERT(GetUnclonedSelfHostedCanonicalName(selfHostedFun));
#endif
}

bool js::IsSelfHostedFunctionWithName(JSFunction* fun, JSAtom* name) {
  return fun->isSelfHostedBuiltin() && fun->isExtended() &&
         GetClonedSelfHostedFunctionName(fun) == name;
}

static_assert(
    JSString::MAX_LENGTH <= INT32_MAX,
    "StringIteratorNext in builtin/String.js assumes the stored index "
    "into the string is an Int32Value");

static_assert(JSString::MAX_LENGTH == MAX_STRING_LENGTH,
              "JSString::MAX_LENGTH matches self-hosted constant for maximum "
              "string length");

static_assert(ARGS_LENGTH_MAX == MAX_ARGS_LENGTH,
              "ARGS_LENGTH_MAX matches self-hosted constant for maximum "
              "arguments length");
