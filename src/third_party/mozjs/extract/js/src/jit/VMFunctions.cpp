/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/VMFunctions.h"

#include "mozilla/FloatingPoint.h"

#include "builtin/MapObject.h"
#include "builtin/String.h"
#include "ds/OrderedHashTable.h"
#include "gc/Cell.h"
#include "gc/GC.h"
#include "jit/arm/Simulator-arm.h"
#include "jit/AtomicOperations.h"
#include "jit/BaselineIC.h"
#include "jit/CalleeToken.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/mips32/Simulator-mips32.h"
#include "jit/mips64/Simulator-mips64.h"
#include "jit/Simulator.h"
#include "js/experimental/JitInfo.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/StackLimits.h"    // js::AutoCheckRecursionLimit
#include "js/friend/WindowProxy.h"    // js::IsWindow
#include "js/Printf.h"
#include "js/TraceKind.h"
#include "proxy/ScriptedProxyHandler.h"
#include "util/Unicode.h"
#include "vm/ArrayObject.h"
#include "vm/Compartment.h"
#include "vm/Interpreter.h"
#include "vm/JSAtomUtils.h"  // AtomizeString
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/SelfHosting.h"
#include "vm/StaticStrings.h"
#include "vm/TypedArrayObject.h"
#include "vm/TypeofEqOperand.h"  // TypeofEqOperand
#include "vm/Watchtower.h"
#include "wasm/WasmGcObject.h"

#include "debugger/DebugAPI-inl.h"
#include "jit/BaselineFrame-inl.h"
#include "jit/VMFunctionList-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/JSAtomUtils-inl.h"  // TypeName
#include "vm/JSContext-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/PlainObject-inl.h"  // js::CreateThis
#include "vm/StringObject-inl.h"

using namespace js;
using namespace js::jit;

namespace js {

class ArgumentsObject;
class NamedLambdaObject;
class AsyncFunctionGeneratorObject;
class RegExpObject;

namespace jit {

struct IonOsrTempData;

struct PopValues {
  uint8_t numValues;
  explicit constexpr PopValues(uint8_t numValues = 0) : numValues(numValues) {}
};

template <class>
struct ReturnTypeToDataType { /* Unexpected return type for a VMFunction. */
};
template <>
struct ReturnTypeToDataType<void> {
  static const DataType result = Type_Void;
};
template <>
struct ReturnTypeToDataType<bool> {
  static const DataType result = Type_Bool;
};
template <class T>
struct ReturnTypeToDataType<T*> {
  // Assume by default that any pointer return types are cells.
  static_assert(std::is_base_of_v<gc::Cell, T>);

  static const DataType result = Type_Cell;
};

// Convert argument types to properties of the argument known by the jit.
template <class T>
struct TypeToArgProperties {
  static const uint32_t result =
      (sizeof(T) <= sizeof(void*) ? VMFunctionData::Word
                                  : VMFunctionData::Double);
};
template <>
struct TypeToArgProperties<const Value&> {
  static const uint32_t result =
      TypeToArgProperties<Value>::result | VMFunctionData::ByRef;
};
template <>
struct TypeToArgProperties<HandleValue> {
  static const uint32_t result =
      TypeToArgProperties<Value>::result | VMFunctionData::ByRef;
};
template <>
struct TypeToArgProperties<MutableHandleValue> {
  static const uint32_t result =
      TypeToArgProperties<Value>::result | VMFunctionData::ByRef;
};
template <>
struct TypeToArgProperties<HandleId> {
  static const uint32_t result =
      TypeToArgProperties<jsid>::result | VMFunctionData::ByRef;
};
template <class T>
struct TypeToArgProperties<Handle<T*>> {
  // Assume by default that any pointer handle types are cells.
  static_assert(std::is_base_of_v<gc::Cell, T>);

  static const uint32_t result =
      TypeToArgProperties<T*>::result | VMFunctionData::ByRef;
};
template <class T>
struct TypeToArgProperties<Handle<T>> {
  // Fail for Handle types that aren't specialized above.
};

// Convert argument type to whether or not it should be passed in a float
// register on platforms that have them, like x64.
template <class T>
struct TypeToPassInFloatReg {
  static const uint32_t result = 0;
};
template <>
struct TypeToPassInFloatReg<double> {
  static const uint32_t result = 1;
};

// Convert argument types to root types used by the gc, see TraceJitExitFrame.
template <class T>
struct TypeToRootType {
  static const uint32_t result = VMFunctionData::RootNone;
};
template <>
struct TypeToRootType<HandleValue> {
  static const uint32_t result = VMFunctionData::RootValue;
};
template <>
struct TypeToRootType<MutableHandleValue> {
  static const uint32_t result = VMFunctionData::RootValue;
};
template <>
struct TypeToRootType<HandleId> {
  static const uint32_t result = VMFunctionData::RootId;
};
template <class T>
struct TypeToRootType<Handle<T*>> {
  // Assume by default that any pointer types are cells.
  static_assert(std::is_base_of_v<gc::Cell, T>);

  static constexpr uint32_t rootType() {
    using JS::TraceKind;

    switch (JS::MapTypeToTraceKind<T>::kind) {
      case TraceKind::Object:
        return VMFunctionData::RootObject;
      case TraceKind::BigInt:
        return VMFunctionData::RootBigInt;
      case TraceKind::String:
        return VMFunctionData::RootString;
      case TraceKind::Shape:
      case TraceKind::Script:
      case TraceKind::Scope:
        return VMFunctionData::RootCell;
      case TraceKind::Symbol:
      case TraceKind::BaseShape:
      case TraceKind::Null:
      case TraceKind::JitCode:
      case TraceKind::RegExpShared:
      case TraceKind::GetterSetter:
      case TraceKind::PropMap:
        MOZ_CRASH("Unexpected trace kind");
    }
  }

  static constexpr uint32_t result = rootType();
};
template <class T>
struct TypeToRootType<Handle<T>> {
  // Fail for Handle types that aren't specialized above.
};

template <class>
struct OutParamToDataType {
  static const DataType result = Type_Void;
};
template <class T>
struct OutParamToDataType<const T*> {
  // Const pointers can't be output parameters.
  static const DataType result = Type_Void;
};
template <>
struct OutParamToDataType<uint64_t*> {
  // Already used as an input type, so it can't be used as an output param.
  static const DataType result = Type_Void;
};
template <>
struct OutParamToDataType<JSObject*> {
  // Already used as an input type, so it can't be used as an output param.
  static const DataType result = Type_Void;
};
template <>
struct OutParamToDataType<JSString*> {
  // Already used as an input type, so it can't be used as an output param.
  static const DataType result = Type_Void;
};
template <>
struct OutParamToDataType<BaselineFrame*> {
  // Already used as an input type, so it can't be used as an output param.
  static const DataType result = Type_Void;
};
template <>
struct OutParamToDataType<gc::AllocSite*> {
  // Already used as an input type, so it can't be used as an output param.
  static const DataType result = Type_Void;
};
template <>
struct OutParamToDataType<Value*> {
  static const DataType result = Type_Value;
};
template <>
struct OutParamToDataType<int*> {
  static const DataType result = Type_Int32;
};
template <>
struct OutParamToDataType<uint32_t*> {
  static const DataType result = Type_Int32;
};
template <>
struct OutParamToDataType<bool*> {
  static const DataType result = Type_Bool;
};
template <>
struct OutParamToDataType<double*> {
  static const DataType result = Type_Double;
};
template <class T>
struct OutParamToDataType<T*> {
  // Fail for pointer types that aren't specialized above.
};
template <class T>
struct OutParamToDataType<T**> {
  static const DataType result = Type_Pointer;
};
template <class T>
struct OutParamToDataType<MutableHandle<T>> {
  static const DataType result = Type_Handle;
};

template <class>
struct OutParamToRootType {
  static const VMFunctionData::RootType result = VMFunctionData::RootNone;
};
template <>
struct OutParamToRootType<MutableHandleValue> {
  static const VMFunctionData::RootType result = VMFunctionData::RootValue;
};
template <>
struct OutParamToRootType<MutableHandleObject> {
  static const VMFunctionData::RootType result = VMFunctionData::RootObject;
};
template <>
struct OutParamToRootType<MutableHandleString> {
  static const VMFunctionData::RootType result = VMFunctionData::RootString;
};
template <>
struct OutParamToRootType<MutableHandleBigInt> {
  static const VMFunctionData::RootType result = VMFunctionData::RootBigInt;
};

// Construct a bit mask from a list of types.  The mask is constructed as an OR
// of the mask produced for each argument. The result of each argument is
// shifted by its index, such that the result of the first argument is on the
// low bits of the mask, and the result of the last argument in part of the
// high bits of the mask.
template <template <typename> class Each, typename ResultType, size_t Shift,
          typename... Args>
struct BitMask;

template <template <typename> class Each, typename ResultType, size_t Shift>
struct BitMask<Each, ResultType, Shift> {
  static constexpr ResultType result = ResultType();
};

template <template <typename> class Each, typename ResultType, size_t Shift,
          typename HeadType, typename... TailTypes>
struct BitMask<Each, ResultType, Shift, HeadType, TailTypes...> {
  static_assert(ResultType(Each<HeadType>::result) < (1 << Shift),
                "not enough bits reserved by the shift for individual results");
  static_assert(sizeof...(TailTypes) < (8 * sizeof(ResultType) / Shift),
                "not enough bits in the result type to store all bit masks");

