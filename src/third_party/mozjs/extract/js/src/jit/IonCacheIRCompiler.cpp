/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/IonCacheIRCompiler.h"
#include "mozilla/Maybe.h"

#include <algorithm>

#include "jit/CacheIRCompiler.h"
#include "jit/CacheIRWriter.h"
#include "jit/IonIC.h"
#include "jit/JitcodeMap.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/JitZone.h"
#include "jit/JSJitFrameIter.h"
#include "jit/Linker.h"
#include "jit/SharedICHelpers.h"
#include "jit/VMFunctions.h"
#include "proxy/DeadObjectProxy.h"
#include "proxy/Proxy.h"
#include "util/Memory.h"
#include "vm/StaticStrings.h"

#include "jit/JSJitFrameIter-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "jit/VMFunctionList-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::Maybe;

namespace JS {
struct ExpandoAndGeneration;
}

using JS::ExpandoAndGeneration;

namespace js {
namespace jit {

// IonCacheIRCompiler compiles CacheIR to IonIC native code.
IonCacheIRCompiler::IonCacheIRCompiler(JSContext* cx, TempAllocator& alloc,
                                       const CacheIRWriter& writer, IonIC* ic,
                                       IonScript* ionScript,
                                       uint32_t stubDataOffset)
    : CacheIRCompiler(cx, alloc, writer, stubDataOffset, Mode::Ion,
                      StubFieldPolicy::Constant),
      writer_(writer),
      ic_(ic),
      ionScript_(ionScript),
      savedLiveRegs_(false),
      localTracingSlots_(0),
      perfSpewer_(ic->pc()) {
  MOZ_ASSERT(ic_);
  MOZ_ASSERT(ionScript_);
}

template <typename T>
T IonCacheIRCompiler::rawPointerStubField(uint32_t offset) {
  static_assert(sizeof(T) == sizeof(uintptr_t), "T must have pointer size");
  return (T)readStubWord(offset, StubField::Type::RawPointer);
}

template <typename T>
T IonCacheIRCompiler::rawInt64StubField(uint32_t offset) {
  static_assert(sizeof(T) == sizeof(int64_t), "T musthave int64 size");
  return (T)readStubInt64(offset, StubField::Type::RawInt64);
}

template <typename Fn, Fn fn>
void IonCacheIRCompiler::callVM(MacroAssembler& masm) {
  VMFunctionId id = VMFunctionToId<Fn, fn>::id;
  callVMInternal(masm, id);
}

void IonCacheIRCompiler::pushStubCodePointer() {
  stubJitCodeOffset_.emplace(masm.PushWithPatch(ImmPtr((void*)-1)));
}

// AutoSaveLiveRegisters must be used when we make a call that can GC. The
// constructor ensures all live registers are stored on the stack (where the GC
// expects them) and the destructor restores these registers.
AutoSaveLiveRegisters::AutoSaveLiveRegisters(IonCacheIRCompiler& compiler)
    : compiler_(compiler) {
  MOZ_ASSERT(compiler_.liveRegs_.isSome());
  MOZ_ASSERT(compiler_.ic_);
  compiler_.allocator.saveIonLiveRegisters(
      compiler_.masm, compiler_.liveRegs_.ref(),
      compiler_.ic_->scratchRegisterForEntryJump(), compiler_.ionScript_);
  compiler_.savedLiveRegs_ = true;
}
AutoSaveLiveRegisters::~AutoSaveLiveRegisters() {
  MOZ_ASSERT(compiler_.stubJitCodeOffset_.isSome(),
             "Must have pushed JitCode* pointer");
  compiler_.allocator.restoreIonLiveRegisters(compiler_.masm,
                                              compiler_.liveRegs_.ref());
  MOZ_ASSERT_IF(!compiler_.masm.oom(), compiler_.masm.framePushed() ==
                                           compiler_.ionScript_->frameSize());
}

}  // namespace jit
}  // namespace js

void CacheRegisterAllocator::saveIonLiveRegisters(MacroAssembler& masm,
                                                  LiveRegisterSet liveRegs,
                                                  Register scratch,
                                                  IonScript* ionScript) {
  // We have to push all registers in liveRegs on the stack. It's possible we
  // stored other values in our live registers and stored operands on the
  // stack (where our live registers should go), so this requires some careful
  // work. Try to keep it simple by taking one small step at a time.

  // Step 1. Discard any dead operands so we can reuse their registers.
  freeDeadOperandLocations(masm);

  // Step 2. Figure out the size of our live regs.  This is consistent with
  // the fact that we're using storeRegsInMask to generate the save code and
  // PopRegsInMask to generate the restore code.
  size_t sizeOfLiveRegsInBytes =
      MacroAssembler::PushRegsInMaskSizeInBytes(liveRegs);

  MOZ_ASSERT(sizeOfLiveRegsInBytes > 0);

  // Step 3. Ensure all non-input operands are on the stack.
  size_t numInputs = writer_.numInputOperands();
  for (size_t i = numInputs; i < operandLocations_.length(); i++) {
    OperandLocation& loc = operandLocations_[i];
    if (loc.isInRegister()) {
      spillOperandToStack(masm, &loc);
    }
  }

  // Step 4. Restore the register state, but don't discard the stack as
  // non-input operands are stored there.
  restoreInputState(masm, /* shouldDiscardStack = */ false);

  // We just restored the input state, so no input operands should be stored
  // on the stack.
#ifdef DEBUG
  for (size_t i = 0; i < numInputs; i++) {
    const OperandLocation& loc = operandLocations_[i];
    MOZ_ASSERT(!loc.isOnStack());
  }
#endif

  // Step 5. At this point our register state is correct. Stack values,
  // however, may cover the space where we have to store the live registers.
  // Move them out of the way.

  bool hasOperandOnStack = false;
  for (size_t i = numInputs; i < operandLocations_.length(); i++) {
    OperandLocation& loc = operandLocations_[i];
    if (!loc.isOnStack()) {
      continue;
    }

    hasOperandOnStack = true;

    size_t operandSize = loc.stackSizeInBytes();
    size_t operandStackPushed = loc.stackPushed();
    MOZ_ASSERT(operandSize > 0);
    MOZ_ASSERT(stackPushed_ >= operandStackPushed);
    MOZ_ASSERT(operandStackPushed >= operandSize);

    // If this operand doesn't cover the live register space, there's
    // nothing to do.
    if (operandStackPushed - operandSize >= sizeOfLiveRegsInBytes) {
      MOZ_ASSERT(stackPushed_ > sizeOfLiveRegsInBytes);
      continue;
    }

    // Reserve stack space for the live registers if needed.
    if (sizeOfLiveRegsInBytes > stackPushed_) {
      size_t extraBytes = sizeOfLiveRegsInBytes - stackPushed_;
      MOZ_ASSERT((extraBytes % sizeof(uintptr_t)) == 0);
      masm.subFromStackPtr(Imm32(extraBytes));
      stackPushed_ += extraBytes;
    }

    // Push the operand below the live register space.
    if (loc.kind() == OperandLocation::PayloadStack) {
      masm.push(
          Address(masm.getStackPointer(), stackPushed_ - operandStackPushed));
      stackPushed_ += operandSize;
      loc.setPayloadStack(stackPushed_, loc.payloadType());
      continue;
    }
    MOZ_ASSERT(loc.kind() == OperandLocation::ValueStack);
    masm.pushValue(
        Address(masm.getStackPointer(), stackPushed_ - operandStackPushed));
    stackPushed_ += operandSize;
    loc.setValueStack(stackPushed_);
  }

  // Step 6. If we have any operands on the stack, adjust their stackPushed
  // values to not include sizeOfLiveRegsInBytes (this simplifies code down
  // the line). Then push/store the live registers.
  if (hasOperandOnStack) {
    MOZ_ASSERT(stackPushed_ > sizeOfLiveRegsInBytes);
    stackPushed_ -= sizeOfLiveRegsInBytes;

    for (size_t i = numInputs; i < operandLocations_.length(); i++) {
      OperandLocation& loc = operandLocations_[i];
      if (loc.isOnStack()) {
        loc.adjustStackPushed(-int32_t(sizeOfLiveRegsInBytes));
      }
    }

    size_t stackBottom = stackPushed_ + sizeOfLiveRegsInBytes;
    masm.storeRegsInMask(liveRegs, Address(masm.getStackPointer(), stackBottom),
                         scratch);
    masm.setFramePushed(masm.framePushed() + sizeOfLiveRegsInBytes);
  } else {
    // If no operands are on the stack, discard the unused stack space.
    if (stackPushed_ > 0) {
      masm.addToStackPtr(Imm32(stackPushed_));
      stackPushed_ = 0;
    }
    masm.PushRegsInMask(liveRegs);
  }
  freePayloadSlots_.clear();
  freeValueSlots_.clear();

  MOZ_ASSERT_IF(!masm.oom(), masm.framePushed() == ionScript->frameSize() +
                                                       sizeOfLiveRegsInBytes);

  // Step 7. All live registers and non-input operands are stored on the stack
  // now, so at this point all registers except for the input registers are
  // available.
  availableRegs_.set() = GeneralRegisterSet::Not(inputRegisterSet());
  availableRegsAfterSpill_.set() = GeneralRegisterSet();

  // Step 8. We restored our input state, so we have to fix up aliased input
  // registers again.
  fixupAliasedInputs(masm);
}

void CacheRegisterAllocator::restoreIonLiveRegisters(MacroAssembler& masm,
                                                     LiveRegisterSet liveRegs) {
  masm.PopRegsInMask(liveRegs);

  availableRegs_.set() = GeneralRegisterSet();
  availableRegsAfterSpill_.set() = GeneralRegisterSet::All();
}

static void* GetReturnAddressToIonCode(JSContext* cx) {
  JSJitFrameIter frame(cx->activation()->asJit());
  MOZ_ASSERT(frame.type() == FrameType::Exit,
             "An exit frame is expected as update functions are called with a "
             "VMFunction.");

  void* returnAddr = frame.returnAddress();
#ifdef DEBUG
  ++frame;
  MOZ_ASSERT(frame.isIonJS());
#endif
  return returnAddr;
}

