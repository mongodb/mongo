/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/SelfHosting.h"

#include "mozilla/BinarySearch.h"
#include "mozilla/Casting.h"
#include "mozilla/Maybe.h"
#include "mozilla/ScopeExit.h"  // mozilla::MakeScopeExit
#include "mozilla/Utf8.h"       // mozilla::Utf8Unit

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
#  include "builtin/intl/Segmenter.h"
#endif
#include "builtin/MapObject.h"
#include "builtin/Object.h"
#include "builtin/Promise.h"
#include "builtin/Reflect.h"
#include "builtin/RegExp.h"
#include "builtin/SelfHostingDefines.h"
#include "builtin/String.h"
#ifdef ENABLE_RECORD_TUPLE
#  include "builtin/TupleObject.h"
#endif
#include "frontend/BytecodeCompiler.h"    // CompileGlobalScriptToStencil
#include "frontend/CompilationStencil.h"  // js::frontend::CompilationStencil
#include "frontend/FrontendContext.h"     // AutoReportFrontendContext
#include "jit/AtomicOperations.h"
#include "jit/InlinableNatives.h"
#include "jit/TrampolineNatives.h"
#include "js/CompilationAndEvaluation.h"
#include "js/Conversions.h"
#include "js/ErrorReport.h"  // JS::PrintError
#include "js/experimental/JSStencil.h"
#include "js/experimental/TypedData.h"  // JS_GetArrayBufferViewType
#include "js/friend/ErrorMessages.h"    // js::GetErrorMessage, JSMSG_*
#include "js/HashTable.h"
#include "js/Printer.h"
#include "js/PropertySpec.h"
#include "js/ScalarType.h"  // js::Scalar::Type
#include "js/SourceText.h"  // JS::SourceText
#include "js/TracingAPI.h"
#include "js/Transcoding.h"
#include "js/Warnings.h"  // JS::{,Set}WarningReporter
#include "js/Wrapper.h"
#include "vm/ArgumentsObject.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/BigIntType.h"
#include "vm/Compression.h"
#include "vm/DateObject.h"
#include "vm/ErrorReporting.h"  // js::MaybePrintAndClearPendingException
#include "vm/Float16.h"
#include "vm/FrameIter.h"  // js::ScriptFrameIter
#include "vm/GeneratorObject.h"
#include "vm/Interpreter.h"
#include "vm/Iteration.h"
#include "vm/JSAtomUtils.h"  // Atomize
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/PIC.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/Realm.h"
#include "vm/RegExpObject.h"
#include "vm/StringType.h"
#include "vm/ToSource.h"  // js::ValueToSource
#include "vm/TypedArrayObject.h"
#include "vm/Uint8Clamped.h"
#include "vm/WrapperObject.h"

#include "vm/Compartment-inl.h"
#include "vm/JSAtomUtils-inl.h"  // PrimitiveValueToId
#include "vm/JSFunction-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/TypedArrayObject-inl.h"

using namespace js;
using namespace js::selfhosted;

using JS::CompileOptions;
using mozilla::Maybe;

static bool intrinsic_ToObject(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  JSObject* obj = ToObject(cx, args[0]);
  if (!obj) {
    return false;
  }
  args.rval().setObject(*obj);
  return true;
}

#ifdef ENABLE_RECORD_TUPLE

bool intrinsic_ThisTupleValue(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  mozilla::Maybe<TupleType&> result = js::ThisTupleValue(cx, args[0]);
  if (!result) {
    return false;
  }
  args.rval().setExtendedPrimitive(*result);
  return true;
}

bool intrinsic_TupleLength(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  mozilla::Maybe<TupleType&> result = js::ThisTupleValue(cx, args[0]);
  if (!result) {
    return false;
  }
  args.rval().setInt32((*result).getDenseInitializedLength());
  return true;
}
#endif

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

#ifdef ENABLE_RECORD_TUPLE
// returns true for TupleTypes and TupleObjects
bool js::IsTupleUnchecked(JSContext* cx, const CallArgs& args) {
  args.rval().setBoolean(IsTuple(args.get(0)));
  return true;
}

/* Identical to Tuple.prototype.isTuple, but with an
 * added check that args.length() is 1
 */
bool js::intrinsic_IsTuple(JSContext* cx, unsigned argc, JS::Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  return js::IsTupleUnchecked(cx, args);
}
#endif

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
      str->dumpCharsNoQuote(out);
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
      str->dumpCharsNoQuote(out);
      out.putChar('\n');
    } else {
      cx->recoverFromOutOfMemory();
    }
  }
#endif
  args.rval().setUndefined();
  return true;
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

bool js::IsExtendedUnclonedSelfHostedFunctionName(JSAtom* name) {
  if (name->length() < 2) {
    return false;
  }
  return name->latin1OrTwoByteChar(0) ==
         ExtendedUnclonedSelfHostedFunctionNamePrefix;
}

void js::SetClonedSelfHostedFunctionName(JSFunction* fun,
                                         js::PropertyName* name) {
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
  genObj->setClosed(cx);
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
  static_assert(TYPEDARRAY_KIND_FLOAT16 == Scalar::Type::Float16,
                "TYPEDARRAY_KIND_FLOAT16 doesn't match the scalar type");

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
  MOZ_ASSERT(args[0].toObject().is<TypedArrayObject>());

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
  MOZ_ASSERT(args[0].toObject().is<TypedArrayObject>());

  auto* tarr = &args[0].toObject().as<TypedArrayObject>();
  args.rval().setNumber(tarr->byteOffsetMaybeOutOfBounds());
  return true;
}

static bool intrinsic_TypedArrayElementSize(JSContext* cx, unsigned argc,
                                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].toObject().is<TypedArrayObject>());

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
  MOZ_ASSERT(args[0].toObject().is<TypedArrayObject>());

  auto* tarr = &args[0].toObject().as<TypedArrayObject>();

  mozilla::Maybe<size_t> length = tarr->length();
  if (!length) {
    // Return zero for detached buffers to match JIT code.
    if (tarr->hasDetachedBuffer()) {
      args.rval().setInt32(0);
      return true;
    }

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_RESIZED_BOUNDS);
    return false;
  }

  args.rval().setNumber(*length);
  return true;
}

// Return the value of [[ArrayLength]] internal slot of the TypedArray. If the
// length is out-of-bounds, always return zero.
static bool intrinsic_TypedArrayLengthZeroOnOutOfBounds(JSContext* cx,
                                                        unsigned argc,
                                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].toObject().is<TypedArrayObject>());

  auto* tarr = &args[0].toObject().as<TypedArrayObject>();
  args.rval().setNumber(tarr->length().valueOr(0));
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

  mozilla::Maybe<size_t> length = obj->length();
  if (!length) {
    // Return zero for detached buffers to match JIT code.
    if (obj->hasDetachedBuffer()) {
      args.rval().setInt32(0);
      return true;
    }

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_RESIZED_BOUNDS);
    return false;
  }

  args.rval().setNumber(*length);
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

