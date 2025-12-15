/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CacheIRWriter_h
#define jit_CacheIRWriter_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Casting.h"
#include "mozilla/Maybe.h"

#include <stddef.h>
#include <stdint.h>

#include "jstypes.h"
#include "NamespaceImports.h"

#include "gc/AllocKind.h"
#include "jit/ABIFunctions.h"
#include "jit/CacheIR.h"
#include "jit/CacheIROpsGenerated.h"
#include "jit/CompactBuffer.h"
#include "jit/ICState.h"
#include "jit/Simulator.h"
#include "jit/TypeData.h"
#include "js/AllocPolicy.h"
#include "js/CallArgs.h"
#include "js/Class.h"
#include "js/experimental/JitInfo.h"
#include "js/Id.h"
#include "js/RootingAPI.h"
#include "js/ScalarType.h"
#include "js/Value.h"
#include "js/Vector.h"
#include "util/Memory.h"
#include "vm/GuardFuse.h"
#include "vm/JSFunction.h"
#include "vm/JSScript.h"
#include "vm/List.h"
#include "vm/Opcodes.h"
#include "vm/RealmFuses.h"
#include "vm/Shape.h"
#include "vm/TypeofEqOperand.h"  // TypeofEqOperand
#include "wasm/WasmConstants.h"
#include "wasm/WasmValType.h"

class JS_PUBLIC_API JSTracer;
struct JS_PUBLIC_API JSContext;

class JSObject;
class JSString;

namespace JS {
class Symbol;
}

namespace js {

class GetterSetter;
enum class UnaryMathFunction : uint8_t;

namespace gc {
class AllocSite;
}

namespace jit {

class ICScript;
struct CacheIRAOTStub;

// Class to record CacheIR + some additional metadata for code generation.
class MOZ_RAII CacheIRWriter : public JS::CustomAutoRooter {
#ifdef DEBUG
  JSContext* cx_;
#endif
  CompactBufferWriter buffer_;

  uint32_t nextOperandId_;
  uint32_t nextInstructionId_;
  uint32_t numInputOperands_;

  TypeData typeData_;

  // The data (shapes, slot offsets, etc.) that will be stored in the ICStub.
  Vector<StubField, 8, SystemAllocPolicy> stubFields_;
  size_t stubDataSize_;

  // For each operand id, record which instruction accessed it last. This
  // information greatly improves register allocation.
  Vector<uint32_t, 8, SystemAllocPolicy> operandLastUsed_;

  // OperandId and stub offsets are stored in a single byte, so make sure
  // this doesn't overflow. We use a very conservative limit for now.
  static const size_t MaxOperandIds = 20;
  static const size_t MaxStubDataSizeInBytes = 20 * sizeof(uintptr_t);
  bool tooLarge_;

  // Assume this stub can't be trial inlined until we see a scripted call/inline
  // instruction.
  TrialInliningState trialInliningState_ = TrialInliningState::Failure;

  // Basic caching to avoid quadatic lookup behaviour in readStubField.
  mutable uint32_t lastOffset_;
  mutable uint32_t lastIndex_;

#ifdef DEBUG
  // Information for assertLengthMatches.
  mozilla::Maybe<CacheOp> currentOp_;
  size_t currentOpArgsStart_ = 0;
#endif

#ifdef DEBUG
  void assertSameCompartment(JSObject* obj);
  void assertSameZone(Shape* shape);
#else
  void assertSameCompartment(JSObject* obj) {}
  void assertSameZone(Shape* shape) {}
#endif

  void writeOp(CacheOp op) {
    buffer_.writeFixedUint16_t(uint16_t(op));
    nextInstructionId_++;
#ifdef DEBUG
    MOZ_ASSERT(currentOp_.isNothing(), "Missing call to assertLengthMatches?");
    currentOp_.emplace(op);
    currentOpArgsStart_ = buffer_.length();
#endif
  }

  void assertLengthMatches() {
#ifdef DEBUG
    // After writing arguments, assert the length matches CacheIROpArgLengths.
    size_t expectedLen = CacheIROpInfos[size_t(*currentOp_)].argLength;
    MOZ_ASSERT_IF(!failed(),
                  buffer_.length() - currentOpArgsStart_ == expectedLen);
    currentOp_.reset();
#endif
  }