  static constexpr ResultType result =
      ResultType(Each<HeadType>::result) |
      (BitMask<Each, ResultType, Shift, TailTypes...>::result << Shift);
};

// Helper template to build the VMFunctionData for a function.
template <typename... Args>
struct VMFunctionDataHelper;

template <class R, typename... Args>
struct VMFunctionDataHelper<R (*)(JSContext*, Args...)>
    : public VMFunctionData {
  using Fun = R (*)(JSContext*, Args...);

  static constexpr DataType returnType() {
    return ReturnTypeToDataType<R>::result;
  }
  static constexpr DataType outParam() {
    return OutParamToDataType<typename LastArg<Args...>::Type>::result;
  }
  static constexpr RootType outParamRootType() {
    return OutParamToRootType<typename LastArg<Args...>::Type>::result;
  }
  static constexpr size_t NbArgs() { return sizeof...(Args); }
  static constexpr size_t explicitArgs() {
    return NbArgs() - (outParam() != Type_Void ? 1 : 0);
  }
  static constexpr uint32_t argumentProperties() {
    return BitMask<TypeToArgProperties, uint32_t, 2, Args...>::result;
  }
  static constexpr uint32_t argumentPassedInFloatRegs() {
    return BitMask<TypeToPassInFloatReg, uint32_t, 2, Args...>::result;
  }
  static constexpr uint64_t argumentRootTypes() {
    return BitMask<TypeToRootType, uint64_t, 3, Args...>::result;
  }
  constexpr explicit VMFunctionDataHelper(const char* name)
      : VMFunctionData(name, explicitArgs(), argumentProperties(),
                       argumentPassedInFloatRegs(), argumentRootTypes(),
                       outParam(), outParamRootType(), returnType(),
                       /* extraValuesToPop = */ 0) {}
  constexpr explicit VMFunctionDataHelper(const char* name,
                                          PopValues extraValuesToPop)
      : VMFunctionData(name, explicitArgs(), argumentProperties(),
                       argumentPassedInFloatRegs(), argumentRootTypes(),
                       outParam(), outParamRootType(), returnType(),
                       extraValuesToPop.numValues) {}
};

// GCC warns when the signature does not have matching attributes (for example
// [[nodiscard]]). Squelch this warning to avoid a GCC-only footgun.
#if MOZ_IS_GCC
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wignored-attributes"
#endif

// Generate VMFunctionData array.
static constexpr VMFunctionData vmFunctions[] = {
#define DEF_VMFUNCTION_HELPER(name, fp, values_to_pop) \
  VMFunctionDataHelper<decltype(&(::fp))>(#name, PopValues values_to_pop),

#define DEF_VMFUNCTION(name, fp, ...) \
  DEF_VMFUNCTION_HELPER(name, fp, (__VA_ARGS__))

    VMFUNCTION_LIST(DEF_VMFUNCTION)
#undef DEF_VMFUNCTION
#undef DEF_VMFUNCTION_HELPER
};

#if MOZ_IS_GCC
#  pragma GCC diagnostic pop
#endif

// Generate arrays storing C++ function pointers. These pointers are not stored
// in VMFunctionData because there's no good way to cast them to void* in
// constexpr code. Compilers are smart enough to treat the const array below as
// constexpr.
#define DEF_VMFUNCTION(name, fp, ...) (void*)(::fp),
static void* const vmFunctionTargets[] = {VMFUNCTION_LIST(DEF_VMFUNCTION)};
#undef DEF_VMFUNCTION

const VMFunctionData& GetVMFunction(VMFunctionId id) {
  return vmFunctions[size_t(id)];
}

static DynFn GetVMFunctionTarget(VMFunctionId id) {
  return DynFn{vmFunctionTargets[size_t(id)]};
}

size_t NumVMFunctions() { return size_t(VMFunctionId::Count); }

size_t VMFunctionData::sizeOfOutParamStackSlot() const {
  switch (outParam) {
    case Type_Value:
      return sizeof(Value);

    case Type_Pointer:
    case Type_Int32:
    case Type_Bool:
      return sizeof(uintptr_t);

    case Type_Double:
      return sizeof(double);

    case Type_Handle:
      switch (outParamRootType) {
        case RootNone:
          MOZ_CRASH("Handle must have root type");
        case RootObject:
        case RootString:
        case RootCell:
        case RootBigInt:
        case RootId:
          return sizeof(uintptr_t);
        case RootValue:
          return sizeof(Value);
      }
      MOZ_CRASH("Invalid type");

    case Type_Void:
      return 0;

    case Type_Cell:
      MOZ_CRASH("Unexpected outparam type");
  }

  MOZ_CRASH("Invalid type");
}

bool JitRuntime::generateVMWrappers(JSContext* cx, MacroAssembler& masm,
                                    PerfSpewerRangeRecorder& rangeRecorder) {
  // Generate all VM function wrappers.

  static constexpr size_t NumVMFunctions = size_t(VMFunctionId::Count);

  if (!functionWrapperOffsets_.reserve(NumVMFunctions)) {
    return false;
  }

#ifdef DEBUG
  const char* lastName = nullptr;
#endif

  for (size_t i = 0; i < NumVMFunctions; i++) {
    VMFunctionId id = VMFunctionId(i);
    const VMFunctionData& fun = GetVMFunction(id);

#ifdef DEBUG
    // Assert the list is sorted by name.
    if (lastName) {
      MOZ_ASSERT(strcmp(lastName, fun.name()) < 0,
                 "VM function list must be sorted by name");
    }
    lastName = fun.name();
#endif

    JitSpew(JitSpew_Codegen, "# VM function wrapper (%s)", fun.name());

    uint32_t offset;
    if (!generateVMWrapper(cx, masm, id, fun, GetVMFunctionTarget(id),
                           &offset)) {
      return false;
    }
#if defined(JS_ION_PERF)
    rangeRecorder.recordVMWrapperOffset(fun.name());
#else
    rangeRecorder.recordOffset("Trampoline: VMWrapper");
#endif

    MOZ_ASSERT(functionWrapperOffsets_.length() == size_t(id));
    functionWrapperOffsets_.infallibleAppend(offset);
  }

  return true;
};

bool InvokeFunction(JSContext* cx, HandleObject obj, bool constructing,
                    bool ignoresReturnValue, uint32_t argc, Value* argv,
                    MutableHandleValue rval) {
  RootedExternalValueArray argvRoot(cx, argc + 1 + constructing, argv);

  // Data in the argument vector is arranged for a JIT -> JIT call.
  RootedValue thisv(cx, argv[0]);
  Value* argvWithoutThis = argv + 1;

  RootedValue fval(cx, ObjectValue(*obj));
  if (constructing) {
    if (!IsConstructor(fval)) {
      ReportValueError(cx, JSMSG_NOT_CONSTRUCTOR, JSDVG_IGNORE_STACK, fval,
                       nullptr);
      return false;
    }

    ConstructArgs cargs(cx);
    if (!cargs.init(cx, argc)) {
      return false;
    }

    for (uint32_t i = 0; i < argc; i++) {
      cargs[i].set(argvWithoutThis[i]);
    }

    RootedValue newTarget(cx, argvWithoutThis[argc]);

    // See CreateThisFromIon for why this can be NullValue.
    if (thisv.isNull()) {
      thisv.setMagic(JS_IS_CONSTRUCTING);
    }

    // If |this| hasn't been created, or is JS_UNINITIALIZED_LEXICAL,
    // we can use normal construction code without creating an extraneous
    // object.
    if (thisv.isMagic()) {
      MOZ_ASSERT(thisv.whyMagic() == JS_IS_CONSTRUCTING ||
                 thisv.whyMagic() == JS_UNINITIALIZED_LEXICAL);

      RootedObject obj(cx);
      if (!Construct(cx, fval, cargs, newTarget, &obj)) {
        return false;
      }

      rval.setObject(*obj);
      return true;
    }

    // Otherwise the default |this| has already been created.  We could
    // almost perform a *call* at this point, but we'd break |new.target|
    // in the function.  So in this one weird case we call a one-off
    // construction path that *won't* set |this| to JS_IS_CONSTRUCTING.
    return InternalConstructWithProvidedThis(cx, fval, thisv, cargs, newTarget,
                                             rval);
  }

  InvokeArgsMaybeIgnoresReturnValue args(cx);
  if (!args.init(cx, argc, ignoresReturnValue)) {
    return false;
  }

  for (size_t i = 0; i < argc; i++) {
    args[i].set(argvWithoutThis[i]);
  }

  return Call(cx, fval, thisv, args, rval);
}

void* GetContextSensitiveInterpreterStub() {
  return TlsContext.get()->runtime()->jitRuntime()->interpreterStub().value;
}

bool InvokeFromInterpreterStub(JSContext* cx,
                               InterpreterStubExitFrameLayout* frame) {
  JitFrameLayout* jsFrame = frame->jsFrame();
  CalleeToken token = jsFrame->calleeToken();

  Value* argv = jsFrame->thisAndActualArgs();
  uint32_t numActualArgs = jsFrame->numActualArgs();
  bool constructing = CalleeTokenIsConstructing(token);
  RootedFunction fun(cx, CalleeTokenToFunction(token));

  // Ensure new.target immediately follows the actual arguments (the arguments
  // rectifier added padding).
  if (constructing && numActualArgs < fun->nargs()) {
    argv[1 + numActualArgs] = argv[1 + fun->nargs()];
  }

  RootedValue rval(cx);
  if (!InvokeFunction(cx, fun, constructing,
                      /* ignoresReturnValue = */ false, numActualArgs, argv,
                      &rval)) {
    return false;
  }

  // Overwrite |this| with the return value.
  argv[0] = rval;
  return true;
}

static bool CheckOverRecursedImpl(JSContext* cx, size_t extra) {
  // We just failed the jitStackLimit check. There are two possible reasons:
  //  1) jitStackLimit was the real stack limit and we're over-recursed
  //  2) jitStackLimit was set to JS::NativeStackLimitMin by
  //     JSContext::requestInterrupt and we need to call
  //     JSContext::handleInterrupt.

  // This handles 1).
#ifdef JS_SIMULATOR
  if (cx->simulator()->overRecursedWithExtra(extra)) {
    ReportOverRecursed(cx);
    return false;
  }
#else
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.checkWithExtra(cx, extra)) {
    return false;
  }
#endif

  // This handles 2).
  gc::MaybeVerifyBarriers(cx);
  return cx->handleInterrupt();
}

bool CheckOverRecursed(JSContext* cx) { return CheckOverRecursedImpl(cx, 0); }

bool CheckOverRecursedBaseline(JSContext* cx, BaselineFrame* frame) {
  // The stack check in Baseline happens before pushing locals so we have to
  // account for that by including script->nslots() in the C++ recursion check.
  size_t extra = frame->script()->nslots() * sizeof(Value);
  return CheckOverRecursedImpl(cx, extra);
}

bool MutatePrototype(JSContext* cx, Handle<PlainObject*> obj,
                     HandleValue value) {
  if (!value.isObjectOrNull()) {
    return true;
  }

  RootedObject newProto(cx, value.toObjectOrNull());
  return SetPrototype(cx, obj, newProto);
}

template <EqualityKind Kind>
bool StringsEqual(JSContext* cx, HandleString lhs, HandleString rhs,
                  bool* res) {
  JSLinearString* linearLhs = lhs->ensureLinear(cx);
  if (!linearLhs) {
    return false;
  }
  JSLinearString* linearRhs = rhs->ensureLinear(cx);
  if (!linearRhs) {
    return false;
  }

  *res = EqualChars(linearLhs, linearRhs);

  if constexpr (Kind == EqualityKind::NotEqual) {
    *res = !*res;
  }
  return true;
}

template bool StringsEqual<EqualityKind::Equal>(JSContext* cx, HandleString lhs,
                                                HandleString rhs, bool* res);
template bool StringsEqual<EqualityKind::NotEqual>(JSContext* cx,
                                                   HandleString lhs,
                                                   HandleString rhs, bool* res);

template <ComparisonKind Kind>
bool StringsCompare(JSContext* cx, HandleString lhs, HandleString rhs,
                    bool* res) {
  int32_t result;
  if (!js::CompareStrings(cx, lhs, rhs, &result)) {
    return false;
  }
  if (Kind == ComparisonKind::LessThan) {
    *res = result < 0;
  } else {
    *res = result >= 0;
  }
  return true;
}

template bool StringsCompare<ComparisonKind::LessThan>(JSContext* cx,
                                                       HandleString lhs,
                                                       HandleString rhs,
                                                       bool* res);
template bool StringsCompare<ComparisonKind::GreaterThanOrEqual>(
    JSContext* cx, HandleString lhs, HandleString rhs, bool* res);

JSString* ArrayJoin(JSContext* cx, HandleObject array, HandleString sep) {
  JS::RootedValueArray<3> argv(cx);
  argv[0].setUndefined();
  argv[1].setObject(*array);
  argv[2].setString(sep);
  if (!js::array_join(cx, 1, argv.begin())) {
    return nullptr;
  }
  return argv[0].toString();
}

bool SetArrayLength(JSContext* cx, HandleObject obj, HandleValue value,
                    bool strict) {
  Handle<ArrayObject*> array = obj.as<ArrayObject>();

  RootedId id(cx, NameToId(cx->names().length));
  ObjectOpResult result;

  // SetArrayLength is called by IC stubs for SetProp and SetElem on arrays'
  // "length" property.
  //
  // ArraySetLength below coerces |value| before checking for length being
  // writable, and in the case of illegal values, will throw RangeError even
  // when "length" is not writable. This is incorrect observable behavior,
  // as a regular [[Set]] operation will check for "length" being
  // writable before attempting any assignment.
  //
  // So, perform ArraySetLength if and only if "length" is writable.
  if (array->lengthIsWritable()) {
    Rooted<PropertyDescriptor> desc(
        cx, PropertyDescriptor::Data(value, JS::PropertyAttribute::Writable));
    if (!ArraySetLength(cx, array, id, desc, result)) {
      return false;
    }
  } else {
    MOZ_ALWAYS_TRUE(result.fail(JSMSG_READ_ONLY));
  }

  return result.checkStrictModeError(cx, obj, id, strict);
}

bool CharCodeAt(JSContext* cx, HandleString str, int32_t index,
                uint32_t* code) {
  char16_t c;
  if (!str->getChar(cx, index, &c)) {
    return false;
  }
  *code = c;
  return true;
}

bool CodePointAt(JSContext* cx, HandleString str, int32_t index,
                 uint32_t* code) {
  char32_t codePoint;
  if (!str->getCodePoint(cx, size_t(index), &codePoint)) {
    return false;
  }
  *code = codePoint;
  return true;
}

JSLinearString* StringFromCharCodeNoGC(JSContext* cx, int32_t code) {
  AutoUnsafeCallWithABI unsafe;

  char16_t c = char16_t(code);

  if (StaticStrings::hasUnit(c)) {
    return cx->staticStrings().getUnit(c);
  }

  return NewInlineString<NoGC>(cx, {c}, 1);
}

JSLinearString* LinearizeForCharAccessPure(JSString* str) {
  AutoUnsafeCallWithABI unsafe;

  // Should only be called on ropes.
  MOZ_ASSERT(str->isRope());

  // ensureLinear is intentionally called with a nullptr to avoid OOM reporting.
  return str->ensureLinear(nullptr);
}

JSLinearString* LinearizeForCharAccess(JSContext* cx, JSString* str) {
  // Should only be called on ropes.
  MOZ_ASSERT(str->isRope());

  return str->ensureLinear(cx);
}

template <typename CharT>
static size_t StringTrimStartIndex(mozilla::Range<CharT> chars) {
  size_t begin = 0;
  while (begin < chars.length() && unicode::IsSpace(chars[begin])) {
    ++begin;
  }
  return begin;
}

template <typename CharT>
static size_t StringTrimEndIndex(mozilla::Range<CharT> chars, size_t begin) {
  size_t end = chars.length();
  while (end > begin && unicode::IsSpace(chars[end - 1])) {
    --end;
  }
  return end;
}

int32_t StringTrimStartIndex(const JSString* str) {
  AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(str->isLinear());

  const auto* linear = &str->asLinear();

  size_t begin;
  if (linear->hasLatin1Chars()) {
    JS::AutoCheckCannotGC nogc;
    begin = StringTrimStartIndex(linear->latin1Range(nogc));
  } else {
    JS::AutoCheckCannotGC nogc;
    begin = StringTrimStartIndex(linear->twoByteRange(nogc));
  }
  return int32_t(begin);
}

int32_t StringTrimEndIndex(const JSString* str, int32_t start) {
  AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(str->isLinear());
  MOZ_ASSERT(start >= 0 && size_t(start) <= str->length());

  const auto* linear = &str->asLinear();

  size_t end;
  if (linear->hasLatin1Chars()) {
    JS::AutoCheckCannotGC nogc;
    end = StringTrimEndIndex(linear->latin1Range(nogc), size_t(start));
  } else {
    JS::AutoCheckCannotGC nogc;
    end = StringTrimEndIndex(linear->twoByteRange(nogc), size_t(start));
  }
  return int32_t(end);
}

JSString* CharCodeToLowerCase(JSContext* cx, int32_t code) {
  RootedString str(cx, StringFromCharCode(cx, code));
  if (!str) {
    return nullptr;
  }
  return js::StringToLowerCase(cx, str);
}

JSString* CharCodeToUpperCase(JSContext* cx, int32_t code) {
  RootedString str(cx, StringFromCharCode(cx, code));
  if (!str) {
    return nullptr;
  }
  return js::StringToUpperCase(cx, str);
}

bool SetProperty(JSContext* cx, HandleObject obj, Handle<PropertyName*> name,
                 HandleValue value, bool strict, jsbytecode* pc) {
  RootedId id(cx, NameToId(name));

  RootedValue receiver(cx, ObjectValue(*obj));
  ObjectOpResult result;
  if (MOZ_LIKELY(!obj->getOpsSetProperty())) {
    JSOp op = JSOp(*pc);
    if (op == JSOp::SetName || op == JSOp::StrictSetName ||
        op == JSOp::SetGName || op == JSOp::StrictSetGName) {
      if (!NativeSetProperty<Unqualified>(cx, obj.as<NativeObject>(), id, value,
                                          receiver, result)) {
        return false;
      }
    } else {
      if (!NativeSetProperty<Qualified>(cx, obj.as<NativeObject>(), id, value,
                                        receiver, result)) {
        return false;
      }
    }
  } else {
    if (!SetProperty(cx, obj, id, value, receiver, result)) {
      return false;
    }
  }
  return result.checkStrictModeError(cx, obj, id, strict);
}

bool InterruptCheck(JSContext* cx) {
  gc::MaybeVerifyBarriers(cx);

  return CheckForInterrupt(cx);
}

JSObject* NewStringObject(JSContext* cx, HandleString str) {
  return StringObject::create(cx, str);
}

bool OperatorIn(JSContext* cx, HandleValue key, HandleObject obj, bool* out) {
  RootedId id(cx);
  return ToPropertyKey(cx, key, &id) && HasProperty(cx, obj, id, out);
}

bool GetIntrinsicValue(JSContext* cx, Handle<PropertyName*> name,
                       MutableHandleValue rval) {
  return GlobalObject::getIntrinsicValue(cx, cx->global(), name, rval);
}

bool CreateThisFromIC(JSContext* cx, HandleObject callee,
                      HandleObject newTarget, MutableHandleValue rval) {
  HandleFunction fun = callee.as<JSFunction>();
  MOZ_ASSERT(fun->isInterpreted());
  MOZ_ASSERT(fun->isConstructor());
  MOZ_ASSERT(cx->realm() == fun->realm(),
             "Realm switching happens before creating this");

  // CreateThis expects rval to be this magic value.
  rval.set(MagicValue(JS_IS_CONSTRUCTING));

  if (!js::CreateThis(cx, fun, newTarget, GenericObject, rval)) {
    return false;
  }

  MOZ_ASSERT_IF(rval.isObject(), fun->realm() == rval.toObject().nonCCWRealm());
  return true;
}

bool CreateThisFromIon(JSContext* cx, HandleObject callee,
                       HandleObject newTarget, MutableHandleValue rval) {
  // Return JS_IS_CONSTRUCTING for cases not supported by the inline call path.
  rval.set(MagicValue(JS_IS_CONSTRUCTING));

  if (!callee->is<JSFunction>()) {
    return true;
  }

  HandleFunction fun = callee.as<JSFunction>();
  if (!fun->isInterpreted() || !fun->isConstructor()) {
    return true;
  }

  // If newTarget is not a function or is a function with a possibly-getter
  // .prototype property, return NullValue to signal to LCallGeneric that it has
  // to take the slow path. Note that we return NullValue instead of a
  // MagicValue only because it's easier and faster to check for in JIT code
  // (if we returned a MagicValue, JIT code would have to check both the type
  // tag and the JSWhyMagic payload).
  if (!fun->constructorNeedsUninitializedThis()) {
    if (!newTarget->is<JSFunction>()) {
      rval.setNull();
      return true;
    }
    JSFunction* newTargetFun = &newTarget->as<JSFunction>();
    if (!newTargetFun->hasNonConfigurablePrototypeDataProperty()) {
      rval.setNull();
      return true;
    }
  }

  AutoRealm ar(cx, fun);
  if (!js::CreateThis(cx, fun, newTarget, GenericObject, rval)) {
    return false;
  }

  MOZ_ASSERT_IF(rval.isObject(), fun->realm() == rval.toObject().nonCCWRealm());
  return true;
}

void PostWriteBarrier(JSRuntime* rt, js::gc::Cell* cell) {
  AutoUnsafeCallWithABI unsafe;
  rt->gc.storeBuffer().putWholeCellDontCheckLast(cell);
}

static const size_t MAX_WHOLE_CELL_BUFFER_SIZE = 4096;

void PostWriteElementBarrier(JSRuntime* rt, JSObject* obj, int32_t index) {
  AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(!IsInsideNursery(obj));

  NativeObject* nobj = &obj->as<NativeObject>();

  MOZ_ASSERT(index >= 0);
  MOZ_ASSERT(uint32_t(index) < nobj->getDenseInitializedLength());

  if (nobj->isInWholeCellBuffer()) {
    return;
  }

  if (nobj->getDenseInitializedLength() > MAX_WHOLE_CELL_BUFFER_SIZE
#ifdef JS_GC_ZEAL
      || rt->hasZealMode(gc::ZealMode::ElementsBarrier)
#endif
  ) {
    rt->gc.storeBuffer().putSlot(nobj, HeapSlot::Element,
                                 nobj->unshiftedIndex(index), 1);
    return;
  }

  rt->gc.storeBuffer().putWholeCell(obj);
}

void PostGlobalWriteBarrier(JSRuntime* rt, GlobalObject* obj) {
  MOZ_ASSERT(obj->JSObject::is<GlobalObject>());

  if (!obj->realm()->globalWriteBarriered) {
    AutoUnsafeCallWithABI unsafe;
    rt->gc.storeBuffer().putWholeCell(obj);
    obj->realm()->globalWriteBarriered = 1;
  }
}

bool GetInt32FromStringPure(JSContext* cx, JSString* str, int32_t* result) {
  // We shouldn't GC here as this is called directly from IC code.
  AutoUnsafeCallWithABI unsafe;

  double d;
  if (!StringToNumberPure(cx, str, &d)) {
    return false;
  }

  return mozilla::NumberIsInt32(d, result);
}

int32_t GetIndexFromString(JSString* str) {
  // We shouldn't GC here as this is called directly from IC code.
  AutoUnsafeCallWithABI unsafe;

  if (!str->isLinear()) {
    return -1;
  }

  uint32_t index = UINT32_MAX;  // Initialize this to appease Valgrind.
  if (!str->asLinear().isIndex(&index) || index > INT32_MAX) {
    return -1;
  }

  return int32_t(index);
}

JSObject* WrapObjectPure(JSContext* cx, JSObject* obj) {
  // IC code calls this directly so we shouldn't GC.
  AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(obj);
  MOZ_ASSERT(cx->compartment() != obj->compartment());

  // From: Compartment::getNonWrapperObjectForCurrentCompartment
  // Note that if the object is same-compartment, but has been wrapped into a
  // different compartment, we need to unwrap it and return the bare same-
  // compartment object. Note again that windows are always wrapped by a
  // WindowProxy even when same-compartment so take care not to strip this
  // particular wrapper.
  obj = UncheckedUnwrap(obj, /* stopAtWindowProxy = */ true);
  if (cx->compartment() == obj->compartment()) {
    MOZ_ASSERT(!IsWindow(obj));
    JS::ExposeObjectToActiveJS(obj);
    return obj;
  }

  // Try to Lookup an existing wrapper for this object. We assume that
  // if we can find such a wrapper, not calling preWrap is correct.
  if (ObjectWrapperMap::Ptr p = cx->compartment()->lookupWrapper(obj)) {
    JSObject* wrapped = p->value().get();

    // Ensure the wrapper is still exposed.
    JS::ExposeObjectToActiveJS(wrapped);
    return wrapped;
  }

  return nullptr;
}

bool DebugPrologue(JSContext* cx, BaselineFrame* frame) {
  return DebugAPI::onEnterFrame(cx, frame);
}

bool DebugEpilogueOnBaselineReturn(JSContext* cx, BaselineFrame* frame,
                                   const jsbytecode* pc) {
  if (!DebugEpilogue(cx, frame, pc, true)) {
    return false;
  }

  return true;
}

bool DebugEpilogue(JSContext* cx, BaselineFrame* frame, const jsbytecode* pc,
                   bool ok) {
  // If DebugAPI::onLeaveFrame returns |true| we have to return the frame's
  // return value. If it returns |false|, the debugger threw an exception.
  // In both cases we have to pop debug scopes.
  ok = DebugAPI::onLeaveFrame(cx, frame, pc, ok);

  // Unwind to the outermost environment.
  EnvironmentIter ei(cx, frame, pc);
  UnwindAllEnvironmentsInFrame(cx, ei);

  if (!ok) {
    // Pop this frame by updating packedExitFP, so that the exception
    // handling code will start at the previous frame.
    JitFrameLayout* prefix = frame->framePrefix();
    EnsureUnwoundJitExitFrame(cx->activation()->asJit(), prefix);
    return false;
  }

  return true;
}

void FrameIsDebuggeeCheck(BaselineFrame* frame) {
  AutoUnsafeCallWithABI unsafe;
  if (frame->script()->isDebuggee()) {
    frame->setIsDebuggee();
  }
}

JSObject* CreateGeneratorFromFrame(JSContext* cx, BaselineFrame* frame) {
  return AbstractGeneratorObject::createFromFrame(cx, frame);
}

JSObject* CreateGenerator(JSContext* cx, HandleFunction callee,
                          HandleScript script, HandleObject environmentChain,
                          HandleObject args) {
  Rooted<ArgumentsObject*> argsObj(
      cx, args ? &args->as<ArgumentsObject>() : nullptr);
  return AbstractGeneratorObject::create(cx, callee, script, environmentChain,
                                         argsObj);
}

bool NormalSuspend(JSContext* cx, HandleObject obj, BaselineFrame* frame,
                   uint32_t frameSize, const jsbytecode* pc) {
  MOZ_ASSERT(JSOp(*pc) == JSOp::InitialYield || JSOp(*pc) == JSOp::Yield ||
             JSOp(*pc) == JSOp::Await);

  // Minus one because we don't want to include the return value.
  uint32_t numSlots = frame->numValueSlots(frameSize) - 1;
  MOZ_ASSERT(numSlots >= frame->script()->nfixed());
  return AbstractGeneratorObject::suspend(cx, obj, frame, pc, numSlots);
}

bool FinalSuspend(JSContext* cx, HandleObject obj, const jsbytecode* pc) {
  MOZ_ASSERT(JSOp(*pc) == JSOp::FinalYieldRval);
  AbstractGeneratorObject::finalSuspend(cx, obj);
  return true;
}

bool InterpretResume(JSContext* cx, HandleObject obj, Value* stackValues,
                     MutableHandleValue rval) {
  MOZ_ASSERT(obj->is<AbstractGeneratorObject>());

  // The |stackValues| argument points to the JSOp::Resume operands on the
  // native stack. Because the stack grows down, these values are:
  //
  //   [resumeKind, argument, generator, ..]

  MOZ_ASSERT(stackValues[2].toObject() == *obj);

  GeneratorResumeKind resumeKind = IntToResumeKind(stackValues[0].toInt32());
  JSAtom* kind = ResumeKindToAtom(cx, resumeKind);

  FixedInvokeArgs<3> args(cx);

  args[0].setObject(*obj);
  args[1].set(stackValues[1]);
  args[2].setString(kind);

  return CallSelfHostedFunction(cx, cx->names().InterpretGeneratorResume,
                                UndefinedHandleValue, args, rval);
}

bool DebugAfterYield(JSContext* cx, BaselineFrame* frame) {
  // The BaselineFrame has just been constructed by JSOp::Resume in the
  // caller. We need to set its debuggee flag as necessary.
  //
  // If a breakpoint is set on JSOp::AfterYield, or stepping is enabled,
  // we may already have done this work. Don't fire onEnterFrame again.
  if (frame->script()->isDebuggee() && !frame->isDebuggee()) {
    frame->setIsDebuggee();
    return DebugAPI::onResumeFrame(cx, frame);
  }

  return true;
}

bool GeneratorThrowOrReturn(JSContext* cx, BaselineFrame* frame,
                            Handle<AbstractGeneratorObject*> genObj,
                            HandleValue arg, int32_t resumeKindArg) {
  GeneratorResumeKind resumeKind = IntToResumeKind(resumeKindArg);
  MOZ_ALWAYS_FALSE(
      js::GeneratorThrowOrReturn(cx, frame, genObj, arg, resumeKind));
  return false;
}

bool GlobalDeclInstantiationFromIon(JSContext* cx, HandleScript script,
                                    const jsbytecode* pc) {
  MOZ_ASSERT(!script->hasNonSyntacticScope());

  RootedObject envChain(cx, &cx->global()->lexicalEnvironment());
  GCThingIndex lastFun = GET_GCTHING_INDEX(pc);

  return GlobalOrEvalDeclInstantiation(cx, envChain, script, lastFun);
}

bool InitFunctionEnvironmentObjects(JSContext* cx, BaselineFrame* frame) {
  return frame->initFunctionEnvironmentObjects(cx);
}

bool NewArgumentsObject(JSContext* cx, BaselineFrame* frame,
                        MutableHandleValue res) {
  ArgumentsObject* obj = ArgumentsObject::createExpected(cx, frame);
  if (!obj) {
    return false;
  }
  res.setObject(*obj);
  return true;
}

ArrayObject* NewArrayObjectEnsureDenseInitLength(JSContext* cx, int32_t count) {
  MOZ_ASSERT(count >= 0);

  auto* array = NewDenseFullyAllocatedArray(cx, count);
  if (!array) {
    return nullptr;
  }
  array->ensureDenseInitializedLength(0, count);

  return array;
}

ArrayObject* InitRestParameter(JSContext* cx, uint32_t length, Value* rest,
                               Handle<ArrayObject*> arrRes) {
  if (arrRes) {
    // Fast path: we managed to allocate the array inline; initialize the
    // elements.
    MOZ_ASSERT(arrRes->getDenseInitializedLength() == 0);

    // We don't call this function if we can initialize the elements in JIT
    // code.
    MOZ_ASSERT(length > arrRes->getDenseCapacity());

    if (!arrRes->growElements(cx, length)) {
      return nullptr;
    }
    arrRes->initDenseElements(rest, length);
    arrRes->setLength(length);
    return arrRes;
  }

  return NewDenseCopiedArray(cx, length, rest);
}

bool HandleDebugTrap(JSContext* cx, BaselineFrame* frame,
                     const uint8_t* retAddr) {
  RootedScript script(cx, frame->script());
  jsbytecode* pc;
  if (frame->runningInInterpreter()) {
    pc = frame->interpreterPC();
  } else {
    BaselineScript* blScript = script->baselineScript();
    pc = blScript->retAddrEntryFromReturnAddress(retAddr).pc(script);
  }

  // The Baseline Interpreter calls HandleDebugTrap for every op when the script
  // is in step mode or has breakpoints. The Baseline Compiler can toggle
  // breakpoints more granularly for specific bytecode PCs.
  if (frame->runningInInterpreter()) {
    MOZ_ASSERT(DebugAPI::hasAnyBreakpointsOrStepMode(script));
  } else {
    MOZ_ASSERT(DebugAPI::stepModeEnabled(script) ||
               DebugAPI::hasBreakpointsAt(script, pc));
  }

  if (JSOp(*pc) == JSOp::AfterYield) {
    // JSOp::AfterYield will set the frame's debuggee flag and call the
    // onEnterFrame handler, but if we set a breakpoint there we have to do
    // it now.
    MOZ_ASSERT(!frame->isDebuggee());

    if (!DebugAfterYield(cx, frame)) {
      return false;
    }

    // If the frame is not a debuggee we're done. This can happen, for instance,
    // if the onEnterFrame hook called removeDebuggee.
    if (!frame->isDebuggee()) {
      return true;
    }
  }

  MOZ_ASSERT(frame->isDebuggee());

  if (DebugAPI::stepModeEnabled(script) && !DebugAPI::onSingleStep(cx)) {
    return false;
  }

  if (DebugAPI::hasBreakpointsAt(script, pc) && !DebugAPI::onTrap(cx)) {
    return false;
  }

  return true;
}

bool OnDebuggerStatement(JSContext* cx, BaselineFrame* frame) {
  return DebugAPI::onDebuggerStatement(cx, frame);
}

bool GlobalHasLiveOnDebuggerStatement(JSContext* cx) {
  AutoUnsafeCallWithABI unsafe;
  return cx->realm()->isDebuggee() &&
         DebugAPI::hasDebuggerStatementHook(cx->global());
}

bool PushLexicalEnv(JSContext* cx, BaselineFrame* frame,
                    Handle<LexicalScope*> scope) {
  return frame->pushLexicalEnvironment(cx, scope);
}

bool DebugLeaveThenPopLexicalEnv(JSContext* cx, BaselineFrame* frame,
                                 const jsbytecode* pc) {
  MOZ_ALWAYS_TRUE(DebugLeaveLexicalEnv(cx, frame, pc));
  frame->popOffEnvironmentChain<ScopedLexicalEnvironmentObject>();
  return true;
}

bool FreshenLexicalEnv(JSContext* cx, BaselineFrame* frame) {
  return frame->freshenLexicalEnvironment<false>(cx);
}

bool DebuggeeFreshenLexicalEnv(JSContext* cx, BaselineFrame* frame,
                               const jsbytecode* pc) {
  return frame->freshenLexicalEnvironment<true>(cx, pc);
}

bool RecreateLexicalEnv(JSContext* cx, BaselineFrame* frame) {
  return frame->recreateLexicalEnvironment<false>(cx);
}

bool DebuggeeRecreateLexicalEnv(JSContext* cx, BaselineFrame* frame,
                                const jsbytecode* pc) {
  return frame->recreateLexicalEnvironment<true>(cx, pc);
}

bool DebugLeaveLexicalEnv(JSContext* cx, BaselineFrame* frame,
                          const jsbytecode* pc) {
  MOZ_ASSERT_IF(!frame->runningInInterpreter(),
                frame->script()->baselineScript()->hasDebugInstrumentation());
  if (cx->realm()->isDebuggee()) {
    DebugEnvironments::onPopLexical(cx, frame, pc);
  }
  return true;
}

bool PushClassBodyEnv(JSContext* cx, BaselineFrame* frame,
                      Handle<ClassBodyScope*> scope) {
  return frame->pushClassBodyEnvironment(cx, scope);
}

bool PushVarEnv(JSContext* cx, BaselineFrame* frame, Handle<Scope*> scope) {
  return frame->pushVarEnvironment(cx, scope);
}

bool EnterWith(JSContext* cx, BaselineFrame* frame, HandleValue val,
               Handle<WithScope*> templ) {
  return EnterWithOperation(cx, frame, val, templ);
}

bool LeaveWith(JSContext* cx, BaselineFrame* frame) {
  if (MOZ_UNLIKELY(frame->isDebuggee())) {
    DebugEnvironments::onPopWith(frame);
  }
  frame->popOffEnvironmentChain<WithEnvironmentObject>();
  return true;
}

bool InitBaselineFrameForOsr(BaselineFrame* frame,
                             InterpreterFrame* interpFrame,
                             uint32_t numStackValues) {
  return frame->initForOsr(interpFrame, numStackValues);
}

JSString* StringReplace(JSContext* cx, HandleString string,
                        HandleString pattern, HandleString repl) {
  MOZ_ASSERT(string);
  MOZ_ASSERT(pattern);
  MOZ_ASSERT(repl);

  return str_replace_string_raw(cx, string, pattern, repl);
}

void AssertValidBigIntPtr(JSContext* cx, JS::BigInt* bi) {
  AutoUnsafeCallWithABI unsafe;
  // FIXME: check runtime?
  MOZ_ASSERT(cx->zone() == bi->zone());
  MOZ_ASSERT(bi->isAligned());
  MOZ_ASSERT(bi->getAllocKind() == gc::AllocKind::BIGINT);
}

void AssertValidObjectPtr(JSContext* cx, JSObject* obj) {
  AutoUnsafeCallWithABI unsafe;
#ifdef DEBUG
  // Check what we can, so that we'll hopefully assert/crash if we get a
  // bogus object (pointer).
  MOZ_ASSERT(obj->compartment() == cx->compartment());
  MOZ_ASSERT(obj->zoneFromAnyThread() == cx->zone());
  MOZ_ASSERT(obj->runtimeFromMainThread() == cx->runtime());

  if (obj->isTenured()) {
    MOZ_ASSERT(obj->isAligned());
    gc::AllocKind kind = obj->asTenured().getAllocKind();
    MOZ_ASSERT(gc::IsObjectAllocKind(kind));
  }
#endif
}

void AssertValidStringPtr(JSContext* cx, JSString* str) {
  AutoUnsafeCallWithABI unsafe;
#ifdef DEBUG
  // We can't closely inspect strings from another runtime.
  if (str->runtimeFromAnyThread() != cx->runtime()) {
    MOZ_ASSERT(str->isPermanentAtom());
    return;
  }

  if (str->isAtom()) {
    MOZ_ASSERT(str->zone()->isAtomsZone());
  } else {
    MOZ_ASSERT(str->zone() == cx->zone());
  }

  MOZ_ASSERT(str->isAligned());
  MOZ_ASSERT(str->length() <= JSString::MAX_LENGTH);

  gc::AllocKind kind = str->getAllocKind();
  if (str->isFatInline()) {
    if (str->isAtom()) {
      MOZ_ASSERT(kind == gc::AllocKind::FAT_INLINE_ATOM);
    } else {
      MOZ_ASSERT(kind == gc::AllocKind::FAT_INLINE_STRING);
    }
  } else if (str->isExternal()) {
    MOZ_ASSERT(kind == gc::AllocKind::EXTERNAL_STRING);
  } else if (str->isAtom()) {
    MOZ_ASSERT(kind == gc::AllocKind::ATOM);
  } else if (str->isLinear()) {
    MOZ_ASSERT(kind == gc::AllocKind::STRING ||
               kind == gc::AllocKind::FAT_INLINE_STRING);
  } else {
    MOZ_ASSERT(kind == gc::AllocKind::STRING);
  }
#endif
}

void AssertValidSymbolPtr(JSContext* cx, JS::Symbol* sym) {
  AutoUnsafeCallWithABI unsafe;

  // We can't closely inspect symbols from another runtime.
  if (sym->runtimeFromAnyThread() != cx->runtime()) {
    MOZ_ASSERT(sym->isWellKnownSymbol());
    return;
  }

  MOZ_ASSERT(sym->zone()->isAtomsZone());
  MOZ_ASSERT(sym->isAligned());
  if (JSAtom* desc = sym->description()) {
    AssertValidStringPtr(cx, desc);
  }

  MOZ_ASSERT(sym->getAllocKind() == gc::AllocKind::SYMBOL);
}

void AssertValidValue(JSContext* cx, Value* v) {
  AutoUnsafeCallWithABI unsafe;
  if (v->isObject()) {
    AssertValidObjectPtr(cx, &v->toObject());
  } else if (v->isString()) {
    AssertValidStringPtr(cx, v->toString());
  } else if (v->isSymbol()) {
    AssertValidSymbolPtr(cx, v->toSymbol());
  } else if (v->isBigInt()) {
    AssertValidBigIntPtr(cx, v->toBigInt());
  }
}

bool ObjectIsCallable(JSObject* obj) {
  AutoUnsafeCallWithABI unsafe;
  return obj->isCallable();
}

bool ObjectIsConstructor(JSObject* obj) {
  AutoUnsafeCallWithABI unsafe;
  return obj->isConstructor();
}

JSObject* ObjectKeys(JSContext* cx, HandleObject obj) {
  JS::RootedValueArray<3> argv(cx);
  argv[0].setUndefined();   // rval
  argv[1].setUndefined();   // this
  argv[2].setObject(*obj);  // arg0
  if (!js::obj_keys(cx, 1, argv.begin())) {
    return nullptr;
  }
  return argv[0].toObjectOrNull();
}

bool ObjectKeysLength(JSContext* cx, HandleObject obj, int32_t* length) {
  MOZ_ASSERT(!obj->is<ProxyObject>());
  return js::obj_keys_length(cx, obj, *length);
}

void JitValuePreWriteBarrier(JSRuntime* rt, Value* vp) {
  AutoUnsafeCallWithABI unsafe;
  MOZ_ASSERT(vp->isGCThing());
  MOZ_ASSERT(!vp->toGCThing()->isMarkedBlack());
  gc::ValuePreWriteBarrier(*vp);
}

void JitStringPreWriteBarrier(JSRuntime* rt, JSString** stringp) {
  AutoUnsafeCallWithABI unsafe;
  MOZ_ASSERT(*stringp);
  MOZ_ASSERT(!(*stringp)->isMarkedBlack());
  gc::PreWriteBarrier(*stringp);
}

void JitObjectPreWriteBarrier(JSRuntime* rt, JSObject** objp) {
  AutoUnsafeCallWithABI unsafe;
  MOZ_ASSERT(*objp);
  MOZ_ASSERT(!(*objp)->isMarkedBlack());
  gc::PreWriteBarrier(*objp);
}

void JitShapePreWriteBarrier(JSRuntime* rt, Shape** shapep) {
  AutoUnsafeCallWithABI unsafe;
  MOZ_ASSERT(!(*shapep)->isMarkedBlack());
  gc::PreWriteBarrier(*shapep);
}

void JitWasmAnyRefPreWriteBarrier(JSRuntime* rt, wasm::AnyRef* refp) {
  AutoUnsafeCallWithABI unsafe;
  MOZ_ASSERT(refp->isGCThing());
  MOZ_ASSERT(!(*refp).toGCThing()->isMarkedBlack());
  gc::WasmAnyRefPreWriteBarrier(*refp);
}

bool ThrowRuntimeLexicalError(JSContext* cx, unsigned errorNumber) {
  ScriptFrameIter iter(cx);
  RootedScript script(cx, iter.script());
  ReportRuntimeLexicalError(cx, errorNumber, script, iter.pc());
  return false;
}

bool ThrowBadDerivedReturnOrUninitializedThis(JSContext* cx, HandleValue v) {
  MOZ_ASSERT(!v.isObject());
  if (v.isUndefined()) {
    return js::ThrowUninitializedThis(cx);
  }

  ReportValueError(cx, JSMSG_BAD_DERIVED_RETURN, JSDVG_IGNORE_STACK, v,
                   nullptr);
  return false;
}

bool BaselineGetFunctionThis(JSContext* cx, BaselineFrame* frame,
                             MutableHandleValue res) {
  return GetFunctionThis(cx, frame, res);
}

bool CallNativeGetter(JSContext* cx, HandleFunction callee,
                      HandleValue receiver, MutableHandleValue result) {
  AutoRealm ar(cx, callee);

  MOZ_ASSERT(callee->isNativeFun());
  JSNative natfun = callee->native();

  JS::RootedValueArray<2> vp(cx);
  vp[0].setObject(*callee.get());
  vp[1].set(receiver);

  if (!natfun(cx, 0, vp.begin())) {
    return false;
  }

  result.set(vp[0]);
  return true;
}

bool CallDOMGetter(JSContext* cx, const JSJitInfo* info, HandleObject obj,
                   MutableHandleValue result) {
  MOZ_ASSERT(info->type() == JSJitInfo::Getter);
  MOZ_ASSERT(obj->is<NativeObject>());
  MOZ_ASSERT(obj->getClass()->isDOMClass());
  MOZ_ASSERT(obj->as<NativeObject>().numFixedSlots() > 0);

#ifdef DEBUG
  DOMInstanceClassHasProtoAtDepth instanceChecker =
      cx->runtime()->DOMcallbacks->instanceClassMatchesProto;
  MOZ_ASSERT(instanceChecker(obj->getClass(), info->protoID, info->depth));
#endif

  // Loading DOM_OBJECT_SLOT, which must be the first slot.
  JS::Value val = JS::GetReservedSlot(obj, 0);
  JSJitGetterOp getter = info->getter;
  return getter(cx, obj, val.toPrivate(), JSJitGetterCallArgs(result));
}

bool CallNativeSetter(JSContext* cx, HandleFunction callee, HandleObject obj,
                      HandleValue rhs) {
  AutoRealm ar(cx, callee);

  MOZ_ASSERT(callee->isNativeFun());
  JSNative natfun = callee->native();

  JS::RootedValueArray<3> vp(cx);
  vp[0].setObject(*callee.get());
  vp[1].setObject(*obj.get());
  vp[2].set(rhs);

  return natfun(cx, 1, vp.begin());
}

bool CallDOMSetter(JSContext* cx, const JSJitInfo* info, HandleObject obj,
                   HandleValue value) {
  MOZ_ASSERT(info->type() == JSJitInfo::Setter);
  MOZ_ASSERT(obj->is<NativeObject>());
  MOZ_ASSERT(obj->getClass()->isDOMClass());
  MOZ_ASSERT(obj->as<NativeObject>().numFixedSlots() > 0);

#ifdef DEBUG
  DOMInstanceClassHasProtoAtDepth instanceChecker =
      cx->runtime()->DOMcallbacks->instanceClassMatchesProto;
  MOZ_ASSERT(instanceChecker(obj->getClass(), info->protoID, info->depth));
#endif

  // Loading DOM_OBJECT_SLOT, which must be the first slot.
  JS::Value val = JS::GetReservedSlot(obj, 0);
  JSJitSetterOp setter = info->setter;

  RootedValue v(cx, value);
  return setter(cx, obj, val.toPrivate(), JSJitSetterCallArgs(&v));
}

bool EqualStringsHelperPure(JSString* str1, JSString* str2) {
  // IC code calls this directly so we shouldn't GC.
  AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(str1->isAtom());
  MOZ_ASSERT(!str2->isAtom());
  MOZ_ASSERT(str1->length() == str2->length());

  // ensureLinear is intentionally called with a nullptr to avoid OOM
  // reporting; if it fails, we will continue to the next stub.
  JSLinearString* str2Linear = str2->ensureLinear(nullptr);
  if (!str2Linear) {
    return false;
  }

  return EqualChars(&str1->asLinear(), str2Linear);
}

static bool MaybeTypedArrayIndexString(jsid id) {
  MOZ_ASSERT(id.isAtom() || id.isSymbol());

  if (MOZ_LIKELY(id.isAtom())) {
    JSAtom* str = id.toAtom();
    if (str->length() > 0) {
      // Only check the first character because we want this function to be
      // fast.
      return CanStartTypedArrayIndex(str->latin1OrTwoByteChar(0));
    }
  }
  return false;
}

static void VerifyCacheEntry(JSContext* cx, NativeObject* obj, PropertyKey key,
                             const MegamorphicCacheEntry& entry) {
#ifdef DEBUG
  if (entry.isMissingProperty()) {
    NativeObject* pobj;
    PropertyResult prop;
    MOZ_ASSERT(LookupPropertyPure(cx, obj, key, &pobj, &prop));
    MOZ_ASSERT(prop.isNotFound());
    return;
  }
  if (entry.isMissingOwnProperty()) {
    MOZ_ASSERT(!obj->containsPure(key));
    return;
  }
  MOZ_ASSERT(entry.isDataProperty());
  for (size_t i = 0, numHops = entry.numHops(); i < numHops; i++) {
    MOZ_ASSERT(!obj->containsPure(key));
    obj = &obj->staticPrototype()->as<NativeObject>();
  }
  mozilla::Maybe<PropertyInfo> prop = obj->lookupPure(key);
  MOZ_ASSERT(prop.isSome());
  MOZ_ASSERT(prop->isDataProperty());
  MOZ_ASSERT(obj->getTaggedSlotOffset(prop->slot()) == entry.slotOffset());
#endif
}

static MOZ_ALWAYS_INLINE bool GetNativeDataPropertyPureImpl(
    JSContext* cx, JSObject* obj, jsid id, MegamorphicCacheEntry* entry,
    Value* vp) {
  MOZ_ASSERT(obj->is<NativeObject>());
  NativeObject* nobj = &obj->as<NativeObject>();
  Shape* receiverShape = obj->shape();
  MegamorphicCache& cache = cx->caches().megamorphicCache;

  MOZ_ASSERT(entry);

  size_t numHops = 0;
  while (true) {
    MOZ_ASSERT(!nobj->getOpsLookupProperty());

    uint32_t index;
    if (PropMap* map = nobj->shape()->lookup(cx, id, &index)) {
      PropertyInfo prop = map->getPropertyInfo(index);
      if (!prop.isDataProperty()) {
        return false;
      }
      TaggedSlotOffset offset = nobj->getTaggedSlotOffset(prop.slot());
      cache.initEntryForDataProperty(entry, receiverShape, id, numHops, offset);
      *vp = nobj->getSlot(prop.slot());
      return true;
    }

    // Property not found. Watch out for Class hooks and TypedArrays.
    if (MOZ_UNLIKELY(!nobj->is<PlainObject>())) {
      if (ClassMayResolveId(cx->names(), nobj->getClass(), id, nobj)) {
        return false;
      }

      // Don't skip past TypedArrayObjects if the id can be a TypedArray index.
      if (nobj->is<TypedArrayObject>()) {
        if (MaybeTypedArrayIndexString(id)) {
          return false;
        }
      }
    }

    JSObject* proto = nobj->staticPrototype();
    if (!proto) {
      cache.initEntryForMissingProperty(entry, receiverShape, id);
      vp->setUndefined();
      return true;
    }

    if (!proto->is<NativeObject>()) {
      return false;
    }
    nobj = &proto->as<NativeObject>();
    numHops++;
  }
}

bool GetNativeDataPropertyPureWithCacheLookup(JSContext* cx, JSObject* obj,
                                              PropertyKey id,
                                              MegamorphicCacheEntry* entry,
                                              Value* vp) {
  AutoUnsafeCallWithABI unsafe;

  // If we're on x86, we didn't have enough registers to populate this
  // directly in Baseline JITted code, so we do the lookup here.
  Shape* receiverShape = obj->shape();
  MegamorphicCache& cache = cx->caches().megamorphicCache;

  if (cache.lookup(receiverShape, id, &entry)) {
    NativeObject* nobj = &obj->as<NativeObject>();
    VerifyCacheEntry(cx, nobj, id, *entry);
    if (entry->isDataProperty()) {
      for (size_t i = 0, numHops = entry->numHops(); i < numHops; i++) {
        nobj = &nobj->staticPrototype()->as<NativeObject>();
      }
      uint32_t offset = entry->slotOffset().offset();
      if (entry->slotOffset().isFixedSlot()) {
        size_t index = NativeObject::getFixedSlotIndexFromOffset(offset);
        *vp = nobj->getFixedSlot(index);
      } else {
        size_t index = NativeObject::getDynamicSlotIndexFromOffset(offset);
        *vp = nobj->getDynamicSlot(index);
      }
      return true;
    }
    if (entry->isMissingProperty()) {
      vp->setUndefined();
      return true;
    }
    MOZ_ASSERT(entry->isMissingOwnProperty());
  }

  return GetNativeDataPropertyPureImpl(cx, obj, id, entry, vp);
}

bool CheckProxyGetByValueResult(JSContext* cx, HandleObject obj,
                                HandleValue idVal, HandleValue value,
                                MutableHandleValue result) {
  MOZ_ASSERT(idVal.isString() || idVal.isSymbol());
  RootedId rootedId(cx);
  if (!PrimitiveValueToId<CanGC>(cx, idVal, &rootedId)) {
    return false;
  }

  auto validation =
      ScriptedProxyHandler::checkGetTrapResult(cx, obj, rootedId, value);
  if (validation != ScriptedProxyHandler::GetTrapValidationResult::OK) {
    ScriptedProxyHandler::reportGetTrapValidationError(cx, rootedId,
                                                       validation);
    return false;
  }
  result.set(value);
  return true;
}

bool GetNativeDataPropertyPure(JSContext* cx, JSObject* obj, PropertyKey id,
                               MegamorphicCacheEntry* entry, Value* vp) {
  AutoUnsafeCallWithABI unsafe;
  return GetNativeDataPropertyPureImpl(cx, obj, id, entry, vp);
}

static MOZ_ALWAYS_INLINE bool ValueToAtomOrSymbolPure(JSContext* cx,
                                                      const Value& idVal,
                                                      jsid* id) {
  if (MOZ_LIKELY(idVal.isString())) {
    JSString* s = idVal.toString();
    JSAtom* atom;
    if (s->isAtom()) {
      atom = &s->asAtom();
    } else {
      atom = AtomizeString(cx, s);
      if (!atom) {
        cx->recoverFromOutOfMemory();
        return false;
      }
    }

    // Watch out for integer ids because they may be stored in dense elements.
    static_assert(PropertyKey::IntMin == 0);
    static_assert(NativeObject::MAX_DENSE_ELEMENTS_COUNT < PropertyKey::IntMax,
                  "All dense elements must have integer jsids");
    uint32_t index;
    if (MOZ_UNLIKELY(atom->isIndex(&index) && index <= PropertyKey::IntMax)) {
      return false;
    }

    *id = PropertyKey::NonIntAtom(atom);
    return true;
  }

  if (idVal.isSymbol()) {
    *id = PropertyKey::Symbol(idVal.toSymbol());
    return true;
  }

  if (idVal.isNull()) {
    *id = PropertyKey::NonIntAtom(cx->names().null);
    return true;
  }

  if (idVal.isUndefined()) {
    *id = PropertyKey::NonIntAtom(cx->names().undefined);
    return true;
  }

  return false;
}

bool GetNativeDataPropertyByValuePure(JSContext* cx, JSObject* obj,
                                      MegamorphicCacheEntry* entry, Value* vp) {
  AutoUnsafeCallWithABI unsafe;

  // vp[0] contains the id, result will be stored in vp[1].
  Value idVal = vp[0];
  jsid id;
  if (!ValueToAtomOrSymbolPure(cx, idVal, &id)) {
    return false;
  }

  Shape* receiverShape = obj->shape();
  MegamorphicCache& cache = cx->caches().megamorphicCache;
  if (!entry) {
    cache.lookup(receiverShape, id, &entry);
  }

  Value* res = vp + 1;
  return GetNativeDataPropertyPureImpl(cx, obj, id, entry, res);
}

bool ObjectHasGetterSetterPure(JSContext* cx, JSObject* objArg, jsid id,
                               GetterSetter* getterSetter) {
  AutoUnsafeCallWithABI unsafe;

  // Window objects may require outerizing (passing the WindowProxy to the
  // getter/setter), so we don't support them here.
  if (MOZ_UNLIKELY(!objArg->is<NativeObject>() || IsWindow(objArg))) {
    return false;
  }

  NativeObject* nobj = &objArg->as<NativeObject>();

  while (true) {
    uint32_t index;
    if (PropMap* map = nobj->shape()->lookup(cx, id, &index)) {
      PropertyInfo prop = map->getPropertyInfo(index);
      if (!prop.isAccessorProperty()) {
        return false;
      }
      GetterSetter* actualGetterSetter = nobj->getGetterSetter(prop);
      if (actualGetterSetter == getterSetter) {
        return true;
      }
      return (actualGetterSetter->getter() == getterSetter->getter() &&
              actualGetterSetter->setter() == getterSetter->setter());
    }

    // Property not found. Watch out for Class hooks.
    if (!nobj->is<PlainObject>()) {
      if (ClassMayResolveId(cx->names(), nobj->getClass(), id, nobj)) {
        return false;
      }
    }

    JSObject* proto = nobj->staticPrototype();
    if (!proto) {
      return false;
    }

    if (!proto->is<NativeObject>()) {
      return false;
    }
    nobj = &proto->as<NativeObject>();
  }
}

template <bool HasOwn>
bool HasNativeDataPropertyPure(JSContext* cx, JSObject* obj,
                               MegamorphicCacheEntry* entry, Value* vp) {
  AutoUnsafeCallWithABI unsafe;

  // vp[0] contains the id, result will be stored in vp[1].
  Value idVal = vp[0];
  jsid id;
  if (!ValueToAtomOrSymbolPure(cx, idVal, &id)) {
    return false;
  }

  MegamorphicCache& cache = cx->caches().megamorphicCache;
  Shape* receiverShape = obj->shape();
  if (!entry) {
    if (cache.lookup(receiverShape, id, &entry)) {
      VerifyCacheEntry(cx, &obj->as<NativeObject>(), id, *entry);
    }
  }

  size_t numHops = 0;
  do {
    if (MOZ_UNLIKELY(!obj->is<NativeObject>())) {
      return false;
    }

    MOZ_ASSERT(!obj->getOpsLookupProperty());

    NativeObject* nobj = &obj->as<NativeObject>();
    uint32_t index;
    if (PropMap* map = nobj->shape()->lookup(cx, id, &index)) {
      PropertyInfo prop = map->getPropertyInfo(index);
      if (prop.isDataProperty()) {
        TaggedSlotOffset offset = nobj->getTaggedSlotOffset(prop.slot());
        cache.initEntryForDataProperty(entry, receiverShape, id, numHops,
                                       offset);
      }
      vp[1].setBoolean(true);
      return true;
    }

    // Property not found. Watch out for Class hooks and TypedArrays.
    if (MOZ_UNLIKELY(!obj->is<PlainObject>())) {
      // Fail if there's a resolve hook, unless the mayResolve hook tells us
      // the resolve hook won't define a property with this id.
      if (ClassMayResolveId(cx->names(), obj->getClass(), id, obj)) {
        return false;
      }

      // Don't skip past TypedArrayObjects if the id can be a TypedArray
      // index.
      if (obj->is<TypedArrayObject>()) {
        if (MaybeTypedArrayIndexString(id)) {
          return false;
        }
      }
    }

    // If implementing Object.hasOwnProperty, don't follow protochain.
    if constexpr (HasOwn) {
      break;
    }

    // Get prototype. Objects that may allow dynamic prototypes are already
    // filtered out above.
    obj = obj->staticPrototype();
    numHops++;
  } while (obj);

  // Missing property.
  if (entry) {
    if constexpr (HasOwn) {
      cache.initEntryForMissingOwnProperty(entry, receiverShape, id);
    } else {
      cache.initEntryForMissingProperty(entry, receiverShape, id);
    }
  }
  vp[1].setBoolean(false);
  return true;
}

template bool HasNativeDataPropertyPure<true>(JSContext* cx, JSObject* obj,
                                              MegamorphicCacheEntry* entry,
                                              Value* vp);

template bool HasNativeDataPropertyPure<false>(JSContext* cx, JSObject* obj,
                                               MegamorphicCacheEntry* entry,
                                               Value* vp);

bool HasNativeElementPure(JSContext* cx, NativeObject* obj, int32_t index,
                          Value* vp) {
  AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(obj->is<NativeObject>());
  MOZ_ASSERT(!obj->getOpsHasProperty());
  MOZ_ASSERT(!obj->getOpsLookupProperty());
  MOZ_ASSERT(!obj->getOpsGetOwnPropertyDescriptor());

  if (MOZ_UNLIKELY(index < 0)) {
    return false;
  }

  if (obj->containsDenseElement(index)) {
    vp[0].setBoolean(true);
    return true;
  }

  jsid id = PropertyKey::Int(index);
  uint32_t unused;
  if (obj->shape()->lookup(cx, id, &unused)) {
    vp[0].setBoolean(true);
    return true;
  }

  // Fail if there's a resolve hook, unless the mayResolve hook tells
  // us the resolve hook won't define a property with this id.
  if (MOZ_UNLIKELY(ClassMayResolveId(cx->names(), obj->getClass(), id, obj))) {
    return false;
  }
  // TypedArrayObject are also native and contain indexed properties.
  if (MOZ_UNLIKELY(obj->is<TypedArrayObject>())) {
    size_t length = obj->as<TypedArrayObject>().length().valueOr(0);
    vp[0].setBoolean(uint32_t(index) < length);
    return true;
  }

  vp[0].setBoolean(false);
  return true;
}

// Fast path for setting/adding a plain object property. This is the common case
// for megamorphic SetProp/SetElem.
template <bool UseCache>
static bool TryAddOrSetPlainObjectProperty(JSContext* cx,
                                           Handle<PlainObject*> obj,
                                           PropertyKey key, HandleValue value,
                                           bool* optimized) {
  MOZ_ASSERT(!*optimized);

  Shape* receiverShape = obj->shape();
  MegamorphicSetPropCache& cache = *cx->caches().megamorphicSetPropCache;

#ifdef DEBUG
  if constexpr (UseCache) {
    MegamorphicSetPropCache::Entry* entry;
    if (cache.lookup(receiverShape, key, &entry)) {
      if (entry->afterShape() != nullptr) {  // AddProp
        NativeObject* holder = nullptr;
        PropertyResult prop;
        MOZ_ASSERT(LookupPropertyPure(cx, obj, key, &holder, &prop));
        MOZ_ASSERT(obj != holder);
        MOZ_ASSERT_IF(prop.isFound(),
                      prop.isNativeProperty() &&
                          prop.propertyInfo().isDataProperty() &&
                          prop.propertyInfo().writable());
      } else {  // SetProp
        mozilla::Maybe<PropertyInfo> prop = obj->lookupPure(key);
        MOZ_ASSERT(prop.isSome());
        MOZ_ASSERT(prop->isDataProperty());
        MOZ_ASSERT(obj->getTaggedSlotOffset(prop->slot()) ==
                   entry->slotOffset());
      }
    }
  }
#endif

  // Fast path for changing a data property.
  uint32_t index;
  if (PropMap* map = obj->shape()->lookup(cx, key, &index)) {
    PropertyInfo prop = map->getPropertyInfo(index);
    if (!prop.isDataProperty() || !prop.writable()) {
      return true;
    }
    obj->setSlot(prop.slot(), value);
    if (!Watchtower::watchPropertyModification<AllowGC::NoGC>(cx, obj, key)) {
      return false;
    }
    *optimized = true;

    if constexpr (UseCache) {
      TaggedSlotOffset offset = obj->getTaggedSlotOffset(prop.slot());
      cache.set(receiverShape, nullptr, key, offset, 0);
    }
    return true;
  }

  // Don't support "__proto__". This lets us take advantage of the
  // hasNonWritableOrAccessorPropExclProto optimization below.
  if (MOZ_UNLIKELY(!obj->isExtensible() || key.isAtom(cx->names().proto_))) {
    return true;
  }

  // Ensure the proto chain contains only plain objects. Deoptimize for accessor
  // properties and non-writable data properties (we can't shadow non-writable
  // properties).
  JSObject* proto = obj->staticPrototype();
  while (proto) {
    if (!proto->is<PlainObject>()) {
      return true;
    }
    PlainObject* plainProto = &proto->as<PlainObject>();
    if (plainProto->hasNonWritableOrAccessorPropExclProto()) {
      uint32_t index;
      if (PropMap* map = plainProto->shape()->lookup(cx, key, &index)) {
        PropertyInfo prop = map->getPropertyInfo(index);
        if (!prop.isDataProperty() || !prop.writable()) {
          return true;
        }
        break;
      }
    }
    proto = plainProto->staticPrototype();
  }

#ifdef DEBUG
  // At this point either the property is missing or it's a writable data
  // property on the proto chain that we can shadow.
  {
    NativeObject* holder = nullptr;
    PropertyResult prop;
    MOZ_ASSERT(LookupPropertyPure(cx, obj, key, &holder, &prop));
    MOZ_ASSERT(obj != holder);
    MOZ_ASSERT_IF(prop.isFound(), prop.isNativeProperty() &&
                                      prop.propertyInfo().isDataProperty() &&
                                      prop.propertyInfo().writable());
  }
#endif

  *optimized = true;
  Rooted<PropertyKey> keyRoot(cx, key);
  Rooted<Shape*> receiverShapeRoot(cx, receiverShape);
  uint32_t resultSlot = 0;
  size_t numDynamic = obj->numDynamicSlots();
  bool res = AddDataPropertyToPlainObject(cx, obj, keyRoot, value, &resultSlot);

  if constexpr (UseCache) {
    if (res && obj->shape()->isShared() &&
        resultSlot < SharedPropMap::MaxPropsForNonDictionary &&
        !Watchtower::watchesPropertyAdd(obj)) {
      TaggedSlotOffset offset = obj->getTaggedSlotOffset(resultSlot);
      uint32_t newCapacity = 0;
      if (!(resultSlot < obj->numFixedSlots() ||
            (resultSlot - obj->numFixedSlots()) < numDynamic)) {
        newCapacity = obj->numDynamicSlots();
      }
      cache.set(receiverShapeRoot, obj->shape(), keyRoot, offset, newCapacity);
    }
  }

  return res;
}

template <bool Cached>
bool SetElementMegamorphic(JSContext* cx, HandleObject obj, HandleValue index,
                           HandleValue value, bool strict) {
  if (obj->is<PlainObject>()) {
    PropertyKey key;
    if (ValueToAtomOrSymbolPure(cx, index, &key)) {
      bool optimized = false;
      if (!TryAddOrSetPlainObjectProperty<Cached>(cx, obj.as<PlainObject>(),
                                                  key, value, &optimized)) {
        return false;
      }
      if (optimized) {
        return true;
      }
    }
  }
  Rooted<Value> receiver(cx, ObjectValue(*obj));
  return SetObjectElementWithReceiver(cx, obj, index, value, receiver, strict);
}

template bool SetElementMegamorphic<false>(JSContext* cx, HandleObject obj,
                                           HandleValue index, HandleValue value,
                                           bool strict);
template bool SetElementMegamorphic<true>(JSContext* cx, HandleObject obj,
                                          HandleValue index, HandleValue value,
                                          bool strict);

template <bool Cached>
bool SetPropertyMegamorphic(JSContext* cx, HandleObject obj, HandleId id,
                            HandleValue value, bool strict) {
  if (obj->is<PlainObject>()) {
    bool optimized = false;
    if (!TryAddOrSetPlainObjectProperty<Cached>(cx, obj.as<PlainObject>(), id,
                                                value, &optimized)) {
      return false;
    }
    if (optimized) {
      return true;
    }
  }
  Rooted<Value> receiver(cx, ObjectValue(*obj));
  ObjectOpResult result;
  return SetProperty(cx, obj, id, value, receiver, result) &&
         result.checkStrictModeError(cx, obj, id, strict);
}

template bool SetPropertyMegamorphic<false>(JSContext* cx, HandleObject obj,
                                            HandleId id, HandleValue value,
                                            bool strict);
template bool SetPropertyMegamorphic<true>(JSContext* cx, HandleObject obj,
                                           HandleId id, HandleValue value,
                                           bool strict);

void HandleCodeCoverageAtPC(BaselineFrame* frame, jsbytecode* pc) {
  AutoUnsafeCallWithABI unsafe(UnsafeABIStrictness::AllowPendingExceptions);

  MOZ_ASSERT(frame->runningInInterpreter());

  JSScript* script = frame->script();
  MOZ_ASSERT(pc == script->main() || BytecodeIsJumpTarget(JSOp(*pc)));

  if (!script->hasScriptCounts()) {
    if (!script->realm()->collectCoverageForDebug()) {
      return;
    }
    JSContext* cx = script->runtimeFromMainThread()->mainContextFromOwnThread();
    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (!script->initScriptCounts(cx)) {
      oomUnsafe.crash("initScriptCounts");
    }
  }

  PCCounts* counts = script->maybeGetPCCounts(pc);
  MOZ_ASSERT(counts);
  counts->numExec()++;
}

void HandleCodeCoverageAtPrologue(BaselineFrame* frame) {
  AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(frame->runningInInterpreter());

  JSScript* script = frame->script();
  jsbytecode* main = script->main();
  if (!BytecodeIsJumpTarget(JSOp(*main))) {
    HandleCodeCoverageAtPC(frame, main);
  }
}

JSString* TypeOfNameObject(JSObject* obj, JSRuntime* rt) {
  AutoUnsafeCallWithABI unsafe;
  JSType type = js::TypeOfObject(obj);
  return TypeName(type, *rt->commonNames);
}

bool TypeOfEqObject(JSObject* obj, TypeofEqOperand operand) {
  AutoUnsafeCallWithABI unsafe;
  bool result = js::TypeOfObject(obj) == operand.type();
  if (operand.compareOp() == JSOp::Ne) {
    result = !result;
  }
  return result;
}

bool GetPrototypeOf(JSContext* cx, HandleObject target,
                    MutableHandleValue rval) {
  MOZ_ASSERT(target->hasDynamicPrototype());

  RootedObject proto(cx);
  if (!GetPrototype(cx, target, &proto)) {
    return false;
  }
  rval.setObjectOrNull(proto);
  return true;
}

static JSString* ConvertObjectToStringForConcat(JSContext* cx,
                                                HandleValue obj) {
  MOZ_ASSERT(obj.isObject());
  RootedValue rootedObj(cx, obj);
  if (!ToPrimitive(cx, &rootedObj)) {
    return nullptr;
  }
  return ToString<CanGC>(cx, rootedObj);
}

bool DoConcatStringObject(JSContext* cx, HandleValue lhs, HandleValue rhs,
                          MutableHandleValue res) {
  JSString* lstr = nullptr;
  JSString* rstr = nullptr;

  if (lhs.isString()) {
    // Convert rhs first.
    MOZ_ASSERT(lhs.isString() && rhs.isObject());
    rstr = ConvertObjectToStringForConcat(cx, rhs);
    if (!rstr) {
      return false;
    }

    // lhs is already string.
    lstr = lhs.toString();
  } else {
    MOZ_ASSERT(rhs.isString() && lhs.isObject());
    // Convert lhs first.
    lstr = ConvertObjectToStringForConcat(cx, lhs);
    if (!lstr) {
      return false;
    }

    // rhs is already string.
    rstr = rhs.toString();
  }

  JSString* str = ConcatStrings<NoGC>(cx, lstr, rstr);
  if (!str) {
    RootedString nlstr(cx, lstr), nrstr(cx, rstr);
    str = ConcatStrings<CanGC>(cx, nlstr, nrstr);
    if (!str) {
      return false;
    }
  }

  res.setString(str);
  return true;
}

bool IsPossiblyWrappedTypedArray(JSContext* cx, JSObject* obj, bool* result) {
  JSObject* unwrapped = CheckedUnwrapDynamic(obj, cx);
  if (!unwrapped) {
    ReportAccessDenied(cx);
    return false;
  }

  *result = unwrapped->is<TypedArrayObject>();
  return true;
}

// Called from CreateDependentString::generateFallback.
void* AllocateDependentString(JSContext* cx) {
  AutoUnsafeCallWithABI unsafe;
  return cx->newCell<JSDependentString, NoGC>(js::gc::Heap::Default);
}
void* AllocateFatInlineString(JSContext* cx) {
  AutoUnsafeCallWithABI unsafe;
  return cx->newCell<JSFatInlineString, NoGC>(js::gc::Heap::Default);
}

// Called to allocate a BigInt if inline allocation failed.
void* AllocateBigIntNoGC(JSContext* cx, bool requestMinorGC) {
  AutoUnsafeCallWithABI unsafe;

  if (requestMinorGC && cx->nursery().isEnabled()) {
    cx->nursery().requestMinorGC(JS::GCReason::OUT_OF_NURSERY);
  }

  return cx->newCell<JS::BigInt, NoGC>(js::gc::Heap::Tenured);
}

void AllocateAndInitTypedArrayBuffer(JSContext* cx, TypedArrayObject* obj,
                                     int32_t count) {
  AutoUnsafeCallWithABI unsafe;

  // Initialize the data slot to UndefinedValue to signal to our JIT caller that
  // the allocation failed if the slot isn't overwritten below.
  obj->initFixedSlot(TypedArrayObject::DATA_SLOT, UndefinedValue());

  // Negative numbers or zero will bail out to the slow path, which in turn will
  // raise an invalid argument exception or create a correct object with zero
  // elements.
  constexpr size_t byteLengthLimit = TypedArrayObject::ByteLengthLimit;
  if (count <= 0 || size_t(count) > byteLengthLimit / obj->bytesPerElement()) {
    obj->setFixedSlot(TypedArrayObject::LENGTH_SLOT, PrivateValue(size_t(0)));
    return;
  }

  obj->setFixedSlot(TypedArrayObject::LENGTH_SLOT, PrivateValue(count));

  size_t nbytes = size_t(count) * obj->bytesPerElement();
  MOZ_ASSERT(nbytes <= byteLengthLimit);
  nbytes = RoundUp(nbytes, sizeof(Value));

  MOZ_ASSERT(!obj->isTenured());
  void* buf = cx->nursery().allocateZeroedBuffer(obj, nbytes,
                                                 js::ArrayBufferContentsArena);
  if (buf) {
    InitReservedSlot(obj, TypedArrayObject::DATA_SLOT, buf, nbytes,
                     MemoryUse::TypedArrayElements);
  }
}

#ifdef JS_GC_PROBES
void TraceCreateObject(JSObject* obj) {
  AutoUnsafeCallWithABI unsafe;
  js::gc::gcprobes::CreateObject(obj);
}
#endif

#if JS_BITS_PER_WORD == 32
BigInt* CreateBigIntFromInt64(JSContext* cx, uint32_t low, uint32_t high) {
  uint64_t n = (static_cast<uint64_t>(high) << 32) + low;
  return js::BigInt::createFromInt64(cx, n);
}

BigInt* CreateBigIntFromUint64(JSContext* cx, uint32_t low, uint32_t high) {
  uint64_t n = (static_cast<uint64_t>(high) << 32) + low;
  return js::BigInt::createFromUint64(cx, n);
}
#else
BigInt* CreateBigIntFromInt64(JSContext* cx, uint64_t i64) {
  return js::BigInt::createFromInt64(cx, i64);
}

BigInt* CreateBigIntFromUint64(JSContext* cx, uint64_t i64) {
  return js::BigInt::createFromUint64(cx, i64);
}
#endif

bool DoStringToInt64(JSContext* cx, HandleString str, uint64_t* res) {
  BigInt* bi;
  JS_TRY_VAR_OR_RETURN_FALSE(cx, bi, js::StringToBigInt(cx, str));

  if (!bi) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BIGINT_INVALID_SYNTAX);
    return false;
  }

