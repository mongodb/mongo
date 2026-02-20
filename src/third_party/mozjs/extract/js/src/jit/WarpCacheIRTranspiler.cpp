/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/WarpCacheIRTranspiler.h"

#include "mozilla/Casting.h"
#include "mozilla/Maybe.h"

#include "jsmath.h"

#include "jit/AtomicOp.h"
#include "jit/CacheIR.h"
#include "jit/CacheIRCompiler.h"
#include "jit/CacheIROpsGenerated.h"
#include "jit/CacheIRReader.h"
#include "jit/LIR.h"
#include "jit/MIR-wasm.h"
#include "jit/MIR.h"
#include "jit/MIRGenerator.h"
#include "jit/MIRGraph.h"
#include "jit/WarpBuilder.h"
#include "jit/WarpBuilderShared.h"
#include "jit/WarpSnapshot.h"
#include "js/ScalarType.h"  // js::Scalar::Type
#include "vm/BytecodeLocation.h"
#include "vm/TypeofEqOperand.h"  // TypeofEqOperand
#include "wasm/WasmCode.h"

#include "gc/ObjectKind-inl.h"
#include "vm/NativeObject-inl.h"
#include "wasm/WasmInstance-inl.h"

using namespace js;
using namespace js::jit;

// The CacheIR transpiler generates MIR from Baseline CacheIR.
class MOZ_RAII WarpCacheIRTranspiler : public WarpBuilderShared {
  WarpBuilder* builder_;
  BytecodeLocation loc_;
  const CacheIRStubInfo* stubInfo_;
  const uint8_t* stubData_;

  // Vector mapping OperandId to corresponding MDefinition.
  using MDefinitionStackVector = Vector<MDefinition*, 8, SystemAllocPolicy>;
  MDefinitionStackVector operands_;

  CallInfo* callInfo_;

  // Array mapping call arguments to OperandId.
  using ArgumentKindArray =
      mozilla::EnumeratedArray<ArgumentKind, OperandId,
                               size_t(ArgumentKind::NumKinds)>;
  ArgumentKindArray argumentOperandIds_;

  void setArgumentId(ArgumentKind kind, OperandId id) {
    MOZ_ASSERT(kind != ArgumentKind::Callee);
    MOZ_ASSERT(!argumentOperandIds_[kind].valid());
    argumentOperandIds_[kind] = id;
  }

  void updateArgumentsFromOperands();

#ifdef DEBUG
  // Used to assert that there is only one effectful instruction
  // per stub. And that this instruction has a resume point.
  MInstruction* effectful_ = nullptr;
  bool pushedResult_ = false;
#endif

  inline void addUnchecked(MInstruction* ins) {
    current->add(ins);

    // If we have not set a more specific bailout kind, mark this instruction
    // as transpiled CacheIR. If one of these instructions bails out, we
    // expect to hit the baseline fallback stub and invalidate the Warp script
    // in tryAttach.
    if (ins->bailoutKind() == BailoutKind::Unknown) {
      ins->setBailoutKind(BailoutKind::TranspiledCacheIR);
    }
  }

  inline void add(MInstruction* ins) {
    MOZ_ASSERT(!ins->isEffectful());
    addUnchecked(ins);
  }

  inline void addEffectful(MInstruction* ins) {
    MOZ_ASSERT(ins->isEffectful());
    MOZ_ASSERT(!effectful_, "Can only have one effectful instruction");
    addUnchecked(ins);
#ifdef DEBUG
    effectful_ = ins;
#endif
  }

  // Bypasses all checks in addEffectful. Use with caution!
  inline void addEffectfulUnsafe(MInstruction* ins) {
    MOZ_ASSERT(ins->isEffectful());
    addUnchecked(ins);
  }

  [[nodiscard]] bool resumeAfterUnchecked(MInstruction* ins) {
    return WarpBuilderShared::resumeAfter(ins, loc_);
  }
  [[nodiscard]] bool resumeAfter(MInstruction* ins) {
    MOZ_ASSERT(effectful_ == ins);
    return resumeAfterUnchecked(ins);
  }

  // CacheIR instructions writing to the IC's result register (the *Result
  // instructions) must call this to push the result onto the virtual stack.
  void pushResult(MDefinition* result) {
    MOZ_ASSERT(!pushedResult_, "Can't have more than one result");
    current->push(result);
#ifdef DEBUG
    pushedResult_ = true;
#endif
  }

  MDefinition* getOperand(OperandId id) const { return operands_[id.id()]; }

  void setOperand(OperandId id, MDefinition* def) { operands_[id.id()] = def; }

  [[nodiscard]] bool defineOperand(OperandId id, MDefinition* def) {
    MOZ_ASSERT(id.id() == operands_.length());
    return operands_.append(def);
  }

  uintptr_t readStubWord(uint32_t offset) {
    return stubInfo_->getStubRawWord(stubData_, offset);
  }

  Shape* shapeStubField(uint32_t offset) {
    return reinterpret_cast<Shape*>(readStubWord(offset));
  }
  GetterSetter* getterSetterStubField(uint32_t offset) {
    return reinterpret_cast<GetterSetter*>(readStubWord(offset));
  }
  const JSClass* classStubField(uint32_t offset) {
    return reinterpret_cast<const JSClass*>(readStubWord(offset));
  }
  JSString* stringStubField(uint32_t offset) {
    return reinterpret_cast<JSString*>(readStubWord(offset));
  }
  JS::Symbol* symbolStubField(uint32_t offset) {
    return reinterpret_cast<JS::Symbol*>(readStubWord(offset));
  }
  BaseScript* baseScriptStubField(uint32_t offset) {
    return reinterpret_cast<BaseScript*>(readStubWord(offset));
  }
  const JSJitInfo* jitInfoStubField(uint32_t offset) {
    return reinterpret_cast<const JSJitInfo*>(readStubWord(offset));
  }
  JSNative jsnativeStubField(uint32_t offset) {
    return reinterpret_cast<JSNative>(readStubWord(offset));
  }
  JS::ExpandoAndGeneration* expandoAndGenerationField(uint32_t offset) {
    return reinterpret_cast<JS::ExpandoAndGeneration*>(readStubWord(offset));
  }
  const wasm::FuncExport* wasmFuncExportField(uint32_t offset) {
    return reinterpret_cast<const wasm::FuncExport*>(readStubWord(offset));
  }
  NativeIteratorListHead* nativeIteratorListHeadStubField(uint32_t offset) {
    return reinterpret_cast<NativeIteratorListHead*>(readStubWord(offset));
  }
  size_t* fuseStubField(uint32_t offset) {
    return reinterpret_cast<size_t*>(readStubWord(offset));
  }
  gc::Heap allocSiteInitialHeapField(uint32_t offset) {
    uintptr_t word = readStubWord(offset);
    MOZ_ASSERT(word == uintptr_t(gc::Heap::Default) ||
               word == uintptr_t(gc::Heap::Tenured));
    return gc::Heap(word);
  }
  const void* rawPointerField(uint32_t offset) {
    return reinterpret_cast<const void*>(readStubWord(offset));
  }
  jsid idStubField(uint32_t offset) {
    return jsid::fromRawBits(readStubWord(offset));
  }
  int32_t int32StubField(uint32_t offset) {
    return static_cast<int32_t>(readStubWord(offset));
  }
  uint32_t uint32StubField(uint32_t offset) {
    return static_cast<uint32_t>(readStubWord(offset));
  }
  uint64_t uint64StubField(uint32_t offset) {
    return static_cast<uint64_t>(stubInfo_->getStubRawInt64(stubData_, offset));
  }
  Value valueStubField(uint32_t offset) {
    uint64_t raw =
        static_cast<uint64_t>(stubInfo_->getStubRawInt64(stubData_, offset));
    Value val = Value::fromRawBits(raw);
    MOZ_ASSERT_IF(val.isGCThing(), val.toGCThing()->isTenured());
    return val;
  }
  double doubleStubField(uint32_t offset) {
    uint64_t raw =
        static_cast<uint64_t>(stubInfo_->getStubRawInt64(stubData_, offset));
    return mozilla::BitwiseCast<double>(raw);
  }

  // This must only be called when the caller knows the object is tenured and
  // not a nursery index.
  JSObject* tenuredObjectStubField(uint32_t offset) {
    WarpObjectField field = WarpObjectField::fromData(readStubWord(offset));
    return field.toObject();
  }

  // Returns either MConstant or MNurseryIndex. See WarpObjectField.
  MInstruction* objectStubField(uint32_t offset);

  const JSClass* classForGuardClassKind(GuardClassKind kind);

  [[nodiscard]] bool emitGuardTo(ValOperandId inputId, MIRType type);

  [[nodiscard]] bool emitToString(OperandId inputId, StringOperandId resultId);

  template <typename T>
  [[nodiscard]] bool emitDoubleBinaryArithResult(NumberOperandId lhsId,
                                                 NumberOperandId rhsId);

  template <typename T>
  [[nodiscard]] bool emitInt32BinaryArithResult(Int32OperandId lhsId,
                                                Int32OperandId rhsId);

  template <typename T>
  [[nodiscard]] bool emitBigIntBinaryArithResult(BigIntOperandId lhsId,
                                                 BigIntOperandId rhsId);

  template <typename T>
  [[nodiscard]] bool emitBigIntBinaryArithEffectfulResult(
      BigIntOperandId lhsId, BigIntOperandId rhsId);

  template <typename T>
  [[nodiscard]] bool emitBigIntUnaryArithResult(BigIntOperandId inputId);

  template <typename T>
  [[nodiscard]] bool emitBigIntPtrBinaryArith(IntPtrOperandId lhsId,
                                              IntPtrOperandId rhsId,
                                              IntPtrOperandId resultId);

  [[nodiscard]] bool emitCompareResult(JSOp op, OperandId lhsId,
                                       OperandId rhsId,
                                       MCompare::CompareType compareType);

  [[nodiscard]] bool emitTruthyResult(OperandId inputId);

  [[nodiscard]] bool emitNewIteratorResult(MNewIterator::Type type,
                                           uint32_t templateObjectOffset);

  MInstruction* addBoundsCheck(MDefinition* index, MDefinition* length);

  [[nodiscard]] MInstruction* convertToBoolean(MDefinition* input);

  bool emitAddAndStoreSlotShared(MAddAndStoreSlot::Kind kind,
                                 ObjOperandId objId, uint32_t offsetOffset,
                                 ValOperandId rhsId, uint32_t newShapeOffset);

  MInstruction* emitTypedArrayLength(ArrayBufferViewKind viewKind,
                                     MDefinition* obj);

  MInstruction* emitDataViewLength(ArrayBufferViewKind viewKind,
                                   MDefinition* obj);

  void addDataViewData(ArrayBufferViewKind viewKind, MDefinition* obj,
                       Scalar::Type type, MDefinition** offset,
                       MInstruction** elements);

  [[nodiscard]] bool emitAtomicsBinaryOp(
      ObjOperandId objId, IntPtrOperandId indexId, uint32_t valueId,
      Scalar::Type elementType, bool forEffect, ArrayBufferViewKind viewKind,
      AtomicOp op);

  [[nodiscard]] bool emitLoadArgumentSlot(ValOperandId resultId,
                                          uint32_t slotIndex);

  // Calls are either Native (native function without a JitEntry),
  // a DOM Native (native function with a JitInfo OpType::Method),
  // or Scripted (scripted function or native function with a JitEntry).
  enum class CallKind { Native, DOM, Scripted };

  [[nodiscard]] bool updateCallInfo(MDefinition* callee, CallFlags flags);

  [[nodiscard]] bool emitCallFunction(
      ObjOperandId calleeId, Int32OperandId argcId,
      mozilla::Maybe<ObjOperandId> thisObjId, CallFlags flags, CallKind kind,
      mozilla::Maybe<uint32_t> siteOffset = mozilla::Nothing());
  [[nodiscard]] bool emitFunApplyArgsObj(WrappedFunction* wrappedTarget,
                                         CallFlags flags);

  MDefinition* convertWasmArg(MDefinition* arg, wasm::ValType::Kind kind);

  WrappedFunction* maybeWrappedFunction(MDefinition* callee, CallKind kind,
                                        uint16_t nargs, FunctionFlags flags);
  WrappedFunction* maybeCallTarget(MDefinition* callee, CallKind kind);

  bool maybeCreateThis(MDefinition* callee, CallFlags flags, CallKind kind);

  [[nodiscard]] bool emitCallGetterResult(CallKind kind,
                                          ValOperandId receiverId,
                                          uint32_t getterOffset, bool sameRealm,
                                          uint32_t nargsAndFlagsOffset);
  [[nodiscard]] bool emitCallSetter(CallKind kind, ObjOperandId receiverId,
                                    uint32_t setterOffset, ValOperandId rhsId,
                                    bool sameRealm,
                                    uint32_t nargsAndFlagsOffset);

#ifndef JS_CODEGEN_X86
  [[nodiscard]] bool emitCallScriptedProxyGetShared(
      MDefinition* target, MDefinition* receiver, MDefinition* handler,
      MDefinition* id, MDefinition* trapDef, WrappedFunction* trap);
#endif

  CACHE_IR_TRANSPILER_GENERATED

 public:
  WarpCacheIRTranspiler(WarpBuilder* builder, BytecodeLocation loc,
                        CallInfo* callInfo, const WarpCacheIR* cacheIRSnapshot)
      : WarpBuilderShared(builder->snapshot(), builder->mirGen(),
                          builder->currentBlock()),
        builder_(builder),
        loc_(loc),
        stubInfo_(cacheIRSnapshot->stubInfo()),
        stubData_(cacheIRSnapshot->stubData()),
        callInfo_(callInfo) {}

  [[nodiscard]] bool transpile(std::initializer_list<MDefinition*> inputs);
};

bool WarpCacheIRTranspiler::transpile(
    std::initializer_list<MDefinition*> inputs) {
  if (!operands_.append(inputs.begin(), inputs.end())) {
    return false;
  }

  CacheIRReader reader(stubInfo_);
  do {
    CacheOp op = reader.readOp();
    switch (op) {
#define DEFINE_OP(op, ...)   \
  case CacheOp::op:          \
    if (!emit##op(reader)) { \
      return false;          \
    }                        \
    break;
      CACHE_IR_TRANSPILER_OPS(DEFINE_OP)
#undef DEFINE_OP

      default:
        fprintf(stderr, "Unsupported op: %s\n", CacheIROpNames[size_t(op)]);
        MOZ_CRASH("Unsupported op");
    }
  } while (reader.more());

  // Effectful instructions should have a resume point. We allow a limited
  // number of exceptions:
  // - MIonToWasmCall: Resumes after MInt64ToBigInt
  // - MLoadUnboxedScalar: Resumes after MInt64ToBigInt
  // - MAtomicTypedArrayElementBinop: Resumes after MInt64ToBigInt
  // - MAtomicExchangeTypedArrayElement: Resumes after MInt64ToBigInt
  // - MCompareExchangeTypedArrayElement: Resumes after MInt64ToBigInt
  // - MResizableTypedArrayLength: Resumes after MPostIntPtrConversion
  // - MResizableDataViewByteLength: Resumes after MPostIntPtrConversion
  // - MGrowableSharedArrayBufferByteLength: Resumes after MPostIntPtrConversion
  MOZ_ASSERT_IF(effectful_,
                effectful_->resumePoint() || effectful_->isIonToWasmCall() ||
                    effectful_->isLoadUnboxedScalar() ||
                    effectful_->isAtomicTypedArrayElementBinop() ||
                    effectful_->isAtomicExchangeTypedArrayElement() ||
                    effectful_->isCompareExchangeTypedArrayElement() ||
                    effectful_->isResizableTypedArrayLength() ||
                    effectful_->isResizableDataViewByteLength() ||
                    effectful_->isGrowableSharedArrayBufferByteLength());
  return true;
}

MInstruction* WarpCacheIRTranspiler::objectStubField(uint32_t offset) {
  WarpObjectField field = WarpObjectField::fromData(readStubWord(offset));

  if (field.isNurseryIndex()) {
    auto* ins = MNurseryObject::New(alloc(), field.toNurseryIndex());
    add(ins);
    return ins;
  }

  auto* ins = MConstant::NewObject(alloc(), field.toObject());
  add(ins);
  return ins;
}