  void writeOperandId(OperandId opId) {
    if (opId.id() < MaxOperandIds) {
      static_assert(MaxOperandIds <= UINT8_MAX,
                    "operand id must fit in a single byte");
      buffer_.writeByte(opId.id());
    } else {
      tooLarge_ = true;
      return;
    }
    if (opId.id() >= operandLastUsed_.length()) {
      buffer_.propagateOOM(operandLastUsed_.resize(opId.id() + 1));
      if (buffer_.oom()) {
        return;
      }
    }
    MOZ_ASSERT(nextInstructionId_ > 0);
    operandLastUsed_[opId.id()] = nextInstructionId_ - 1;
  }

  void writeCallFlagsImm(CallFlags flags) { buffer_.writeByte(flags.toByte()); }

  void addStubField(uint64_t value, StubField::Type fieldType) {
    size_t fieldOffset = stubDataSize_;
#ifndef JS_64BIT
    // On 32-bit platforms there are two stub field sizes (4 bytes and 8 bytes).
    // Ensure 8-byte fields are properly aligned.
    if (StubField::sizeIsInt64(fieldType)) {
      fieldOffset = AlignBytes(fieldOffset, sizeof(uint64_t));
    }
#endif
    MOZ_ASSERT((fieldOffset % StubField::sizeInBytes(fieldType)) == 0);

    size_t newStubDataSize = fieldOffset + StubField::sizeInBytes(fieldType);
    if (newStubDataSize < MaxStubDataSizeInBytes) {
#ifndef JS_64BIT
      // Add a RawInt32 stub field for padding if necessary, because when we
      // iterate over the stub fields we assume there are no 'holes'.
      if (fieldOffset != stubDataSize_) {
        MOZ_ASSERT((stubDataSize_ + sizeof(uintptr_t)) == fieldOffset);
        buffer_.propagateOOM(
            stubFields_.append(StubField(0, StubField::Type::RawInt32)));
      }
#endif
      buffer_.propagateOOM(stubFields_.append(StubField(value, fieldType)));
      MOZ_ASSERT((fieldOffset % sizeof(uintptr_t)) == 0);
      buffer_.writeByte(fieldOffset / sizeof(uintptr_t));
      stubDataSize_ = newStubDataSize;
    } else {
      tooLarge_ = true;
    }
  }

  void writeShapeField(Shape* shape) {
    MOZ_ASSERT(shape);
    assertSameZone(shape);
    addStubField(uintptr_t(shape), StubField::Type::Shape);
  }
  void writeWeakShapeField(Shape* shape) {
    MOZ_ASSERT(shape);
    assertSameZone(shape);
    addStubField(uintptr_t(shape), StubField::Type::WeakShape);
  }
  void writeWeakGetterSetterField(GetterSetter* gs) {
    MOZ_ASSERT(gs);
    addStubField(uintptr_t(gs), StubField::Type::WeakGetterSetter);
  }
  void writeObjectField(JSObject* obj) {
    MOZ_ASSERT(obj);
    assertSameCompartment(obj);
    addStubField(uintptr_t(obj), StubField::Type::JSObject);
  }
  void writeWeakObjectField(JSObject* obj) {
    MOZ_ASSERT(obj);
    assertSameCompartment(obj);
    addStubField(uintptr_t(obj), StubField::Type::WeakObject);
  }
  void writeStringField(JSString* str) {
    MOZ_ASSERT(str);
    addStubField(uintptr_t(str), StubField::Type::String);
  }
  void writeSymbolField(JS::Symbol* sym) {
    MOZ_ASSERT(sym);
    addStubField(uintptr_t(sym), StubField::Type::Symbol);
  }
  void writeWeakBaseScriptField(BaseScript* script) {
    MOZ_ASSERT(script);
    addStubField(uintptr_t(script), StubField::Type::WeakBaseScript);
  }
  void writeJitCodeField(JitCode* code) {
    MOZ_ASSERT(code);
    addStubField(uintptr_t(code), StubField::Type::JitCode);
  }
  void writeRawInt32Field(uint32_t val) {
    addStubField(val, StubField::Type::RawInt32);
  }
  void writeRawPointerField(const void* ptr) {
    addStubField(uintptr_t(ptr), StubField::Type::RawPointer);
  }
  void writeIdField(jsid id) {
    addStubField(id.asRawBits(), StubField::Type::Id);
  }
  void writeValueField(const Value& val) {
    addStubField(val.asRawBits(), StubField::Type::Value);
  }
  void writeRawInt64Field(uint64_t val) {
    addStubField(val, StubField::Type::RawInt64);
  }
  void writeDoubleField(double d) {
    uint64_t bits = mozilla::BitwiseCast<uint64_t>(d);
    addStubField(bits, StubField::Type::Double);
  }
  void writeAllocSiteField(gc::AllocSite* ptr) {
    addStubField(uintptr_t(ptr), StubField::Type::AllocSite);
  }