// The AutoSaveLiveRegisters parameter is used to ensure registers were saved
void IonCacheIRCompiler::enterStubFrame(MacroAssembler& masm,
                                        const AutoSaveLiveRegisters&) {
  MOZ_ASSERT(!enteredStubFrame_);
  pushStubCodePointer();
  masm.PushFrameDescriptor(FrameType::IonJS);
  masm.Push(ImmPtr(GetReturnAddressToIonCode(cx_)));

  masm.Push(FramePointer);
  masm.moveStackPtrTo(FramePointer);

  enteredStubFrame_ = true;
}

void IonCacheIRCompiler::storeTracedValue(MacroAssembler& masm,
                                          ValueOperand value) {
  MOZ_ASSERT(localTracingSlots_ < 255);
  masm.Push(value);
  localTracingSlots_++;
}

void IonCacheIRCompiler::loadTracedValue(MacroAssembler& masm,
                                         uint8_t slotIndex,
                                         ValueOperand value) {
  MOZ_ASSERT(slotIndex <= localTracingSlots_);
  int32_t offset = IonICCallFrameLayout::LocallyTracedValueOffset +
                   slotIndex * sizeof(Value);
  masm.loadValue(Address(FramePointer, -offset), value);
}

bool IonCacheIRCompiler::init() {
  if (!allocator.init()) {
    return false;
  }

  size_t numInputs = writer_.numInputOperands();
  MOZ_ASSERT(numInputs == NumInputsForCacheKind(ic_->kind()));

  AllocatableGeneralRegisterSet available;

  switch (ic_->kind()) {
    case CacheKind::GetProp:
    case CacheKind::GetElem: {
      IonGetPropertyIC* ic = ic_->asGetPropertyIC();
      ValueOperand output = ic->output();

      available.add(output);

      liveRegs_.emplace(ic->liveRegs());
      outputUnchecked_.emplace(output);

      MOZ_ASSERT(numInputs == 1 || numInputs == 2);

      allocator.initInputLocation(0, ic->value());
      if (numInputs > 1) {
        allocator.initInputLocation(1, ic->id());
      }
      break;
    }
    case CacheKind::GetPropSuper:
    case CacheKind::GetElemSuper: {
      IonGetPropSuperIC* ic = ic_->asGetPropSuperIC();
      ValueOperand output = ic->output();

      available.add(output);

      liveRegs_.emplace(ic->liveRegs());
      outputUnchecked_.emplace(output);

      MOZ_ASSERT(numInputs == 2 || numInputs == 3);

      allocator.initInputLocation(0, ic->object(), JSVAL_TYPE_OBJECT);

      if (ic->kind() == CacheKind::GetPropSuper) {
        MOZ_ASSERT(numInputs == 2);
        allocator.initInputLocation(1, ic->receiver());
      } else {
        MOZ_ASSERT(numInputs == 3);
        allocator.initInputLocation(1, ic->id());
        allocator.initInputLocation(2, ic->receiver());
      }
      break;
    }
    case CacheKind::SetProp:
    case CacheKind::SetElem: {
      IonSetPropertyIC* ic = ic_->asSetPropertyIC();

      available.add(ic->temp());

      liveRegs_.emplace(ic->liveRegs());

      allocator.initInputLocation(0, ic->object(), JSVAL_TYPE_OBJECT);

      if (ic->kind() == CacheKind::SetProp) {
        MOZ_ASSERT(numInputs == 2);
        allocator.initInputLocation(1, ic->rhs());
      } else {
        MOZ_ASSERT(numInputs == 3);
        allocator.initInputLocation(1, ic->id());
        allocator.initInputLocation(2, ic->rhs());
      }
      break;
    }
    case CacheKind::GetName: {
      IonGetNameIC* ic = ic_->asGetNameIC();
      ValueOperand output = ic->output();

      available.add(output);
      available.add(ic->temp());

      liveRegs_.emplace(ic->liveRegs());
      outputUnchecked_.emplace(output);

      MOZ_ASSERT(numInputs == 1);
      allocator.initInputLocation(0, ic->environment(), JSVAL_TYPE_OBJECT);
      break;
    }
    case CacheKind::BindName: {
      IonBindNameIC* ic = ic_->asBindNameIC();
      Register output = ic->output();

      available.add(output);
      available.add(ic->temp());

      liveRegs_.emplace(ic->liveRegs());
      outputUnchecked_.emplace(
          TypedOrValueRegister(MIRType::Object, AnyRegister(output)));

      MOZ_ASSERT(numInputs == 1);
      allocator.initInputLocation(0, ic->environment(), JSVAL_TYPE_OBJECT);
      break;
    }
    case CacheKind::GetIterator: {
      IonGetIteratorIC* ic = ic_->asGetIteratorIC();
      Register output = ic->output();

      available.add(output);
      available.add(ic->temp1());
      available.add(ic->temp2());

      liveRegs_.emplace(ic->liveRegs());
      outputUnchecked_.emplace(
          TypedOrValueRegister(MIRType::Object, AnyRegister(output)));

      MOZ_ASSERT(numInputs == 1);
      allocator.initInputLocation(0, ic->value());
      break;
    }
    case CacheKind::OptimizeSpreadCall: {
      auto* ic = ic_->asOptimizeSpreadCallIC();
      ValueOperand output = ic->output();

      available.add(output);
      available.add(ic->temp());

      liveRegs_.emplace(ic->liveRegs());
      outputUnchecked_.emplace(output);

      MOZ_ASSERT(numInputs == 1);
      allocator.initInputLocation(0, ic->value());
      break;
    }
    case CacheKind::In: {
      IonInIC* ic = ic_->asInIC();
      Register output = ic->output();

      available.add(output);

      liveRegs_.emplace(ic->liveRegs());
      outputUnchecked_.emplace(
          TypedOrValueRegister(MIRType::Boolean, AnyRegister(output)));

      MOZ_ASSERT(numInputs == 2);
      allocator.initInputLocation(0, ic->key());
      allocator.initInputLocation(
          1, TypedOrValueRegister(MIRType::Object, AnyRegister(ic->object())));
      break;
    }
    case CacheKind::HasOwn: {
      IonHasOwnIC* ic = ic_->asHasOwnIC();
      Register output = ic->output();

      available.add(output);

      liveRegs_.emplace(ic->liveRegs());
      outputUnchecked_.emplace(
          TypedOrValueRegister(MIRType::Boolean, AnyRegister(output)));

      MOZ_ASSERT(numInputs == 2);
      allocator.initInputLocation(0, ic->id());
      allocator.initInputLocation(1, ic->value());
      break;
    }
    case CacheKind::CheckPrivateField: {
      IonCheckPrivateFieldIC* ic = ic_->asCheckPrivateFieldIC();
      Register output = ic->output();

      available.add(output);

      liveRegs_.emplace(ic->liveRegs());
      outputUnchecked_.emplace(
          TypedOrValueRegister(MIRType::Boolean, AnyRegister(output)));

      MOZ_ASSERT(numInputs == 2);
      allocator.initInputLocation(0, ic->value());
      allocator.initInputLocation(1, ic->id());
      break;
    }
    case CacheKind::InstanceOf: {
      IonInstanceOfIC* ic = ic_->asInstanceOfIC();
      Register output = ic->output();
      available.add(output);
      liveRegs_.emplace(ic->liveRegs());
      outputUnchecked_.emplace(
          TypedOrValueRegister(MIRType::Boolean, AnyRegister(output)));

      MOZ_ASSERT(numInputs == 2);
      allocator.initInputLocation(0, ic->lhs());
      allocator.initInputLocation(
          1, TypedOrValueRegister(MIRType::Object, AnyRegister(ic->rhs())));
      break;
    }
    case CacheKind::ToPropertyKey: {
      IonToPropertyKeyIC* ic = ic_->asToPropertyKeyIC();
      ValueOperand output = ic->output();

      available.add(output);

      liveRegs_.emplace(ic->liveRegs());
      outputUnchecked_.emplace(TypedOrValueRegister(output));

      MOZ_ASSERT(numInputs == 1);
      allocator.initInputLocation(0, ic->input());
      break;
    }
    case CacheKind::UnaryArith: {
      IonUnaryArithIC* ic = ic_->asUnaryArithIC();
      ValueOperand output = ic->output();

      available.add(output);

      liveRegs_.emplace(ic->liveRegs());
      outputUnchecked_.emplace(TypedOrValueRegister(output));

      MOZ_ASSERT(numInputs == 1);
      allocator.initInputLocation(0, ic->input());
      break;
    }
    case CacheKind::BinaryArith: {
      IonBinaryArithIC* ic = ic_->asBinaryArithIC();
      ValueOperand output = ic->output();

      available.add(output);

      liveRegs_.emplace(ic->liveRegs());
      outputUnchecked_.emplace(TypedOrValueRegister(output));

      MOZ_ASSERT(numInputs == 2);
      allocator.initInputLocation(0, ic->lhs());
      allocator.initInputLocation(1, ic->rhs());
      break;
    }
    case CacheKind::Compare: {
      IonCompareIC* ic = ic_->asCompareIC();
      Register output = ic->output();

      available.add(output);

      liveRegs_.emplace(ic->liveRegs());
      outputUnchecked_.emplace(
          TypedOrValueRegister(MIRType::Boolean, AnyRegister(output)));

      MOZ_ASSERT(numInputs == 2);
      allocator.initInputLocation(0, ic->lhs());
      allocator.initInputLocation(1, ic->rhs());
      break;
    }
    case CacheKind::CloseIter: {
      IonCloseIterIC* ic = ic_->asCloseIterIC();

      available.add(ic->temp());

      liveRegs_.emplace(ic->liveRegs());
      allocator.initInputLocation(0, ic->iter(), JSVAL_TYPE_OBJECT);
      break;
    }
    case CacheKind::OptimizeGetIterator: {
      auto* ic = ic_->asOptimizeGetIteratorIC();
      Register output = ic->output();

      available.add(output);
      available.add(ic->temp());

      liveRegs_.emplace(ic->liveRegs());
      outputUnchecked_.emplace(
          TypedOrValueRegister(MIRType::Boolean, AnyRegister(output)));

      MOZ_ASSERT(numInputs == 1);
      allocator.initInputLocation(0, ic->value());
      break;
    }
    case CacheKind::Call:
    case CacheKind::TypeOf:
    case CacheKind::TypeOfEq:
    case CacheKind::ToBool:
    case CacheKind::LazyConstant:
    case CacheKind::NewArray:
    case CacheKind::NewObject:
    case CacheKind::Lambda:
    case CacheKind::GetImport:
      MOZ_CRASH("Unsupported IC");
  }

  liveFloatRegs_ = LiveFloatRegisterSet(liveRegs_->fpus());

  allocator.initAvailableRegs(available);
  allocator.initAvailableRegsAfterSpill();
  return true;
}