  *res = js::BigInt::toUint64(bi);
  return true;
}

template <EqualityKind Kind>
bool BigIntEqual(BigInt* x, BigInt* y) {
  AutoUnsafeCallWithABI unsafe;
  bool res = BigInt::equal(x, y);
  if (Kind != EqualityKind::Equal) {
    res = !res;
  }
  return res;
}

template bool BigIntEqual<EqualityKind::Equal>(BigInt* x, BigInt* y);
template bool BigIntEqual<EqualityKind::NotEqual>(BigInt* x, BigInt* y);

template <ComparisonKind Kind>
bool BigIntCompare(BigInt* x, BigInt* y) {
  AutoUnsafeCallWithABI unsafe;
  bool res = BigInt::lessThan(x, y);
  if (Kind != ComparisonKind::LessThan) {
    res = !res;
  }
  return res;
}

template bool BigIntCompare<ComparisonKind::LessThan>(BigInt* x, BigInt* y);
template bool BigIntCompare<ComparisonKind::GreaterThanOrEqual>(BigInt* x,
                                                                BigInt* y);

template <EqualityKind Kind>
bool BigIntNumberEqual(BigInt* x, double y) {
  AutoUnsafeCallWithABI unsafe;
  bool res = BigInt::equal(x, y);
  if (Kind != EqualityKind::Equal) {
    res = !res;
  }
  return res;
}