bool WarpCacheIRTranspiler::emitGuardClass(ObjOperandId objId,
                                           GuardClassKind kind) {
  MDefinition* def = getOperand(objId);

  MInstruction* ins;
  if (kind == GuardClassKind::JSFunction) {
    ins = MGuardToFunction::New(alloc(), def);
  } else {
    const JSClass* classp = classForGuardClassKind(kind);
    ins = MGuardToClass::New(alloc(), def, classp);
  }

  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardEitherClass(ObjOperandId objId,
                                                 GuardClassKind kind1,
                                                 GuardClassKind kind2) {
  MDefinition* def = getOperand(objId);

  // We don't yet need this case, so it's unsupported for now.
  MOZ_ASSERT(kind1 != GuardClassKind::JSFunction &&
             kind2 != GuardClassKind::JSFunction);

  const JSClass* classp1 = classForGuardClassKind(kind1);
  const JSClass* classp2 = classForGuardClassKind(kind2);
  auto* ins = MGuardToEitherClass::New(alloc(), def, classp1, classp2);

  add(ins);

  setOperand(objId, ins);
  return true;
}

const JSClass* WarpCacheIRTranspiler::classForGuardClassKind(
    GuardClassKind kind) {
  switch (kind) {
    case GuardClassKind::Array:
    case GuardClassKind::PlainObject:
    case GuardClassKind::FixedLengthArrayBuffer:
    case GuardClassKind::ResizableArrayBuffer:
    case GuardClassKind::FixedLengthSharedArrayBuffer:
    case GuardClassKind::GrowableSharedArrayBuffer:
    case GuardClassKind::FixedLengthDataView:
    case GuardClassKind::ResizableDataView:
    case GuardClassKind::MappedArguments:
    case GuardClassKind::UnmappedArguments:
    case GuardClassKind::Set:
    case GuardClassKind::Map:
    case GuardClassKind::BoundFunction:
    case GuardClassKind::Date:
      return ClassFor(kind);
    case GuardClassKind::WindowProxy:
      return mirGen().runtime->maybeWindowProxyClass();
    case GuardClassKind::JSFunction:
      break;
  }
  MOZ_CRASH("unexpected kind");
}

bool WarpCacheIRTranspiler::emitGuardAnyClass(ObjOperandId objId,
                                              uint32_t claspOffset) {
  MDefinition* def = getOperand(objId);
  const JSClass* classp = classStubField(claspOffset);

  auto* ins = MGuardToClass::New(alloc(), def, classp);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardShape(ObjOperandId objId,
                                           uint32_t shapeOffset) {
  MDefinition* def = getOperand(objId);

  // No read barrier is required because snapshot data is not weak and is traced
  // as part of IonCompileTask.
  Shape* shape = shapeStubField(shapeOffset);

  auto* ins = MGuardShape::New(alloc(), def, shape);
  add(ins);

  setOperand(objId, ins);
  return true;
}

template <auto FuseMember, CompilationDependency::Type DepType>
struct RealmFuseDependency final : public CompilationDependency {
  RealmFuseDependency() : CompilationDependency(DepType) {}

  virtual bool registerDependency(JSContext* cx, HandleScript script) override {
    MOZ_ASSERT(checkDependency(cx));

    return (cx->realm()->realmFuses.*FuseMember).addFuseDependency(cx, script);
  }

  virtual UniquePtr<CompilationDependency> clone() const override {
    return MakeUnique<RealmFuseDependency<FuseMember, DepType>>();
  }

  virtual bool checkDependency(JSContext* cx) override {
    return (cx->realm()->realmFuses.*FuseMember).intact();
  }

  virtual bool operator==(const CompilationDependency& dep) const override {
    return dep.type == type;
  }
};

bool WarpCacheIRTranspiler::emitGuardFuse(RealmFuses::FuseIndex fuseIndex) {
  // Register a compilation dependency (for invalidating realm fuses) or add a
  // fuse guard (for other fuses).
  switch (fuseIndex) {
    case RealmFuses::FuseIndex::OptimizeGetIteratorFuse: {
      using Dependency =
          RealmFuseDependency<&RealmFuses::optimizeGetIteratorFuse,
                              CompilationDependency::Type::GetIterator>;
      return mirGen().tracker.addDependency(Dependency());
    }
    case RealmFuses::FuseIndex::OptimizeArraySpeciesFuse: {
      using Dependency =
          RealmFuseDependency<&RealmFuses::optimizeArraySpeciesFuse,
                              CompilationDependency::Type::ArraySpecies>;
      return mirGen().tracker.addDependency(Dependency());
    }
    case RealmFuses::FuseIndex::OptimizeRegExpPrototypeFuse: {
      using Dependency =
          RealmFuseDependency<&RealmFuses::optimizeRegExpPrototypeFuse,
                              CompilationDependency::Type::RegExpPrototype>;
      return mirGen().tracker.addDependency(Dependency());
    }
    case RealmFuses::FuseIndex::OptimizeStringPrototypeSymbolsFuse: {
      using Dependency = RealmFuseDependency<
          &RealmFuses::optimizeStringPrototypeSymbolsFuse,
          CompilationDependency::Type::StringPrototypeSymbols>;
      return mirGen().tracker.addDependency(Dependency());
    }
    default:
      MOZ_ASSERT(!RealmFuses::isInvalidatingFuse(fuseIndex));
      auto* ins = MGuardFuse::New(alloc(), fuseIndex);
      add(ins);
      return true;
  }
}

bool WarpCacheIRTranspiler::emitGuardMultipleShapes(ObjOperandId objId,
                                                    uint32_t shapesOffset) {
  MDefinition* def = getOperand(objId);
  MInstruction* shapeList = objectStubField(shapesOffset);

  auto* ins = MGuardMultipleShapes::New(alloc(), def, shapeList);
  if (builder_->info().inlineScriptTree()->hasSharedICScript()) {
    ins->setBailoutKind(BailoutKind::MonomorphicInlinedStubFolding);
  }
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardNullProto(ObjOperandId objId) {
  MDefinition* def = getOperand(objId);

  auto* ins = MGuardNullProto::New(alloc(), def);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardIsNativeObject(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MGuardIsNativeObject::New(alloc(), obj);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardIsProxy(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MGuardIsProxy::New(alloc(), obj);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardIsNotProxy(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MGuardIsNotProxy::New(alloc(), obj);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardIsNotDOMProxy(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MGuardIsNotDOMProxy::New(alloc(), obj);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardHasGetterSetter(
    ObjOperandId objId, uint32_t idOffset, uint32_t getterSetterOffset) {
  MDefinition* obj = getOperand(objId);
  jsid id = idStubField(idOffset);
  GetterSetter* gs = getterSetterStubField(getterSetterOffset);

  auto* ins = MGuardHasGetterSetter::New(alloc(), obj, id, gs);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitProxyGetResult(ObjOperandId objId,
                                               uint32_t idOffset) {
  MDefinition* obj = getOperand(objId);
  jsid id = idStubField(idOffset);

  auto* ins = MProxyGet::New(alloc(), obj, id);
  addEffectful(ins);

  pushResult(ins);
  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitProxyGetByValueResult(ObjOperandId objId,
                                                      ValOperandId idId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* id = getOperand(idId);

  auto* ins = MProxyGetByValue::New(alloc(), obj, id);
  addEffectful(ins);

  pushResult(ins);
  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitProxyHasPropResult(ObjOperandId objId,
                                                   ValOperandId idId,
                                                   bool hasOwn) {
  MDefinition* obj = getOperand(objId);
  MDefinition* id = getOperand(idId);

  auto* ins = MProxyHasProp::New(alloc(), obj, id, hasOwn);
  addEffectful(ins);

  pushResult(ins);
  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitProxySet(ObjOperandId objId, uint32_t idOffset,
                                         ValOperandId rhsId, bool strict) {
  MDefinition* obj = getOperand(objId);
  jsid id = idStubField(idOffset);
  MDefinition* rhs = getOperand(rhsId);

  auto* ins = MProxySet::New(alloc(), obj, rhs, id, strict);
  addEffectful(ins);

  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitProxySetByValue(ObjOperandId objId,
                                                ValOperandId idId,
                                                ValOperandId rhsId,
                                                bool strict) {
  MDefinition* obj = getOperand(objId);
  MDefinition* id = getOperand(idId);
  MDefinition* rhs = getOperand(rhsId);

  auto* ins = MProxySetByValue::New(alloc(), obj, id, rhs, strict);
  addEffectful(ins);

  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitCallSetArrayLength(ObjOperandId objId,
                                                   bool strict,
                                                   ValOperandId rhsId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* rhs = getOperand(rhsId);

  auto* ins = MCallSetArrayLength::New(alloc(), obj, rhs, strict);
  addEffectful(ins);

  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitCallDOMGetterResult(ObjOperandId objId,
                                                    uint32_t jitInfoOffset) {
  MDefinition* obj = getOperand(objId);
  const JSJitInfo* jitInfo = jitInfoStubField(jitInfoOffset);

  MInstruction* ins;
  if (jitInfo->isAlwaysInSlot) {
    ins = MGetDOMMember::New(alloc(), jitInfo, obj, nullptr, nullptr);
  } else {
    // TODO(post-Warp): realms, guard operands (movable?).
    ins = MGetDOMProperty::New(alloc(), jitInfo, DOMObjectKind::Native,
                               (JS::Realm*)mirGen().realm->realmPtr(), obj,
                               nullptr, nullptr);
  }

  if (!ins) {
    return false;
  }

  if (ins->isEffectful()) {
    addEffectful(ins);
    pushResult(ins);
    return resumeAfter(ins);
  }

  add(ins);
  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitCallDOMSetter(ObjOperandId objId,
                                              uint32_t jitInfoOffset,
                                              ValOperandId rhsId) {
  MDefinition* obj = getOperand(objId);
  const JSJitInfo* jitInfo = jitInfoStubField(jitInfoOffset);
  MDefinition* value = getOperand(rhsId);

  MOZ_ASSERT(jitInfo->type() == JSJitInfo::Setter);
  auto* set =
      MSetDOMProperty::New(alloc(), jitInfo->setter, DOMObjectKind::Native,
                           (JS::Realm*)mirGen().realm->realmPtr(), obj, value);
  addEffectful(set);
  return resumeAfter(set);
}

bool WarpCacheIRTranspiler::emitLoadDOMExpandoValue(ObjOperandId objId,
                                                    ValOperandId resultId) {
  MDefinition* proxy = getOperand(objId);

  auto* ins = MLoadDOMExpandoValue::New(alloc(), proxy);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitLoadDOMExpandoValueGuardGeneration(
    ObjOperandId objId, uint32_t expandoAndGenerationOffset,
    uint32_t generationOffset, ValOperandId resultId) {
  MDefinition* proxy = getOperand(objId);
  JS::ExpandoAndGeneration* expandoAndGeneration =
      expandoAndGenerationField(expandoAndGenerationOffset);
  uint64_t generation = uint64StubField(generationOffset);

  auto* ins = MLoadDOMExpandoValueGuardGeneration::New(
      alloc(), proxy, expandoAndGeneration, generation);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitLoadDOMExpandoValueIgnoreGeneration(
    ObjOperandId objId, ValOperandId resultId) {
  MDefinition* proxy = getOperand(objId);

  auto* ins = MLoadDOMExpandoValueIgnoreGeneration::New(alloc(), proxy);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitGuardDOMExpandoMissingOrGuardShape(
    ValOperandId expandoId, uint32_t shapeOffset) {
  MDefinition* expando = getOperand(expandoId);
  Shape* shape = shapeStubField(shapeOffset);

  auto* ins = MGuardDOMExpandoMissingOrGuardShape::New(alloc(), expando, shape);
  add(ins);

  setOperand(expandoId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMegamorphicLoadSlotResult(ObjOperandId objId,
                                                          uint32_t nameOffset) {
  MDefinition* obj = getOperand(objId);
  PropertyName* name = stringStubField(nameOffset)->asAtom().asPropertyName();

  auto* ins = MMegamorphicLoadSlot::New(alloc(), obj, NameToId(name));

  add(ins);
  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMegamorphicLoadSlotPermissiveResult(
    ObjOperandId objId, uint32_t nameOffset) {
  MDefinition* obj = getOperand(objId);
  PropertyName* name = stringStubField(nameOffset)->asAtom().asPropertyName();

  auto* ins = MMegamorphicLoadSlotPermissive::New(alloc(), obj, NameToId(name));

  addEffectful(ins);
  pushResult(ins);
  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitMegamorphicLoadSlotByValueResult(
    ObjOperandId objId, ValOperandId idId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* id = getOperand(idId);

  auto* ins = MMegamorphicLoadSlotByValue::New(alloc(), obj, id);

  add(ins);
  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMegamorphicLoadSlotByValuePermissiveResult(
    ObjOperandId objId, ValOperandId idId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* id = getOperand(idId);

  auto* ins = MMegamorphicLoadSlotByValuePermissive::New(alloc(), obj, id);

  addEffectful(ins);
  pushResult(ins);
  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitMegamorphicStoreSlot(ObjOperandId objId,
                                                     uint32_t idOffset,
                                                     ValOperandId rhsId,
                                                     bool strict) {
  MDefinition* obj = getOperand(objId);
  jsid id = idStubField(idOffset);
  MDefinition* rhs = getOperand(rhsId);

  auto* ins = MMegamorphicStoreSlot::New(alloc(), obj, rhs, id, strict);
  addEffectful(ins);

  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitMegamorphicHasPropResult(ObjOperandId objId,
                                                         ValOperandId idId,
                                                         bool hasOwn) {
  MDefinition* obj = getOperand(objId);
  MDefinition* id = getOperand(idId);

  auto* ins = MMegamorphicHasProp::New(alloc(), obj, id, hasOwn);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitSmallObjectVariableKeyHasOwnResult(
    StringOperandId idId, uint32_t propNamesOffset, uint32_t shapeOffset) {
  MDefinition* id = getOperand(idId);
  SharedShape* shape = &shapeStubField(shapeOffset)->asShared();

  auto* ins = MSmallObjectVariableKeyHasProp::New(alloc(), id, shape);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMegamorphicSetElement(ObjOperandId objId,
                                                      ValOperandId idId,
                                                      ValOperandId rhsId,
                                                      bool strict) {
  MDefinition* obj = getOperand(objId);
  MDefinition* id = getOperand(idId);
  MDefinition* rhs = getOperand(rhsId);

  auto* ins = MMegamorphicSetElement::New(alloc(), obj, id, rhs, strict);
  addEffectful(ins);

  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitObjectToIteratorResult(
    ObjOperandId objId, uint32_t enumeratorsAddrOffset) {
  MDefinition* obj = getOperand(objId);
  NativeIteratorListHead* enumeratorsAddr =
      nativeIteratorListHeadStubField(enumeratorsAddrOffset);

  auto* ins = MObjectToIterator::New(alloc(), obj, enumeratorsAddr);
  addEffectful(ins);
  pushResult(ins);
  if (!resumeAfter(ins)) {
    return false;
  }

  return true;
}

bool WarpCacheIRTranspiler::emitValueToIteratorResult(ValOperandId valId) {
  MDefinition* val = getOperand(valId);

  auto* ins = MValueToIterator::New(alloc(), val);
  addEffectful(ins);

  pushResult(ins);
  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitGuardIsNotArrayBufferMaybeShared(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MGuardIsNotArrayBufferMaybeShared::New(alloc(), obj);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardIsTypedArray(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MGuardIsTypedArray::New(alloc(), obj);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardIsFixedLengthTypedArray(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MGuardIsFixedLengthTypedArray::New(alloc(), obj);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardIsResizableTypedArray(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MGuardIsResizableTypedArray::New(alloc(), obj);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardHasProxyHandler(ObjOperandId objId,
                                                     uint32_t handlerOffset) {
  MDefinition* obj = getOperand(objId);
  const void* handler = rawPointerField(handlerOffset);

  auto* ins = MGuardHasProxyHandler::New(alloc(), obj, handler);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardProto(ObjOperandId objId,
                                           uint32_t protoOffset) {
  MDefinition* def = getOperand(objId);
  MDefinition* proto = objectStubField(protoOffset);

  auto* ins = MGuardProto::New(alloc(), def, proto);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardDynamicSlotIsSpecificObject(
    ObjOperandId objId, ObjOperandId expectedId, uint32_t slotOffset) {
  size_t slotIndex = int32StubField(slotOffset);
  MDefinition* obj = getOperand(objId);
  MDefinition* expected = getOperand(expectedId);

  auto* slots = MSlots::New(alloc(), obj);
  add(slots);

  auto* load = MLoadDynamicSlot::New(alloc(), slots, slotIndex);
  add(load);

  auto* unbox = MUnbox::New(alloc(), load, MIRType::Object, MUnbox::Fallible);
  add(unbox);

  auto* guard = MGuardObjectIdentity::New(alloc(), unbox, expected,
                                          /* bailOnEquality = */ false);
  add(guard);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadDynamicSlot(ValOperandId resultId,
                                                ObjOperandId objId,
                                                uint32_t slotOffset) {
  size_t slotIndex = int32StubField(slotOffset);
  MDefinition* obj = getOperand(objId);

  auto* slots = MSlots::New(alloc(), obj);
  add(slots);

  auto* load = MLoadDynamicSlot::New(alloc(), slots, slotIndex);
  add(load);

  return defineOperand(resultId, load);
}

bool WarpCacheIRTranspiler::emitGuardDynamicSlotIsNotObject(
    ObjOperandId objId, uint32_t slotOffset) {
  size_t slotIndex = int32StubField(slotOffset);
  MDefinition* obj = getOperand(objId);

  auto* slots = MSlots::New(alloc(), obj);
  add(slots);

  auto* load = MLoadDynamicSlot::New(alloc(), slots, slotIndex);
  add(load);

  auto* guard = MGuardIsNotObject::New(alloc(), load);
  add(guard);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardFixedSlotValue(ObjOperandId objId,
                                                    uint32_t offsetOffset,
                                                    uint32_t valOffset) {
  MDefinition* obj = getOperand(objId);

  size_t offset = int32StubField(offsetOffset);
  Value val = valueStubField(valOffset);

  uint32_t slotIndex = NativeObject::getFixedSlotIndexFromOffset(offset);

  auto* load = MLoadFixedSlot::New(alloc(), obj, slotIndex);
  add(load);

  auto* guard = MGuardValue::New(alloc(), load, val);
  add(guard);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardDynamicSlotValue(ObjOperandId objId,
                                                      uint32_t offsetOffset,
                                                      uint32_t valOffset) {
  MDefinition* obj = getOperand(objId);

  size_t offset = int32StubField(offsetOffset);
  Value val = valueStubField(valOffset);

  size_t slotIndex = NativeObject::getDynamicSlotIndexFromOffset(offset);

  auto* slots = MSlots::New(alloc(), obj);
  add(slots);

  auto* load = MLoadDynamicSlot::New(alloc(), slots, slotIndex);
  add(load);

  auto* guard = MGuardValue::New(alloc(), load, val);
  add(guard);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadScriptedProxyHandler(ObjOperandId resultId,
                                                         ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* load = MLoadScriptedProxyHandler::New(alloc(), obj);
  add(load);

  return defineOperand(resultId, load);
}

bool WarpCacheIRTranspiler::emitIdToStringOrSymbol(ValOperandId resultId,
                                                   ValOperandId idId) {
  MDefinition* id = getOperand(idId);

  auto* ins = MIdToStringOrSymbol::New(alloc(), id);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitGuardSpecificAtom(StringOperandId strId,
                                                  uint32_t expectedOffset) {
  MDefinition* str = getOperand(strId);
  JSString* expected = stringStubField(expectedOffset);

  auto* ins = MGuardSpecificAtom::New(alloc(), str, &expected->asAtom());
  add(ins);

  setOperand(strId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardSpecificSymbol(SymbolOperandId symId,
                                                    uint32_t expectedOffset) {
  MDefinition* symbol = getOperand(symId);
  JS::Symbol* expected = symbolStubField(expectedOffset);

  auto* ins = MGuardSpecificSymbol::New(alloc(), symbol, expected);
  add(ins);

  setOperand(symId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardSpecificInt32(Int32OperandId numId,
                                                   int32_t expected) {
  MDefinition* num = getOperand(numId);

  auto* ins = MGuardSpecificInt32::New(alloc(), num, expected);
  add(ins);

  setOperand(numId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardSpecificValue(ValOperandId valId,
                                                   uint32_t expectedOffset) {
  MDefinition* val = getOperand(valId);
  Value expected = valueStubField(expectedOffset);

  auto* ins = MGuardValue::New(alloc(), val, expected);
  add(ins);

  setOperand(valId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardSpecificObject(ObjOperandId objId,
                                                    uint32_t expectedOffset) {
  MDefinition* obj = getOperand(objId);
  MDefinition* expected = objectStubField(expectedOffset);

  auto* ins = MGuardObjectIdentity::New(alloc(), obj, expected,
                                        /* bailOnEquality = */ false);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardSpecificFunction(
    ObjOperandId objId, uint32_t expectedOffset, uint32_t nargsAndFlagsOffset) {
  MDefinition* obj = getOperand(objId);
  MDefinition* expected = objectStubField(expectedOffset);
  uint32_t nargsAndFlags = uint32StubField(nargsAndFlagsOffset);

  uint16_t nargs = nargsAndFlags >> 16;
  FunctionFlags flags = FunctionFlags(uint16_t(nargsAndFlags));

  auto* ins = MGuardSpecificFunction::New(alloc(), obj, expected, nargs, flags);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardFunctionScript(
    ObjOperandId funId, uint32_t expectedOffset, uint32_t nargsAndFlagsOffset) {
  MDefinition* fun = getOperand(funId);
  BaseScript* expected = baseScriptStubField(expectedOffset);
  uint32_t nargsAndFlags = uint32StubField(nargsAndFlagsOffset);

  uint16_t nargs = nargsAndFlags >> 16;
  FunctionFlags flags = FunctionFlags(uint16_t(nargsAndFlags));

  auto* ins = MGuardFunctionScript::New(alloc(), fun, expected, nargs, flags);
  add(ins);

  setOperand(funId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardStringToIndex(StringOperandId strId,
                                                   Int32OperandId resultId) {
  MDefinition* str = getOperand(strId);

  auto* ins = MGuardStringToIndex::New(alloc(), str);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitGuardStringToInt32(StringOperandId strId,
                                                   Int32OperandId resultId) {
  MDefinition* str = getOperand(strId);

  auto* ins = MGuardStringToInt32::New(alloc(), str);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitGuardStringToNumber(StringOperandId strId,
                                                    NumberOperandId resultId) {
  MDefinition* str = getOperand(strId);

  auto* ins = MGuardStringToDouble::New(alloc(), str);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitGuardNoDenseElements(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MGuardNoDenseElements::New(alloc(), obj);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardFunctionHasJitEntry(ObjOperandId funId) {
  MDefinition* fun = getOperand(funId);
  uint16_t expectedFlags = FunctionFlags::HasJitEntryFlags();
  uint16_t unexpectedFlags = 0;

  auto* ins =
      MGuardFunctionFlags::New(alloc(), fun, expectedFlags, unexpectedFlags);
  add(ins);

  setOperand(funId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardFunctionHasNoJitEntry(ObjOperandId funId) {
  MDefinition* fun = getOperand(funId);
  uint16_t expectedFlags = 0;
  uint16_t unexpectedFlags = FunctionFlags::HasJitEntryFlags();

  auto* ins =
      MGuardFunctionFlags::New(alloc(), fun, expectedFlags, unexpectedFlags);
  add(ins);

  setOperand(funId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardFunctionIsNonBuiltinCtor(
    ObjOperandId funId) {
  MDefinition* fun = getOperand(funId);

  auto* ins = MGuardFunctionIsNonBuiltinCtor::New(alloc(), fun);
  add(ins);

  setOperand(funId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardFunctionIsConstructor(ObjOperandId funId) {
  MDefinition* fun = getOperand(funId);
  uint16_t expectedFlags = FunctionFlags::CONSTRUCTOR;
  uint16_t unexpectedFlags = 0;

  auto* ins =
      MGuardFunctionFlags::New(alloc(), fun, expectedFlags, unexpectedFlags);
  add(ins);

  setOperand(funId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardNotClassConstructor(ObjOperandId funId) {
  MDefinition* fun = getOperand(funId);

  auto* ins =
      MGuardFunctionKind::New(alloc(), fun, FunctionFlags::ClassConstructor,
                              /*bailOnEquality=*/true);
  add(ins);

  setOperand(funId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardArrayIsPacked(ObjOperandId arrayId) {
  MDefinition* array = getOperand(arrayId);

  auto* ins = MGuardArrayIsPacked::New(alloc(), array);
  add(ins);

  setOperand(arrayId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardArgumentsObjectFlags(ObjOperandId objId,
                                                          uint8_t flags) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MGuardArgumentsObjectFlags::New(alloc(), obj, flags);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardObjectHasSameRealm(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MGuardObjectHasSameRealm::New(alloc(), obj);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardNonDoubleType(ValOperandId inputId,
                                                   ValueType type) {
  switch (type) {
    case ValueType::String:
    case ValueType::Symbol:
    case ValueType::BigInt:
    case ValueType::Int32:
    case ValueType::Boolean:
      return emitGuardTo(inputId, MIRTypeFromValueType(JSValueType(type)));
    case ValueType::Undefined:
      return emitGuardIsUndefined(inputId);
    case ValueType::Null:
      return emitGuardIsNull(inputId);
    case ValueType::Double:
    case ValueType::Magic:
    case ValueType::PrivateGCThing:
    case ValueType::Object:
      break;
  }

  MOZ_CRASH("unexpected type");
}

bool WarpCacheIRTranspiler::emitGuardTo(ValOperandId inputId, MIRType type) {
  MDefinition* def = getOperand(inputId);
  if (def->type() == type) {
    return true;
  }

  auto* ins = MUnbox::New(alloc(), def, type, MUnbox::Fallible);
  add(ins);

  setOperand(inputId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardToObject(ValOperandId inputId) {
  return emitGuardTo(inputId, MIRType::Object);
}

bool WarpCacheIRTranspiler::emitGuardToString(ValOperandId inputId) {
  return emitGuardTo(inputId, MIRType::String);
}

bool WarpCacheIRTranspiler::emitGuardToSymbol(ValOperandId inputId) {
  return emitGuardTo(inputId, MIRType::Symbol);
}

bool WarpCacheIRTranspiler::emitGuardToBigInt(ValOperandId inputId) {
  return emitGuardTo(inputId, MIRType::BigInt);
}

bool WarpCacheIRTranspiler::emitGuardToBoolean(ValOperandId inputId) {
  return emitGuardTo(inputId, MIRType::Boolean);
}

bool WarpCacheIRTranspiler::emitGuardToInt32(ValOperandId inputId) {
  return emitGuardTo(inputId, MIRType::Int32);
}

bool WarpCacheIRTranspiler::emitGuardBooleanToInt32(ValOperandId inputId,
                                                    Int32OperandId resultId) {
  if (!emitGuardTo(inputId, MIRType::Boolean)) {
    return false;
  }

  MDefinition* input = getOperand(inputId);
  MOZ_ASSERT(input->type() == MIRType::Boolean);

  auto* ins = MBooleanToInt32::New(alloc(), input);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitGuardIsNumber(ValOperandId inputId) {
  MDefinition* def = getOperand(inputId);

  // No guard needed when the input is already a number type.
  if (IsNumberType(def->type())) {
    return true;
  }

  // MIRType::Double also implies int32 in Ion.
  return emitGuardTo(inputId, MIRType::Double);
}

bool WarpCacheIRTranspiler::emitGuardIsNullOrUndefined(ValOperandId inputId) {
  MDefinition* input = getOperand(inputId);
  if (input->type() == MIRType::Null || input->type() == MIRType::Undefined) {
    return true;
  }

  auto* ins = MGuardNullOrUndefined::New(alloc(), input);
  add(ins);

  setOperand(inputId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardIsNull(ValOperandId inputId) {
  MDefinition* input = getOperand(inputId);
  if (input->type() == MIRType::Null) {
    return true;
  }

  auto* ins = MGuardValue::New(alloc(), input, NullValue());
  add(ins);
  setOperand(inputId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardIsUndefined(ValOperandId inputId) {
  MDefinition* input = getOperand(inputId);
  if (input->type() == MIRType::Undefined) {
    return true;
  }

  auto* ins = MGuardValue::New(alloc(), input, UndefinedValue());
  add(ins);
  setOperand(inputId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardIsExtensible(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MGuardIsExtensible::New(alloc(), obj);
  add(ins);
  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardInt32IsNonNegative(
    Int32OperandId indexId) {
  MDefinition* index = getOperand(indexId);

  auto* ins = MGuardInt32IsNonNegative::New(alloc(), index);
  add(ins);
  setOperand(indexId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardIndexIsNotDenseElement(
    ObjOperandId objId, Int32OperandId indexId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);

  auto* ins = MGuardIndexIsNotDenseElement::New(alloc(), obj, index);
  add(ins);
  setOperand(indexId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardIndexIsValidUpdateOrAdd(
    ObjOperandId objId, Int32OperandId indexId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);

  auto* ins = MGuardIndexIsValidUpdateOrAdd::New(alloc(), obj, index);
  add(ins);
  setOperand(indexId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitCallAddOrUpdateSparseElementHelper(
    ObjOperandId objId, Int32OperandId idId, ValOperandId rhsId, bool strict) {
  MDefinition* obj = getOperand(objId);
  MDefinition* id = getOperand(idId);
  MDefinition* rhs = getOperand(rhsId);

  auto* ins = MCallAddOrUpdateSparseElement::New(alloc(), obj, id, rhs, strict);
  addEffectful(ins);

  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitGuardTagNotEqual(ValueTagOperandId lhsId,
                                                 ValueTagOperandId rhsId) {
  MDefinition* lhs = getOperand(lhsId);
  MDefinition* rhs = getOperand(rhsId);

  auto* ins = MGuardTagNotEqual::New(alloc(), lhs, rhs);
  add(ins);

  return true;
}

bool WarpCacheIRTranspiler::emitGuardToInt32Index(ValOperandId inputId,
                                                  Int32OperandId resultId) {
  MDefinition* input = getOperand(inputId);
  auto* ins =
      MToNumberInt32::New(alloc(), input, IntConversionInputKind::NumbersOnly);

  // ToPropertyKey(-0) is "0", so we can silently convert -0 to 0 here.
  ins->setNeedsNegativeZeroCheck(false);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitTruncateDoubleToUInt32(
    NumberOperandId inputId, Int32OperandId resultId) {
  MDefinition* input = getOperand(inputId);
  auto* ins = MTruncateToInt32::New(alloc(), input);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitDoubleToUint8Clamped(NumberOperandId inputId,
                                                     Int32OperandId resultId) {
  MDefinition* input = getOperand(inputId);
  auto* ins = MClampToUint8::New(alloc(), input);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitGuardToInt32ModUint32(ValOperandId valId,
                                                      Int32OperandId resultId) {
  MDefinition* input = getOperand(valId);
  auto* ins = MTruncateToInt32::New(alloc(), input);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitGuardToUint8Clamped(ValOperandId valId,
                                                    Int32OperandId resultId) {
  MDefinition* input = getOperand(valId);
  auto* ins = MClampToUint8::New(alloc(), input);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitToString(OperandId inputId,
                                         StringOperandId resultId) {
  MDefinition* input = getOperand(inputId);
  auto* ins =
      MToString::New(alloc(), input, MToString::SideEffectHandling::Bailout);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitInt32ToIntPtr(Int32OperandId inputId,
                                              IntPtrOperandId resultId) {
  MDefinition* input = getOperand(inputId);
  auto* ins = MInt32ToIntPtr::New(alloc(), input);
  add(ins);
  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitGuardNumberToIntPtrIndex(
    NumberOperandId inputId, bool supportOOB, IntPtrOperandId resultId) {
  MDefinition* input = getOperand(inputId);
  auto* ins = MGuardNumberToIntPtrIndex::New(alloc(), input, supportOOB);
  add(ins);
  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitCallInt32ToString(Int32OperandId inputId,
                                                  StringOperandId resultId) {
  return emitToString(inputId, resultId);
}

bool WarpCacheIRTranspiler::emitCallNumberToString(NumberOperandId inputId,
                                                   StringOperandId resultId) {
  return emitToString(inputId, resultId);
}

bool WarpCacheIRTranspiler::emitInt32ToStringWithBaseResult(
    Int32OperandId inputId, Int32OperandId baseId) {
  MDefinition* input = getOperand(inputId);
  MDefinition* base = getOperand(baseId);

  auto* guardedBase = MGuardInt32Range::New(alloc(), base, 2, 36);
  add(guardedBase);

  // Use lower-case characters by default.
  constexpr bool lower = true;

  auto* ins = MInt32ToStringWithBase::New(alloc(), input, guardedBase, lower);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitBooleanToString(BooleanOperandId inputId,
                                                StringOperandId resultId) {
  return emitToString(inputId, resultId);
}

bool WarpCacheIRTranspiler::emitBooleanToNumber(BooleanOperandId inputId,
                                                NumberOperandId resultId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MToDouble::New(alloc(), input);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitStringToAtom(StringOperandId strId) {
  MDefinition* str = getOperand(strId);

  auto* ins = MToHashableString::New(alloc(), str);
  add(ins);

  setOperand(strId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadInt32Result(Int32OperandId valId) {
  MDefinition* val = getOperand(valId);
  MOZ_ASSERT(val->type() == MIRType::Int32);
  pushResult(val);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadDoubleResult(NumberOperandId valId) {
  MDefinition* val = getOperand(valId);
  MOZ_ASSERT(IsNumberType(val->type()));

  if (val->type() != MIRType::Double) {
    auto* ins = MToDouble::New(alloc(), val);
    add(ins);

    val = ins;
  }

  pushResult(val);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadBigIntResult(BigIntOperandId valId) {
  MDefinition* val = getOperand(valId);
  MOZ_ASSERT(val->type() == MIRType::BigInt);
  pushResult(val);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadObjectResult(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);
  MOZ_ASSERT(obj->type() == MIRType::Object);
  pushResult(obj);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadStringResult(StringOperandId strId) {
  MDefinition* str = getOperand(strId);
  MOZ_ASSERT(str->type() == MIRType::String);
  pushResult(str);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadSymbolResult(SymbolOperandId symId) {
  MDefinition* sym = getOperand(symId);
  MOZ_ASSERT(sym->type() == MIRType::Symbol);
  pushResult(sym);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadUndefinedResult() {
  pushResult(constant(UndefinedValue()));
  return true;
}

bool WarpCacheIRTranspiler::emitLoadBooleanResult(bool val) {
  pushResult(constant(BooleanValue(val)));
  return true;
}

bool WarpCacheIRTranspiler::emitLoadInt32Constant(uint32_t valOffset,
                                                  Int32OperandId resultId) {
  int32_t val = int32StubField(valOffset);
  auto* valConst = constant(Int32Value(val));
  return defineOperand(resultId, valConst);
}

bool WarpCacheIRTranspiler::emitLoadDoubleConstant(uint32_t valOffset,
                                                   NumberOperandId resultId) {
  double val = doubleStubField(valOffset);
  auto* valConst = constant(DoubleValue(val));
  return defineOperand(resultId, valConst);
}

bool WarpCacheIRTranspiler::emitLoadBooleanConstant(bool val,
                                                    BooleanOperandId resultId) {
  auto* valConst = constant(BooleanValue(val));
  return defineOperand(resultId, valConst);
}

bool WarpCacheIRTranspiler::emitLoadUndefined(ValOperandId resultId) {
  auto* valConst = constant(UndefinedValue());
  return defineOperand(resultId, valConst);
}

bool WarpCacheIRTranspiler::emitLoadConstantString(uint32_t strOffset,
                                                   StringOperandId resultId) {
  JSString* val = stringStubField(strOffset);
  auto* valConst = constant(StringValue(val));
  return defineOperand(resultId, valConst);
}

bool WarpCacheIRTranspiler::emitLoadConstantStringResult(uint32_t strOffset) {
  JSString* val = stringStubField(strOffset);
  auto* valConst = constant(StringValue(val));
  pushResult(valConst);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadTypeOfObjectResult(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);
  auto* typeOf = MTypeOf::New(alloc(), obj);
  add(typeOf);

  auto* ins = MTypeOfName::New(alloc(), typeOf);
  add(ins);
  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadTypeOfEqObjectResult(
    ObjOperandId objId, TypeofEqOperand operand) {
  MDefinition* obj = getOperand(objId);
  auto* typeOf = MTypeOf::New(alloc(), obj);
  add(typeOf);

  auto* typeInt = MConstant::New(alloc(), Int32Value(operand.type()));
  add(typeInt);

  auto* ins = MCompare::New(alloc(), typeOf, typeInt, operand.compareOp(),
                            MCompare::Compare_Int32);
  add(ins);
  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadEnclosingEnvironment(
    ObjOperandId objId, ObjOperandId resultId) {
  MDefinition* env = getOperand(objId);
  auto* ins = MEnclosingEnvironment::New(alloc(), env);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitLoadObject(ObjOperandId resultId,
                                           uint32_t objOffset) {
  MInstruction* ins = objectStubField(objOffset);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitLoadProtoObject(ObjOperandId resultId,
                                                uint32_t objOffset,
                                                ObjOperandId receiverObjId) {
  MInstruction* ins = objectStubField(objOffset);
  if (ins->isConstant()) {
    MDefinition* receiverObj = getOperand(receiverObjId);

    ins = MConstantProto::New(alloc(), ins, receiverObj->skipObjectGuards());
    add(ins);
  }
  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitLoadProto(ObjOperandId objId,
                                          ObjOperandId resultId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MObjectStaticProto::New(alloc(), obj);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitLoadInstanceOfObjectResult(
    ValOperandId lhsId, ObjOperandId protoId) {
  MDefinition* lhs = getOperand(lhsId);
  MDefinition* proto = getOperand(protoId);

  auto* instanceOf = MInstanceOf::New(alloc(), lhs, proto);
  addEffectful(instanceOf);

  pushResult(instanceOf);
  return resumeAfter(instanceOf);
}

bool WarpCacheIRTranspiler::emitLoadValueTag(ValOperandId valId,
                                             ValueTagOperandId resultId) {
  MDefinition* val = getOperand(valId);

  auto* ins = MLoadValueTag::New(alloc(), val);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitLoadDynamicSlotResult(ObjOperandId objId,
                                                      uint32_t offsetOffset) {
  int32_t offset = int32StubField(offsetOffset);

  MDefinition* obj = getOperand(objId);
  size_t slotIndex = NativeObject::getDynamicSlotIndexFromOffset(offset);

  auto* slots = MSlots::New(alloc(), obj);
  add(slots);

  auto* load = MLoadDynamicSlot::New(alloc(), slots, slotIndex);
  add(load);

  pushResult(load);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadFixedSlot(ValOperandId resultId,
                                              ObjOperandId objId,
                                              uint32_t offsetOffset) {
  MDefinition* obj = getOperand(objId);

  size_t offset = int32StubField(offsetOffset);
  uint32_t slotIndex = NativeObject::getFixedSlotIndexFromOffset(offset);

  auto* load = MLoadFixedSlot::New(alloc(), obj, slotIndex);
  add(load);

  return defineOperand(resultId, load);
}

bool WarpCacheIRTranspiler::emitLoadFixedSlotResult(ObjOperandId objId,
                                                    uint32_t offsetOffset) {
  int32_t offset = int32StubField(offsetOffset);

  MDefinition* obj = getOperand(objId);
  uint32_t slotIndex = NativeObject::getFixedSlotIndexFromOffset(offset);

  auto* load = MLoadFixedSlot::New(alloc(), obj, slotIndex);
  add(load);

  pushResult(load);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadFixedSlotTypedResult(ObjOperandId objId,
                                                         uint32_t offsetOffset,
                                                         ValueType type) {
  int32_t offset = int32StubField(offsetOffset);

  MDefinition* obj = getOperand(objId);
  uint32_t slotIndex = NativeObject::getFixedSlotIndexFromOffset(offset);

  auto* load = MLoadFixedSlot::New(alloc(), obj, slotIndex);
  load->setResultType(MIRTypeFromValueType(JSValueType(type)));
  add(load);

  pushResult(load);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardIsNotUninitializedLexical(
    ValOperandId valId) {
  MDefinition* val = getOperand(valId);

  auto* lexicalCheck = MLexicalCheck::New(alloc(), val);
  add(lexicalCheck);

  if (snapshot().bailoutInfo().failedLexicalCheck()) {
    lexicalCheck->setNotMovable();
  }

  setOperand(valId, lexicalCheck);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadInt32ArrayLengthResult(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* elements = MElements::New(alloc(), obj);
  add(elements);

  auto* length = MArrayLength::New(alloc(), elements);
  add(length);

  pushResult(length);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadInt32ArrayLength(ObjOperandId objId,
                                                     Int32OperandId resultId) {
  MDefinition* obj = getOperand(objId);

  auto* elements = MElements::New(alloc(), obj);
  add(elements);

  auto* length = MArrayLength::New(alloc(), elements);
  add(length);

  return defineOperand(resultId, length);
}

bool WarpCacheIRTranspiler::emitLoadArgumentsObjectArgResult(
    ObjOperandId objId, Int32OperandId indexId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);

  auto* load = MLoadArgumentsObjectArg::New(alloc(), obj, index);
  add(load);

  pushResult(load);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadArgumentsObjectArgHoleResult(
    ObjOperandId objId, Int32OperandId indexId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);

  auto* load = MLoadArgumentsObjectArgHole::New(alloc(), obj, index);
  add(load);

  pushResult(load);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadArgumentsObjectArgExistsResult(
    ObjOperandId objId, Int32OperandId indexId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);

  auto* ins = MInArgumentsObjectArg::New(alloc(), obj, index);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadArgumentsObjectLengthResult(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* length = MArgumentsObjectLength::New(alloc(), obj);
  add(length);

  pushResult(length);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadArgumentsObjectLength(
    ObjOperandId objId, Int32OperandId resultId) {
  MDefinition* obj = getOperand(objId);

  auto* length = MArgumentsObjectLength::New(alloc(), obj);
  add(length);

  return defineOperand(resultId, length);
}

bool WarpCacheIRTranspiler::emitLoadBoundFunctionNumArgs(
    ObjOperandId objId, Int32OperandId resultId) {
  MDefinition* obj = getOperand(objId);

  auto* numArgs = MBoundFunctionNumArgs::New(alloc(), obj);
  add(numArgs);

  return defineOperand(resultId, numArgs);
}

bool WarpCacheIRTranspiler::emitLoadBoundFunctionTarget(ObjOperandId objId,
                                                        ObjOperandId resultId) {
  MDefinition* obj = getOperand(objId);

  auto* target = MLoadFixedSlotAndUnbox::New(
      alloc(), obj, BoundFunctionObject::targetSlot(), MUnbox::Mode::Infallible,
      MIRType::Object);
  add(target);

  return defineOperand(resultId, target);
}

bool WarpCacheIRTranspiler::emitLoadBoundFunctionArgument(
    ObjOperandId objId, uint32_t index, ValOperandId resultId) {
  MDefinition* obj = getOperand(objId);

  auto* boundArgs = MLoadFixedSlotAndUnbox::New(
      alloc(), obj, BoundFunctionObject::firstInlineBoundArgSlot(),
      MUnbox::Mode::Infallible, MIRType::Object);
  add(boundArgs);

  auto* elements = MElements::New(alloc(), boundArgs);
  add(elements);

  auto argIndex = constant(Int32Value(index));
  auto* load = MLoadElement::New(alloc(), elements, argIndex);
  add(load);

  return defineOperand(resultId, load);
}

bool WarpCacheIRTranspiler::emitGuardBoundFunctionIsConstructor(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* guard = MGuardBoundFunctionIsConstructor::New(alloc(), obj);
  add(guard);

  setOperand(objId, guard);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardObjectIdentity(ObjOperandId obj1Id,
                                                    ObjOperandId obj2Id) {
  MDefinition* obj1 = getOperand(obj1Id);
  MDefinition* obj2 = getOperand(obj2Id);

  auto* guard = MGuardObjectIdentity::New(alloc(), obj1, obj2,
                                          /* bailOnEquality = */ false);
  add(guard);
  return true;
}

bool WarpCacheIRTranspiler::emitArrayFromArgumentsObjectResult(
    ObjOperandId objId, uint32_t shapeOffset) {
  MDefinition* obj = getOperand(objId);
  Shape* shape = shapeStubField(shapeOffset);
  MOZ_ASSERT(shape);

  auto* array = MArrayFromArgumentsObject::New(alloc(), obj, shape);
  addEffectful(array);

  pushResult(array);
  return resumeAfter(array);
}

bool WarpCacheIRTranspiler::emitLoadFunctionLengthResult(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* length = MFunctionLength::New(alloc(), obj);
  add(length);

  pushResult(length);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadFunctionNameResult(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* name = MFunctionName::New(alloc(), obj);
  add(name);

  pushResult(name);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadArrayBufferByteLengthInt32Result(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* length = MArrayBufferByteLength::New(alloc(), obj);
  add(length);

  auto* lengthInt32 = MNonNegativeIntPtrToInt32::New(alloc(), length);
  add(lengthInt32);

  pushResult(lengthInt32);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadArrayBufferByteLengthDoubleResult(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* length = MArrayBufferByteLength::New(alloc(), obj);
  add(length);

  auto* lengthDouble = MIntPtrToDouble::New(alloc(), length);
  add(lengthDouble);

  pushResult(lengthDouble);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadArrayBufferViewLengthInt32Result(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  // Use a separate instruction for converting the length to Int32, so that we
  // can fold the MArrayBufferViewLength instruction with length instructions
  // added for bounds checks.

  auto* length = MArrayBufferViewLength::New(alloc(), obj);
  add(length);

  auto* lengthInt32 = MNonNegativeIntPtrToInt32::New(alloc(), length);
  add(lengthInt32);

  pushResult(lengthInt32);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadArrayBufferViewLengthDoubleResult(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* length = MArrayBufferViewLength::New(alloc(), obj);
  add(length);

  auto* lengthDouble = MIntPtrToDouble::New(alloc(), length);
  add(lengthDouble);

  pushResult(lengthDouble);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadStringLengthResult(StringOperandId strId) {
  MDefinition* str = getOperand(strId);

  auto* length = MStringLength::New(alloc(), str);
  add(length);

  pushResult(length);
  return true;
}

MInstruction* WarpCacheIRTranspiler::addBoundsCheck(MDefinition* index,
                                                    MDefinition* length) {
  MInstruction* check = MBoundsCheck::New(alloc(), index, length);
  add(check);

  if (snapshot().bailoutInfo().failedBoundsCheck()) {
    check->setNotMovable();
  }

  if (JitOptions.spectreIndexMasking) {
    // Use a separate MIR instruction for the index masking. Doing this as
    // part of MBoundsCheck would be unsound because bounds checks can be
    // optimized or eliminated completely. Consider this:
    //
    //   for (var i = 0; i < x; i++)
    //        res = arr[i];
    //
    // If we can prove |x < arr.length|, we are able to eliminate the bounds
    // check, but we should not get rid of the index masking because the
    // |i < x| branch could still be mispredicted.
    //
    // Using a separate instruction lets us eliminate the bounds check
    // without affecting the index masking.
    check = MSpectreMaskIndex::New(alloc(), check, length);
    add(check);
  }

  return check;
}

bool WarpCacheIRTranspiler::emitLoadDenseElementResult(ObjOperandId objId,
                                                       Int32OperandId indexId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);

  auto* elements = MElements::New(alloc(), obj);
  add(elements);

  auto* length = MInitializedLength::New(alloc(), elements);
  add(length);

  index = addBoundsCheck(index, length);

  auto* load = MLoadElement::New(alloc(), elements, index);
  add(load);

  pushResult(load);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadDenseElementHoleResult(
    ObjOperandId objId, Int32OperandId indexId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);

  auto* elements = MElements::New(alloc(), obj);
  add(elements);

  auto* length = MInitializedLength::New(alloc(), elements);
  add(length);

  auto* load = MLoadElementHole::New(alloc(), elements, index, length);
  add(load);

  pushResult(load);
  return true;
}

bool WarpCacheIRTranspiler::emitCallGetSparseElementResult(
    ObjOperandId objId, Int32OperandId indexId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);

  auto* call = MCallGetSparseElement::New(alloc(), obj, index);
  addEffectful(call);

  pushResult(call);
  return resumeAfter(call);
}

bool WarpCacheIRTranspiler::emitCallNativeGetElementResult(
    ObjOperandId objId, Int32OperandId indexId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);

  auto* call = MCallNativeGetElement::New(alloc(), obj, index);
  addEffectful(call);

  pushResult(call);
  return resumeAfter(call);
}

bool WarpCacheIRTranspiler::emitCallNativeGetElementSuperResult(
    ObjOperandId objId, Int32OperandId indexId, ValOperandId receiverId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);
  MDefinition* receiver = getOperand(receiverId);

  auto* call = MCallNativeGetElementSuper::New(alloc(), obj, index, receiver);
  addEffectful(call);

  pushResult(call);
  return resumeAfter(call);
}

bool WarpCacheIRTranspiler::emitLoadDenseElementExistsResult(
    ObjOperandId objId, Int32OperandId indexId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);

  // Get the elements vector.
  auto* elements = MElements::New(alloc(), obj);
  add(elements);

  auto* length = MInitializedLength::New(alloc(), elements);
  add(length);

  // Check if id < initLength.
  index = addBoundsCheck(index, length);

  // And check elem[id] is not a hole.
  auto* guard = MGuardElementNotHole::New(alloc(), elements, index);
  add(guard);

  pushResult(constant(BooleanValue(true)));
  return true;
}

bool WarpCacheIRTranspiler::emitLoadDenseElementHoleExistsResult(
    ObjOperandId objId, Int32OperandId indexId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);

  // Get the elements vector.
  auto* elements = MElements::New(alloc(), obj);
  add(elements);

  auto* length = MInitializedLength::New(alloc(), elements);
  add(length);

  // Check if id < initLength and elem[id] not a hole.
  auto* ins = MInArray::New(alloc(), elements, index, length);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitCallObjectHasSparseElementResult(
    ObjOperandId objId, Int32OperandId indexId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);

  auto* ins = MCallObjectHasSparseElement::New(alloc(), obj, index);
  add(ins);

  pushResult(ins);
  return true;
}

MInstruction* WarpCacheIRTranspiler::emitTypedArrayLength(
    ArrayBufferViewKind viewKind, MDefinition* obj) {
  if (viewKind == ArrayBufferViewKind::FixedLength) {
    auto* length = MArrayBufferViewLength::New(alloc(), obj);
    add(length);

    return length;
  }

  // Bounds check doesn't require a memory barrier. See IsValidIntegerIndex
  // abstract operation which reads the underlying buffer byte length using
  // "unordered" memory order.
  auto barrier = MemoryBarrierRequirement::NotRequired;

  // Movable and removable because no memory barrier is needed.
  auto* length = MResizableTypedArrayLength::New(alloc(), obj, barrier);
  length->setMovable();
  length->setNotGuard();
  add(length);

  return length;
}

bool WarpCacheIRTranspiler::emitLoadTypedArrayElementExistsResult(
    ObjOperandId objId, IntPtrOperandId indexId, ArrayBufferViewKind viewKind) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);

  auto* length = emitTypedArrayLength(viewKind, obj);

  // Unsigned comparison to catch negative indices.
  auto* ins = MCompare::New(alloc(), index, length, JSOp::Lt,
                            MCompare::Compare_UIntPtr);
  add(ins);

  pushResult(ins);
  return true;
}

static MIRType MIRTypeForArrayBufferViewRead(Scalar::Type arrayType,
                                             bool forceDoubleForUint32) {
  switch (arrayType) {
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Uint8Clamped:
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Int32:
      return MIRType::Int32;
    case Scalar::Uint32:
      return forceDoubleForUint32 ? MIRType::Double : MIRType::Int32;
    case Scalar::Float16:
    case Scalar::Float32:
      return MIRType::Float32;
    case Scalar::Float64:
      return MIRType::Double;
    case Scalar::BigInt64:
    case Scalar::BigUint64:
      return MIRType::Int64;
    default:
      break;
  }
  MOZ_CRASH("Unknown typed array type");
}

bool WarpCacheIRTranspiler::emitLoadTypedArrayElementResult(
    ObjOperandId objId, IntPtrOperandId indexId, Scalar::Type elementType,
    bool handleOOB, bool forceDoubleForUint32, ArrayBufferViewKind viewKind) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);

  auto* length = emitTypedArrayLength(viewKind, obj);

  if (!handleOOB) {
    // MLoadTypedArrayElementHole does the bounds checking.
    index = addBoundsCheck(index, length);
  }

  auto* elements = MArrayBufferViewElements::New(alloc(), obj);
  add(elements);

  if (handleOOB) {
    auto* load = MLoadTypedArrayElementHole::New(
        alloc(), elements, index, length, elementType, forceDoubleForUint32);
    add(load);

    pushResult(load);
    return true;
  }

  auto* load = MLoadUnboxedScalar::New(alloc(), elements, index, elementType);
  load->setResultType(
      MIRTypeForArrayBufferViewRead(elementType, forceDoubleForUint32));
  add(load);

  MInstruction* result = load;
  if (Scalar::isBigIntType(elementType)) {
    result = MInt64ToBigInt::New(alloc(), load,
                                 Scalar::isSignedIntType(elementType));
    add(result);
  }

  pushResult(result);
  return true;
}

bool WarpCacheIRTranspiler::emitLinearizeForCharAccess(
    StringOperandId strId, Int32OperandId indexId, StringOperandId resultId) {
  MDefinition* str = getOperand(strId);
  MDefinition* index = getOperand(indexId);

  auto* ins = MLinearizeForCharAccess::New(alloc(), str, index);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitLinearizeForCodePointAccess(
    StringOperandId strId, Int32OperandId indexId, StringOperandId resultId) {
  MDefinition* str = getOperand(strId);
  MDefinition* index = getOperand(indexId);

  auto* ins = MLinearizeForCodePointAccess::New(alloc(), str, index);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitToRelativeStringIndex(Int32OperandId indexId,
                                                      StringOperandId strId,
                                                      Int32OperandId resultId) {
  MDefinition* str = getOperand(strId);
  MDefinition* index = getOperand(indexId);

  auto* length = MStringLength::New(alloc(), str);
  add(length);

  auto* ins = MToRelativeStringIndex::New(alloc(), index, length);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitLoadStringCharResult(StringOperandId strId,
                                                     Int32OperandId indexId,
                                                     bool handleOOB) {
  MDefinition* str = getOperand(strId);
  MDefinition* index = getOperand(indexId);

  if (handleOOB) {
    auto* charCode = MCharCodeAtOrNegative::New(alloc(), str, index);
    add(charCode);

    auto* fromCharCode = MFromCharCodeEmptyIfNegative::New(alloc(), charCode);
    add(fromCharCode);

    pushResult(fromCharCode);
    return true;
  }

  auto* length = MStringLength::New(alloc(), str);
  add(length);

  index = addBoundsCheck(index, length);

  auto* charCode = MCharCodeAt::New(alloc(), str, index);
  add(charCode);

  auto* fromCharCode = MFromCharCode::New(alloc(), charCode);
  add(fromCharCode);

  pushResult(fromCharCode);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadStringAtResult(StringOperandId strId,
                                                   Int32OperandId indexId,
                                                   bool handleOOB) {
  MDefinition* str = getOperand(strId);
  MDefinition* index = getOperand(indexId);

  if (handleOOB) {
    auto* charCode = MCharCodeAtOrNegative::New(alloc(), str, index);
    add(charCode);

    auto* fromCharCode =
        MFromCharCodeUndefinedIfNegative::New(alloc(), charCode);
    add(fromCharCode);

    pushResult(fromCharCode);
    return true;
  }

  auto* length = MStringLength::New(alloc(), str);
  add(length);

  index = addBoundsCheck(index, length);

  auto* charCode = MCharCodeAt::New(alloc(), str, index);
  add(charCode);

  auto* fromCharCode = MFromCharCode::New(alloc(), charCode);
  add(fromCharCode);

  pushResult(fromCharCode);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadStringCharCodeResult(StringOperandId strId,
                                                         Int32OperandId indexId,
                                                         bool handleOOB) {
  MDefinition* str = getOperand(strId);
  MDefinition* index = getOperand(indexId);

  if (handleOOB) {
    auto* charCode = MCharCodeAtOrNegative::New(alloc(), str, index);
    add(charCode);

    auto* ins = MNegativeToNaN::New(alloc(), charCode);
    add(ins);

    pushResult(ins);
    return true;
  }

  auto* length = MStringLength::New(alloc(), str);
  add(length);

  index = addBoundsCheck(index, length);

  auto* charCode = MCharCodeAt::New(alloc(), str, index);
  add(charCode);

  pushResult(charCode);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadStringCodePointResult(
    StringOperandId strId, Int32OperandId indexId, bool handleOOB) {
  MDefinition* str = getOperand(strId);
  MDefinition* index = getOperand(indexId);

  if (handleOOB) {
    auto* codePoint = MCodePointAtOrNegative::New(alloc(), str, index);
    add(codePoint);

    auto* ins = MNegativeToUndefined::New(alloc(), codePoint);
    add(ins);

    pushResult(ins);
    return true;
  }

  auto* length = MStringLength::New(alloc(), str);
  add(length);

  index = addBoundsCheck(index, length);

  auto* codePoint = MCodePointAt::New(alloc(), str, index);
  add(codePoint);

  pushResult(codePoint);
  return true;
}

bool WarpCacheIRTranspiler::emitNewMapObjectResult(
    uint32_t templateObjectOffset) {
  JSObject* templateObj = tenuredObjectStubField(templateObjectOffset);

  auto* obj = MNewMapObject::New(alloc(), templateObj);
  addEffectful(obj);

  pushResult(obj);
  return resumeAfter(obj);
}

bool WarpCacheIRTranspiler::emitNewSetObjectResult(
    uint32_t templateObjectOffset) {
  JSObject* templateObj = tenuredObjectStubField(templateObjectOffset);

  auto* obj = MNewSetObject::New(alloc(), templateObj);
  addEffectful(obj);

  pushResult(obj);
  return resumeAfter(obj);
}

bool WarpCacheIRTranspiler::emitNewMapObjectFromIterableResult(
    uint32_t templateObjectOffset, ValOperandId iterableId) {
  JSObject* templateObj = tenuredObjectStubField(templateObjectOffset);
  MDefinition* iterable = getOperand(iterableId);

  auto* obj = MNewMapObjectFromIterable::New(alloc(), iterable, templateObj);
  addEffectful(obj);

  pushResult(obj);
  return resumeAfter(obj);
}

bool WarpCacheIRTranspiler::emitNewSetObjectFromIterableResult(
    uint32_t templateObjectOffset, ValOperandId iterableId) {
  JSObject* templateObj = tenuredObjectStubField(templateObjectOffset);
  MDefinition* iterable = getOperand(iterableId);

  auto* obj = MNewSetObjectFromIterable::New(alloc(), iterable, templateObj);
  addEffectful(obj);

  pushResult(obj);
  return resumeAfter(obj);
}

bool WarpCacheIRTranspiler::emitNewStringObjectResult(
    uint32_t templateObjectOffset, StringOperandId strId) {
  JSObject* templateObj = tenuredObjectStubField(templateObjectOffset);
  MDefinition* string = getOperand(strId);

  auto* obj = MNewStringObject::New(alloc(), string, templateObj);
  addEffectful(obj);

  pushResult(obj);
  return resumeAfter(obj);
}

bool WarpCacheIRTranspiler::emitStringFromCharCodeResult(
    Int32OperandId codeId) {
  MDefinition* code = getOperand(codeId);

  auto* fromCharCode = MFromCharCode::New(alloc(), code);
  add(fromCharCode);

  pushResult(fromCharCode);
  return true;
}

bool WarpCacheIRTranspiler::emitStringFromCodePointResult(
    Int32OperandId codeId) {
  MDefinition* code = getOperand(codeId);

  auto* fromCodePoint = MFromCodePoint::New(alloc(), code);
  add(fromCodePoint);

  pushResult(fromCodePoint);
  return true;
}

bool WarpCacheIRTranspiler::emitStringIncludesResult(
    StringOperandId strId, StringOperandId searchStrId) {
  MDefinition* str = getOperand(strId);
  MDefinition* searchStr = getOperand(searchStrId);

  auto* includes = MStringIncludes::New(alloc(), str, searchStr);
  add(includes);

  pushResult(includes);
  return true;
}

bool WarpCacheIRTranspiler::emitStringIndexOfResult(
    StringOperandId strId, StringOperandId searchStrId) {
  MDefinition* str = getOperand(strId);
  MDefinition* searchStr = getOperand(searchStrId);

  auto* indexOf = MStringIndexOf::New(alloc(), str, searchStr);
  add(indexOf);

  pushResult(indexOf);
  return true;
}

bool WarpCacheIRTranspiler::emitStringLastIndexOfResult(
    StringOperandId strId, StringOperandId searchStrId) {
  MDefinition* str = getOperand(strId);
  MDefinition* searchStr = getOperand(searchStrId);

  auto* lastIndexOf = MStringLastIndexOf::New(alloc(), str, searchStr);
  add(lastIndexOf);

  pushResult(lastIndexOf);
  return true;
}

bool WarpCacheIRTranspiler::emitStringStartsWithResult(
    StringOperandId strId, StringOperandId searchStrId) {
  MDefinition* str = getOperand(strId);
  MDefinition* searchStr = getOperand(searchStrId);

  auto* startsWith = MStringStartsWith::New(alloc(), str, searchStr);
  add(startsWith);

  pushResult(startsWith);
  return true;
}

bool WarpCacheIRTranspiler::emitStringEndsWithResult(
    StringOperandId strId, StringOperandId searchStrId) {
  MDefinition* str = getOperand(strId);
  MDefinition* searchStr = getOperand(searchStrId);

  auto* endsWith = MStringEndsWith::New(alloc(), str, searchStr);
  add(endsWith);

  pushResult(endsWith);
  return true;
}

bool WarpCacheIRTranspiler::emitStringToLowerCaseResult(StringOperandId strId) {
  MDefinition* str = getOperand(strId);

  auto* convert =
      MStringConvertCase::New(alloc(), str, MStringConvertCase::LowerCase);
  add(convert);

  pushResult(convert);
  return true;
}

bool WarpCacheIRTranspiler::emitStringToUpperCaseResult(StringOperandId strId) {
  MDefinition* str = getOperand(strId);

  auto* convert =
      MStringConvertCase::New(alloc(), str, MStringConvertCase::UpperCase);
  add(convert);

  pushResult(convert);
  return true;
}

bool WarpCacheIRTranspiler::emitStringTrimResult(StringOperandId strId) {
  MDefinition* str = getOperand(strId);

  auto* linear = MLinearizeString::New(alloc(), str);
  add(linear);

  auto* start = MStringTrimStartIndex::New(alloc(), linear);
  add(start);

  auto* end = MStringTrimEndIndex::New(alloc(), linear, start);
  add(end);

  // Safe to truncate because both operands are positive and end >= start.
  auto* length = MSub::New(alloc(), end, start, MIRType::Int32);
  length->setTruncateKind(TruncateKind::Truncate);
  add(length);

  auto* substr = MSubstr::New(alloc(), linear, start, length);
  add(substr);

  pushResult(substr);
  return true;
}

bool WarpCacheIRTranspiler::emitStringTrimStartResult(StringOperandId strId) {
  MDefinition* str = getOperand(strId);

  auto* linear = MLinearizeString::New(alloc(), str);
  add(linear);

  auto* start = MStringTrimStartIndex::New(alloc(), linear);
  add(start);

  auto* end = MStringLength::New(alloc(), linear);
  add(end);

  // Safe to truncate because both operands are positive and end >= start.
  auto* length = MSub::New(alloc(), end, start, MIRType::Int32);
  length->setTruncateKind(TruncateKind::Truncate);
  add(length);

  auto* substr = MSubstr::New(alloc(), linear, start, length);
  add(substr);

  pushResult(substr);
  return true;
}

bool WarpCacheIRTranspiler::emitStringTrimEndResult(StringOperandId strId) {
  MDefinition* str = getOperand(strId);

  auto* linear = MLinearizeString::New(alloc(), str);
  add(linear);

  auto* start = constant(Int32Value(0));

  auto* length = MStringTrimEndIndex::New(alloc(), linear, start);
  add(length);

  auto* substr = MSubstr::New(alloc(), linear, start, length);
  add(substr);

  pushResult(substr);
  return true;
}

bool WarpCacheIRTranspiler::emitStoreDynamicSlot(ObjOperandId objId,
                                                 uint32_t offsetOffset,
                                                 ValOperandId rhsId) {
  int32_t offset = int32StubField(offsetOffset);

  MDefinition* obj = getOperand(objId);
  size_t slotIndex = NativeObject::getDynamicSlotIndexFromOffset(offset);
  MDefinition* rhs = getOperand(rhsId);

  auto* barrier = MPostWriteBarrier::New(alloc(), obj, rhs);
  add(barrier);

  auto* slots = MSlots::New(alloc(), obj);
  add(slots);

  auto* store = MStoreDynamicSlot::NewBarriered(alloc(), slots, slotIndex, rhs);
  addEffectful(store);
  return resumeAfter(store);
}

bool WarpCacheIRTranspiler::emitStoreFixedSlot(ObjOperandId objId,
                                               uint32_t offsetOffset,
                                               ValOperandId rhsId) {
  int32_t offset = int32StubField(offsetOffset);

  MDefinition* obj = getOperand(objId);
  size_t slotIndex = NativeObject::getFixedSlotIndexFromOffset(offset);
  MDefinition* rhs = getOperand(rhsId);

  auto* barrier = MPostWriteBarrier::New(alloc(), obj, rhs);
  add(barrier);

  auto* store = MStoreFixedSlot::NewBarriered(alloc(), obj, slotIndex, rhs);
  addEffectful(store);
  return resumeAfter(store);
}

bool WarpCacheIRTranspiler::emitStoreFixedSlotUndefinedResult(
    ObjOperandId objId, uint32_t offsetOffset, ValOperandId rhsId) {
  int32_t offset = int32StubField(offsetOffset);

  MDefinition* obj = getOperand(objId);
  size_t slotIndex = NativeObject::getFixedSlotIndexFromOffset(offset);
  MDefinition* rhs = getOperand(rhsId);

  auto* barrier = MPostWriteBarrier::New(alloc(), obj, rhs);
  add(barrier);

  auto* store = MStoreFixedSlot::NewBarriered(alloc(), obj, slotIndex, rhs);
  addEffectful(store);

  auto* undef = constant(UndefinedValue());
  pushResult(undef);

  return resumeAfter(store);
}

bool WarpCacheIRTranspiler::emitAddAndStoreSlotShared(
    MAddAndStoreSlot::Kind kind, ObjOperandId objId, uint32_t offsetOffset,
    ValOperandId rhsId, uint32_t newShapeOffset) {
  int32_t offset = int32StubField(offsetOffset);
  Shape* shape = shapeStubField(newShapeOffset);

  MDefinition* obj = getOperand(objId);
  MDefinition* rhs = getOperand(rhsId);

  auto* barrier = MPostWriteBarrier::New(alloc(), obj, rhs);
  add(barrier);

  auto* addAndStore =
      MAddAndStoreSlot::New(alloc(), obj, rhs, kind, offset, shape);
  addEffectful(addAndStore);

  return resumeAfter(addAndStore);
}

bool WarpCacheIRTranspiler::emitAddAndStoreFixedSlot(ObjOperandId objId,
                                                     uint32_t offsetOffset,
                                                     ValOperandId rhsId,
                                                     uint32_t newShapeOffset) {
  return emitAddAndStoreSlotShared(MAddAndStoreSlot::Kind::FixedSlot, objId,
                                   offsetOffset, rhsId, newShapeOffset);
}

bool WarpCacheIRTranspiler::emitAddAndStoreDynamicSlot(
    ObjOperandId objId, uint32_t offsetOffset, ValOperandId rhsId,
    uint32_t newShapeOffset) {
  return emitAddAndStoreSlotShared(MAddAndStoreSlot::Kind::DynamicSlot, objId,
                                   offsetOffset, rhsId, newShapeOffset);
}

bool WarpCacheIRTranspiler::emitAllocateAndStoreDynamicSlot(
    ObjOperandId objId, uint32_t offsetOffset, ValOperandId rhsId,
    uint32_t newShapeOffset, uint32_t numNewSlotsOffset) {
  int32_t offset = int32StubField(offsetOffset);
  Shape* shape = shapeStubField(newShapeOffset);
  uint32_t numNewSlots = uint32StubField(numNewSlotsOffset);

  MDefinition* obj = getOperand(objId);
  MDefinition* rhs = getOperand(rhsId);

  auto* barrier = MPostWriteBarrier::New(alloc(), obj, rhs);
  add(barrier);

  auto* allocateAndStore =
      MAllocateAndStoreSlot::New(alloc(), obj, rhs, offset, shape, numNewSlots);
  addEffectful(allocateAndStore);

  return resumeAfter(allocateAndStore);
}

bool WarpCacheIRTranspiler::emitAddSlotAndCallAddPropHook(
    ObjOperandId objId, ValOperandId rhsId, uint32_t newShapeOffset) {
  Shape* shape = shapeStubField(newShapeOffset);
  MDefinition* obj = getOperand(objId);
  MDefinition* rhs = getOperand(rhsId);

  auto* addProp = MAddSlotAndCallAddPropHook::New(alloc(), obj, rhs, shape);
  addEffectful(addProp);

  return resumeAfter(addProp);
}

bool WarpCacheIRTranspiler::emitStoreDenseElement(ObjOperandId objId,
                                                  Int32OperandId indexId,
                                                  ValOperandId rhsId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);
  MDefinition* rhs = getOperand(rhsId);

  auto* elements = MElements::New(alloc(), obj);
  add(elements);

  auto* length = MInitializedLength::New(alloc(), elements);
  add(length);

  index = addBoundsCheck(index, length);

  auto* barrier = MPostWriteElementBarrier::New(alloc(), obj, rhs, index);
  add(barrier);

  bool needsHoleCheck = true;
  auto* store = MStoreElement::NewBarriered(alloc(), elements, index, rhs,
                                            needsHoleCheck);
  addEffectful(store);
  return resumeAfter(store);
}

bool WarpCacheIRTranspiler::emitStoreDenseElementHole(ObjOperandId objId,
                                                      Int32OperandId indexId,
                                                      ValOperandId rhsId,
                                                      bool handleAdd) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);
  MDefinition* rhs = getOperand(rhsId);

  auto* elements = MElements::New(alloc(), obj);
  add(elements);

  MInstruction* store;
  if (handleAdd) {
    // TODO(post-Warp): Consider changing MStoreElementHole to match IC code.
    store = MStoreElementHole::New(alloc(), obj, elements, index, rhs);
  } else {
    auto* length = MInitializedLength::New(alloc(), elements);
    add(length);

    index = addBoundsCheck(index, length);

    auto* barrier = MPostWriteElementBarrier::New(alloc(), obj, rhs, index);
    add(barrier);

    bool needsHoleCheck = false;
    store = MStoreElement::NewBarriered(alloc(), elements, index, rhs,
                                        needsHoleCheck);
  }
  addEffectful(store);

  return resumeAfter(store);
}

bool WarpCacheIRTranspiler::emitStoreTypedArrayElement(
    ObjOperandId objId, Scalar::Type elementType, IntPtrOperandId indexId,
    uint32_t rhsId, bool handleOOB, ArrayBufferViewKind viewKind) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);
  MDefinition* rhs = getOperand(ValOperandId(rhsId));

  auto* length = emitTypedArrayLength(viewKind, obj);

  if (!handleOOB) {
    // MStoreTypedArrayElementHole does the bounds checking.
    index = addBoundsCheck(index, length);
  }

  auto* elements = MArrayBufferViewElements::New(alloc(), obj);
  add(elements);

  MInstruction* store;
  if (handleOOB) {
    store = MStoreTypedArrayElementHole::New(alloc(), elements, length, index,
                                             rhs, elementType);
  } else {
    store =
        MStoreUnboxedScalar::New(alloc(), elements, index, rhs, elementType);
  }
  addEffectful(store);
  return resumeAfter(store);
}

MInstruction* WarpCacheIRTranspiler::emitDataViewLength(
    ArrayBufferViewKind viewKind, MDefinition* obj) {
  if (viewKind == ArrayBufferViewKind::FixedLength) {
    auto* length = MArrayBufferViewLength::New(alloc(), obj);
    add(length);

    return length;
  }

  // Bounds check doesn't require a memory barrier. See GetViewValue and
  // SetViewValue abstract operations which read the underlying buffer byte
  // length using "unordered" memory order.
  auto barrier = MemoryBarrierRequirement::NotRequired;

  // Movable and removable because no memory barrier is needed.
  auto* length = MResizableDataViewByteLength::New(alloc(), obj, barrier);
  length->setMovable();
  length->setNotGuard();
  add(length);

  return length;
}

void WarpCacheIRTranspiler::addDataViewData(ArrayBufferViewKind viewKind,
                                            MDefinition* obj, Scalar::Type type,
                                            MDefinition** offset,
                                            MInstruction** elements) {
  auto* length = emitDataViewLength(viewKind, obj);

  // Adjust the length to account for accesses near the end of the dataview.
  if (size_t byteSize = Scalar::byteSize(type); byteSize > 1) {
    // To ensure |0 <= offset && offset + byteSize <= length|, first adjust the
    // length by subtracting |byteSize - 1| (bailing out if that becomes
    // negative).
    length = MAdjustDataViewLength::New(alloc(), length, byteSize);
    add(length);
  }

  *offset = addBoundsCheck(*offset, length);

  *elements = MArrayBufferViewElements::New(alloc(), obj);
  add(*elements);
}

bool WarpCacheIRTranspiler::emitLoadDataViewValueResult(
    ObjOperandId objId, IntPtrOperandId offsetId,
    BooleanOperandId littleEndianId, Scalar::Type elementType,
    bool forceDoubleForUint32, ArrayBufferViewKind viewKind) {
  MDefinition* obj = getOperand(objId);
  MDefinition* offset = getOperand(offsetId);
  MDefinition* littleEndian = getOperand(littleEndianId);

  // Add bounds check and get the DataViewObject's elements.
  MInstruction* elements;
  addDataViewData(viewKind, obj, elementType, &offset, &elements);

  // Load the element.
  MInstruction* load;
  if (Scalar::byteSize(elementType) == 1) {
    load = MLoadUnboxedScalar::New(alloc(), elements, offset, elementType);
  } else {
    load = MLoadDataViewElement::New(alloc(), elements, offset, littleEndian,
                                     elementType);
  }
  add(load);

  MIRType knownType =
      MIRTypeForArrayBufferViewRead(elementType, forceDoubleForUint32);
  load->setResultType(knownType);

  MInstruction* result = load;
  if (Scalar::isBigIntType(elementType)) {
    result = MInt64ToBigInt::New(alloc(), load,
                                 Scalar::isSignedIntType(elementType));
    add(result);
  }

  pushResult(result);
  return true;
}

bool WarpCacheIRTranspiler::emitStoreDataViewValueResult(
    ObjOperandId objId, IntPtrOperandId offsetId, uint32_t valueId,
    BooleanOperandId littleEndianId, Scalar::Type elementType,
    ArrayBufferViewKind viewKind) {
  MDefinition* obj = getOperand(objId);
  MDefinition* offset = getOperand(offsetId);
  MDefinition* value = getOperand(ValOperandId(valueId));
  MDefinition* littleEndian = getOperand(littleEndianId);

  // Add bounds check and get the DataViewObject's elements.
  MInstruction* elements;
  addDataViewData(viewKind, obj, elementType, &offset, &elements);

  // Store the element.
  MInstruction* store;
  if (Scalar::byteSize(elementType) == 1) {
    store =
        MStoreUnboxedScalar::New(alloc(), elements, offset, value, elementType);
  } else {
    store = MStoreDataViewElement::New(alloc(), elements, offset, value,
                                       littleEndian, elementType);
  }
  addEffectful(store);

  pushResult(constant(UndefinedValue()));

  return resumeAfter(store);
}

bool WarpCacheIRTranspiler::emitInt32IncResult(Int32OperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* constOne = MConstant::New(alloc(), Int32Value(1));
  add(constOne);

  auto* ins = MAdd::New(alloc(), input, constOne, MIRType::Int32);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitDoubleIncResult(NumberOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* constOne = MConstant::New(alloc(), DoubleValue(1.0));
  add(constOne);

  auto* ins = MAdd::New(alloc(), input, constOne, MIRType::Double);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitInt32DecResult(Int32OperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* constOne = MConstant::New(alloc(), Int32Value(1));
  add(constOne);

  auto* ins = MSub::New(alloc(), input, constOne, MIRType::Int32);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitDoubleDecResult(NumberOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* constOne = MConstant::New(alloc(), DoubleValue(1.0));
  add(constOne);

  auto* ins = MSub::New(alloc(), input, constOne, MIRType::Double);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitInt32NegationResult(Int32OperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* constNegOne = MConstant::New(alloc(), Int32Value(-1));
  add(constNegOne);

  auto* ins = MMul::New(alloc(), input, constNegOne, MIRType::Int32);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitDoubleNegationResult(NumberOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* constNegOne = MConstant::New(alloc(), DoubleValue(-1.0));
  add(constNegOne);

  auto* ins = MMul::New(alloc(), input, constNegOne, MIRType::Double);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitInt32NotResult(Int32OperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MBitNot::New(alloc(), input, MIRType::Int32);
  add(ins);

  pushResult(ins);
  return true;
}

template <typename T>
bool WarpCacheIRTranspiler::emitDoubleBinaryArithResult(NumberOperandId lhsId,
                                                        NumberOperandId rhsId) {
  MDefinition* lhs = getOperand(lhsId);
  MDefinition* rhs = getOperand(rhsId);

  auto* ins = T::New(alloc(), lhs, rhs, MIRType::Double);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitDoubleAddResult(NumberOperandId lhsId,
                                                NumberOperandId rhsId) {
  return emitDoubleBinaryArithResult<MAdd>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitDoubleSubResult(NumberOperandId lhsId,
                                                NumberOperandId rhsId) {
  return emitDoubleBinaryArithResult<MSub>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitDoubleMulResult(NumberOperandId lhsId,
                                                NumberOperandId rhsId) {
  return emitDoubleBinaryArithResult<MMul>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitDoubleDivResult(NumberOperandId lhsId,
                                                NumberOperandId rhsId) {
  return emitDoubleBinaryArithResult<MDiv>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitDoubleModResult(NumberOperandId lhsId,
                                                NumberOperandId rhsId) {
  return emitDoubleBinaryArithResult<MMod>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitDoublePowResult(NumberOperandId lhsId,
                                                NumberOperandId rhsId) {
  return emitDoubleBinaryArithResult<MPow>(lhsId, rhsId);
}

template <typename T>
bool WarpCacheIRTranspiler::emitInt32BinaryArithResult(Int32OperandId lhsId,
                                                       Int32OperandId rhsId) {
  MDefinition* lhs = getOperand(lhsId);
  MDefinition* rhs = getOperand(rhsId);

  auto* ins = T::New(alloc(), lhs, rhs, MIRType::Int32);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitInt32AddResult(Int32OperandId lhsId,
                                               Int32OperandId rhsId) {
  return emitInt32BinaryArithResult<MAdd>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitInt32SubResult(Int32OperandId lhsId,
                                               Int32OperandId rhsId) {
  return emitInt32BinaryArithResult<MSub>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitInt32MulResult(Int32OperandId lhsId,
                                               Int32OperandId rhsId) {
  return emitInt32BinaryArithResult<MMul>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitInt32DivResult(Int32OperandId lhsId,
                                               Int32OperandId rhsId) {
  return emitInt32BinaryArithResult<MDiv>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitInt32ModResult(Int32OperandId lhsId,
                                               Int32OperandId rhsId) {
  return emitInt32BinaryArithResult<MMod>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitInt32PowResult(Int32OperandId lhsId,
                                               Int32OperandId rhsId) {
  return emitInt32BinaryArithResult<MPow>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitInt32BitOrResult(Int32OperandId lhsId,
                                                 Int32OperandId rhsId) {
  return emitInt32BinaryArithResult<MBitOr>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitInt32BitXorResult(Int32OperandId lhsId,
                                                  Int32OperandId rhsId) {
  return emitInt32BinaryArithResult<MBitXor>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitInt32BitAndResult(Int32OperandId lhsId,
                                                  Int32OperandId rhsId) {
  return emitInt32BinaryArithResult<MBitAnd>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitInt32LeftShiftResult(Int32OperandId lhsId,
                                                     Int32OperandId rhsId) {
  return emitInt32BinaryArithResult<MLsh>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitInt32RightShiftResult(Int32OperandId lhsId,
                                                      Int32OperandId rhsId) {
  return emitInt32BinaryArithResult<MRsh>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitInt32URightShiftResult(Int32OperandId lhsId,
                                                       Int32OperandId rhsId,
                                                       bool forceDouble) {
  MDefinition* lhs = getOperand(lhsId);
  MDefinition* rhs = getOperand(rhsId);

  MIRType specialization = forceDouble ? MIRType::Double : MIRType::Int32;
  auto* ins = MUrsh::New(alloc(), lhs, rhs, specialization);
  add(ins);

  pushResult(ins);
  return true;
}

template <typename T>
bool WarpCacheIRTranspiler::emitBigIntBinaryArithResult(BigIntOperandId lhsId,
                                                        BigIntOperandId rhsId) {
  MDefinition* lhs = getOperand(lhsId);
  MDefinition* rhs = getOperand(rhsId);

  auto* ins = T::New(alloc(), lhs, rhs);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitBigIntAddResult(BigIntOperandId lhsId,
                                                BigIntOperandId rhsId) {
  return emitBigIntBinaryArithResult<MBigIntAdd>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitBigIntSubResult(BigIntOperandId lhsId,
                                                BigIntOperandId rhsId) {
  return emitBigIntBinaryArithResult<MBigIntSub>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitBigIntMulResult(BigIntOperandId lhsId,
                                                BigIntOperandId rhsId) {
  return emitBigIntBinaryArithResult<MBigIntMul>(lhsId, rhsId);
}

template <typename T>
bool WarpCacheIRTranspiler::emitBigIntBinaryArithEffectfulResult(
    BigIntOperandId lhsId, BigIntOperandId rhsId) {
  MDefinition* lhs = getOperand(lhsId);
  MDefinition* rhs = getOperand(rhsId);

  auto* ins = T::New(alloc(), lhs, rhs);

  if (ins->isEffectful()) {
    addEffectful(ins);

    pushResult(ins);
    return resumeAfter(ins);
  }

  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitBigIntDivResult(BigIntOperandId lhsId,
                                                BigIntOperandId rhsId) {
  return emitBigIntBinaryArithEffectfulResult<MBigIntDiv>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitBigIntModResult(BigIntOperandId lhsId,
                                                BigIntOperandId rhsId) {
  return emitBigIntBinaryArithEffectfulResult<MBigIntMod>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitBigIntPowResult(BigIntOperandId lhsId,
                                                BigIntOperandId rhsId) {
  return emitBigIntBinaryArithEffectfulResult<MBigIntPow>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitBigIntBitAndResult(BigIntOperandId lhsId,
                                                   BigIntOperandId rhsId) {
  return emitBigIntBinaryArithResult<MBigIntBitAnd>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitBigIntBitOrResult(BigIntOperandId lhsId,
                                                  BigIntOperandId rhsId) {
  return emitBigIntBinaryArithResult<MBigIntBitOr>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitBigIntBitXorResult(BigIntOperandId lhsId,
                                                   BigIntOperandId rhsId) {
  return emitBigIntBinaryArithResult<MBigIntBitXor>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitBigIntLeftShiftResult(BigIntOperandId lhsId,
                                                      BigIntOperandId rhsId) {
  return emitBigIntBinaryArithResult<MBigIntLsh>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitBigIntRightShiftResult(BigIntOperandId lhsId,
                                                       BigIntOperandId rhsId) {
  return emitBigIntBinaryArithResult<MBigIntRsh>(lhsId, rhsId);
}

template <typename T>
bool WarpCacheIRTranspiler::emitBigIntUnaryArithResult(
    BigIntOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = T::New(alloc(), input);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitBigIntIncResult(BigIntOperandId inputId) {
  return emitBigIntUnaryArithResult<MBigIntIncrement>(inputId);
}

bool WarpCacheIRTranspiler::emitBigIntDecResult(BigIntOperandId inputId) {
  return emitBigIntUnaryArithResult<MBigIntDecrement>(inputId);
}

bool WarpCacheIRTranspiler::emitBigIntNegationResult(BigIntOperandId inputId) {
  return emitBigIntUnaryArithResult<MBigIntNegate>(inputId);
}

bool WarpCacheIRTranspiler::emitBigIntNotResult(BigIntOperandId inputId) {
  return emitBigIntUnaryArithResult<MBigIntBitNot>(inputId);
}

bool WarpCacheIRTranspiler::emitBigIntToIntPtr(BigIntOperandId inputId,
                                               IntPtrOperandId resultId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MBigIntToIntPtr::New(alloc(), input);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitIntPtrToBigIntResult(IntPtrOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MIntPtrToBigInt::New(alloc(), input);
  add(ins);

  pushResult(ins);
  return true;
}

template <typename T>
bool WarpCacheIRTranspiler::emitBigIntPtrBinaryArith(IntPtrOperandId lhsId,
                                                     IntPtrOperandId rhsId,
                                                     IntPtrOperandId resultId) {
  MDefinition* lhs = getOperand(lhsId);
  MDefinition* rhs = getOperand(rhsId);

  auto* ins = T::New(alloc(), lhs, rhs);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitBigIntPtrAdd(IntPtrOperandId lhsId,
                                             IntPtrOperandId rhsId,
                                             IntPtrOperandId resultId) {
  return emitBigIntPtrBinaryArith<MBigIntPtrAdd>(lhsId, rhsId, resultId);
}

bool WarpCacheIRTranspiler::emitBigIntPtrSub(IntPtrOperandId lhsId,
                                             IntPtrOperandId rhsId,
                                             IntPtrOperandId resultId) {
  return emitBigIntPtrBinaryArith<MBigIntPtrSub>(lhsId, rhsId, resultId);
}

bool WarpCacheIRTranspiler::emitBigIntPtrMul(IntPtrOperandId lhsId,
                                             IntPtrOperandId rhsId,
                                             IntPtrOperandId resultId) {
  return emitBigIntPtrBinaryArith<MBigIntPtrMul>(lhsId, rhsId, resultId);
}

bool WarpCacheIRTranspiler::emitBigIntPtrDiv(IntPtrOperandId lhsId,
                                             IntPtrOperandId rhsId,
                                             IntPtrOperandId resultId) {
  return emitBigIntPtrBinaryArith<MBigIntPtrDiv>(lhsId, rhsId, resultId);
}

bool WarpCacheIRTranspiler::emitBigIntPtrMod(IntPtrOperandId lhsId,
                                             IntPtrOperandId rhsId,
                                             IntPtrOperandId resultId) {
  return emitBigIntPtrBinaryArith<MBigIntPtrMod>(lhsId, rhsId, resultId);
}

bool WarpCacheIRTranspiler::emitBigIntPtrPow(IntPtrOperandId lhsId,
                                             IntPtrOperandId rhsId,
                                             IntPtrOperandId resultId) {
  return emitBigIntPtrBinaryArith<MBigIntPtrPow>(lhsId, rhsId, resultId);
}

bool WarpCacheIRTranspiler::emitBigIntPtrBitOr(IntPtrOperandId lhsId,
                                               IntPtrOperandId rhsId,
                                               IntPtrOperandId resultId) {
  return emitBigIntPtrBinaryArith<MBigIntPtrBitOr>(lhsId, rhsId, resultId);
}

bool WarpCacheIRTranspiler::emitBigIntPtrBitXor(IntPtrOperandId lhsId,
                                                IntPtrOperandId rhsId,
                                                IntPtrOperandId resultId) {
  return emitBigIntPtrBinaryArith<MBigIntPtrBitXor>(lhsId, rhsId, resultId);
}

bool WarpCacheIRTranspiler::emitBigIntPtrBitAnd(IntPtrOperandId lhsId,
                                                IntPtrOperandId rhsId,
                                                IntPtrOperandId resultId) {
  return emitBigIntPtrBinaryArith<MBigIntPtrBitAnd>(lhsId, rhsId, resultId);
}

bool WarpCacheIRTranspiler::emitBigIntPtrLeftShift(IntPtrOperandId lhsId,
                                                   IntPtrOperandId rhsId,
                                                   IntPtrOperandId resultId) {
  return emitBigIntPtrBinaryArith<MBigIntPtrLsh>(lhsId, rhsId, resultId);
}

bool WarpCacheIRTranspiler::emitBigIntPtrRightShift(IntPtrOperandId lhsId,
                                                    IntPtrOperandId rhsId,
                                                    IntPtrOperandId resultId) {
  return emitBigIntPtrBinaryArith<MBigIntPtrRsh>(lhsId, rhsId, resultId);
}

bool WarpCacheIRTranspiler::emitBigIntPtrInc(IntPtrOperandId inputId,
                                             IntPtrOperandId resultId) {
  MDefinition* input = getOperand(inputId);

  auto* constOne = MConstant::NewIntPtr(alloc(), 1);
  add(constOne);

  auto* ins = MBigIntPtrAdd::New(alloc(), input, constOne);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitBigIntPtrDec(IntPtrOperandId inputId,
                                             IntPtrOperandId resultId) {
  MDefinition* input = getOperand(inputId);

  auto* constOne = MConstant::NewIntPtr(alloc(), 1);
  add(constOne);

  auto* ins = MBigIntPtrSub::New(alloc(), input, constOne);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitBigIntPtrNegation(IntPtrOperandId inputId,
                                                  IntPtrOperandId resultId) {
  MDefinition* input = getOperand(inputId);

  auto* constNegOne = MConstant::NewIntPtr(alloc(), -1);
  add(constNegOne);

  auto* ins = MBigIntPtrMul::New(alloc(), input, constNegOne);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitBigIntPtrNot(IntPtrOperandId inputId,
                                             IntPtrOperandId resultId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MBigIntPtrBitNot::New(alloc(), input);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitConcatStringsResult(StringOperandId lhsId,
                                                    StringOperandId rhsId,
                                                    uint32_t stubOffset) {
  MDefinition* lhs = getOperand(lhsId);
  MDefinition* rhs = getOperand(rhsId);

  auto* ins = MConcat::New(alloc(), lhs, rhs);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitCompareResult(
    JSOp op, OperandId lhsId, OperandId rhsId,
    MCompare::CompareType compareType) {
  MDefinition* lhs = getOperand(lhsId);
  MDefinition* rhs = getOperand(rhsId);

  auto* ins = MCompare::New(alloc(), lhs, rhs, op, compareType);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitCompareInt32Result(JSOp op,
                                                   Int32OperandId lhsId,
                                                   Int32OperandId rhsId) {
  return emitCompareResult(op, lhsId, rhsId, MCompare::Compare_Int32);
}

bool WarpCacheIRTranspiler::emitCompareDoubleResult(JSOp op,
                                                    NumberOperandId lhsId,
                                                    NumberOperandId rhsId) {
  return emitCompareResult(op, lhsId, rhsId, MCompare::Compare_Double);
}

bool WarpCacheIRTranspiler::emitCompareObjectResult(JSOp op, ObjOperandId lhsId,
                                                    ObjOperandId rhsId) {
  MOZ_ASSERT(IsEqualityOp(op));
  return emitCompareResult(op, lhsId, rhsId, MCompare::Compare_Object);
}

bool WarpCacheIRTranspiler::emitCompareStringResult(JSOp op,
                                                    StringOperandId lhsId,
                                                    StringOperandId rhsId) {
  return emitCompareResult(op, lhsId, rhsId, MCompare::Compare_String);
}

bool WarpCacheIRTranspiler::emitCompareSymbolResult(JSOp op,
                                                    SymbolOperandId lhsId,
                                                    SymbolOperandId rhsId) {
  MOZ_ASSERT(IsEqualityOp(op));
  return emitCompareResult(op, lhsId, rhsId, MCompare::Compare_Symbol);
}

bool WarpCacheIRTranspiler::emitCompareBigIntResult(JSOp op,
                                                    BigIntOperandId lhsId,
                                                    BigIntOperandId rhsId) {
  return emitCompareResult(op, lhsId, rhsId, MCompare::Compare_BigInt);
}

bool WarpCacheIRTranspiler::emitCompareBigIntInt32Result(JSOp op,
                                                         BigIntOperandId lhsId,
                                                         Int32OperandId rhsId) {
  return emitCompareResult(op, lhsId, rhsId, MCompare::Compare_BigInt_Int32);
}

bool WarpCacheIRTranspiler::emitCompareBigIntNumberResult(
    JSOp op, BigIntOperandId lhsId, NumberOperandId rhsId) {
  return emitCompareResult(op, lhsId, rhsId, MCompare::Compare_BigInt_Double);
}

bool WarpCacheIRTranspiler::emitCompareBigIntStringResult(
    JSOp op, BigIntOperandId lhsId, StringOperandId rhsId) {
  return emitCompareResult(op, lhsId, rhsId, MCompare::Compare_BigInt_String);
}

bool WarpCacheIRTranspiler::emitCompareNullUndefinedResult(
    JSOp op, bool isUndefined, ValOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  MOZ_ASSERT(IsEqualityOp(op));

  // A previously emitted guard ensures that one side of the comparison
  // is null or undefined.
  MDefinition* cst =
      isUndefined ? constant(UndefinedValue()) : constant(NullValue());
  auto compareType =
      isUndefined ? MCompare::Compare_Undefined : MCompare::Compare_Null;
  auto* ins = MCompare::New(alloc(), input, cst, op, compareType);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitCompareDoubleSameValueResult(
    NumberOperandId lhsId, NumberOperandId rhsId) {
  MDefinition* lhs = getOperand(lhsId);
  MDefinition* rhs = getOperand(rhsId);

  auto* sameValue = MSameValueDouble::New(alloc(), lhs, rhs);
  add(sameValue);

  pushResult(sameValue);
  return true;
}

bool WarpCacheIRTranspiler::emitSameValueResult(ValOperandId lhsId,
                                                ValOperandId rhsId) {
  MDefinition* lhs = getOperand(lhsId);
  MDefinition* rhs = getOperand(rhsId);

  auto* sameValue = MSameValue::New(alloc(), lhs, rhs);
  add(sameValue);

  pushResult(sameValue);
  return true;
}

bool WarpCacheIRTranspiler::emitIndirectTruncateInt32Result(
    Int32OperandId valId) {
  MDefinition* val = getOperand(valId);
  MOZ_ASSERT(val->type() == MIRType::Int32);

  auto* truncate =
      MLimitedTruncate::New(alloc(), val, TruncateKind::IndirectTruncate);
  add(truncate);

  pushResult(truncate);
  return true;
}

bool WarpCacheIRTranspiler::emitMathHypot2NumberResult(
    NumberOperandId firstId, NumberOperandId secondId) {
  MDefinitionVector vector(alloc());
  if (!vector.reserve(2)) {
    return false;
  }

  vector.infallibleAppend(getOperand(firstId));
  vector.infallibleAppend(getOperand(secondId));

  auto* ins = MHypot::New(alloc(), vector);
  if (!ins) {
    return false;
  }
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathHypot3NumberResult(
    NumberOperandId firstId, NumberOperandId secondId,
    NumberOperandId thirdId) {
  MDefinitionVector vector(alloc());
  if (!vector.reserve(3)) {
    return false;
  }

  vector.infallibleAppend(getOperand(firstId));
  vector.infallibleAppend(getOperand(secondId));
  vector.infallibleAppend(getOperand(thirdId));

  auto* ins = MHypot::New(alloc(), vector);
  if (!ins) {
    return false;
  }
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathHypot4NumberResult(
    NumberOperandId firstId, NumberOperandId secondId, NumberOperandId thirdId,
    NumberOperandId fourthId) {
  MDefinitionVector vector(alloc());
  if (!vector.reserve(4)) {
    return false;
  }

  vector.infallibleAppend(getOperand(firstId));
  vector.infallibleAppend(getOperand(secondId));
  vector.infallibleAppend(getOperand(thirdId));
  vector.infallibleAppend(getOperand(fourthId));

  auto* ins = MHypot::New(alloc(), vector);
  if (!ins) {
    return false;
  }
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathRandomResult(uint32_t rngOffset) {
#ifdef DEBUG
  // CodeGenerator uses CompileRealm::addressOfRandomNumberGenerator. Assert it
  // matches the RNG pointer stored in the stub field.
  const void* rng = rawPointerField(rngOffset);
  MOZ_ASSERT(rng == mirGen().realm->addressOfRandomNumberGenerator());
#endif

  auto* ins = MRandom::New(alloc());
  addEffectful(ins);

  pushResult(ins);
  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitInt32MinMax(bool isMax, Int32OperandId firstId,
                                            Int32OperandId secondId,
                                            Int32OperandId resultId) {
  MDefinition* first = getOperand(firstId);
  MDefinition* second = getOperand(secondId);

  auto* ins = MMinMax::New(alloc(), first, second, MIRType::Int32, isMax);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitNumberMinMax(bool isMax,
                                             NumberOperandId firstId,
                                             NumberOperandId secondId,
                                             NumberOperandId resultId) {
  MDefinition* first = getOperand(firstId);
  MDefinition* second = getOperand(secondId);

  auto* ins = MMinMax::New(alloc(), first, second, MIRType::Double, isMax);
  add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitInt32MinMaxArrayResult(ObjOperandId arrayId,
                                                       bool isMax) {
  MDefinition* array = getOperand(arrayId);

  auto* ins = MMinMaxArray::New(alloc(), array, MIRType::Int32, isMax);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitNumberMinMaxArrayResult(ObjOperandId arrayId,
                                                        bool isMax) {
  MDefinition* array = getOperand(arrayId);

  auto* ins = MMinMaxArray::New(alloc(), array, MIRType::Double, isMax);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathAbsInt32Result(Int32OperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MAbs::New(alloc(), input, MIRType::Int32);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathAbsNumberResult(NumberOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MAbs::New(alloc(), input, MIRType::Double);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathClz32Result(Int32OperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MClz::New(alloc(), input, MIRType::Int32);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathSignInt32Result(Int32OperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MSign::New(alloc(), input, MIRType::Int32);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathSignNumberResult(NumberOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MSign::New(alloc(), input, MIRType::Double);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathSignNumberToInt32Result(
    NumberOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MSign::New(alloc(), input, MIRType::Int32);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathImulResult(Int32OperandId lhsId,
                                               Int32OperandId rhsId) {
  MDefinition* lhs = getOperand(lhsId);
  MDefinition* rhs = getOperand(rhsId);

  auto* ins = MMul::New(alloc(), lhs, rhs, MIRType::Int32, MMul::Integer);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathFloorToInt32Result(
    NumberOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MFloor::New(alloc(), input);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathCeilToInt32Result(NumberOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MCeil::New(alloc(), input);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathTruncToInt32Result(
    NumberOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MTrunc::New(alloc(), input);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathRoundToInt32Result(
    NumberOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MRound::New(alloc(), input);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathSqrtNumberResult(NumberOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MSqrt::New(alloc(), input, MIRType::Double);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathFRoundNumberResult(
    NumberOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MToFloat32::New(alloc(), input);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathF16RoundNumberResult(
    NumberOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MToFloat16::New(alloc(), input);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathAtan2NumberResult(NumberOperandId yId,
                                                      NumberOperandId xId) {
  MDefinition* y = getOperand(yId);
  MDefinition* x = getOperand(xId);

  auto* ins = MAtan2::New(alloc(), y, x);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathFunctionNumberResult(
    NumberOperandId inputId, UnaryMathFunction fun) {
  MDefinition* input = getOperand(inputId);

  auto* ins = MMathFunction::New(alloc(), input, fun);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathFloorNumberResult(NumberOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  MInstruction* ins;
  if (MNearbyInt::HasAssemblerSupport(RoundingMode::Down)) {
    ins = MNearbyInt::New(alloc(), input, MIRType::Double, RoundingMode::Down);
  } else {
    ins = MMathFunction::New(alloc(), input, UnaryMathFunction::Floor);
  }
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathCeilNumberResult(NumberOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  MInstruction* ins;
  if (MNearbyInt::HasAssemblerSupport(RoundingMode::Up)) {
    ins = MNearbyInt::New(alloc(), input, MIRType::Double, RoundingMode::Up);
  } else {
    ins = MMathFunction::New(alloc(), input, UnaryMathFunction::Ceil);
  }
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMathTruncNumberResult(NumberOperandId inputId) {
  MDefinition* input = getOperand(inputId);

  MInstruction* ins;
  if (MNearbyInt::HasAssemblerSupport(RoundingMode::TowardsZero)) {
    ins = MNearbyInt::New(alloc(), input, MIRType::Double,
                          RoundingMode::TowardsZero);
  } else {
    ins = MMathFunction::New(alloc(), input, UnaryMathFunction::Trunc);
  }
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitNumberParseIntResult(StringOperandId strId,
                                                     Int32OperandId radixId) {
  MDefinition* str = getOperand(strId);
  MDefinition* radix = getOperand(radixId);

  auto* ins = MNumberParseInt::New(alloc(), str, radix);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitDoubleParseIntResult(NumberOperandId numId) {
  MDefinition* num = getOperand(numId);

  auto* ins = MDoubleParseInt::New(alloc(), num);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitObjectToStringResult(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MObjectClassToString::New(alloc(), obj);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitReflectGetPrototypeOfResult(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MGetPrototypeOf::New(alloc(), obj);
  addEffectful(ins);
  pushResult(ins);

  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitArrayPush(ObjOperandId objId,
                                          ValOperandId rhsId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* value = getOperand(rhsId);

  auto* ins = MArrayPush::New(alloc(), obj, value);
  addEffectful(ins);
  pushResult(ins);

  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitArrayJoinResult(ObjOperandId objId,
                                                StringOperandId sepId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* sep = getOperand(sepId);

  auto* join = MArrayJoin::New(alloc(), obj, sep);
  addEffectful(join);

  pushResult(join);
  return resumeAfter(join);
}

bool WarpCacheIRTranspiler::emitObjectKeysResult(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* join = MObjectKeys::New(alloc(), obj);
  addEffectful(join);

  pushResult(join);
  return resumeAfter(join);
}

bool WarpCacheIRTranspiler::emitPackedArrayPopResult(ObjOperandId arrayId) {
  MDefinition* array = getOperand(arrayId);

  auto* ins = MArrayPopShift::New(alloc(), array, MArrayPopShift::Pop);
  addEffectful(ins);

  pushResult(ins);
  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitPackedArrayShiftResult(ObjOperandId arrayId) {
  MDefinition* array = getOperand(arrayId);

  auto* ins = MArrayPopShift::New(alloc(), array, MArrayPopShift::Shift);
  addEffectful(ins);

  pushResult(ins);
  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitPackedArraySliceResult(
    uint32_t templateObjectOffset, ObjOperandId arrayId, Int32OperandId beginId,
    Int32OperandId endId) {
  JSObject* templateObj = tenuredObjectStubField(templateObjectOffset);

  MDefinition* array = getOperand(arrayId);
  MDefinition* begin = getOperand(beginId);
  MDefinition* end = getOperand(endId);

  // TODO: support pre-tenuring.
  gc::Heap heap = gc::Heap::Default;

  auto* ins = MArraySlice::New(alloc(), array, begin, end, templateObj, heap);
  addEffectful(ins);

  pushResult(ins);
  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitArgumentsSliceResult(
    uint32_t templateObjectOffset, ObjOperandId argsId, Int32OperandId beginId,
    Int32OperandId endId) {
  JSObject* templateObj = tenuredObjectStubField(templateObjectOffset);

  MDefinition* args = getOperand(argsId);
  MDefinition* begin = getOperand(beginId);
  MDefinition* end = getOperand(endId);

  // TODO: support pre-tenuring.
  gc::Heap heap = gc::Heap::Default;

  auto* ins =
      MArgumentsSlice::New(alloc(), args, begin, end, templateObj, heap);
  addEffectful(ins);

  pushResult(ins);
  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitHasClassResult(ObjOperandId objId,
                                               uint32_t claspOffset) {
  MDefinition* obj = getOperand(objId);
  const JSClass* clasp = classStubField(claspOffset);

  auto* hasClass = MHasClass::New(alloc(), obj, clasp);
  add(hasClass);

  pushResult(hasClass);
  return true;
}

bool WarpCacheIRTranspiler::emitHasShapeResult(ObjOperandId objId,
                                               uint32_t shapeOffset) {
  MDefinition* obj = getOperand(objId);
  Shape* shape = shapeStubField(shapeOffset);

  auto* hasShape = MHasShape::New(alloc(), obj, shape);
  add(hasShape);

  pushResult(hasShape);
  return true;
}

bool WarpCacheIRTranspiler::emitCallRegExpMatcherResult(
    ObjOperandId regexpId, StringOperandId inputId, Int32OperandId lastIndexId,
    uint32_t stubOffset) {
  MDefinition* regexp = getOperand(regexpId);
  MDefinition* input = getOperand(inputId);
  MDefinition* lastIndex = getOperand(lastIndexId);

  auto* matcher = MRegExpMatcher::New(alloc(), regexp, input, lastIndex);
  addEffectful(matcher);
  pushResult(matcher);

  return resumeAfter(matcher);
}

bool WarpCacheIRTranspiler::emitCallRegExpSearcherResult(
    ObjOperandId regexpId, StringOperandId inputId, Int32OperandId lastIndexId,
    uint32_t stubOffset) {
  MDefinition* regexp = getOperand(regexpId);
  MDefinition* input = getOperand(inputId);
  MDefinition* lastIndex = getOperand(lastIndexId);

  auto* searcher = MRegExpSearcher::New(alloc(), regexp, input, lastIndex);
  addEffectful(searcher);
  pushResult(searcher);

  return resumeAfter(searcher);
}

bool WarpCacheIRTranspiler::emitRegExpSearcherLastLimitResult() {
  auto* limit = MRegExpSearcherLastLimit::New(alloc());
  addEffectful(limit);
  pushResult(limit);

  return resumeAfter(limit);
}

bool WarpCacheIRTranspiler::emitRegExpBuiltinExecMatchResult(
    ObjOperandId regexpId, StringOperandId inputId, uint32_t stubOffset) {
  MDefinition* regexp = getOperand(regexpId);
  MDefinition* input = getOperand(inputId);

  auto* ins = MRegExpExecMatch::New(alloc(), regexp, input);
  addEffectful(ins);
  pushResult(ins);

  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitRegExpBuiltinExecTestResult(
    ObjOperandId regexpId, StringOperandId inputId, uint32_t stubOffset) {
  MDefinition* regexp = getOperand(regexpId);
  MDefinition* input = getOperand(inputId);

  auto* ins = MRegExpExecTest::New(alloc(), regexp, input);
  addEffectful(ins);
  pushResult(ins);

  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitRegExpHasCaptureGroupsResult(
    ObjOperandId regexpId, StringOperandId inputId) {
  MDefinition* regexp = getOperand(regexpId);
  MDefinition* input = getOperand(inputId);

  auto* result = MRegExpHasCaptureGroups::New(alloc(), regexp, input);
  addEffectful(result);
  pushResult(result);

  return resumeAfter(result);
}

MInstruction* WarpCacheIRTranspiler::convertToBoolean(MDefinition* input) {
  // Convert to bool with the '!!' idiom.
  //
  // The FoldTests and GVN passes both specifically handle this pattern. If you
  // change this code, make sure to update FoldTests and GVN, too.

  auto* resultInverted = MNot::New(alloc(), input);
  add(resultInverted);
  auto* result = MNot::New(alloc(), resultInverted);
  add(result);

  return result;
}

bool WarpCacheIRTranspiler::emitRegExpFlagResult(ObjOperandId regexpId,
                                                 int32_t flagsMask) {
  MDefinition* regexp = getOperand(regexpId);

  auto* flags = MLoadFixedSlot::New(alloc(), regexp, RegExpObject::flagsSlot());
  flags->setResultType(MIRType::Int32);
  add(flags);

  auto* mask = MConstant::New(alloc(), Int32Value(flagsMask));
  add(mask);

  auto* maskedFlag = MBitAnd::New(alloc(), flags, mask, MIRType::Int32);
  add(maskedFlag);

  auto* result = convertToBoolean(maskedFlag);

  pushResult(result);
  return true;
}

bool WarpCacheIRTranspiler::emitCallSubstringKernelResult(
    StringOperandId strId, Int32OperandId beginId, Int32OperandId lengthId) {
  MDefinition* str = getOperand(strId);
  MDefinition* begin = getOperand(beginId);
  MDefinition* length = getOperand(lengthId);

  auto* substr = MSubstr::New(alloc(), str, begin, length);
  add(substr);

  pushResult(substr);
  return true;
}

bool WarpCacheIRTranspiler::emitStringReplaceStringResult(
    StringOperandId strId, StringOperandId patternId,
    StringOperandId replacementId) {
  MDefinition* str = getOperand(strId);
  MDefinition* pattern = getOperand(patternId);
  MDefinition* replacement = getOperand(replacementId);

  auto* replace = MStringReplace::New(alloc(), str, pattern, replacement);
  add(replace);

  pushResult(replace);
  return true;
}

bool WarpCacheIRTranspiler::emitStringSplitStringResult(
    StringOperandId strId, StringOperandId separatorId) {
  MDefinition* str = getOperand(strId);
  MDefinition* separator = getOperand(separatorId);

  auto* split = MStringSplit::New(alloc(), str, separator);
  add(split);

  pushResult(split);
  return true;
}

bool WarpCacheIRTranspiler::emitGetFirstDollarIndexResult(
    StringOperandId strId) {
  MDefinition* str = getOperand(strId);

  auto* firstDollarIndex = MGetFirstDollarIndex::New(alloc(), str);
  add(firstDollarIndex);

  pushResult(firstDollarIndex);
  return true;
}

bool WarpCacheIRTranspiler::emitIsArrayResult(ValOperandId inputId) {
  MDefinition* value = getOperand(inputId);

  auto* isArray = MIsArray::New(alloc(), value);
  addEffectful(isArray);
  pushResult(isArray);

  return resumeAfter(isArray);
}

bool WarpCacheIRTranspiler::emitIsObjectResult(ValOperandId inputId) {
  MDefinition* value = getOperand(inputId);

  if (value->type() == MIRType::Object) {
    pushResult(constant(BooleanValue(true)));
  } else {
    auto* isObject = MIsObject::New(alloc(), value);
    add(isObject);
    pushResult(isObject);
  }

  return true;
}

bool WarpCacheIRTranspiler::emitIsPackedArrayResult(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* isPackedArray = MIsPackedArray::New(alloc(), obj);
  add(isPackedArray);

  pushResult(isPackedArray);
  return true;
}

bool WarpCacheIRTranspiler::emitIsCallableResult(ValOperandId inputId) {
  MDefinition* value = getOperand(inputId);

  auto* isCallable = MIsCallable::New(alloc(), value);
  add(isCallable);

  pushResult(isCallable);
  return true;
}

bool WarpCacheIRTranspiler::emitIsConstructorResult(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* isConstructor = MIsConstructor::New(alloc(), obj);
  add(isConstructor);

  pushResult(isConstructor);
  return true;
}

bool WarpCacheIRTranspiler::emitIsCrossRealmArrayConstructorResult(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MIsCrossRealmArrayConstructor::New(alloc(), obj);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitIsTypedArrayResult(ObjOperandId objId,
                                                   bool isPossiblyWrapped) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MIsTypedArray::New(alloc(), obj, isPossiblyWrapped);
  if (isPossiblyWrapped) {
    addEffectful(ins);
  } else {
    add(ins);
  }

  pushResult(ins);

  if (isPossiblyWrapped) {
    if (!resumeAfter(ins)) {
      return false;
    }
  }

  return true;
}

bool WarpCacheIRTranspiler::emitArrayBufferViewByteOffsetInt32Result(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* byteOffset = MArrayBufferViewByteOffset::New(alloc(), obj);
  add(byteOffset);

  auto* byteOffsetInt32 = MNonNegativeIntPtrToInt32::New(alloc(), byteOffset);
  add(byteOffsetInt32);

  pushResult(byteOffsetInt32);
  return true;
}

bool WarpCacheIRTranspiler::emitArrayBufferViewByteOffsetDoubleResult(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* byteOffset = MArrayBufferViewByteOffset::New(alloc(), obj);
  add(byteOffset);

  auto* byteOffsetDouble = MIntPtrToDouble::New(alloc(), byteOffset);
  add(byteOffsetDouble);

  pushResult(byteOffsetDouble);
  return true;
}

bool WarpCacheIRTranspiler::
    emitResizableTypedArrayByteOffsetMaybeOutOfBoundsInt32Result(
        ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* byteOffset =
      MResizableTypedArrayByteOffsetMaybeOutOfBounds::New(alloc(), obj);
  add(byteOffset);

  auto* byteOffsetInt32 = MNonNegativeIntPtrToInt32::New(alloc(), byteOffset);
  add(byteOffsetInt32);

  pushResult(byteOffsetInt32);
  return true;
}

bool WarpCacheIRTranspiler::
    emitResizableTypedArrayByteOffsetMaybeOutOfBoundsDoubleResult(
        ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* byteOffset =
      MResizableTypedArrayByteOffsetMaybeOutOfBounds::New(alloc(), obj);
  add(byteOffset);

  auto* byteOffsetDouble = MIntPtrToDouble::New(alloc(), byteOffset);
  add(byteOffsetDouble);

  pushResult(byteOffsetDouble);
  return true;
}

bool WarpCacheIRTranspiler::emitResizableTypedArrayLengthInt32Result(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  // Explicit |length| accesses are seq-consistent atomic loads.
  auto barrier = MemoryBarrierRequirement::Required;

  auto* length = MResizableTypedArrayLength::New(alloc(), obj, barrier);
  addEffectful(length);

  auto* lengthInt32 = MNonNegativeIntPtrToInt32::New(alloc(), length);
  add(lengthInt32);

  auto* postConversion = MPostIntPtrConversion::New(alloc(), lengthInt32);
  add(postConversion);

  pushResult(postConversion);
  return resumeAfterUnchecked(postConversion);
}

bool WarpCacheIRTranspiler::emitResizableTypedArrayLengthDoubleResult(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  // Explicit |length| accesses are seq-consistent atomic loads.
  auto barrier = MemoryBarrierRequirement::Required;

  auto* length = MResizableTypedArrayLength::New(alloc(), obj, barrier);
  addEffectful(length);

  auto* lengthDouble = MIntPtrToDouble::New(alloc(), length);
  add(lengthDouble);

  auto* postConversion = MPostIntPtrConversion::New(alloc(), lengthDouble);
  add(postConversion);

  pushResult(postConversion);
  return resumeAfterUnchecked(postConversion);
}

bool WarpCacheIRTranspiler::emitTypedArrayByteLengthInt32Result(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* length = MArrayBufferViewLength::New(alloc(), obj);
  add(length);

  auto* lengthInt32 = MNonNegativeIntPtrToInt32::New(alloc(), length);
  add(lengthInt32);

  auto* size = MTypedArrayElementSize::New(alloc(), obj);
  add(size);

  auto* mul = MMul::New(alloc(), lengthInt32, size, MIRType::Int32);
  mul->setCanBeNegativeZero(false);
  add(mul);

  pushResult(mul);
  return true;
}

bool WarpCacheIRTranspiler::emitTypedArrayByteLengthDoubleResult(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* length = MArrayBufferViewLength::New(alloc(), obj);
  add(length);

  auto* lengthDouble = MIntPtrToDouble::New(alloc(), length);
  add(lengthDouble);

  auto* size = MTypedArrayElementSize::New(alloc(), obj);
  add(size);

  auto* sizeDouble = MToDouble::New(alloc(), size);
  add(sizeDouble);

  auto* mul = MMul::New(alloc(), lengthDouble, sizeDouble, MIRType::Double);
  mul->setCanBeNegativeZero(false);
  add(mul);

  pushResult(mul);
  return true;
}

bool WarpCacheIRTranspiler::emitResizableTypedArrayByteLengthInt32Result(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  // Explicit |byteLength| accesses are seq-consistent atomic loads.
  auto barrier = MemoryBarrierRequirement::Required;

  auto* length = MResizableTypedArrayLength::New(alloc(), obj, barrier);
  addEffectful(length);

  auto* lengthInt32 = MNonNegativeIntPtrToInt32::New(alloc(), length);
  add(lengthInt32);

  auto* size = MTypedArrayElementSize::New(alloc(), obj);
  add(size);

  auto* mul = MMul::New(alloc(), lengthInt32, size, MIRType::Int32);
  mul->setCanBeNegativeZero(false);
  add(mul);

  auto* postConversion = MPostIntPtrConversion::New(alloc(), mul);
  add(postConversion);

  pushResult(postConversion);
  return resumeAfterUnchecked(postConversion);
}

bool WarpCacheIRTranspiler::emitResizableTypedArrayByteLengthDoubleResult(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  // Explicit |byteLength| accesses are seq-consistent atomic loads.
  auto barrier = MemoryBarrierRequirement::Required;

  auto* length = MResizableTypedArrayLength::New(alloc(), obj, barrier);
  addEffectful(length);

  auto* lengthDouble = MIntPtrToDouble::New(alloc(), length);
  add(lengthDouble);

  auto* size = MTypedArrayElementSize::New(alloc(), obj);
  add(size);

  auto* sizeDouble = MToDouble::New(alloc(), size);
  add(sizeDouble);

  auto* mul = MMul::New(alloc(), lengthDouble, sizeDouble, MIRType::Double);
  mul->setCanBeNegativeZero(false);
  add(mul);

  auto* postConversion = MPostIntPtrConversion::New(alloc(), mul);
  add(postConversion);

  pushResult(postConversion);
  return resumeAfterUnchecked(postConversion);
}

bool WarpCacheIRTranspiler::emitTypedArrayElementSizeResult(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MTypedArrayElementSize::New(alloc(), obj);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitResizableDataViewByteLengthInt32Result(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  // Explicit |byteLength| accesses are seq-consistent atomic loads.
  auto barrier = MemoryBarrierRequirement::Required;

  auto* length = MResizableDataViewByteLength::New(alloc(), obj, barrier);
  addEffectful(length);

  auto* lengthInt32 = MNonNegativeIntPtrToInt32::New(alloc(), length);
  add(lengthInt32);

  auto* postConversion = MPostIntPtrConversion::New(alloc(), lengthInt32);
  add(postConversion);

  pushResult(postConversion);
  return resumeAfterUnchecked(postConversion);
}

bool WarpCacheIRTranspiler::emitResizableDataViewByteLengthDoubleResult(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  // Explicit |byteLength| accesses are seq-consistent atomic loads.
  auto barrier = MemoryBarrierRequirement::Required;

  auto* length = MResizableDataViewByteLength::New(alloc(), obj, barrier);
  addEffectful(length);

  auto* lengthDouble = MIntPtrToDouble::New(alloc(), length);
  add(lengthDouble);

  auto* postConversion = MPostIntPtrConversion::New(alloc(), lengthDouble);
  add(postConversion);

  pushResult(postConversion);
  return resumeAfterUnchecked(postConversion);
}

bool WarpCacheIRTranspiler::emitGrowableSharedArrayBufferByteLengthInt32Result(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* length = MGrowableSharedArrayBufferByteLength::New(alloc(), obj);
  addEffectful(length);

  auto* lengthInt32 = MNonNegativeIntPtrToInt32::New(alloc(), length);
  add(lengthInt32);

  auto* postConversion = MPostIntPtrConversion::New(alloc(), lengthInt32);
  add(postConversion);

  pushResult(postConversion);
  return resumeAfterUnchecked(postConversion);
}

bool WarpCacheIRTranspiler::emitGrowableSharedArrayBufferByteLengthDoubleResult(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* length = MGrowableSharedArrayBufferByteLength::New(alloc(), obj);
  addEffectful(length);

  auto* lengthDouble = MIntPtrToDouble::New(alloc(), length);
  add(lengthDouble);

  auto* postConversion = MPostIntPtrConversion::New(alloc(), lengthDouble);
  add(postConversion);

  pushResult(postConversion);
  return resumeAfterUnchecked(postConversion);
}

bool WarpCacheIRTranspiler::emitGuardHasAttachedArrayBuffer(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MGuardHasAttachedArrayBuffer::New(alloc(), obj);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardResizableArrayBufferViewInBounds(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MGuardResizableArrayBufferViewInBounds::New(alloc(), obj);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardResizableArrayBufferViewInBoundsOrDetached(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins =
      MGuardResizableArrayBufferViewInBoundsOrDetached::New(alloc(), obj);
  add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitIsTypedArrayConstructorResult(
    ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MIsTypedArrayConstructor::New(alloc(), obj);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGetNextMapSetEntryForIteratorResult(
    ObjOperandId iterId, ObjOperandId resultArrId, bool isMap) {
  MDefinition* iter = getOperand(iterId);
  MDefinition* resultArr = getOperand(resultArrId);

  MGetNextEntryForIterator::Mode mode =
      isMap ? MGetNextEntryForIterator::Map : MGetNextEntryForIterator::Set;
  auto* ins = MGetNextEntryForIterator::New(alloc(), iter, resultArr, mode);
  addEffectful(ins);
  pushResult(ins);

  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitFrameIsConstructingResult() {
  if (const CallInfo* callInfo = builder_->inlineCallInfo()) {
    auto* ins = constant(BooleanValue(callInfo->constructing()));
    pushResult(ins);
    return true;
  }

  auto* ins = MIsConstructing::New(alloc());
  add(ins);
  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitNewIteratorResult(
    MNewIterator::Type type, uint32_t templateObjectOffset) {
  JSObject* templateObj = tenuredObjectStubField(templateObjectOffset);

  auto* templateConst = constant(ObjectValue(*templateObj));
  auto* iter = MNewIterator::New(alloc(), templateConst, type);
  add(iter);

  pushResult(iter);
  return true;
}

bool WarpCacheIRTranspiler::emitNewArrayIteratorResult(
    uint32_t templateObjectOffset) {
  return emitNewIteratorResult(MNewIterator::ArrayIterator,
                               templateObjectOffset);
}

bool WarpCacheIRTranspiler::emitNewStringIteratorResult(
    uint32_t templateObjectOffset) {
  return emitNewIteratorResult(MNewIterator::StringIterator,
                               templateObjectOffset);
}

bool WarpCacheIRTranspiler::emitNewRegExpStringIteratorResult(
    uint32_t templateObjectOffset) {
  return emitNewIteratorResult(MNewIterator::RegExpStringIterator,
                               templateObjectOffset);
}

bool WarpCacheIRTranspiler::emitObjectCreateResult(
    uint32_t templateObjectOffset) {
  JSObject* templateObj = tenuredObjectStubField(templateObjectOffset);

  auto* templateConst = constant(ObjectValue(*templateObj));

  // TODO: support pre-tenuring.
  gc::Heap heap = gc::Heap::Default;
  auto* obj =
      MNewObject::New(alloc(), templateConst, heap, MNewObject::ObjectCreate);
  addEffectful(obj);

  pushResult(obj);
  return resumeAfter(obj);
}

bool WarpCacheIRTranspiler::emitNewArrayFromLengthResult(
    uint32_t templateObjectOffset, Int32OperandId lengthId,
    uint32_t siteOffset) {
  JSObject* templateObj = tenuredObjectStubField(templateObjectOffset);
  MDefinition* length = getOperand(lengthId);
  gc::Heap heap = allocSiteInitialHeapField(siteOffset);

  if (length->isConstant()) {
    int32_t lenInt32 = length->toConstant()->toInt32();
    if (lenInt32 >= 0 &&
        uint32_t(lenInt32) == templateObj->as<ArrayObject>().length()) {
      uint32_t len = uint32_t(lenInt32);
      auto* templateConst = constant(ObjectValue(*templateObj));

      size_t inlineLength =
          gc::GetGCKindSlots(templateObj->asTenured().getAllocKind()) -
          ObjectElements::VALUES_PER_HEADER;

      MNewArray* obj;
      if (len > inlineLength) {
        obj = MNewArray::NewVM(alloc(), len, templateConst, heap);
      } else {
        obj = MNewArray::New(alloc(), len, templateConst, heap);
      }
      add(obj);
      pushResult(obj);
      return true;
    }
  }

  auto* obj = MNewArrayDynamicLength::New(alloc(), length, templateObj, heap);
  addEffectful(obj);
  pushResult(obj);
  return resumeAfter(obj);
}

bool WarpCacheIRTranspiler::emitNewTypedArrayFromLengthResult(
    uint32_t templateObjectOffset, Int32OperandId lengthId) {
  auto* templateObj = &tenuredObjectStubField(templateObjectOffset)
                           ->as<FixedLengthTypedArrayObject>();
  MDefinition* length = getOperand(lengthId);

  // TODO: support pre-tenuring.
  gc::Heap heap = gc::Heap::Default;

  if (length->isConstant()) {
    int32_t len = length->toConstant()->toInt32();
    if (len > 0 && uint32_t(len) == templateObj->length()) {
      auto* templateConst = constant(ObjectValue(*templateObj));
      auto* obj = MNewTypedArray::New(alloc(), templateConst, heap);
      add(obj);
      pushResult(obj);
      return true;
    }
  }

  auto* obj =
      MNewTypedArrayDynamicLength::New(alloc(), length, templateObj, heap);
  addEffectful(obj);
  pushResult(obj);
  return resumeAfter(obj);
}

bool WarpCacheIRTranspiler::emitNewTypedArrayFromArrayBufferResult(
    uint32_t templateObjectOffset, ObjOperandId bufferId,
    ValOperandId byteOffsetId, ValOperandId lengthId) {
  JSObject* templateObj = tenuredObjectStubField(templateObjectOffset);
  MDefinition* buffer = getOperand(bufferId);
  MDefinition* byteOffset = getOperand(byteOffsetId);
  MDefinition* length = getOperand(lengthId);

  // TODO: support pre-tenuring.
  gc::Heap heap = gc::Heap::Default;

  auto* obj = MNewTypedArrayFromArrayBuffer::New(alloc(), buffer, byteOffset,
                                                 length, templateObj, heap);
  addEffectful(obj);

  pushResult(obj);
  return resumeAfter(obj);
}

bool WarpCacheIRTranspiler::emitNewTypedArrayFromArrayResult(
    uint32_t templateObjectOffset, ObjOperandId arrayId) {
  JSObject* templateObj = tenuredObjectStubField(templateObjectOffset);
  MDefinition* array = getOperand(arrayId);

  // TODO: support pre-tenuring.
  gc::Heap heap = gc::Heap::Default;

  auto* obj = MNewTypedArrayFromArray::New(alloc(), array, templateObj, heap);
  addEffectful(obj);

  pushResult(obj);
  return resumeAfter(obj);
}

bool WarpCacheIRTranspiler::emitAtomicsCompareExchangeResult(
    ObjOperandId objId, IntPtrOperandId indexId, uint32_t expectedId,
    uint32_t replacementId, Scalar::Type elementType,
    ArrayBufferViewKind viewKind) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);
  MDefinition* expected = getOperand(ValOperandId(expectedId));
  MDefinition* replacement = getOperand(ValOperandId(replacementId));

  auto* length = emitTypedArrayLength(viewKind, obj);

  index = addBoundsCheck(index, length);

  auto* elements = MArrayBufferViewElements::New(alloc(), obj);
  add(elements);

  bool forceDoubleForUint32 = true;
  MIRType knownType =
      MIRTypeForArrayBufferViewRead(elementType, forceDoubleForUint32);

  auto* cas = MCompareExchangeTypedArrayElement::New(
      alloc(), elements, index, elementType, expected, replacement);
  cas->setResultType(knownType);
  addEffectful(cas);

  MInstruction* result = cas;
  if (Scalar::isBigIntType(elementType)) {
    result =
        MInt64ToBigInt::New(alloc(), cas, Scalar::isSignedIntType(elementType));

    // Make non-movable so we can attach a resume point.
    result->setNotMovable();

    add(result);
  }

  pushResult(result);
  return resumeAfterUnchecked(result);
}

bool WarpCacheIRTranspiler::emitAtomicsExchangeResult(
    ObjOperandId objId, IntPtrOperandId indexId, uint32_t valueId,
    Scalar::Type elementType, ArrayBufferViewKind viewKind) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);
  MDefinition* value = getOperand(ValOperandId(valueId));

  auto* length = emitTypedArrayLength(viewKind, obj);

  index = addBoundsCheck(index, length);

  auto* elements = MArrayBufferViewElements::New(alloc(), obj);
  add(elements);

  bool forceDoubleForUint32 = true;
  MIRType knownType =
      MIRTypeForArrayBufferViewRead(elementType, forceDoubleForUint32);

  auto* exchange = MAtomicExchangeTypedArrayElement::New(
      alloc(), elements, index, value, elementType);
  exchange->setResultType(knownType);
  addEffectful(exchange);

  MInstruction* result = exchange;
  if (Scalar::isBigIntType(elementType)) {
    result = MInt64ToBigInt::New(alloc(), exchange,
                                 Scalar::isSignedIntType(elementType));

    // Make non-movable so we can attach a resume point.
    result->setNotMovable();

    add(result);
  }

  pushResult(result);
  return resumeAfterUnchecked(result);
}

bool WarpCacheIRTranspiler::emitAtomicsBinaryOp(
    ObjOperandId objId, IntPtrOperandId indexId, uint32_t valueId,
    Scalar::Type elementType, bool forEffect, ArrayBufferViewKind viewKind,
    AtomicOp op) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);
  MDefinition* value = getOperand(ValOperandId(valueId));

  auto* length = emitTypedArrayLength(viewKind, obj);

  index = addBoundsCheck(index, length);

  auto* elements = MArrayBufferViewElements::New(alloc(), obj);
  add(elements);

  bool forceDoubleForUint32 = true;
  MIRType knownType =
      MIRTypeForArrayBufferViewRead(elementType, forceDoubleForUint32);

  auto* binop = MAtomicTypedArrayElementBinop::New(
      alloc(), op, elements, index, elementType, value, forEffect);
  if (!forEffect) {
    binop->setResultType(knownType);
  }
  addEffectful(binop);

  if (forEffect) {
    pushResult(constant(UndefinedValue()));
    return resumeAfter(binop);
  }

  MInstruction* result = binop;
  if (Scalar::isBigIntType(elementType)) {
    result = MInt64ToBigInt::New(alloc(), binop,
                                 Scalar::isSignedIntType(elementType));

    // Make non-movable so we can attach a resume point.
    result->setNotMovable();

    add(result);
  }

  pushResult(result);
  return resumeAfterUnchecked(result);
}

bool WarpCacheIRTranspiler::emitAtomicsAddResult(
    ObjOperandId objId, IntPtrOperandId indexId, uint32_t valueId,
    Scalar::Type elementType, bool forEffect, ArrayBufferViewKind viewKind) {
  return emitAtomicsBinaryOp(objId, indexId, valueId, elementType, forEffect,
                             viewKind, AtomicOp::Add);
}

bool WarpCacheIRTranspiler::emitAtomicsSubResult(
    ObjOperandId objId, IntPtrOperandId indexId, uint32_t valueId,
    Scalar::Type elementType, bool forEffect, ArrayBufferViewKind viewKind) {
  return emitAtomicsBinaryOp(objId, indexId, valueId, elementType, forEffect,
                             viewKind, AtomicOp::Sub);
}

bool WarpCacheIRTranspiler::emitAtomicsAndResult(
    ObjOperandId objId, IntPtrOperandId indexId, uint32_t valueId,
    Scalar::Type elementType, bool forEffect, ArrayBufferViewKind viewKind) {
  return emitAtomicsBinaryOp(objId, indexId, valueId, elementType, forEffect,
                             viewKind, AtomicOp::And);
}

bool WarpCacheIRTranspiler::emitAtomicsOrResult(
    ObjOperandId objId, IntPtrOperandId indexId, uint32_t valueId,
    Scalar::Type elementType, bool forEffect, ArrayBufferViewKind viewKind) {
  return emitAtomicsBinaryOp(objId, indexId, valueId, elementType, forEffect,
                             viewKind, AtomicOp::Or);
}

bool WarpCacheIRTranspiler::emitAtomicsXorResult(
    ObjOperandId objId, IntPtrOperandId indexId, uint32_t valueId,
    Scalar::Type elementType, bool forEffect, ArrayBufferViewKind viewKind) {
  return emitAtomicsBinaryOp(objId, indexId, valueId, elementType, forEffect,
                             viewKind, AtomicOp::Xor);
}

bool WarpCacheIRTranspiler::emitAtomicsLoadResult(
    ObjOperandId objId, IntPtrOperandId indexId, Scalar::Type elementType,
    ArrayBufferViewKind viewKind) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);

  auto* length = emitTypedArrayLength(viewKind, obj);

  index = addBoundsCheck(index, length);

  auto* elements = MArrayBufferViewElements::New(alloc(), obj);
  add(elements);

  bool forceDoubleForUint32 = true;
  MIRType knownType =
      MIRTypeForArrayBufferViewRead(elementType, forceDoubleForUint32);

  auto* load = MLoadUnboxedScalar::New(alloc(), elements, index, elementType,
                                       MemoryBarrierRequirement::Required);
  load->setResultType(knownType);
  addEffectful(load);

  MInstruction* result = load;
  if (Scalar::isBigIntType(elementType)) {
    result = MInt64ToBigInt::New(alloc(), load,
                                 Scalar::isSignedIntType(elementType));

    // Make non-movable so we can attach a resume point.
    result->setNotMovable();

    add(result);
  }

  pushResult(result);
  return resumeAfterUnchecked(result);
}

bool WarpCacheIRTranspiler::emitAtomicsStoreResult(
    ObjOperandId objId, IntPtrOperandId indexId, uint32_t valueId,
    Scalar::Type elementType, ArrayBufferViewKind viewKind) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);
  MDefinition* value = getOperand(ValOperandId(valueId));

  auto* length = emitTypedArrayLength(viewKind, obj);

  index = addBoundsCheck(index, length);

  auto* elements = MArrayBufferViewElements::New(alloc(), obj);
  add(elements);

  auto* store =
      MStoreUnboxedScalar::New(alloc(), elements, index, value, elementType,
                               MemoryBarrierRequirement::Required);
  addEffectful(store);

  pushResult(value);
  return resumeAfter(store);
}

bool WarpCacheIRTranspiler::emitAtomicsIsLockFreeResult(
    Int32OperandId valueId) {
  MDefinition* value = getOperand(valueId);

  auto* ilf = MAtomicIsLockFree::New(alloc(), value);
  add(ilf);

  pushResult(ilf);
  return true;
}

bool WarpCacheIRTranspiler::emitAtomicsPauseResult() {
  auto* ins = MAtomicPause::New(alloc());
  add(ins);

  pushResult(constant(UndefinedValue()));
  return true;
}

bool WarpCacheIRTranspiler::emitBigIntAsIntNResult(Int32OperandId bitsId,
                                                   BigIntOperandId bigIntId) {
  MDefinition* bits = getOperand(bitsId);
  MDefinition* bigInt = getOperand(bigIntId);

  auto* ins = MBigIntAsIntN::New(alloc(), bits, bigInt);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitBigIntAsUintNResult(Int32OperandId bitsId,
                                                    BigIntOperandId bigIntId) {
  MDefinition* bits = getOperand(bitsId);
  MDefinition* bigInt = getOperand(bigIntId);

  auto* ins = MBigIntAsUintN::New(alloc(), bits, bigInt);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardToNonGCThing(ValOperandId inputId) {
  MDefinition* def = getOperand(inputId);
  if (IsNonGCThing(def->type())) {
    return true;
  }

  auto* ins = MGuardNonGCThing::New(alloc(), def);
  add(ins);

  setOperand(inputId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitSetHasNonGCThingResult(ObjOperandId setId,
                                                       ValOperandId valId) {
  MDefinition* set = getOperand(setId);
  MDefinition* val = getOperand(valId);

  auto* hashValue = MToHashableNonGCThing::New(alloc(), val);
  add(hashValue);

  auto* hash = MHashNonGCThing::New(alloc(), hashValue);
  add(hash);

  auto* ins = MSetObjectHasNonBigInt::New(alloc(), set, hashValue, hash);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitSetHasStringResult(ObjOperandId setId,
                                                   StringOperandId strId) {
  MDefinition* set = getOperand(setId);
  MDefinition* str = getOperand(strId);

  auto* hashValue = MToHashableString::New(alloc(), str);
  add(hashValue);

  auto* hash = MHashString::New(alloc(), hashValue);
  add(hash);

  auto* ins = MSetObjectHasNonBigInt::New(alloc(), set, hashValue, hash);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitSetHasSymbolResult(ObjOperandId setId,
                                                   SymbolOperandId symId) {
  MDefinition* set = getOperand(setId);
  MDefinition* sym = getOperand(symId);

  auto* hash = MHashSymbol::New(alloc(), sym);
  add(hash);

  auto* ins = MSetObjectHasNonBigInt::New(alloc(), set, sym, hash);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitSetHasBigIntResult(ObjOperandId setId,
                                                   BigIntOperandId bigIntId) {
  MDefinition* set = getOperand(setId);
  MDefinition* bigInt = getOperand(bigIntId);

  auto* hash = MHashBigInt::New(alloc(), bigInt);
  add(hash);

  auto* ins = MSetObjectHasBigInt::New(alloc(), set, bigInt, hash);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitSetHasObjectResult(ObjOperandId setId,
                                                   ObjOperandId objId) {
  MDefinition* set = getOperand(setId);
  MDefinition* obj = getOperand(objId);

  auto* hash = MHashObject::New(alloc(), set, obj);
  add(hash);

  auto* ins = MSetObjectHasNonBigInt::New(alloc(), set, obj, hash);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitSetHasResult(ObjOperandId setId,
                                             ValOperandId valId) {
  MDefinition* set = getOperand(setId);
  MDefinition* val = getOperand(valId);

#ifdef JS_PUNBOX64
  auto* hashValue = MToHashableValue::New(alloc(), val);
  add(hashValue);

  auto* hash = MHashValue::New(alloc(), set, hashValue);
  add(hash);

  auto* ins = MSetObjectHasValue::New(alloc(), set, hashValue, hash);
  add(ins);
#else
  auto* ins = MSetObjectHasValueVMCall::New(alloc(), set, val);
  add(ins);
#endif

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitSetDeleteResult(ObjOperandId setId,
                                                ValOperandId keyId) {
  MDefinition* set = getOperand(setId);
  MDefinition* key = getOperand(keyId);

  auto* ins = MSetObjectDelete::New(alloc(), set, key);
  addEffectful(ins);

  pushResult(ins);
  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitSetAddResult(ObjOperandId setId,
                                             ValOperandId keyId) {
  MDefinition* set = getOperand(setId);
  MDefinition* key = getOperand(keyId);

  auto* ins = MSetObjectAdd::New(alloc(), set, key);
  addEffectful(ins);

  pushResult(set);
  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitSetSizeResult(ObjOperandId setId) {
  MDefinition* set = getOperand(setId);

  auto* ins = MSetObjectSize::New(alloc(), set);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMapHasNonGCThingResult(ObjOperandId mapId,
                                                       ValOperandId valId) {
  MDefinition* map = getOperand(mapId);
  MDefinition* val = getOperand(valId);

  auto* hashValue = MToHashableNonGCThing::New(alloc(), val);
  add(hashValue);

  auto* hash = MHashNonGCThing::New(alloc(), hashValue);
  add(hash);

  auto* ins = MMapObjectHasNonBigInt::New(alloc(), map, hashValue, hash);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMapHasStringResult(ObjOperandId mapId,
                                                   StringOperandId strId) {
  MDefinition* map = getOperand(mapId);
  MDefinition* str = getOperand(strId);

  auto* hashValue = MToHashableString::New(alloc(), str);
  add(hashValue);

  auto* hash = MHashString::New(alloc(), hashValue);
  add(hash);

  auto* ins = MMapObjectHasNonBigInt::New(alloc(), map, hashValue, hash);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMapHasSymbolResult(ObjOperandId mapId,
                                                   SymbolOperandId symId) {
  MDefinition* map = getOperand(mapId);
  MDefinition* sym = getOperand(symId);

  auto* hash = MHashSymbol::New(alloc(), sym);
  add(hash);

  auto* ins = MMapObjectHasNonBigInt::New(alloc(), map, sym, hash);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMapHasBigIntResult(ObjOperandId mapId,
                                                   BigIntOperandId bigIntId) {
  MDefinition* map = getOperand(mapId);
  MDefinition* bigInt = getOperand(bigIntId);

  auto* hash = MHashBigInt::New(alloc(), bigInt);
  add(hash);

  auto* ins = MMapObjectHasBigInt::New(alloc(), map, bigInt, hash);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMapHasObjectResult(ObjOperandId mapId,
                                                   ObjOperandId objId) {
  MDefinition* map = getOperand(mapId);
  MDefinition* obj = getOperand(objId);

  auto* hash = MHashObject::New(alloc(), map, obj);
  add(hash);

  auto* ins = MMapObjectHasNonBigInt::New(alloc(), map, obj, hash);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMapHasResult(ObjOperandId mapId,
                                             ValOperandId valId) {
  MDefinition* map = getOperand(mapId);
  MDefinition* val = getOperand(valId);

#ifdef JS_PUNBOX64
  auto* hashValue = MToHashableValue::New(alloc(), val);
  add(hashValue);

  auto* hash = MHashValue::New(alloc(), map, hashValue);
  add(hash);

  auto* ins = MMapObjectHasValue::New(alloc(), map, hashValue, hash);
  add(ins);
#else
  auto* ins = MMapObjectHasValueVMCall::New(alloc(), map, val);
  add(ins);
#endif

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMapGetNonGCThingResult(ObjOperandId mapId,
                                                       ValOperandId valId) {
  MDefinition* map = getOperand(mapId);
  MDefinition* val = getOperand(valId);

  auto* hashValue = MToHashableNonGCThing::New(alloc(), val);
  add(hashValue);

  auto* hash = MHashNonGCThing::New(alloc(), hashValue);
  add(hash);

  auto* ins = MMapObjectGetNonBigInt::New(alloc(), map, hashValue, hash);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMapGetStringResult(ObjOperandId mapId,
                                                   StringOperandId strId) {
  MDefinition* map = getOperand(mapId);
  MDefinition* str = getOperand(strId);

  auto* hashValue = MToHashableString::New(alloc(), str);
  add(hashValue);

  auto* hash = MHashString::New(alloc(), hashValue);
  add(hash);

  auto* ins = MMapObjectGetNonBigInt::New(alloc(), map, hashValue, hash);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMapGetSymbolResult(ObjOperandId mapId,
                                                   SymbolOperandId symId) {
  MDefinition* map = getOperand(mapId);
  MDefinition* sym = getOperand(symId);

  auto* hash = MHashSymbol::New(alloc(), sym);
  add(hash);

  auto* ins = MMapObjectGetNonBigInt::New(alloc(), map, sym, hash);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMapGetBigIntResult(ObjOperandId mapId,
                                                   BigIntOperandId bigIntId) {
  MDefinition* map = getOperand(mapId);
  MDefinition* bigInt = getOperand(bigIntId);

  auto* hash = MHashBigInt::New(alloc(), bigInt);
  add(hash);

  auto* ins = MMapObjectGetBigInt::New(alloc(), map, bigInt, hash);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMapGetObjectResult(ObjOperandId mapId,
                                                   ObjOperandId objId) {
  MDefinition* map = getOperand(mapId);
  MDefinition* obj = getOperand(objId);

  auto* hash = MHashObject::New(alloc(), map, obj);
  add(hash);

  auto* ins = MMapObjectGetNonBigInt::New(alloc(), map, obj, hash);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMapGetResult(ObjOperandId mapId,
                                             ValOperandId valId) {
  MDefinition* map = getOperand(mapId);
  MDefinition* val = getOperand(valId);

#ifdef JS_PUNBOX64
  auto* hashValue = MToHashableValue::New(alloc(), val);
  add(hashValue);

  auto* hash = MHashValue::New(alloc(), map, hashValue);
  add(hash);

  auto* ins = MMapObjectGetValue::New(alloc(), map, hashValue, hash);
  add(ins);
#else
  auto* ins = MMapObjectGetValueVMCall::New(alloc(), map, val);
  add(ins);
#endif

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitMapDeleteResult(ObjOperandId mapId,
                                                ValOperandId keyId) {
  MDefinition* map = getOperand(mapId);
  MDefinition* key = getOperand(keyId);

  auto* ins = MMapObjectDelete::New(alloc(), map, key);
  addEffectful(ins);

  pushResult(ins);
  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitMapSetResult(ObjOperandId mapId,
                                             ValOperandId keyId,
                                             ValOperandId valId) {
  MDefinition* map = getOperand(mapId);
  MDefinition* key = getOperand(keyId);
  MDefinition* val = getOperand(valId);

  auto* ins = MMapObjectSet::New(alloc(), map, key, val);
  addEffectful(ins);

  pushResult(map);
  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitMapSizeResult(ObjOperandId mapId) {
  MDefinition* map = getOperand(mapId);

  auto* ins = MMapObjectSize::New(alloc(), map);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitDateFillLocalTimeSlots(ObjOperandId dateId) {
  MDefinition* date = getOperand(dateId);

  // MDateFillLocalTimeSlots is effectful because it can store into fixed slots,
  // but it's safe to repeat this action after a bailout, therefore it's okay to
  // use |addEffectfulUnsafe|.
  auto* ins = MDateFillLocalTimeSlots::New(alloc(), date);
  addEffectfulUnsafe(ins);

  return true;
}

bool WarpCacheIRTranspiler::emitDateHoursFromSecondsIntoYearResult(
    ValOperandId secondsIntoYearId) {
  MDefinition* secondsIntoYear = getOperand(secondsIntoYearId);

  auto* ins = MDateHoursFromSecondsIntoYear::New(alloc(), secondsIntoYear);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitDateMinutesFromSecondsIntoYearResult(
    ValOperandId secondsIntoYearId) {
  MDefinition* secondsIntoYear = getOperand(secondsIntoYearId);

  auto* ins = MDateMinutesFromSecondsIntoYear::New(alloc(), secondsIntoYear);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitDateSecondsFromSecondsIntoYearResult(
    ValOperandId secondsIntoYearId) {
  MDefinition* secondsIntoYear = getOperand(secondsIntoYearId);

  auto* ins = MDateSecondsFromSecondsIntoYear::New(alloc(), secondsIntoYear);
  add(ins);

  pushResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitTruthyResult(OperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* result = convertToBoolean(input);

  pushResult(result);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadInt32TruthyResult(ValOperandId inputId) {
  return emitTruthyResult(inputId);
}

bool WarpCacheIRTranspiler::emitLoadDoubleTruthyResult(
    NumberOperandId inputId) {
  return emitTruthyResult(inputId);
}

bool WarpCacheIRTranspiler::emitLoadStringTruthyResult(
    StringOperandId inputId) {
  return emitTruthyResult(inputId);
}

bool WarpCacheIRTranspiler::emitLoadObjectTruthyResult(ObjOperandId inputId) {
  return emitTruthyResult(inputId);
}

bool WarpCacheIRTranspiler::emitLoadBigIntTruthyResult(
    BigIntOperandId inputId) {
  return emitTruthyResult(inputId);
}

bool WarpCacheIRTranspiler::emitLoadValueTruthyResult(ValOperandId inputId) {
  return emitTruthyResult(inputId);
}

bool WarpCacheIRTranspiler::emitLoadOperandResult(ValOperandId inputId) {
  MDefinition* input = getOperand(inputId);
  pushResult(input);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadWrapperTarget(ObjOperandId objId,
                                                  ObjOperandId resultId,
                                                  bool fallible) {
  MDefinition* obj = getOperand(objId);

  auto* ins = MLoadWrapperTarget::New(alloc(), obj, fallible);
  if (fallible) {
    ins->setGuard();
  }
  add(ins);

  return defineOperand(resultId, ins);
}

// When we transpile a call, we may generate guards for some
// arguments.  To make sure the call instruction depends on those
// guards, when the transpiler creates an operand for an argument, we
// register the OperandId of that argument in argumentIds_. (See
// emitLoadArgumentSlot.) Before generating the call, we update the
// CallInfo to use the appropriate value from operands_.
// Note: The callee is an explicit argument to the call op, and is
// tracked separately.
void WarpCacheIRTranspiler::updateArgumentsFromOperands() {
  for (uint32_t i = 0; i < uint32_t(ArgumentKind::NumKinds); i++) {
    ArgumentKind kind = ArgumentKind(i);
    OperandId id = argumentOperandIds_[kind];
    if (id.valid()) {
      switch (kind) {
        case ArgumentKind::This:
          callInfo_->setThis(getOperand(id));
          break;
        case ArgumentKind::NewTarget:
          callInfo_->setNewTarget(getOperand(id));
          break;
        case ArgumentKind::Arg0:
          callInfo_->setArg(0, getOperand(id));
          break;
        case ArgumentKind::Arg1:
          callInfo_->setArg(1, getOperand(id));
          break;
        case ArgumentKind::Arg2:
          callInfo_->setArg(2, getOperand(id));
          break;
        case ArgumentKind::Arg3:
          callInfo_->setArg(3, getOperand(id));
          break;
        case ArgumentKind::Arg4:
          callInfo_->setArg(4, getOperand(id));
          break;
        case ArgumentKind::Arg5:
          callInfo_->setArg(5, getOperand(id));
          break;
        case ArgumentKind::Arg6:
          callInfo_->setArg(6, getOperand(id));
          break;
        case ArgumentKind::Arg7:
          callInfo_->setArg(7, getOperand(id));
          break;
        case ArgumentKind::Callee:
        case ArgumentKind::NumKinds:
          MOZ_CRASH("Unexpected argument kind");
      }
    }
  }
}

bool WarpCacheIRTranspiler::emitLoadArgumentSlot(ValOperandId resultId,
                                                 uint32_t slotIndex) {
  // Reverse of GetIndexOfArgument.

  // Layout:
  // NewTarget | Args.. (reversed)      | ThisValue | Callee
  // 0         | ArgC .. Arg1 Arg0 (+1) | argc (+1) | argc + 1 (+ 1)
  // ^ (if constructing)

  // NewTarget (optional)
  if (callInfo_->constructing()) {
    if (slotIndex == 0) {
      setArgumentId(ArgumentKind::NewTarget, resultId);
      return defineOperand(resultId, callInfo_->getNewTarget());
    }

    slotIndex -= 1;  // Adjust slot index to match non-constructing calls.
  }

  // Args..
  if (slotIndex < callInfo_->argc()) {
    uint32_t arg = callInfo_->argc() - 1 - slotIndex;
    ArgumentKind kind = ArgumentKindForArgIndex(arg);
    MOZ_ASSERT(kind < ArgumentKind::NumKinds);
    setArgumentId(kind, resultId);
    return defineOperand(resultId, callInfo_->getArg(arg));
  }

  // ThisValue
  if (slotIndex == callInfo_->argc()) {
    setArgumentId(ArgumentKind::This, resultId);
    return defineOperand(resultId, callInfo_->thisArg());
  }

  // Callee
  MOZ_ASSERT(slotIndex == callInfo_->argc() + 1);
  return defineOperand(resultId, callInfo_->callee());
}

bool WarpCacheIRTranspiler::emitLoadArgumentFixedSlot(ValOperandId resultId,
                                                      uint8_t slotIndex) {
  return emitLoadArgumentSlot(resultId, slotIndex);
}

bool WarpCacheIRTranspiler::emitLoadArgumentDynamicSlot(ValOperandId resultId,
                                                        Int32OperandId argcId,
                                                        uint8_t slotIndex) {
#ifdef DEBUG
  MDefinition* argc = getOperand(argcId);
  MOZ_ASSERT(argc->toConstant()->toInt32() ==
             static_cast<int32_t>(callInfo_->argc()));
#endif

  return emitLoadArgumentSlot(resultId, callInfo_->argc() + slotIndex);
}

WrappedFunction* WarpCacheIRTranspiler::maybeWrappedFunction(
    MDefinition* callee, CallKind kind, uint16_t nargs, FunctionFlags flags) {
  MOZ_ASSERT(callee->isConstant() || callee->isNurseryObject());

  // If this is a native without a JitEntry, WrappedFunction needs to know the
  // target JSFunction.
  // TODO: support nursery-allocated natives with WrappedFunction, maybe by
  // storing the JSNative in the Baseline stub like flags/nargs.
  bool isNative = flags.isNativeWithoutJitEntry();
  if (isNative && !callee->isConstant()) {
    return nullptr;
  }

  JSFunction* nativeTarget = nullptr;
  if (isNative) {
    nativeTarget = &callee->toConstant()->toObject().as<JSFunction>();
  }

  WrappedFunction* wrappedTarget =
      new (alloc()) WrappedFunction(nativeTarget, nargs, flags);
  MOZ_ASSERT_IF(kind == CallKind::Native || kind == CallKind::DOM,
                wrappedTarget->isNativeWithoutJitEntry());
  MOZ_ASSERT_IF(kind == CallKind::Scripted, wrappedTarget->hasJitEntry());
  return wrappedTarget;
}

WrappedFunction* WarpCacheIRTranspiler::maybeCallTarget(MDefinition* callee,
                                                        CallKind kind) {
  // CacheIR emits the following for specialized calls:
  //     GuardSpecificFunction <callee> <func> ..
  //     Call(Native|Scripted)Function <callee> ..
  // or:
  //     GuardClass <callee> ..
  //     GuardFunctionScript <callee> <script> ..
  //     CallScriptedFunction <callee> ..
  //
  // We can use the <func> JSFunction or <script> BaseScript to specialize this
  // call.
  if (callee->isGuardSpecificFunction()) {
    auto* guard = callee->toGuardSpecificFunction();
    return maybeWrappedFunction(guard->expected(), kind, guard->nargs(),
                                guard->flags());
  }
  if (callee->isGuardFunctionScript()) {
    MOZ_ASSERT(kind == CallKind::Scripted);
    auto* guard = callee->toGuardFunctionScript();
    WrappedFunction* wrappedTarget = new (alloc()) WrappedFunction(
        /* nativeFun = */ nullptr, guard->nargs(), guard->flags());
    MOZ_ASSERT(wrappedTarget->hasJitEntry());
    return wrappedTarget;
  }
  return nullptr;
}

// If it is possible to use MCall for this call, update callInfo_ to use
// the correct arguments. Otherwise, update the ArgFormat of callInfo_.
bool WarpCacheIRTranspiler::updateCallInfo(MDefinition* callee,
                                           CallFlags flags) {
  // The transpilation will add various guards to the callee.
  // We replace the callee referenced by the CallInfo, so that
  // the resulting call instruction depends on these guards.
  callInfo_->setCallee(callee);

  // The transpilation may also add guards to other arguments.
  // We replace those arguments in the CallInfo here.
  updateArgumentsFromOperands();

  switch (flags.getArgFormat()) {
    case CallFlags::Standard:
      MOZ_ASSERT(callInfo_->argFormat() == CallInfo::ArgFormat::Standard);
      break;
    case CallFlags::Spread:
      MOZ_ASSERT(callInfo_->argFormat() == CallInfo::ArgFormat::Array);
      break;
    case CallFlags::FunCall:
      // Note: We already changed the callee to the target
      // function instead of the |call| function.
      MOZ_ASSERT(!callInfo_->constructing());
      MOZ_ASSERT(callInfo_->argFormat() == CallInfo::ArgFormat::Standard);

      if (callInfo_->argc() == 0) {
        // Special case for fun.call() with no arguments.
        auto* undef = constant(UndefinedValue());
        callInfo_->setThis(undef);
      } else {
        // The first argument for |call| is the new this value.
        callInfo_->setThis(callInfo_->getArg(0));

        // Shift down all other arguments by removing the first.
        callInfo_->removeArg(0);
      }
      break;
    case CallFlags::FunApplyArgsObj:
      MOZ_ASSERT(!callInfo_->constructing());
      MOZ_ASSERT(callInfo_->argFormat() == CallInfo::ArgFormat::Standard);

      callInfo_->setArgFormat(CallInfo::ArgFormat::FunApplyArgsObj);
      break;
    case CallFlags::FunApplyArray: {
      MDefinition* argFunc = callInfo_->thisArg();
      MDefinition* argThis = callInfo_->getArg(0);
      callInfo_->setCallee(argFunc);
      callInfo_->setThis(argThis);
      callInfo_->setArgFormat(CallInfo::ArgFormat::Array);
      break;
    }
    case CallFlags::FunApplyNullUndefined:
      // Note: We already changed the callee to the target
      // function instead of the |apply| function.
      MOZ_ASSERT(callInfo_->argc() == 2);
      MOZ_ASSERT(!callInfo_->constructing());
      MOZ_ASSERT(callInfo_->argFormat() == CallInfo::ArgFormat::Standard);
      callInfo_->setThis(callInfo_->getArg(0));
      callInfo_->getArg(1)->setImplicitlyUsedUnchecked();
      callInfo_->removeArg(1);
      callInfo_->removeArg(0);
      break;
    default:
      MOZ_CRASH("Unsupported arg format");
  }
  return true;
}

// Returns true if we are generating a call to CreateThisFromIon and
// must check its return value.
bool WarpCacheIRTranspiler::maybeCreateThis(MDefinition* callee,
                                            CallFlags flags, CallKind kind) {
  MOZ_ASSERT(kind != CallKind::DOM, "DOM functions are not constructors");
  MDefinition* thisArg = callInfo_->thisArg();

  if (kind == CallKind::Native) {
    // Native functions keep the is-constructing MagicValue as |this|.
    // If one of the arguments uses spread syntax this can be a loop phi with
    // MIRType::Value.
    MOZ_ASSERT(thisArg->type() == MIRType::MagicIsConstructing ||
               thisArg->isPhi());
    return false;
  }
  MOZ_ASSERT(kind == CallKind::Scripted);

  if (thisArg->isNewPlainObject()) {
    // We have already updated |this| based on MetaScriptedThisShape. We do
    // not need to generate a check.
    return false;
  }
  if (flags.needsUninitializedThis()) {
    MConstant* uninit = constant(MagicValue(JS_UNINITIALIZED_LEXICAL));
    thisArg->setImplicitlyUsedUnchecked();
    callInfo_->setThis(uninit);
    return false;
  }
  // See the Native case above.
  MOZ_ASSERT(thisArg->type() == MIRType::MagicIsConstructing ||
             thisArg->isPhi());

  auto* newTarget = unboxObjectInfallible(callInfo_->getNewTarget());
  auto* createThis = MCreateThis::New(alloc(), callee, newTarget);
  add(createThis);

  thisArg->setImplicitlyUsedUnchecked();
  callInfo_->setThis(createThis);

  return true;
}

bool WarpCacheIRTranspiler::emitCallFunction(
    ObjOperandId calleeId, Int32OperandId argcId,
    mozilla::Maybe<ObjOperandId> thisObjId, CallFlags flags, CallKind kind,
    mozilla::Maybe<uint32_t> siteOffset) {
  MDefinition* callee = getOperand(calleeId);
  if (kind == CallKind::Scripted && callInfo_ && callInfo_->isInlined()) {
    // We are transpiling to generate the correct guards. We also
    // update the CallInfo to use the correct arguments. Code for the
    // inlined function itself will be generated in
    // WarpBuilder::buildInlinedCall.
    if (!updateCallInfo(callee, flags)) {
      return false;
    }
    if (callInfo_->constructing()) {
      MOZ_ASSERT(flags.isConstructing());

      // We call maybeCreateThis to update |this|, but inlined constructors
      // never need a VM call. CallIRGenerator::getThisForScripted ensures that
      // we don't attach a specialized stub unless we have a template object or
      // know that the constructor needs uninitialized this.
      MOZ_ALWAYS_FALSE(maybeCreateThis(callee, flags, CallKind::Scripted));
      mozilla::DebugOnly<MDefinition*> thisArg = callInfo_->thisArg();
      MOZ_ASSERT(thisArg->isNewPlainObject() ||
                 thisArg->type() == MIRType::MagicUninitializedLexical);
    }

    if (flags.getArgFormat() == CallFlags::FunCall) {
      callInfo_->setInliningResumeMode(ResumeMode::InlinedFunCall);
    } else {
      MOZ_ASSERT(flags.getArgFormat() == CallFlags::Standard);
      callInfo_->setInliningResumeMode(ResumeMode::InlinedStandardCall);
    }

    switch (callInfo_->argFormat()) {
      case CallInfo::ArgFormat::Standard:
        break;
      default:
        MOZ_CRASH("Unsupported arg format");
    }
    return true;
  }

#ifdef DEBUG
  MDefinition* argc = getOperand(argcId);
  MOZ_ASSERT(argc->toConstant()->toInt32() ==
             static_cast<int32_t>(callInfo_->argc()));
#endif

  if (!updateCallInfo(callee, flags)) {
    return false;
  }

  if (kind == CallKind::DOM) {
    MOZ_ASSERT(flags.getArgFormat() == CallFlags::Standard);
    // For DOM calls |this| has a class guard.
    MDefinition* thisObj = getOperand(*thisObjId);
    callInfo_->setThis(thisObj);
  }

  WrappedFunction* wrappedTarget = maybeCallTarget(callee, kind);

  bool needsThisCheck = false;
  if (callInfo_->constructing()) {
    MOZ_ASSERT(flags.isConstructing());
    needsThisCheck = maybeCreateThis(callee, flags, kind);
    if (needsThisCheck) {
      wrappedTarget = nullptr;
    }
  }

  switch (callInfo_->argFormat()) {
    case CallInfo::ArgFormat::Standard: {
      gc::Heap initialHeap = gc::Heap::Default;
      if (siteOffset) {
        MOZ_ASSERT(kind == CallKind::DOM);
        MOZ_ASSERT(readStubWord(*siteOffset) <= (uintptr_t)(gc::Heap::Tenured));
        initialHeap = static_cast<gc::Heap>(readStubWord(*siteOffset));
      }
      MCall* call = makeCall(*callInfo_, needsThisCheck, wrappedTarget,
                             kind == CallKind::DOM, initialHeap);
      if (!call) {
        return false;
      }

      if (flags.isSameRealm()) {
        call->setNotCrossRealm();
      }

      if (call->isEffectful()) {
        addEffectful(call);
        pushResult(call);
        return resumeAfter(call);
      }

      MOZ_ASSERT(kind == CallKind::DOM);
      add(call);
      pushResult(call);
      return true;
    }
    case CallInfo::ArgFormat::Array: {
      MInstruction* call = makeSpreadCall(*callInfo_, needsThisCheck,
                                          flags.isSameRealm(), wrappedTarget);
      if (!call) {
        return false;
      }
      addEffectful(call);
      pushResult(call);

      return resumeAfter(call);
    }
    case CallInfo::ArgFormat::FunApplyArgsObj: {
      return emitFunApplyArgsObj(wrappedTarget, flags);
    }
  }
  MOZ_CRASH("unreachable");
}

bool WarpCacheIRTranspiler::emitFunApplyArgsObj(WrappedFunction* wrappedTarget,
                                                CallFlags flags) {
  MOZ_ASSERT(!callInfo_->constructing());

  MDefinition* callee = callInfo_->thisArg();
  MDefinition* thisArg = callInfo_->getArg(0);
  MDefinition* argsObj = callInfo_->getArg(1);

  MApplyArgsObj* apply =
      MApplyArgsObj::New(alloc(), wrappedTarget, callee, argsObj, thisArg);

  if (flags.isSameRealm()) {
    apply->setNotCrossRealm();
  }
  if (callInfo_->ignoresReturnValue()) {
    apply->setIgnoresReturnValue();
  }

  addEffectful(apply);
  pushResult(apply);

  return resumeAfter(apply);
}

#ifndef JS_SIMULATOR
bool WarpCacheIRTranspiler::emitCallNativeFunction(ObjOperandId calleeId,
                                                   Int32OperandId argcId,
                                                   CallFlags flags,
                                                   uint32_t argcFixed,
                                                   bool ignoresReturnValue) {
  // Instead of ignoresReturnValue we use CallInfo::ignoresReturnValue.
  return emitCallFunction(calleeId, argcId, mozilla::Nothing(), flags,
                          CallKind::Native);
}

bool WarpCacheIRTranspiler::emitCallDOMFunction(ObjOperandId calleeId,
                                                Int32OperandId argcId,
                                                ObjOperandId thisObjId,
                                                CallFlags flags,
                                                uint32_t argcFixed) {
  return emitCallFunction(calleeId, argcId, mozilla::Some(thisObjId), flags,
                          CallKind::DOM);
}

bool WarpCacheIRTranspiler::emitCallDOMFunctionWithAllocSite(
    ObjOperandId calleeId, Int32OperandId argcId, ObjOperandId thisObjId,
    CallFlags flags, uint32_t argcFixed, uint32_t siteOffset) {
  return emitCallFunction(calleeId, argcId, mozilla::Some(thisObjId), flags,
                          CallKind::DOM, mozilla::Some(siteOffset));
}
#else
bool WarpCacheIRTranspiler::emitCallNativeFunction(ObjOperandId calleeId,
                                                   Int32OperandId argcId,
                                                   CallFlags flags,
                                                   uint32_t argcFixed,
                                                   uint32_t targetOffset) {
  return emitCallFunction(calleeId, argcId, mozilla::Nothing(), flags,
                          CallKind::Native);
}

bool WarpCacheIRTranspiler::emitCallDOMFunction(
    ObjOperandId calleeId, Int32OperandId argcId, ObjOperandId thisObjId,
    CallFlags flags, uint32_t argcFixed, uint32_t targetOffset) {
  return emitCallFunction(calleeId, argcId, mozilla::Some(thisObjId), flags,
                          CallKind::DOM);
}

bool WarpCacheIRTranspiler::emitCallDOMFunctionWithAllocSite(
    ObjOperandId calleeId, Int32OperandId argcId, ObjOperandId thisObjId,
    CallFlags flags, uint32_t argcFixed, uint32_t siteOffset,
    uint32_t targetOffset) {
  return emitCallFunction(calleeId, argcId, mozilla::Some(thisObjId), flags,
                          CallKind::DOM, mozilla::Some(siteOffset));
}
#endif

bool WarpCacheIRTranspiler::emitCallScriptedFunction(ObjOperandId calleeId,
                                                     Int32OperandId argcId,
                                                     CallFlags flags,
                                                     uint32_t argcFixed) {
  return emitCallFunction(calleeId, argcId, mozilla::Nothing(), flags,
                          CallKind::Scripted);
}

bool WarpCacheIRTranspiler::emitCallInlinedFunction(ObjOperandId calleeId,
                                                    Int32OperandId argcId,
                                                    uint32_t icScriptOffset,
                                                    CallFlags flags,
                                                    uint32_t argcFixed) {
  return emitCallFunction(calleeId, argcId, mozilla::Nothing(), flags,
                          CallKind::Scripted);
}

#ifdef JS_PUNBOX64
bool WarpCacheIRTranspiler::emitCallScriptedProxyGetShared(
    MDefinition* target, MDefinition* receiver, MDefinition* handler,
    MDefinition* id, MDefinition* trapDef, WrappedFunction* trap) {
  CallInfo callInfo(alloc(), /* constructing = */ false,
                    /* ignoresRval = */ false);
  callInfo.initForProxyGet(trapDef, handler, target, id, receiver);

  MCall* call = makeCall(callInfo, /* needsThisCheck = */ false, trap);
  if (!call) {
    return false;
  }

  addEffectful(call);

  if (!current->ensureHasSlots(3)) {
    return false;
  }
  current->push(call);
  current->push(id);
  current->push(target);

  MResumePoint* resumePoint =
      MResumePoint::New(alloc(), current, loc_.toRawBytecode(),
                        ResumeMode::ResumeAfterCheckProxyGetResult);
  if (!resumePoint) {
    return false;
  }
  call->setResumePoint(resumePoint);

  current->pop();
  current->pop();

  MCheckScriptedProxyGetResult* check =
      MCheckScriptedProxyGetResult::New(alloc(), target, id, call);
  addEffectfulUnsafe(check);

  return resumeAfterUnchecked(check);
}

bool WarpCacheIRTranspiler::emitCallScriptedProxyGetResult(
    ValOperandId targetId, ObjOperandId receiverId, ObjOperandId handlerId,
    ObjOperandId trapId, uint32_t idOffset, uint32_t nargsAndFlags) {
  MDefinition* target = getOperand(targetId);
  MDefinition* receiver = getOperand(receiverId);
  MDefinition* handler = getOperand(handlerId);
  MDefinition* trap = getOperand(trapId);
  jsid id = idStubField(idOffset);
  MDefinition* idDef = constant(StringValue(id.toAtom()));
  WrappedFunction* wrappedTarget = maybeCallTarget(trap, CallKind::Scripted);
  MOZ_RELEASE_ASSERT(wrappedTarget);
  return emitCallScriptedProxyGetShared(target, receiver, handler, idDef, trap,
                                        wrappedTarget);
}

bool WarpCacheIRTranspiler::emitCallScriptedProxyGetByValueResult(
    ValOperandId targetId, ObjOperandId receiverId, ObjOperandId handlerId,
    ValOperandId idId, ObjOperandId trapId, uint32_t nargsAndFlags) {
  MDefinition* target = getOperand(targetId);
  MDefinition* receiver = getOperand(receiverId);
  MDefinition* handler = getOperand(handlerId);
  MDefinition* trap = getOperand(trapId);
  MDefinition* idDef = getOperand(idId);
  WrappedFunction* wrappedTarget = maybeCallTarget(trap, CallKind::Scripted);
  MOZ_RELEASE_ASSERT(wrappedTarget);
  return emitCallScriptedProxyGetShared(target, receiver, handler, idDef, trap,
                                        wrappedTarget);
}
#endif

bool WarpCacheIRTranspiler::emitCallClassHook(ObjOperandId calleeId,
                                              Int32OperandId argcId,
                                              CallFlags flags,
                                              uint32_t argcFixed,
                                              uint32_t targetOffset) {
  MDefinition* callee = getOperand(calleeId);
  JSNative target = jsnativeStubField(targetOffset);

#ifdef DEBUG
  MDefinition* argc = getOperand(argcId);
  MOZ_ASSERT(argc->toConstant()->toInt32() ==
             static_cast<int32_t>(callInfo_->argc()));
#endif

  if (!updateCallInfo(callee, flags)) {
    return false;
  }

  MOZ_ASSERT(callInfo_->argFormat() == CallInfo::ArgFormat::Standard);
  MOZ_ASSERT(flags.getArgFormat() == CallFlags::ArgFormat::Standard);

  // Callees can be from any realm. If this changes, we should update
  // MCallClassHook::maybeCrossRealm.
  MOZ_ASSERT(!flags.isSameRealm());

  auto* call = MCallClassHook::New(alloc(), target, callInfo_->argc(),
                                   callInfo_->constructing());
  if (!call) {
    return false;
  }

  if (callInfo_->ignoresReturnValue()) {
    call->setIgnoresReturnValue();
  }

  call->initCallee(callInfo_->callee());
  call->addArg(0, callInfo_->thisArg());

  for (uint32_t i = 0; i < callInfo_->argc(); i++) {
    call->addArg(i + 1, callInfo_->getArg(i));
  }

  if (callInfo_->constructing()) {
    call->addArg(1 + callInfo_->argc(), callInfo_->getNewTarget());
  }

  addEffectful(call);
  pushResult(call);

  return resumeAfter(call);
}

bool WarpCacheIRTranspiler::emitCallBoundScriptedFunction(
    ObjOperandId calleeId, ObjOperandId targetId, Int32OperandId argcId,
    CallFlags flags, uint32_t numBoundArgs) {
  MDefinition* callee = getOperand(calleeId);
  MDefinition* target = getOperand(targetId);

  MOZ_ASSERT(callInfo_->argFormat() == CallInfo::ArgFormat::Standard);
  MOZ_ASSERT(callInfo_->constructing() == flags.isConstructing());

  callInfo_->setCallee(target);
  updateArgumentsFromOperands();

  WrappedFunction* wrappedTarget = maybeCallTarget(target, CallKind::Scripted);

  bool needsThisCheck = false;
  if (callInfo_->constructing()) {
    callInfo_->setNewTarget(target);
    needsThisCheck = maybeCreateThis(target, flags, CallKind::Scripted);
    if (needsThisCheck) {
      wrappedTarget = nullptr;
    }
  } else {
    auto* thisv = MLoadFixedSlot::New(alloc(), callee,
                                      BoundFunctionObject::boundThisSlot());
    add(thisv);
    callInfo_->thisArg()->setImplicitlyUsedUnchecked();
    callInfo_->setThis(thisv);
  }

  bool usingInlineBoundArgs =
      numBoundArgs <= BoundFunctionObject::MaxInlineBoundArgs;

  MElements* elements = nullptr;
  if (!usingInlineBoundArgs) {
    auto* boundArgs = MLoadFixedSlot::New(
        alloc(), callee, BoundFunctionObject::firstInlineBoundArgSlot());
    add(boundArgs);
    auto* boundArgsObj = unboxObjectInfallible(boundArgs, IsMovable::Yes);
    elements = MElements::New(alloc(), boundArgsObj);
    add(elements);
  }

  auto loadBoundArg = [&](size_t index) {
    MInstruction* arg;
    if (usingInlineBoundArgs) {
      size_t slot = BoundFunctionObject::firstInlineBoundArgSlot() + index;
      arg = MLoadFixedSlot::New(alloc(), callee, slot);
    } else {
      arg = MLoadElement::New(alloc(), elements, constant(Int32Value(index)));
    }
    add(arg);
    return arg;
  };
  if (!callInfo_->prependArgs(numBoundArgs, loadBoundArg)) {
    return false;
  }

  MCall* call = makeCall(*callInfo_, needsThisCheck, wrappedTarget);
  if (!call) {
    return false;
  }

  if (flags.isSameRealm()) {
    call->setNotCrossRealm();
  }

  addEffectful(call);
  pushResult(call);
  return resumeAfter(call);
}

bool WarpCacheIRTranspiler::emitBindFunctionResult(
    ObjOperandId targetId, uint32_t argc, uint32_t templateObjectOffset) {
  MDefinition* target = getOperand(targetId);
  JSObject* templateObj = tenuredObjectStubField(templateObjectOffset);

  MOZ_ASSERT(callInfo_->argc() == argc);

  auto* bound = MBindFunction::New(alloc(), target, argc, templateObj);
  if (!bound) {
    return false;
  }
  addEffectful(bound);

  for (uint32_t i = 0; i < argc; i++) {
    bound->initArg(i, callInfo_->getArg(i));
  }

  pushResult(bound);
  return resumeAfter(bound);
}

bool WarpCacheIRTranspiler::emitSpecializedBindFunctionResult(
    ObjOperandId targetId, uint32_t argc, uint32_t templateObjectOffset) {
  MDefinition* target = getOperand(targetId);
  JSObject* templateObj = tenuredObjectStubField(templateObjectOffset);

  MOZ_ASSERT(callInfo_->argc() == argc);

  auto* bound = MNewBoundFunction::New(alloc(), templateObj);
  add(bound);

  size_t numBoundArgs = argc > 0 ? argc - 1 : 0;
  MOZ_ASSERT(numBoundArgs <= BoundFunctionObject::MaxInlineBoundArgs);

  auto initSlot = [&](size_t slot, MDefinition* value) {
#ifdef DEBUG
    // Assert we can elide the post write barrier. See also the comment in
    // WarpBuilder::buildNamedLambdaEnv.
    add(MAssertCanElidePostWriteBarrier::New(alloc(), bound, value));
#endif
    addUnchecked(MStoreFixedSlot::NewUnbarriered(alloc(), bound, slot, value));
  };

  initSlot(BoundFunctionObject::targetSlot(), target);
  if (argc > 0) {
    initSlot(BoundFunctionObject::boundThisSlot(), callInfo_->getArg(0));
  }
  for (size_t i = 0; i < numBoundArgs; i++) {
    size_t slot = BoundFunctionObject::firstInlineBoundArgSlot() + i;
    initSlot(slot, callInfo_->getArg(1 + i));
  }

  pushResult(bound);
  return true;
}

bool WarpCacheIRTranspiler::emitCallWasmFunction(
    ObjOperandId calleeId, Int32OperandId argcId, CallFlags flags,
    uint32_t argcFixed, uint32_t funcExportOffset, uint32_t instanceOffset) {
  MDefinition* callee = getOperand(calleeId);
#ifdef DEBUG
  MDefinition* argc = getOperand(argcId);
  MOZ_ASSERT(argc->toConstant()->toInt32() ==
             static_cast<int32_t>(callInfo_->argc()));
#endif
  JSObject* instanceObject = tenuredObjectStubField(instanceOffset);
  auto* wasmInstanceObj = &instanceObject->as<WasmInstanceObject>();
  const wasm::FuncExport* funcExport = wasmFuncExportField(funcExportOffset);
  const wasm::FuncType& sig =
      wasmInstanceObj->instance().code().codeMeta().getFuncType(
          funcExport->funcIndex());

  if (!updateCallInfo(callee, flags)) {
    return false;
  }

  static_assert(wasm::MaxArgsForJitInlineCall <= MaxNumLInstructionOperands,
                "arguments must fit in LIR operands");
  MOZ_ASSERT(sig.args().length() <= wasm::MaxArgsForJitInlineCall);

  MOZ_ASSERT(callInfo_->argFormat() == CallInfo::ArgFormat::Standard);

  auto* call = MIonToWasmCall::New(alloc(), wasmInstanceObj, *funcExport);
  if (!call) {
    return false;
  }

  mozilla::Maybe<MDefinition*> undefined;
  for (size_t i = 0; i < sig.args().length(); i++) {
    if (!alloc().ensureBallast()) {
      return false;
    }

    MDefinition* arg;
    if (i < callInfo_->argc()) {
      arg = callInfo_->getArg(i);
    } else {
      if (!undefined) {
        undefined.emplace(constant(UndefinedValue()));
      }
      arg = convertWasmArg(*undefined, sig.args()[i].kind());
    }
    call->initArg(i, arg);
  }

  addEffectful(call);

  // Add any post-function call conversions that are necessary.
  MInstruction* postConversion = call;
  const wasm::ValTypeVector& results = sig.results();
  MOZ_ASSERT(results.length() <= 1, "Multi-value returns not supported.");
  if (results.length() == 0) {
    // No results to convert.
  } else {
    switch (results[0].kind()) {
      case wasm::ValType::I64:
        // JS expects a BigInt from I64 types.
        postConversion =
            MInt64ToBigInt::New(alloc(), call, /* isSigned = */ true);

        // Make non-movable so we can attach a resume point.
        postConversion->setNotMovable();

        add(postConversion);
        break;
      default:
        // No spectre.index_masking of i32 results required, as the generated
        // stub takes care of that.
        break;
    }
  }

  // The resume point has to be attached to the post-conversion instruction
  // (if present) instead of to the call. This way, if the call triggers an
  // invalidation bailout, we will have the BigInt value on the Baseline stack.
  // Potential alternative solution: attach the resume point to the call and
  // have bailouts turn the Int64 value into a BigInt, maybe with a recover
  // instruction.
  pushResult(postConversion);
  return resumeAfterUnchecked(postConversion);
}

MDefinition* WarpCacheIRTranspiler::convertWasmArg(MDefinition* arg,
                                                   wasm::ValType::Kind kind) {
  // An invariant in this code is that any type conversion operation that has
  // externally visible effects, such as invoking valueOf on an object argument,
  // must bailout so that we don't have to worry about replaying effects during
  // argument conversion.
  MInstruction* conversion = nullptr;
  switch (kind) {
    case wasm::ValType::I32:
      conversion = MTruncateToInt32::New(alloc(), arg);
      break;
    case wasm::ValType::I64:
      conversion = MToInt64::New(alloc(), arg);
      break;
    case wasm::ValType::F32:
      conversion = MToFloat32::New(alloc(), arg);
      break;
    case wasm::ValType::F64:
      conversion = MToDouble::New(alloc(), arg);
      break;
    case wasm::ValType::V128:
      MOZ_CRASH("Unexpected type for Wasm JitEntry");
    case wasm::ValType::Ref:
      // Transform the JS representation into an AnyRef representation.
      // The resulting type is MIRType::WasmAnyRef.  These cases are all
      // effect-free.
      switch (arg->type()) {
        case MIRType::Object:
          conversion = MWasmAnyRefFromJSObject::New(alloc(), arg);
          break;
        case MIRType::String:
          conversion = MWasmAnyRefFromJSString::New(alloc(), arg);
          break;
        case MIRType::Null:
          arg->setImplicitlyUsedUnchecked();
          conversion = MWasmNullConstant::New(alloc(), wasm::MaybeRefType());
          break;
        default:
          conversion = MWasmAnyRefFromJSValue::New(alloc(), arg);
          break;
      }
      break;
  }

  add(conversion);
  return conversion;
}

bool WarpCacheIRTranspiler::emitGuardWasmArg(ValOperandId argId,
                                             wasm::ValType::Kind kind) {
  MDefinition* arg = getOperand(argId);
  MDefinition* conversion = convertWasmArg(arg, kind);

  setOperand(argId, conversion);
  return true;
}

bool WarpCacheIRTranspiler::emitCallGetterResult(CallKind kind,
                                                 ValOperandId receiverId,
                                                 uint32_t getterOffset,
                                                 bool sameRealm,
                                                 uint32_t nargsAndFlagsOffset) {
  MDefinition* receiver = getOperand(receiverId);
  MDefinition* getter = objectStubField(getterOffset);
  if (kind == CallKind::Scripted && callInfo_ && callInfo_->isInlined()) {
    // We are transpiling to generate the correct guards. We also update the
    // CallInfo to use the correct arguments. Code for the inlined getter
    // itself will be generated in WarpBuilder::buildInlinedCall.
    callInfo_->initForGetterCall(getter, receiver);
    callInfo_->setInliningResumeMode(ResumeMode::InlinedAccessor);

    // Make sure there's enough room to push the arguments on the stack.
    if (!current->ensureHasSlots(2)) {
      return false;
    }

    return true;
  }

  uint32_t nargsAndFlags = uint32StubField(nargsAndFlagsOffset);
  uint16_t nargs = nargsAndFlags >> 16;
  FunctionFlags flags = FunctionFlags(uint16_t(nargsAndFlags));
  WrappedFunction* wrappedTarget =
      maybeWrappedFunction(getter, kind, nargs, flags);

  bool ignoresRval = loc_.resultIsPopped();
  CallInfo callInfo(alloc(), /* constructing = */ false, ignoresRval);
  callInfo.initForGetterCall(getter, receiver);

  MCall* call = makeCall(callInfo, /* needsThisCheck = */ false, wrappedTarget);
  if (!call) {
    return false;
  }

  if (sameRealm) {
    call->setNotCrossRealm();
  }

  addEffectful(call);
  pushResult(call);

  return resumeAfter(call);
}

bool WarpCacheIRTranspiler::emitCallScriptedGetterResult(
    ValOperandId receiverId, uint32_t getterOffset, bool sameRealm,
    uint32_t nargsAndFlagsOffset) {
  return emitCallGetterResult(CallKind::Scripted, receiverId, getterOffset,
                              sameRealm, nargsAndFlagsOffset);
}

bool WarpCacheIRTranspiler::emitCallInlinedGetterResult(
    ValOperandId receiverId, uint32_t getterOffset, uint32_t icScriptOffset,
    bool sameRealm, uint32_t nargsAndFlagsOffset) {
  return emitCallGetterResult(CallKind::Scripted, receiverId, getterOffset,
                              sameRealm, nargsAndFlagsOffset);
}

bool WarpCacheIRTranspiler::emitCallNativeGetterResult(
    ValOperandId receiverId, uint32_t getterOffset, bool sameRealm,
    uint32_t nargsAndFlagsOffset) {
  return emitCallGetterResult(CallKind::Native, receiverId, getterOffset,
                              sameRealm, nargsAndFlagsOffset);
}

bool WarpCacheIRTranspiler::emitCallSetter(CallKind kind,
                                           ObjOperandId receiverId,
                                           uint32_t setterOffset,
                                           ValOperandId rhsId, bool sameRealm,
                                           uint32_t nargsAndFlagsOffset) {
  MDefinition* receiver = getOperand(receiverId);
  MDefinition* setter = objectStubField(setterOffset);
  MDefinition* rhs = getOperand(rhsId);
  if (kind == CallKind::Scripted && callInfo_ && callInfo_->isInlined()) {
    // We are transpiling to generate the correct guards. We also update the
    // CallInfo to use the correct arguments. Code for the inlined setter
    // itself will be generated in WarpBuilder::buildInlinedCall.
    callInfo_->initForSetterCall(setter, receiver, rhs);
    callInfo_->setInliningResumeMode(ResumeMode::InlinedAccessor);

    // Make sure there's enough room to push the arguments on the stack.
    if (!current->ensureHasSlots(3)) {
      return false;
    }

    return true;
  }

  uint32_t nargsAndFlags = uint32StubField(nargsAndFlagsOffset);
  uint16_t nargs = nargsAndFlags >> 16;
  FunctionFlags flags = FunctionFlags(uint16_t(nargsAndFlags));
  WrappedFunction* wrappedTarget =
      maybeWrappedFunction(setter, kind, nargs, flags);

  CallInfo callInfo(alloc(), /* constructing = */ false,
                    /* ignoresReturnValue = */ true);
  callInfo.initForSetterCall(setter, receiver, rhs);

  MCall* call = makeCall(callInfo, /* needsThisCheck = */ false, wrappedTarget);
  if (!call) {
    return false;
  }

  if (sameRealm) {
    call->setNotCrossRealm();
  }

  addEffectful(call);
  return resumeAfter(call);
}

bool WarpCacheIRTranspiler::emitCallScriptedSetter(
    ObjOperandId receiverId, uint32_t setterOffset, ValOperandId rhsId,
    bool sameRealm, uint32_t nargsAndFlagsOffset) {
  return emitCallSetter(CallKind::Scripted, receiverId, setterOffset, rhsId,
                        sameRealm, nargsAndFlagsOffset);
}

bool WarpCacheIRTranspiler::emitCallInlinedSetter(
    ObjOperandId receiverId, uint32_t setterOffset, ValOperandId rhsId,
    uint32_t icScriptOffset, bool sameRealm, uint32_t nargsAndFlagsOffset) {
  return emitCallSetter(CallKind::Scripted, receiverId, setterOffset, rhsId,
                        sameRealm, nargsAndFlagsOffset);
}

bool WarpCacheIRTranspiler::emitCallNativeSetter(ObjOperandId receiverId,
                                                 uint32_t setterOffset,
                                                 ValOperandId rhsId,
                                                 bool sameRealm,
                                                 uint32_t nargsAndFlagsOffset) {
  return emitCallSetter(CallKind::Native, receiverId, setterOffset, rhsId,
                        sameRealm, nargsAndFlagsOffset);
}

bool WarpCacheIRTranspiler::emitMetaScriptedThisShape(
    uint32_t thisShapeOffset) {
  SharedShape* shape = &shapeStubField(thisShapeOffset)->asShared();
  MOZ_ASSERT(shape->getObjectClass() == &PlainObject::class_);

  MConstant* shapeConst = MConstant::NewShape(alloc(), shape);
  add(shapeConst);

  // TODO: support pre-tenuring.
  gc::Heap heap = gc::Heap::Default;

  uint32_t numFixedSlots = shape->numFixedSlots();
  uint32_t numDynamicSlots = NativeObject::calculateDynamicSlots(shape);
  gc::AllocKind kind = gc::GetGCObjectKind(numFixedSlots);
  MOZ_ASSERT(gc::GetObjectFinalizeKind(&PlainObject::class_) ==
             gc::FinalizeKind::None);
  MOZ_ASSERT(!IsFinalizedKind(kind));

  auto* createThis = MNewPlainObject::New(alloc(), shapeConst, numFixedSlots,
                                          numDynamicSlots, kind, heap);
  add(createThis);

  callInfo_->thisArg()->setImplicitlyUsedUnchecked();
  callInfo_->setThis(createThis);
  return true;
}

bool WarpCacheIRTranspiler::emitReturnFromIC() { return true; }

bool WarpCacheIRTranspiler::emitBailout() {
  auto* bail = MBail::New(alloc());
  add(bail);

  return true;
}

bool WarpCacheIRTranspiler::emitAssertPropertyLookup(ObjOperandId objId,
                                                     uint32_t idOffset,
                                                     uint32_t slotOffset) {
  // We currently only emit checks in baseline.
  return true;
}

bool WarpCacheIRTranspiler::emitAssertFloat32Result(ValOperandId valId,
                                                    bool mustBeFloat32) {
  MDefinition* val = getOperand(valId);

  auto* assert = MAssertFloat32::New(alloc(), val, mustBeFloat32);
  addEffectful(assert);

  pushResult(constant(UndefinedValue()));
  return resumeAfter(assert);
}

bool WarpCacheIRTranspiler::emitAssertRecoveredOnBailoutResult(
    ValOperandId valId, bool mustBeRecovered) {
  MDefinition* val = getOperand(valId);

  // Don't assert for recovered instructions when recovering is disabled.
  if (JitOptions.disableRecoverIns) {
    pushResult(constant(UndefinedValue()));
    return true;
  }

  if (JitOptions.checkRangeAnalysis) {
    // If we are checking the range of all instructions, then the guards
    // inserted by Range Analysis prevent the use of recover instruction. Thus,
    // we just disable these checks.
    pushResult(constant(UndefinedValue()));
    return true;
  }

  auto* assert = MAssertRecoveredOnBailout::New(alloc(), val, mustBeRecovered);
  addEffectfulUnsafe(assert);
  current->push(assert);

  // Create an instruction sequence which implies that the argument of the
  // assertRecoveredOnBailout function would be encoded at least in one
  // Snapshot.
  auto* nop = MNop::New(alloc());
  add(nop);

  auto* resumePoint = MResumePoint::New(
      alloc(), nop->block(), loc_.toRawBytecode(), ResumeMode::ResumeAfter);
  if (!resumePoint) {
    return false;
  }
  nop->setResumePoint(resumePoint);

  auto* encode = MEncodeSnapshot::New(alloc());
  addEffectfulUnsafe(encode);

  current->pop();

  pushResult(constant(UndefinedValue()));
  return true;
}

bool WarpCacheIRTranspiler::emitGuardNoAllocationMetadataBuilder(
    uint32_t builderAddrOffset) {
  // This is a no-op because we discard all JIT code when set an allocation
  // metadata callback.
  return true;
}

bool WarpCacheIRTranspiler::emitNewPlainObjectResult(uint32_t numFixedSlots,
                                                     uint32_t numDynamicSlots,
                                                     gc::AllocKind allocKind,
                                                     uint32_t shapeOffset,
                                                     uint32_t siteOffset) {
  Shape* shape = shapeStubField(shapeOffset);
  gc::Heap heap = allocSiteInitialHeapField(siteOffset);

  auto* shapeConstant = MConstant::NewShape(alloc(), shape);
  add(shapeConstant);

  auto* obj = MNewPlainObject::New(alloc(), shapeConstant, numFixedSlots,
                                   numDynamicSlots, allocKind, heap);
  add(obj);

  pushResult(obj);
  return true;
}

bool WarpCacheIRTranspiler::emitNewArrayObjectResult(uint32_t length,
                                                     uint32_t shapeOffset,
                                                     uint32_t siteOffset) {
  Shape* shape = shapeStubField(shapeOffset);
  gc::Heap heap = allocSiteInitialHeapField(siteOffset);

  auto* shapeConstant = MConstant::NewShape(alloc(), shape);
  add(shapeConstant);

  auto* obj = MNewArrayObject::New(alloc(), shapeConstant, length, heap);
  add(obj);

  pushResult(obj);
  return true;
}

bool WarpCacheIRTranspiler::emitNewFunctionCloneResult(uint32_t canonicalOffset,
                                                       gc::AllocKind allocKind,
                                                       uint32_t siteOffset) {
  JSObject* fun = tenuredObjectStubField(canonicalOffset);
  MOZ_ASSERT(fun->is<JSFunction>());

  gc::Heap heap = allocSiteInitialHeapField(siteOffset);

  MDefinition* env = currentBlock()->environmentChain();

  // The environment chain must be an object, but the MIR type can be Value when
  // phis are involved.
  if (env->type() != MIRType::Object) {
    MOZ_ASSERT(env->type() == MIRType::Value);
    auto* unbox =
        MUnbox::New(alloc(), env, MIRType::Object, MUnbox::Infallible);
    current->add(unbox);
    env = unbox;
  }

  MConstant* funConst = constant(ObjectValue(*fun));

  auto* ins = MLambda::New(alloc(), env, funConst, heap);
  addEffectful(ins);
  pushResult(ins);
  return resumeAfter(ins);
}

bool WarpCacheIRTranspiler::emitCloseIterScriptedResult(ObjOperandId iterId,
                                                        ObjOperandId calleeId,
                                                        CompletionKind kind,
                                                        uint32_t calleeNargs) {
  MDefinition* iter = getOperand(iterId);
  MDefinition* callee = getOperand(calleeId);

  WrappedFunction* wrappedTarget = maybeCallTarget(callee, CallKind::Scripted);
  MOZ_ASSERT(wrappedTarget);
  MOZ_ASSERT(wrappedTarget->nargs() == calleeNargs);
  MOZ_ASSERT(wrappedTarget->hasJitEntry());

  bool constructing = false;
  bool ignoresRval = false;
  bool needsThisCheck = false;
  CallInfo callInfo(alloc(), constructing, ignoresRval);
  callInfo.initForCloseIter(iter, callee);
  MCall* call = makeCall(callInfo, needsThisCheck, wrappedTarget);
  if (!call) {
    return false;
  }
  addEffectful(call);
  if (kind == CompletionKind::Throw) {
    return resumeAfter(call);
  }

  // If we bail out here, after the call but before the CheckIsObj, we
  // can't simply resume in the baseline interpreter. If we resume
  // after the CloseIter, we won't check the return value. If we
  // resume at the CloseIter, we will call the |return| method twice.
  // Instead, we use a special resume mode that captures the
  // intermediate value, and then checks that it's an object while
  // bailing out.
  current->push(call);
  MResumePoint* resumePoint =
      MResumePoint::New(alloc(), current, loc_.toRawBytecode(),
                        ResumeMode::ResumeAfterCheckIsObject);
  if (!resumePoint) {
    return false;
  }
  call->setResumePoint(resumePoint);
  current->pop();

  MCheckIsObj* check = MCheckIsObj::New(
      alloc(), call, uint8_t(CheckIsObjectKind::IteratorReturn));
  addEffectfulUnsafe(check);

  return resumeAfterUnchecked(check);
}

bool WarpCacheIRTranspiler::emitGuardGlobalGeneration(
    uint32_t expectedOffset, uint32_t generationAddrOffset) {
  uint32_t expected = uint32StubField(expectedOffset);
  const void* generationAddr = rawPointerField(generationAddrOffset);

  auto guard = MGuardGlobalGeneration::New(alloc(), expected, generationAddr);
  add(guard);

  return true;
}

#ifdef FUZZING_JS_FUZZILLI
bool WarpCacheIRTranspiler::emitFuzzilliHashResult(ValOperandId valId) {
  MDefinition* input = getOperand(valId);

  auto* hash = MFuzzilliHash::New(alloc(), input);
  add(hash);

  auto* store = MFuzzilliHashStore::New(alloc(), hash);
  addEffectful(store);
  pushResult(constant(UndefinedValue()));

  return resumeAfter(store);
}
#endif

static void MaybeSetImplicitlyUsed(uint32_t numInstructionIdsBefore,
                                   MDefinition* input) {
  // When building MIR from bytecode, for each MDefinition that's an operand to
  // a bytecode instruction, we must either add an SSA use or set the
  // ImplicitlyUsed flag on that definition. The ImplicitlyUsed flag prevents
  // the backend from optimizing-out values that will be used by Baseline after
  // a bailout.
  //
  // WarpBuilder uses WarpPoppedValueUseChecker to assert this invariant in
  // debug builds.
  //
  // This function is responsible for setting the ImplicitlyUsed flag for an
  // input when using the transpiler. It looks at the input's most recent use
  // and if that's an instruction that was added while transpiling this JSOp
  // (based on the MIR instruction id) we don't set the ImplicitlyUsed flag.

  if (input->isImplicitlyUsed()) {
    // Nothing to do.
    return;
  }

  // If the most recent use of 'input' is an instruction we just added, there is
  // nothing to do.
  MDefinition* inputUse = input->maybeMostRecentlyAddedDefUse();
  if (inputUse && inputUse->id() >= numInstructionIdsBefore) {
    return;
  }

  // The transpiler didn't add a use for 'input'.
  input->setImplicitlyUsed();
}

bool jit::TranspileCacheIRToMIR(WarpBuilder* builder, BytecodeLocation loc,
                                const WarpCacheIR* cacheIRSnapshot,
                                std::initializer_list<MDefinition*> inputs,
                                CallInfo* maybeCallInfo) {
  uint32_t numInstructionIdsBefore =
      builder->mirGen().graph().getNumInstructionIds();

  WarpCacheIRTranspiler transpiler(builder, loc, maybeCallInfo,
                                   cacheIRSnapshot);
  if (!transpiler.transpile(inputs)) {
    return false;
  }

  for (MDefinition* input : inputs) {
    MaybeSetImplicitlyUsed(numInstructionIdsBefore, input);
  }

  if (maybeCallInfo) {
    auto maybeSetFlag = [numInstructionIdsBefore](MDefinition* def) {
      MaybeSetImplicitlyUsed(numInstructionIdsBefore, def);
    };
    maybeCallInfo->forEachCallOperand(maybeSetFlag);
  }

  return true;
}