  void writeJSOpImm(JSOp op) {
    static_assert(sizeof(JSOp) == sizeof(uint8_t), "JSOp must fit in a byte");
    buffer_.writeByte(uint8_t(op));
  }
  void writeTypeofEqOperandImm(TypeofEqOperand operand) {
    buffer_.writeByte(operand.rawValue());
  }
  void writeGuardClassKindImm(GuardClassKind kind) {
    static_assert(sizeof(GuardClassKind) == sizeof(uint8_t),
                  "GuardClassKind must fit in a byte");
    buffer_.writeByte(uint8_t(kind));
  }
  void writeArrayBufferViewKindImm(ArrayBufferViewKind kind) {
    static_assert(sizeof(ArrayBufferViewKind) == sizeof(uint8_t),
                  "ArrayBufferViewKind must fit in a byte");
    buffer_.writeByte(uint8_t(kind));
  }
  void writeValueTypeImm(ValueType type) {
    static_assert(sizeof(ValueType) == sizeof(uint8_t),
                  "ValueType must fit in uint8_t");
    buffer_.writeByte(uint8_t(type));
  }
  void writeJSWhyMagicImm(JSWhyMagic whyMagic) {
    static_assert(JS_WHY_MAGIC_COUNT <= UINT8_MAX,
                  "JSWhyMagic must fit in uint8_t");
    buffer_.writeByte(uint8_t(whyMagic));
  }
  void writeScalarTypeImm(Scalar::Type type) {
    MOZ_ASSERT(size_t(type) <= UINT8_MAX);
    buffer_.writeByte(uint8_t(type));
  }
  void writeUnaryMathFunctionImm(UnaryMathFunction fun) {
    static_assert(sizeof(UnaryMathFunction) == sizeof(uint8_t),
                  "UnaryMathFunction must fit in a byte");
    buffer_.writeByte(uint8_t(fun));
  }
  void writeCompletionKindImm(CompletionKind kind) {
    static_assert(sizeof(CompletionKind) == sizeof(uint8_t),
                  "CompletionKind must fit in a byte");
    buffer_.writeByte(uint8_t(kind));
  }
  void writeBoolImm(bool b) { buffer_.writeByte(uint32_t(b)); }
  void writeRealmFuseIndexImm(RealmFuses::FuseIndex realmFuseIndex) {
    static_assert(sizeof(RealmFuses::FuseIndex) == sizeof(uint8_t),
                  "RealmFuses::FuseIndex must fit in a byte");
    buffer_.writeByte(uint8_t(realmFuseIndex));
  }

  void writeByteImm(uint32_t b) {
    MOZ_ASSERT(b <= UINT8_MAX);
    buffer_.writeByte(b);
  }

  void writeInt32Imm(int32_t i32) { buffer_.writeFixedUint32_t(i32); }
  void writeUInt32Imm(uint32_t u32) { buffer_.writeFixedUint32_t(u32); }
  void writePointer(const void* ptr) { buffer_.writeRawPointer(ptr); }