JitCode* IonCacheIRCompiler::compile(IonICStub* stub) {
  AutoCreatedBy acb(masm, "IonCacheIRCompiler::compile");

  masm.setFramePushed(ionScript_->frameSize());
  if (cx_->runtime()->geckoProfiler().enabled()) {
    masm.enableProfilingInstrumentation();
  }

  allocator.fixupAliasedInputs(masm);

  CacheIRReader reader(writer_);
  do {
    CacheOp op = reader.readOp();
    perfSpewer_.recordInstruction(masm, op);
    switch (op) {
#define DEFINE_OP(op, ...)                 \
  case CacheOp::op:                        \
    if (!emit##op(reader)) return nullptr; \
    break;
      CACHE_IR_OPS(DEFINE_OP)
#undef DEFINE_OP

      default:
        MOZ_CRASH("Invalid op");
    }
    allocator.nextOp();
  } while (reader.more());

  masm.assumeUnreachable("Should have returned from IC");

  // Done emitting the main IC code. Now emit the failure paths.
  for (size_t i = 0; i < failurePaths.length(); i++) {
    if (!emitFailurePath(i)) {
      return nullptr;
    }
    Register scratch = ic_->scratchRegisterForEntryJump();
    CodeOffset offset = masm.movWithPatch(ImmWord(-1), scratch);
    masm.jump(Address(scratch, 0));
    if (!nextCodeOffsets_.append(offset)) {
      return nullptr;
    }
  }

  Linker linker(masm);
  Rooted<JitCode*> newStubCode(cx_, linker.newCode(cx_, CodeKind::Ion));
  if (!newStubCode) {
    cx_->recoverFromOutOfMemory();
    return nullptr;
  }

  newStubCode->setLocalTracingSlots(localTracingSlots_);

  for (CodeOffset offset : nextCodeOffsets_) {
    Assembler::PatchDataWithValueCheck(CodeLocationLabel(newStubCode, offset),
                                       ImmPtr(stub->nextCodeRawPtr()),
                                       ImmPtr((void*)-1));
  }
  if (stubJitCodeOffset_) {
    Assembler::PatchDataWithValueCheck(
        CodeLocationLabel(newStubCode, *stubJitCodeOffset_),
        ImmPtr(newStubCode.get()), ImmPtr((void*)-1));
  }

  return newStubCode;
}

#ifdef DEBUG
void IonCacheIRCompiler::assertFloatRegisterAvailable(FloatRegister reg) {
  switch (ic_->kind()) {
    case CacheKind::GetProp:
    case CacheKind::GetElem:
    case CacheKind::GetPropSuper:
    case CacheKind::GetElemSuper:
    case CacheKind::GetName:
    case CacheKind::BindName:
    case CacheKind::GetIterator:
    case CacheKind::In:
    case CacheKind::HasOwn:
    case CacheKind::CheckPrivateField:
    case CacheKind::InstanceOf:
    case CacheKind::UnaryArith:
    case CacheKind::ToPropertyKey:
    case CacheKind::OptimizeSpreadCall:
    case CacheKind::CloseIter:
    case CacheKind::OptimizeGetIterator:
      MOZ_CRASH("No float registers available");
    case CacheKind::SetProp:
    case CacheKind::SetElem:
      // FloatReg0 is available per LIRGenerator::visitSetPropertyCache.
      MOZ_ASSERT(reg == FloatReg0);
      break;
    case CacheKind::BinaryArith:
    case CacheKind::Compare:
      // FloatReg0 and FloatReg1 are available per
      // LIRGenerator::visitBinaryCache.
      MOZ_ASSERT(reg == FloatReg0 || reg == FloatReg1);
      break;
    case CacheKind::Call:
    case CacheKind::TypeOf:
    case CacheKind::TypeOfEq:
    case CacheKind::ToBool:
    case CacheKind::LazyConstant:
    case CacheKind::NewArray:
    case CacheKind::NewObject:
    case CacheKind::Lambda:
    case CacheKind::GetImport:
      MOZ_CRASH("Unsupported IC");
  }
}
#endif

bool IonCacheIRCompiler::emitGuardShape(ObjOperandId objId,
                                        uint32_t shapeOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  Shape* shape = weakShapeStubField(shapeOffset);

  bool needSpectreMitigations = objectGuardNeedsSpectreMitigations(objId);

  Maybe<AutoScratchRegister> maybeScratch;
  if (needSpectreMitigations) {
    maybeScratch.emplace(allocator, masm);
  }

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  if (needSpectreMitigations) {
    masm.branchTestObjShape(Assembler::NotEqual, obj, shape, *maybeScratch, obj,
                            failure->label());
  } else {
    masm.branchTestObjShapeNoSpectreMitigations(Assembler::NotEqual, obj, shape,
                                                failure->label());
  }

  return true;
}

bool IonCacheIRCompiler::emitGuardProto(ObjOperandId objId,
                                        uint32_t protoOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  JSObject* proto = weakObjectStubField(protoOffset);

  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadObjProto(obj, scratch);
  masm.branchPtr(Assembler::NotEqual, scratch, ImmGCPtr(proto),
                 failure->label());
  return true;
}

bool IonCacheIRCompiler::emitGuardCompartment(ObjOperandId objId,
                                              uint32_t globalOffset,
                                              uint32_t compartmentOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  JSObject* globalWrapper = objectStubField(globalOffset);
  JS::Compartment* compartment = compartmentStubField(compartmentOffset);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Verify that the global wrapper is still valid, as
  // it is pre-requisite for doing the compartment check.
  masm.movePtr(ImmGCPtr(globalWrapper), scratch);
  Address handlerAddr(scratch, ProxyObject::offsetOfHandler());
  masm.branchPtr(Assembler::Equal, handlerAddr,
                 ImmPtr(&DeadObjectProxy::singleton), failure->label());

  masm.branchTestObjCompartment(Assembler::NotEqual, obj, compartment, scratch,
                                failure->label());
  return true;
}

bool IonCacheIRCompiler::emitGuardAnyClass(ObjOperandId objId,
                                           uint32_t claspOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch(allocator, masm);

  const JSClass* clasp = classStubField(claspOffset);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  if (objectGuardNeedsSpectreMitigations(objId)) {
    masm.branchTestObjClass(Assembler::NotEqual, obj, clasp, scratch, obj,
                            failure->label());
  } else {
    masm.branchTestObjClassNoSpectreMitigations(Assembler::NotEqual, obj, clasp,
                                                scratch, failure->label());
  }

  return true;
}

bool IonCacheIRCompiler::emitGuardHasProxyHandler(ObjOperandId objId,
                                                  uint32_t handlerOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  const void* handler = proxyHandlerStubField(handlerOffset);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Address handlerAddr(obj, ProxyObject::offsetOfHandler());
  masm.branchPtr(Assembler::NotEqual, handlerAddr, ImmPtr(handler),
                 failure->label());
  return true;
}

bool IonCacheIRCompiler::emitGuardSpecificObject(ObjOperandId objId,
                                                 uint32_t expectedOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  JSObject* expected = weakObjectStubField(expectedOffset);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branchPtr(Assembler::NotEqual, obj, ImmGCPtr(expected),
                 failure->label());
  return true;
}

bool IonCacheIRCompiler::emitGuardSpecificFunction(
    ObjOperandId objId, uint32_t expectedOffset, uint32_t nargsAndFlagsOffset) {
  return emitGuardSpecificObject(objId, expectedOffset);
}

bool IonCacheIRCompiler::emitGuardSpecificAtom(StringOperandId strId,
                                               uint32_t expectedOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register str = allocator.useRegister(masm, strId);
  AutoScratchRegister scratch(allocator, masm);

  JSAtom* atom = &stringStubField(expectedOffset)->asAtom();

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  LiveRegisterSet volatileRegs = liveVolatileRegs();
  volatileRegs.takeUnchecked(scratch);

  masm.guardSpecificAtom(str, atom, scratch, volatileRegs, failure->label());
  return true;
}

bool IonCacheIRCompiler::emitGuardSpecificSymbol(SymbolOperandId symId,
                                                 uint32_t expectedOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register sym = allocator.useRegister(masm, symId);
  JS::Symbol* expected = symbolStubField(expectedOffset);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branchPtr(Assembler::NotEqual, sym, ImmGCPtr(expected),
                 failure->label());
  return true;
}

bool IonCacheIRCompiler::emitGuardSpecificValue(ValOperandId valId,
                                                uint32_t expectedOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  ValueOperand val = allocator.useValueRegister(masm, valId);
  Value expected = valueStubField(expectedOffset);

  Maybe<AutoScratchRegister> maybeScratch;
  if (expected.isNaN()) {
    maybeScratch.emplace(allocator, masm);
  }

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  if (expected.isNaN()) {
    masm.branchTestNaNValue(Assembler::NotEqual, val, *maybeScratch,
                            failure->label());
  } else {
    masm.branchTestValue(Assembler::NotEqual, val, expected, failure->label());
  }
  return true;
}

bool IonCacheIRCompiler::emitLoadValueResult(uint32_t valOffset) {
  MOZ_CRASH("Baseline-specific op");
}

bool IonCacheIRCompiler::emitLoadFixedSlotResult(ObjOperandId objId,
                                                 uint32_t offsetOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  int32_t offset = int32StubField(offsetOffset);
  masm.loadTypedOrValue(Address(obj, offset), output);
  return true;
}

bool IonCacheIRCompiler::emitLoadFixedSlotTypedResult(ObjOperandId objId,
                                                      uint32_t offsetOffset,
                                                      ValueType) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitLoadDynamicSlotResult(ObjOperandId objId,
                                                   uint32_t offsetOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  int32_t offset = int32StubField(offsetOffset);

  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), scratch);
  masm.loadTypedOrValue(Address(scratch, offset), output);
  return true;
}