template bool BigIntNumberEqual<EqualityKind::Equal>(BigInt* x, double y);
template bool BigIntNumberEqual<EqualityKind::NotEqual>(BigInt* x, double y);

template <ComparisonKind Kind>
bool BigIntNumberCompare(BigInt* x, double y) {
  AutoUnsafeCallWithABI unsafe;
  mozilla::Maybe<bool> res = BigInt::lessThan(x, y);
  if (Kind == ComparisonKind::LessThan) {
    return res.valueOr(false);
  }
  return !res.valueOr(true);
}

template bool BigIntNumberCompare<ComparisonKind::LessThan>(BigInt* x,
                                                            double y);
template bool BigIntNumberCompare<ComparisonKind::GreaterThanOrEqual>(BigInt* x,
                                                                      double y);

template <ComparisonKind Kind>
bool NumberBigIntCompare(double x, BigInt* y) {
  AutoUnsafeCallWithABI unsafe;
  mozilla::Maybe<bool> res = BigInt::lessThan(x, y);
  if (Kind == ComparisonKind::LessThan) {
    return res.valueOr(false);
  }
  return !res.valueOr(true);
}

template bool NumberBigIntCompare<ComparisonKind::LessThan>(double x,
                                                            BigInt* y);
template bool NumberBigIntCompare<ComparisonKind::GreaterThanOrEqual>(
    double x, BigInt* y);