  void writeJSNativeImm(JSNative native) {
    writePointer(JS_FUNC_TO_DATA_PTR(void*, native));
  }
  void writeStaticStringImm(const char* str) { writePointer(str); }

  void writeWasmValTypeImm(wasm::ValType::Kind kind) {
    static_assert(unsigned(wasm::TypeCode::Limit) <= UINT8_MAX);
    buffer_.writeByte(uint8_t(kind));
  }

  void writeAllocKindImm(gc::AllocKind kind) {
    static_assert(unsigned(gc::AllocKind::LIMIT) <= UINT8_MAX);
    buffer_.writeByte(uint8_t(kind));
  }

  uint32_t newOperandId() { return nextOperandId_++; }

  CacheIRWriter(const CacheIRWriter&) = delete;
  CacheIRWriter& operator=(const CacheIRWriter&) = delete;

 public:
  explicit CacheIRWriter(JSContext* cx)
      : CustomAutoRooter(cx),
#ifdef DEBUG
        cx_(cx),
#endif
        nextOperandId_(0),
        nextInstructionId_(0),
        numInputOperands_(0),
        stubDataSize_(0),
        tooLarge_(false),
        lastOffset_(0),
        lastIndex_(0) {
  }

#ifdef ENABLE_JS_AOT_ICS
  CacheIRWriter(JSContext* cx, const CacheIRAOTStub& aot);
#endif

  bool tooLarge() const { return tooLarge_; }
  bool oom() const { return buffer_.oom(); }
  bool failed() const { return tooLarge() || oom(); }

  TrialInliningState trialInliningState() const { return trialInliningState_; }

  uint32_t numInputOperands() const { return numInputOperands_; }
  uint32_t numOperandIds() const { return nextOperandId_; }
  uint32_t numInstructions() const { return nextInstructionId_; }

  size_t numStubFields() const { return stubFields_.length(); }
  const StubField& stubField(uint32_t i) const { return stubFields_[i]; }
  StubField::Type stubFieldType(uint32_t i) const {
    return stubFields_[i].type();
  }

  uint32_t setInputOperandId(uint32_t op) {
    MOZ_ASSERT(op == nextOperandId_);
    nextOperandId_++;
    numInputOperands_++;
    return op;
  }

  TypeData typeData() const { return typeData_; }
  void setTypeData(TypeData data) { typeData_ = data; }

  void trace(JSTracer* trc) override {
    // For now, assert we only GC before we append stub fields.
    MOZ_RELEASE_ASSERT(stubFields_.empty());
  }

  size_t stubDataSize() const { return stubDataSize_; }
  void copyStubData(uint8_t* dest) const;
  bool stubDataEquals(const uint8_t* stubData) const;
  bool stubDataEqualsIgnoring(const uint8_t* stubData,
                              uint32_t ignoreOffset) const;

  bool operandIsDead(uint32_t operandId, uint32_t currentInstruction) const {
    if (operandId >= operandLastUsed_.length()) {
      return false;
    }
    return currentInstruction > operandLastUsed_[operandId];
  }
  uint32_t operandLastUsed(uint32_t operandId) const {
    return operandLastUsed_[operandId];
  }

  const uint8_t* codeStart() const {
    MOZ_ASSERT(!failed());
    return buffer_.buffer();
  }

  const uint8_t* codeEnd() const {
    MOZ_ASSERT(!failed());
    return buffer_.buffer() + buffer_.length();
  }

  uint32_t codeLength() const {
    MOZ_ASSERT(!failed());
    return buffer_.length();
  }

  // This should not be used when compiling Baseline code, as Baseline code
  // shouldn't bake in stub values.
  StubField readStubField(uint32_t offset, StubField::Type type) const;

  ObjOperandId guardToObject(ValOperandId input) {
    guardToObject_(input);
    return ObjOperandId(input.id());
  }

  StringOperandId guardToString(ValOperandId input) {
    guardToString_(input);
    return StringOperandId(input.id());
  }

  SymbolOperandId guardToSymbol(ValOperandId input) {
    guardToSymbol_(input);
    return SymbolOperandId(input.id());
  }

