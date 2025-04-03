/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineCacheIRCompiler.h"

#include "gc/GC.h"
#include "jit/CacheIR.h"
#include "jit/CacheIRCloner.h"
#include "jit/CacheIRWriter.h"
#include "jit/JitFrames.h"
#include "jit/JitRealm.h"
#include "jit/JitRuntime.h"
#include "jit/JitZone.h"
#include "jit/Linker.h"
#include "jit/MoveEmitter.h"
#include "jit/RegExpStubConstants.h"
#include "jit/SharedICHelpers.h"
#include "jit/VMFunctions.h"
#include "js/experimental/JitInfo.h"  // JSJitInfo
#include "js/friend/DOMProxy.h"       // JS::ExpandoAndGeneration
#include "proxy/DeadObjectProxy.h"
#include "proxy/Proxy.h"
#include "util/Unicode.h"
#include "vm/JSAtom.h"
#include "vm/StaticStrings.h"

#include "jit/JitScript-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "jit/SharedICHelpers-inl.h"
#include "jit/VMFunctionList-inl.h"
#include "vm/List-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::Maybe;

using JS::ExpandoAndGeneration;

namespace js {
namespace jit {

Address CacheRegisterAllocator::addressOf(MacroAssembler& masm,
                                          BaselineFrameSlot slot) const {
  uint32_t offset =
      stackPushed_ + ICStackValueOffset + slot.slot() * sizeof(JS::Value);
  if (JitOptions.enableICFramePointers) {
    // The frame pointer is also on the stack.
    offset += sizeof(uintptr_t);
  }
  return Address(masm.getStackPointer(), offset);
}
BaseValueIndex CacheRegisterAllocator::addressOf(MacroAssembler& masm,
                                                 Register argcReg,
                                                 BaselineFrameSlot slot) const {
  uint32_t offset =
      stackPushed_ + ICStackValueOffset + slot.slot() * sizeof(JS::Value);
  if (JitOptions.enableICFramePointers) {
    // The frame pointer is also on the stack.
    offset += sizeof(uintptr_t);
  }
  return BaseValueIndex(masm.getStackPointer(), argcReg, offset);
}

// BaselineCacheIRCompiler compiles CacheIR to BaselineIC native code.
BaselineCacheIRCompiler::BaselineCacheIRCompiler(JSContext* cx,
                                                 TempAllocator& alloc,
                                                 const CacheIRWriter& writer,
                                                 uint32_t stubDataOffset)
    : CacheIRCompiler(cx, alloc, writer, stubDataOffset, Mode::Baseline,
                      StubFieldPolicy::Address),
      makesGCCalls_(false) {}

// AutoStubFrame methods
AutoStubFrame::AutoStubFrame(BaselineCacheIRCompiler& compiler)
    : compiler(compiler)
#ifdef DEBUG
      ,
      framePushedAtEnterStubFrame_(0)
#endif
{
}
void AutoStubFrame::enter(MacroAssembler& masm, Register scratch,
                          CallCanGC canGC) {
  MOZ_ASSERT(compiler.allocator.stackPushed() == 0);

  if (JitOptions.enableICFramePointers) {
    // If we have already pushed the frame pointer, pop it
    // before creating the stub frame.
    masm.pop(FramePointer);
  }
  EmitBaselineEnterStubFrame(masm, scratch);

#ifdef DEBUG
  framePushedAtEnterStubFrame_ = masm.framePushed();
#endif

  MOZ_ASSERT(!compiler.enteredStubFrame_);
  compiler.enteredStubFrame_ = true;
  if (canGC == CallCanGC::CanGC) {
    compiler.makesGCCalls_ = true;
  }
}
void AutoStubFrame::leave(MacroAssembler& masm) {
  MOZ_ASSERT(compiler.enteredStubFrame_);
  compiler.enteredStubFrame_ = false;

#ifdef DEBUG
  masm.setFramePushed(framePushedAtEnterStubFrame_);
#endif

  EmitBaselineLeaveStubFrame(masm);
  if (JitOptions.enableICFramePointers) {
    // We will pop the frame pointer when we return,
    // so we have to push it again now.
    masm.push(FramePointer);
  }
}

#ifdef DEBUG
AutoStubFrame::~AutoStubFrame() { MOZ_ASSERT(!compiler.enteredStubFrame_); }
#endif

}  // namespace jit
}  // namespace js

bool BaselineCacheIRCompiler::makesGCCalls() const { return makesGCCalls_; }

Address BaselineCacheIRCompiler::stubAddress(uint32_t offset) const {
  return Address(ICStubReg, stubDataOffset_ + offset);
}

template <typename Fn, Fn fn>
void BaselineCacheIRCompiler::callVM(MacroAssembler& masm) {
  VMFunctionId id = VMFunctionToId<Fn, fn>::id;
  callVMInternal(masm, id);
}

JitCode* BaselineCacheIRCompiler::compile() {
  AutoCreatedBy acb(masm, "BaselineCacheIRCompiler::compile");

#ifndef JS_USE_LINK_REGISTER
  masm.adjustFrame(sizeof(intptr_t));
#endif
#ifdef JS_CODEGEN_ARM
  masm.setSecondScratchReg(BaselineSecondScratchReg);
#endif
  if (JitOptions.enableICFramePointers) {
    /* [SMDOC] Baseline IC Frame Pointers
     *
     *  In general, ICs don't have frame pointers until just before
     *  doing a VM call, at which point we retroactively create a stub
     *  frame. However, for the sake of external profilers, we
     *  optionally support full-IC frame pointers in baseline ICs, with
     *  the following approach:
     *    1. We push a frame pointer when we enter an IC.
     *    2. We pop the frame pointer when we return from an IC, or
     *       when we jump to the next IC.
     *    3. Entering a stub frame for a VM call already pushes a
     *       frame pointer, so we pop our existing frame pointer
     *       just before entering a stub frame and push it again
     *       just after leaving a stub frame.
     *  Some ops take advantage of the fact that the frame pointer is
     *  not updated until we enter a stub frame to read values from
     *  the caller's frame. To support this, we allocate a separate
     *  baselineFrame register when IC frame pointers are enabled.
     */
    masm.push(FramePointer);
    masm.moveStackPtrTo(FramePointer);

    MOZ_ASSERT(baselineFrameReg() != FramePointer);
    masm.loadPtr(Address(FramePointer, 0), baselineFrameReg());
  }

  // Count stub entries: We count entries rather than successes as it much
  // easier to ensure ICStubReg is valid at entry than at exit.
  Address enteredCount(ICStubReg, ICCacheIRStub::offsetOfEnteredCount());
  masm.add32(Imm32(1), enteredCount);

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

  MOZ_ASSERT(!enteredStubFrame_);
  masm.assumeUnreachable("Should have returned from IC");

  // Done emitting the main IC code. Now emit the failure paths.
  for (size_t i = 0; i < failurePaths.length(); i++) {
    if (!emitFailurePath(i)) {
      return nullptr;
    }
    if (JitOptions.enableICFramePointers) {
      masm.pop(FramePointer);
    }
    EmitStubGuardFailure(masm);
  }

  Linker linker(masm);
  Rooted<JitCode*> newStubCode(cx_, linker.newCode(cx_, CodeKind::Baseline));
  if (!newStubCode) {
    cx_->recoverFromOutOfMemory();
    return nullptr;
  }

  return newStubCode;
}

bool BaselineCacheIRCompiler::emitGuardShape(ObjOperandId objId,
                                             uint32_t shapeOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch1(allocator, masm);

  bool needSpectreMitigations = objectGuardNeedsSpectreMitigations(objId);

  Maybe<AutoScratchRegister> maybeScratch2;
  if (needSpectreMitigations) {
    maybeScratch2.emplace(allocator, masm);
  }

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Address addr(stubAddress(shapeOffset));
  masm.loadPtr(addr, scratch1);
  if (needSpectreMitigations) {
    masm.branchTestObjShape(Assembler::NotEqual, obj, scratch1, *maybeScratch2,
                            obj, failure->label());
  } else {
    masm.branchTestObjShapeNoSpectreMitigations(Assembler::NotEqual, obj,
                                                scratch1, failure->label());
  }

  return true;
}

bool BaselineCacheIRCompiler::emitGuardProto(ObjOperandId objId,
                                             uint32_t protoOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Address addr(stubAddress(protoOffset));
  masm.loadObjProto(obj, scratch);
  masm.branchPtr(Assembler::NotEqual, addr, scratch, failure->label());
  return true;
}

bool BaselineCacheIRCompiler::emitGuardCompartment(ObjOperandId objId,
                                                   uint32_t globalOffset,
                                                   uint32_t compartmentOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Verify that the global wrapper is still valid, as
  // it is pre-requisite for doing the compartment check.
  Address globalWrapper(stubAddress(globalOffset));
  masm.loadPtr(globalWrapper, scratch);
  Address handlerAddr(scratch, ProxyObject::offsetOfHandler());
  masm.branchPtr(Assembler::Equal, handlerAddr,
                 ImmPtr(&DeadObjectProxy::singleton), failure->label());

  Address addr(stubAddress(compartmentOffset));
  masm.branchTestObjCompartment(Assembler::NotEqual, obj, addr, scratch,
                                failure->label());
  return true;
}

bool BaselineCacheIRCompiler::emitGuardAnyClass(ObjOperandId objId,
                                                uint32_t claspOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Address testAddr(stubAddress(claspOffset));
  if (objectGuardNeedsSpectreMitigations(objId)) {
    masm.branchTestObjClass(Assembler::NotEqual, obj, testAddr, scratch, obj,
                            failure->label());
  } else {
    masm.branchTestObjClassNoSpectreMitigations(
        Assembler::NotEqual, obj, testAddr, scratch, failure->label());
  }

  return true;
}

bool BaselineCacheIRCompiler::emitGuardHasProxyHandler(ObjOperandId objId,
                                                       uint32_t handlerOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Address testAddr(stubAddress(handlerOffset));
  masm.loadPtr(testAddr, scratch);

  Address handlerAddr(obj, ProxyObject::offsetOfHandler());
  masm.branchPtr(Assembler::NotEqual, handlerAddr, scratch, failure->label());
  return true;
}

bool BaselineCacheIRCompiler::emitGuardSpecificObject(ObjOperandId objId,
                                                      uint32_t expectedOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Address addr(stubAddress(expectedOffset));
  masm.branchPtr(Assembler::NotEqual, addr, obj, failure->label());
  return true;
}

bool BaselineCacheIRCompiler::emitGuardSpecificFunction(
    ObjOperandId objId, uint32_t expectedOffset, uint32_t nargsAndFlagsOffset) {
  return emitGuardSpecificObject(objId, expectedOffset);
}

bool BaselineCacheIRCompiler::emitGuardFunctionScript(
    ObjOperandId funId, uint32_t expectedOffset, uint32_t nargsAndFlagsOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register fun = allocator.useRegister(masm, funId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Address addr(stubAddress(expectedOffset));
  masm.loadPrivate(Address(fun, JSFunction::offsetOfJitInfoOrScript()),
                   scratch);
  masm.branchPtr(Assembler::NotEqual, addr, scratch, failure->label());
  return true;
}

bool BaselineCacheIRCompiler::emitGuardSpecificAtom(StringOperandId strId,
                                                    uint32_t expectedOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register str = allocator.useRegister(masm, strId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Address atomAddr(stubAddress(expectedOffset));

  Label done;
  masm.branchPtr(Assembler::Equal, atomAddr, str, &done);

  // The pointers are not equal, so if the input string is also an atom it
  // must be a different string.
  masm.branchTest32(Assembler::NonZero, Address(str, JSString::offsetOfFlags()),
                    Imm32(JSString::ATOM_BIT), failure->label());

  // Check the length.
  masm.loadPtr(atomAddr, scratch);
  masm.loadStringLength(scratch, scratch);
  masm.branch32(Assembler::NotEqual, Address(str, JSString::offsetOfLength()),
                scratch, failure->label());

  // We have a non-atomized string with the same length. Call a helper
  // function to do the comparison.
  LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                               liveVolatileFloatRegs());
  masm.PushRegsInMask(volatileRegs);

  using Fn = bool (*)(JSString* str1, JSString* str2);
  masm.setupUnalignedABICall(scratch);
  masm.loadPtr(atomAddr, scratch);
  masm.passABIArg(scratch);
  masm.passABIArg(str);
  masm.callWithABI<Fn, EqualStringsHelperPure>();
  masm.storeCallPointerResult(scratch);

  LiveRegisterSet ignore;
  ignore.add(scratch);
  masm.PopRegsInMaskIgnore(volatileRegs, ignore);
  masm.branchIfFalseBool(scratch, failure->label());

  masm.bind(&done);
  return true;
}

bool BaselineCacheIRCompiler::emitGuardSpecificSymbol(SymbolOperandId symId,
                                                      uint32_t expectedOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register sym = allocator.useRegister(masm, symId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Address addr(stubAddress(expectedOffset));
  masm.branchPtr(Assembler::NotEqual, addr, sym, failure->label());
  return true;
}

bool BaselineCacheIRCompiler::emitLoadValueResult(uint32_t valOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  masm.loadValue(stubAddress(valOffset), output.valueReg());
  return true;
}

bool BaselineCacheIRCompiler::emitLoadFixedSlotResult(ObjOperandId objId,
                                                      uint32_t offsetOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  masm.load32(stubAddress(offsetOffset), scratch);
  masm.loadValue(BaseIndex(obj, scratch, TimesOne), output.valueReg());
  return true;
}

bool BaselineCacheIRCompiler::emitLoadFixedSlotTypedResult(
    ObjOperandId objId, uint32_t offsetOffset, ValueType) {
  // The type is only used by Warp.
  return emitLoadFixedSlotResult(objId, offsetOffset);
}

bool BaselineCacheIRCompiler::emitLoadDynamicSlotResult(ObjOperandId objId,
                                                        uint32_t offsetOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  AutoScratchRegister scratch2(allocator, masm);

  masm.load32(stubAddress(offsetOffset), scratch);
  masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), scratch2);
  masm.loadValue(BaseIndex(scratch2, scratch, TimesOne), output.valueReg());
  return true;
}

bool BaselineCacheIRCompiler::emitCallScriptedGetterShared(
    ValOperandId receiverId, uint32_t getterOffset, bool sameRealm,
    uint32_t nargsAndFlagsOffset, Maybe<uint32_t> icScriptOffset) {
  ValueOperand receiver = allocator.useValueRegister(masm, receiverId);
  Address getterAddr(stubAddress(getterOffset));

  AutoScratchRegister code(allocator, masm);
  AutoScratchRegister callee(allocator, masm);
  AutoScratchRegister scratch(allocator, masm);

  bool isInlined = icScriptOffset.isSome();

  // First, retrieve raw jitcode for getter.
  masm.loadPtr(getterAddr, callee);
  if (isInlined) {
    FailurePath* failure;
    if (!addFailurePath(&failure)) {
      return false;
    }
    masm.loadBaselineJitCodeRaw(callee, code, failure->label());
  } else {
    masm.loadJitCodeRaw(callee, code);
  }

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  if (!sameRealm) {
    masm.switchToObjectRealm(callee, scratch);
  }

  // Align the stack such that the JitFrameLayout is aligned on
  // JitStackAlignment.
  masm.alignJitStackBasedOnNArgs(0, /*countIncludesThis = */ false);

  // Getter is called with 0 arguments, just |receiver| as thisv.
  // Note that we use Push, not push, so that callJit will align the stack
  // properly on ARM.
  masm.Push(receiver);

  if (isInlined) {
    // Store icScript in the context.
    Address icScriptAddr(stubAddress(*icScriptOffset));
    masm.loadPtr(icScriptAddr, scratch);
    masm.storeICScriptInJSContext(scratch);
  }

  masm.Push(callee);
  masm.PushFrameDescriptorForJitCall(FrameType::BaselineStub, /* argc = */ 0);

  // Handle arguments underflow.
  Label noUnderflow;
  masm.loadFunctionArgCount(callee, callee);
  masm.branch32(Assembler::Equal, callee, Imm32(0), &noUnderflow);

  // Call the arguments rectifier.
  ArgumentsRectifierKind kind = isInlined
                                    ? ArgumentsRectifierKind::TrialInlining
                                    : ArgumentsRectifierKind::Normal;
  TrampolinePtr argumentsRectifier =
      cx_->runtime()->jitRuntime()->getArgumentsRectifier(kind);
  masm.movePtr(argumentsRectifier, code);

  masm.bind(&noUnderflow);
  masm.callJit(code);

  stubFrame.leave(masm);

  if (!sameRealm) {
    masm.switchToBaselineFrameRealm(R1.scratchReg());
  }

  return true;
}