template <EqualityKind Kind>
bool BigIntStringEqual(JSContext* cx, HandleBigInt x, HandleString y,
                       bool* res) {
  JS_TRY_VAR_OR_RETURN_FALSE(cx, *res, BigInt::equal(cx, x, y));
  if (Kind != EqualityKind::Equal) {
    *res = !*res;
  }
  return true;
}

template bool BigIntStringEqual<EqualityKind::Equal>(JSContext* cx,
                                                     HandleBigInt x,
                                                     HandleString y, bool* res);
template bool BigIntStringEqual<EqualityKind::NotEqual>(JSContext* cx,
                                                        HandleBigInt x,
                                                        HandleString y,
                                                        bool* res);

bool InstantiatedBigIntStringEqual(JSContext* cx, HandleBigInt x,
                                   HandleString y, bool* res) {
  return BigIntStringEqual<EqualityKind::Equal>(cx, x, y, res);
}

bool InstantiatedBigIntStringNotEqual(JSContext* cx, HandleBigInt x,
                                      HandleString y, bool* res) {
  return BigIntStringEqual<EqualityKind::NotEqual>(cx, x, y, res);
}

template <ComparisonKind Kind>
bool BigIntStringCompare(JSContext* cx, HandleBigInt x, HandleString y,
                         bool* res) {
  mozilla::Maybe<bool> result;
  if (!BigInt::lessThan(cx, x, y, result)) {
    return false;
  }
  if (Kind == ComparisonKind::LessThan) {
    *res = result.valueOr(false);
  } else {
    *res = !result.valueOr(true);
  }
  return true;
}