bool IonCacheIRCompiler::emitCallScriptedGetterResult(
    ValOperandId receiverId, uint32_t getterOffset, bool sameRealm,
    uint32_t nargsAndFlagsOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoSaveLiveRegisters save(*this);
  AutoOutputRegister output(*this);

  ValueOperand receiver = allocator.useValueRegister(masm, receiverId);

  JSFunction* target = &objectStubField(getterOffset)->as<JSFunction>();
  AutoScratchRegister scratch(allocator, masm);

  MOZ_ASSERT(sameRealm == (cx_->realm() == target->realm()));

  allocator.discardStack(masm);

  uint32_t framePushedBefore = masm.framePushed();

  enterStubFrame(masm, save);

  // The JitFrameLayout pushed below will be aligned to JitStackAlignment,
  // so we just have to make sure the stack is aligned after we push the
  // |this| + argument Values.
  uint32_t argSize = (target->nargs() + 1) * sizeof(Value);
  uint32_t padding =
      ComputeByteAlignment(masm.framePushed() + argSize, JitStackAlignment);
  MOZ_ASSERT(padding % sizeof(uintptr_t) == 0);
  MOZ_ASSERT(padding < JitStackAlignment);
  masm.reserveStack(padding);

  for (size_t i = 0; i < target->nargs(); i++) {
    masm.Push(UndefinedValue());
  }
  masm.Push(receiver);

  if (!sameRealm) {
    masm.switchToRealm(target->realm(), scratch);
  }

  masm.movePtr(ImmGCPtr(target), scratch);

  masm.Push(scratch);
  masm.PushFrameDescriptorForJitCall(FrameType::IonICCall, /* argc = */ 0);

  // Check stack alignment. Add 2 * sizeof(uintptr_t) for the return address and
  // frame pointer pushed by the call/callee.
  MOZ_ASSERT(
      ((masm.framePushed() + 2 * sizeof(uintptr_t)) % JitStackAlignment) == 0);

  MOZ_ASSERT(target->hasJitEntry());
  masm.loadJitCodeRaw(scratch, scratch);
  masm.callJit(scratch);

  if (!sameRealm) {
    static_assert(!JSReturnOperand.aliases(ReturnReg),
                  "ReturnReg available as scratch after scripted calls");
    masm.switchToRealm(cx_->realm(), ReturnReg);
  }

  masm.storeCallResultValue(output);

  // Restore the frame pointer and stack pointer.
  masm.loadPtr(Address(FramePointer, 0), FramePointer);
  masm.freeStack(masm.framePushed() - framePushedBefore);
  return true;
}

#ifdef JS_PUNBOX64
template <typename IdType>
bool IonCacheIRCompiler::emitCallScriptedProxyGetShared(
    ValOperandId targetId, ObjOperandId receiverId, ObjOperandId handlerId,
    ObjOperandId trapId, IdType id, uint32_t nargsAndFlags) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoSaveLiveRegisters save(*this);
  AutoOutputRegister output(*this);

  ValueOperand target = allocator.useValueRegister(masm, targetId);
  Register receiver = allocator.useRegister(masm, receiverId);
  Register handler = allocator.useRegister(masm, handlerId);
  Register callee = allocator.useRegister(masm, trapId);
  ValueOperand idVal;
  if constexpr (std::is_same_v<IdType, ValOperandId>) {
    idVal = allocator.useValueRegister(masm, id);
  }
  size_t nargs = nargsAndFlags >> JSFunction::ArgCountShift;

  AutoScratchRegister scratch(allocator, masm);
  AutoScratchRegister scratch2(allocator, masm);
  ValueOperand scratchVal(scratch);
  ValueOperand scratchVal2(scratch2);

  allocator.discardStack(masm);

  uint32_t framePushedBefore = masm.framePushed();

  enterStubFrame(masm, save);

  // We need to keep the target around to potentially validate the proxy result
  storeTracedValue(masm, target);
  if constexpr (std::is_same_v<IdType, ValOperandId>) {
    // Same for the id, assuming it's not baked in
    storeTracedValue(masm, idVal);
  }
  uint32_t framePushedBeforeArgs = masm.framePushed();

  // The JitFrameLayout pushed below will be aligned to JitStackAlignment,
  // so we just have to make sure the stack is aligned after we push the
  // |this| + argument Values.
  uint32_t argSize = (std::max(nargs, (size_t)3) + 1) * sizeof(Value);
  uint32_t padding =
      ComputeByteAlignment(masm.framePushed() + argSize, JitStackAlignment);
  MOZ_ASSERT(padding % sizeof(uintptr_t) == 0);
  MOZ_ASSERT(padding < JitStackAlignment);
  masm.reserveStack(padding);

  for (size_t i = 3; i < nargs; i++) {
    masm.Push(UndefinedValue());
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, receiver, scratchVal);
  masm.Push(scratchVal);

  if constexpr (std::is_same_v<IdType, ValOperandId>) {
    masm.Push(idVal);
  } else {
    masm.movePropertyKey(idStubField(id), scratch);
    masm.tagValue(JSVAL_TYPE_STRING, scratch, scratchVal);
    masm.Push(scratchVal);
  }

  masm.Push(target);

  masm.tagValue(JSVAL_TYPE_OBJECT, handler, scratchVal);
  masm.Push(scratchVal);

  masm.Push(callee);
  masm.PushFrameDescriptorForJitCall(FrameType::IonICCall, /* argc = */ 3);

  // Check stack alignment. Add 2 * sizeof(uintptr_t) for the return address and
  // frame pointer pushed by the call/callee.
  MOZ_ASSERT(
      ((masm.framePushed() + 2 * sizeof(uintptr_t)) % JitStackAlignment) == 0);

  masm.loadJitCodeRaw(callee, scratch);
  masm.callJit(scratch);

  masm.storeCallResultValue(output);

  Label success, end;
  loadTracedValue(masm, 0, scratchVal);
  masm.unboxObject(scratchVal, scratch);
  masm.branchTestObjectNeedsProxyResultValidation(Assembler::Zero, scratch,
                                                  scratch2, &success);

  if constexpr (std::is_same_v<IdType, ValOperandId>) {
    loadTracedValue(masm, 1, scratchVal2);
  } else {
    masm.moveValue(StringValue(idStubField(id).toString()), scratchVal2);
  }

  uint32_t framePushedAfterCall = masm.framePushed();
  masm.freeStack(masm.framePushed() - framePushedBeforeArgs);

  masm.Push(output.valueReg());
  masm.Push(scratchVal2);
  masm.Push(scratch);

  using Fn = bool (*)(JSContext*, HandleObject, HandleValue, HandleValue,
                      MutableHandleValue);
  callVM<Fn, CheckProxyGetByValueResult>(masm);

  masm.storeCallResultValue(output);

  masm.jump(&end);
  masm.bind(&success);
  masm.setFramePushed(framePushedAfterCall);

  // Restore the frame pointer and stack pointer.
  masm.loadPtr(Address(FramePointer, 0), FramePointer);
  masm.freeStack(masm.framePushed() - framePushedBefore);
  masm.bind(&end);

  return true;
}

bool IonCacheIRCompiler::emitCallScriptedProxyGetResult(
    ValOperandId targetId, ObjOperandId receiverId, ObjOperandId handlerId,
    ObjOperandId trapId, uint32_t id, uint32_t nargsAndFlags) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  return emitCallScriptedProxyGetShared(targetId, receiverId, handlerId, trapId,
                                        id, nargsAndFlags);
}

bool IonCacheIRCompiler::emitCallScriptedProxyGetByValueResult(
    ValOperandId targetId, ObjOperandId receiverId, ObjOperandId handlerId,
    ValOperandId idId, ObjOperandId trapId, uint32_t nargsAndFlags) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  return emitCallScriptedProxyGetShared(targetId, receiverId, handlerId, trapId,
                                        idId, nargsAndFlags);
}
#endif

bool IonCacheIRCompiler::emitCallInlinedGetterResult(
    ValOperandId receiverId, uint32_t getterOffset, uint32_t icScriptOffset,
    bool sameRealm, uint32_t nargsAndFlagsOffset) {
  MOZ_CRASH("Trial inlining not supported in Ion");
}

