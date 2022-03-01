/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_VMFunctions_h
#define jit_VMFunctions_h

#include "mozilla/Assertions.h"

#include <stddef.h>
#include <stdint.h>

#include "jstypes.h"
#include "NamespaceImports.h"

#include "gc/Rooting.h"
#include "jit/IonTypes.h"
#include "js/ScalarType.h"
#include "js/TypeDecls.h"

class JSJitInfo;
class JSLinearString;

namespace js {

class AbstractGeneratorObject;
class GlobalObject;
class InterpreterFrame;
class LexicalScope;
class ClassBodyScope;
class NativeObject;
class PropertyName;
class Shape;
class TypedArrayObject;
class WithScope;

namespace gc {

struct Cell;

}

namespace jit {

class BaselineFrame;
class InterpreterStubExitFrameLayout;

enum DataType : uint8_t {
  Type_Void,
  Type_Bool,
  Type_Int32,
  Type_Double,
  Type_Pointer,
  Type_Object,
  Type_Value,
  Type_Handle
};

enum MaybeTailCall : bool { TailCall, NonTailCall };

// [SMDOC] JIT-to-C++ Function Calls. (callVM)
//
// Sometimes it is easier to reuse C++ code by calling VM's functions. Calling a
// function from the VM can be achieved with the use of callWithABI but this is
// discouraged when the called functions might trigger exceptions and/or
// garbage collections which are expecting to walk the stack. VMFunctions and
// callVM are interfaces provided to handle the exception handling and register
// the stack end (JITActivation) such that walking the stack is made possible.
//
// VMFunctionData is a structure which contains the necessary information needed
// for generating a trampoline function to make a call (with generateVMWrapper)
// and to root the arguments of the function (in TraceJitExitFrame).
// VMFunctionData is created with the VMFunctionDataHelper template, which
// infers the VMFunctionData fields from the function signature. The rooting and
// trampoline code is therefore determined by the arguments of a function and
// their locations in the signature of a function.
//
// VM functions all expect a JSContext* as first argument. This argument is
// implicitly provided by the trampoline code (in generateVMWrapper) and used
// for creating new objects or reporting errors. If your function does not make
// use of a JSContext* argument, then you might probably use a callWithABI
// call.
//
// Functions described using the VMFunction system must conform to a simple
// protocol: the return type must have a special "failure" value (for example,
// false for bool, or nullptr for Objects). If the function is designed to
// return a value that does not meet this requirement - such as
// object-or-nullptr, or an integer, an optional, final outParam can be
// specified. In this case, the return type must be boolean to indicate
// failure.
//
// JIT Code usage:
//
// Different JIT compilers in SpiderMonkey have their own implementations of
// callVM to call VM functions. However, the general shape of them is that
// arguments (excluding the JSContext or trailing out-param) are pushed on to
// the stack from right to left (rightmost argument is pushed first).
//
// Regardless of return value protocol being used (final outParam, or return
// value) the generated trampolines ensure the return value ends up in
// JSReturnOperand, ReturnReg or ReturnDoubleReg.
//
// Example:
//
// The details will differ slightly between the different compilers in
// SpiderMonkey, but the general shape of our usage looks like this:
//
// Suppose we have a function Foo:
//
//      bool Foo(JSContext* cx, HandleObject x, HandleId y,
//               MutableHandleValue z);
//
// This function returns true on success, and z is the outparam return value.
//
// A VM function wrapper for this can be created by adding an entry to
// VM_FUNCTION_LIST in VMFunctionList-inl.h:
//
//    _(Foo, js::Foo)
//
// In the compiler code the call would then be issued like this:
//
//      masm.Push(id);
//      masm.Push(obj);
//
//      using Fn = bool (*)(JSContext*, HandleObject, HandleId,
//                          MutableHandleValue);
//      if (!callVM<Fn, js::Foo>()) {
//          return false;
//      }
//
// After this, the result value is in the return value register.

// Data for a VM function. All VMFunctionDatas are stored in a constexpr array.
struct VMFunctionData {
#if defined(DEBUG) || defined(JS_JITSPEW) || defined(JS_TRACE_LOGGING)
  // Informative name of the wrapped function. The name should not be present
  // in release builds in order to save memory.
  const char* name_;
#endif