bool BaselineCacheIRCompiler::emitCallScriptedGetterResult(
    ValOperandId receiverId, uint32_t getterOffset, bool sameRealm,
    uint32_t nargsAndFlagsOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Maybe<uint32_t> icScriptOffset = mozilla::Nothing();
  return emitCallScriptedGetterShared(receiverId, getterOffset, sameRealm,
                                      nargsAndFlagsOffset, icScriptOffset);
}

bool BaselineCacheIRCompiler::emitCallInlinedGetterResult(
    ValOperandId receiverId, uint32_t getterOffset, uint32_t icScriptOffset,
    bool sameRealm, uint32_t nargsAndFlagsOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  return emitCallScriptedGetterShared(receiverId, getterOffset, sameRealm,
                                      nargsAndFlagsOffset,
                                      mozilla::Some(icScriptOffset));
}

bool BaselineCacheIRCompiler::emitCallNativeGetterResult(
    ValOperandId receiverId, uint32_t getterOffset, bool sameRealm,
    uint32_t nargsAndFlagsOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  ValueOperand receiver = allocator.useValueRegister(masm, receiverId);
  Address getterAddr(stubAddress(getterOffset));

  AutoScratchRegister scratch(allocator, masm);

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  // Load the callee in the scratch register.
  masm.loadPtr(getterAddr, scratch);

  masm.Push(receiver);
  masm.Push(scratch);

  using Fn =
      bool (*)(JSContext*, HandleFunction, HandleValue, MutableHandleValue);
  callVM<Fn, CallNativeGetter>(masm);

  stubFrame.leave(masm);
  return true;
}

bool BaselineCacheIRCompiler::emitCallDOMGetterResult(ObjOperandId objId,
                                                      uint32_t jitInfoOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register obj = allocator.useRegister(masm, objId);
  Address jitInfoAddr(stubAddress(jitInfoOffset));

  AutoScratchRegister scratch(allocator, masm);

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  // Load the JSJitInfo in the scratch register.
  masm.loadPtr(jitInfoAddr, scratch);

  masm.Push(obj);
  masm.Push(scratch);

  using Fn =
      bool (*)(JSContext*, const JSJitInfo*, HandleObject, MutableHandleValue);
  callVM<Fn, CallDOMGetter>(masm);

  stubFrame.leave(masm);
  return true;
}

bool BaselineCacheIRCompiler::emitProxyGetResult(ObjOperandId objId,
                                                 uint32_t idOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  Address idAddr(stubAddress(idOffset));

  AutoScratchRegister scratch(allocator, masm);

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  // Load the jsid in the scratch register.
  masm.loadPtr(idAddr, scratch);

  masm.Push(scratch);
  masm.Push(obj);

  using Fn = bool (*)(JSContext*, HandleObject, HandleId, MutableHandleValue);
  callVM<Fn, ProxyGetProperty>(masm);

  stubFrame.leave(masm);
  return true;
}

bool BaselineCacheIRCompiler::emitFrameIsConstructingResult() {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register outputScratch = output.valueReg().scratchReg();

  // Load the CalleeToken.
  Address tokenAddr(baselineFrameReg(), JitFrameLayout::offsetOfCalleeToken());
  masm.loadPtr(tokenAddr, outputScratch);

  // The low bit indicates whether this call is constructing, just clear the
  // other bits.
  static_assert(CalleeToken_Function == 0x0);
  static_assert(CalleeToken_FunctionConstructing == 0x1);
  masm.andPtr(Imm32(0x1), outputScratch);

  masm.tagValue(JSVAL_TYPE_BOOLEAN, outputScratch, output.valueReg());
  return true;
}

bool BaselineCacheIRCompiler::emitLoadConstantStringResult(uint32_t strOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  masm.loadPtr(stubAddress(strOffset), scratch);
  masm.tagValue(JSVAL_TYPE_STRING, scratch, output.valueReg());
  return true;
}

bool BaselineCacheIRCompiler::emitCompareStringResult(JSOp op,
                                                      StringOperandId lhsId,
                                                      StringOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);

  Register left = allocator.useRegister(masm, lhsId);
  Register right = allocator.useRegister(masm, rhsId);

  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  allocator.discardStack(masm);

  Label slow, done;
  masm.compareStrings(op, left, right, scratch, &slow);
  masm.jump(&done);
  masm.bind(&slow);
  {
    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch);

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

    stubFrame.leave(masm);
    masm.storeCallPointerResult(scratch);
  }
  masm.bind(&done);
  masm.tagValue(JSVAL_TYPE_BOOLEAN, scratch, output.valueReg());
  return true;
}

bool BaselineCacheIRCompiler::emitSameValueResult(ValOperandId lhsId,
                                                  ValOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegister scratch(allocator, masm);
  ValueOperand lhs = allocator.useValueRegister(masm, lhsId);
#ifdef JS_CODEGEN_X86
  // Use the output to avoid running out of registers.
  allocator.copyToScratchValueRegister(masm, rhsId, output.valueReg());
  ValueOperand rhs = output.valueReg();
#else
  ValueOperand rhs = allocator.useValueRegister(masm, rhsId);
#endif

  allocator.discardStack(masm);

  Label done;
  Label call;

  // Check to see if the values have identical bits.
  // This is correct for SameValue because SameValue(NaN,NaN) is true,
  // and SameValue(0,-0) is false.
  masm.branch64(Assembler::NotEqual, lhs.toRegister64(), rhs.toRegister64(),
                &call);
  masm.moveValue(BooleanValue(true), output.valueReg());
  masm.jump(&done);

  {
    masm.bind(&call);

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch);

    masm.pushValue(lhs);
    masm.pushValue(rhs);

    using Fn = bool (*)(JSContext*, HandleValue, HandleValue, bool*);
    callVM<Fn, SameValue>(masm);

    stubFrame.leave(masm);
    masm.tagValue(JSVAL_TYPE_BOOLEAN, ReturnReg, output.valueReg());
  }

  masm.bind(&done);
  return true;
}

bool BaselineCacheIRCompiler::emitStoreSlotShared(bool isFixed,
                                                  ObjOperandId objId,
                                                  uint32_t offsetOffset,
                                                  ValOperandId rhsId) {
  Register obj = allocator.useRegister(masm, objId);
  ValueOperand val = allocator.useValueRegister(masm, rhsId);

  AutoScratchRegister scratch1(allocator, masm);
  Maybe<AutoScratchRegister> scratch2;
  if (!isFixed) {
    scratch2.emplace(allocator, masm);
  }

  Address offsetAddr = stubAddress(offsetOffset);
  masm.load32(offsetAddr, scratch1);

  if (isFixed) {
    BaseIndex slot(obj, scratch1, TimesOne);
    EmitPreBarrier(masm, slot, MIRType::Value);
    masm.storeValue(val, slot);
  } else {
    masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), scratch2.ref());
    BaseIndex slot(scratch2.ref(), scratch1, TimesOne);
    EmitPreBarrier(masm, slot, MIRType::Value);
    masm.storeValue(val, slot);
  }

  emitPostBarrierSlot(obj, val, scratch1);
  return true;
}

bool BaselineCacheIRCompiler::emitStoreFixedSlot(ObjOperandId objId,
                                                 uint32_t offsetOffset,
                                                 ValOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  return emitStoreSlotShared(true, objId, offsetOffset, rhsId);
}

bool BaselineCacheIRCompiler::emitStoreDynamicSlot(ObjOperandId objId,
                                                   uint32_t offsetOffset,
                                                   ValOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  return emitStoreSlotShared(false, objId, offsetOffset, rhsId);
}

bool BaselineCacheIRCompiler::emitAddAndStoreSlotShared(
    CacheOp op, ObjOperandId objId, uint32_t offsetOffset, ValOperandId rhsId,
    uint32_t newShapeOffset, Maybe<uint32_t> numNewSlotsOffset) {
  Register obj = allocator.useRegister(masm, objId);
  ValueOperand val = allocator.useValueRegister(masm, rhsId);

  AutoScratchRegister scratch1(allocator, masm);
  AutoScratchRegister scratch2(allocator, masm);

  Address newShapeAddr = stubAddress(newShapeOffset);
  Address offsetAddr = stubAddress(offsetOffset);

  if (op == CacheOp::AllocateAndStoreDynamicSlot) {
    // We have to (re)allocate dynamic slots. Do this first, as it's the
    // only fallible operation here. Note that growSlotsPure is fallible but
    // does not GC.
    Address numNewSlotsAddr = stubAddress(*numNewSlotsOffset);

    FailurePath* failure;
    if (!addFailurePath(&failure)) {
      return false;
    }

    LiveRegisterSet save(GeneralRegisterSet::Volatile(),
                         liveVolatileFloatRegs());
    masm.PushRegsInMask(save);

    using Fn = bool (*)(JSContext* cx, NativeObject* obj, uint32_t newCount);
    masm.setupUnalignedABICall(scratch1);
    masm.loadJSContext(scratch1);
    masm.passABIArg(scratch1);
    masm.passABIArg(obj);
    masm.load32(numNewSlotsAddr, scratch2);
    masm.passABIArg(scratch2);
    masm.callWithABI<Fn, NativeObject::growSlotsPure>();
    masm.storeCallPointerResult(scratch1);

    LiveRegisterSet ignore;
    ignore.add(scratch1);
    masm.PopRegsInMaskIgnore(save, ignore);

    masm.branchIfFalseBool(scratch1, failure->label());
  }

  // Update the object's shape.
  masm.loadPtr(newShapeAddr, scratch1);
  masm.storeObjShape(scratch1, obj,
                     [](MacroAssembler& masm, const Address& addr) {
                       EmitPreBarrier(masm, addr, MIRType::Shape);
                     });

  // Perform the store. No pre-barrier required since this is a new
  // initialization.
  masm.load32(offsetAddr, scratch1);
  if (op == CacheOp::AddAndStoreFixedSlot) {
    BaseIndex slot(obj, scratch1, TimesOne);
    masm.storeValue(val, slot);
  } else {
    MOZ_ASSERT(op == CacheOp::AddAndStoreDynamicSlot ||
               op == CacheOp::AllocateAndStoreDynamicSlot);
    masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), scratch2);
    BaseIndex slot(scratch2, scratch1, TimesOne);
    masm.storeValue(val, slot);
  }

  emitPostBarrierSlot(obj, val, scratch1);
  return true;
}

bool BaselineCacheIRCompiler::emitAddAndStoreFixedSlot(
    ObjOperandId objId, uint32_t offsetOffset, ValOperandId rhsId,
    uint32_t newShapeOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Maybe<uint32_t> numNewSlotsOffset = mozilla::Nothing();
  return emitAddAndStoreSlotShared(CacheOp::AddAndStoreFixedSlot, objId,
                                   offsetOffset, rhsId, newShapeOffset,
                                   numNewSlotsOffset);
}

bool BaselineCacheIRCompiler::emitAddAndStoreDynamicSlot(
    ObjOperandId objId, uint32_t offsetOffset, ValOperandId rhsId,
    uint32_t newShapeOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Maybe<uint32_t> numNewSlotsOffset = mozilla::Nothing();
  return emitAddAndStoreSlotShared(CacheOp::AddAndStoreDynamicSlot, objId,
                                   offsetOffset, rhsId, newShapeOffset,
                                   numNewSlotsOffset);
}

bool BaselineCacheIRCompiler::emitAllocateAndStoreDynamicSlot(
    ObjOperandId objId, uint32_t offsetOffset, ValOperandId rhsId,
    uint32_t newShapeOffset, uint32_t numNewSlotsOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  return emitAddAndStoreSlotShared(CacheOp::AllocateAndStoreDynamicSlot, objId,
                                   offsetOffset, rhsId, newShapeOffset,
                                   mozilla::Some(numNewSlotsOffset));
}

bool BaselineCacheIRCompiler::emitArrayJoinResult(ObjOperandId objId,
                                                  StringOperandId sepId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  Register sep = allocator.useRegister(masm, sepId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  allocator.discardStack(masm);

  // Load obj->elements in scratch.
  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);
  Address lengthAddr(scratch, ObjectElements::offsetOfLength());

  // If array length is 0, return empty string.
  Label finished;

  {
    Label arrayNotEmpty;
    masm.branch32(Assembler::NotEqual, lengthAddr, Imm32(0), &arrayNotEmpty);
    masm.movePtr(ImmGCPtr(cx_->names().empty), scratch);
    masm.tagValue(JSVAL_TYPE_STRING, scratch, output.valueReg());
    masm.jump(&finished);
    masm.bind(&arrayNotEmpty);
  }

  Label vmCall;

  // Otherwise, handle array length 1 case.
  masm.branch32(Assembler::NotEqual, lengthAddr, Imm32(1), &vmCall);

  // But only if initializedLength is also 1.
  Address initLength(scratch, ObjectElements::offsetOfInitializedLength());
  masm.branch32(Assembler::NotEqual, initLength, Imm32(1), &vmCall);

  // And only if elem0 is a string.
  Address elementAddr(scratch, 0);
  masm.branchTestString(Assembler::NotEqual, elementAddr, &vmCall);

  // Store the value.
  masm.loadValue(elementAddr, output.valueReg());
  masm.jump(&finished);

  // Otherwise call into the VM.
  {
    masm.bind(&vmCall);

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch);

    masm.Push(sep);
    masm.Push(obj);

    using Fn = JSString* (*)(JSContext*, HandleObject, HandleString);
    callVM<Fn, jit::ArrayJoin>(masm);

    stubFrame.leave(masm);

    masm.tagValue(JSVAL_TYPE_STRING, ReturnReg, output.valueReg());
  }

  masm.bind(&finished);

  return true;
}

bool BaselineCacheIRCompiler::emitPackedArraySliceResult(
    uint32_t templateObjectOffset, ObjOperandId arrayId, Int32OperandId beginId,
    Int32OperandId endId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch1(allocator, masm, output);
  AutoScratchRegisterMaybeOutputType scratch2(allocator, masm, output);

  Register array = allocator.useRegister(masm, arrayId);
  Register begin = allocator.useRegister(masm, beginId);
  Register end = allocator.useRegister(masm, endId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branchArrayIsNotPacked(array, scratch1, scratch2, failure->label());

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch1);

  // Don't attempt to pre-allocate the object, instead always use the slow
  // path.
  ImmPtr result(nullptr);

  masm.Push(result);
  masm.Push(end);
  masm.Push(begin);
  masm.Push(array);

  using Fn =
      JSObject* (*)(JSContext*, HandleObject, int32_t, int32_t, HandleObject);
  callVM<Fn, ArraySliceDense>(masm);

  stubFrame.leave(masm);

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, output.valueReg());
  return true;
}

bool BaselineCacheIRCompiler::emitArgumentsSliceResult(
    uint32_t templateObjectOffset, ObjOperandId argsId, Int32OperandId beginId,
    Int32OperandId endId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  Register args = allocator.useRegister(masm, argsId);
  Register begin = allocator.useRegister(masm, beginId);
  Register end = allocator.useRegister(masm, endId);

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  // Don't attempt to pre-allocate the object, instead always use the slow path.
  ImmPtr result(nullptr);

  masm.Push(result);
  masm.Push(end);
  masm.Push(begin);
  masm.Push(args);

  using Fn =
      JSObject* (*)(JSContext*, HandleObject, int32_t, int32_t, HandleObject);
  callVM<Fn, ArgumentsSliceDense>(masm);

  stubFrame.leave(masm);

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, output.valueReg());
  return true;
}

bool BaselineCacheIRCompiler::emitIsArrayResult(ValOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegister scratch1(allocator, masm);
  AutoScratchRegisterMaybeOutput scratch2(allocator, masm, output);

  ValueOperand val = allocator.useValueRegister(masm, inputId);

  allocator.discardStack(masm);

  Label isNotArray;
  // Primitives are never Arrays.
  masm.fallibleUnboxObject(val, scratch1, &isNotArray);

  Label isArray;
  masm.branchTestObjClass(Assembler::Equal, scratch1, &ArrayObject::class_,
                          scratch2, scratch1, &isArray);

  // isArray can also return true for Proxy wrapped Arrays.
  masm.branchTestObjectIsProxy(false, scratch1, scratch2, &isNotArray);
  Label done;
  {
    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch2);

    masm.Push(scratch1);

    using Fn = bool (*)(JSContext*, HandleObject, bool*);
    callVM<Fn, js::IsArrayFromJit>(masm);

    stubFrame.leave(masm);

    masm.tagValue(JSVAL_TYPE_BOOLEAN, ReturnReg, output.valueReg());
    masm.jump(&done);
  }

  masm.bind(&isNotArray);
  masm.moveValue(BooleanValue(false), output.valueReg());
  masm.jump(&done);

  masm.bind(&isArray);
  masm.moveValue(BooleanValue(true), output.valueReg());

  masm.bind(&done);
  return true;
}