  BigIntOperandId guardToBigInt(ValOperandId input) {
    guardToBigInt_(input);
    return BigIntOperandId(input.id());
  }

  BooleanOperandId guardToBoolean(ValOperandId input) {
    guardToBoolean_(input);
    return BooleanOperandId(input.id());
  }

  Int32OperandId guardToInt32(ValOperandId input) {
    guardToInt32_(input);
    return Int32OperandId(input.id());
  }

  NumberOperandId guardIsNumber(ValOperandId input) {
    guardIsNumber_(input);
    return NumberOperandId(input.id());
  }

  StringOperandId stringToAtom(StringOperandId input) {
    stringToAtom_(input);
    return input;
  }

  ValOperandId boxObject(ObjOperandId input) {
    return ValOperandId(input.id());
  }

  void guardShapeForClass(ObjOperandId obj, Shape* shape) {
    // Guard shape to ensure that object class is unchanged. This is true
    // for all shapes.
    guardShape(obj, shape);
  }

  void guardShapeForOwnProperties(ObjOperandId obj, Shape* shape) {
    // Guard shape to detect changes to (non-dense) own properties. This
    // also implies |guardShapeForClass|.
    MOZ_ASSERT(shape->getObjectClass()->isNativeObject());
    guardShape(obj, shape);
  }

 public:
  void guardSpecificFunction(ObjOperandId obj, JSFunction* expected) {
    // Guard object is a specific function. This implies immutable fields on
    // the JSFunction struct itself are unchanged.
    // Bake in the nargs and FunctionFlags so Warp can use them off-main thread,
    // instead of directly using the JSFunction fields.
    uint32_t nargsAndFlags = expected->flagsAndArgCountRaw();
    guardSpecificFunction_(obj, expected, nargsAndFlags);
  }

  void guardFunctionScript(ObjOperandId fun, BaseScript* expected) {
    // Guard function has a specific BaseScript. This implies immutable fields
    // on the JSFunction struct itself are unchanged and are equivalent for
    // lambda clones.
    // Bake in the nargs and FunctionFlags so Warp can use them off-main thread,
    // instead of directly using the JSFunction fields.
    uint32_t nargsAndFlags = expected->function()->flagsAndArgCountRaw();
    guardFunctionScript_(fun, expected, nargsAndFlags);
  }

  ValOperandId loadArgumentFixedSlot(ArgumentKind kind, uint32_t argc,
                                     CallFlags flags) {
    bool addArgc;
    int32_t slotIndex = GetIndexOfArgument(kind, flags, &addArgc);
    if (addArgc) {
      slotIndex += argc;
    }
    MOZ_ASSERT(slotIndex >= 0);
    MOZ_RELEASE_ASSERT(slotIndex <= UINT8_MAX);
    return loadArgumentFixedSlot_(slotIndex);
  }

  ValOperandId loadArgumentDynamicSlot(ArgumentKind kind, Int32OperandId argcId,
                                       CallFlags flags) {
    bool addArgc;
    int32_t slotIndex = GetIndexOfArgument(kind, flags, &addArgc);
    if (addArgc) {
      return loadArgumentDynamicSlot_(argcId, slotIndex);
    }
    return loadArgumentFixedSlot_(slotIndex);
  }

  ObjOperandId loadSpreadArgs() {
    ArgumentKind kind = ArgumentKind::Arg0;
    uint32_t argc = 1;
    CallFlags flags(CallFlags::Spread);
    return ObjOperandId(loadArgumentFixedSlot(kind, argc, flags).id());
  }

  void callScriptedFunction(ObjOperandId callee, Int32OperandId argc,
                            CallFlags flags, uint32_t argcFixed) {
    callScriptedFunction_(callee, argc, flags, argcFixed);
    trialInliningState_ = TrialInliningState::Candidate;
  }

  void callInlinedFunction(ObjOperandId callee, Int32OperandId argc,
                           ICScript* icScript, CallFlags flags,
                           uint32_t argcFixed) {
    callInlinedFunction_(callee, argc, icScript, flags, argcFixed);
    trialInliningState_ = TrialInliningState::Inlined;
  }