  // Note: a maximum of seven root types is supported.
  enum RootType : uint8_t {
    RootNone = 0,
    RootObject,
    RootString,
    RootId,
    RootFunction,
    RootValue,
    RootCell,
    RootBigInt
  };

  // Contains an combination of enumerated types used by the gc for marking
  // arguments of the VM wrapper.
  uint64_t argumentRootTypes;

  enum ArgProperties {
    WordByValue = 0,
    DoubleByValue = 1,
    WordByRef = 2,
    DoubleByRef = 3,
    // BitMask version.
    Word = 0,
    Double = 1,
    ByRef = 2
  };

  // Contains properties about the first 16 arguments.
  uint32_t argumentProperties;

  // Which arguments should be passed in float register on platforms that
  // have them.
  uint32_t argumentPassedInFloatRegs;

  // Number of arguments expected, excluding JSContext * as an implicit
  // first argument and an outparam as a possible implicit final argument.
  uint8_t explicitArgs;

  // The root type of the out param if outParam == Type_Handle.
  RootType outParamRootType;

  // The outparam may be any Type_*, and must be the final argument to the
  // function, if not Void. outParam != Void implies that the return type
  // has a boolean failure mode.
  DataType outParam;

  // Type returned by the C function and used by the VMFunction wrapper to
  // check for failures of the C function.  Valid failure/return types are
  // boolean and object pointers which are asserted inside the VMFunction
  // constructor. If the C function use an outparam (!= Type_Void), then
  // the only valid failure/return type is boolean -- object pointers are
  // pointless because the wrapper will only use it to compare it against
  // nullptr before discarding its value.
  DataType returnType;

  // Number of Values the VM wrapper should pop from the stack when it returns.
  // Used by baseline IC stubs so that they can use tail calls to call the VM
  // wrapper.
  uint8_t extraValuesToPop;

  // On some architectures, called functions need to explicitly push their
  // return address, for a tail call, there is nothing to push, so tail-callness
  // needs to be known at compile time.
  MaybeTailCall expectTailCall;

  uint32_t argc() const {
    // JSContext * + args + (OutParam? *)
    return 1 + explicitArgc() + ((outParam == Type_Void) ? 0 : 1);
  }

  DataType failType() const { return returnType; }

  // Whether this function returns anything more than a boolean flag for
  // failures.
  bool returnsData() const {
    return returnType == Type_Object || outParam != Type_Void;
  }

  ArgProperties argProperties(uint32_t explicitArg) const {
    return ArgProperties((argumentProperties >> (2 * explicitArg)) & 3);
  }

  RootType argRootType(uint32_t explicitArg) const {
    return RootType((argumentRootTypes >> (3 * explicitArg)) & 7);
  }

  bool argPassedInFloatReg(uint32_t explicitArg) const {
    return ((argumentPassedInFloatRegs >> explicitArg) & 1) == 1;
  }

#if defined(DEBUG) || defined(JS_JITSPEW) || defined(JS_TRACE_LOGGING)
  const char* name() const { return name_; }
#endif

  // Return the stack size consumed by explicit arguments.
  size_t explicitStackSlots() const {
    size_t stackSlots = explicitArgs;

    // Fetch all double-word flags of explicit arguments.
    uint32_t n = ((1 << (explicitArgs * 2)) - 1)  // = Explicit argument mask.
                 & 0x55555555                     // = Mask double-size args.
                 & argumentProperties;

    // Add the number of double-word flags. (expect a few loop
    // iteration)
    while (n) {
      stackSlots++;
      n &= n - 1;
    }
    return stackSlots;
  }