bool BaselineCacheIRCompiler::emitIsTypedArrayResult(ObjOperandId objId,
                                                     bool isPossiblyWrapped) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  Register obj = allocator.useRegister(masm, objId);

  allocator.discardStack(masm);

  Label notTypedArray, isProxy, done;
  masm.loadObjClassUnsafe(obj, scratch);
  masm.branchIfClassIsNotTypedArray(scratch, &notTypedArray);
  masm.moveValue(BooleanValue(true), output.valueReg());
  masm.jump(&done);

  masm.bind(&notTypedArray);
  if (isPossiblyWrapped) {
    masm.branchTestClassIsProxy(true, scratch, &isProxy);
  }
  masm.moveValue(BooleanValue(false), output.valueReg());

  if (isPossiblyWrapped) {
    masm.jump(&done);

    masm.bind(&isProxy);

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch);

    masm.Push(obj);

    using Fn = bool (*)(JSContext*, JSObject*, bool*);
    callVM<Fn, jit::IsPossiblyWrappedTypedArray>(masm);

    stubFrame.leave(masm);

    masm.tagValue(JSVAL_TYPE_BOOLEAN, ReturnReg, output.valueReg());
  }

  masm.bind(&done);
  return true;
}

bool BaselineCacheIRCompiler::emitLoadStringCharResult(StringOperandId strId,
                                                       Int32OperandId indexId,
                                                       bool handleOOB) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register str = allocator.useRegister(masm, strId);
  Register index = allocator.useRegister(masm, indexId);
  AutoScratchRegisterMaybeOutput scratch1(allocator, masm, output);
  AutoScratchRegisterMaybeOutputType scratch2(allocator, masm, output);
  AutoScratchRegister scratch3(allocator, masm);

  // Bounds check, load string char.
  Label done;
  Label loadFailed;
  if (!handleOOB) {
    FailurePath* failure;
    if (!addFailurePath(&failure)) {
      return false;
    }

    masm.spectreBoundsCheck32(index, Address(str, JSString::offsetOfLength()),
                              scratch1, failure->label());
    masm.loadStringChar(str, index, scratch1, scratch2, scratch3,
                        failure->label());

    allocator.discardStack(masm);
  } else {
    // Discard the stack before jumping to |done|.
    allocator.discardStack(masm);

    // Return the empty string for out-of-bounds access.
    masm.movePtr(ImmGCPtr(cx_->names().empty), scratch2);

    // This CacheIR op is always preceded by |LinearizeForCharAccess|, so we're
    // guaranteed to see no nested ropes.
    masm.spectreBoundsCheck32(index, Address(str, JSString::offsetOfLength()),
                              scratch1, &done);
    masm.loadStringChar(str, index, scratch1, scratch2, scratch3, &loadFailed);
  }

  // Load StaticString for this char. For larger code units perform a VM call.
  Label vmCall;
  masm.boundsCheck32PowerOfTwo(scratch1, StaticStrings::UNIT_STATIC_LIMIT,
                               &vmCall);
  masm.movePtr(ImmPtr(&cx_->staticStrings().unitStaticTable), scratch2);
  masm.loadPtr(BaseIndex(scratch2, scratch1, ScalePointer), scratch2);

  masm.jump(&done);

  if (handleOOB) {
    masm.bind(&loadFailed);
    masm.assumeUnreachable("loadStringChar can't fail for linear strings");
  }

  {
    masm.bind(&vmCall);

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch2);

    masm.Push(scratch1);

    using Fn = JSLinearString* (*)(JSContext*, int32_t);
    callVM<Fn, jit::StringFromCharCode>(masm);

    stubFrame.leave(masm);

    masm.storeCallPointerResult(scratch2);
  }

  masm.bind(&done);
  masm.tagValue(JSVAL_TYPE_STRING, scratch2, output.valueReg());
  return true;
}

bool BaselineCacheIRCompiler::emitStringFromCodeResult(Int32OperandId codeId,
                                                       StringCode stringCode) {
  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  Register code = allocator.useRegister(masm, codeId);

  FailurePath* failure = nullptr;
  if (stringCode == StringCode::CodePoint) {
    if (!addFailurePath(&failure)) {
      return false;
    }
  }

  if (stringCode == StringCode::CodePoint) {
    // Note: This condition must match tryAttachStringFromCodePoint to prevent
    // failure loops.
    masm.branch32(Assembler::Above, code, Imm32(unicode::NonBMPMax),
                  failure->label());
  }

  allocator.discardStack(masm);

  // We pre-allocate atoms for the first UNIT_STATIC_LIMIT characters.
  // For code units larger than that, we must do a VM call.
  Label vmCall;
  masm.boundsCheck32PowerOfTwo(code, StaticStrings::UNIT_STATIC_LIMIT, &vmCall);

  masm.movePtr(ImmPtr(cx_->runtime()->staticStrings->unitStaticTable), scratch);
  masm.loadPtr(BaseIndex(scratch, code, ScalePointer), scratch);
  Label done;
  masm.jump(&done);

  {
    masm.bind(&vmCall);

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch);

    masm.Push(code);

    if (stringCode == StringCode::CodeUnit) {
      using Fn = JSLinearString* (*)(JSContext*, int32_t);
      callVM<Fn, jit::StringFromCharCode>(masm);
    } else {
      using Fn = JSString* (*)(JSContext*, int32_t);
      callVM<Fn, jit::StringFromCodePoint>(masm);
    }

    stubFrame.leave(masm);
    masm.storeCallPointerResult(scratch);
  }

  masm.bind(&done);
  masm.tagValue(JSVAL_TYPE_STRING, scratch, output.valueReg());
  return true;
}

bool BaselineCacheIRCompiler::emitStringFromCharCodeResult(
    Int32OperandId codeId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  return emitStringFromCodeResult(codeId, StringCode::CodeUnit);
}

bool BaselineCacheIRCompiler::emitStringFromCodePointResult(
    Int32OperandId codeId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  return emitStringFromCodeResult(codeId, StringCode::CodePoint);
}

bool BaselineCacheIRCompiler::emitMathRandomResult(uint32_t rngOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegister scratch1(allocator, masm);
  AutoScratchRegister64 scratch2(allocator, masm);
  AutoAvailableFloatRegister scratchFloat(*this, FloatReg0);

  Address rngAddr(stubAddress(rngOffset));
  masm.loadPtr(rngAddr, scratch1);

  masm.randomDouble(scratch1, scratchFloat, scratch2,
                    output.valueReg().toRegister64());

  if (js::SupportDifferentialTesting()) {
    masm.loadConstantDouble(0.0, scratchFloat);
  }

  masm.boxDouble(scratchFloat, output.valueReg(), scratchFloat);
  return true;
}

bool BaselineCacheIRCompiler::emitReflectGetPrototypeOfResult(
    ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  Register obj = allocator.useRegister(masm, objId);

  allocator.discardStack(masm);

  MOZ_ASSERT(uintptr_t(TaggedProto::LazyProto) == 1);

  masm.loadObjProto(obj, scratch);

  Label hasProto;
  masm.branchPtr(Assembler::Above, scratch, ImmWord(1), &hasProto);

  // Call into the VM for lazy prototypes.
  Label slow, done;
  masm.branchPtr(Assembler::Equal, scratch, ImmWord(1), &slow);

  masm.moveValue(NullValue(), output.valueReg());
  masm.jump(&done);

  masm.bind(&hasProto);
  masm.tagValue(JSVAL_TYPE_OBJECT, scratch, output.valueReg());
  masm.jump(&done);

  {
    masm.bind(&slow);

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch);

    masm.Push(obj);

    using Fn = bool (*)(JSContext*, HandleObject, MutableHandleValue);
    callVM<Fn, jit::GetPrototypeOf>(masm);

    stubFrame.leave(masm);
  }

  masm.bind(&done);
  return true;
}

bool BaselineCacheIRCompiler::emitHasClassResult(ObjOperandId objId,
                                                 uint32_t claspOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  Address claspAddr(stubAddress(claspOffset));
  masm.loadObjClassUnsafe(obj, scratch);
  masm.cmpPtrSet(Assembler::Equal, claspAddr, scratch.get(), scratch);
  masm.tagValue(JSVAL_TYPE_BOOLEAN, scratch, output.valueReg());
  return true;
}

void BaselineCacheIRCompiler::emitAtomizeString(Register str, Register temp,
                                                Label* failure) {
  Label isAtom;
  masm.branchTest32(Assembler::NonZero, Address(str, JSString::offsetOfFlags()),
                    Imm32(JSString::ATOM_BIT), &isAtom);
  {
    LiveRegisterSet save(GeneralRegisterSet::Volatile(),
                         liveVolatileFloatRegs());
    masm.PushRegsInMask(save);

    using Fn = JSAtom* (*)(JSContext* cx, JSString* str);
    masm.setupUnalignedABICall(temp);
    masm.loadJSContext(temp);
    masm.passABIArg(temp);
    masm.passABIArg(str);
    masm.callWithABI<Fn, jit::AtomizeStringNoGC>();
    masm.storeCallPointerResult(temp);

    LiveRegisterSet ignore;
    ignore.add(temp);
    masm.PopRegsInMaskIgnore(save, ignore);

    masm.branchPtr(Assembler::Equal, temp, ImmWord(0), failure);
    masm.mov(temp, str);
  }
  masm.bind(&isAtom);
}

bool BaselineCacheIRCompiler::emitSetHasStringResult(ObjOperandId setId,
                                                     StringOperandId strId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register set = allocator.useRegister(masm, setId);
  Register str = allocator.useRegister(masm, strId);

  AutoScratchRegister scratch1(allocator, masm);
  AutoScratchRegister scratch2(allocator, masm);
  AutoScratchRegister scratch3(allocator, masm);
  AutoScratchRegister scratch4(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  emitAtomizeString(str, scratch1, failure->label());
  masm.prepareHashString(str, scratch1, scratch2);

  masm.tagValue(JSVAL_TYPE_STRING, str, output.valueReg());
  masm.setObjectHasNonBigInt(set, output.valueReg(), scratch1, scratch2,
                             scratch3, scratch4);
  masm.tagValue(JSVAL_TYPE_BOOLEAN, scratch2, output.valueReg());
  return true;
}

bool BaselineCacheIRCompiler::emitMapHasStringResult(ObjOperandId mapId,
                                                     StringOperandId strId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register map = allocator.useRegister(masm, mapId);
  Register str = allocator.useRegister(masm, strId);

  AutoScratchRegister scratch1(allocator, masm);
  AutoScratchRegister scratch2(allocator, masm);
  AutoScratchRegister scratch3(allocator, masm);
  AutoScratchRegister scratch4(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  emitAtomizeString(str, scratch1, failure->label());
  masm.prepareHashString(str, scratch1, scratch2);

  masm.tagValue(JSVAL_TYPE_STRING, str, output.valueReg());
  masm.mapObjectHasNonBigInt(map, output.valueReg(), scratch1, scratch2,
                             scratch3, scratch4);
  masm.tagValue(JSVAL_TYPE_BOOLEAN, scratch2, output.valueReg());
  return true;
}

bool BaselineCacheIRCompiler::emitMapGetStringResult(ObjOperandId mapId,
                                                     StringOperandId strId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register map = allocator.useRegister(masm, mapId);
  Register str = allocator.useRegister(masm, strId);

  AutoScratchRegister scratch1(allocator, masm);
  AutoScratchRegister scratch2(allocator, masm);
  AutoScratchRegister scratch3(allocator, masm);
  AutoScratchRegister scratch4(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  emitAtomizeString(str, scratch1, failure->label());
  masm.prepareHashString(str, scratch1, scratch2);

  masm.tagValue(JSVAL_TYPE_STRING, str, output.valueReg());
  masm.mapObjectGetNonBigInt(map, output.valueReg(), scratch1,
                             output.valueReg(), scratch2, scratch3, scratch4);
  return true;
}

bool BaselineCacheIRCompiler::emitCallNativeSetter(
    ObjOperandId receiverId, uint32_t setterOffset, ValOperandId rhsId,
    bool sameRealm, uint32_t nargsAndFlagsOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register receiver = allocator.useRegister(masm, receiverId);
  Address setterAddr(stubAddress(setterOffset));
  ValueOperand val = allocator.useValueRegister(masm, rhsId);

  AutoScratchRegister scratch(allocator, masm);

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  // Load the callee in the scratch register.
  masm.loadPtr(setterAddr, scratch);

  masm.Push(val);
  masm.Push(receiver);
  masm.Push(scratch);

  using Fn = bool (*)(JSContext*, HandleFunction, HandleObject, HandleValue);
  callVM<Fn, CallNativeSetter>(masm);

  stubFrame.leave(masm);
  return true;
}

bool BaselineCacheIRCompiler::emitCallScriptedSetterShared(
    ObjOperandId receiverId, uint32_t setterOffset, ValOperandId rhsId,
    bool sameRealm, uint32_t nargsAndFlagsOffset,
    Maybe<uint32_t> icScriptOffset) {
  AutoScratchRegister callee(allocator, masm);
  AutoScratchRegister scratch(allocator, masm);
#if defined(JS_CODEGEN_X86)
  Register code = scratch;
#else
  AutoScratchRegister code(allocator, masm);
#endif

  Register receiver = allocator.useRegister(masm, receiverId);
  Address setterAddr(stubAddress(setterOffset));
  ValueOperand val = allocator.useValueRegister(masm, rhsId);

  bool isInlined = icScriptOffset.isSome();

  // First, load the callee.
  masm.loadPtr(setterAddr, callee);

  if (isInlined) {
    // If we are calling a trial-inlined setter, guard that the
    // target has a BaselineScript.
    FailurePath* failure;
    if (!addFailurePath(&failure)) {
      return false;
    }
    masm.loadBaselineJitCodeRaw(callee, code, failure->label());
  }

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  if (!sameRealm) {
    masm.switchToObjectRealm(callee, scratch);
  }

  // Align the stack such that the JitFrameLayout is aligned on
  // JitStackAlignment.
  masm.alignJitStackBasedOnNArgs(1, /*countIncludesThis = */ false);

  // Setter is called with 1 argument, and |receiver| as thisv. Note that we use
  // Push, not push, so that callJit will align the stack properly on ARM.
  masm.Push(val);
  masm.Push(TypedOrValueRegister(MIRType::Object, AnyRegister(receiver)));

  // Push callee.
  masm.Push(callee);

  // Push frame descriptor.
  masm.PushFrameDescriptorForJitCall(FrameType::BaselineStub, /* argc = */ 1);

  if (isInlined) {
    // Store icScript in the context.
    Address icScriptAddr(stubAddress(*icScriptOffset));
    masm.loadPtr(icScriptAddr, scratch);
    masm.storeICScriptInJSContext(scratch);
  }

  // Load the jitcode pointer.
  if (isInlined) {
    // On non-x86 platforms, this pointer is still in a register
    // after guarding on it above. On x86, we don't have enough
    // registers and have to reload it here.
#ifdef JS_CODEGEN_X86
    masm.loadBaselineJitCodeRaw(callee, code);
#endif
  } else {
    masm.loadJitCodeRaw(callee, code);
  }

  // Handle arguments underflow. The rhs value is no longer needed and
  // can be used as scratch.
  Label noUnderflow;
  Register scratch2 = val.scratchReg();
  masm.loadFunctionArgCount(callee, scratch2);
  masm.branch32(Assembler::BelowOrEqual, scratch2, Imm32(1), &noUnderflow);

  // Call the arguments rectifier.
  ArgumentsRectifierKind kind = isInlined
                                    ? ArgumentsRectifierKind::TrialInlining
                                    : ArgumentsRectifierKind::Normal;
  TrampolinePtr argumentsRectifier =
      cx_->runtime()->jitRuntime()->getArgumentsRectifier(kind);
  masm.movePtr(argumentsRectifier, code);

  masm.bind(&noUnderflow);
  masm.callJit(code);

  stubFrame.leave(masm);

  if (!sameRealm) {
    masm.switchToBaselineFrameRealm(R1.scratchReg());
  }

  return true;
}

bool BaselineCacheIRCompiler::emitCallScriptedSetter(
    ObjOperandId receiverId, uint32_t setterOffset, ValOperandId rhsId,
    bool sameRealm, uint32_t nargsAndFlagsOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Maybe<uint32_t> icScriptOffset = mozilla::Nothing();
  return emitCallScriptedSetterShared(receiverId, setterOffset, rhsId,
                                      sameRealm, nargsAndFlagsOffset,
                                      icScriptOffset);
}

bool BaselineCacheIRCompiler::emitCallInlinedSetter(
    ObjOperandId receiverId, uint32_t setterOffset, ValOperandId rhsId,
    uint32_t icScriptOffset, bool sameRealm, uint32_t nargsAndFlagsOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  return emitCallScriptedSetterShared(receiverId, setterOffset, rhsId,
                                      sameRealm, nargsAndFlagsOffset,
                                      mozilla::Some(icScriptOffset));
}

bool BaselineCacheIRCompiler::emitCallDOMSetter(ObjOperandId objId,
                                                uint32_t jitInfoOffset,
                                                ValOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  ValueOperand val = allocator.useValueRegister(masm, rhsId);
  Address jitInfoAddr(stubAddress(jitInfoOffset));

  AutoScratchRegister scratch(allocator, masm);

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  // Load the JSJitInfo in the scratch register.
  masm.loadPtr(jitInfoAddr, scratch);

  masm.Push(val);
  masm.Push(obj);
  masm.Push(scratch);

  using Fn = bool (*)(JSContext*, const JSJitInfo*, HandleObject, HandleValue);
  callVM<Fn, CallDOMSetter>(masm);

  stubFrame.leave(masm);
  return true;
}

bool BaselineCacheIRCompiler::emitCallSetArrayLength(ObjOperandId objId,
                                                     bool strict,
                                                     ValOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  ValueOperand val = allocator.useValueRegister(masm, rhsId);

  AutoScratchRegister scratch(allocator, masm);

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  masm.Push(Imm32(strict));
  masm.Push(val);
  masm.Push(obj);

  using Fn = bool (*)(JSContext*, HandleObject, HandleValue, bool);
  callVM<Fn, jit::SetArrayLength>(masm);

  stubFrame.leave(masm);
  return true;
}

bool BaselineCacheIRCompiler::emitProxySet(ObjOperandId objId,
                                           uint32_t idOffset,
                                           ValOperandId rhsId, bool strict) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  ValueOperand val = allocator.useValueRegister(masm, rhsId);
  Address idAddr(stubAddress(idOffset));

  AutoScratchRegister scratch(allocator, masm);

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  // Load the jsid in the scratch register.
  masm.loadPtr(idAddr, scratch);

  masm.Push(Imm32(strict));
  masm.Push(val);
  masm.Push(scratch);
  masm.Push(obj);

  using Fn = bool (*)(JSContext*, HandleObject, HandleId, HandleValue, bool);
  callVM<Fn, ProxySetProperty>(masm);

  stubFrame.leave(masm);
  return true;
}

bool BaselineCacheIRCompiler::emitProxySetByValue(ObjOperandId objId,
                                                  ValOperandId idId,
                                                  ValOperandId rhsId,
                                                  bool strict) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  ValueOperand idVal = allocator.useValueRegister(masm, idId);
  ValueOperand val = allocator.useValueRegister(masm, rhsId);

  allocator.discardStack(masm);

  // We need a scratch register but we don't have any registers available on
  // x86, so temporarily store |obj| in the frame's scratch slot.
  int scratchOffset = BaselineFrame::reverseOffsetOfScratchValue();
  masm.storePtr(obj, Address(baselineFrameReg(), scratchOffset));

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, obj);

  // Restore |obj|. Because we entered a stub frame we first have to load
  // the original frame pointer.
  masm.loadPtr(Address(FramePointer, 0), obj);
  masm.loadPtr(Address(obj, scratchOffset), obj);

  masm.Push(Imm32(strict));
  masm.Push(val);
  masm.Push(idVal);
  masm.Push(obj);

  using Fn = bool (*)(JSContext*, HandleObject, HandleValue, HandleValue, bool);
  callVM<Fn, ProxySetPropertyByValue>(masm);

  stubFrame.leave(masm);
  return true;
}