  void callNativeFunction(ObjOperandId calleeId, Int32OperandId argc, JSOp op,
                          JSFunction* calleeFunc, CallFlags flags,
                          uint32_t argcFixed) {
    // Some native functions can be implemented faster if we know that
    // the return value is ignored.
    bool ignoresReturnValue =
        op == JSOp::CallIgnoresRv && calleeFunc->hasJitInfo() &&
        calleeFunc->jitInfo()->type() == JSJitInfo::IgnoresReturnValueNative;

#ifdef JS_SIMULATOR
    // The simulator requires VM calls to be redirected to a special
    // swi instruction to handle them, so we store the redirected
    // pointer in the stub and use that instead of the original one.
    // If we are calling the ignoresReturnValue version of a native
    // function, we bake it into the redirected pointer.
    // (See BaselineCacheIRCompiler::emitCallNativeFunction.)
    JSNative target = ignoresReturnValue
                          ? calleeFunc->jitInfo()->ignoresReturnValueMethod
                          : calleeFunc->native();
    void* rawPtr = JS_FUNC_TO_DATA_PTR(void*, target);
    void* redirected = Simulator::RedirectNativeFunction(rawPtr, Args_General3);
    callNativeFunction_(calleeId, argc, flags, argcFixed, redirected);
#else
    // If we are not running in the simulator, we generate different jitcode
    // to find the ignoresReturnValue version of a native function.
    callNativeFunction_(calleeId, argc, flags, argcFixed, ignoresReturnValue);
#endif
  }

  void callDOMFunction(ObjOperandId calleeId, Int32OperandId argc,
                       ObjOperandId thisObjId, JSFunction* calleeFunc,
                       CallFlags flags, uint32_t argcFixed) {
#ifdef JS_SIMULATOR
    void* rawPtr = JS_FUNC_TO_DATA_PTR(void*, calleeFunc->native());
    void* redirected = Simulator::RedirectNativeFunction(rawPtr, Args_General3);
    callDOMFunction_(calleeId, argc, thisObjId, flags, argcFixed, redirected);
#else
    callDOMFunction_(calleeId, argc, thisObjId, flags, argcFixed);
#endif
  }

  void callDOMFunctionWithAllocSite(ObjOperandId calleeId, Int32OperandId argc,
                                    ObjOperandId thisObjId,
                                    JSFunction* calleeFunc, CallFlags flags,
                                    uint32_t argcFixed,
                                    gc::AllocSite* allocSite) {
#ifdef JS_SIMULATOR
    void* rawPtr = JS_FUNC_TO_DATA_PTR(void*, calleeFunc->native());
    void* redirected = Simulator::RedirectNativeFunction(rawPtr, Args_General3);
    callDOMFunctionWithAllocSite_(calleeId, argc, thisObjId, flags, argcFixed,
                                  allocSite, redirected);
#else
    callDOMFunctionWithAllocSite_(calleeId, argc, thisObjId, flags, argcFixed,
                                  allocSite);
#endif
  }

  void callAnyNativeFunction(ObjOperandId calleeId, Int32OperandId argc,
                             CallFlags flags, uint32_t argcFixed) {
    MOZ_ASSERT(!flags.isSameRealm());
#ifdef JS_SIMULATOR
    const void* redirected = RedirectedCallAnyNative();
    callNativeFunction_(calleeId, argc, flags, argcFixed, redirected);
#else
    callNativeFunction_(calleeId, argc, flags, argcFixed,
                        /* ignoresReturnValue = */ false);
#endif
  }

  void callClassHook(ObjOperandId calleeId, Int32OperandId argc, JSNative hook,
                     CallFlags flags, uint32_t argcFixed) {
    MOZ_ASSERT(!flags.isSameRealm());
    void* target = JS_FUNC_TO_DATA_PTR(void*, hook);
#ifdef JS_SIMULATOR
    // The simulator requires VM calls to be redirected to a special
    // swi instruction to handle them, so we store the redirected
    // pointer in the stub and use that instead of the original one.
    target = Simulator::RedirectNativeFunction(target, Args_General3);
#endif
    callClassHook_(calleeId, argc, flags, argcFixed, target);
  }