bool IonCacheIRCompiler::emitCallNativeGetterResult(
    ValOperandId receiverId, uint32_t getterOffset, bool sameRealm,
    uint32_t nargsAndFlagsOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoSaveLiveRegisters save(*this);
  AutoOutputRegister output(*this);

  ValueOperand receiver = allocator.useValueRegister(masm, receiverId);

  JSFunction* target = &objectStubField(getterOffset)->as<JSFunction>();
  MOZ_ASSERT(target->isNativeFun());

  AutoScratchRegisterMaybeOutput argJSContext(allocator, masm, output);
  AutoScratchRegisterMaybeOutputType argUintN(allocator, masm, output);
  AutoScratchRegister argVp(allocator, masm);
  AutoScratchRegister scratch(allocator, masm);

  allocator.discardStack(masm);

  // Native functions have the signature:
  //  bool (*)(JSContext*, unsigned, Value* vp)
  // Where vp[0] is space for an outparam, vp[1] is |this|, and vp[2] onward
  // are the function arguments.

  // Construct vp array:
  // Push receiver value for |this|
  masm.Push(receiver);
  // Push callee/outparam.
  masm.Push(ObjectValue(*target));

  // Preload arguments into registers.
  masm.loadJSContext(argJSContext);
  masm.move32(Imm32(0), argUintN);
  masm.moveStackPtrTo(argVp.get());

  // Push marking data for later use.
  masm.Push(argUintN);
  pushStubCodePointer();

  if (!masm.icBuildOOLFakeExitFrame(GetReturnAddressToIonCode(cx_), save)) {
    return false;
  }
  masm.enterFakeExitFrame(argJSContext, scratch, ExitFrameType::IonOOLNative);

  if (!sameRealm) {
    masm.switchToRealm(target->realm(), scratch);
  }

  // Construct and execute call.
  masm.setupUnalignedABICall(scratch);
  masm.passABIArg(argJSContext);
  masm.passABIArg(argUintN);
  masm.passABIArg(argVp);
  masm.callWithABI(DynamicFunction<JSNative>(target->native()),
                   ABIType::General,
                   CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  // Test for failure.
  masm.branchIfFalseBool(ReturnReg, masm.exceptionLabel());

  if (!sameRealm) {
    masm.switchToRealm(cx_->realm(), ReturnReg);
  }

  // Load the outparam vp[0] into output register(s).
  Address outparam(masm.getStackPointer(),
                   IonOOLNativeExitFrameLayout::offsetOfResult());
  masm.loadValue(outparam, output.valueReg());

  if (JitOptions.spectreJitToCxxCalls) {
    masm.speculationBarrier();
  }

  masm.adjustStack(IonOOLNativeExitFrameLayout::Size(0));
  return true;
}

bool IonCacheIRCompiler::emitCallDOMGetterResult(ObjOperandId objId,
                                                 uint32_t jitInfoOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoSaveLiveRegisters save(*this);
  AutoOutputRegister output(*this);

  Register obj = allocator.useRegister(masm, objId);

  const JSJitInfo* info = rawPointerStubField<const JSJitInfo*>(jitInfoOffset);

  allocator.discardStack(masm);
  enterStubFrame(masm, save);

  masm.Push(obj);
  masm.Push(ImmPtr(info));

  using Fn =
      bool (*)(JSContext*, const JSJitInfo*, HandleObject, MutableHandleValue);
  callVM<Fn, jit::CallDOMGetter>(masm);

  masm.storeCallResultValue(output);
  return true;
}

bool IonCacheIRCompiler::emitCallDOMSetter(ObjOperandId objId,
                                           uint32_t jitInfoOffset,
                                           ValOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoSaveLiveRegisters save(*this);

  Register obj = allocator.useRegister(masm, objId);
  ValueOperand val = allocator.useValueRegister(masm, rhsId);

  const JSJitInfo* info = rawPointerStubField<const JSJitInfo*>(jitInfoOffset);

  allocator.discardStack(masm);
  enterStubFrame(masm, save);

  masm.Push(val);
  masm.Push(obj);
  masm.Push(ImmPtr(info));

  using Fn = bool (*)(JSContext*, const JSJitInfo*, HandleObject, HandleValue);
  callVM<Fn, jit::CallDOMSetter>(masm);
  return true;
}

bool IonCacheIRCompiler::emitProxyGetResult(ObjOperandId objId,
                                            uint32_t idOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoSaveLiveRegisters save(*this);
  AutoOutputRegister output(*this);

  Register obj = allocator.useRegister(masm, objId);
  jsid id = idStubField(idOffset);

  // ProxyGetProperty(JSContext* cx, HandleObject proxy, HandleId id,
  //                  MutableHandleValue vp)
  AutoScratchRegisterMaybeOutput argJSContext(allocator, masm, output);
  AutoScratchRegisterMaybeOutputType argProxy(allocator, masm, output);
  AutoScratchRegister argId(allocator, masm);
  AutoScratchRegister argVp(allocator, masm);
  AutoScratchRegister scratch(allocator, masm);

  allocator.discardStack(masm);

  // Push stubCode for marking.
  pushStubCodePointer();

  // Push args on stack first so we can take pointers to make handles.
  masm.Push(UndefinedValue());
  masm.moveStackPtrTo(argVp.get());

  masm.Push(id, scratch);
  masm.moveStackPtrTo(argId.get());

  // Push the proxy. Also used as receiver.
  masm.Push(obj);
  masm.moveStackPtrTo(argProxy.get());

  masm.loadJSContext(argJSContext);

  if (!masm.icBuildOOLFakeExitFrame(GetReturnAddressToIonCode(cx_), save)) {
    return false;
  }
  masm.enterFakeExitFrame(argJSContext, scratch, ExitFrameType::IonOOLProxy);

  // Make the call.
  using Fn = bool (*)(JSContext* cx, HandleObject proxy, HandleId id,
                      MutableHandleValue vp);
  masm.setupUnalignedABICall(scratch);
  masm.passABIArg(argJSContext);
  masm.passABIArg(argProxy);
  masm.passABIArg(argId);
  masm.passABIArg(argVp);
  masm.callWithABI<Fn, ProxyGetProperty>(
      ABIType::General, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  // Test for failure.
  masm.branchIfFalseBool(ReturnReg, masm.exceptionLabel());

  // Load the outparam vp[0] into output register(s).
  Address outparam(masm.getStackPointer(),
                   IonOOLProxyExitFrameLayout::offsetOfResult());
  masm.loadValue(outparam, output.valueReg());

  // Spectre mitigation in case of speculative execution within C++ code.
  if (JitOptions.spectreJitToCxxCalls) {
    masm.speculationBarrier();
  }

  // masm.leaveExitFrame & pop locals
  masm.adjustStack(IonOOLProxyExitFrameLayout::Size());
  return true;
}

bool IonCacheIRCompiler::emitFrameIsConstructingResult() {
  MOZ_CRASH("Baseline-specific op");
}

bool IonCacheIRCompiler::emitLoadConstantStringResult(uint32_t strOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  MOZ_CRASH("not used in ion");
}

bool IonCacheIRCompiler::emitCompareStringResult(JSOp op, StringOperandId lhsId,
                                                 StringOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoSaveLiveRegisters save(*this);
  AutoOutputRegister output(*this);

  Register left = allocator.useRegister(masm, lhsId);
  Register right = allocator.useRegister(masm, rhsId);

  allocator.discardStack(masm);

  Label slow, done;
  MOZ_ASSERT(!output.hasValue());
  masm.compareStrings(op, left, right, output.typedReg().gpr(), &slow);

  masm.jump(&done);
  masm.bind(&slow);

  enterStubFrame(masm, save);

  // Push the operands in reverse order for JSOp::Le and JSOp::Gt:
  // - |left <= right| is implemented as |right >= left|.
  // - |left > right| is implemented as |right < left|.
  if (op == JSOp::Le || op == JSOp::Gt) {
    masm.Push(left);
    masm.Push(right);
  } else {
    masm.Push(right);
    masm.Push(left);
  }

  using Fn = bool (*)(JSContext*, HandleString, HandleString, bool*);
  if (op == JSOp::Eq || op == JSOp::StrictEq) {
    callVM<Fn, jit::StringsEqual<EqualityKind::Equal>>(masm);
  } else if (op == JSOp::Ne || op == JSOp::StrictNe) {
    callVM<Fn, jit::StringsEqual<EqualityKind::NotEqual>>(masm);
  } else if (op == JSOp::Lt || op == JSOp::Gt) {
    callVM<Fn, jit::StringsCompare<ComparisonKind::LessThan>>(masm);
  } else {
    MOZ_ASSERT(op == JSOp::Le || op == JSOp::Ge);
    callVM<Fn, jit::StringsCompare<ComparisonKind::GreaterThanOrEqual>>(masm);
  }

  masm.storeCallBoolResult(output.typedReg().gpr());
  masm.bind(&done);
  return true;
}

bool IonCacheIRCompiler::emitStoreFixedSlot(ObjOperandId objId,
                                            uint32_t offsetOffset,
                                            ValOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  int32_t offset = int32StubField(offsetOffset);
  ConstantOrRegister val = allocator.useConstantOrRegister(masm, rhsId);
  AutoScratchRegister scratch(allocator, masm);

  Address slot(obj, offset);
  EmitPreBarrier(masm, slot, MIRType::Value);
  masm.storeConstantOrRegister(val, slot);
  emitPostBarrierSlot(obj, val, scratch);
  return true;
}

bool IonCacheIRCompiler::emitStoreDynamicSlot(ObjOperandId objId,
                                              uint32_t offsetOffset,
                                              ValOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  int32_t offset = int32StubField(offsetOffset);
  ConstantOrRegister val = allocator.useConstantOrRegister(masm, rhsId);
  AutoScratchRegister scratch(allocator, masm);

  masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), scratch);
  Address slot(scratch, offset);
  EmitPreBarrier(masm, slot, MIRType::Value);
  masm.storeConstantOrRegister(val, slot);
  emitPostBarrierSlot(obj, val, scratch);
  return true;
}

bool IonCacheIRCompiler::emitAddAndStoreSlotShared(
    CacheOp op, ObjOperandId objId, uint32_t offsetOffset, ValOperandId rhsId,
    uint32_t newShapeOffset, Maybe<uint32_t> numNewSlotsOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  int32_t offset = int32StubField(offsetOffset);
  ConstantOrRegister val = allocator.useConstantOrRegister(masm, rhsId);

  AutoScratchRegister scratch1(allocator, masm);

  Maybe<AutoScratchRegister> scratch2;
  if (op == CacheOp::AllocateAndStoreDynamicSlot) {
    scratch2.emplace(allocator, masm);
  }

  Shape* newShape = shapeStubField(newShapeOffset);

  if (op == CacheOp::AllocateAndStoreDynamicSlot) {
    // We have to (re)allocate dynamic slots. Do this first, as it's the
    // only fallible operation here. Note that growSlotsPure is
    // fallible but does not GC.

    FailurePath* failure;
    if (!addFailurePath(&failure)) {
      return false;
    }

    int32_t numNewSlots = int32StubField(*numNewSlotsOffset);
    MOZ_ASSERT(numNewSlots > 0);

    LiveRegisterSet save = liveVolatileRegs();
    masm.PushRegsInMask(save);

    using Fn = bool (*)(JSContext* cx, NativeObject* obj, uint32_t newCount);
    masm.setupUnalignedABICall(scratch1);
    masm.loadJSContext(scratch1);
    masm.passABIArg(scratch1);
    masm.passABIArg(obj);
    masm.move32(Imm32(numNewSlots), scratch2.ref());
    masm.passABIArg(scratch2.ref());
    masm.callWithABI<Fn, NativeObject::growSlotsPure>();
    masm.storeCallPointerResult(scratch1);

    LiveRegisterSet ignore;
    ignore.add(scratch1);
    masm.PopRegsInMaskIgnore(save, ignore);

    masm.branchIfFalseBool(scratch1, failure->label());
  }

  // Update the object's shape.
  masm.storeObjShape(newShape, obj,
                     [](MacroAssembler& masm, const Address& addr) {
                       EmitPreBarrier(masm, addr, MIRType::Shape);
                     });

  // Perform the store. No pre-barrier required since this is a new
  // initialization.
  if (op == CacheOp::AddAndStoreFixedSlot) {
    Address slot(obj, offset);
    masm.storeConstantOrRegister(val, slot);
  } else {
    MOZ_ASSERT(op == CacheOp::AddAndStoreDynamicSlot ||
               op == CacheOp::AllocateAndStoreDynamicSlot);
    masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), scratch1);
    Address slot(scratch1, offset);
    masm.storeConstantOrRegister(val, slot);
  }

  emitPostBarrierSlot(obj, val, scratch1);

  return true;
}