template bool BigIntStringCompare<ComparisonKind::LessThan>(JSContext* cx,
                                                            HandleBigInt x,
                                                            HandleString y,
                                                            bool* res);
template bool BigIntStringCompare<ComparisonKind::GreaterThanOrEqual>(
    JSContext* cx, HandleBigInt x, HandleString y, bool* res);

bool InstantiatedBigIntStringLessThan(JSContext* cx, HandleBigInt x,
                                      HandleString y, bool* res) {
  return BigIntStringCompare<ComparisonKind::LessThan>(cx, x, y, res);
}

bool InstantiatedBigIntStringGreaterThanOrEqual(JSContext* cx, HandleBigInt x,
                                                HandleString y, bool* res) {
  return BigIntStringCompare<ComparisonKind::GreaterThanOrEqual>(cx, x, y, res);
}

template <ComparisonKind Kind>
bool StringBigIntCompare(JSContext* cx, HandleString x, HandleBigInt y,
                         bool* res) {
  mozilla::Maybe<bool> result;
  if (!BigInt::lessThan(cx, x, y, result)) {
    return false;
  }
  if (Kind == ComparisonKind::LessThan) {
    *res = result.valueOr(false);
  } else {
    *res = !result.valueOr(true);
  }
  return true;
}

template bool StringBigIntCompare<ComparisonKind::LessThan>(JSContext* cx,
                                                            HandleString x,
                                                            HandleBigInt y,
                                                            bool* res);