  // Double-size argument which are passed by value are taking the space
  // of 2 C arguments.  This function is used to compute the number of
  // argument expected by the C function.  This is not the same as
  // explicitStackSlots because reference to stack slots may take one less
  // register in the total count.
  size_t explicitArgc() const {
    size_t stackSlots = explicitArgs;

    // Fetch all explicit arguments.
    uint32_t n = ((1 << (explicitArgs * 2)) - 1)  // = Explicit argument mask.
                 & argumentProperties;

    // Filter double-size arguments (0x5 = 0b0101) and remove (& ~)
    // arguments passed by reference (0b1010 >> 1 == 0b0101).
    n = (n & 0x55555555) & ~(n >> 1);

    // Add the number of double-word transfered by value. (expect a few
    // loop iteration)
    while (n) {
      stackSlots++;
      n &= n - 1;
    }
    return stackSlots;
  }

  size_t doubleByRefArgs() const {
    size_t count = 0;

    // Fetch all explicit arguments.
    uint32_t n = ((1 << (explicitArgs * 2)) - 1)  // = Explicit argument mask.
                 & argumentProperties;

    // Filter double-size arguments (0x5 = 0b0101) and take (&) only
    // arguments passed by reference (0b1010 >> 1 == 0b0101).
    n = (n & 0x55555555) & (n >> 1);

    // Add the number of double-word transfered by refference. (expect a
    // few loop iterations)
    while (n) {
      count++;
      n &= n - 1;
    }
    return count;
  }

  constexpr VMFunctionData(const char* name, uint32_t explicitArgs,
                           uint32_t argumentProperties,
                           uint32_t argumentPassedInFloatRegs,
                           uint64_t argRootTypes, DataType outParam,
                           RootType outParamRootType, DataType returnType,
                           uint8_t extraValuesToPop = 0,
                           MaybeTailCall expectTailCall = NonTailCall)
      :
#if defined(DEBUG) || defined(JS_JITSPEW) || defined(JS_TRACE_LOGGING)
        name_(name),
#endif
        argumentRootTypes(argRootTypes),
        argumentProperties(argumentProperties),
        argumentPassedInFloatRegs(argumentPassedInFloatRegs),
        explicitArgs(explicitArgs),
        outParamRootType(outParamRootType),
        outParam(outParam),
        returnType(returnType),
        extraValuesToPop(extraValuesToPop),
        expectTailCall(expectTailCall) {
    // Check for valid failure/return type.
    MOZ_ASSERT_IF(outParam != Type_Void,
                  returnType == Type_Void || returnType == Type_Bool);
    MOZ_ASSERT(returnType == Type_Void || returnType == Type_Bool ||
               returnType == Type_Object);
  }