bool IonCacheIRCompiler::emitAddAndStoreFixedSlot(ObjOperandId objId,
                                                  uint32_t offsetOffset,
                                                  ValOperandId rhsId,
                                                  uint32_t newShapeOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Maybe<uint32_t> numNewSlotsOffset = mozilla::Nothing();
  return emitAddAndStoreSlotShared(CacheOp::AddAndStoreFixedSlot, objId,
                                   offsetOffset, rhsId, newShapeOffset,
                                   numNewSlotsOffset);
}

bool IonCacheIRCompiler::emitAddAndStoreDynamicSlot(ObjOperandId objId,
                                                    uint32_t offsetOffset,
                                                    ValOperandId rhsId,
                                                    uint32_t newShapeOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Maybe<uint32_t> numNewSlotsOffset = mozilla::Nothing();
  return emitAddAndStoreSlotShared(CacheOp::AddAndStoreDynamicSlot, objId,
                                   offsetOffset, rhsId, newShapeOffset,
                                   numNewSlotsOffset);
}

bool IonCacheIRCompiler::emitAllocateAndStoreDynamicSlot(
    ObjOperandId objId, uint32_t offsetOffset, ValOperandId rhsId,
    uint32_t newShapeOffset, uint32_t numNewSlotsOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  return emitAddAndStoreSlotShared(CacheOp::AllocateAndStoreDynamicSlot, objId,
                                   offsetOffset, rhsId, newShapeOffset,
                                   mozilla::Some(numNewSlotsOffset));
}

bool IonCacheIRCompiler::emitLoadStringCharResult(StringOperandId strId,
                                                  Int32OperandId indexId,
                                                  bool handleOOB) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register str = allocator.useRegister(masm, strId);
  Register index = allocator.useRegister(masm, indexId);
  AutoScratchRegisterMaybeOutput scratch1(allocator, masm, output);
  AutoScratchRegisterMaybeOutputType scratch2(allocator, masm, output);
  AutoScratchRegister scratch3(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Bounds check, load string char.
  Label done;
  Label loadFailed;
  if (!handleOOB) {
    masm.spectreBoundsCheck32(index, Address(str, JSString::offsetOfLength()),
                              scratch1, failure->label());
    masm.loadStringChar(str, index, scratch1, scratch2, scratch3,
                        failure->label());
  } else {
    // Return the empty string for out-of-bounds access.
    masm.movePtr(ImmGCPtr(cx_->runtime()->emptyString), scratch2);

    // This CacheIR op is always preceded by |LinearizeForCharAccess|, so we're
    // guaranteed to see no nested ropes.
    masm.spectreBoundsCheck32(index, Address(str, JSString::offsetOfLength()),
                              scratch1, &done);
    masm.loadStringChar(str, index, scratch1, scratch2, scratch3, &loadFailed);
  }

  // Load StaticString for this char. For larger code units perform a VM call.
  Label vmCall;
  masm.lookupStaticString(scratch1, scratch2, cx_->staticStrings(), &vmCall);
  masm.jump(&done);

  if (handleOOB) {
    masm.bind(&loadFailed);
    masm.assumeUnreachable("loadStringChar can't fail for linear strings");
  }

  {
    masm.bind(&vmCall);

    // FailurePath and AutoSaveLiveRegisters don't get along very well. Both are
    // modifying the stack and expect that no other stack manipulations are
    // made. Therefore we need to use an ABI call instead of a VM call here.

    LiveRegisterSet volatileRegs = liveVolatileRegs();
    volatileRegs.takeUnchecked(scratch1);
    volatileRegs.takeUnchecked(scratch2);
    volatileRegs.takeUnchecked(scratch3);
    volatileRegs.takeUnchecked(output);
    masm.PushRegsInMask(volatileRegs);

    using Fn = JSLinearString* (*)(JSContext* cx, int32_t code);
    masm.setupUnalignedABICall(scratch2);
    masm.loadJSContext(scratch2);
    masm.passABIArg(scratch2);
    masm.passABIArg(scratch1);
    masm.callWithABI<Fn, jit::StringFromCharCodeNoGC>();
    masm.storeCallPointerResult(scratch2);

    masm.PopRegsInMask(volatileRegs);

    masm.branchPtr(Assembler::Equal, scratch2, ImmWord(0), failure->label());
  }

  masm.bind(&done);
  masm.tagValue(JSVAL_TYPE_STRING, scratch2, output.valueReg());
  return true;
}

bool IonCacheIRCompiler::emitCallNativeSetter(ObjOperandId receiverId,
                                              uint32_t setterOffset,
                                              ValOperandId rhsId,
                                              bool sameRealm,
                                              uint32_t nargsAndFlagsOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoSaveLiveRegisters save(*this);

  Register receiver = allocator.useRegister(masm, receiverId);
  JSFunction* target = &objectStubField(setterOffset)->as<JSFunction>();
  MOZ_ASSERT(target->isNativeFun());
  ConstantOrRegister val = allocator.useConstantOrRegister(masm, rhsId);

  AutoScratchRegister argJSContext(allocator, masm);
  AutoScratchRegister argVp(allocator, masm);
  AutoScratchRegister argUintN(allocator, masm);
#ifndef JS_CODEGEN_X86
  AutoScratchRegister scratch(allocator, masm);
#else
  // Not enough registers on x86.
  Register scratch = argUintN;
#endif

  allocator.discardStack(masm);

  // Set up the call:
  //  bool (*)(JSContext*, unsigned, Value* vp)
  // vp[0] is callee/outparam
  // vp[1] is |this|
  // vp[2] is the value

  // Build vp and move the base into argVpReg.
  masm.Push(val);
  masm.Push(TypedOrValueRegister(MIRType::Object, AnyRegister(receiver)));
  masm.Push(ObjectValue(*target));
  masm.moveStackPtrTo(argVp.get());

  // Preload other regs.
  masm.loadJSContext(argJSContext);
  masm.move32(Imm32(1), argUintN);

  // Push marking data for later use.
  masm.Push(argUintN);
  pushStubCodePointer();

  if (!masm.icBuildOOLFakeExitFrame(GetReturnAddressToIonCode(cx_), save)) {
    return false;
  }
  masm.enterFakeExitFrame(argJSContext, scratch, ExitFrameType::IonOOLNative);

  if (!sameRealm) {
    masm.switchToRealm(target->realm(), scratch);
  }

  // Make the call.
  masm.setupUnalignedABICall(scratch);
#ifdef JS_CODEGEN_X86
  // Reload argUintN because it was clobbered.
  masm.move32(Imm32(1), argUintN);
#endif
  masm.passABIArg(argJSContext);
  masm.passABIArg(argUintN);
  masm.passABIArg(argVp);
  masm.callWithABI(DynamicFunction<JSNative>(target->native()),
                   ABIType::General,
                   CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  // Test for failure.
  masm.branchIfFalseBool(ReturnReg, masm.exceptionLabel());

  if (!sameRealm) {
    masm.switchToRealm(cx_->realm(), ReturnReg);
  }

  masm.adjustStack(IonOOLNativeExitFrameLayout::Size(1));
  return true;
}

bool IonCacheIRCompiler::emitCallScriptedSetter(ObjOperandId receiverId,
                                                uint32_t setterOffset,
                                                ValOperandId rhsId,
                                                bool sameRealm,
                                                uint32_t nargsAndFlagsOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoSaveLiveRegisters save(*this);

  Register receiver = allocator.useRegister(masm, receiverId);
  JSFunction* target = &objectStubField(setterOffset)->as<JSFunction>();
  ConstantOrRegister val = allocator.useConstantOrRegister(masm, rhsId);

  MOZ_ASSERT(sameRealm == (cx_->realm() == target->realm()));

  AutoScratchRegister scratch(allocator, masm);

  allocator.discardStack(masm);

  uint32_t framePushedBefore = masm.framePushed();

  enterStubFrame(masm, save);

  // The JitFrameLayout pushed below will be aligned to JitStackAlignment,
  // so we just have to make sure the stack is aligned after we push the
  // |this| + argument Values.
  size_t numArgs = std::max<size_t>(1, target->nargs());
  uint32_t argSize = (numArgs + 1) * sizeof(Value);
  uint32_t padding =
      ComputeByteAlignment(masm.framePushed() + argSize, JitStackAlignment);
  MOZ_ASSERT(padding % sizeof(uintptr_t) == 0);
  MOZ_ASSERT(padding < JitStackAlignment);
  masm.reserveStack(padding);

  for (size_t i = 1; i < target->nargs(); i++) {
    masm.Push(UndefinedValue());
  }
  masm.Push(val);
  masm.Push(TypedOrValueRegister(MIRType::Object, AnyRegister(receiver)));

  if (!sameRealm) {
    masm.switchToRealm(target->realm(), scratch);
  }

  masm.movePtr(ImmGCPtr(target), scratch);

  masm.Push(scratch);
  masm.PushFrameDescriptorForJitCall(FrameType::IonICCall, /* argc = */ 1);

  // Check stack alignment. Add 2 * sizeof(uintptr_t) for the return address and
  // frame pointer pushed by the call/callee.
  MOZ_ASSERT(
      ((masm.framePushed() + 2 * sizeof(uintptr_t)) % JitStackAlignment) == 0);

  MOZ_ASSERT(target->hasJitEntry());
  masm.loadJitCodeRaw(scratch, scratch);
  masm.callJit(scratch);

  if (!sameRealm) {
    masm.switchToRealm(cx_->realm(), ReturnReg);
  }

  // Restore the frame pointer and stack pointer.
  masm.loadPtr(Address(FramePointer, 0), FramePointer);
  masm.freeStack(masm.framePushed() - framePushedBefore);
  return true;
}

bool IonCacheIRCompiler::emitCallInlinedSetter(
    ObjOperandId receiverId, uint32_t setterOffset, ValOperandId rhsId,
    uint32_t icScriptOffset, bool sameRealm, uint32_t nargsAndFlagsOffset) {
  MOZ_CRASH("Trial inlining not supported in Ion");
}

bool IonCacheIRCompiler::emitCallSetArrayLength(ObjOperandId objId, bool strict,
                                                ValOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoSaveLiveRegisters save(*this);

  Register obj = allocator.useRegister(masm, objId);
  ConstantOrRegister val = allocator.useConstantOrRegister(masm, rhsId);

  allocator.discardStack(masm);
  enterStubFrame(masm, save);

  masm.Push(Imm32(strict));
  masm.Push(val);
  masm.Push(obj);

  using Fn = bool (*)(JSContext*, HandleObject, HandleValue, bool);
  callVM<Fn, jit::SetArrayLength>(masm);
  return true;
}

bool IonCacheIRCompiler::emitProxySet(ObjOperandId objId, uint32_t idOffset,
                                      ValOperandId rhsId, bool strict) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoSaveLiveRegisters save(*this);

  Register obj = allocator.useRegister(masm, objId);
  ConstantOrRegister val = allocator.useConstantOrRegister(masm, rhsId);
  jsid id = idStubField(idOffset);

  AutoScratchRegister scratch(allocator, masm);

  allocator.discardStack(masm);
  enterStubFrame(masm, save);

  masm.Push(Imm32(strict));
  masm.Push(val);
  masm.Push(id, scratch);
  masm.Push(obj);

  using Fn = bool (*)(JSContext*, HandleObject, HandleId, HandleValue, bool);
  callVM<Fn, ProxySetProperty>(masm);
  return true;
}