bool BaselineCacheIRCompiler::emitCallAddOrUpdateSparseElementHelper(
    ObjOperandId objId, Int32OperandId idId, ValOperandId rhsId, bool strict) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  Register id = allocator.useRegister(masm, idId);
  ValueOperand val = allocator.useValueRegister(masm, rhsId);
  AutoScratchRegister scratch(allocator, masm);

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  masm.Push(Imm32(strict));
  masm.Push(val);
  masm.Push(id);
  masm.Push(obj);

  using Fn = bool (*)(JSContext* cx, Handle<NativeObject*> obj, int32_t int_id,
                      HandleValue v, bool strict);
  callVM<Fn, AddOrUpdateSparseElementHelper>(masm);

  stubFrame.leave(masm);
  return true;
}

bool BaselineCacheIRCompiler::emitMegamorphicSetElement(ObjOperandId objId,
                                                        ValOperandId idId,
                                                        ValOperandId rhsId,
                                                        bool strict) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  ValueOperand idVal = allocator.useValueRegister(masm, idId);
  ValueOperand val = allocator.useValueRegister(masm, rhsId);

#ifdef JS_CODEGEN_X86
  allocator.discardStack(masm);
  // We need a scratch register but we don't have any registers available on
  // x86, so temporarily store |obj| in the frame's scratch slot.
  int scratchOffset = BaselineFrame::reverseOffsetOfScratchValue();
  masm.storePtr(obj, Address(baselineFrameReg_, scratchOffset));

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, obj);

  // Restore |obj|. Because we entered a stub frame we first have to load
  // the original frame pointer.
  masm.loadPtr(Address(FramePointer, 0), obj);
  masm.loadPtr(Address(obj, scratchOffset), obj);
#else
  AutoScratchRegister scratch(allocator, masm);

  allocator.discardStack(masm);
  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);
#endif

  masm.Push(Imm32(strict));
  masm.Push(val);
  masm.Push(idVal);
  masm.Push(obj);

  using Fn = bool (*)(JSContext*, HandleObject, HandleValue, HandleValue, bool);
  callVM<Fn, SetElementMegamorphic<false>>(masm);

  stubFrame.leave(masm);
  return true;
}

bool BaselineCacheIRCompiler::emitReturnFromIC() {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  allocator.discardStack(masm);
  if (JitOptions.enableICFramePointers) {
    masm.pop(FramePointer);
  }
  EmitReturnFromIC(masm);
  return true;
}

bool BaselineCacheIRCompiler::emitLoadArgumentFixedSlot(ValOperandId resultId,
                                                        uint8_t slotIndex) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  ValueOperand resultReg = allocator.defineValueRegister(masm, resultId);
  Address addr = allocator.addressOf(masm, BaselineFrameSlot(slotIndex));
  masm.loadValue(addr, resultReg);
  return true;
}

bool BaselineCacheIRCompiler::emitLoadArgumentDynamicSlot(ValOperandId resultId,
                                                          Int32OperandId argcId,
                                                          uint8_t slotIndex) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  ValueOperand resultReg = allocator.defineValueRegister(masm, resultId);
  Register argcReg = allocator.useRegister(masm, argcId);
  BaseValueIndex addr =
      allocator.addressOf(masm, argcReg, BaselineFrameSlot(slotIndex));
  masm.loadValue(addr, resultReg);
  return true;
}

bool BaselineCacheIRCompiler::emitGuardDOMExpandoMissingOrGuardShape(
    ValOperandId expandoId, uint32_t shapeOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  ValueOperand val = allocator.useValueRegister(masm, expandoId);
  AutoScratchRegister shapeScratch(allocator, masm);
  AutoScratchRegister objScratch(allocator, masm);
  Address shapeAddr(stubAddress(shapeOffset));

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Label done;
  masm.branchTestUndefined(Assembler::Equal, val, &done);

  masm.debugAssertIsObject(val);
  masm.loadPtr(shapeAddr, shapeScratch);
  masm.unboxObject(val, objScratch);
  // The expando object is not used in this case, so we don't need Spectre
  // mitigations.
  masm.branchTestObjShapeNoSpectreMitigations(Assembler::NotEqual, objScratch,
                                              shapeScratch, failure->label());

  masm.bind(&done);
  return true;
}

bool BaselineCacheIRCompiler::emitLoadDOMExpandoValueGuardGeneration(
    ObjOperandId objId, uint32_t expandoAndGenerationOffset,
    uint32_t generationOffset, ValOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  Address expandoAndGenerationAddr(stubAddress(expandoAndGenerationOffset));
  Address generationAddr(stubAddress(generationOffset));

  AutoScratchRegister scratch(allocator, masm);
  ValueOperand output = allocator.defineValueRegister(masm, resultId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()), scratch);
  Address expandoAddr(scratch,
                      js::detail::ProxyReservedSlots::offsetOfPrivateSlot());

  // Load the ExpandoAndGeneration* in the output scratch register and guard
  // it matches the proxy's ExpandoAndGeneration.
  masm.loadPtr(expandoAndGenerationAddr, output.scratchReg());
  masm.branchPrivatePtr(Assembler::NotEqual, expandoAddr, output.scratchReg(),
                        failure->label());

  // Guard expandoAndGeneration->generation matches the expected generation.
  masm.branch64(
      Assembler::NotEqual,
      Address(output.scratchReg(), ExpandoAndGeneration::offsetOfGeneration()),
      generationAddr, scratch, failure->label());

  // Load expandoAndGeneration->expando into the output Value register.
  masm.loadValue(
      Address(output.scratchReg(), ExpandoAndGeneration::offsetOfExpando()),
      output);
  return true;
}

bool BaselineCacheIRCompiler::init(CacheKind kind) {
  if (!allocator.init()) {
    return false;
  }

  size_t numInputs = writer_.numInputOperands();
  MOZ_ASSERT(numInputs == NumInputsForCacheKind(kind));

  // Baseline passes the first 2 inputs in R0/R1, other Values are stored on
  // the stack.
  size_t numInputsInRegs = std::min(numInputs, size_t(2));
  AllocatableGeneralRegisterSet available =
      BaselineICAvailableGeneralRegs(numInputsInRegs);

  switch (kind) {
    case CacheKind::NewArray:
    case CacheKind::NewObject:
    case CacheKind::GetIntrinsic:
      MOZ_ASSERT(numInputs == 0);
      outputUnchecked_.emplace(R0);
      break;
    case CacheKind::GetProp:
    case CacheKind::TypeOf:
    case CacheKind::ToPropertyKey:
    case CacheKind::GetIterator:
    case CacheKind::OptimizeSpreadCall:
    case CacheKind::ToBool:
    case CacheKind::UnaryArith:
      MOZ_ASSERT(numInputs == 1);
      allocator.initInputLocation(0, R0);
      outputUnchecked_.emplace(R0);
      break;
    case CacheKind::Compare:
    case CacheKind::GetElem:
    case CacheKind::GetPropSuper:
    case CacheKind::In:
    case CacheKind::HasOwn:
    case CacheKind::CheckPrivateField:
    case CacheKind::InstanceOf:
    case CacheKind::BinaryArith:
      MOZ_ASSERT(numInputs == 2);
      allocator.initInputLocation(0, R0);
      allocator.initInputLocation(1, R1);
      outputUnchecked_.emplace(R0);
      break;
    case CacheKind::SetProp:
      MOZ_ASSERT(numInputs == 2);
      allocator.initInputLocation(0, R0);
      allocator.initInputLocation(1, R1);
      break;
    case CacheKind::GetElemSuper:
      MOZ_ASSERT(numInputs == 3);
      allocator.initInputLocation(0, BaselineFrameSlot(0));
      allocator.initInputLocation(1, R1);
      allocator.initInputLocation(2, R0);
      outputUnchecked_.emplace(R0);
      break;
    case CacheKind::SetElem:
      MOZ_ASSERT(numInputs == 3);
      allocator.initInputLocation(0, R0);
      allocator.initInputLocation(1, R1);
      allocator.initInputLocation(2, BaselineFrameSlot(0));
      break;
    case CacheKind::GetName:
    case CacheKind::BindName:
      MOZ_ASSERT(numInputs == 1);
      allocator.initInputLocation(0, R0.scratchReg(), JSVAL_TYPE_OBJECT);
#if defined(JS_NUNBOX32)
      // availableGeneralRegs can't know that GetName/BindName is only using
      // the payloadReg and not typeReg on x86.
      available.add(R0.typeReg());
#endif
      outputUnchecked_.emplace(R0);
      break;
    case CacheKind::Call:
      MOZ_ASSERT(numInputs == 1);
      allocator.initInputLocation(0, R0.scratchReg(), JSVAL_TYPE_INT32);
#if defined(JS_NUNBOX32)
      // availableGeneralRegs can't know that Call is only using
      // the payloadReg and not typeReg on x86.
      available.add(R0.typeReg());
#endif
      outputUnchecked_.emplace(R0);
      break;
    case CacheKind::CloseIter:
      MOZ_ASSERT(numInputs == 1);
      allocator.initInputLocation(0, R0.scratchReg(), JSVAL_TYPE_OBJECT);
#if defined(JS_NUNBOX32)
      // availableGeneralRegs can't know that CloseIter is only using
      // the payloadReg and not typeReg on x86.
      available.add(R0.typeReg());
#endif
      break;
  }

  // Baseline doesn't allocate float registers so none of them are live.
  liveFloatRegs_ = LiveFloatRegisterSet(FloatRegisterSet());

  if (JitOptions.enableICFramePointers) {
    baselineFrameReg_ = available.takeAny();
  }

  allocator.initAvailableRegs(available);
  return true;
}

static void ResetEnteredCounts(const ICEntry* icEntry) {
  ICStub* stub = icEntry->firstStub();
  while (true) {
    stub->resetEnteredCount();
    if (stub->isFallback()) {
      return;
    }
    stub = stub->toCacheIRStub()->next();
  }
}

static ICStubSpace* StubSpaceForStub(bool makesGCCalls, JSScript* script,
                                     ICScript* icScript) {
  if (makesGCCalls) {
    return icScript->jitScriptStubSpace();
  }
  return script->zone()->jitZone()->optimizedStubSpace();
}

static const uint32_t MaxFoldedShapes = 16;

bool js::jit::TryFoldingStubs(JSContext* cx, ICFallbackStub* fallback,
                              JSScript* script, ICScript* icScript) {
  ICEntry* icEntry = icScript->icEntryForStub(fallback);
  ICStub* entryStub = icEntry->firstStub();

  // Don't fold unless there are at least two stubs.
  if (entryStub == fallback) {
    return true;
  }
  ICCacheIRStub* firstStub = entryStub->toCacheIRStub();
  if (firstStub->next()->isFallback()) {
    return true;
  }

  const uint8_t* firstStubData = firstStub->stubDataStart();
  const CacheIRStubInfo* stubInfo = firstStub->stubInfo();

  // Check to see if:
  //   a) all of the stubs in this chain have the exact same code.
  //   b) all of the stubs have the same stub field data, except
  //      for a single GuardShape where they differ.
  //   c) at least one stub after the first has a non-zero entry count.
  //
  // If all of these conditions hold, then we generate a single stub
  // that covers all the existing cases by replacing GuardShape with
  // GuardMultipleShapes.

  uint32_t numActive = 0;
  Maybe<uint32_t> foldableFieldOffset;
  RootedValue shape(cx);
  RootedValueVector shapeList(cx);

  auto addShape = [&shapeList, cx](uintptr_t rawShape) -> bool {
    Shape* shape = reinterpret_cast<Shape*>(rawShape);
    if (cx->compartment() != shape->compartment()) {
      return false;
    }
    if (!shapeList.append(PrivateGCThingValue(shape))) {
      cx->recoverFromOutOfMemory();
      return false;
    }
    return true;
  };

  for (ICCacheIRStub* other = firstStub->nextCacheIR(); other;
       other = other->nextCacheIR()) {
    // Verify that the stubs share the same code.
    if (other->stubInfo() != stubInfo) {
      return true;
    }
    const uint8_t* otherStubData = other->stubDataStart();

    if (other->enteredCount() > 0) {
      numActive++;
    }

    uint32_t fieldIndex = 0;
    size_t offset = 0;
    while (stubInfo->fieldType(fieldIndex) != StubField::Type::Limit) {
      StubField::Type fieldType = stubInfo->fieldType(fieldIndex);

      if (StubField::sizeIsWord(fieldType)) {
        uintptr_t firstRaw = stubInfo->getStubRawWord(firstStubData, offset);
        uintptr_t otherRaw = stubInfo->getStubRawWord(otherStubData, offset);

        if (firstRaw != otherRaw) {
          if (fieldType != StubField::Type::Shape) {
            // Case 1: a field differs that is not a Shape. We only support
            // folding GuardShape to GuardMultipleShapes.
            return true;
          }
          if (foldableFieldOffset.isNothing()) {
            // Case 2: this is the first field where the stub data differs.
            foldableFieldOffset.emplace(offset);
            if (!addShape(firstRaw) || !addShape(otherRaw)) {
              return true;
            }
          } else if (*foldableFieldOffset == offset) {
            // Case 3: this is the corresponding offset in a different stub.
            if (!addShape(otherRaw)) {
              return true;
            }
          } else {
            // Case 4: we have found more than one field that differs.
            return true;
          }
        }
      } else {
        MOZ_ASSERT(StubField::sizeIsInt64(fieldType));

        // We do not support folding any ops with int64-sized fields.
        if (stubInfo->getStubRawInt64(firstStubData, offset) !=
            stubInfo->getStubRawInt64(otherStubData, offset)) {
          return true;
        }
      }

      offset += StubField::sizeInBytes(fieldType);
      fieldIndex++;
    }

    // We should never attach two completely identical stubs.
    MOZ_ASSERT(foldableFieldOffset.isSome());
  }

  if (numActive == 0) {
    return true;
  }

  // Clone the CacheIR, replacing GuardShape with GuardMultipleShapes.
  CacheIRWriter writer(cx);
  CacheIRReader reader(stubInfo);
  CacheIRCloner cloner(firstStub);

  // Initialize the operands.
  CacheKind cacheKind = stubInfo->kind();
  for (uint32_t i = 0; i < NumInputsForCacheKind(cacheKind); i++) {
    writer.setInputOperandId(i);
  }

  bool success = false;
  while (reader.more()) {
    CacheOp op = reader.readOp();
    switch (op) {
      case CacheOp::GuardShape: {
        ObjOperandId objId = reader.objOperandId();
        uint32_t shapeOffset = reader.stubOffset();
        if (shapeOffset == *foldableFieldOffset) {
          // Ensure that the allocation of the ListObject doesn't trigger a GC
          // and free the stubInfo we're currently reading. Note that
          // AutoKeepJitScripts isn't sufficient, because optimized stubs can be
          // discarded even if the JitScript is preserved.
          gc::AutoSuppressGC suppressGC(cx);

          Rooted<ListObject*> shapeObj(cx, ListObject::create(cx));
          if (!shapeObj) {
            return false;
          }
          for (uint32_t i = 0; i < shapeList.length(); i++) {
            if (!shapeObj->append(cx, shapeList[i])) {
              cx->recoverFromOutOfMemory();
              return false;
            }
          }

          writer.guardMultipleShapes(objId, shapeObj);
          success = true;
        } else {
          Shape* shape = stubInfo->getStubField<Shape*>(firstStub, shapeOffset);
          writer.guardShape(objId, shape);
        }
        break;
      }
      default:
        cloner.cloneOp(op, reader, writer);
        break;
    }
  }
  if (!success) {
    // If the shape field that differed was not part of a GuardShape,
    // we can't fold these stubs together.
    return true;
  }

  // Replace the existing stubs with the new folded stub.
  fallback->discardStubs(cx, icEntry);

  ICAttachResult result = AttachBaselineCacheIRStub(
      cx, writer, cacheKind, script, icScript, fallback, "StubFold");
  if (result == ICAttachResult::OOM) {
    ReportOutOfMemory(cx);
    return false;
  }
  MOZ_ASSERT(result == ICAttachResult::Attached);

  fallback->setHasFoldedStub();
  return true;
}