template bool StringBigIntCompare<ComparisonKind::GreaterThanOrEqual>(
    JSContext* cx, HandleString x, HandleBigInt y, bool* res);

bool InstantiatedStringBigIntLessThan(JSContext* cx, HandleString x,
                                      HandleBigInt y, bool* res) {
  return StringBigIntCompare<ComparisonKind::LessThan>(cx, x, y, res);
}

bool InstantiatedStringBigIntGreaterThanOrEqual(JSContext* cx, HandleString x,
                                                HandleBigInt y, bool* res) {
  return StringBigIntCompare<ComparisonKind::GreaterThanOrEqual>(cx, x, y, res);
}

BigInt* BigIntAsIntN(JSContext* cx, HandleBigInt x, int32_t bits) {
  MOZ_ASSERT(bits >= 0);
  return BigInt::asIntN(cx, x, uint64_t(bits));
}

BigInt* BigIntAsUintN(JSContext* cx, HandleBigInt x, int32_t bits) {
  MOZ_ASSERT(bits >= 0);
  return BigInt::asUintN(cx, x, uint64_t(bits));
}

template <typename T>
static int32_t AtomicsCompareExchange(TypedArrayObject* typedArray,
                                      size_t index, int32_t expected,
                                      int32_t replacement) {
  AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(!typedArray->hasDetachedBuffer());
  MOZ_ASSERT_IF(typedArray->hasResizableBuffer(), !typedArray->isOutOfBounds());
  MOZ_ASSERT(index < typedArray->length().valueOr(0));

  SharedMem<T*> addr = typedArray->dataPointerEither().cast<T*>();
  return jit::AtomicOperations::compareExchangeSeqCst(addr + index, T(expected),
                                                      T(replacement));
}

AtomicsCompareExchangeFn AtomicsCompareExchange(Scalar::Type elementType) {
  switch (elementType) {
    case Scalar::Int8:
      return AtomicsCompareExchange<int8_t>;
    case Scalar::Uint8:
      return AtomicsCompareExchange<uint8_t>;
    case Scalar::Int16:
      return AtomicsCompareExchange<int16_t>;
    case Scalar::Uint16:
      return AtomicsCompareExchange<uint16_t>;
    case Scalar::Int32:
      return AtomicsCompareExchange<int32_t>;
    case Scalar::Uint32:
      return AtomicsCompareExchange<uint32_t>;
    default:
      MOZ_CRASH("Unexpected TypedArray type");
  }
}

template <typename T>
static int32_t AtomicsExchange(TypedArrayObject* typedArray, size_t index,
                               int32_t value) {
  AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(!typedArray->hasDetachedBuffer());
  MOZ_ASSERT_IF(typedArray->hasResizableBuffer(), !typedArray->isOutOfBounds());
  MOZ_ASSERT(index < typedArray->length().valueOr(0));

  SharedMem<T*> addr = typedArray->dataPointerEither().cast<T*>();
  return jit::AtomicOperations::exchangeSeqCst(addr + index, T(value));
}

AtomicsReadWriteModifyFn AtomicsExchange(Scalar::Type elementType) {
  switch (elementType) {
    case Scalar::Int8:
      return AtomicsExchange<int8_t>;
    case Scalar::Uint8:
      return AtomicsExchange<uint8_t>;
    case Scalar::Int16:
      return AtomicsExchange<int16_t>;
    case Scalar::Uint16:
      return AtomicsExchange<uint16_t>;
    case Scalar::Int32:
      return AtomicsExchange<int32_t>;
    case Scalar::Uint32:
      return AtomicsExchange<uint32_t>;
    default:
      MOZ_CRASH("Unexpected TypedArray type");
  }
}