bool IonCacheIRCompiler::emitProxySetByValue(ObjOperandId objId,
                                             ValOperandId idId,
                                             ValOperandId rhsId, bool strict) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoSaveLiveRegisters save(*this);

  Register obj = allocator.useRegister(masm, objId);
  ConstantOrRegister idVal = allocator.useConstantOrRegister(masm, idId);
  ConstantOrRegister val = allocator.useConstantOrRegister(masm, rhsId);

  allocator.discardStack(masm);
  enterStubFrame(masm, save);

  masm.Push(Imm32(strict));
  masm.Push(val);
  masm.Push(idVal);
  masm.Push(obj);

  using Fn = bool (*)(JSContext*, HandleObject, HandleValue, HandleValue, bool);
  callVM<Fn, ProxySetPropertyByValue>(masm);
  return true;
}

bool IonCacheIRCompiler::emitCallAddOrUpdateSparseElementHelper(
    ObjOperandId objId, Int32OperandId idId, ValOperandId rhsId, bool strict) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoSaveLiveRegisters save(*this);

  Register obj = allocator.useRegister(masm, objId);
  Register id = allocator.useRegister(masm, idId);
  ValueOperand val = allocator.useValueRegister(masm, rhsId);

  allocator.discardStack(masm);
  enterStubFrame(masm, save);

  masm.Push(Imm32(strict));
  masm.Push(val);
  masm.Push(id);
  masm.Push(obj);

  using Fn = bool (*)(JSContext* cx, Handle<NativeObject*> obj, int32_t int_id,
                      HandleValue v, bool strict);
  callVM<Fn, AddOrUpdateSparseElementHelper>(masm);
  return true;
}

bool IonCacheIRCompiler::emitMegamorphicSetElement(ObjOperandId objId,
                                                   ValOperandId idId,
                                                   ValOperandId rhsId,
                                                   bool strict) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoSaveLiveRegisters save(*this);

  Register obj = allocator.useRegister(masm, objId);
  ConstantOrRegister idVal = allocator.useConstantOrRegister(masm, idId);
  ConstantOrRegister val = allocator.useConstantOrRegister(masm, rhsId);

  allocator.discardStack(masm);
  enterStubFrame(masm, save);

  masm.Push(Imm32(strict));
  masm.Push(val);
  masm.Push(idVal);
  masm.Push(obj);

  using Fn = bool (*)(JSContext*, HandleObject, HandleValue, HandleValue, bool);
  callVM<Fn, SetElementMegamorphic<false>>(masm);
  return true;
}

bool IonCacheIRCompiler::emitReturnFromIC() {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  if (!savedLiveRegs_) {
    allocator.restoreInputState(masm);
  }

  uint8_t* rejoinAddr = ic_->rejoinAddr(ionScript_);
  masm.jump(ImmPtr(rejoinAddr));
  return true;
}

bool IonCacheIRCompiler::emitGuardDOMExpandoMissingOrGuardShape(
    ValOperandId expandoId, uint32_t shapeOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  ValueOperand val = allocator.useValueRegister(masm, expandoId);
  Shape* shape = shapeStubField(shapeOffset);

  AutoScratchRegister objScratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Label done;
  masm.branchTestUndefined(Assembler::Equal, val, &done);

  masm.debugAssertIsObject(val);
  masm.unboxObject(val, objScratch);
  // The expando object is not used in this case, so we don't need Spectre
  // mitigations.
  masm.branchTestObjShapeNoSpectreMitigations(Assembler::NotEqual, objScratch,
                                              shape, failure->label());

  masm.bind(&done);
  return true;
}

bool IonCacheIRCompiler::emitLoadDOMExpandoValueGuardGeneration(
    ObjOperandId objId, uint32_t expandoAndGenerationOffset,
    uint32_t generationOffset, ValOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  ExpandoAndGeneration* expandoAndGeneration =
      rawPointerStubField<ExpandoAndGeneration*>(expandoAndGenerationOffset);
  uint64_t generation = rawInt64StubField<uint64_t>(generationOffset);

  ValueOperand output = allocator.defineValueRegister(masm, resultId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadDOMExpandoValueGuardGeneration(obj, output, expandoAndGeneration,
                                          generation, failure->label());
  return true;
}

void IonIC::attachCacheIRStub(JSContext* cx, const CacheIRWriter& writer,
                              CacheKind kind, IonScript* ionScript,
                              bool* attached) {
  // We shouldn't GC or report OOM (or any other exception) here.
  AutoAssertNoPendingException aanpe(cx);
  JS::AutoCheckCannotGC nogc;

  MOZ_ASSERT(!*attached);

  // Do nothing if the IR generator failed or triggered a GC that invalidated
  // the script.
  if (writer.tooLarge()) {
    cx->runtime()->setUseCounter(cx->global(), JSUseCounter::IC_STUB_TOO_LARGE);
    return;
  }
  if (writer.oom()) {
    cx->runtime()->setUseCounter(cx->global(), JSUseCounter::IC_STUB_OOM);
    return;
  }
  MOZ_ASSERT(!writer.failed());

  if (ionScript->invalidated()) {
    return;
  }

  JitZone* jitZone = cx->zone()->jitZone();

  constexpr uint32_t stubDataOffset = sizeof(IonICStub);
  static_assert(stubDataOffset % sizeof(uint64_t) == 0,
                "Stub fields must be aligned");

  // Try to reuse a previously-allocated CacheIRStubInfo.
  CacheIRStubKey::Lookup lookup(kind, ICStubEngine::IonIC, writer.codeStart(),
                                writer.codeLength());
  CacheIRStubInfo* stubInfo = jitZone->getIonCacheIRStubInfo(lookup);
  if (!stubInfo) {
    // Allocate the shared CacheIRStubInfo. Note that the
    // putIonCacheIRStubInfo call below will transfer ownership to
    // the stub info HashSet, so we don't have to worry about freeing
    // it below.

    // For Ion ICs, we don't track/use the makesGCCalls flag, so just pass true.
    bool makesGCCalls = true;
    stubInfo = CacheIRStubInfo::New(kind, ICStubEngine::IonIC, makesGCCalls,
                                    stubDataOffset, writer);
    if (!stubInfo) {
      return;
    }

    CacheIRStubKey key(stubInfo);
    if (!jitZone->putIonCacheIRStubInfo(lookup, key)) {
      return;
    }
  }

  MOZ_ASSERT(stubInfo);

  // Ensure we don't attach duplicate stubs. This can happen if a stub failed
  // for some reason and the IR generator doesn't check for exactly the same
  // conditions.
  for (IonICStub* stub = firstStub_; stub; stub = stub->next()) {
    if (stub->stubInfo() != stubInfo) {
      continue;
    }
    if (!writer.stubDataEquals(stub->stubDataStart())) {
      continue;
    }
    return;
  }

  size_t bytesNeeded = stubInfo->stubDataOffset() + stubInfo->stubDataSize();

  // Allocate the IonICStub in the JitZone's stub space. Ion stubs and
  // CacheIRStubInfo instances for Ion stubs can be purged on GC. That's okay
  // because the stub code is rooted separately when we make a VM call, and
  // stub code should never access the IonICStub after making a VM call. The
  // IonICStub::poison method poisons the stub to catch bugs in this area.
  ICStubSpace* stubSpace = cx->zone()->jitZone()->stubSpace();
  void* newStubMem = stubSpace->alloc(bytesNeeded);
  if (!newStubMem) {
    return;
  }

  IonICStub* newStub =
      new (newStubMem) IonICStub(fallbackAddr(ionScript), stubInfo);
  writer.copyStubData(newStub->stubDataStart());

  TempAllocator temp(&cx->tempLifoAlloc());
  JitContext jctx(cx);
  IonCacheIRCompiler compiler(cx, temp, writer, this, ionScript,
                              stubDataOffset);
  if (!compiler.init()) {
    return;
  }

  JitCode* code = compiler.compile(newStub);
  if (!code) {
    return;
  }

  // Record the stub code if perf spewer is enabled.
  CacheKind stubKind = newStub->stubInfo()->kind();
  compiler.perfSpewer().saveProfile(cx, script(), code,
                                    CacheKindNames[uint8_t(stubKind)]);

  // Add an entry to the profiler's code table, so that the profiler can
  // identify this as Ion code.
  if (ionScript->hasProfilingInstrumentation()) {
    uint8_t* addr = rejoinAddr(ionScript);
    auto entry = MakeJitcodeGlobalEntry<IonICEntry>(cx, code, code->raw(),
                                                    code->rawEnd(), addr);
    if (!entry) {
      cx->recoverFromOutOfMemory();
      return;
    }

    auto* globalTable = cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
    if (!globalTable->addEntry(std::move(entry))) {
      return;
    }
  }

  attachStub(newStub, code);
  *attached = true;
}

bool IonCacheIRCompiler::emitCallStringObjectConcatResult(ValOperandId lhsId,
                                                          ValOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoSaveLiveRegisters save(*this);
  AutoOutputRegister output(*this);

  ValueOperand lhs = allocator.useValueRegister(masm, lhsId);
  ValueOperand rhs = allocator.useValueRegister(masm, rhsId);

  allocator.discardStack(masm);

  enterStubFrame(masm, save);
  masm.Push(rhs);
  masm.Push(lhs);

  using Fn = bool (*)(JSContext*, HandleValue, HandleValue, MutableHandleValue);
  callVM<Fn, DoConcatStringObject>(masm);

  masm.storeCallResultValue(output);
  return true;
}

bool IonCacheIRCompiler::emitCloseIterScriptedResult(ObjOperandId iterId,
                                                     ObjOperandId calleeId,
                                                     CompletionKind kind,
                                                     uint32_t calleeNargs) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoSaveLiveRegisters save(*this);

  Register iter = allocator.useRegister(masm, iterId);
  Register callee = allocator.useRegister(masm, calleeId);

  allocator.discardStack(masm);

  uint32_t framePushedBefore = masm.framePushed();

  // Construct IonICCallFrameLayout.
  enterStubFrame(masm, save);

  uint32_t stubFramePushed = masm.framePushed();

  // The JitFrameLayout pushed below will be aligned to JitStackAlignment,
  // so we just have to make sure the stack is aligned after we push |this|
  // and |calleeNargs| undefined arguments.
  uint32_t argSize = (calleeNargs + 1) * sizeof(Value);
  uint32_t padding =
      ComputeByteAlignment(masm.framePushed() + argSize, JitStackAlignment);
  MOZ_ASSERT(padding % sizeof(uintptr_t) == 0);
  MOZ_ASSERT(padding < JitStackAlignment);
  masm.reserveStack(padding);

  for (uint32_t i = 0; i < calleeNargs; i++) {
    masm.Push(UndefinedValue());
  }
  masm.Push(TypedOrValueRegister(MIRType::Object, AnyRegister(iter)));

  masm.Push(callee);
  masm.PushFrameDescriptorForJitCall(FrameType::IonICCall, /* argc = */ 0);

  masm.loadJitCodeRaw(callee, callee);
  masm.callJit(callee);

  if (kind != CompletionKind::Throw) {
    // Verify that the return value is an object.
    Label success;
    masm.branchTestObject(Assembler::Equal, JSReturnOperand, &success);

    // We can reuse the same stub frame, but we first have to pop the arguments
    // from the previous call.
    uint32_t framePushedAfterCall = masm.framePushed();
    masm.freeStack(masm.framePushed() - stubFramePushed);

    masm.push(Imm32(int32_t(CheckIsObjectKind::IteratorReturn)));
    using Fn = bool (*)(JSContext*, CheckIsObjectKind);
    callVM<Fn, ThrowCheckIsObject>(masm);

    masm.bind(&success);
    masm.setFramePushed(framePushedAfterCall);
  }

  // Restore the frame pointer and stack pointer.
  masm.loadPtr(Address(FramePointer, 0), FramePointer);
  masm.freeStack(masm.framePushed() - framePushedBefore);
  return true;
}