static bool AddToFoldedStub(JSContext* cx, const CacheIRWriter& writer,
                            ICScript* icScript, ICFallbackStub* fallback) {
  ICEntry* icEntry = icScript->icEntryForStub(fallback);
  ICStub* entryStub = icEntry->firstStub();

  // We only update folded stubs if they're the only stub in the IC.
  if (entryStub == fallback) {
    return false;
  }
  ICCacheIRStub* stub = entryStub->toCacheIRStub();
  if (!stub->next()->isFallback()) {
    return false;
  }

  const CacheIRStubInfo* stubInfo = stub->stubInfo();
  const uint8_t* stubData = stub->stubDataStart();

  Maybe<uint32_t> shapeFieldOffset;
  RootedValue newShape(cx);
  Rooted<ListObject*> foldedShapes(cx);

  CacheIRReader stubReader(stubInfo);
  CacheIRReader newReader(writer);
  while (newReader.more() && stubReader.more()) {
    CacheOp newOp = newReader.readOp();
    CacheOp stubOp = stubReader.readOp();
    switch (stubOp) {
      case CacheOp::GuardMultipleShapes: {
        // Check that the new stub has a corresponding GuardShape.
        if (newOp != CacheOp::GuardShape) {
          return false;
        }

        // Check that the object being guarded is the same.
        if (newReader.objOperandId() != stubReader.objOperandId()) {
          return false;
        }

        // Check that the field offset is the same.
        uint32_t newShapeOffset = newReader.stubOffset();
        uint32_t stubShapesOffset = stubReader.stubOffset();
        if (newShapeOffset != stubShapesOffset) {
          return false;
        }
        MOZ_ASSERT(shapeFieldOffset.isNothing());
        shapeFieldOffset.emplace(newShapeOffset);

        // Get the shape from the new stub
        StubField shapeField =
            writer.readStubField(newShapeOffset, StubField::Type::Shape);
        Shape* shape = reinterpret_cast<Shape*>(shapeField.asWord());
        newShape = PrivateGCThingValue(shape);

        // Get the shape array from the old stub.
        JSObject* shapeList =
            stubInfo->getStubField<JSObject*>(stub, stubShapesOffset);
        foldedShapes = &shapeList->as<ListObject>();
        MOZ_ASSERT(foldedShapes->compartment() == shape->compartment());
        break;
      }
      default: {
        // Check that the op is the same.
        if (newOp != stubOp) {
          return false;
        }

        // Check that the arguments are the same.
        uint32_t argLength = CacheIROpInfos[size_t(newOp)].argLength;
        for (uint32_t i = 0; i < argLength; i++) {
          if (newReader.readByte() != stubReader.readByte()) {
            return false;
          }
        }
      }
    }
  }

  MOZ_ASSERT(shapeFieldOffset.isSome());

  // Check to verify that all the other stub fields are the same.
  if (!writer.stubDataEqualsIgnoring(stubData, *shapeFieldOffset)) {
    return false;
  }

  // Limit the maximum number of shapes we will add before giving up.
  if (foldedShapes->length() == MaxFoldedShapes) {
    return false;
  }

  if (!foldedShapes->append(cx, newShape)) {
    cx->recoverFromOutOfMemory();
    return false;
  }

  return true;
}

ICAttachResult js::jit::AttachBaselineCacheIRStub(
    JSContext* cx, const CacheIRWriter& writer, CacheKind kind,
    JSScript* outerScript, ICScript* icScript, ICFallbackStub* stub,
    const char* name) {
  // We shouldn't GC or report OOM (or any other exception) here.
  AutoAssertNoPendingException aanpe(cx);
  JS::AutoCheckCannotGC nogc;

  if (writer.tooLarge()) {
    return ICAttachResult::TooLarge;
  }
  if (writer.oom()) {
    return ICAttachResult::OOM;
  }
  MOZ_ASSERT(!writer.failed());

  // Just a sanity check: the caller should ensure we don't attach an
  // unlimited number of stubs.
#ifdef DEBUG
  static const size_t MaxOptimizedCacheIRStubs = 16;
  MOZ_ASSERT(stub->numOptimizedStubs() < MaxOptimizedCacheIRStubs);
#endif

  constexpr uint32_t stubDataOffset = sizeof(ICCacheIRStub);
  static_assert(stubDataOffset % sizeof(uint64_t) == 0,
                "Stub fields must be aligned");

  JitZone* jitZone = cx->zone()->jitZone();

  // Check if we already have JitCode for this stub.
  CacheIRStubInfo* stubInfo;
  CacheIRStubKey::Lookup lookup(kind, ICStubEngine::Baseline,
                                writer.codeStart(), writer.codeLength());
  JitCode* code = jitZone->getBaselineCacheIRStubCode(lookup, &stubInfo);
  if (!code) {
    // We have to generate stub code.
    TempAllocator temp(&cx->tempLifoAlloc());
    JitContext jctx(cx);
    BaselineCacheIRCompiler comp(cx, temp, writer, stubDataOffset);
    if (!comp.init(kind)) {
      return ICAttachResult::OOM;
    }

    code = comp.compile();
    if (!code) {
      return ICAttachResult::OOM;
    }

    comp.perfSpewer().saveProfile(code, name);

    // Allocate the shared CacheIRStubInfo. Note that the
    // putBaselineCacheIRStubCode call below will transfer ownership
    // to the stub code HashMap, so we don't have to worry about freeing
    // it below.
    MOZ_ASSERT(!stubInfo);
    stubInfo =
        CacheIRStubInfo::New(kind, ICStubEngine::Baseline, comp.makesGCCalls(),
                             stubDataOffset, writer);
    if (!stubInfo) {
      return ICAttachResult::OOM;
    }

    CacheIRStubKey key(stubInfo);
    if (!jitZone->putBaselineCacheIRStubCode(lookup, key, code)) {
      return ICAttachResult::OOM;
    }
  }

  MOZ_ASSERT(code);
  MOZ_ASSERT(stubInfo);
  MOZ_ASSERT(stubInfo->stubDataSize() == writer.stubDataSize());

  ICEntry* icEntry = icScript->icEntryForStub(stub);

  // Ensure we don't attach duplicate stubs. This can happen if a stub failed
  // for some reason and the IR generator doesn't check for exactly the same
  // conditions.
  for (ICStub* iter = icEntry->firstStub(); iter != stub;
       iter = iter->toCacheIRStub()->next()) {
    auto otherStub = iter->toCacheIRStub();
    if (otherStub->stubInfo() != stubInfo) {
      continue;
    }
    if (!writer.stubDataEquals(otherStub->stubDataStart())) {
      continue;
    }

    // We found a stub that's exactly the same as the stub we're about to
    // attach. Just return nullptr, the caller should do nothing in this
    // case.
    JitSpew(JitSpew_BaselineICFallback,
            "Tried attaching identical stub for (%s:%u:%u)",
            outerScript->filename(), outerScript->lineno(),
            outerScript->column());
    return ICAttachResult::DuplicateStub;
  }

  // Try including this case in an existing folded stub.
  if (stub->hasFoldedStub() && AddToFoldedStub(cx, writer, icScript, stub)) {
    // Instead of adding a new stub, we have added a new case to an
    // existing folded stub. We do not have to invalidate Warp,
    // because the ListObject that stores the cases is shared between
    // baseline and Warp. Reset the entered count for the fallback
    // stub so that we can still transpile, and reset the bailout
    // counter if we have already been transpiled.
    stub->resetEnteredCount();
    JSScript* owningScript = nullptr;
    if (cx->zone()->jitZone()->hasStubFoldingBailoutData(outerScript)) {
      owningScript = cx->zone()->jitZone()->stubFoldingBailoutParent();
    } else {
      owningScript = icScript->isInlined()
                         ? icScript->inliningRoot()->owningScript()
                         : outerScript;
    }
    cx->zone()->jitZone()->clearStubFoldingBailoutData();
    if (stub->usedByTranspiler() && owningScript->hasIonScript()) {
      owningScript->ionScript()->resetNumFixableBailouts();
    }
    return ICAttachResult::Attached;
  }

  // Time to allocate and attach a new stub.

  size_t bytesNeeded = stubInfo->stubDataOffset() + stubInfo->stubDataSize();

  ICStubSpace* stubSpace =
      StubSpaceForStub(stubInfo->makesGCCalls(), outerScript, icScript);
  void* newStubMem = stubSpace->alloc(bytesNeeded);
  if (!newStubMem) {
    return ICAttachResult::OOM;
  }

  // Resetting the entered counts on the IC chain makes subsequent reasoning
  // about the chain much easier.
  ResetEnteredCounts(icEntry);

  switch (stub->trialInliningState()) {
    case TrialInliningState::Initial:
    case TrialInliningState::Candidate:
      stub->setTrialInliningState(writer.trialInliningState());
      break;
    case TrialInliningState::MonomorphicInlined:
    case TrialInliningState::Inlined:
      stub->setTrialInliningState(TrialInliningState::Failure);
      break;
    case TrialInliningState::Failure:
      break;
  }

  auto newStub = new (newStubMem) ICCacheIRStub(code, stubInfo);
  writer.copyStubData(newStub->stubDataStart());
  newStub->setTypeData(writer.typeData());
  stub->addNewStub(icEntry, newStub);
  return ICAttachResult::Attached;
}

uint8_t* ICCacheIRStub::stubDataStart() {
  return reinterpret_cast<uint8_t*>(this) + stubInfo_->stubDataOffset();
}

bool BaselineCacheIRCompiler::emitCallStringObjectConcatResult(
    ValOperandId lhsId, ValOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  ValueOperand lhs = allocator.useValueRegister(masm, lhsId);
  ValueOperand rhs = allocator.useValueRegister(masm, rhsId);

  AutoScratchRegister scratch(allocator, masm);

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  masm.pushValue(rhs);
  masm.pushValue(lhs);

  using Fn = bool (*)(JSContext*, HandleValue, HandleValue, MutableHandleValue);
  callVM<Fn, DoConcatStringObject>(masm);

  stubFrame.leave(masm);
  return true;
}

// The value of argc entering the call IC is not always the value of
// argc entering the callee. (For example, argc for a spread call IC
// is always 1, but argc for the callee is the length of the array.)
// In these cases, we update argc as part of the call op itself, to
// avoid modifying input operands while it is still possible to fail a
// guard. We also limit callee argc to a reasonable value to avoid
// blowing the stack limit.
bool BaselineCacheIRCompiler::updateArgc(CallFlags flags, Register argcReg,
                                         Register scratch) {
  CallFlags::ArgFormat format = flags.getArgFormat();
  switch (format) {
    case CallFlags::Standard:
      // Standard calls have no extra guards, and argc is already correct.
      return true;
    case CallFlags::FunCall:
      // fun_call has no extra guards, and argc will be corrected in
      // pushFunCallArguments.
      return true;
    case CallFlags::FunApplyNullUndefined:
      // argc must be 0 if null or undefined is passed as second argument to
      // |apply|.
      masm.move32(Imm32(0), argcReg);
      return true;
    default:
      break;
  }

  // We need to guard the length of the arguments.
  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Load callee argc into scratch.
  switch (flags.getArgFormat()) {
    case CallFlags::Spread:
    case CallFlags::FunApplyArray: {
      // Load the length of the elements.
      BaselineFrameSlot slot(flags.isConstructing());
      masm.unboxObject(allocator.addressOf(masm, slot), scratch);
      masm.loadPtr(Address(scratch, NativeObject::offsetOfElements()), scratch);
      masm.load32(Address(scratch, ObjectElements::offsetOfLength()), scratch);
      break;
    }
    case CallFlags::FunApplyArgsObj: {
      // Load the arguments object length.
      BaselineFrameSlot slot(0);
      masm.unboxObject(allocator.addressOf(masm, slot), scratch);
      masm.loadArgumentsObjectLength(scratch, scratch, failure->label());
      break;
    }
    default:
      MOZ_CRASH("Unknown arg format");
  }

  // Ensure that callee argc does not exceed the limit.
  masm.branch32(Assembler::Above, scratch, Imm32(JIT_ARGS_LENGTH_MAX),
                failure->label());

  // We're past the final guard. Update argc with the new value.
  masm.move32(scratch, argcReg);

  return true;
}

void BaselineCacheIRCompiler::pushArguments(Register argcReg,
                                            Register calleeReg,
                                            Register scratch, Register scratch2,
                                            CallFlags flags, uint32_t argcFixed,
                                            bool isJitCall) {
  switch (flags.getArgFormat()) {
    case CallFlags::Standard:
      pushStandardArguments(argcReg, scratch, scratch2, argcFixed, isJitCall,
                            flags.isConstructing());
      break;
    case CallFlags::Spread:
      pushArrayArguments(argcReg, scratch, scratch2, isJitCall,
                         flags.isConstructing());
      break;
    case CallFlags::FunCall:
      pushFunCallArguments(argcReg, calleeReg, scratch, scratch2, argcFixed,
                           isJitCall);
      break;
    case CallFlags::FunApplyArgsObj:
      pushFunApplyArgsObj(argcReg, calleeReg, scratch, scratch2, isJitCall);
      break;
    case CallFlags::FunApplyArray:
      pushArrayArguments(argcReg, scratch, scratch2, isJitCall,
                         /*isConstructing =*/false);
      break;
    case CallFlags::FunApplyNullUndefined:
      pushFunApplyNullUndefinedArguments(calleeReg, isJitCall);
      break;
    default:
      MOZ_CRASH("Invalid arg format");
  }
}