  constexpr VMFunctionData(const VMFunctionData& o) = default;
};

// Extract the last element of a list of types.
template <typename... ArgTypes>
struct LastArg;

template <>
struct LastArg<> {
  using Type = void;
};

template <typename HeadType>
struct LastArg<HeadType> {
  using Type = HeadType;
};

template <typename HeadType, typename... TailTypes>
struct LastArg<HeadType, TailTypes...> {
  using Type = typename LastArg<TailTypes...>::Type;
};

[[nodiscard]] bool InvokeFunction(JSContext* cx, HandleObject obj0,
                                  bool constructing, bool ignoresReturnValue,
                                  uint32_t argc, Value* argv,
                                  MutableHandleValue rval);

class InterpreterStubExitFrameLayout;
bool InvokeFromInterpreterStub(JSContext* cx,
                               InterpreterStubExitFrameLayout* frame);
void* GetContextSensitiveInterpreterStub();

bool CheckOverRecursed(JSContext* cx);
bool CheckOverRecursedBaseline(JSContext* cx, BaselineFrame* frame);

[[nodiscard]] bool MutatePrototype(JSContext* cx, HandlePlainObject obj,
                                   HandleValue value);

enum class EqualityKind : bool { NotEqual, Equal };

template <EqualityKind Kind>
bool StringsEqual(JSContext* cx, HandleString lhs, HandleString rhs, bool* res);

enum class ComparisonKind : bool { GreaterThanOrEqual, LessThan };

template <ComparisonKind Kind>
bool StringsCompare(JSContext* cx, HandleString lhs, HandleString rhs,
                    bool* res);

[[nodiscard]] bool ArrayPushDense(JSContext* cx, HandleArrayObject arr,
                                  HandleValue v, uint32_t* length);
JSString* ArrayJoin(JSContext* cx, HandleObject array, HandleString sep);
[[nodiscard]] bool SetArrayLength(JSContext* cx, HandleObject obj,
                                  HandleValue value, bool strict);

[[nodiscard]] bool CharCodeAt(JSContext* cx, HandleString str, int32_t index,
                              uint32_t* code);
JSLinearString* StringFromCharCode(JSContext* cx, int32_t code);
JSLinearString* StringFromCharCodeNoGC(JSContext* cx, int32_t code);
JSString* StringFromCodePoint(JSContext* cx, int32_t codePoint);

[[nodiscard]] bool SetProperty(JSContext* cx, HandleObject obj,
                               HandlePropertyName name, HandleValue value,
                               bool strict, jsbytecode* pc);

[[nodiscard]] bool InterruptCheck(JSContext* cx);

JSObject* NewCallObject(JSContext* cx, HandleShape shape);
JSObject* NewStringObject(JSContext* cx, HandleString str);

bool OperatorIn(JSContext* cx, HandleValue key, HandleObject obj, bool* out);

[[nodiscard]] bool GetIntrinsicValue(JSContext* cx, HandlePropertyName name,
                                     MutableHandleValue rval);

[[nodiscard]] bool CreateThisFromIC(JSContext* cx, HandleObject callee,
                                    HandleObject newTarget,
                                    MutableHandleValue rval);
[[nodiscard]] bool CreateThisFromIon(JSContext* cx, HandleObject callee,
                                     HandleObject newTarget,
                                     MutableHandleValue rval);

void PostWriteBarrier(JSRuntime* rt, js::gc::Cell* cell);
void PostGlobalWriteBarrier(JSRuntime* rt, GlobalObject* obj);

enum class IndexInBounds { Yes, Maybe };

template <IndexInBounds InBounds>
void PostWriteElementBarrier(JSRuntime* rt, JSObject* obj, int32_t index);

// If |str| represents an int32, assign it to |result| and return true.
// Otherwise return false.
bool GetInt32FromStringPure(JSContext* cx, JSString* str, int32_t* result);

// If |str| is an index in the range [0, INT32_MAX], return it. If the string
// is not an index in this range, return -1.
int32_t GetIndexFromString(JSString* str);

JSObject* WrapObjectPure(JSContext* cx, JSObject* obj);

[[nodiscard]] bool DebugPrologue(JSContext* cx, BaselineFrame* frame);
[[nodiscard]] bool DebugEpilogue(JSContext* cx, BaselineFrame* frame,
                                 jsbytecode* pc, bool ok);
[[nodiscard]] bool DebugEpilogueOnBaselineReturn(JSContext* cx,
                                                 BaselineFrame* frame,
                                                 jsbytecode* pc);
void FrameIsDebuggeeCheck(BaselineFrame* frame);

JSObject* CreateGeneratorFromFrame(JSContext* cx, BaselineFrame* frame);
JSObject* CreateGenerator(JSContext* cx, HandleFunction, HandleScript,
                          HandleObject, HandleObject);

[[nodiscard]] bool NormalSuspend(JSContext* cx, HandleObject obj,
                                 BaselineFrame* frame, uint32_t frameSize,
                                 jsbytecode* pc);
[[nodiscard]] bool FinalSuspend(JSContext* cx, HandleObject obj,
                                jsbytecode* pc);
[[nodiscard]] bool InterpretResume(JSContext* cx, HandleObject obj,
                                   Value* stackValues, MutableHandleValue rval);
[[nodiscard]] bool DebugAfterYield(JSContext* cx, BaselineFrame* frame);
[[nodiscard]] bool GeneratorThrowOrReturn(
    JSContext* cx, BaselineFrame* frame,
    Handle<AbstractGeneratorObject*> genObj, HandleValue arg,
    int32_t resumeKindArg);

[[nodiscard]] bool GlobalDeclInstantiationFromIon(JSContext* cx,
                                                  HandleScript script,
                                                  jsbytecode* pc);
[[nodiscard]] bool InitFunctionEnvironmentObjects(JSContext* cx,
                                                  BaselineFrame* frame);

[[nodiscard]] bool NewArgumentsObject(JSContext* cx, BaselineFrame* frame,
                                      MutableHandleValue res);

JSObject* CopyLexicalEnvironmentObject(JSContext* cx, HandleObject env,
                                       bool copySlots);

JSObject* InitRestParameter(JSContext* cx, uint32_t length, Value* rest,
                            HandleObject res);

[[nodiscard]] bool HandleDebugTrap(JSContext* cx, BaselineFrame* frame,
                                   uint8_t* retAddr);
[[nodiscard]] bool OnDebuggerStatement(JSContext* cx, BaselineFrame* frame);
[[nodiscard]] bool GlobalHasLiveOnDebuggerStatement(JSContext* cx);

[[nodiscard]] bool EnterWith(JSContext* cx, BaselineFrame* frame,
                             HandleValue val, Handle<WithScope*> templ);
[[nodiscard]] bool LeaveWith(JSContext* cx, BaselineFrame* frame);

[[nodiscard]] bool PushLexicalEnv(JSContext* cx, BaselineFrame* frame,
                                  Handle<LexicalScope*> scope);
[[nodiscard]] bool PushClassBodyEnv(JSContext* cx, BaselineFrame* frame,
                                    Handle<ClassBodyScope*> scope);
[[nodiscard]] bool PopLexicalEnv(JSContext* cx, BaselineFrame* frame);
[[nodiscard]] bool DebugLeaveThenPopLexicalEnv(JSContext* cx,
                                               BaselineFrame* frame,
                                               jsbytecode* pc);
[[nodiscard]] bool FreshenLexicalEnv(JSContext* cx, BaselineFrame* frame);
[[nodiscard]] bool DebugLeaveThenFreshenLexicalEnv(JSContext* cx,
                                                   BaselineFrame* frame,
                                                   jsbytecode* pc);
[[nodiscard]] bool RecreateLexicalEnv(JSContext* cx, BaselineFrame* frame);
[[nodiscard]] bool DebugLeaveThenRecreateLexicalEnv(JSContext* cx,
                                                    BaselineFrame* frame,
                                                    jsbytecode* pc);
[[nodiscard]] bool DebugLeaveLexicalEnv(JSContext* cx, BaselineFrame* frame,
                                        jsbytecode* pc);

[[nodiscard]] bool PushVarEnv(JSContext* cx, BaselineFrame* frame,
                              HandleScope scope);

[[nodiscard]] bool InitBaselineFrameForOsr(BaselineFrame* frame,
                                           InterpreterFrame* interpFrame,
                                           uint32_t numStackValues);

JSString* StringReplace(JSContext* cx, HandleString string,
                        HandleString pattern, HandleString repl);

[[nodiscard]] bool SetDenseElement(JSContext* cx, HandleNativeObject obj,
                                   int32_t index, HandleValue value,
                                   bool strict);

void AssertValidBigIntPtr(JSContext* cx, JS::BigInt* bi);
void AssertValidObjectPtr(JSContext* cx, JSObject* obj);
void AssertValidStringPtr(JSContext* cx, JSString* str);
void AssertValidSymbolPtr(JSContext* cx, JS::Symbol* sym);
void AssertValidValue(JSContext* cx, Value* v);

void JitValuePreWriteBarrier(JSRuntime* rt, Value* vp);
void JitStringPreWriteBarrier(JSRuntime* rt, JSString** stringp);
void JitObjectPreWriteBarrier(JSRuntime* rt, JSObject** objp);
void JitShapePreWriteBarrier(JSRuntime* rt, Shape** shapep);

bool ObjectIsCallable(JSObject* obj);
bool ObjectIsConstructor(JSObject* obj);

[[nodiscard]] bool ThrowRuntimeLexicalError(JSContext* cx,
                                            unsigned errorNumber);

[[nodiscard]] bool ThrowBadDerivedReturn(JSContext* cx, HandleValue v);

[[nodiscard]] bool ThrowBadDerivedReturnOrUninitializedThis(JSContext* cx,
                                                            HandleValue v);

[[nodiscard]] bool BaselineGetFunctionThis(JSContext* cx, BaselineFrame* frame,
                                           MutableHandleValue res);

[[nodiscard]] bool CallNativeGetter(JSContext* cx, HandleFunction callee,
                                    HandleValue receiver,
                                    MutableHandleValue result);

bool CallDOMGetter(JSContext* cx, const JSJitInfo* jitInfo, HandleObject obj,
                   MutableHandleValue result);

bool CallDOMSetter(JSContext* cx, const JSJitInfo* jitInfo, HandleObject obj,
                   HandleValue value);

[[nodiscard]] bool CallNativeSetter(JSContext* cx, HandleFunction callee,
                                    HandleObject obj, HandleValue rhs);

[[nodiscard]] bool EqualStringsHelperPure(JSString* str1, JSString* str2);

void HandleCodeCoverageAtPC(BaselineFrame* frame, jsbytecode* pc);
void HandleCodeCoverageAtPrologue(BaselineFrame* frame);

bool GetNativeDataPropertyPure(JSContext* cx, JSObject* obj, PropertyName* name,
                               Value* vp);

bool GetNativeDataPropertyByValuePure(JSContext* cx, JSObject* obj, Value* vp);

template <bool HasOwn>
bool HasNativeDataPropertyPure(JSContext* cx, JSObject* obj, Value* vp);

bool HasNativeElementPure(JSContext* cx, NativeObject* obj, int32_t index,
                          Value* vp);

bool SetNativeDataPropertyPure(JSContext* cx, JSObject* obj, PropertyName* name,
                               Value* val);

bool ObjectHasGetterSetterPure(JSContext* cx, JSObject* objArg, jsid id,
                               GetterSetter* getterSetter);

JSString* TypeOfObject(JSObject* obj, JSRuntime* rt);

bool GetPrototypeOf(JSContext* cx, HandleObject target,
                    MutableHandleValue rval);

bool DoConcatStringObject(JSContext* cx, HandleValue lhs, HandleValue rhs,
                          MutableHandleValue res);

bool IsPossiblyWrappedTypedArray(JSContext* cx, JSObject* obj, bool* result);

void* AllocateString(JSContext* cx);
void* AllocateFatInlineString(JSContext* cx);
void* AllocateBigIntNoGC(JSContext* cx, bool requestMinorGC);
void AllocateAndInitTypedArrayBuffer(JSContext* cx, TypedArrayObject* obj,
                                     int32_t count);

void* CreateMatchResultFallbackFunc(JSContext* cx, gc::AllocKind kind,
                                    size_t nDynamicSlots);
#ifdef JS_GC_PROBES
void TraceCreateObject(JSObject* obj);
#endif

bool DoStringToInt64(JSContext* cx, HandleString str, uint64_t* res);

#if JS_BITS_PER_WORD == 32
BigInt* CreateBigIntFromInt64(JSContext* cx, uint32_t low, uint32_t high);
BigInt* CreateBigIntFromUint64(JSContext* cx, uint32_t low, uint32_t high);
#else
BigInt* CreateBigIntFromInt64(JSContext* cx, uint64_t i64);
BigInt* CreateBigIntFromUint64(JSContext* cx, uint64_t i64);
#endif

template <EqualityKind Kind>
bool BigIntEqual(BigInt* x, BigInt* y);

template <ComparisonKind Kind>
bool BigIntCompare(BigInt* x, BigInt* y);

template <EqualityKind Kind>
bool BigIntNumberEqual(BigInt* x, double y);

template <ComparisonKind Kind>
bool BigIntNumberCompare(BigInt* x, double y);

template <ComparisonKind Kind>
bool NumberBigIntCompare(double x, BigInt* y);

template <EqualityKind Kind>
bool BigIntStringEqual(JSContext* cx, HandleBigInt x, HandleString y,
                       bool* res);

template <ComparisonKind Kind>
bool BigIntStringCompare(JSContext* cx, HandleBigInt x, HandleString y,
                         bool* res);

template <ComparisonKind Kind>
bool StringBigIntCompare(JSContext* cx, HandleString x, HandleBigInt y,
                         bool* res);

BigInt* BigIntAsIntN(JSContext* cx, HandleBigInt x, int32_t bits);
BigInt* BigIntAsUintN(JSContext* cx, HandleBigInt x, int32_t bits);

using AtomicsCompareExchangeFn = int32_t (*)(TypedArrayObject*, size_t, int32_t,
                                             int32_t);

using AtomicsReadWriteModifyFn = int32_t (*)(TypedArrayObject*, size_t,
                                             int32_t);

AtomicsCompareExchangeFn AtomicsCompareExchange(Scalar::Type elementType);
AtomicsReadWriteModifyFn AtomicsExchange(Scalar::Type elementType);
AtomicsReadWriteModifyFn AtomicsAdd(Scalar::Type elementType);
AtomicsReadWriteModifyFn AtomicsSub(Scalar::Type elementType);
AtomicsReadWriteModifyFn AtomicsAnd(Scalar::Type elementType);
AtomicsReadWriteModifyFn AtomicsOr(Scalar::Type elementType);
AtomicsReadWriteModifyFn AtomicsXor(Scalar::Type elementType);

BigInt* AtomicsLoad64(JSContext* cx, TypedArrayObject* typedArray,
                      size_t index);

void AtomicsStore64(TypedArrayObject* typedArray, size_t index, BigInt* value);

BigInt* AtomicsCompareExchange64(JSContext* cx, TypedArrayObject* typedArray,
                                 size_t index, BigInt* expected,
                                 BigInt* replacement);

BigInt* AtomicsExchange64(JSContext* cx, TypedArrayObject* typedArray,
                          size_t index, BigInt* value);

BigInt* AtomicsAdd64(JSContext* cx, TypedArrayObject* typedArray, size_t index,
                     BigInt* value);
BigInt* AtomicsAnd64(JSContext* cx, TypedArrayObject* typedArray, size_t index,
                     BigInt* value);
BigInt* AtomicsOr64(JSContext* cx, TypedArrayObject* typedArray, size_t index,
                    BigInt* value);
BigInt* AtomicsSub64(JSContext* cx, TypedArrayObject* typedArray, size_t index,
                     BigInt* value);
BigInt* AtomicsXor64(JSContext* cx, TypedArrayObject* typedArray, size_t index,
                     BigInt* value);

// Functions used when JS_MASM_VERBOSE is enabled.
void AssumeUnreachable(const char* output);
void Printf0(const char* output);
void Printf1(const char* output, uintptr_t value);

enum class TailCallVMFunctionId;
enum class VMFunctionId;

extern const VMFunctionData& GetVMFunction(VMFunctionId id);
extern const VMFunctionData& GetVMFunction(TailCallVMFunctionId id);

}  // namespace jit
}  // namespace js

#if defined(JS_CODEGEN_ARM)
extern "C" {
extern MOZ_EXPORT int64_t __aeabi_idivmod(int, int);
extern MOZ_EXPORT int64_t __aeabi_uidivmod(int, int);
}
#endif

#endif /* jit_VMFunctions_h */