template <typename T>
static int32_t AtomicsAdd(TypedArrayObject* typedArray, size_t index,
                          int32_t value) {
  AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(!typedArray->hasDetachedBuffer());
  MOZ_ASSERT_IF(typedArray->hasResizableBuffer(), !typedArray->isOutOfBounds());
  MOZ_ASSERT(index < typedArray->length().valueOr(0));

  SharedMem<T*> addr = typedArray->dataPointerEither().cast<T*>();
  return jit::AtomicOperations::fetchAddSeqCst(addr + index, T(value));
}

AtomicsReadWriteModifyFn AtomicsAdd(Scalar::Type elementType) {
  switch (elementType) {
    case Scalar::Int8:
      return AtomicsAdd<int8_t>;
    case Scalar::Uint8:
      return AtomicsAdd<uint8_t>;
    case Scalar::Int16:
      return AtomicsAdd<int16_t>;
    case Scalar::Uint16:
      return AtomicsAdd<uint16_t>;
    case Scalar::Int32:
      return AtomicsAdd<int32_t>;
    case Scalar::Uint32:
      return AtomicsAdd<uint32_t>;
    default:
      MOZ_CRASH("Unexpected TypedArray type");
  }
}

template <typename T>
static int32_t AtomicsSub(TypedArrayObject* typedArray, size_t index,
                          int32_t value) {
  AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(!typedArray->hasDetachedBuffer());
  MOZ_ASSERT_IF(typedArray->hasResizableBuffer(), !typedArray->isOutOfBounds());
  MOZ_ASSERT(index < typedArray->length().valueOr(0));

  SharedMem<T*> addr = typedArray->dataPointerEither().cast<T*>();
  return jit::AtomicOperations::fetchSubSeqCst(addr + index, T(value));
}

AtomicsReadWriteModifyFn AtomicsSub(Scalar::Type elementType) {
  switch (elementType) {
    case Scalar::Int8:
      return AtomicsSub<int8_t>;
    case Scalar::Uint8:
      return AtomicsSub<uint8_t>;
    case Scalar::Int16:
      return AtomicsSub<int16_t>;
    case Scalar::Uint16:
      return AtomicsSub<uint16_t>;
    case Scalar::Int32:
      return AtomicsSub<int32_t>;
    case Scalar::Uint32:
      return AtomicsSub<uint32_t>;
    default:
      MOZ_CRASH("Unexpected TypedArray type");
  }
}

template <typename T>
static int32_t AtomicsAnd(TypedArrayObject* typedArray, size_t index,
                          int32_t value) {
  AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(!typedArray->hasDetachedBuffer());
  MOZ_ASSERT_IF(typedArray->hasResizableBuffer(), !typedArray->isOutOfBounds());
  MOZ_ASSERT(index < typedArray->length().valueOr(0));

  SharedMem<T*> addr = typedArray->dataPointerEither().cast<T*>();
  return jit::AtomicOperations::fetchAndSeqCst(addr + index, T(value));
}

AtomicsReadWriteModifyFn AtomicsAnd(Scalar::Type elementType) {
  switch (elementType) {
    case Scalar::Int8:
      return AtomicsAnd<int8_t>;
    case Scalar::Uint8:
      return AtomicsAnd<uint8_t>;
    case Scalar::Int16:
      return AtomicsAnd<int16_t>;
    case Scalar::Uint16:
      return AtomicsAnd<uint16_t>;
    case Scalar::Int32:
      return AtomicsAnd<int32_t>;
    case Scalar::Uint32:
      return AtomicsAnd<uint32_t>;
    default:
      MOZ_CRASH("Unexpected TypedArray type");
  }
}

template <typename T>
static int32_t AtomicsOr(TypedArrayObject* typedArray, size_t index,
                         int32_t value) {
  AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(!typedArray->hasDetachedBuffer());
  MOZ_ASSERT_IF(typedArray->hasResizableBuffer(), !typedArray->isOutOfBounds());
  MOZ_ASSERT(index < typedArray->length().valueOr(0));

  SharedMem<T*> addr = typedArray->dataPointerEither().cast<T*>();
  return jit::AtomicOperations::fetchOrSeqCst(addr + index, T(value));
}

AtomicsReadWriteModifyFn AtomicsOr(Scalar::Type elementType) {
  switch (elementType) {
    case Scalar::Int8:
      return AtomicsOr<int8_t>;
    case Scalar::Uint8:
      return AtomicsOr<uint8_t>;
    case Scalar::Int16:
      return AtomicsOr<int16_t>;
    case Scalar::Uint16:
      return AtomicsOr<uint16_t>;
    case Scalar::Int32:
      return AtomicsOr<int32_t>;
    case Scalar::Uint32:
      return AtomicsOr<uint32_t>;
    default:
      MOZ_CRASH("Unexpected TypedArray type");
  }
}

template <typename T>
static int32_t AtomicsXor(TypedArrayObject* typedArray, size_t index,
                          int32_t value) {
  AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(!typedArray->hasDetachedBuffer());
  MOZ_ASSERT_IF(typedArray->hasResizableBuffer(), !typedArray->isOutOfBounds());
  MOZ_ASSERT(index < typedArray->length().valueOr(0));

  SharedMem<T*> addr = typedArray->dataPointerEither().cast<T*>();
  return jit::AtomicOperations::fetchXorSeqCst(addr + index, T(value));
}

AtomicsReadWriteModifyFn AtomicsXor(Scalar::Type elementType) {
  switch (elementType) {
    case Scalar::Int8:
      return AtomicsXor<int8_t>;
    case Scalar::Uint8:
      return AtomicsXor<uint8_t>;
    case Scalar::Int16:
      return AtomicsXor<int16_t>;
    case Scalar::Uint16:
      return AtomicsXor<uint16_t>;
    case Scalar::Int32:
      return AtomicsXor<int32_t>;
    case Scalar::Uint32:
      return AtomicsXor<uint32_t>;
    default:
      MOZ_CRASH("Unexpected TypedArray type");
  }
}

template <typename AtomicOp, typename... Args>
static BigInt* AtomicAccess64(JSContext* cx, TypedArrayObject* typedArray,
                              size_t index, AtomicOp op, Args... args) {
  MOZ_ASSERT(Scalar::isBigIntType(typedArray->type()));
  MOZ_ASSERT(!typedArray->hasDetachedBuffer());
  MOZ_ASSERT_IF(typedArray->hasResizableBuffer(), !typedArray->isOutOfBounds());
  MOZ_ASSERT(index < typedArray->length().valueOr(0));

  if (typedArray->type() == Scalar::BigInt64) {
    SharedMem<int64_t*> addr = typedArray->dataPointerEither().cast<int64_t*>();
    int64_t v = op(addr + index, BigInt::toInt64(args)...);
    return BigInt::createFromInt64(cx, v);
  }

  SharedMem<uint64_t*> addr = typedArray->dataPointerEither().cast<uint64_t*>();
  uint64_t v = op(addr + index, BigInt::toUint64(args)...);
  return BigInt::createFromUint64(cx, v);
}

template <typename AtomicOp, typename... Args>
static auto AtomicAccess64(TypedArrayObject* typedArray, size_t index,
                           AtomicOp op, Args... args) {
  MOZ_ASSERT(Scalar::isBigIntType(typedArray->type()));
  MOZ_ASSERT(!typedArray->hasDetachedBuffer());
  MOZ_ASSERT_IF(typedArray->hasResizableBuffer(), !typedArray->isOutOfBounds());
  MOZ_ASSERT(index < typedArray->length().valueOr(0));

  if (typedArray->type() == Scalar::BigInt64) {
    SharedMem<int64_t*> addr = typedArray->dataPointerEither().cast<int64_t*>();
    return op(addr + index, BigInt::toInt64(args)...);
  }

  SharedMem<uint64_t*> addr = typedArray->dataPointerEither().cast<uint64_t*>();
  return op(addr + index, BigInt::toUint64(args)...);
}

BigInt* AtomicsLoad64(JSContext* cx, TypedArrayObject* typedArray,
                      size_t index) {
  return AtomicAccess64(cx, typedArray, index, [](auto addr) {
    return jit::AtomicOperations::loadSeqCst(addr);
  });
}

void AtomicsStore64(TypedArrayObject* typedArray, size_t index,
                    const BigInt* value) {
  AutoUnsafeCallWithABI unsafe;

  AtomicAccess64(
      typedArray, index,
      [](auto addr, auto val) {
        jit::AtomicOperations::storeSeqCst(addr, val);
      },
      value);
}

BigInt* AtomicsCompareExchange64(JSContext* cx, TypedArrayObject* typedArray,
                                 size_t index, const BigInt* expected,
                                 const BigInt* replacement) {
  return AtomicAccess64(
      cx, typedArray, index,
      [](auto addr, auto oldval, auto newval) {
        return jit::AtomicOperations::compareExchangeSeqCst(addr, oldval,
                                                            newval);
      },
      expected, replacement);
}

BigInt* AtomicsExchange64(JSContext* cx, TypedArrayObject* typedArray,
                          size_t index, const BigInt* value) {
  return AtomicAccess64(
      cx, typedArray, index,
      [](auto addr, auto val) {
        return jit::AtomicOperations::exchangeSeqCst(addr, val);
      },
      value);
}

BigInt* AtomicsAdd64(JSContext* cx, TypedArrayObject* typedArray, size_t index,
                     const BigInt* value) {
  return AtomicAccess64(
      cx, typedArray, index,
      [](auto addr, auto val) {
        return jit::AtomicOperations::fetchAddSeqCst(addr, val);
      },
      value);
}

BigInt* AtomicsAnd64(JSContext* cx, TypedArrayObject* typedArray, size_t index,
                     const BigInt* value) {
  return AtomicAccess64(
      cx, typedArray, index,
      [](auto addr, auto val) {
        return jit::AtomicOperations::fetchAndSeqCst(addr, val);
      },
      value);
}

BigInt* AtomicsOr64(JSContext* cx, TypedArrayObject* typedArray, size_t index,
                    const BigInt* value) {
  return AtomicAccess64(
      cx, typedArray, index,
      [](auto addr, auto val) {
        return jit::AtomicOperations::fetchOrSeqCst(addr, val);
      },
      value);
}

BigInt* AtomicsSub64(JSContext* cx, TypedArrayObject* typedArray, size_t index,
                     const BigInt* value) {
  return AtomicAccess64(
      cx, typedArray, index,
      [](auto addr, auto val) {
        return jit::AtomicOperations::fetchSubSeqCst(addr, val);
      },
      value);
}

BigInt* AtomicsXor64(JSContext* cx, TypedArrayObject* typedArray, size_t index,
                     const BigInt* value) {
  return AtomicAccess64(
      cx, typedArray, index,
      [](auto addr, auto val) {
        return jit::AtomicOperations::fetchXorSeqCst(addr, val);
      },
      value);
}

JSAtom* AtomizeStringNoGC(JSContext* cx, JSString* str) {
  // IC code calls this directly so we shouldn't GC.
  AutoUnsafeCallWithABI unsafe;

  JSAtom* atom = AtomizeString(cx, str);
  if (!atom) {
    cx->recoverFromOutOfMemory();
    return nullptr;
  }

  return atom;
}

bool SetObjectHas(JSContext* cx, HandleObject obj, HandleValue key,
                  bool* rval) {
  return SetObject::has(cx, obj, key, rval);
}

bool MapObjectHas(JSContext* cx, HandleObject obj, HandleValue key,
                  bool* rval) {
  return MapObject::has(cx, obj, key, rval);
}

bool MapObjectGet(JSContext* cx, HandleObject obj, HandleValue key,
                  MutableHandleValue rval) {
  return MapObject::get(cx, obj, key, rval);
}

#ifdef DEBUG
template <class OrderedHashTable>
static mozilla::HashNumber HashValue(JSContext* cx, OrderedHashTable* hashTable,
                                     const Value* value) {
  RootedValue rootedValue(cx, *value);
  HashableValue hashable;
  MOZ_ALWAYS_TRUE(hashable.setValue(cx, rootedValue));

  return hashTable->hash(hashable);
}
#endif

void AssertSetObjectHash(JSContext* cx, SetObject* obj, const Value* value,
                         mozilla::HashNumber actualHash) {
  AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(actualHash == HashValue(cx, obj->getData(), value));
}

void AssertMapObjectHash(JSContext* cx, MapObject* obj, const Value* value,
                         mozilla::HashNumber actualHash) {
  AutoUnsafeCallWithABI unsafe;

  MOZ_ASSERT(actualHash == HashValue(cx, obj->getData(), value));
}

void AssertPropertyLookup(NativeObject* obj, PropertyKey id, uint32_t slot) {
  AutoUnsafeCallWithABI unsafe;
#ifdef DEBUG
  mozilla::Maybe<PropertyInfo> prop = obj->lookupPure(id);
  MOZ_ASSERT(prop.isSome());
  MOZ_ASSERT(prop->slot() == slot);
#else
  MOZ_CRASH("This should only be called in debug builds.");
#endif
}

void AssumeUnreachable(const char* output) {
  MOZ_ReportAssertionFailure(output, __FILE__, __LINE__);
}

void Printf0(const char* output) {
  AutoUnsafeCallWithABI unsafe;

  // Use stderr instead of stdout because this is only used for debug
  // output. stderr is less likely to interfere with the program's normal
  // output, and it's always unbuffered.
  fprintf(stderr, "%s", output);
}

void Printf1(const char* output, uintptr_t value) {
  AutoUnsafeCallWithABI unsafe;
  AutoEnterOOMUnsafeRegion oomUnsafe;
  js::UniqueChars line = JS_sprintf_append(nullptr, output, value);
  if (!line) {
    oomUnsafe.crash("OOM at masm.printf");
  }
  fprintf(stderr, "%s", line.get());
}

}  // namespace jit
}  // namespace js