void BaselineCacheIRCompiler::pushStandardArguments(
    Register argcReg, Register scratch, Register scratch2, uint32_t argcFixed,
    bool isJitCall, bool isConstructing) {
  MOZ_ASSERT(enteredStubFrame_);

  // The arguments to the call IC are pushed on the stack left-to-right.
  // Our calling conventions want them right-to-left in the callee, so
  // we duplicate them on the stack in reverse order.

  int additionalArgc = 1 + !isJitCall + isConstructing;
  if (argcFixed < MaxUnrolledArgCopy) {
#ifdef DEBUG
    Label ok;
    masm.branch32(Assembler::Equal, argcReg, Imm32(argcFixed), &ok);
    masm.assumeUnreachable("Invalid argcFixed value");
    masm.bind(&ok);
#endif

    size_t realArgc = argcFixed + additionalArgc;

    if (isJitCall) {
      masm.alignJitStackBasedOnNArgs(realArgc, /*countIncludesThis = */ true);
    }

    for (size_t i = 0; i < realArgc; ++i) {
      masm.pushValue(Address(
          FramePointer, BaselineStubFrameLayout::Size() + i * sizeof(Value)));
    }
  } else {
    MOZ_ASSERT(argcFixed == MaxUnrolledArgCopy);

    // argPtr initially points to the last argument. Skip the stub frame.
    Register argPtr = scratch2;
    Address argAddress(FramePointer, BaselineStubFrameLayout::Size());
    masm.computeEffectiveAddress(argAddress, argPtr);

    // countReg contains the total number of arguments to copy.
    // In addition to the actual arguments, we have to copy hidden arguments.
    // We always have to copy |this|.
    // If we are constructing, we have to copy |newTarget|.
    // If we are not a jit call, we have to copy |callee|.
    // We use a scratch register to avoid clobbering argc, which is an input
    // reg.
    Register countReg = scratch;
    masm.move32(argcReg, countReg);
    masm.add32(Imm32(additionalArgc), countReg);

    // Align the stack such that the JitFrameLayout is aligned on the
    // JitStackAlignment.
    if (isJitCall) {
      masm.alignJitStackBasedOnNArgs(countReg, /*countIncludesThis = */ true);
    }

    // Push all values, starting at the last one.
    Label loop, done;
    masm.branchTest32(Assembler::Zero, countReg, countReg, &done);
    masm.bind(&loop);
    {
      masm.pushValue(Address(argPtr, 0));
      masm.addPtr(Imm32(sizeof(Value)), argPtr);

      masm.branchSub32(Assembler::NonZero, Imm32(1), countReg, &loop);
    }
    masm.bind(&done);
  }
}

void BaselineCacheIRCompiler::pushArrayArguments(Register argcReg,
                                                 Register scratch,
                                                 Register scratch2,
                                                 bool isJitCall,
                                                 bool isConstructing) {
  MOZ_ASSERT(enteredStubFrame_);

  // Pull the array off the stack before aligning.
  Register startReg = scratch;
  size_t arrayOffset =
      (isConstructing * sizeof(Value)) + BaselineStubFrameLayout::Size();
  masm.unboxObject(Address(FramePointer, arrayOffset), startReg);
  masm.loadPtr(Address(startReg, NativeObject::offsetOfElements()), startReg);

  // Align the stack such that the JitFrameLayout is aligned on the
  // JitStackAlignment.
  if (isJitCall) {
    Register alignReg = argcReg;
    if (isConstructing) {
      // If we are constructing, we must take newTarget into account.
      alignReg = scratch2;
      masm.computeEffectiveAddress(Address(argcReg, 1), alignReg);
    }
    masm.alignJitStackBasedOnNArgs(alignReg, /*countIncludesThis =*/false);
  }

  // Push newTarget, if necessary
  if (isConstructing) {
    masm.pushValue(Address(FramePointer, BaselineStubFrameLayout::Size()));
  }

  // Push arguments: set up endReg to point to &array[argc]
  Register endReg = scratch2;
  BaseValueIndex endAddr(startReg, argcReg);
  masm.computeEffectiveAddress(endAddr, endReg);

  // Copying pre-decrements endReg by 8 until startReg is reached
  Label copyDone;
  Label copyStart;
  masm.bind(&copyStart);
  masm.branchPtr(Assembler::Equal, endReg, startReg, &copyDone);
  masm.subPtr(Imm32(sizeof(Value)), endReg);
  masm.pushValue(Address(endReg, 0));
  masm.jump(&copyStart);
  masm.bind(&copyDone);

  // Push |this|.
  size_t thisvOffset =
      BaselineStubFrameLayout::Size() + (1 + isConstructing) * sizeof(Value);
  masm.pushValue(Address(FramePointer, thisvOffset));

  // Push |callee| if needed.
  if (!isJitCall) {
    size_t calleeOffset =
        BaselineStubFrameLayout::Size() + (2 + isConstructing) * sizeof(Value);
    masm.pushValue(Address(FramePointer, calleeOffset));
  }
}

void BaselineCacheIRCompiler::pushFunApplyNullUndefinedArguments(
    Register calleeReg, bool isJitCall) {
  // argc is already set to 0, so we just have to push |this| and (for native
  // calls) the callee.

  MOZ_ASSERT(enteredStubFrame_);

  // Align the stack such that the JitFrameLayout is aligned on the
  // JitStackAlignment.
  if (isJitCall) {
    masm.alignJitStackBasedOnNArgs(0, /*countIncludesThis =*/false);
  }

  // Push |this|.
  size_t thisvOffset = BaselineStubFrameLayout::Size() + 1 * sizeof(Value);
  masm.pushValue(Address(FramePointer, thisvOffset));

  // Push |callee| if needed.
  if (!isJitCall) {
    masm.Push(TypedOrValueRegister(MIRType::Object, AnyRegister(calleeReg)));
  }
}

void BaselineCacheIRCompiler::pushFunCallArguments(
    Register argcReg, Register calleeReg, Register scratch, Register scratch2,
    uint32_t argcFixed, bool isJitCall) {
  if (argcFixed == 0) {
    if (isJitCall) {
      // Align the stack to 0 args.
      masm.alignJitStackBasedOnNArgs(0, /*countIncludesThis = */ false);
    }

    // Store the new |this|.
    masm.pushValue(UndefinedValue());

    // Store |callee| if needed.
    if (!isJitCall) {
      masm.Push(TypedOrValueRegister(MIRType::Object, AnyRegister(calleeReg)));
    }
  } else if (argcFixed < MaxUnrolledArgCopy) {
    // See below for why we subtract 1 from argcFixed.
    argcFixed -= 1;
    masm.sub32(Imm32(1), argcReg);
    pushStandardArguments(argcReg, scratch, scratch2, argcFixed, isJitCall,
                          /*isConstructing =*/false);
  } else {
    Label zeroArgs, done;
    masm.branchTest32(Assembler::Zero, argcReg, argcReg, &zeroArgs);

    // When we call fun_call, the stack looks like the left column (note
    // that newTarget will not be present, because fun_call cannot be a
    // constructor call):
    //
    // ***Arguments to fun_call***
    // callee (fun_call)               ***Arguments to target***
    // this (target function)   -----> callee
    // arg0 (this of target)    -----> this
    // arg1 (arg0 of target)    -----> arg0
    // argN (argN-1 of target)  -----> arg1
    //
    // As demonstrated in the right column, this is exactly what we need
    // the stack to look like when calling pushStandardArguments for target,
    // except with one more argument. If we subtract 1 from argc,
    // everything works out correctly.
    masm.sub32(Imm32(1), argcReg);

    pushStandardArguments(argcReg, scratch, scratch2, argcFixed, isJitCall,
                          /*isConstructing =*/false);

    masm.jump(&done);
    masm.bind(&zeroArgs);

    // The exception is the case where argc == 0:
    //
    // ***Arguments to fun_call***
    // callee (fun_call)               ***Arguments to target***
    // this (target function)   -----> callee
    // <nothing>                -----> this
    //
    // In this case, we push |undefined| for |this|.

    if (isJitCall) {
      // Align the stack to 0 args.
      masm.alignJitStackBasedOnNArgs(0, /*countIncludesThis = */ false);
    }

    // Store the new |this|.
    masm.pushValue(UndefinedValue());

    // Store |callee| if needed.
    if (!isJitCall) {
      masm.Push(TypedOrValueRegister(MIRType::Object, AnyRegister(calleeReg)));
    }

    masm.bind(&done);
  }
}

void BaselineCacheIRCompiler::pushFunApplyArgsObj(Register argcReg,
                                                  Register calleeReg,
                                                  Register scratch,
                                                  Register scratch2,
                                                  bool isJitCall) {
  MOZ_ASSERT(enteredStubFrame_);

  // Load the arguments object off the stack before aligning.
  Register argsReg = scratch;
  masm.unboxObject(Address(FramePointer, BaselineStubFrameLayout::Size()),
                   argsReg);

  // Align the stack such that the JitFrameLayout is aligned on the
  // JitStackAlignment.
  if (isJitCall) {
    masm.alignJitStackBasedOnNArgs(argcReg, /*countIncludesThis =*/false);
  }

  // Load ArgumentsData.
  masm.loadPrivate(Address(argsReg, ArgumentsObject::getDataSlotOffset()),
                   argsReg);

  // We push the arguments onto the stack last-to-first.
  // Compute the bounds of the arguments array.
  Register currReg = scratch2;
  Address argsStartAddr(argsReg, ArgumentsData::offsetOfArgs());
  masm.computeEffectiveAddress(argsStartAddr, argsReg);
  BaseValueIndex argsEndAddr(argsReg, argcReg);
  masm.computeEffectiveAddress(argsEndAddr, currReg);

  // Loop until all arguments have been pushed.
  Label done, loop;
  masm.bind(&loop);
  masm.branchPtr(Assembler::Equal, currReg, argsReg, &done);
  masm.subPtr(Imm32(sizeof(Value)), currReg);

  Address currArgAddr(currReg, 0);
#ifdef DEBUG
  // Arguments are forwarded to the call object if they are closed over.
  // In this case, OVERRIDDEN_ELEMENTS_BIT should be set.
  Label notForwarded;
  masm.branchTestMagic(Assembler::NotEqual, currArgAddr, &notForwarded);
  masm.assumeUnreachable("Should have checked for overridden elements");
  masm.bind(&notForwarded);
#endif
  masm.pushValue(currArgAddr);

  masm.jump(&loop);
  masm.bind(&done);

  // Push arg0 as |this| for call
  masm.pushValue(
      Address(FramePointer, BaselineStubFrameLayout::Size() + sizeof(Value)));

  // Push |callee| if needed.
  if (!isJitCall) {
    masm.Push(TypedOrValueRegister(MIRType::Object, AnyRegister(calleeReg)));
  }
}

void BaselineCacheIRCompiler::pushBoundFunctionArguments(
    Register argcReg, Register calleeReg, Register scratch, Register scratch2,
    CallFlags flags, uint32_t numBoundArgs, bool isJitCall) {
  bool isConstructing = flags.isConstructing();
  uint32_t additionalArgc = 1 + isConstructing;  // |this| and newTarget

  // Calculate total number of Values to push.
  Register countReg = scratch;
  masm.computeEffectiveAddress(Address(argcReg, numBoundArgs + additionalArgc),
                               countReg);

  // Align the stack such that the JitFrameLayout is aligned on the
  // JitStackAlignment.
  if (isJitCall) {
    masm.alignJitStackBasedOnNArgs(countReg, /*countIncludesThis = */ true);
  }

  if (isConstructing) {
    // Push the bound function's target as newTarget.
    Address boundTarget(calleeReg, BoundFunctionObject::offsetOfTargetSlot());
    masm.pushValue(boundTarget);
  }

  // Ensure argPtr initially points to the last argument. Skip the stub frame.
  Register argPtr = scratch2;
  Address argAddress(FramePointer, BaselineStubFrameLayout::Size());
  if (isConstructing) {
    // Skip newTarget.
    argAddress.offset += sizeof(Value);
  }
  masm.computeEffectiveAddress(argAddress, argPtr);

  // Push all supplied arguments, starting at the last one.
  Label loop, done;
  masm.branchTest32(Assembler::Zero, argcReg, argcReg, &done);
  masm.move32(argcReg, countReg);
  masm.bind(&loop);
  {
    masm.pushValue(Address(argPtr, 0));
    masm.addPtr(Imm32(sizeof(Value)), argPtr);

    masm.branchSub32(Assembler::NonZero, Imm32(1), countReg, &loop);
  }
  masm.bind(&done);

  // Push the bound arguments, starting at the last one.
  constexpr size_t inlineArgsOffset =
      BoundFunctionObject::offsetOfFirstInlineBoundArg();
  if (numBoundArgs <= BoundFunctionObject::MaxInlineBoundArgs) {
    for (size_t i = 0; i < numBoundArgs; i++) {
      size_t argIndex = numBoundArgs - i - 1;
      Address argAddr(calleeReg, inlineArgsOffset + argIndex * sizeof(Value));
      masm.pushValue(argAddr);
    }
  } else {
    masm.unboxObject(Address(calleeReg, inlineArgsOffset), scratch);
    masm.loadPtr(Address(scratch, NativeObject::offsetOfElements()), scratch);
    for (size_t i = 0; i < numBoundArgs; i++) {
      size_t argIndex = numBoundArgs - i - 1;
      Address argAddr(scratch, argIndex * sizeof(Value));
      masm.pushValue(argAddr);
    }
  }

  if (isConstructing) {
    // Push the |this| Value. This is either the object we allocated or the
    // JS_UNINITIALIZED_LEXICAL magic value. It's stored in the BaselineFrame,
    // so skip past the stub frame, (unbound) arguments and newTarget.
    BaseValueIndex thisAddress(FramePointer, argcReg,
                               BaselineStubFrameLayout::Size() + sizeof(Value));
    masm.pushValue(thisAddress, scratch);
  } else {
    // Push the bound |this|.
    Address boundThis(calleeReg, BoundFunctionObject::offsetOfBoundThisSlot());
    masm.pushValue(boundThis);
  }
}

bool BaselineCacheIRCompiler::emitCallNativeShared(
    NativeCallType callType, ObjOperandId calleeId, Int32OperandId argcId,
    CallFlags flags, uint32_t argcFixed, Maybe<bool> ignoresReturnValue,
    Maybe<uint32_t> targetOffset) {
  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  AutoScratchRegister scratch2(allocator, masm);

  Register calleeReg = allocator.useRegister(masm, calleeId);
  Register argcReg = allocator.useRegister(masm, argcId);

  bool isConstructing = flags.isConstructing();
  bool isSameRealm = flags.isSameRealm();

  if (!updateArgc(flags, argcReg, scratch)) {
    return false;
  }

  allocator.discardStack(masm);

  // Push a stub frame so that we can perform a non-tail call.
  // Note that this leaves the return address in TailCallReg.
  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  if (!isSameRealm) {
    masm.switchToObjectRealm(calleeReg, scratch);
  }

  pushArguments(argcReg, calleeReg, scratch, scratch2, flags, argcFixed,
                /*isJitCall =*/false);

  // Native functions have the signature:
  //
  //    bool (*)(JSContext*, unsigned, Value* vp)
  //
  // Where vp[0] is space for callee/return value, vp[1] is |this|, and vp[2]
  // onward are the function arguments.

  // Initialize vp.
  masm.moveStackPtrTo(scratch2.get());

  // Construct a native exit frame.
  masm.push(argcReg);

  masm.pushFrameDescriptor(FrameType::BaselineStub);
  masm.push(ICTailCallReg);
  masm.push(FramePointer);
  masm.loadJSContext(scratch);
  masm.enterFakeExitFrameForNative(scratch, scratch, isConstructing);

  // Execute call.
  masm.setupUnalignedABICall(scratch);
  masm.loadJSContext(scratch);
  masm.passABIArg(scratch);
  masm.passABIArg(argcReg);
  masm.passABIArg(scratch2);

  switch (callType) {
    case NativeCallType::Native: {
#ifdef JS_SIMULATOR
      // The simulator requires VM calls to be redirected to a special
      // swi instruction to handle them, so we store the redirected
      // pointer in the stub and use that instead of the original one.
      // (See CacheIRWriter::callNativeFunction.)
      Address redirectedAddr(stubAddress(*targetOffset));
      masm.callWithABI(redirectedAddr);
#else
      if (*ignoresReturnValue) {
        masm.loadPrivate(
            Address(calleeReg, JSFunction::offsetOfJitInfoOrScript()),
            calleeReg);
        masm.callWithABI(
            Address(calleeReg, JSJitInfo::offsetOfIgnoresReturnValueNative()));
      } else {
        // This depends on the native function pointer being stored unchanged as
        // a PrivateValue.
        masm.callWithABI(Address(calleeReg, JSFunction::offsetOfNativeOrEnv()));
      }
#endif
    } break;
    case NativeCallType::ClassHook: {
      Address nativeAddr(stubAddress(*targetOffset));
      masm.callWithABI(nativeAddr);
    } break;
  }

  // Test for failure.
  masm.branchIfFalseBool(ReturnReg, masm.exceptionLabel());

  // Load the return value.
  masm.loadValue(
      Address(masm.getStackPointer(), NativeExitFrameLayout::offsetOfResult()),
      output.valueReg());

  stubFrame.leave(masm);

  if (!isSameRealm) {
    masm.switchToBaselineFrameRealm(scratch2);
  }

  return true;
}

#ifdef JS_SIMULATOR
bool BaselineCacheIRCompiler::emitCallNativeFunction(ObjOperandId calleeId,
                                                     Int32OperandId argcId,
                                                     CallFlags flags,
                                                     uint32_t argcFixed,
                                                     uint32_t targetOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Maybe<bool> ignoresReturnValue;
  Maybe<uint32_t> targetOffset_ = mozilla::Some(targetOffset);
  return emitCallNativeShared(NativeCallType::Native, calleeId, argcId, flags,
                              argcFixed, ignoresReturnValue, targetOffset_);
}