static bool intrinsic_TypedArrayIsAutoLength(JSContext* cx, unsigned argc,
                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].toObject().is<TypedArrayObject>());

  JSObject* obj = &args[0].toObject();
  bool isAutoLength = obj->is<ResizableTypedArrayObject>() &&
                      obj->as<ResizableTypedArrayObject>().isAutoLength();
  args.rval().setBoolean(isAutoLength);
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

    case Scalar::Float16:
      return targetType == Scalar::Float16;

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

  auto sourceLength = source->length();
  if (!sourceLength) {
    if (source->hasDetachedBuffer()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TYPED_ARRAY_DETACHED);
    } else {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TYPED_ARRAY_RESIZED_BOUNDS);
    }
    return false;
  }
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

  MOZ_ASSERT(count > 0);
  MOZ_ASSERT(count <= unsafeTypedArrayCrossCompartment->length().valueOr(0));

  size_t elementSize = TypedArrayElemSize(sourceType);
  MOZ_ASSERT(elementSize ==
             TypedArrayElemSize(unsafeTypedArrayCrossCompartment->type()));

  SharedMem<uint8_t*> sourceData =
      source->dataPointerEither().cast<uint8_t*>() + sourceOffset * elementSize;

  SharedMem<uint8_t*> unsafeTargetDataCrossCompartment =
      unsafeTypedArrayCrossCompartment->dataPointerEither().cast<uint8_t*>();

  size_t byteLength = std::min(count, *sourceLength) * elementSize;

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

  Rooted<FixedLengthTypedArrayObject*> target(
      cx, &args[0].toObject().as<FixedLengthTypedArrayObject>());
  MOZ_ASSERT(!target->hasDetachedBuffer());
  MOZ_ASSERT(!target->isSharedMemory());

  Rooted<ArrayObject*> source(cx, &args[1].toObject().as<ArrayObject>());
  MOZ_ASSERT(IsPackedArray(source));
  MOZ_ASSERT(source->length() == target->length());

  switch (target->type()) {
#define INIT_TYPED_ARRAY(_, T, N)                                      \
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

template <bool ForTest>
static bool intrinsic_RegExpBuiltinExec(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  MOZ_ASSERT(args[0].isObject());
  MOZ_ASSERT(args[0].toObject().is<RegExpObject>());
  MOZ_ASSERT(args[1].isString());

  Rooted<RegExpObject*> obj(cx, &args[0].toObject().as<RegExpObject>());
  Rooted<JSString*> string(cx, args[1].toString());
  return RegExpBuiltinExec(cx, obj, string, ForTest, args.rval());
}

template <bool ForTest>
static bool intrinsic_RegExpExec(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  MOZ_ASSERT(args[0].isObject());
  MOZ_ASSERT(args[1].isString());

  Rooted<JSObject*> obj(cx, &args[0].toObject());
  Rooted<JSString*> string(cx, args[1].toString());
  return RegExpExec(cx, obj, string, ForTest, args.rval());
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

  Rooted<ArrayObject*> matchResult(cx, &args[0].toObject().as<ArrayObject>());

  Rooted<JSLinearString*> string(cx, args[1].toString()->ensureLinear(cx));
  if (!string) {
    return false;
  }

  int32_t position = int32_t(args[2].toNumber());
  MOZ_ASSERT(position >= 0);

  Rooted<JSLinearString*> replacement(cx, args[3].toString()->ensureLinear(cx));
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

static bool intrinsic_RegExpHasCaptureGroups(JSContext* cx, unsigned argc,
                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  MOZ_ASSERT(args[0].isObject());
  MOZ_ASSERT(args[1].isString());

  Rooted<RegExpObject*> obj(cx, &args[0].toObject().as<RegExpObject>());
  Rooted<JSString*> string(cx, args[1].toString());

  bool result;
  if (!RegExpHasCaptureGroups(cx, obj, string, &result)) {
    return false;
  }

  args.rval().setBoolean(result);
  return true;
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
  Rooted<PropertyName*> name(
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
  Rooted<PropertyName*> funName(cx, funAtom->asPropertyName());
  return CallSelfHostedFunction(cx, funName, thisv, args, rval);
}
#endif

bool js::CallSelfHostedFunction(JSContext* cx, Handle<PropertyName*> name,
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
                                            Handle<Value> thisValue) {
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
      "EnsureTypedArrayWithArrayBuffer",
      "RegExpSearchSlowPath",
      "RegExpReplaceSlowPath",
      "RegExpMatchSlowPath",
  };

  ScriptFrameIter iter(cx);
  MOZ_ASSERT(iter.isFunctionFrame());

  while (!iter.done()) {
    MOZ_ASSERT(iter.callee(cx)->isSelfHostedOrIntrinsic());
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
                               InformalValueTypeName(thisValue));
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

  const char* locale = cx->realm()->getLocale();
  if (!locale) {
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

  const char* locale = cx->realm()->getLocale();
  if (!locale) {
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

static bool intrinsic_ConstructFunction(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);
  MOZ_ASSERT(IsConstructor(args[0]));
  MOZ_ASSERT(IsConstructor(args[1]));
  MOZ_ASSERT(args[2].toObject().is<ArrayObject>());

  Rooted<ArrayObject*> argsList(cx, &args[2].toObject().as<ArrayObject>());
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

static JSObject* NewIteratorRecord(JSContext* cx, HandleObject iterator,
                                   HandleValue nextMethod) {
  gc::AllocKind allocKind = gc::GetGCObjectKind(3);
  Rooted<PlainObject*> obj(
      cx, NewPlainObjectWithProtoAndAllocKind(cx, nullptr, allocKind));
  if (!obj) {
    return nullptr;
  }

  RootedId propid(cx, NameToId(cx->names().iterator));
  RootedValue value(cx, ObjectValue(*iterator));
  if (!NativeDefineDataProperty(cx, obj, propid, value, JSPROP_ENUMERATE)) {
    return nullptr;
  }

  propid = NameToId(cx->names().nextMethod);
  value.set(nextMethod);
  if (!NativeDefineDataProperty(cx, obj, propid, value, JSPROP_ENUMERATE)) {
    return nullptr;
  }

  propid = NameToId(cx->names().done);
  value.setBoolean(false);
  if (!NativeDefineDataProperty(cx, obj, propid, value, JSPROP_ENUMERATE)) {
    return nullptr;
  }

  return obj;
}

static bool intrinsic_CreateAsyncFromSyncIterator(JSContext* cx, unsigned argc,
                                                  Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);

  RootedObject iterator(cx, &args[0].toObject());
  RootedObject asyncIterator(
      cx, CreateAsyncFromSyncIterator(cx, iterator, args[1]));
  if (!asyncIterator) {
    return false;
  }

  RootedValue nextMethod(cx);
  if (!GetProperty(cx, asyncIterator, asyncIterator, cx->names().next,
                   &nextMethod)) {
    return false;
  }

  RootedObject iteratorRecord(cx,
                              NewIteratorRecord(cx, asyncIterator, nextMethod));
  if (!iteratorRecord) {
    return false;
  }

  args.rval().setObject(*iteratorRecord);
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

static bool intrinsic_newList(JSContext* cx, unsigned argc, js::Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 0);

  ArrayObject* list = NewArrayWithNullProto(cx);
  if (!list) {
    return false;
  }

  args.rval().setObject(*list);
  return true;
}

static const JSFunctionSpec intrinsic_functions[] = {
    // Intrinsic helper functions
    JS_INLINABLE_FN("ArrayBufferByteLength",
                    intrinsic_ArrayBufferByteLength<ArrayBufferObject>, 1, 0,
                    IntrinsicArrayBufferByteLength),
    JS_FN("ArrayBufferCopyData",
          intrinsic_ArrayBufferCopyData<ArrayBufferObject>, 6, 0),
    JS_INLINABLE_FN("ArrayIteratorPrototypeOptimizable",
                    intrinsic_ArrayIteratorPrototypeOptimizable, 0, 0,
                    IntrinsicArrayIteratorPrototypeOptimizable),
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
    JS_FN("CreateAsyncFromSyncIterator", intrinsic_CreateAsyncFromSyncIterator,
          2, 0),
    JS_FN("CreateMapIterationResultPair",
          intrinsic_CreateMapIterationResultPair, 0, 0),
    JS_FN("CreateSetIterationResult", intrinsic_CreateSetIterationResult, 0, 0),
    JS_FN("DecompileArg", intrinsic_DecompileArg, 2, 0),
    JS_FN("DefineDataProperty", intrinsic_DefineDataProperty, 4, 0),
    JS_FN("DefineProperty", intrinsic_DefineProperty, 6, 0),
    JS_FN("DumpMessage", intrinsic_DumpMessage, 1, 0),
    JS_FN("FlatStringMatch", FlatStringMatch, 2, 0),
    JS_FN("FlatStringSearch", FlatStringSearch, 2, 0),
    JS_FN("GeneratorIsRunning", intrinsic_GeneratorIsRunning, 1, 0),
    JS_FN("GeneratorObjectIsClosed", intrinsic_GeneratorObjectIsClosed, 1, 0),
    JS_FN("GeneratorSetClosed", intrinsic_GeneratorSetClosed, 1, 0),
    JS_FN("GetElemBaseForLambda", intrinsic_GetElemBaseForLambda, 1, 0),
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
    JS_FN("IntrinsicAsyncGeneratorNext", AsyncGeneratorNext, 1, 0),
    JS_FN("IntrinsicAsyncGeneratorReturn", AsyncGeneratorReturn, 1, 0),
    JS_FN("IntrinsicAsyncGeneratorThrow", AsyncGeneratorThrow, 1, 0),
    JS_INLINABLE_FN("IsArray", intrinsic_IsArray, 1, 0, ArrayIsArray),
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
#ifdef ENABLE_RECORD_TUPLE
    JS_FN("IsTuple", intrinsic_IsTuple, 1, 0),
#endif
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
    JS_INLINABLE_FN("NewArrayIterator", intrinsic_NewArrayIterator, 0, 0,
                    IntrinsicNewArrayIterator),
    JS_FN("NewAsyncIteratorHelper", intrinsic_NewAsyncIteratorHelper, 0, 0),
    JS_FN("NewIteratorHelper", intrinsic_NewIteratorHelper, 0, 0),
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
    JS_INLINABLE_FN("RegExpBuiltinExec", intrinsic_RegExpBuiltinExec<false>, 2,
                    0, IntrinsicRegExpBuiltinExec),
    JS_INLINABLE_FN("RegExpBuiltinExecForTest",
                    intrinsic_RegExpBuiltinExec<true>, 2, 0,
                    IntrinsicRegExpBuiltinExecForTest),
    JS_FN("RegExpConstructRaw", regexp_construct_raw_flags, 2, 0),
    JS_FN("RegExpCreate", intrinsic_RegExpCreate, 2, 0),
    JS_INLINABLE_FN("RegExpExec", intrinsic_RegExpExec<false>, 2, 0,
                    IntrinsicRegExpExec),
    JS_INLINABLE_FN("RegExpExecForTest", intrinsic_RegExpExec<true>, 2, 0,
                    IntrinsicRegExpExecForTest),
    JS_FN("RegExpGetSubstitution", intrinsic_RegExpGetSubstitution, 5, 0),
    JS_INLINABLE_FN("RegExpHasCaptureGroups", intrinsic_RegExpHasCaptureGroups,
                    2, 0, RegExpHasCaptureGroups),
    JS_INLINABLE_FN("RegExpInstanceOptimizable", RegExpInstanceOptimizable, 1,
                    0, RegExpInstanceOptimizable),
    JS_INLINABLE_FN("RegExpMatcher", RegExpMatcher, 3, 0, RegExpMatcher),
    JS_INLINABLE_FN("RegExpPrototypeOptimizable", RegExpPrototypeOptimizable, 1,
                    0, RegExpPrototypeOptimizable),
    JS_INLINABLE_FN("RegExpSearcher", RegExpSearcher, 3, 0, RegExpSearcher),
    JS_INLINABLE_FN("RegExpSearcherLastLimit", RegExpSearcherLastLimit, 0, 0,
                    RegExpSearcherLastLimit),
    JS_FN("ReportUsageCounter", intrinsic_ReportUsageCounter, 2, 0),
    JS_INLINABLE_FN("SameValue", js::obj_is, 2, 0, ObjectIs),
    JS_FN("SetCopy", SetObject::copy, 1, 0),
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
#ifdef ENABLE_RECORD_TUPLE
    JS_FN("ThisTupleValue", intrinsic_ThisTupleValue, 1, 0),
#endif
    JS_FN("ThrowAggregateError", intrinsic_ThrowAggregateError, 4, 0),
    JS_FN("ThrowInternalError", intrinsic_ThrowInternalError, 4, 0),
    JS_FN("ThrowRangeError", intrinsic_ThrowRangeError, 4, 0),
    JS_FN("ThrowTypeError", intrinsic_ThrowTypeError, 4, 0),
    JS_FN("ToBigInt", intrinsic_ToBigInt, 1, 0),
    JS_INLINABLE_FN("ToInteger", intrinsic_ToInteger, 1, 0, IntrinsicToInteger),
    JS_INLINABLE_FN("ToLength", intrinsic_ToLength, 1, 0, IntrinsicToLength),
    JS_INLINABLE_FN("ToObject", intrinsic_ToObject, 1, 0, IntrinsicToObject),
    JS_FN("ToPropertyKey", intrinsic_ToPropertyKey, 1, 0),
    JS_FN("ToSource", intrinsic_ToSource, 1, 0),
#ifdef ENABLE_RECORD_TUPLE
    JS_FN("TupleLength", intrinsic_TupleLength, 1, 0),
#endif
    JS_FN("TypedArrayBitwiseSlice", intrinsic_TypedArrayBitwiseSlice, 4, 0),
    JS_FN("TypedArrayBuffer", intrinsic_TypedArrayBuffer, 1, 0),
    JS_INLINABLE_FN("TypedArrayByteOffset", intrinsic_TypedArrayByteOffset, 1,
                    0, IntrinsicTypedArrayByteOffset),
    JS_INLINABLE_FN("TypedArrayElementSize", intrinsic_TypedArrayElementSize, 1,
                    0, IntrinsicTypedArrayElementSize),
    JS_FN("TypedArrayInitFromPackedArray",
          intrinsic_TypedArrayInitFromPackedArray, 2, 0),
    JS_FN("TypedArrayIsAutoLength", intrinsic_TypedArrayIsAutoLength, 1, 0),
    JS_INLINABLE_FN("TypedArrayLength", intrinsic_TypedArrayLength, 1, 0,
                    IntrinsicTypedArrayLength),
    JS_INLINABLE_FN("TypedArrayLengthZeroOnOutOfBounds",
                    intrinsic_TypedArrayLengthZeroOnOutOfBounds, 1, 0,
                    IntrinsicTypedArrayLengthZeroOnOutOfBounds),
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
    JS_FN("intl_CallSegmentIteratorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<SegmentIteratorObject>>, 2, 0),
    JS_FN("intl_CallSegmenterMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<SegmenterObject>>, 2, 0),
    JS_FN("intl_CallSegmentsMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<SegmentsObject>>, 2, 0),
    JS_FN("intl_Collator", intl_Collator, 2, 0),
    JS_FN("intl_CompareStrings", intl_CompareStrings, 3, 0),
    JS_FN("intl_ComputeDisplayName", intl_ComputeDisplayName, 6, 0),
    JS_FN("intl_CreateDateTimeFormat", intl_CreateDateTimeFormat, 4, 0),
    JS_FN("intl_CreateSegmentIterator", intl_CreateSegmentIterator, 1, 0),
    JS_FN("intl_CreateSegmentsObject", intl_CreateSegmentsObject, 2, 0),
    JS_FN("intl_FindNextSegmentBoundaries", intl_FindNextSegmentBoundaries, 1,
          0),
    JS_FN("intl_FindSegmentBoundaries", intl_FindSegmentBoundaries, 2, 0),
    JS_FN("intl_FormatDateTime", intl_FormatDateTime, 2, 0),
    JS_FN("intl_FormatDateTimeRange", intl_FormatDateTimeRange, 4, 0),
    JS_FN("intl_FormatList", intl_FormatList, 3, 0),
    JS_FN("intl_FormatNumber", intl_FormatNumber, 3, 0),
    JS_FN("intl_FormatNumberRange", intl_FormatNumberRange, 4, 0),
    JS_FN("intl_FormatRelativeTime", intl_FormatRelativeTime, 4, 0),
    JS_FN("intl_GetCalendarInfo", intl_GetCalendarInfo, 1, 0),
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
    JS_INLINABLE_FN("intl_GuardToSegmentIterator",
                    intrinsic_GuardToBuiltin<SegmentIteratorObject>, 1, 0,
                    IntlGuardToSegmentIterator),
    JS_INLINABLE_FN("intl_GuardToSegmenter",
                    intrinsic_GuardToBuiltin<SegmenterObject>, 1, 0,
                    IntlGuardToSegmenter),
    JS_INLINABLE_FN("intl_GuardToSegments",
                    intrinsic_GuardToBuiltin<SegmentsObject>, 1, 0,
                    IntlGuardToSegments),
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
    JS_FN("intl_SelectPluralRuleRange", intl_SelectPluralRuleRange, 3, 0),
    JS_FN("intl_SupportedValuesOf", intl_SupportedValuesOf, 1, 0),
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
    JS_FN("intl_isIgnorePunctuation", intl_isIgnorePunctuation, 1, 0),
    JS_FN("intl_isUpperCaseFirst", intl_isUpperCaseFirst, 1, 0),
    JS_FN("intl_numberingSystem", intl_numberingSystem, 1, 0),
    JS_FN("intl_resolveDateTimeFormatComponents",
          intl_resolveDateTimeFormatComponents, 3, 0),
    JS_FN("intl_supportedLocaleOrFallback", intl_supportedLocaleOrFallback, 1,
          0),
    JS_FN("intl_toLocaleLowerCase", intl_toLocaleLowerCase, 2, 0),
    JS_FN("intl_toLocaleUpperCase", intl_toLocaleUpperCase, 2, 0),
#endif  // JS_HAS_INTL_API

    // Standard builtins used by self-hosting.
    JS_FN("new_List", intrinsic_newList, 0, 0),
    JS_INLINABLE_FN("std_Array", array_construct, 1, 0, Array),
    JS_FN("std_Array_includes", array_includes, 1, 0),
    JS_FN("std_Array_indexOf", array_indexOf, 1, 0),
    JS_FN("std_Array_lastIndexOf", array_lastIndexOf, 1, 0),
    JS_INLINABLE_FN("std_Array_pop", array_pop, 0, 0, ArrayPop),
    JS_TRAMPOLINE_FN("std_Array_sort", array_sort, 1, 0, ArraySort),
    JS_FN("std_BigInt_valueOf", BigIntObject::valueOf, 0, 0),
    JS_FN("std_Date_now", date_now, 0, 0),
    JS_FN("std_Function_apply", fun_apply, 2, 0),
    JS_FN("std_Map_entries", MapObject::entries, 0, 0),
    JS_FN("std_Map_get", MapObject::get, 1, 0),
    JS_FN("std_Map_set", MapObject::set, 2, 0),
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
    JS_FN("std_Set_add", SetObject::add, 1, 0),
    JS_FN("std_Set_delete", SetObject::delete_, 1, 0),
    JS_INLINABLE_FN("std_Set_has", SetObject::has, 1, 0, SetHas),
    JS_INLINABLE_FN("std_Set_size", SetObject::size, 1, 0, SetSize),
    JS_FN("std_Set_values", SetObject::values, 0, 0),
    JS_INLINABLE_FN("std_String_charCodeAt", str_charCodeAt, 1, 0,
                    StringCharCodeAt),
    JS_INLINABLE_FN("std_String_codePointAt", str_codePointAt, 1, 0,
                    StringCodePointAt),
    JS_INLINABLE_FN("std_String_endsWith", str_endsWith, 1, 0, StringEndsWith),
    JS_INLINABLE_FN("std_String_fromCharCode", str_fromCharCode, 1, 0,
                    StringFromCharCode),
    JS_INLINABLE_FN("std_String_fromCodePoint", str_fromCodePoint, 1, 0,
                    StringFromCodePoint),
    JS_FN("std_String_includes", str_includes, 1, 0),
    JS_INLINABLE_FN("std_String_indexOf", str_indexOf, 1, 0, StringIndexOf),
    JS_INLINABLE_FN("std_String_startsWith", str_startsWith, 1, 0,
                    StringStartsWith),
#ifdef ENABLE_RECORD_TUPLE
    JS_FN("std_Tuple_unchecked", tuple_construct, 1, 0),
#endif
    JS_TRAMPOLINE_FN("std_TypedArray_sort", TypedArrayObject::sort, 1, 0,
                     TypedArraySort),

    JS_FS_END};

#ifdef DEBUG

static void CheckSelfHostedIntrinsics() {
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
}

class CheckTenuredTracer : public JS::CallbackTracer {
  HashSet<gc::Cell*, DefaultHasher<gc::Cell*>, SystemAllocPolicy> visited;
  Vector<JS::GCCellPtr, 0, SystemAllocPolicy> stack;

 public:
  explicit CheckTenuredTracer(JSRuntime* rt) : JS::CallbackTracer(rt) {}
  void check() {
    while (!stack.empty()) {
      JS::TraceChildren(this, stack.popCopy());
    }
  }
  void onChild(JS::GCCellPtr thing, const char* name) override {
    gc::Cell* cell = thing.asCell();
    MOZ_RELEASE_ASSERT(cell->isTenured(), "Expected tenured cell");
    if (!visited.has(cell)) {
      if (!visited.put(cell) || !stack.append(thing)) {
        // Ignore OOM. This can happen during fuzzing.
        return;
      }
    }
  }
};

static void CheckSelfHostingDataIsTenured(JSRuntime* rt) {
  // Check everything is tenured as we don't trace it when collecting the
  // nursery.
  CheckTenuredTracer trc(rt);
  rt->traceSelfHostingStencil(&trc);
  trc.check();
}

#endif

const JSFunctionSpec* js::FindIntrinsicSpec(js::PropertyName* name) {
  size_t limit = std::size(intrinsic_functions) - 1;
  MOZ_ASSERT(!intrinsic_functions[limit].name);

  MOZ_ASSERT(name->hasLatin1Chars());

  JS::AutoCheckCannotGC nogc;
  const char* chars = reinterpret_cast<const char*>(name->latin1Chars(nogc));
  size_t len = name->length();

  // NOTE: CheckSelfHostedIntrinsics checks that the intrinsic_functions list is
  // sorted appropriately so that we can use binary search here.

  size_t loc = 0;
  bool match = mozilla::BinarySearchIf(
      intrinsic_functions, 0, limit,
      [chars, len](const JSFunctionSpec& spec) {
        // The spec string is null terminated but the `name` string is not, so
        // compare chars up until the length of `name`. Since the `name` string
        // does not contain any nulls, seeing the null terminator of the spec
        // string will terminate the loop appropriately. A final comparison
        // against null is needed to determine if the spec string has an extra
        // suffix.
        const char* spec_chars = spec.name.string();
        for (size_t i = 0; i < len; ++i) {
          if (auto cmp_result = int(chars[i]) - int(spec_chars[i])) {
            return cmp_result;
          }
        }
        return int('\0') - int(spec_chars[len]);
      },
      &loc);
  if (match) {
    return &intrinsic_functions[loc];
  }
  return nullptr;
}

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

// Report all errors and warnings to stderr because it is too early in the
// startup process for any other error reporting to be used, and we don't want
// errors in self-hosted code to be silently swallowed.
class MOZ_STACK_CLASS AutoPrintSelfHostingFrontendContext
    : public FrontendContext {
  JSContext* cx_;

 public:
  explicit AutoPrintSelfHostingFrontendContext(JSContext* cx) : cx_(cx) {
    setCurrentJSContext(cx_);
  }
  ~AutoPrintSelfHostingFrontendContext() {
    // TODO: Remove this once JSContext is removed from frontend.
    MaybePrintAndClearPendingException(cx_);

    if (hadOutOfMemory()) {
      fprintf(stderr, "Out of memory\n");
    }

    if (maybeError()) {
      JS::PrintError(stderr, &*maybeError(), true);
    }
    for (CompileError& error : warnings()) {
      JS::PrintError(stderr, &error, true);
    }
    if (hadOverRecursed()) {
      fprintf(stderr, "Over recursed\n");
    }
    if (hadAllocationOverflow()) {
      fprintf(stderr, "Allocation overflow\n");
    }
  }
};

[[nodiscard]] static bool InitSelfHostingFromStencil(
    JSContext* cx, frontend::CompilationAtomCache& atomCache,
    const frontend::CompilationStencil& stencil) {
  // Build the JSAtom -> ScriptIndexRange mapping and save on the runtime.
  {
    auto& scriptMap = cx->runtime()->selfHostScriptMap.ref();

    // We don't easily know the number of top-level functions, so use the total
    // number of stencil functions instead. There is very little nesting of
    // functions in self-hosted code so this is a good approximation.
    size_t numSelfHostedScripts = stencil.scriptData.size();
    if (!scriptMap.reserve(numSelfHostedScripts)) {
      ReportOutOfMemory(cx);
      return false;
    }

    auto topLevelThings =
        stencil.scriptData[frontend::CompilationStencil::TopLevelIndex]
            .gcthings(stencil);

    // Iterate over the (named) top-level functions. We record the ScriptIndex
    // as well as the ScriptIndex of the next top-level function. Scripts
    // between these two indices are the inner functions of the first one. We
    // only record named scripts here since they are what might be looked up.
    Rooted<JSAtom*> prevAtom(cx);
    frontend::ScriptIndex prevIndex;
    for (frontend::TaggedScriptThingIndex thing : topLevelThings) {
      if (!thing.isFunction()) {
        continue;
      }

      frontend::ScriptIndex index = thing.toFunction();
      const auto& script = stencil.scriptData[index];

      if (prevAtom) {
        frontend::ScriptIndexRange range{prevIndex, index};
        scriptMap.putNewInfallible(prevAtom, range);
      }

      prevAtom = script.functionAtom
                     ? atomCache.getExistingAtomAt(cx, script.functionAtom)
                     : nullptr;
      prevIndex = index;
    }
    if (prevAtom) {
      frontend::ScriptIndexRange range{
          prevIndex, frontend::ScriptIndex(stencil.scriptData.size())};
      scriptMap.putNewInfallible(prevAtom, range);
    }

    // We over-estimated the capacity of `scriptMap`, so check that the estimate
    // hasn't drifted too hasn't drifted too far since this was written. If this
    // assert fails, we may need a new way to size the `scriptMap`.
    MOZ_ASSERT(numSelfHostedScripts < (scriptMap.count() * 1.15));
  }

#ifdef DEBUG
  // Check that the list of intrinsics is well-formed.
  CheckSelfHostedIntrinsics();
  CheckSelfHostingDataIsTenured(cx->runtime());
#endif

  return true;
}

bool JSRuntime::initSelfHostingStencil(JSContext* cx,
                                       JS::SelfHostedCache xdrCache,
                                       JS::SelfHostedWriter xdrWriter) {
  if (parentRuntime) {
    MOZ_RELEASE_ASSERT(
        parentRuntime->hasInitializedSelfHosting(),
        "Parent runtime must initialize self-hosting before workers");

    selfHostStencilInput_ = parentRuntime->selfHostStencilInput_;
    selfHostStencil_ = parentRuntime->selfHostStencil_;
    return true;
  }

  // Variables used to instantiate scripts.
  CompileOptions options(cx);
  FillSelfHostingCompileOptions(options);

  // Try initializing from Stencil XDR.
  bool decodeOk = false;
  AutoPrintSelfHostingFrontendContext fc(cx);
  if (xdrCache.Length() > 0) {
    // Allow the VM to directly use bytecode from the XDR buffer without
    // copying it. The buffer must outlive all runtimes (including workers).
    options.borrowBuffer = true;
    options.usePinnedBytecode = true;

    Rooted<UniquePtr<frontend::CompilationInput>> input(
        cx, cx->new_<frontend::CompilationInput>(options));
    if (!input) {
      return false;
    }
    {
      AutoReportFrontendContext fc(cx);
      if (!input->initForSelfHostingGlobal(&fc)) {
        return false;
      }
    }

    RefPtr<frontend::CompilationStencil> stencil(
        cx->new_<frontend::CompilationStencil>(input->source));
    if (!stencil) {
      return false;
    }
    if (!stencil->deserializeStencils(&fc, options, xdrCache, &decodeOk)) {
      return false;
    }

    if (decodeOk) {
      MOZ_ASSERT(input->atomCache.empty());

      MOZ_ASSERT(!hasSelfHostStencil());

      // Move it to the runtime.
      setSelfHostingStencil(&input, std::move(stencil));

      return true;
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

  Rooted<UniquePtr<frontend::CompilationInput>> input(
      cx, cx->new_<frontend::CompilationInput>(options));
  if (!input) {
    return false;
  }
  frontend::NoScopeBindingCache scopeCache;
  RefPtr<frontend::CompilationStencil> stencil =
      frontend::CompileGlobalScriptToStencil(cx, &fc, cx->tempLifoAlloc(),
                                             *input, &scopeCache, srcBuf,
                                             ScopeKind::Global);
  if (!stencil) {
    return false;
  }

  // Serialize the stencil to XDR.
  if (xdrWriter) {
    JS::TranscodeBuffer xdrBuffer;
    bool succeeded = false;
    if (!stencil->serializeStencils(cx, *input, xdrBuffer, &succeeded)) {
      return false;
    }
    if (!succeeded) {
      JS_ReportErrorASCII(cx, "Encoding failure");
      return false;
    }

    if (!xdrWriter(cx, xdrBuffer)) {
      return false;
    }
  }

  MOZ_ASSERT(input->atomCache.empty());

  MOZ_ASSERT(!hasSelfHostStencil());

  // Move it to the runtime.
  setSelfHostingStencil(&input, std::move(stencil));

  return true;
}

void JSRuntime::setSelfHostingStencil(
    MutableHandle<UniquePtr<frontend::CompilationInput>> input,
    RefPtr<frontend::CompilationStencil>&& stencil) {
  MOZ_ASSERT(!selfHostStencilInput_);
  MOZ_ASSERT(!selfHostStencil_);

  selfHostStencilInput_ = input.release();
  selfHostStencil_ = stencil.forget().take();

#ifdef DEBUG
  CheckSelfHostingDataIsTenured(this);
#endif
}

bool JSRuntime::initSelfHostingFromStencil(JSContext* cx) {
  return InitSelfHostingFromStencil(
      cx, cx->runtime()->selfHostStencilInput_->atomCache,
      *cx->runtime()->selfHostStencil_);
}

void JSRuntime::finishSelfHosting() {
  if (!parentRuntime) {
    js_delete(selfHostStencilInput_.ref());
    if (selfHostStencil_) {
      // delete selfHostStencil_ by decrementing the ref-count of the last
      // instance.
      RefPtr<frontend::CompilationStencil> stencil;
      *getter_AddRefs(stencil) = selfHostStencil_;
      MOZ_ASSERT(stencil->refCount == 1);
    }
  }

  selfHostStencilInput_ = nullptr;
  selfHostStencil_ = nullptr;

  selfHostScriptMap.ref().clear();
}

void JSRuntime::traceSelfHostingStencil(JSTracer* trc) {
  if (selfHostStencilInput_.ref()) {
    selfHostStencilInput_->trace(trc);
  }
  selfHostScriptMap.ref().trace(trc);
}

GeneratorKind JSRuntime::getSelfHostedFunctionGeneratorKind(
    js::PropertyName* name) {
  frontend::ScriptIndex index = getSelfHostedScriptIndexRange(name)->start;
  auto flags = selfHostStencil().scriptExtra[index].immutableFlags;
  return flags.hasFlag(js::ImmutableScriptFlagsEnum::IsGenerator)
             ? GeneratorKind::Generator
             : GeneratorKind::NotGenerator;
}

// Returns the ScriptSourceObject to use for cloned self-hosted scripts in the
// current realm.
ScriptSourceObject* js::SelfHostingScriptSourceObject(JSContext* cx) {
  return GlobalObject::getOrCreateSelfHostingScriptSourceObject(cx,
                                                                cx->global());
}

/* static */
ScriptSourceObject* GlobalObject::getOrCreateSelfHostingScriptSourceObject(
    JSContext* cx, Handle<GlobalObject*> global) {
  MOZ_ASSERT(cx->global() == global);

  if (ScriptSourceObject* sso = global->data().selfHostingScriptSource) {
    return sso;
  }

  CompileOptions options(cx);
  FillSelfHostingCompileOptions(options);

  RefPtr<ScriptSource> source(cx->new_<ScriptSource>());
  if (!source) {
    return nullptr;
  }

  Rooted<ScriptSourceObject*> sourceObject(cx);
  {
    AutoReportFrontendContext fc(cx);
    if (!source->initFromOptions(&fc, options)) {
      return nullptr;
    }

    sourceObject = ScriptSourceObject::create(cx, source.get());
    if (!sourceObject) {
      return nullptr;
    }

    JS::InstantiateOptions instantiateOptions(options);
    if (!ScriptSourceObject::initFromOptions(cx, sourceObject,
                                             instantiateOptions)) {
      return nullptr;
    }

    global->data().selfHostingScriptSource.init(sourceObject);
  }

  return sourceObject;
}

bool JSRuntime::delazifySelfHostedFunction(JSContext* cx,
                                           Handle<PropertyName*> name,
                                           HandleFunction targetFun) {
  MOZ_ASSERT(targetFun->isExtended());
  MOZ_ASSERT(targetFun->hasSelfHostedLazyScript());

  auto indexRange = *getSelfHostedScriptIndexRange(name);
  auto& stencil = cx->runtime()->selfHostStencil();

  if (!stencil.delazifySelfHostedFunction(
          cx, cx->runtime()->selfHostStencilInput().atomCache, indexRange,
          targetFun)) {
    return false;
  }

  // Relazifiable self-hosted functions may be relazified later into a
  // SelfHostedLazyScript, dropping the BaseScript entirely. This only applies
  // to named function being delazified. Inner functions used by self-hosting
  // are never relazified.
  BaseScript* targetScript = targetFun->baseScript();
  if (targetScript->isRelazifiable()) {
    targetScript->setAllowRelazify();
  }

  return true;
}

mozilla::Maybe<frontend::ScriptIndexRange>
JSRuntime::getSelfHostedScriptIndexRange(js::PropertyName* name) {
  if (parentRuntime) {
    return parentRuntime->getSelfHostedScriptIndexRange(name);
  }
  MOZ_ASSERT(name->isPermanentAndMayBeShared());
  if (auto ptr = selfHostScriptMap.ref().readonlyThreadsafeLookup(name)) {
    return mozilla::Some(ptr->value());
  }
  return mozilla::Nothing();
}

static bool GetComputedIntrinsic(JSContext* cx, Handle<PropertyName*> name,
                                 MutableHandleValue vp) {
  // If the intrinsic was not in hardcoded set, run the top-level of the
  // selfhosted script. This will generate values and call `SetIntrinsic` to
  // save them on a special "computed intrinsics holder". We then can check for
  // our required values and cache on the normal intrinsics holder.

  Rooted<NativeObject*> computedIntrinsicsHolder(
      cx, cx->global()->getComputedIntrinsicsHolder());
  if (!computedIntrinsicsHolder) {
    auto computedIntrinsicHolderGuard = mozilla::MakeScopeExit(
        [cx]() { cx->global()->setComputedIntrinsicsHolder(nullptr); });

    // Instantiate a script in current realm from the shared Stencil.
    JSRuntime* runtime = cx->runtime();
    RootedScript script(
        cx, runtime->selfHostStencil().instantiateSelfHostedTopLevelForRealm(
                cx, runtime->selfHostStencilInput()));
    if (!script) {
      return false;
    }

    // Attach the computed intrinsics holder to the global now to capture
    // generated values.
    computedIntrinsicsHolder =
        NewPlainObjectWithProto(cx, nullptr, TenuredObject);
    if (!computedIntrinsicsHolder) {
      return false;
    }
    cx->global()->setComputedIntrinsicsHolder(computedIntrinsicsHolder);

    // Disable the interrupt callback while executing the top-level script.
    // This prevents recursive calls to GetComputedIntrinsic through the
    // interrupt callback.
    bool hadInterruptsDisabled = JS_DisableInterruptCallback(cx);
    auto resetInterrupts = mozilla::MakeScopeExit(
        [&]() { JS_ResetInterruptCallback(cx, hadInterruptsDisabled); });

    // Attempt to execute the top-level script. If they fails to run to
    // successful completion, throw away the holder to avoid a partial
    // initialization state.
    if (!JS_ExecuteScript(cx, script)) {
      return false;
    }

    // Successfully ran the self-host top-level in current realm, so these
    // computed intrinsic values are now source of truth for the realm.
    computedIntrinsicHolderGuard.release();
  }

  // Cache the individual intrinsic on the standard holder object so that we
  // only have to look for it in one place when performing `GetIntrinsic`.
  mozilla::Maybe<PropertyInfo> prop =
      computedIntrinsicsHolder->lookup(cx, name);
  MOZ_RELEASE_ASSERT(prop, "SelfHosted intrinsic not found");
  RootedValue value(cx, computedIntrinsicsHolder->getSlot(prop->slot()));
  return GlobalObject::addIntrinsicValue(cx, cx->global(), name, value);
}

bool JSRuntime::getSelfHostedValue(JSContext* cx, Handle<PropertyName*> name,
                                   MutableHandleValue vp) {
  // If the self-hosted value we want is a function in the stencil, instantiate
  // a lazy self-hosted function for it. This is typical when a self-hosted
  // function calls other self-hosted helper functions.
  if (auto index = getSelfHostedScriptIndexRange(name)) {
    JSFunction* fun =
        cx->runtime()->selfHostStencil().instantiateSelfHostedLazyFunction(
            cx, cx->runtime()->selfHostStencilInput().atomCache, index->start,
            name);
    if (!fun) {
      return false;
    }
    vp.setObject(*fun);
    return true;
  }

  return GetComputedIntrinsic(cx, name, vp);
}

void JSRuntime::assertSelfHostedFunctionHasCanonicalName(
    Handle<PropertyName*> name) {
#ifdef DEBUG
  frontend::ScriptIndex index = getSelfHostedScriptIndexRange(name)->start;
  MOZ_ASSERT(selfHostStencil().scriptData[index].hasSelfHostedCanonicalName());
#endif
}

bool js::IsSelfHostedFunctionWithName(JSFunction* fun, JSAtom* name) {
  return fun->isSelfHostedBuiltin() && fun->isExtended() &&
         GetClonedSelfHostedFunctionName(fun) == name;
}

bool js::IsSelfHostedFunctionWithName(const Value& v, JSAtom* name) {
  if (!v.isObject() || !v.toObject().is<JSFunction>()) {
    return false;
  }
  JSFunction* fun = &v.toObject().as<JSFunction>();
  return IsSelfHostedFunctionWithName(fun, name);
}

// MG:XXX: Need to adopt a solid policy around CCW constructors.
// -- Going to default to "different realm don't matter".
bool js::ReportUsageCounter(JSContext* cx, HandleObject constructorArg,
                            int32_t builtin, int32_t type) {
  RootedObject constructor(cx, constructorArg);

  // Do this here rather than as part of the self-hosted code.
  if (builtin == SUBCLASSING_DETERMINE_THROUGH_CONSTRUCTOR) {
    MOZ_ASSERT(constructor);

    do {
      if (IsPromiseConstructor(constructor)) {
        builtin = SUBCLASSING_PROMISE;
        break;
      }
      if (IsTypedArrayConstructor(constructor)) {
        builtin = SUBCLASSING_TYPEDARRAY;
        break;
      }
      if (IsArrayConstructor(constructor)) {
        builtin = SUBCLASSING_ARRAY;
        break;
      }
      if (IsCrossCompartmentWrapper(constructor)) {
        // Bail on reporting CCWs.
        return true;
      }

      Rooted<GlobalObject*> global(cx, &constructor->nonCCWGlobal());
      RootedObject abConstructor(
          cx, GlobalObject::getOrCreateArrayBufferConstructor(cx, global));
      if (!abConstructor) {
        return false;
      }
      if (constructor == abConstructor) {
        builtin = SUBCLASSING_ARRAYBUFFER;
        break;
      }

      RootedObject sabConstructor(
          cx,
          GlobalObject::getOrCreateSharedArrayBufferConstructor(cx, global));
      if (!sabConstructor) {
        return false;
      }
      if (constructor == sabConstructor) {
        builtin = SUBCLASSING_SHAREDARRAYBUFFER;
        break;
      }

      RootedObject regExpConstructor(
          cx, GlobalObject::getOrCreateRegExpConstructor(cx, global));
      if (!regExpConstructor) {
        return false;
      }
      if (constructor == regExpConstructor) {
        builtin = SUBCLASSING_REGEXP;
        break;
      }

    } while (false);

    // We should have determined the constructor here.
    if (builtin == SUBCLASSING_DETERMINE_THROUGH_CONSTRUCTOR) {
      MOZ_CRASH("Unable to determine constructor where expected.");
      return true;
    }
    // We -do- want to report here, because we've determined exterior to this
    // that we're in a subclassing scenario; so don't do the check for a
    // matching constructor here.
    constructor = nullptr;
  }

  switch (builtin) {
    case SUBCLASSING_ARRAY: {
      // Check if the provided function is actually the array constructor
      // anyhow; Constructor may be nullptr if check has already been done.
      if (constructor && IsArrayConstructor(constructor)) {
        return true;
      }
      switch (type) {
        case SUBCLASSING_TYPE_II:
          cx->runtime()->setUseCounter(cx->global(),
                                       JSUseCounter::SUBCLASSING_ARRAY_TYPE_II);
          return true;
        case SUBCLASSING_TYPE_III:
          cx->runtime()->setUseCounter(
              cx->global(), JSUseCounter::SUBCLASSING_ARRAY_TYPE_III);
          return true;
        default:
          MOZ_CRASH("Unexpected Subclassing Type");
      }
    }
    case SUBCLASSING_PROMISE: {
      if (constructor && IsPromiseConstructor(constructor)) {
        return true;
      }
      switch (type) {
        case SUBCLASSING_TYPE_II:
          cx->runtime()->setUseCounter(
              cx->global(), JSUseCounter::SUBCLASSING_PROMISE_TYPE_II);
          return true;
        case SUBCLASSING_TYPE_III:
          cx->runtime()->setUseCounter(
              cx->global(), JSUseCounter::SUBCLASSING_PROMISE_TYPE_III);
          return true;
        default:
          MOZ_CRASH("Unexpected Subclassing Type");
      }
    }
    case SUBCLASSING_TYPEDARRAY: {
      // So here's a design question: Do we ultimately care about
      // matching typed arrays? i.e. if someone does
      // UInt8Array.from.call(Int8Array, 0), should this count
      // as subclassing?
      //
      // I'm inclined to say not at the moment -- but it would
      // be a behaviour change if we removed subclassing.
      if (constructor && IsTypedArrayConstructor(constructor)) {
        return true;
      }
      switch (type) {
        case SUBCLASSING_TYPE_II:
          cx->runtime()->setUseCounter(
              cx->global(), JSUseCounter::SUBCLASSING_TYPEDARRAY_TYPE_II);
          return true;
        case SUBCLASSING_TYPE_III:
          cx->runtime()->setUseCounter(
              cx->global(), JSUseCounter::SUBCLASSING_TYPEDARRAY_TYPE_III);
          return true;
        default:
          MOZ_CRASH("Unhandled subclassing type");
      }
    }
    case SUBCLASSING_ARRAYBUFFER:
    case SUBCLASSING_SHAREDARRAYBUFFER: {
      MOZ_ASSERT(type == SUBCLASSING_TYPE_III);
      cx->runtime()->setUseCounter(
          cx->global(),
          builtin == SUBCLASSING_ARRAYBUFFER
              ? JSUseCounter::SUBCLASSING_ARRAYBUFFER_TYPE_III
              : JSUseCounter::SUBCLASSING_SHAREDARRAYBUFFER_TYPE_III);
      return true;
    }
    case SUBCLASSING_REGEXP: {
      switch (type) {
        case SUBCLASSING_TYPE_III:
          cx->runtime()->setUseCounter(
              cx->global(), JSUseCounter::SUBCLASSING_REGEXP_TYPE_III);
          return true;
        case SUBCLASSING_TYPE_IV:
          cx->runtime()->setUseCounter(
              cx->global(), JSUseCounter::SUBCLASSING_REGEXP_TYPE_IV);
          return true;
        default:
          MOZ_CRASH("Unexpected RegExp Subclassing Type");
      }
    }
    default:
      MOZ_CRASH("Unexpected builtin");
  };
}

// In the interests of efficiency and simplicity, we would like this function
// to have to do as little as possible, and take as few parameters as possible.
//
// Nevethreless, we also wish to be able to report correctly for interesting
// cases like Array.from.call(Map, ...) -- essentially, catching the case where
// someone using a subclassing-elgible class as the 'this' value, thereby
// executing a sub classing.
//
// To handle this with just two parameters, we treat our integer parameter as a
// packed integer; This introduces some magic, but allows us to communicate all
// call-site constant data in a single int32.
//`
// The packing is as follows:
//
// SubclassingElgibleBuiltins << 16 | SubclassingType
//
// This produces the following magic constant values:
//
// Array Subclassing   Type II:  (1 << 16) | 2 == 0x010002
// Array Subclassing   Type III: (1 << 16) | 3 == 0x010003
// Array Subclassing   Type IV:  (1 << 16) | 4 == 0x010000
//
// Promise Subclassing Type II:  (2 << 16) | 2 == 0x020002
// Promise Subclassing Type III: (2 << 16) | 3 == 0x020003
// Promise Subclassing Type IV:  (2 << 16) | 4 == 0x020004
//
// ... etc.
//
// Subclassing is reported iff the constructor provided doesn't match
// the existing prototype.
//
bool js::intrinsic_ReportUsageCounter(JSContext* cx, unsigned int argc,
                                      JS::Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  // Currently usage counter is only used for reporting subclassing
  // as a result we hyper-specialize the following
  MOZ_ASSERT(args.length() == 2);
  MOZ_ASSERT(args.get(0).isObject());
  MOZ_ASSERT(args.get(1).isInt32());

  HandleValue arg0 = args.get(0);
  RootedObject constructor(cx, &arg0.toObject());

  int32_t packedTypeAndBuiltin = args.get(1).toInt32();

  int32_t type = packedTypeAndBuiltin & SUBCLASSING_TYPE_MASK;
  MOZ_ASSERT(type >= SUBCLASSING_TYPE_II && type <= SUBCLASSING_TYPE_IV);

  int32_t builtin = packedTypeAndBuiltin >> SUBCLASSING_BUILTIN_SHIFT;
  MOZ_ASSERT(builtin >= 0 && builtin < SUBCLASSING_LAST_BUILTIN);

  return ReportUsageCounter(cx, constructor, builtin, type);
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