  void callScriptedGetterResult(ValOperandId receiver, JSFunction* getter,
                                bool sameRealm) {
    MOZ_ASSERT(getter->hasJitEntry());
    uint32_t nargsAndFlags = getter->flagsAndArgCountRaw();
    callScriptedGetterResult_(receiver, getter, sameRealm, nargsAndFlags);
    trialInliningState_ = TrialInliningState::Candidate;
  }

  void callInlinedGetterResult(ValOperandId receiver, JSFunction* getter,
                               ICScript* icScript, bool sameRealm) {
    MOZ_ASSERT(getter->hasJitEntry());
    uint32_t nargsAndFlags = getter->flagsAndArgCountRaw();
    callInlinedGetterResult_(receiver, getter, icScript, sameRealm,
                             nargsAndFlags);
    trialInliningState_ = TrialInliningState::Inlined;
  }

  void callNativeGetterResult(ValOperandId receiver, JSFunction* getter,
                              bool sameRealm) {
    MOZ_ASSERT(getter->isNativeWithoutJitEntry());
    uint32_t nargsAndFlags = getter->flagsAndArgCountRaw();
    callNativeGetterResult_(receiver, getter, sameRealm, nargsAndFlags);
  }

  void callScriptedSetter(ObjOperandId receiver, JSFunction* setter,
                          ValOperandId rhs, bool sameRealm) {
    MOZ_ASSERT(setter->hasJitEntry());
    uint32_t nargsAndFlags = setter->flagsAndArgCountRaw();
    callScriptedSetter_(receiver, setter, rhs, sameRealm, nargsAndFlags);
    trialInliningState_ = TrialInliningState::Candidate;
  }

  void callInlinedSetter(ObjOperandId receiver, JSFunction* setter,
                         ValOperandId rhs, ICScript* icScript, bool sameRealm) {
    MOZ_ASSERT(setter->hasJitEntry());
    uint32_t nargsAndFlags = setter->flagsAndArgCountRaw();
    callInlinedSetter_(receiver, setter, rhs, icScript, sameRealm,
                       nargsAndFlags);
    trialInliningState_ = TrialInliningState::Inlined;
  }

  void callNativeSetter(ObjOperandId receiver, JSFunction* setter,
                        ValOperandId rhs, bool sameRealm) {
    MOZ_ASSERT(setter->isNativeWithoutJitEntry());
    uint32_t nargsAndFlags = setter->flagsAndArgCountRaw();
    callNativeSetter_(receiver, setter, rhs, sameRealm, nargsAndFlags);
  }

#ifdef JS_PUNBOX64
  void callScriptedProxyGetResult(ValOperandId target, ObjOperandId receiver,
                                  ObjOperandId handler, ObjOperandId trapId,
                                  JSFunction* trap, HandleId property) {
    MOZ_ASSERT(trap->hasJitEntry());
    uint32_t nargsAndFlags = trap->flagsAndArgCountRaw();
    callScriptedProxyGetResult_(target, receiver, handler, trapId, property,
                                nargsAndFlags);
  }

  void callScriptedProxyGetByValueResult(
      ValOperandId target, ObjOperandId receiver, ObjOperandId handler,
      ValOperandId property, ObjOperandId trapId, JSFunction* trap) {
    MOZ_ASSERT(trap->hasJitEntry());
    uint32_t nargsAndFlags = trap->flagsAndArgCountRaw();
    callScriptedProxyGetByValueResult_(target, receiver, handler, property,
                                       trapId, nargsAndFlags);
  }
#endif

  void metaScriptedThisShape(Shape* thisShape) {
    metaScriptedThisShape_(thisShape);
  }

  void guardMultipleShapes(ObjOperandId obj, ListObject* shapes) {
    MOZ_ASSERT(shapes->length() > 0);
    guardMultipleShapes_(obj, shapes);
  }

  friend class CacheIRCloner;

  CACHE_IR_WRITER_GENERATED
};

}  // namespace jit
}  // namespace js

#endif /* jit_CacheIRWriter_h */