bool BaselineCacheIRCompiler::emitCallDOMFunction(
    ObjOperandId calleeId, Int32OperandId argcId, ObjOperandId thisObjId,
    CallFlags flags, uint32_t argcFixed, uint32_t targetOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Maybe<bool> ignoresReturnValue;
  Maybe<uint32_t> targetOffset_ = mozilla::Some(targetOffset);
  return emitCallNativeShared(NativeCallType::Native, calleeId, argcId, flags,
                              argcFixed, ignoresReturnValue, targetOffset_);
}
#else
bool BaselineCacheIRCompiler::emitCallNativeFunction(ObjOperandId calleeId,
                                                     Int32OperandId argcId,
                                                     CallFlags flags,
                                                     uint32_t argcFixed,
                                                     bool ignoresReturnValue) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Maybe<bool> ignoresReturnValue_ = mozilla::Some(ignoresReturnValue);
  Maybe<uint32_t> targetOffset;
  return emitCallNativeShared(NativeCallType::Native, calleeId, argcId, flags,
                              argcFixed, ignoresReturnValue_, targetOffset);
}

bool BaselineCacheIRCompiler::emitCallDOMFunction(ObjOperandId calleeId,
                                                  Int32OperandId argcId,
                                                  ObjOperandId thisObjId,
                                                  CallFlags flags,
                                                  uint32_t argcFixed) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Maybe<bool> ignoresReturnValue = mozilla::Some(false);
  Maybe<uint32_t> targetOffset;
  return emitCallNativeShared(NativeCallType::Native, calleeId, argcId, flags,
                              argcFixed, ignoresReturnValue, targetOffset);
}
#endif

bool BaselineCacheIRCompiler::emitCallClassHook(ObjOperandId calleeId,
                                                Int32OperandId argcId,
                                                CallFlags flags,
                                                uint32_t argcFixed,
                                                uint32_t targetOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Maybe<bool> ignoresReturnValue;
  Maybe<uint32_t> targetOffset_ = mozilla::Some(targetOffset);
  return emitCallNativeShared(NativeCallType::ClassHook, calleeId, argcId,
                              flags, argcFixed, ignoresReturnValue,
                              targetOffset_);
}

// Helper function for loading call arguments from the stack.  Loads
// and unboxes an object from a specific slot.
void BaselineCacheIRCompiler::loadStackObject(ArgumentKind kind,
                                              CallFlags flags, Register argcReg,
                                              Register dest) {
  MOZ_ASSERT(enteredStubFrame_);

  bool addArgc = false;
  int32_t slotIndex = GetIndexOfArgument(kind, flags, &addArgc);

  if (addArgc) {
    int32_t slotOffset =
        slotIndex * sizeof(JS::Value) + BaselineStubFrameLayout::Size();
    BaseValueIndex slotAddr(FramePointer, argcReg, slotOffset);
    masm.unboxObject(slotAddr, dest);
  } else {
    int32_t slotOffset =
        slotIndex * sizeof(JS::Value) + BaselineStubFrameLayout::Size();
    Address slotAddr(FramePointer, slotOffset);
    masm.unboxObject(slotAddr, dest);
  }
}

template <typename T>
void BaselineCacheIRCompiler::storeThis(const T& newThis, Register argcReg,
                                        CallFlags flags) {
  switch (flags.getArgFormat()) {
    case CallFlags::Standard: {
      BaseValueIndex thisAddress(
          FramePointer,
          argcReg,                               // Arguments
          1 * sizeof(Value) +                    // NewTarget
              BaselineStubFrameLayout::Size());  // Stub frame
      masm.storeValue(newThis, thisAddress);
    } break;
    case CallFlags::Spread: {
      Address thisAddress(FramePointer,
                          2 * sizeof(Value) +  // Arg array, NewTarget
                              BaselineStubFrameLayout::Size());  // Stub frame
      masm.storeValue(newThis, thisAddress);
    } break;
    default:
      MOZ_CRASH("Invalid arg format for scripted constructor");
  }
}

/*
 * Scripted constructors require a |this| object to be created prior to the
 * call. When this function is called, the stack looks like (bottom->top):
 *
 * [..., Callee, ThisV, Arg0V, ..., ArgNV, NewTarget, StubFrameHeader]
 *
 * At this point, |ThisV| is JSWhyMagic::JS_IS_CONSTRUCTING.
 *
 * This function calls CreateThis to generate a new |this| object, then
 * overwrites the magic ThisV on the stack.
 */
void BaselineCacheIRCompiler::createThis(Register argcReg, Register calleeReg,
                                         Register scratch, CallFlags flags,
                                         bool isBoundFunction) {
  MOZ_ASSERT(flags.isConstructing());

  if (flags.needsUninitializedThis()) {
    storeThis(MagicValue(JS_UNINITIALIZED_LEXICAL), argcReg, flags);
    return;
  }

  // Save live registers that don't have to be traced.
  LiveGeneralRegisterSet liveNonGCRegs;
  liveNonGCRegs.add(argcReg);
  liveNonGCRegs.add(ICStubReg);
  masm.PushRegsInMask(liveNonGCRegs);

  // CreateThis takes two arguments: callee, and newTarget.

  if (isBoundFunction) {
    // Push the bound function's target as callee and newTarget.
    Address boundTarget(calleeReg, BoundFunctionObject::offsetOfTargetSlot());
    masm.unboxObject(boundTarget, scratch);
    masm.push(scratch);
    masm.push(scratch);
  } else {
    // Push newTarget:
    loadStackObject(ArgumentKind::NewTarget, flags, argcReg, scratch);
    masm.push(scratch);

    // Push callee:
    loadStackObject(ArgumentKind::Callee, flags, argcReg, scratch);
    masm.push(scratch);
  }

  // Call CreateThisFromIC.
  using Fn =
      bool (*)(JSContext*, HandleObject, HandleObject, MutableHandleValue);
  callVM<Fn, CreateThisFromIC>(masm);

#ifdef DEBUG
  Label createdThisOK;
  masm.branchTestObject(Assembler::Equal, JSReturnOperand, &createdThisOK);
  masm.branchTestMagic(Assembler::Equal, JSReturnOperand, &createdThisOK);
  masm.assumeUnreachable(
      "The return of CreateThis must be an object or uninitialized.");
  masm.bind(&createdThisOK);
#endif

  // Restore saved registers.
  masm.PopRegsInMask(liveNonGCRegs);

  // Save |this| value back into pushed arguments on stack.
  MOZ_ASSERT(!liveNonGCRegs.aliases(JSReturnOperand));
  storeThis(JSReturnOperand, argcReg, flags);

  // Restore calleeReg. CreateThisFromIC may trigger a GC, so we reload the
  // callee from the stub frame (which is traced) instead of spilling it to
  // the stack.
  loadStackObject(ArgumentKind::Callee, flags, argcReg, calleeReg);
}

void BaselineCacheIRCompiler::updateReturnValue() {
  Label skipThisReplace;
  masm.branchTestObject(Assembler::Equal, JSReturnOperand, &skipThisReplace);

  // If a constructor does not explicitly return an object, the return value
  // of the constructor is |this|. We load it out of the baseline stub frame.

  // At this point, the stack looks like this:
  //  newTarget
  //  ArgN
  //  ...
  //  Arg0
  //  ThisVal         <---- We want this value.
  //  Callee token          | Skip two stack slots.
  //  Frame descriptor      v
  //  [Top of stack]
  size_t thisvOffset =
      JitFrameLayout::offsetOfThis() - JitFrameLayout::bytesPoppedAfterCall();
  Address thisAddress(masm.getStackPointer(), thisvOffset);
  masm.loadValue(thisAddress, JSReturnOperand);

#ifdef DEBUG
  masm.branchTestObject(Assembler::Equal, JSReturnOperand, &skipThisReplace);
  masm.assumeUnreachable("Return of constructing call should be an object.");
#endif
  masm.bind(&skipThisReplace);
}

bool BaselineCacheIRCompiler::emitCallScriptedFunction(ObjOperandId calleeId,
                                                       Int32OperandId argcId,
                                                       CallFlags flags,
                                                       uint32_t argcFixed) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  AutoScratchRegister scratch2(allocator, masm);

  Register calleeReg = allocator.useRegister(masm, calleeId);
  Register argcReg = allocator.useRegister(masm, argcId);

  bool isConstructing = flags.isConstructing();
  bool isSameRealm = flags.isSameRealm();

  if (!updateArgc(flags, argcReg, scratch)) {
    return false;
  }

  allocator.discardStack(masm);

  // Push a stub frame so that we can perform a non-tail call.
  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  if (!isSameRealm) {
    masm.switchToObjectRealm(calleeReg, scratch);
  }

  if (isConstructing) {
    createThis(argcReg, calleeReg, scratch, flags,
               /* isBoundFunction = */ false);
  }

  pushArguments(argcReg, calleeReg, scratch, scratch2, flags, argcFixed,
                /*isJitCall =*/true);

  // Load the start of the target JitCode.
  Register code = scratch2;
  masm.loadJitCodeRaw(calleeReg, code);

  // Note that we use Push, not push, so that callJit will align the stack
  // properly on ARM.
  masm.PushCalleeToken(calleeReg, isConstructing);
  masm.PushFrameDescriptorForJitCall(FrameType::BaselineStub, argcReg, scratch);

  // Handle arguments underflow.
  Label noUnderflow;
  masm.loadFunctionArgCount(calleeReg, calleeReg);
  masm.branch32(Assembler::AboveOrEqual, argcReg, calleeReg, &noUnderflow);
  {
    // Call the arguments rectifier.
    TrampolinePtr argumentsRectifier =
        cx_->runtime()->jitRuntime()->getArgumentsRectifier();
    masm.movePtr(argumentsRectifier, code);
  }

  masm.bind(&noUnderflow);
  masm.callJit(code);

  // If this is a constructing call, and the callee returns a non-object,
  // replace it with the |this| object passed in.
  if (isConstructing) {
    updateReturnValue();
  }

  stubFrame.leave(masm);

  if (!isSameRealm) {
    masm.switchToBaselineFrameRealm(scratch2);
  }

  return true;
}

bool BaselineCacheIRCompiler::emitCallWasmFunction(
    ObjOperandId calleeId, Int32OperandId argcId, CallFlags flags,
    uint32_t argcFixed, uint32_t funcExportOffset, uint32_t instanceOffset) {
  return emitCallScriptedFunction(calleeId, argcId, flags, argcFixed);
}

bool BaselineCacheIRCompiler::emitCallInlinedFunction(ObjOperandId calleeId,
                                                      Int32OperandId argcId,
                                                      uint32_t icScriptOffset,
                                                      CallFlags flags,
                                                      uint32_t argcFixed) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  AutoScratchRegisterMaybeOutputType scratch2(allocator, masm, output);
  AutoScratchRegister codeReg(allocator, masm);

  Register calleeReg = allocator.useRegister(masm, calleeId);
  Register argcReg = allocator.useRegister(masm, argcId);

  bool isConstructing = flags.isConstructing();
  bool isSameRealm = flags.isSameRealm();

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadBaselineJitCodeRaw(calleeReg, codeReg, failure->label());

  if (!updateArgc(flags, argcReg, scratch)) {
    return false;
  }

  allocator.discardStack(masm);

  // Push a stub frame so that we can perform a non-tail call.
  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  if (!isSameRealm) {
    masm.switchToObjectRealm(calleeReg, scratch);
  }

  Label baselineScriptDiscarded;
  if (isConstructing) {
    createThis(argcReg, calleeReg, scratch, flags,
               /* isBoundFunction = */ false);

    // CreateThisFromIC may trigger a GC and discard the BaselineScript.
    // We have already called discardStack, so we can't use a FailurePath.
    // Instead, we skip storing the ICScript in the JSContext and use a
    // normal non-inlined call.
    masm.loadBaselineJitCodeRaw(calleeReg, codeReg, &baselineScriptDiscarded);
  }

  // Store icScript in the context.
  Address icScriptAddr(stubAddress(icScriptOffset));
  masm.loadPtr(icScriptAddr, scratch);
  masm.storeICScriptInJSContext(scratch);

  if (isConstructing) {
    Label skip;
    masm.jump(&skip);
    masm.bind(&baselineScriptDiscarded);
    masm.loadJitCodeRaw(calleeReg, codeReg);
    masm.bind(&skip);
  }

  pushArguments(argcReg, calleeReg, scratch, scratch2, flags, argcFixed,
                /*isJitCall =*/true);

  // Note that we use Push, not push, so that callJit will align the stack
  // properly on ARM.
  masm.PushCalleeToken(calleeReg, isConstructing);
  masm.PushFrameDescriptorForJitCall(FrameType::BaselineStub, argcReg, scratch);

  // Handle arguments underflow.
  Label noUnderflow;
  masm.loadFunctionArgCount(calleeReg, calleeReg);
  masm.branch32(Assembler::AboveOrEqual, argcReg, calleeReg, &noUnderflow);

  // Call the trial-inlining arguments rectifier.
  ArgumentsRectifierKind kind = ArgumentsRectifierKind::TrialInlining;
  TrampolinePtr argumentsRectifier =
      cx_->runtime()->jitRuntime()->getArgumentsRectifier(kind);
  masm.movePtr(argumentsRectifier, codeReg);

  masm.bind(&noUnderflow);
  masm.callJit(codeReg);

  // If this is a constructing call, and the callee returns a non-object,
  // replace it with the |this| object passed in.
  if (isConstructing) {
    updateReturnValue();
  }

  stubFrame.leave(masm);

  if (!isSameRealm) {
    masm.switchToBaselineFrameRealm(codeReg);
  }

  return true;
}

bool BaselineCacheIRCompiler::emitCallBoundScriptedFunction(
    ObjOperandId calleeId, ObjOperandId targetId, Int32OperandId argcId,
    CallFlags flags, uint32_t numBoundArgs) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  AutoScratchRegister scratch2(allocator, masm);

  Register calleeReg = allocator.useRegister(masm, calleeId);
  Register argcReg = allocator.useRegister(masm, argcId);

  bool isConstructing = flags.isConstructing();
  bool isSameRealm = flags.isSameRealm();

  allocator.discardStack(masm);

  // Push a stub frame so that we can perform a non-tail call.
  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  Address boundTarget(calleeReg, BoundFunctionObject::offsetOfTargetSlot());

  // If we're constructing, switch to the target's realm and create |this|. If
  // we're not constructing, we switch to the target's realm after pushing the
  // arguments and loading the target.
  if (isConstructing) {
    if (!isSameRealm) {
      masm.unboxObject(boundTarget, scratch);
      masm.switchToObjectRealm(scratch, scratch);
    }
    createThis(argcReg, calleeReg, scratch, flags,
               /* isBoundFunction = */ true);
  }

  // Push all arguments, including |this|.
  pushBoundFunctionArguments(argcReg, calleeReg, scratch, scratch2, flags,
                             numBoundArgs, /* isJitCall = */ true);

  // Load the target JSFunction.
  masm.unboxObject(boundTarget, calleeReg);

  if (!isConstructing && !isSameRealm) {
    masm.switchToObjectRealm(calleeReg, scratch);
  }

  // Update argc.
  masm.add32(Imm32(numBoundArgs), argcReg);

  // Load the start of the target JitCode.
  Register code = scratch2;
  masm.loadJitCodeRaw(calleeReg, code);

  // Note that we use Push, not push, so that callJit will align the stack
  // properly on ARM.
  masm.PushCalleeToken(calleeReg, isConstructing);
  masm.PushFrameDescriptorForJitCall(FrameType::BaselineStub, argcReg, scratch);

  // Handle arguments underflow.
  Label noUnderflow;
  masm.loadFunctionArgCount(calleeReg, calleeReg);
  masm.branch32(Assembler::AboveOrEqual, argcReg, calleeReg, &noUnderflow);
  {
    // Call the arguments rectifier.
    TrampolinePtr argumentsRectifier =
        cx_->runtime()->jitRuntime()->getArgumentsRectifier();
    masm.movePtr(argumentsRectifier, code);
  }

  masm.bind(&noUnderflow);
  masm.callJit(code);

  if (isConstructing) {
    updateReturnValue();
  }

  stubFrame.leave(masm);

  if (!isSameRealm) {
    masm.switchToBaselineFrameRealm(scratch2);
  }

  return true;
}