bool IonCacheIRCompiler::emitGuardFunctionScript(ObjOperandId funId,
                                                 uint32_t expectedOffset,
                                                 uint32_t nargsAndFlagsOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register fun = allocator.useRegister(masm, funId);
  AutoScratchRegister scratch(allocator, masm);
  BaseScript* expected = weakBaseScriptStubField(expectedOffset);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadPrivate(Address(fun, JSFunction::offsetOfJitInfoOrScript()),
                   scratch);
  masm.branchPtr(Assembler::NotEqual, scratch, ImmGCPtr(expected),
                 failure->label());
  return true;
}

bool IonCacheIRCompiler::emitCallScriptedFunction(ObjOperandId calleeId,
                                                  Int32OperandId argcId,
                                                  CallFlags flags,
                                                  uint32_t argcFixed) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitCallBoundScriptedFunction(ObjOperandId calleeId,
                                                       ObjOperandId targetId,
                                                       Int32OperandId argcId,
                                                       CallFlags flags,
                                                       uint32_t numBoundArgs) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitCallWasmFunction(
    ObjOperandId calleeId, Int32OperandId argcId, CallFlags flags,
    uint32_t argcFixed, uint32_t funcExportOffset, uint32_t instanceOffset) {
  MOZ_CRASH("Call ICs not used in ion");
}

#ifdef JS_SIMULATOR
bool IonCacheIRCompiler::emitCallNativeFunction(ObjOperandId calleeId,
                                                Int32OperandId argcId,
                                                CallFlags flags,
                                                uint32_t argcFixed,
                                                uint32_t targetOffset) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitCallDOMFunction(
    ObjOperandId calleeId, Int32OperandId argcId, ObjOperandId thisObjId,
    CallFlags flags, uint32_t argcFixed, uint32_t targetOffset) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitCallDOMFunctionWithAllocSite(
    ObjOperandId calleeId, Int32OperandId argcId, ObjOperandId thisObjId,
    CallFlags flags, uint32_t argcFixed, uint32_t siteOffset,
    uint32_t targetOffset) {
  MOZ_CRASH("Call ICs not used in ion");
}
#else
bool IonCacheIRCompiler::emitCallNativeFunction(ObjOperandId calleeId,
                                                Int32OperandId argcId,
                                                CallFlags flags,
                                                uint32_t argcFixed,
                                                bool ignoresReturnValue) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitCallDOMFunction(ObjOperandId calleeId,
                                             Int32OperandId argcId,
                                             ObjOperandId thisObjId,
                                             CallFlags flags,
                                             uint32_t argcFixed) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitCallDOMFunctionWithAllocSite(
    ObjOperandId calleeId, Int32OperandId argcId, ObjOperandId thisObjId,
    CallFlags flags, uint32_t argcFixed, uint32_t siteOffset) {
  MOZ_CRASH("Call ICs not used in ion");
}
#endif

bool IonCacheIRCompiler::emitCallClassHook(ObjOperandId calleeId,
                                           Int32OperandId argcId,
                                           CallFlags flags, uint32_t argcFixed,
                                           uint32_t targetOffset) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitCallInlinedFunction(ObjOperandId calleeId,
                                                 Int32OperandId argcId,
                                                 uint32_t icScriptOffset,
                                                 CallFlags flags,
                                                 uint32_t argcFixed) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitBindFunctionResult(ObjOperandId targetId,
                                                uint32_t argc,
                                                uint32_t templateObjectOffset) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitSpecializedBindFunctionResult(
    ObjOperandId targetId, uint32_t argc, uint32_t templateObjectOffset) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitLoadArgumentFixedSlot(ValOperandId resultId,
                                                   uint8_t slotIndex) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitLoadArgumentDynamicSlot(ValOperandId resultId,
                                                     Int32OperandId argcId,
                                                     uint8_t slotIndex) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitArrayJoinResult(ObjOperandId objId,
                                             StringOperandId sepId) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitPackedArraySliceResult(
    uint32_t templateObjectOffset, ObjOperandId arrayId, Int32OperandId beginId,
    Int32OperandId endId) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitArgumentsSliceResult(uint32_t templateObjectOffset,
                                                  ObjOperandId argsId,
                                                  Int32OperandId beginId,
                                                  Int32OperandId endId) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitIsArrayResult(ValOperandId inputId) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitIsTypedArrayResult(ObjOperandId objId,
                                                bool isPossiblyWrapped) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitStringFromCharCodeResult(Int32OperandId codeId) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitStringFromCodePointResult(Int32OperandId codeId) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitMathRandomResult(uint32_t rngOffset) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitReflectGetPrototypeOfResult(ObjOperandId objId) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitHasClassResult(ObjOperandId objId,
                                            uint32_t claspOffset) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitHasShapeResult(ObjOperandId objId,
                                            uint32_t shapeOffset) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitSameValueResult(ValOperandId lhs,
                                             ValOperandId rhs) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitSetHasStringResult(ObjOperandId setId,
                                                StringOperandId strId) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitMapHasStringResult(ObjOperandId mapId,
                                                StringOperandId strId) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitMapGetStringResult(ObjOperandId mapId,
                                                StringOperandId strId) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitNewArrayObjectResult(uint32_t arrayLength,
                                                  uint32_t shapeOffset,
                                                  uint32_t siteOffset) {
  MOZ_CRASH("NewArray ICs not used in ion");
}

bool IonCacheIRCompiler::emitNewPlainObjectResult(uint32_t numFixedSlots,
                                                  uint32_t numDynamicSlots,
                                                  gc::AllocKind allocKind,
                                                  uint32_t shapeOffset,
                                                  uint32_t siteOffset) {
  MOZ_CRASH("NewObject ICs not used in ion");
}

bool IonCacheIRCompiler::emitNewFunctionCloneResult(uint32_t canonicalOffset,
                                                    gc::AllocKind allocKind,
                                                    uint32_t siteOffset) {
  MOZ_CRASH("Lambda ICs not used in ion");
}

bool IonCacheIRCompiler::emitCallRegExpMatcherResult(ObjOperandId regexpId,
                                                     StringOperandId inputId,
                                                     Int32OperandId lastIndexId,
                                                     uint32_t stubOffset) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitCallRegExpSearcherResult(
    ObjOperandId regexpId, StringOperandId inputId, Int32OperandId lastIndexId,
    uint32_t stubOffset) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitRegExpBuiltinExecMatchResult(
    ObjOperandId regexpId, StringOperandId inputId, uint32_t stubOffset) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitRegExpBuiltinExecTestResult(
    ObjOperandId regexpId, StringOperandId inputId, uint32_t stubOffset) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitRegExpHasCaptureGroupsResult(
    ObjOperandId regexpId, StringOperandId inputId) {
  MOZ_CRASH("Call ICs not used in ion");
}

bool IonCacheIRCompiler::emitLoadStringAtResult(StringOperandId strId,
                                                Int32OperandId indexId,
                                                bool handleOOB) {
  MOZ_CRASH("Call ICs not used in ion");
}