bool BaselineCacheIRCompiler::emitNewArrayObjectResult(uint32_t arrayLength,
                                                       uint32_t shapeOffset,
                                                       uint32_t siteOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  gc::AllocKind allocKind = GuessArrayGCKind(arrayLength);
  MOZ_ASSERT(CanChangeToBackgroundAllocKind(allocKind, &ArrayObject::class_));
  allocKind = ForegroundToBackgroundAllocKind(allocKind);

  uint32_t slotCount = GetGCKindSlots(allocKind);
  MOZ_ASSERT(slotCount >= ObjectElements::VALUES_PER_HEADER);
  uint32_t arrayCapacity = slotCount - ObjectElements::VALUES_PER_HEADER;

  AutoOutputRegister output(*this);
  AutoScratchRegister result(allocator, masm);
  AutoScratchRegister scratch(allocator, masm);
  AutoScratchRegister site(allocator, masm);
  AutoScratchRegisterMaybeOutput shape(allocator, masm, output);

  Address shapeAddr(stubAddress(shapeOffset));
  masm.loadPtr(shapeAddr, shape);

  Address siteAddr(stubAddress(siteOffset));
  masm.loadPtr(siteAddr, site);

  allocator.discardStack(masm);

  Label done;
  Label fail;

  masm.createArrayWithFixedElements(result, shape, scratch, arrayLength,
                                    arrayCapacity, allocKind, gc::Heap::Default,
                                    &fail, AllocSiteInput(site));
  masm.jump(&done);

  {
    masm.bind(&fail);

    // We get here if the nursery is full (unlikely) but also for tenured
    // allocations if the current arena is full and we need to allocate a new
    // one (fairly common).

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch);

    masm.Push(site);
    masm.Push(Imm32(int32_t(allocKind)));
    masm.Push(Imm32(arrayLength));

    using Fn =
        ArrayObject* (*)(JSContext*, uint32_t, gc::AllocKind, gc::AllocSite*);
    callVM<Fn, NewArrayObjectBaselineFallback>(masm);

    stubFrame.leave(masm);
    masm.storeCallPointerResult(result);
  }

  masm.bind(&done);
  masm.tagValue(JSVAL_TYPE_OBJECT, result, output.valueReg());
  return true;
}

bool BaselineCacheIRCompiler::emitNewPlainObjectResult(uint32_t numFixedSlots,
                                                       uint32_t numDynamicSlots,
                                                       gc::AllocKind allocKind,
                                                       uint32_t shapeOffset,
                                                       uint32_t siteOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegister obj(allocator, masm);
  AutoScratchRegister scratch(allocator, masm);
  AutoScratchRegister site(allocator, masm);
  AutoScratchRegisterMaybeOutput shape(allocator, masm, output);

  Address shapeAddr(stubAddress(shapeOffset));
  masm.loadPtr(shapeAddr, shape);

  Address siteAddr(stubAddress(siteOffset));
  masm.loadPtr(siteAddr, site);

  allocator.discardStack(masm);

  Label done;
  Label fail;

  masm.createPlainGCObject(obj, shape, scratch, shape, numFixedSlots,
                           numDynamicSlots, allocKind, gc::Heap::Default, &fail,
                           AllocSiteInput(site));
  masm.jump(&done);

  {
    masm.bind(&fail);

    // We get here if the nursery is full (unlikely) but also for tenured
    // allocations if the current arena is full and we need to allocate a new
    // one (fairly common).

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch);

    masm.Push(site);
    masm.Push(Imm32(int32_t(allocKind)));
    masm.loadPtr(shapeAddr, shape);  // This might have been overwritten.
    masm.Push(shape);

    using Fn = JSObject* (*)(JSContext*, Handle<SharedShape*>, gc::AllocKind,
                             gc::AllocSite*);
    callVM<Fn, NewPlainObjectBaselineFallback>(masm);

    stubFrame.leave(masm);
    masm.storeCallPointerResult(obj);
  }

  masm.bind(&done);
  masm.tagValue(JSVAL_TYPE_OBJECT, obj, output.valueReg());
  return true;
}

bool BaselineCacheIRCompiler::emitBindFunctionResult(
    ObjOperandId targetId, uint32_t argc, uint32_t templateObjectOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegister scratch(allocator, masm);

  Register target = allocator.useRegister(masm, targetId);

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  // Push the arguments in reverse order.
  for (uint32_t i = 0; i < argc; i++) {
    Address argAddress(FramePointer,
                       BaselineStubFrameLayout::Size() + i * sizeof(Value));
    masm.pushValue(argAddress);
  }
  masm.moveStackPtrTo(scratch.get());

  masm.Push(ImmWord(0));  // nullptr for maybeBound
  masm.Push(Imm32(argc));
  masm.Push(scratch);
  masm.Push(target);

  using Fn = BoundFunctionObject* (*)(JSContext*, Handle<JSObject*>, Value*,
                                      uint32_t, Handle<BoundFunctionObject*>);
  callVM<Fn, BoundFunctionObject::functionBindImpl>(masm);

  stubFrame.leave(masm);
  masm.storeCallPointerResult(scratch);

  masm.tagValue(JSVAL_TYPE_OBJECT, scratch, output.valueReg());
  return true;
}

bool BaselineCacheIRCompiler::emitSpecializedBindFunctionResult(
    ObjOperandId targetId, uint32_t argc, uint32_t templateObjectOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch1(allocator, masm);
  AutoScratchRegister scratch2(allocator, masm);

  Register target = allocator.useRegister(masm, targetId);

  StubFieldOffset objectField(templateObjectOffset, StubField::Type::JSObject);
  emitLoadStubField(objectField, scratch2);

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch1);

  // Push the arguments in reverse order.
  for (uint32_t i = 0; i < argc; i++) {
    Address argAddress(FramePointer,
                       BaselineStubFrameLayout::Size() + i * sizeof(Value));
    masm.pushValue(argAddress);
  }
  masm.moveStackPtrTo(scratch1.get());

  masm.Push(scratch2);
  masm.Push(Imm32(argc));
  masm.Push(scratch1);
  masm.Push(target);

  using Fn = BoundFunctionObject* (*)(JSContext*, Handle<JSObject*>, Value*,
                                      uint32_t, Handle<BoundFunctionObject*>);
  callVM<Fn, BoundFunctionObject::functionBindSpecializedBaseline>(masm);

  stubFrame.leave(masm);
  masm.storeCallPointerResult(scratch1);

  masm.tagValue(JSVAL_TYPE_OBJECT, scratch1, output.valueReg());
  return true;
}

bool BaselineCacheIRCompiler::emitCloseIterScriptedResult(
    ObjOperandId iterId, ObjOperandId calleeId, CompletionKind kind,
    uint32_t calleeNargs) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register iter = allocator.useRegister(masm, iterId);
  Register callee = allocator.useRegister(masm, calleeId);

  AutoScratchRegister code(allocator, masm);
  AutoScratchRegister scratch(allocator, masm);

  masm.loadJitCodeRaw(callee, code);

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  // Call the return method.
  masm.alignJitStackBasedOnNArgs(calleeNargs, /*countIncludesThis = */ false);
  for (uint32_t i = 0; i < calleeNargs; i++) {
    masm.pushValue(UndefinedValue());
  }
  masm.Push(TypedOrValueRegister(MIRType::Object, AnyRegister(iter)));
  masm.Push(callee);
  masm.PushFrameDescriptorForJitCall(FrameType::BaselineStub, /* argc = */ 0);

  masm.callJit(code);

  if (kind != CompletionKind::Throw) {
    // Verify that the return value is an object.
    Label success;
    masm.branchTestObject(Assembler::Equal, JSReturnOperand, &success);

    masm.Push(Imm32(int32_t(CheckIsObjectKind::IteratorReturn)));
    using Fn = bool (*)(JSContext*, CheckIsObjectKind);
    callVM<Fn, ThrowCheckIsObject>(masm);

    masm.bind(&success);
  }

  stubFrame.leave(masm);
  return true;
}

static void CallRegExpStub(MacroAssembler& masm, size_t jitRealmStubOffset,
                           Register temp, Label* vmCall) {
  // Call cx->realm()->jitRealm()->regExpStub. We store a pointer to the RegExp
  // stub in the IC stub to keep it alive, but we shouldn't use it if the stub
  // has been discarded in the meantime (because we might have changed GC string
  // pretenuring heuristics that affect behavior of the stub). This is uncommon
  // but can happen if we discarded all JIT code but had some active (Baseline)
  // scripts on the stack.
  masm.loadJSContext(temp);
  masm.loadPtr(Address(temp, JSContext::offsetOfRealm()), temp);
  masm.loadPtr(Address(temp, Realm::offsetOfJitRealm()), temp);
  masm.loadPtr(Address(temp, jitRealmStubOffset), temp);
  masm.branchTestPtr(Assembler::Zero, temp, temp, vmCall);
  masm.call(Address(temp, JitCode::offsetOfCode()));
}

// Used to move inputs to the registers expected by the RegExp stub.
static void SetRegExpStubInputRegisters(MacroAssembler& masm,
                                        Register* regexpSrc,
                                        Register regexpDest, Register* inputSrc,
                                        Register inputDest,
                                        Register* lastIndexSrc,
                                        Register lastIndexDest) {
  MoveResolver& moves = masm.moveResolver();
  if (*regexpSrc != regexpDest) {
    masm.propagateOOM(moves.addMove(MoveOperand(*regexpSrc),
                                    MoveOperand(regexpDest), MoveOp::GENERAL));
    *regexpSrc = regexpDest;
  }
  if (*inputSrc != inputDest) {
    masm.propagateOOM(moves.addMove(MoveOperand(*inputSrc),
                                    MoveOperand(inputDest), MoveOp::GENERAL));
    *inputSrc = inputDest;
  }
  if (lastIndexSrc && *lastIndexSrc != lastIndexDest) {
    masm.propagateOOM(moves.addMove(MoveOperand(*lastIndexSrc),
                                    MoveOperand(lastIndexDest), MoveOp::INT32));
    *lastIndexSrc = lastIndexDest;
  }

  masm.propagateOOM(moves.resolve());

  MoveEmitter emitter(masm);
  emitter.emit(moves);
  emitter.finish();
}

bool BaselineCacheIRCompiler::emitCallRegExpMatcherResult(
    ObjOperandId regexpId, StringOperandId inputId, Int32OperandId lastIndexId,
    uint32_t stubOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register regexp = allocator.useRegister(masm, regexpId);
  Register input = allocator.useRegister(masm, inputId);
  Register lastIndex = allocator.useRegister(masm, lastIndexId);
  Register scratch = output.valueReg().scratchReg();

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  SetRegExpStubInputRegisters(masm, &regexp, RegExpMatcherRegExpReg, &input,
                              RegExpMatcherStringReg, &lastIndex,
                              RegExpMatcherLastIndexReg);

  masm.reserveStack(RegExpReservedStack);

  Label done, vmCall, vmCallNoMatches;
  CallRegExpStub(masm, JitRealm::offsetOfRegExpMatcherStub(), scratch,
                 &vmCallNoMatches);
  masm.branchTestUndefined(Assembler::Equal, JSReturnOperand, &vmCall);

  masm.jump(&done);

  {
    Label pushedMatches;
    masm.bind(&vmCallNoMatches);
    masm.push(ImmWord(0));
    masm.jump(&pushedMatches);

    masm.bind(&vmCall);
    masm.computeEffectiveAddress(
        Address(masm.getStackPointer(), InputOutputDataSize), scratch);
    masm.Push(scratch);

    masm.bind(&pushedMatches);
    masm.Push(lastIndex);
    masm.Push(input);
    masm.Push(regexp);

    using Fn = bool (*)(JSContext*, HandleObject regexp, HandleString input,
                        int32_t lastIndex, MatchPairs* pairs,
                        MutableHandleValue output);
    callVM<Fn, RegExpMatcherRaw>(masm);
  }

  masm.bind(&done);

  static_assert(R0 == JSReturnOperand);

  stubFrame.leave(masm);
  return true;
}

bool BaselineCacheIRCompiler::emitCallRegExpSearcherResult(
    ObjOperandId regexpId, StringOperandId inputId, Int32OperandId lastIndexId,
    uint32_t stubOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register regexp = allocator.useRegister(masm, regexpId);
  Register input = allocator.useRegister(masm, inputId);
  Register lastIndex = allocator.useRegister(masm, lastIndexId);
  Register scratch = output.valueReg().scratchReg();

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  SetRegExpStubInputRegisters(masm, &regexp, RegExpSearcherRegExpReg, &input,
                              RegExpSearcherStringReg, &lastIndex,
                              RegExpSearcherLastIndexReg);
  // Ensure `scratch` doesn't conflict with the stub's input registers.
  scratch = ReturnReg;

  masm.reserveStack(RegExpReservedStack);

  Label done, vmCall, vmCallNoMatches;
  CallRegExpStub(masm, JitRealm::offsetOfRegExpSearcherStub(), scratch,
                 &vmCallNoMatches);
  masm.branch32(Assembler::Equal, scratch, Imm32(RegExpSearcherResultFailed),
                &vmCall);

  masm.jump(&done);

  {
    Label pushedMatches;
    masm.bind(&vmCallNoMatches);
    masm.push(ImmWord(0));
    masm.jump(&pushedMatches);

    masm.bind(&vmCall);
    masm.computeEffectiveAddress(
        Address(masm.getStackPointer(), InputOutputDataSize), scratch);
    masm.Push(scratch);

    masm.bind(&pushedMatches);
    masm.Push(lastIndex);
    masm.Push(input);
    masm.Push(regexp);

    using Fn = bool (*)(JSContext*, HandleObject regexp, HandleString input,
                        int32_t lastIndex, MatchPairs* pairs, int32_t* result);
    callVM<Fn, RegExpSearcherRaw>(masm);
  }

  masm.bind(&done);

  masm.tagValue(JSVAL_TYPE_INT32, ReturnReg, output.valueReg());

  stubFrame.leave(masm);
  return true;
}

bool BaselineCacheIRCompiler::emitRegExpBuiltinExecMatchResult(
    ObjOperandId regexpId, StringOperandId inputId, uint32_t stubOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register regexp = allocator.useRegister(masm, regexpId);
  Register input = allocator.useRegister(masm, inputId);
  Register scratch = output.valueReg().scratchReg();

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  SetRegExpStubInputRegisters(masm, &regexp, RegExpMatcherRegExpReg, &input,
                              RegExpMatcherStringReg, nullptr, InvalidReg);

  masm.reserveStack(RegExpReservedStack);

  Label done, vmCall, vmCallNoMatches;
  CallRegExpStub(masm, JitRealm::offsetOfRegExpExecMatchStub(), scratch,
                 &vmCallNoMatches);
  masm.branchTestUndefined(Assembler::Equal, JSReturnOperand, &vmCall);

  masm.jump(&done);

  {
    Label pushedMatches;
    masm.bind(&vmCallNoMatches);
    masm.push(ImmWord(0));
    masm.jump(&pushedMatches);

    masm.bind(&vmCall);
    masm.computeEffectiveAddress(
        Address(masm.getStackPointer(), InputOutputDataSize), scratch);
    masm.Push(scratch);

    masm.bind(&pushedMatches);
    masm.Push(input);
    masm.Push(regexp);

    using Fn =
        bool (*)(JSContext*, Handle<RegExpObject*> regexp, HandleString input,
                 MatchPairs* pairs, MutableHandleValue output);
    callVM<Fn, RegExpBuiltinExecMatchFromJit>(masm);
  }

  masm.bind(&done);

  static_assert(R0 == JSReturnOperand);

  stubFrame.leave(masm);
  return true;
}

bool BaselineCacheIRCompiler::emitRegExpBuiltinExecTestResult(
    ObjOperandId regexpId, StringOperandId inputId, uint32_t stubOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register regexp = allocator.useRegister(masm, regexpId);
  Register input = allocator.useRegister(masm, inputId);
  Register scratch = output.valueReg().scratchReg();

  allocator.discardStack(masm);

  AutoStubFrame stubFrame(*this);
  stubFrame.enter(masm, scratch);

  SetRegExpStubInputRegisters(masm, &regexp, RegExpExecTestRegExpReg, &input,
                              RegExpExecTestStringReg, nullptr, InvalidReg);
  // Ensure `scratch` doesn't conflict with the stub's input registers.
  scratch = ReturnReg;

  Label done, vmCall;
  CallRegExpStub(masm, JitRealm::offsetOfRegExpExecTestStub(), scratch,
                 &vmCall);
  masm.branch32(Assembler::Equal, scratch, Imm32(RegExpExecTestResultFailed),
                &vmCall);

  masm.jump(&done);

  {
    masm.bind(&vmCall);

    masm.Push(input);
    masm.Push(regexp);

    using Fn = bool (*)(JSContext*, Handle<RegExpObject*> regexp,
                        HandleString input, bool* result);
    callVM<Fn, RegExpBuiltinExecTestFromJit>(masm);
  }

  masm.bind(&done);

  masm.tagValue(JSVAL_TYPE_BOOLEAN, ReturnReg, output.valueReg());

  stubFrame.leave(masm);
  return true;
}
