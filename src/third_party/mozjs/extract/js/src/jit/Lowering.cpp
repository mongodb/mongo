/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Lowering.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/EndianUtils.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/MathAlgorithms.h"

#include <type_traits>

#include "jit/ABIArgGenerator.h"
#include "jit/IonGenericCallStub.h"
#include "jit/IonOptimizationLevels.h"
#include "jit/JitSpewer.h"
#include "jit/LIR.h"
#include "jit/MacroAssembler.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"
#include "jit/SharedICRegisters.h"
#include "js/experimental/JitInfo.h"  // JSJitInfo
#include "util/Memory.h"
#include "wasm/WasmCodegenTypes.h"
#include "wasm/WasmFeatures.h"  // for wasm::ReportSimdAnalysis
#include "wasm/WasmInstanceData.h"

#include "jit/shared/Lowering-shared-inl.h"
#include "vm/BytecodeUtil-inl.h"

using namespace js;
using namespace jit;

using JS::GenericNaN;
using mozilla::DebugOnly;

LBoxAllocation LIRGenerator::useBoxFixedAtStart(MDefinition* mir,
                                                ValueOperand op) {
#if defined(JS_NUNBOX32)
  return useBoxFixed(mir, op.typeReg(), op.payloadReg(), true);
#elif defined(JS_PUNBOX64)
  return useBoxFixed(mir, op.valueReg(), op.scratchReg(), true);
#endif
}

LBoxAllocation LIRGenerator::useBoxAtStart(MDefinition* mir,
                                           LUse::Policy policy) {
  return useBox(mir, policy, /* useAtStart = */ true);
}

void LIRGenerator::visitParameter(MParameter* param) {
  ptrdiff_t offset;
  if (param->index() == MParameter::THIS_SLOT) {
    offset = THIS_FRAME_ARGSLOT;
  } else {
    offset = 1 + param->index();
  }

  LParameter* ins = new (alloc()) LParameter;
  defineBox(ins, param, LDefinition::FIXED);

  offset *= sizeof(Value);
#if defined(JS_NUNBOX32)
#  if MOZ_BIG_ENDIAN()
  ins->getDef(0)->setOutput(LArgument(offset));
  ins->getDef(1)->setOutput(LArgument(offset + 4));
#  else
  ins->getDef(0)->setOutput(LArgument(offset + 4));
  ins->getDef(1)->setOutput(LArgument(offset));
#  endif
#elif defined(JS_PUNBOX64)
  ins->getDef(0)->setOutput(LArgument(offset));
#endif
}

void LIRGenerator::visitCallee(MCallee* ins) {
  define(new (alloc()) LCallee(), ins);
}

void LIRGenerator::visitIsConstructing(MIsConstructing* ins) {
  define(new (alloc()) LIsConstructing(), ins);
}

void LIRGenerator::visitGoto(MGoto* ins) {
  add(new (alloc()) LGoto(ins->target()));
}

void LIRGenerator::visitTableSwitch(MTableSwitch* tableswitch) {
  MDefinition* opd = tableswitch->getOperand(0);

  // There should be at least 1 successor. The default case!
  MOZ_ASSERT(tableswitch->numSuccessors() > 0);

  // If there are no cases, the default case is always taken.
  if (tableswitch->numSuccessors() == 1) {
    add(new (alloc()) LGoto(tableswitch->getDefault()));
    return;
  }

  // If we don't know the type.
  if (opd->type() == MIRType::Value) {
    LTableSwitchV* lir = newLTableSwitchV(tableswitch);
    add(lir);
    return;
  }

  // Case indices are numeric, so other types will always go to the default
  // case.
  if (opd->type() != MIRType::Int32 && opd->type() != MIRType::Double) {
    add(new (alloc()) LGoto(tableswitch->getDefault()));
    return;
  }

  // Return an LTableSwitch, capable of handling either an integer or
  // floating-point index.
  LAllocation index;
  LDefinition tempInt;
  if (opd->type() == MIRType::Int32) {
    index = useRegisterAtStart(opd);
    tempInt = tempCopy(opd, 0);
  } else {
    index = useRegister(opd);
    tempInt = temp(LDefinition::GENERAL);
  }
  add(newLTableSwitch(index, tempInt, tableswitch));
}

void LIRGenerator::visitCheckOverRecursed(MCheckOverRecursed* ins) {
  LCheckOverRecursed* lir = new (alloc()) LCheckOverRecursed();
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNewArray(MNewArray* ins) {
  LNewArray* lir = new (alloc()) LNewArray(temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNewArrayDynamicLength(MNewArrayDynamicLength* ins) {
  MDefinition* length = ins->length();
  MOZ_ASSERT(length->type() == MIRType::Int32);

  LNewArrayDynamicLength* lir =
      new (alloc()) LNewArrayDynamicLength(useRegister(length), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNewIterator(MNewIterator* ins) {
  LNewIterator* lir = new (alloc()) LNewIterator(temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNewTypedArray(MNewTypedArray* ins) {
  LNewTypedArray* lir = new (alloc()) LNewTypedArray(temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNewTypedArrayDynamicLength(
    MNewTypedArrayDynamicLength* ins) {
  MDefinition* length = ins->length();
  MOZ_ASSERT(length->type() == MIRType::Int32);

  LNewTypedArrayDynamicLength* lir =
      new (alloc()) LNewTypedArrayDynamicLength(useRegister(length), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNewTypedArrayFromArray(MNewTypedArrayFromArray* ins) {
  MDefinition* array = ins->array();
  MOZ_ASSERT(array->type() == MIRType::Object);

  auto* lir = new (alloc()) LNewTypedArrayFromArray(useRegisterAtStart(array));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNewTypedArrayFromArrayBuffer(
    MNewTypedArrayFromArrayBuffer* ins) {
  MDefinition* arrayBuffer = ins->arrayBuffer();
  MDefinition* byteOffset = ins->byteOffset();
  MDefinition* length = ins->length();
  MOZ_ASSERT(arrayBuffer->type() == MIRType::Object);
  MOZ_ASSERT(byteOffset->type() == MIRType::Value);
  MOZ_ASSERT(length->type() == MIRType::Value);

  auto* lir = new (alloc()) LNewTypedArrayFromArrayBuffer(
      useRegisterAtStart(arrayBuffer), useBoxAtStart(byteOffset),
      useBoxAtStart(length));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNewObject(MNewObject* ins) {
  LNewObject* lir = new (alloc()) LNewObject(temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitBindFunction(MBindFunction* ins) {
  MDefinition* target = ins->target();
  MOZ_ASSERT(target->type() == MIRType::Object);

  if (!lowerCallArguments(ins)) {
    abort(AbortReason::Alloc, "OOM: LIRGenerator::visitBindFunction");
    return;
  }

  auto* lir = new (alloc())
      LBindFunction(useFixedAtStart(target, CallTempReg0),
                    tempFixed(CallTempReg1), tempFixed(CallTempReg2));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNewBoundFunction(MNewBoundFunction* ins) {
  auto* lir = new (alloc()) LNewBoundFunction(temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNewPlainObject(MNewPlainObject* ins) {
  LNewPlainObject* lir = new (alloc()) LNewPlainObject(temp(), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNewArrayObject(MNewArrayObject* ins) {
  LNewArrayObject* lir = new (alloc()) LNewArrayObject(temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNewNamedLambdaObject(MNewNamedLambdaObject* ins) {
  LNewNamedLambdaObject* lir = new (alloc()) LNewNamedLambdaObject(temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNewCallObject(MNewCallObject* ins) {
  LNewCallObject* lir = new (alloc()) LNewCallObject(temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNewStringObject(MNewStringObject* ins) {
  MOZ_ASSERT(ins->input()->type() == MIRType::String);

  LNewStringObject* lir =
      new (alloc()) LNewStringObject(useRegister(ins->input()), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitInitElemGetterSetter(MInitElemGetterSetter* ins) {
  LInitElemGetterSetter* lir = new (alloc()) LInitElemGetterSetter(
      useRegisterAtStart(ins->object()), useBoxAtStart(ins->id()),
      useRegisterAtStart(ins->value()));
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitMutateProto(MMutateProto* ins) {
  LMutateProto* lir = new (alloc()) LMutateProto(
      useRegisterAtStart(ins->object()), useBoxAtStart(ins->value()));
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitInitPropGetterSetter(MInitPropGetterSetter* ins) {
  LInitPropGetterSetter* lir = new (alloc()) LInitPropGetterSetter(
      useRegisterAtStart(ins->object()), useRegisterAtStart(ins->value()));
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCreateThis(MCreateThis* ins) {
  LCreateThis* lir =
      new (alloc()) LCreateThis(useRegisterOrConstantAtStart(ins->callee()),
                                useRegisterOrConstantAtStart(ins->newTarget()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCreateArgumentsObject(MCreateArgumentsObject* ins) {
  LAllocation callObj = useRegisterAtStart(ins->getCallObject());
  LCreateArgumentsObject* lir = new (alloc())
      LCreateArgumentsObject(callObj, tempFixed(CallTempReg0),
                             tempFixed(CallTempReg1), tempFixed(CallTempReg2));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCreateInlinedArgumentsObject(
    MCreateInlinedArgumentsObject* ins) {
  LAllocation callObj = useRegisterAtStart(ins->getCallObject());
  LAllocation callee = useRegisterAtStart(ins->getCallee());
  uint32_t numActuals = ins->numActuals();
  uint32_t numOperands = numActuals * BOX_PIECES +
                         LCreateInlinedArgumentsObject::NumNonArgumentOperands;

  auto* lir = allocateVariadic<LCreateInlinedArgumentsObject>(
      numOperands, tempFixed(CallTempReg0), tempFixed(CallTempReg1));
  if (!lir) {
    abort(AbortReason::Alloc,
          "OOM: LIRGenerator::visitCreateInlinedArgumentsObject");
    return;
  }

  lir->setOperand(LCreateInlinedArgumentsObject::CallObj, callObj);
  lir->setOperand(LCreateInlinedArgumentsObject::Callee, callee);
  for (uint32_t i = 0; i < numActuals; i++) {
    MDefinition* arg = ins->getArg(i);
    uint32_t index = LCreateInlinedArgumentsObject::ArgIndex(i);
    lir->setBoxOperand(index, useBoxOrTypedOrConstant(arg,
                                                      /*useConstant = */ true,
                                                      /*useAtStart = */ true));
  }

  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitGetInlinedArgument(MGetInlinedArgument* ins) {
#if defined(JS_PUNBOX64)
  // On 64-bit architectures, we don't support boxing a typed register
  // in-place without using a scratch register, so the result register
  // can't be the same as any of the inputs. Fortunately, those
  // architectures have registers to spare.
  const bool useAtStart = false;
#else
  const bool useAtStart = true;
#endif

  LAllocation index =
      useAtStart ? useRegisterAtStart(ins->index()) : useRegister(ins->index());
  uint32_t numActuals = ins->numActuals();
  uint32_t numOperands =
      numActuals * BOX_PIECES + LGetInlinedArgument::NumNonArgumentOperands;

  auto* lir = allocateVariadic<LGetInlinedArgument>(numOperands);
  if (!lir) {
    abort(AbortReason::Alloc, "OOM: LIRGenerator::visitGetInlinedArgument");
    return;
  }

  lir->setOperand(LGetInlinedArgument::Index, index);
  for (uint32_t i = 0; i < numActuals; i++) {
    MDefinition* arg = ins->getArg(i);
    uint32_t index = LGetInlinedArgument::ArgIndex(i);
    lir->setBoxOperand(
        index, useBoxOrTypedOrConstant(arg,
                                       /*useConstant = */ true, useAtStart));
  }
  defineBox(lir, ins);
}

void LIRGenerator::visitGetInlinedArgumentHole(MGetInlinedArgumentHole* ins) {
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_MIPS64)
  // On some 64-bit architectures, we don't support boxing a typed
  // register in-place without using a scratch register, so the result
  // register can't be the same as any of the inputs. Fortunately,
  // those architectures have registers to spare.
  const bool useAtStart = false;
#else
  const bool useAtStart = true;
#endif

  LAllocation index =
      useAtStart ? useRegisterAtStart(ins->index()) : useRegister(ins->index());
  uint32_t numActuals = ins->numActuals();
  uint32_t numOperands =
      numActuals * BOX_PIECES + LGetInlinedArgumentHole::NumNonArgumentOperands;

  auto* lir = allocateVariadic<LGetInlinedArgumentHole>(numOperands);
  if (!lir) {
    abort(AbortReason::Alloc, "OOM: LIRGenerator::visitGetInlinedArgumentHole");
    return;
  }

  lir->setOperand(LGetInlinedArgumentHole::Index, index);
  for (uint32_t i = 0; i < numActuals; i++) {
    MDefinition* arg = ins->getArg(i);
    uint32_t index = LGetInlinedArgumentHole::ArgIndex(i);
    lir->setBoxOperand(
        index, useBoxOrTypedOrConstant(arg,
                                       /*useConstant = */ true, useAtStart));
  }
  assignSnapshot(lir, ins->bailoutKind());
  defineBox(lir, ins);
}

void LIRGenerator::visitGetArgumentsObjectArg(MGetArgumentsObjectArg* ins) {
  LAllocation argsObj = useRegister(ins->argsObject());
  LGetArgumentsObjectArg* lir =
      new (alloc()) LGetArgumentsObjectArg(argsObj, temp());
  defineBox(lir, ins);
}

void LIRGenerator::visitSetArgumentsObjectArg(MSetArgumentsObjectArg* ins) {
  LAllocation argsObj = useRegister(ins->argsObject());
  LSetArgumentsObjectArg* lir = new (alloc())
      LSetArgumentsObjectArg(argsObj, useBox(ins->value()), temp());
  add(lir, ins);
}

void LIRGenerator::visitLoadArgumentsObjectArg(MLoadArgumentsObjectArg* ins) {
  MDefinition* argsObj = ins->argsObject();
  MOZ_ASSERT(argsObj->type() == MIRType::Object);

  MDefinition* index = ins->index();
  MOZ_ASSERT(index->type() == MIRType::Int32);

  auto* lir = new (alloc())
      LLoadArgumentsObjectArg(useRegister(argsObj), useRegister(index), temp());
  assignSnapshot(lir, ins->bailoutKind());
  defineBox(lir, ins);
}

void LIRGenerator::visitLoadArgumentsObjectArgHole(
    MLoadArgumentsObjectArgHole* ins) {
  MDefinition* argsObj = ins->argsObject();
  MOZ_ASSERT(argsObj->type() == MIRType::Object);

  MDefinition* index = ins->index();
  MOZ_ASSERT(index->type() == MIRType::Int32);

  auto* lir = new (alloc()) LLoadArgumentsObjectArgHole(
      useRegister(argsObj), useRegister(index), temp());
  assignSnapshot(lir, ins->bailoutKind());
  defineBox(lir, ins);
}

void LIRGenerator::visitInArgumentsObjectArg(MInArgumentsObjectArg* ins) {
  MDefinition* argsObj = ins->argsObject();
  MOZ_ASSERT(argsObj->type() == MIRType::Object);

  MDefinition* index = ins->index();
  MOZ_ASSERT(index->type() == MIRType::Int32);

  auto* lir = new (alloc())
      LInArgumentsObjectArg(useRegister(argsObj), useRegister(index), temp());
  assignSnapshot(lir, ins->bailoutKind());
  define(lir, ins);
}

void LIRGenerator::visitArgumentsObjectLength(MArgumentsObjectLength* ins) {
  MDefinition* argsObj = ins->argsObject();
  MOZ_ASSERT(argsObj->type() == MIRType::Object);

  auto* lir = new (alloc()) LArgumentsObjectLength(useRegister(argsObj));
  assignSnapshot(lir, ins->bailoutKind());
  define(lir, ins);
}

void LIRGenerator::visitArrayFromArgumentsObject(
    MArrayFromArgumentsObject* ins) {
  MDefinition* argsObj = ins->argsObject();
  MOZ_ASSERT(argsObj->type() == MIRType::Object);

  auto* lir =
      new (alloc()) LArrayFromArgumentsObject(useRegisterAtStart(argsObj));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitGuardArgumentsObjectFlags(
    MGuardArgumentsObjectFlags* ins) {
  MDefinition* argsObj = ins->argsObject();
  MOZ_ASSERT(argsObj->type() == MIRType::Object);

  auto* lir =
      new (alloc()) LGuardArgumentsObjectFlags(useRegister(argsObj), temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, argsObj);
}

void LIRGenerator::visitBoundFunctionNumArgs(MBoundFunctionNumArgs* ins) {
  MDefinition* obj = ins->object();
  MOZ_ASSERT(obj->type() == MIRType::Object);

  auto* lir = new (alloc()) LBoundFunctionNumArgs(useRegisterAtStart(obj));
  define(lir, ins);
}

void LIRGenerator::visitGuardBoundFunctionIsConstructor(
    MGuardBoundFunctionIsConstructor* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  auto* lir = new (alloc())
      LGuardBoundFunctionIsConstructor(useRegister(ins->object()));
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->object());
}

void LIRGenerator::visitReturnFromCtor(MReturnFromCtor* ins) {
  LReturnFromCtor* lir = new (alloc())
      LReturnFromCtor(useBox(ins->value()), useRegister(ins->object()));
  define(lir, ins);
}

void LIRGenerator::visitBoxNonStrictThis(MBoxNonStrictThis* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Object);
  MOZ_ASSERT(ins->input()->type() == MIRType::Value);

  auto* lir = new (alloc()) LBoxNonStrictThis(useBox(ins->input()));
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitImplicitThis(MImplicitThis* ins) {
  MDefinition* env = ins->envChain();
  MOZ_ASSERT(env->type() == MIRType::Object);

  LImplicitThis* lir = new (alloc()) LImplicitThis(useRegisterAtStart(env));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

template <typename T>
bool LIRGenerator::lowerCallArguments(T* call) {
  uint32_t argc = call->numStackArgs();

  // Align the arguments of a call such that the callee would keep the same
  // alignment as the caller.
  uint32_t baseSlot = 0;
  if (JitStackValueAlignment > 1) {
    baseSlot = AlignBytes(argc, JitStackValueAlignment);
  } else {
    baseSlot = argc;
  }

  // Save the maximum number of argument, such that we can have one unique
  // frame size.
  if (baseSlot > maxargslots_) {
    maxargslots_ = baseSlot;
  }

  for (size_t i = 0; i < argc; i++) {
    MDefinition* arg = call->getArg(i);
    uint32_t argslot = baseSlot - i;

    // Values take a slow path.
    if (arg->type() == MIRType::Value) {
      LStackArgV* stack = new (alloc()) LStackArgV(useBox(arg), argslot);
      add(stack);
    } else {
      // Known types can move constant types and/or payloads.
      LStackArgT* stack = new (alloc())
          LStackArgT(useRegisterOrConstant(arg), argslot, arg->type());
      add(stack);
    }

    if (!alloc().ensureBallast()) {
      return false;
    }
  }
  return true;
}

void LIRGenerator::visitCall(MCall* call) {
  MOZ_ASSERT(call->getCallee()->type() == MIRType::Object);

  // In case of oom, skip the rest of the allocations.
  if (!lowerCallArguments(call)) {
    abort(AbortReason::Alloc, "OOM: LIRGenerator::visitCall");
    return;
  }

  WrappedFunction* target = call->getSingleTarget();

  LInstruction* lir;

  if (call->isCallDOMNative()) {
    // Call DOM functions.
    MOZ_ASSERT(target && target->isNativeWithoutJitEntry());
    Register cxReg, objReg, privReg, argsReg;
    GetTempRegForIntArg(0, 0, &cxReg);
    GetTempRegForIntArg(1, 0, &objReg);
    GetTempRegForIntArg(2, 0, &privReg);
    mozilla::DebugOnly<bool> ok = GetTempRegForIntArg(3, 0, &argsReg);
    MOZ_ASSERT(ok, "How can we not have four temp registers?");
    lir = new (alloc()) LCallDOMNative(tempFixed(cxReg), tempFixed(objReg),
                                       tempFixed(privReg), tempFixed(argsReg));
  } else if (target) {
    // Call known functions.
    if (target->isNativeWithoutJitEntry()) {
      Register cxReg, numReg, vpReg, tmpReg;
      GetTempRegForIntArg(0, 0, &cxReg);
      GetTempRegForIntArg(1, 0, &numReg);
      GetTempRegForIntArg(2, 0, &vpReg);

      // Even though this is just a temp reg, use the same API to avoid
      // register collisions.
      mozilla::DebugOnly<bool> ok = GetTempRegForIntArg(3, 0, &tmpReg);
      MOZ_ASSERT(ok, "How can we not have four temp registers?");

      lir = new (alloc()) LCallNative(tempFixed(cxReg), tempFixed(numReg),
                                      tempFixed(vpReg), tempFixed(tmpReg));
    } else {
      lir = new (alloc()) LCallKnown(useRegisterAtStart(call->getCallee()),
                                     tempFixed(CallTempReg0));
    }
  } else {
    // Call anything, using the most generic code.
    lir = new (alloc()) LCallGeneric(
        useFixedAtStart(call->getCallee(), IonGenericCallCalleeReg),
        tempFixed(IonGenericCallArgcReg));
  }
  defineReturn(lir, call);
  assignSafepoint(lir, call);
}

void LIRGenerator::visitCallClassHook(MCallClassHook* call) {
  MDefinition* callee = call->getCallee();
  MOZ_ASSERT(callee->type() == MIRType::Object);

  // In case of oom, skip the rest of the allocations.
  if (!lowerCallArguments(call)) {
    abort(AbortReason::Alloc, "OOM: LIRGenerator::visitCallClassHook");
    return;
  }

  Register cxReg, numReg, vpReg, tmpReg;
  GetTempRegForIntArg(0, 0, &cxReg);
  GetTempRegForIntArg(1, 0, &numReg);
  GetTempRegForIntArg(2, 0, &vpReg);

  // Even though this is just a temp reg, use the same API to avoid
  // register collisions.
  mozilla::DebugOnly<bool> ok = GetTempRegForIntArg(3, 0, &tmpReg);
  MOZ_ASSERT(ok, "How can we not have four temp registers?");

  auto* lir = new (alloc())
      LCallClassHook(useRegisterAtStart(callee), tempFixed(cxReg),
                     tempFixed(numReg), tempFixed(vpReg), tempFixed(tmpReg));
  defineReturn(lir, call);
  assignSafepoint(lir, call);
}

void LIRGenerator::visitApplyArgs(MApplyArgs* apply) {
  MOZ_ASSERT(apply->getFunction()->type() == MIRType::Object);

  // Assert if the return value is already erased.
  static_assert(CallTempReg2 != JSReturnReg_Type);
  static_assert(CallTempReg2 != JSReturnReg_Data);

  auto argc = useFixedAtStart(apply->getArgc(), CallTempReg0);
  auto thisValue =
      useBoxFixedAtStart(apply->getThis(), CallTempReg4, CallTempReg5);
  auto tempObj = tempFixed(CallTempReg1);   // object register
  auto tempCopy = tempFixed(CallTempReg2);  // copy register

  auto* target = apply->getSingleTarget();

  LInstruction* lir;
  if (target && target->isNativeWithoutJitEntry()) {
    auto temp = tempFixed(CallTempReg3);

    lir = new (alloc())
        LApplyArgsNative(argc, thisValue, tempObj, tempCopy, temp);
  } else {
    auto function = useFixedAtStart(apply->getFunction(), CallTempReg3);

    lir = new (alloc())
        LApplyArgsGeneric(function, argc, thisValue, tempObj, tempCopy);
  }

  // Bailout is needed in the case of too many values in the arguments array.
  assignSnapshot(lir, apply->bailoutKind());

  defineReturn(lir, apply);
  assignSafepoint(lir, apply);
}

void LIRGenerator::visitApplyArgsObj(MApplyArgsObj* apply) {
  MOZ_ASSERT(apply->getFunction()->type() == MIRType::Object);

  // Assert if the return value is already erased.
  static_assert(CallTempReg2 != JSReturnReg_Type);
  static_assert(CallTempReg2 != JSReturnReg_Data);

  auto argsObj = useFixedAtStart(apply->getArgsObj(), CallTempReg0);
  auto thisValue =
      useBoxFixedAtStart(apply->getThis(), CallTempReg4, CallTempReg5);
  auto tempObj = tempFixed(CallTempReg1);   // object register
  auto tempCopy = tempFixed(CallTempReg2);  // copy register

  auto* target = apply->getSingleTarget();

  LInstruction* lir;
  if (target && target->isNativeWithoutJitEntry()) {
    auto temp = tempFixed(CallTempReg3);

    lir = new (alloc())
        LApplyArgsObjNative(argsObj, thisValue, tempObj, tempCopy, temp);
  } else {
    auto function = useFixedAtStart(apply->getFunction(), CallTempReg3);

    lir = new (alloc())
        LApplyArgsObj(function, argsObj, thisValue, tempObj, tempCopy);
  }

  // Bailout is needed in the case of too many values in the arguments array.
  assignSnapshot(lir, apply->bailoutKind());

  defineReturn(lir, apply);
  assignSafepoint(lir, apply);
}

void LIRGenerator::visitApplyArray(MApplyArray* apply) {
  MOZ_ASSERT(apply->getFunction()->type() == MIRType::Object);

  // Assert if the return value is already erased.
  static_assert(CallTempReg2 != JSReturnReg_Type);
  static_assert(CallTempReg2 != JSReturnReg_Data);

  auto elements = useFixedAtStart(apply->getElements(), CallTempReg0);
  auto thisValue =
      useBoxFixedAtStart(apply->getThis(), CallTempReg4, CallTempReg5);
  auto tempObj = tempFixed(CallTempReg1);   // object register
  auto tempCopy = tempFixed(CallTempReg2);  // copy register

  auto* target = apply->getSingleTarget();

  LInstruction* lir;
  if (target && target->isNativeWithoutJitEntry()) {
    auto temp = tempFixed(CallTempReg3);

    lir = new (alloc())
        LApplyArrayNative(elements, thisValue, tempObj, tempCopy, temp);
  } else {
    auto function = useFixedAtStart(apply->getFunction(), CallTempReg3);

    lir = new (alloc())
        LApplyArrayGeneric(function, elements, thisValue, tempObj, tempCopy);
  }

  // Bailout is needed in the case of too many values in the array, or empty
  // space at the end of the array.
  assignSnapshot(lir, apply->bailoutKind());

  defineReturn(lir, apply);
  assignSafepoint(lir, apply);
}

void LIRGenerator::visitConstructArgs(MConstructArgs* mir) {
  MOZ_ASSERT(mir->getFunction()->type() == MIRType::Object);
  MOZ_ASSERT(mir->getArgc()->type() == MIRType::Int32);
  MOZ_ASSERT(mir->getNewTarget()->type() == MIRType::Object);
  MOZ_ASSERT(mir->getThis()->type() == MIRType::Value);

  // Assert if the return value is already erased.
  static_assert(CallTempReg2 != JSReturnReg_Type);
  static_assert(CallTempReg2 != JSReturnReg_Data);

  auto argc = useFixedAtStart(mir->getArgc(), CallTempReg0);
  auto newTarget = useFixedAtStart(mir->getNewTarget(), CallTempReg1);
  auto temp = tempFixed(CallTempReg2);

  auto* target = mir->getSingleTarget();

  LInstruction* lir;
  if (target && target->isNativeWithoutJitEntry()) {
    auto temp2 = tempFixed(CallTempReg3);
    auto temp3 = tempFixed(CallTempReg4);

    lir =
        new (alloc()) LConstructArgsNative(argc, newTarget, temp, temp2, temp3);
  } else {
    auto function = useFixedAtStart(mir->getFunction(), CallTempReg3);
    auto thisValue =
        useBoxFixedAtStart(mir->getThis(), CallTempReg4, CallTempReg5);

    lir = new (alloc())
        LConstructArgsGeneric(function, argc, newTarget, thisValue, temp);
  }

  // Bailout is needed in the case of too many values in the arguments array.
  assignSnapshot(lir, mir->bailoutKind());

  defineReturn(lir, mir);
  assignSafepoint(lir, mir);
}

void LIRGenerator::visitConstructArray(MConstructArray* mir) {
  MOZ_ASSERT(mir->getFunction()->type() == MIRType::Object);
  MOZ_ASSERT(mir->getElements()->type() == MIRType::Elements);
  MOZ_ASSERT(mir->getNewTarget()->type() == MIRType::Object);
  MOZ_ASSERT(mir->getThis()->type() == MIRType::Value);

  // Assert if the return value is already erased.
  static_assert(CallTempReg2 != JSReturnReg_Type);
  static_assert(CallTempReg2 != JSReturnReg_Data);

  auto elements = useFixedAtStart(mir->getElements(), CallTempReg0);
  auto newTarget = useFixedAtStart(mir->getNewTarget(), CallTempReg1);
  auto temp = tempFixed(CallTempReg2);

  auto* target = mir->getSingleTarget();

  LInstruction* lir;
  if (target && target->isNativeWithoutJitEntry()) {
    auto temp2 = tempFixed(CallTempReg3);
    auto temp3 = tempFixed(CallTempReg4);

    lir = new (alloc())
        LConstructArrayNative(elements, newTarget, temp, temp2, temp3);
  } else {
    auto function = useFixedAtStart(mir->getFunction(), CallTempReg3);
    auto thisValue =
        useBoxFixedAtStart(mir->getThis(), CallTempReg4, CallTempReg5);

    lir = new (alloc())
        LConstructArrayGeneric(function, elements, newTarget, thisValue, temp);
  }

  // Bailout is needed in the case of too many values in the array, or empty
  // space at the end of the array.
  assignSnapshot(lir, mir->bailoutKind());

  defineReturn(lir, mir);
  assignSafepoint(lir, mir);
}

void LIRGenerator::visitBail(MBail* bail) {
  LBail* lir = new (alloc()) LBail();
  assignSnapshot(lir, bail->bailoutKind());
  add(lir, bail);
}

void LIRGenerator::visitUnreachable(MUnreachable* unreachable) {
  LUnreachable* lir = new (alloc()) LUnreachable();
  add(lir, unreachable);
}

void LIRGenerator::visitEncodeSnapshot(MEncodeSnapshot* mir) {
  LEncodeSnapshot* lir = new (alloc()) LEncodeSnapshot();
  assignSnapshot(lir, mir->bailoutKind());
  add(lir, mir);
}

void LIRGenerator::visitUnreachableResult(MUnreachableResult* mir) {
  if (mir->type() == MIRType::Value) {
    auto* lir = new (alloc()) LUnreachableResultV();
    defineBox(lir, mir);
  } else {
    auto* lir = new (alloc()) LUnreachableResultT();
    define(lir, mir);
  }
}

void LIRGenerator::visitAssertFloat32(MAssertFloat32* assertion) {
  MIRType type = assertion->input()->type();
  DebugOnly<bool> checkIsFloat32 = assertion->mustBeFloat32();

  if (type != MIRType::Value && !JitOptions.eagerIonCompilation()) {
    MOZ_ASSERT_IF(checkIsFloat32, type == MIRType::Float32);
    MOZ_ASSERT_IF(!checkIsFloat32, type != MIRType::Float32);
  }
}

void LIRGenerator::visitAssertRecoveredOnBailout(
    MAssertRecoveredOnBailout* assertion) {
  MOZ_CRASH("AssertRecoveredOnBailout nodes are always recovered on bailouts.");
}

[[nodiscard]] static JSOp ReorderComparison(JSOp op, MDefinition** lhsp,
                                            MDefinition** rhsp) {
  MDefinition* lhs = *lhsp;
  MDefinition* rhs = *rhsp;

  if (lhs->maybeConstantValue()) {
    *rhsp = lhs;
    *lhsp = rhs;
    return ReverseCompareOp(op);
  }
  return op;
}

void LIRGenerator::visitTest(MTest* test) {
  MDefinition* opd = test->getOperand(0);
  MBasicBlock* ifTrue = test->ifTrue();
  MBasicBlock* ifFalse = test->ifFalse();

  // String is converted to length of string in the type analysis phase (see
  // TestPolicy).
  MOZ_ASSERT(opd->type() != MIRType::String);

  // Testing a constant.
  if (MConstant* constant = opd->maybeConstantValue()) {
    bool b;
    if (constant->valueToBoolean(&b)) {
      add(new (alloc()) LGoto(b ? ifTrue : ifFalse));
      return;
    }
  }

  if (opd->type() == MIRType::Value) {
    auto* lir = new (alloc()) LTestVAndBranch(
        ifTrue, ifFalse, useBox(opd), tempDouble(), tempToUnbox(), temp());
    add(lir, test);
    return;
  }

  // Objects are truthy, except if it might emulate undefined.
  if (opd->type() == MIRType::Object) {
    add(new (alloc())
            LTestOAndBranch(useRegister(opd), ifTrue, ifFalse, temp()),
        test);
    return;
  }

  // These must be explicitly sniffed out since they are constants and have
  // no payload.
  if (opd->type() == MIRType::Undefined || opd->type() == MIRType::Null) {
    add(new (alloc()) LGoto(ifFalse));
    return;
  }

  // All symbols are truthy.
  if (opd->type() == MIRType::Symbol) {
    add(new (alloc()) LGoto(ifTrue));
    return;
  }

  // Try to match the pattern
  //   test=MTest(
  //          comp=MCompare(
  //                 {EQ,NE} for {Int,UInt}{32,64},
  //                 bitAnd={MBitAnd,MWasmBinaryBitwise(And{32,64})}(x, y),
  //                 MConstant(0)
  //               )
  //        )
  // and produce a single LBitAndAndBranch node.  This requires both `comp`
  // and `bitAnd` to be marked emit-at-uses.  Since we can't use
  // LBitAndAndBranch to represent a 64-bit AND on a 32-bit target, the 64-bit
  // case is restricted to 64-bit targets.
  if (opd->isCompare() && opd->isEmittedAtUses()) {
#ifdef JS_64BIT
    constexpr bool targetIs64 = true;
#else
    constexpr bool targetIs64 = false;
#endif
    MCompare* comp = opd->toCompare();
    Assembler::Condition compCond =
        JSOpToCondition(comp->compareType(), comp->jsop());
    MDefinition* compL = comp->getOperand(0);
    MDefinition* compR = comp->getOperand(1);
    if ((comp->compareType() == MCompare::Compare_Int32 ||
         comp->compareType() == MCompare::Compare_UInt32 ||
         (targetIs64 && comp->compareType() == MCompare::Compare_Int64) ||
         (targetIs64 && comp->compareType() == MCompare::Compare_UInt64)) &&
        (compCond == Assembler::Equal || compCond == Assembler::NotEqual) &&
        compR->isConstant() &&
        (compR->toConstant()->isInt32(0) ||
         (targetIs64 && compR->toConstant()->isInt64(0))) &&
        (compL->isBitAnd() || (compL->isWasmBinaryBitwise() &&
                               compL->toWasmBinaryBitwise()->subOpcode() ==
                                   MWasmBinaryBitwise::SubOpcode::And))) {
      // The MCompare is OK; now check its first operand (the and-ish node).
      MDefinition* bitAnd = compL;
      MDefinition* bitAndL = bitAnd->getOperand(0);
      MDefinition* bitAndR = bitAnd->getOperand(1);
      MIRType bitAndLTy = bitAndL->type();
      MIRType bitAndRTy = bitAndR->type();
      if (bitAnd->isEmittedAtUses() && bitAndLTy == bitAndRTy &&
          (bitAndLTy == MIRType::Int32 ||
           (targetIs64 && bitAndLTy == MIRType::Int64))) {
        // Pattern match succeeded.
        ReorderCommutative(&bitAndL, &bitAndR, test);
        if (compCond == Assembler::Equal) {
          compCond = Assembler::Zero;
        } else if (compCond == Assembler::NotEqual) {
          compCond = Assembler::NonZero;
        } else {
          MOZ_ASSERT_UNREACHABLE("inequality operators cannot be folded");
        }
        MOZ_ASSERT_IF(!targetIs64, bitAndLTy == MIRType::Int32);
        lowerForBitAndAndBranch(
            new (alloc()) LBitAndAndBranch(
                ifTrue, ifFalse, bitAndLTy == MIRType::Int64, compCond),
            test, bitAndL, bitAndR);
        return;
      }
    }
  }

  // Check if the operand for this test is a compare operation. If it is, we
  // want to emit an LCompare*AndBranch rather than an LTest*AndBranch, to fuse
  // the compare and jump instructions.
  if (opd->isCompare() && opd->isEmittedAtUses()) {
    MCompare* comp = opd->toCompare();
    MDefinition* left = comp->lhs();
    MDefinition* right = comp->rhs();

    // Try to fold the comparison so that we don't have to handle all cases.
    bool result;
    if (comp->tryFold(&result)) {
      add(new (alloc()) LGoto(result ? ifTrue : ifFalse));
      return;
    }

    // Emit LCompare*AndBranch.

    // Compare and branch null/undefined.
    // The second operand has known null/undefined type,
    // so just test the first operand.
    if (comp->compareType() == MCompare::Compare_Null ||
        comp->compareType() == MCompare::Compare_Undefined) {
      if (left->type() == MIRType::Object) {
        auto* lir = new (alloc()) LIsNullOrLikeUndefinedAndBranchT(
            comp, useRegister(left), ifTrue, ifFalse, temp());
        add(lir, test);
        return;
      }

      if (IsLooseEqualityOp(comp->jsop())) {
        auto* lir = new (alloc()) LIsNullOrLikeUndefinedAndBranchV(
            comp, ifTrue, ifFalse, useBox(left), temp(), tempToUnbox());
        add(lir, test);
        return;
      }

      if (comp->compareType() == MCompare::Compare_Null) {
        auto* lir =
            new (alloc()) LIsNullAndBranch(comp, ifTrue, ifFalse, useBox(left));
        add(lir, test);
        return;
      }

      auto* lir = new (alloc())
          LIsUndefinedAndBranch(comp, ifTrue, ifFalse, useBox(left));
      add(lir, test);
      return;
    }

    // Compare and branch Int32, Symbol, Object, or WasmAnyRef pointers.
    if (comp->isInt32Comparison() ||
        comp->compareType() == MCompare::Compare_UInt32 ||
        comp->compareType() == MCompare::Compare_UIntPtr ||
        comp->compareType() == MCompare::Compare_Object ||
        comp->compareType() == MCompare::Compare_Symbol ||
        comp->compareType() == MCompare::Compare_WasmAnyRef) {
      JSOp op = ReorderComparison(comp->jsop(), &left, &right);
      LAllocation lhs = useRegister(left);
      LAllocation rhs;
      if (comp->isInt32Comparison() ||
          comp->compareType() == MCompare::Compare_UInt32 ||
          comp->compareType() == MCompare::Compare_UIntPtr) {
        rhs = useAnyOrInt32Constant(right);
      } else {
        rhs = useAny(right);
      }
      LCompareAndBranch* lir =
          new (alloc()) LCompareAndBranch(comp, op, lhs, rhs, ifTrue, ifFalse);
      add(lir, test);
      return;
    }

    // Compare and branch Int64.
    if (comp->compareType() == MCompare::Compare_Int64 ||
        comp->compareType() == MCompare::Compare_UInt64) {
      JSOp op = ReorderComparison(comp->jsop(), &left, &right);
      lowerForCompareI64AndBranch(test, comp, op, left, right, ifTrue, ifFalse);
      return;
    }

    // Compare and branch doubles.
    if (comp->isDoubleComparison()) {
      LAllocation lhs = useRegister(left);
      LAllocation rhs = useRegister(right);
      LCompareDAndBranch* lir =
          new (alloc()) LCompareDAndBranch(comp, lhs, rhs, ifTrue, ifFalse);
      add(lir, test);
      return;
    }

    // Compare and branch floats.
    if (comp->isFloat32Comparison()) {
      LAllocation lhs = useRegister(left);
      LAllocation rhs = useRegister(right);
      LCompareFAndBranch* lir =
          new (alloc()) LCompareFAndBranch(comp, lhs, rhs, ifTrue, ifFalse);
      add(lir, test);
      return;
    }
  }

  // Check if the operand for this test is a bitand operation. If it is, we want
  // to emit an LBitAndAndBranch rather than an LTest*AndBranch.
  if (opd->isBitAnd() && opd->isEmittedAtUses()) {
    MDefinition* lhs = opd->getOperand(0);
    MDefinition* rhs = opd->getOperand(1);
    if (lhs->type() == MIRType::Int32 && rhs->type() == MIRType::Int32) {
      ReorderCommutative(&lhs, &rhs, test);
      lowerForBitAndAndBranch(new (alloc()) LBitAndAndBranch(ifTrue, ifFalse,
                                                             /*is64=*/false),
                              test, lhs, rhs);
      return;
    }
  }

#if defined(ENABLE_WASM_SIMD) &&                           \
    (defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64) || \
     defined(JS_CODEGEN_ARM64))
  // Check if the operand for this test is an any_true/all_true SIMD operation.
  // If it is, we want to emit an LWasmReduceAndBranchSimd128 node to avoid
  // generating an intermediate boolean result.
  if (opd->isWasmReduceSimd128() && opd->isEmittedAtUses()) {
    MWasmReduceSimd128* node = opd->toWasmReduceSimd128();
    if (canFoldReduceSimd128AndBranch(node->simdOp())) {
#  ifdef DEBUG
      js::wasm::ReportSimdAnalysis("simd128-to-scalar-and-branch -> folded");
#  endif
      auto* lir = new (alloc()) LWasmReduceAndBranchSimd128(
          useRegister(node->input()), node->simdOp(), ifTrue, ifFalse);
      add(lir, test);
      return;
    }
  }
#endif

  if (opd->isIsObject() && opd->isEmittedAtUses()) {
    MDefinition* input = opd->toIsObject()->input();
    MOZ_ASSERT(input->type() == MIRType::Value);

    LIsObjectAndBranch* lir =
        new (alloc()) LIsObjectAndBranch(ifTrue, ifFalse, useBoxAtStart(input));
    add(lir, test);
    return;
  }

  if (opd->isWasmRefIsSubtypeOfAbstract() && opd->isEmittedAtUses()) {
    MWasmRefIsSubtypeOfAbstract* isSubTypeOf =
        opd->toWasmRefIsSubtypeOfAbstract();

    LAllocation ref = useRegister(isSubTypeOf->ref());
    WasmRefIsSubtypeDefs regs =
        useWasmRefIsSubtype(isSubTypeOf->destType(), /*superSTV=*/nullptr);
    add(new (alloc()) LWasmRefIsSubtypeOfAbstractAndBranch(
            ifTrue, ifFalse, isSubTypeOf->sourceType(), isSubTypeOf->destType(),
            ref, regs.scratch1),
        test);
    return;
  }

  if (opd->isWasmRefIsSubtypeOfConcrete() && opd->isEmittedAtUses()) {
    MWasmRefIsSubtypeOfConcrete* isSubTypeOf =
        opd->toWasmRefIsSubtypeOfConcrete();

    LAllocation ref = useRegister(isSubTypeOf->ref());
    WasmRefIsSubtypeDefs regs =
        useWasmRefIsSubtype(isSubTypeOf->destType(), isSubTypeOf->superSTV());
    add(new (alloc()) LWasmRefIsSubtypeOfConcreteAndBranch(
            ifTrue, ifFalse, isSubTypeOf->sourceType(), isSubTypeOf->destType(),
            ref, regs.superSTV, regs.scratch1, regs.scratch2),
        test);
    return;
  }

  if (opd->isIsNullOrUndefined() && opd->isEmittedAtUses()) {
    MIsNullOrUndefined* isNullOrUndefined = opd->toIsNullOrUndefined();
    MDefinition* input = isNullOrUndefined->value();

    if (input->type() == MIRType::Value) {
      auto* lir = new (alloc()) LIsNullOrUndefinedAndBranch(
          isNullOrUndefined, ifTrue, ifFalse, useBoxAtStart(input));
      add(lir, test);
    } else {
      auto* target = IsNullOrUndefined(input->type()) ? ifTrue : ifFalse;
      add(new (alloc()) LGoto(target));
    }
    return;
  }

  if (opd->isIsNoIter()) {
    MOZ_ASSERT(opd->isEmittedAtUses());

    MDefinition* input = opd->toIsNoIter()->input();
    MOZ_ASSERT(input->type() == MIRType::Value);

    LIsNoIterAndBranch* lir =
        new (alloc()) LIsNoIterAndBranch(ifTrue, ifFalse, useBox(input));
    add(lir, test);
    return;
  }

  if (opd->isIteratorHasIndices()) {
    MOZ_ASSERT(opd->isEmittedAtUses());

    MDefinition* object = opd->toIteratorHasIndices()->object();
    MDefinition* iterator = opd->toIteratorHasIndices()->iterator();
    LIteratorHasIndicesAndBranch* lir = new (alloc())
        LIteratorHasIndicesAndBranch(ifTrue, ifFalse, useRegister(object),
                                     useRegister(iterator), temp(), temp());
    add(lir, test);
    return;
  }

  switch (opd->type()) {
    case MIRType::Double:
      add(new (alloc()) LTestDAndBranch(useRegister(opd), ifTrue, ifFalse));
      break;
    case MIRType::Float32:
      add(new (alloc()) LTestFAndBranch(useRegister(opd), ifTrue, ifFalse));
      break;
    case MIRType::Int32:
    case MIRType::Boolean:
      add(new (alloc()) LTestIAndBranch(useRegister(opd), ifTrue, ifFalse));
      break;
    case MIRType::Int64:
      add(new (alloc())
              LTestI64AndBranch(useInt64Register(opd), ifTrue, ifFalse));
      break;
    case MIRType::BigInt:
      add(new (alloc()) LTestBIAndBranch(useRegister(opd), ifTrue, ifFalse));
      break;
    default:
      MOZ_CRASH("Bad type");
  }
}

static inline bool CanEmitCompareAtUses(MInstruction* ins) {
  if (!ins->canEmitAtUses()) {
    return false;
  }

  // If the result is never used, we can usefully defer emission to the use
  // point, since that will never happen.
  MUseIterator iter(ins->usesBegin());
  if (iter == ins->usesEnd()) {
    return true;
  }

  // If the first use isn't of the expected form, the answer is No.
  MNode* node = iter->consumer();
  if (!node->isDefinition()) {
    return false;
  }

  MDefinition* use = node->toDefinition();
  if (!use->isTest() && !use->isWasmSelect()) {
    return false;
  }

  // Emission can be deferred to the first use point, but only if there are no
  // other use points.
  iter++;
  return iter == ins->usesEnd();
}

void LIRGenerator::visitCompare(MCompare* comp) {
  MDefinition* left = comp->lhs();
  MDefinition* right = comp->rhs();

  // Try to fold the comparison so that we don't have to handle all cases.
  bool result;
  if (comp->tryFold(&result)) {
    define(new (alloc()) LInteger(result), comp);
    return;
  }

  // Move below the emitAtUses call if we ever implement
  // LCompareSAndBranch. Doing this now wouldn't be wrong, but doesn't
  // make sense and avoids confusion.
  if (comp->compareType() == MCompare::Compare_String) {
    MConstant* constant = nullptr;
    MDefinition* input = nullptr;
    if (left->isConstant()) {
      constant = left->toConstant();
      input = right;
    } else if (right->isConstant()) {
      constant = right->toConstant();
      input = left;
    }

    if (constant) {
      JSLinearString* linear = &constant->toString()->asLinear();

      if (IsEqualityOp(comp->jsop())) {
        if (MacroAssembler::canCompareStringCharsInline(linear)) {
          auto* lir = new (alloc()) LCompareSInline(useRegister(input), linear);
          define(lir, comp);
          assignSafepoint(lir, comp);
          return;
        }
      } else {
        MOZ_ASSERT(IsRelationalOp(comp->jsop()));

        if (linear->length() == 1) {
          // Move the constant value into the right-hand side operand.
          JSOp op = comp->jsop();
          if (left == constant) {
            op = ReverseCompareOp(op);
          }

          auto* lir = new (alloc())
              LCompareSSingle(useRegister(input), temp(), op, linear);
          define(lir, comp);
          return;
        }
      }
    }

    LCompareS* lir =
        new (alloc()) LCompareS(useRegister(left), useRegister(right));
    define(lir, comp);
    assignSafepoint(lir, comp);
    return;
  }

  // Compare two BigInts.
  if (comp->compareType() == MCompare::Compare_BigInt) {
    auto* lir = new (alloc()) LCompareBigInt(
        useRegister(left), useRegister(right), temp(), temp(), temp());
    define(lir, comp);
    return;
  }

  // Compare BigInt with Int32.
  if (comp->compareType() == MCompare::Compare_BigInt_Int32) {
    auto* lir = new (alloc()) LCompareBigIntInt32(
        useRegister(left), useRegister(right), temp(), temp());
    define(lir, comp);
    return;
  }

  // Compare BigInt with Double.
  if (comp->compareType() == MCompare::Compare_BigInt_Double) {
    auto* lir = new (alloc()) LCompareBigIntDouble(useRegisterAtStart(left),
                                                   useRegisterAtStart(right));
    defineReturn(lir, comp);
    return;
  }

  // Compare BigInt with String.
  if (comp->compareType() == MCompare::Compare_BigInt_String) {
    auto* lir = new (alloc()) LCompareBigIntString(useRegisterAtStart(left),
                                                   useRegisterAtStart(right));
    defineReturn(lir, comp);
    assignSafepoint(lir, comp);
    return;
  }

  // Sniff out if the output of this compare is used only for a branching.
  // If it is, then we will emit an LCompare*AndBranch instruction in place
  // of this compare and any test that uses this compare. Thus, we can
  // ignore this Compare.
  if (CanEmitCompareAtUses(comp)) {
    emitAtUses(comp);
    return;
  }

  // Compare Null and Undefined.
  if (comp->compareType() == MCompare::Compare_Null ||
      comp->compareType() == MCompare::Compare_Undefined) {
    if (left->type() == MIRType::Object) {
      define(new (alloc()) LIsNullOrLikeUndefinedT(useRegister(left)), comp);
      return;
    }

    if (IsLooseEqualityOp(comp->jsop())) {
      auto* lir =
          new (alloc()) LIsNullOrLikeUndefinedV(useBox(left), tempToUnbox());
      define(lir, comp);
      return;
    }

    if (comp->compareType() == MCompare::Compare_Null) {
      auto* lir = new (alloc()) LIsNull(useBox(left));
      define(lir, comp);
      return;
    }

    auto* lir = new (alloc()) LIsUndefined(useBox(left));
    define(lir, comp);
    return;
  }

  // Compare Int32, Symbol, Object or Wasm pointers.
  if (comp->isInt32Comparison() ||
      comp->compareType() == MCompare::Compare_UInt32 ||
      comp->compareType() == MCompare::Compare_UIntPtr ||
      comp->compareType() == MCompare::Compare_Object ||
      comp->compareType() == MCompare::Compare_Symbol ||
      comp->compareType() == MCompare::Compare_WasmAnyRef) {
    JSOp op = ReorderComparison(comp->jsop(), &left, &right);
    LAllocation lhs = useRegister(left);
    LAllocation rhs;
    if (comp->isInt32Comparison() ||
        comp->compareType() == MCompare::Compare_UInt32 ||
        comp->compareType() == MCompare::Compare_UIntPtr) {
      rhs = useAnyOrInt32Constant(right);
    } else {
      rhs = useAny(right);
    }
    define(new (alloc()) LCompare(op, lhs, rhs), comp);
    return;
  }

  // Compare Int64.
  if (comp->compareType() == MCompare::Compare_Int64 ||
      comp->compareType() == MCompare::Compare_UInt64) {
    JSOp op = ReorderComparison(comp->jsop(), &left, &right);
    define(new (alloc()) LCompareI64(op, useInt64Register(left),
                                     useInt64OrConstant(right)),
           comp);
    return;
  }

  // Compare doubles.
  if (comp->isDoubleComparison()) {
    define(new (alloc()) LCompareD(useRegister(left), useRegister(right)),
           comp);
    return;
  }

  // Compare float32.
  if (comp->isFloat32Comparison()) {
    define(new (alloc()) LCompareF(useRegister(left), useRegister(right)),
           comp);
    return;
  }

  MOZ_CRASH("Unrecognized compare type.");
}

void LIRGenerator::visitSameValueDouble(MSameValueDouble* ins) {
  MDefinition* lhs = ins->lhs();
  MDefinition* rhs = ins->rhs();

  MOZ_ASSERT(lhs->type() == MIRType::Double);
  MOZ_ASSERT(rhs->type() == MIRType::Double);

  auto* lir = new (alloc())
      LSameValueDouble(useRegister(lhs), useRegister(rhs), tempDouble());
  define(lir, ins);
}

void LIRGenerator::visitSameValue(MSameValue* ins) {
  MDefinition* lhs = ins->lhs();
  MDefinition* rhs = ins->rhs();

  MOZ_ASSERT(lhs->type() == MIRType::Value);
  MOZ_ASSERT(rhs->type() == MIRType::Value);

  auto* lir = new (alloc()) LSameValue(useBox(lhs), useBox(rhs));
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::lowerBitOp(JSOp op, MBinaryInstruction* ins) {
  MDefinition* lhs = ins->getOperand(0);
  MDefinition* rhs = ins->getOperand(1);
  MOZ_ASSERT(IsIntType(ins->type()));

  if (ins->type() == MIRType::Int32) {
    MOZ_ASSERT(lhs->type() == MIRType::Int32);
    MOZ_ASSERT(rhs->type() == MIRType::Int32);
    ReorderCommutative(&lhs, &rhs, ins);
    lowerForALU(new (alloc()) LBitOpI(op), ins, lhs, rhs);
    return;
  }

  if (ins->type() == MIRType::Int64) {
    MOZ_ASSERT(lhs->type() == MIRType::Int64);
    MOZ_ASSERT(rhs->type() == MIRType::Int64);
    ReorderCommutative(&lhs, &rhs, ins);
    lowerForALUInt64(new (alloc()) LBitOpI64(op), ins, lhs, rhs);
    return;
  }

  MOZ_CRASH("Unhandled integer specialization");
}

void LIRGenerator::visitTypeOf(MTypeOf* ins) {
  MDefinition* opd = ins->input();

  if (opd->type() == MIRType::Object) {
    auto* lir = new (alloc()) LTypeOfO(useRegister(opd));
    define(lir, ins);
    return;
  }

  MOZ_ASSERT(opd->type() == MIRType::Value);

  LTypeOfV* lir = new (alloc()) LTypeOfV(useBox(opd), tempToUnbox());
  define(lir, ins);
}

void LIRGenerator::visitTypeOfName(MTypeOfName* ins) {
  MDefinition* input = ins->input();
  MOZ_ASSERT(input->type() == MIRType::Int32);

  auto* lir = new (alloc()) LTypeOfName(useRegister(input));
  define(lir, ins);
}

void LIRGenerator::visitTypeOfIs(MTypeOfIs* ins) {
  MDefinition* input = ins->input();

  MOZ_ASSERT(input->type() == MIRType::Object ||
             input->type() == MIRType::Value);

  switch (ins->jstype()) {
    case JSTYPE_UNDEFINED:
    case JSTYPE_OBJECT:
    case JSTYPE_FUNCTION: {
      if (input->type() == MIRType::Object) {
        auto* lir = new (alloc()) LTypeOfIsNonPrimitiveO(useRegister(input));
        define(lir, ins);
      } else {
        auto* lir =
            new (alloc()) LTypeOfIsNonPrimitiveV(useBox(input), tempToUnbox());
        define(lir, ins);
      }
      return;
    }

    case JSTYPE_STRING:
    case JSTYPE_NUMBER:
    case JSTYPE_BOOLEAN:
    case JSTYPE_SYMBOL:
    case JSTYPE_BIGINT: {
      MOZ_ASSERT(input->type() == MIRType::Value);

      auto* lir = new (alloc()) LTypeOfIsPrimitive(useBoxAtStart(input));
      define(lir, ins);
      return;
    }

#ifdef ENABLE_RECORD_TUPLE
    case JSTYPE_RECORD:
    case JSTYPE_TUPLE:
#endif
    case JSTYPE_LIMIT:
      break;
  }
  MOZ_CRASH("Unhandled JSType");
}

void LIRGenerator::visitToAsyncIter(MToAsyncIter* ins) {
  LToAsyncIter* lir = new (alloc()) LToAsyncIter(
      useRegisterAtStart(ins->iterator()), useBoxAtStart(ins->nextMethod()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitToPropertyKeyCache(MToPropertyKeyCache* ins) {
  MDefinition* input = ins->getOperand(0);
  MOZ_ASSERT(ins->type() == MIRType::Value);

  auto* lir = new (alloc()) LToPropertyKeyCache(useBox(input));
  defineBox(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitBitNot(MBitNot* ins) {
  MDefinition* input = ins->getOperand(0);

  if (ins->type() == MIRType::Int32) {
    MOZ_ASSERT(input->type() == MIRType::Int32);
    lowerForALU(new (alloc()) LBitNotI(), ins, input);
    return;
  }

  if (ins->type() == MIRType::Int64) {
    MOZ_ASSERT(input->type() == MIRType::Int64);
    lowerForALUInt64(new (alloc()) LBitNotI64(), ins, input);
    return;
  }

  MOZ_CRASH("Unhandled integer specialization");
}

static bool CanEmitBitAndAtUses(MInstruction* ins) {
  if (!ins->canEmitAtUses()) {
    return false;
  }

  MIRType tyL = ins->getOperand(0)->type();
  MIRType tyR = ins->getOperand(1)->type();
  if (tyL != tyR || (tyL != MIRType::Int32 && tyL != MIRType::Int64)) {
    return false;
  }

  MUseIterator iter(ins->usesBegin());
  if (iter == ins->usesEnd()) {
    return false;
  }

  MNode* node = iter->consumer();
  if (!node->isDefinition() || !node->toDefinition()->isInstruction()) {
    return false;
  }

  MInstruction* use = node->toDefinition()->toInstruction();
  if (!use->isTest() && !(use->isCompare() && CanEmitCompareAtUses(use))) {
    return false;
  }

  iter++;
  return iter == ins->usesEnd();
}

void LIRGenerator::visitBitAnd(MBitAnd* ins) {
  // Sniff out if the output of this bitand is used only for a branching.
  // If it is, then we will emit an LBitAndAndBranch instruction in place
  // of this bitand and any test that uses this bitand. Thus, we can
  // ignore this BitAnd.
  if (CanEmitBitAndAtUses(ins)) {
    emitAtUses(ins);
  } else {
    lowerBitOp(JSOp::BitAnd, ins);
  }
}

void LIRGenerator::visitBitOr(MBitOr* ins) { lowerBitOp(JSOp::BitOr, ins); }

void LIRGenerator::visitBitXor(MBitXor* ins) { lowerBitOp(JSOp::BitXor, ins); }

void LIRGenerator::visitWasmBinaryBitwise(MWasmBinaryBitwise* ins) {
  switch (ins->subOpcode()) {
    case MWasmBinaryBitwise::SubOpcode::And:
      if (CanEmitBitAndAtUses(ins)) {
        emitAtUses(ins);
      } else {
        lowerBitOp(JSOp::BitAnd, ins);
      }
      break;
    case MWasmBinaryBitwise::SubOpcode::Or:
      lowerBitOp(JSOp::BitOr, ins);
      break;
    case MWasmBinaryBitwise::SubOpcode::Xor:
      lowerBitOp(JSOp::BitXor, ins);
      break;
    default:
      MOZ_CRASH();
  }
}

void LIRGenerator::lowerShiftOp(JSOp op, MShiftInstruction* ins) {
  MDefinition* lhs = ins->getOperand(0);
  MDefinition* rhs = ins->getOperand(1);

  if (op == JSOp::Ursh && ins->type() == MIRType::Double) {
    MOZ_ASSERT(lhs->type() == MIRType::Int32);
    MOZ_ASSERT(rhs->type() == MIRType::Int32);
    lowerUrshD(ins->toUrsh());
    return;
  }

  MOZ_ASSERT(IsIntType(ins->type()));

  if (ins->type() == MIRType::Int32) {
    MOZ_ASSERT(lhs->type() == MIRType::Int32);
    MOZ_ASSERT(rhs->type() == MIRType::Int32);

    LShiftI* lir = new (alloc()) LShiftI(op);
    if (op == JSOp::Ursh) {
      if (ins->toUrsh()->fallible()) {
        assignSnapshot(lir, ins->bailoutKind());
      }
    }
    lowerForShift(lir, ins, lhs, rhs);
    return;
  }

  if (ins->type() == MIRType::Int64) {
    MOZ_ASSERT(lhs->type() == MIRType::Int64);
    MOZ_ASSERT(rhs->type() == MIRType::Int64);
    lowerForShiftInt64(new (alloc()) LShiftI64(op), ins, lhs, rhs);
    return;
  }

  MOZ_CRASH("Unhandled integer specialization");
}

void LIRGenerator::visitLsh(MLsh* ins) { lowerShiftOp(JSOp::Lsh, ins); }

void LIRGenerator::visitRsh(MRsh* ins) { lowerShiftOp(JSOp::Rsh, ins); }

void LIRGenerator::visitUrsh(MUrsh* ins) { lowerShiftOp(JSOp::Ursh, ins); }

void LIRGenerator::visitSignExtendInt32(MSignExtendInt32* ins) {
  LInstructionHelper<1, 1, 0>* lir;

  if (ins->mode() == MSignExtendInt32::Byte) {
    lir = new (alloc())
        LSignExtendInt32(useByteOpRegisterAtStart(ins->input()), ins->mode());
  } else {
    lir = new (alloc())
        LSignExtendInt32(useRegisterAtStart(ins->input()), ins->mode());
  }

  define(lir, ins);
}

void LIRGenerator::visitRotate(MRotate* ins) {
  MDefinition* input = ins->input();
  MDefinition* count = ins->count();

  if (ins->type() == MIRType::Int32) {
    auto* lir = new (alloc()) LRotate();
    lowerForShift(lir, ins, input, count);
  } else if (ins->type() == MIRType::Int64) {
    auto* lir = new (alloc()) LRotateI64();
    lowerForShiftInt64(lir, ins, input, count);
  } else {
    MOZ_CRASH("unexpected type in visitRotate");
  }
}

void LIRGenerator::visitFloor(MFloor* ins) {
  MIRType type = ins->input()->type();
  MOZ_ASSERT(IsFloatingPointType(type));

  LInstructionHelper<1, 1, 0>* lir;
  if (type == MIRType::Double) {
    lir = new (alloc()) LFloor(useRegister(ins->input()));
  } else {
    lir = new (alloc()) LFloorF(useRegister(ins->input()));
  }

  assignSnapshot(lir, ins->bailoutKind());
  define(lir, ins);
}

void LIRGenerator::visitCeil(MCeil* ins) {
  MIRType type = ins->input()->type();
  MOZ_ASSERT(IsFloatingPointType(type));

  LInstructionHelper<1, 1, 0>* lir;
  if (type == MIRType::Double) {
    lir = new (alloc()) LCeil(useRegister(ins->input()));
  } else {
    lir = new (alloc()) LCeilF(useRegister(ins->input()));
  }

  assignSnapshot(lir, ins->bailoutKind());
  define(lir, ins);
}

void LIRGenerator::visitRound(MRound* ins) {
  MIRType type = ins->input()->type();
  MOZ_ASSERT(IsFloatingPointType(type));

  LInstructionHelper<1, 1, 1>* lir;
  if (type == MIRType::Double) {
    lir = new (alloc()) LRound(useRegister(ins->input()), tempDouble());
  } else {
    lir = new (alloc()) LRoundF(useRegister(ins->input()), tempFloat32());
  }

  assignSnapshot(lir, ins->bailoutKind());
  define(lir, ins);
}

void LIRGenerator::visitTrunc(MTrunc* ins) {
  MIRType type = ins->input()->type();
  MOZ_ASSERT(IsFloatingPointType(type));

  LInstructionHelper<1, 1, 0>* lir;
  if (type == MIRType::Double) {
    lir = new (alloc()) LTrunc(useRegister(ins->input()));
  } else {
    lir = new (alloc()) LTruncF(useRegister(ins->input()));
  }

  assignSnapshot(lir, ins->bailoutKind());
  define(lir, ins);
}

void LIRGenerator::visitNearbyInt(MNearbyInt* ins) {
  MIRType inputType = ins->input()->type();
  MOZ_ASSERT(IsFloatingPointType(inputType));
  MOZ_ASSERT(ins->type() == inputType);

  LInstructionHelper<1, 1, 0>* lir;
  if (inputType == MIRType::Double) {
    lir = new (alloc()) LNearbyInt(useRegisterAtStart(ins->input()));
  } else {
    lir = new (alloc()) LNearbyIntF(useRegisterAtStart(ins->input()));
  }

  define(lir, ins);
}

void LIRGenerator::visitMinMax(MMinMax* ins) {
  MDefinition* first = ins->getOperand(0);
  MDefinition* second = ins->getOperand(1);

  ReorderCommutative(&first, &second, ins);

  LMinMaxBase* lir;
  switch (ins->type()) {
    case MIRType::Int32:
      lir = new (alloc())
          LMinMaxI(useRegisterAtStart(first), useRegisterOrConstant(second));
      break;
    case MIRType::Float32:
      lir = new (alloc())
          LMinMaxF(useRegisterAtStart(first), useRegister(second));
      break;
    case MIRType::Double:
      lir = new (alloc())
          LMinMaxD(useRegisterAtStart(first), useRegister(second));
      break;
    default:
      MOZ_CRASH();
  }

  // Input reuse is OK (for now) even on ARM64: floating min/max are fairly
  // expensive due to SNaN -> QNaN conversion, and int min/max is for asm.js.
  defineReuseInput(lir, ins, 0);
}

void LIRGenerator::visitMinMaxArray(MMinMaxArray* ins) {
  LInstructionHelper<1, 1, 3>* lir;
  if (ins->type() == MIRType::Int32) {
    lir = new (alloc())
        LMinMaxArrayI(useRegisterAtStart(ins->array()), temp(), temp(), temp());
  } else {
    MOZ_ASSERT(ins->type() == MIRType::Double);
    lir = new (alloc()) LMinMaxArrayD(useRegisterAtStart(ins->array()),
                                      tempDouble(), temp(), temp());
  }
  assignSnapshot(lir, ins->bailoutKind());
  define(lir, ins);
}

LInstructionHelper<1, 1, 0>* LIRGenerator::allocateAbs(MAbs* ins,
                                                       LAllocation input) {
  MDefinition* num = ins->input();
  MOZ_ASSERT(IsNumberType(num->type()));

  LInstructionHelper<1, 1, 0>* lir;
  switch (num->type()) {
    case MIRType::Int32:
      lir = new (alloc()) LAbsI(input);
      // needed to handle abs(INT32_MIN)
      if (ins->fallible()) {
        assignSnapshot(lir, ins->bailoutKind());
      }
      break;
    case MIRType::Float32:
      lir = new (alloc()) LAbsF(input);
      break;
    case MIRType::Double:
      lir = new (alloc()) LAbsD(input);
      break;
    default:
      MOZ_CRASH();
  }
  return lir;
}

void LIRGenerator::visitClz(MClz* ins) {
  MDefinition* num = ins->num();

  MOZ_ASSERT(IsIntType(ins->type()));

  if (ins->type() == MIRType::Int32) {
    LClzI* lir = new (alloc()) LClzI(useRegisterAtStart(num));
    define(lir, ins);
    return;
  }

  auto* lir = new (alloc()) LClzI64(useInt64RegisterAtStart(num));
  defineInt64(lir, ins);
}

void LIRGenerator::visitCtz(MCtz* ins) {
  MDefinition* num = ins->num();

  MOZ_ASSERT(IsIntType(ins->type()));

  if (ins->type() == MIRType::Int32) {
    LCtzI* lir = new (alloc()) LCtzI(useRegisterAtStart(num));
    define(lir, ins);
    return;
  }

  auto* lir = new (alloc()) LCtzI64(useInt64RegisterAtStart(num));
  defineInt64(lir, ins);
}

void LIRGenerator::visitPopcnt(MPopcnt* ins) {
  MDefinition* num = ins->num();

  MOZ_ASSERT(IsIntType(ins->type()));

  if (ins->type() == MIRType::Int32) {
    LPopcntI* lir = new (alloc()) LPopcntI(useRegisterAtStart(num), temp());
    define(lir, ins);
    return;
  }

  auto* lir = new (alloc()) LPopcntI64(useInt64RegisterAtStart(num), temp());
  defineInt64(lir, ins);
}

void LIRGenerator::visitSqrt(MSqrt* ins) {
  MDefinition* num = ins->input();
  MOZ_ASSERT(IsFloatingPointType(num->type()));

  LInstructionHelper<1, 1, 0>* lir;
  if (num->type() == MIRType::Double) {
    lir = new (alloc()) LSqrtD(useRegisterAtStart(num));
  } else {
    lir = new (alloc()) LSqrtF(useRegisterAtStart(num));
  }
  define(lir, ins);
}

void LIRGenerator::visitAtan2(MAtan2* ins) {
  MDefinition* y = ins->y();
  MOZ_ASSERT(y->type() == MIRType::Double);

  MDefinition* x = ins->x();
  MOZ_ASSERT(x->type() == MIRType::Double);

  LAtan2D* lir =
      new (alloc()) LAtan2D(useRegisterAtStart(y), useRegisterAtStart(x));
  defineReturn(lir, ins);
}

void LIRGenerator::visitHypot(MHypot* ins) {
  LHypot* lir = nullptr;
  uint32_t length = ins->numOperands();
  for (uint32_t i = 0; i < length; ++i) {
    MOZ_ASSERT(ins->getOperand(i)->type() == MIRType::Double);
  }

  switch (length) {
    case 2:
      lir = new (alloc()) LHypot(useRegisterAtStart(ins->getOperand(0)),
                                 useRegisterAtStart(ins->getOperand(1)));
      break;
    case 3:
      lir = new (alloc()) LHypot(useRegisterAtStart(ins->getOperand(0)),
                                 useRegisterAtStart(ins->getOperand(1)),
                                 useRegisterAtStart(ins->getOperand(2)));
      break;
    case 4:
      lir = new (alloc()) LHypot(useRegisterAtStart(ins->getOperand(0)),
                                 useRegisterAtStart(ins->getOperand(1)),
                                 useRegisterAtStart(ins->getOperand(2)),
                                 useRegisterAtStart(ins->getOperand(3)));
      break;
    default:
      MOZ_CRASH("Unexpected number of arguments to LHypot.");
  }

  defineReturn(lir, ins);
}

void LIRGenerator::visitPow(MPow* ins) {
  MDefinition* input = ins->input();
  MDefinition* power = ins->power();

  if (ins->type() == MIRType::Int32) {
    MOZ_ASSERT(input->type() == MIRType::Int32);
    MOZ_ASSERT(power->type() == MIRType::Int32);

    if (input->isConstant()) {
      // Restrict this optimization to |base <= 256| to avoid generating too
      // many consecutive shift instructions.
      int32_t base = input->toConstant()->toInt32();
      if (2 <= base && base <= 256 && mozilla::IsPowerOfTwo(uint32_t(base))) {
        lowerPowOfTwoI(ins);
        return;
      }
    }

    auto* lir = new (alloc())
        LPowII(useRegister(input), useRegister(power), temp(), temp());
    assignSnapshot(lir, ins->bailoutKind());
    define(lir, ins);
    return;
  }

  MOZ_ASSERT(ins->type() == MIRType::Double);
  MOZ_ASSERT(input->type() == MIRType::Double);
  MOZ_ASSERT(power->type() == MIRType::Int32 ||
             power->type() == MIRType::Double);

  LInstruction* lir;
  if (power->type() == MIRType::Int32) {
    lir = new (alloc())
        LPowI(useRegisterAtStart(input), useRegisterAtStart(power));
  } else {
    lir = new (alloc())
        LPowD(useRegisterAtStart(input), useRegisterAtStart(power));
  }
  defineReturn(lir, ins);
}

void LIRGenerator::visitSign(MSign* ins) {
  if (ins->type() == ins->input()->type()) {
    LInstructionHelper<1, 1, 0>* lir;
    if (ins->type() == MIRType::Int32) {
      lir = new (alloc()) LSignI(useRegister(ins->input()));
    } else {
      MOZ_ASSERT(ins->type() == MIRType::Double);
      lir = new (alloc()) LSignD(useRegister(ins->input()));
    }
    define(lir, ins);
  } else {
    MOZ_ASSERT(ins->type() == MIRType::Int32);
    MOZ_ASSERT(ins->input()->type() == MIRType::Double);

    auto* lir = new (alloc()) LSignDI(useRegister(ins->input()), tempDouble());
    assignSnapshot(lir, ins->bailoutKind());
    define(lir, ins);
  }
}

void LIRGenerator::visitMathFunction(MMathFunction* ins) {
  MOZ_ASSERT(IsFloatingPointType(ins->type()));
  MOZ_ASSERT(ins->type() == ins->input()->type());

  LInstruction* lir;
  if (ins->type() == MIRType::Double) {
    lir = new (alloc()) LMathFunctionD(useRegisterAtStart(ins->input()));
  } else {
    lir = new (alloc()) LMathFunctionF(useRegisterAtStart(ins->input()));
  }
  defineReturn(lir, ins);
}

void LIRGenerator::visitRandom(MRandom* ins) {
  auto* lir = new (alloc()) LRandom(temp(), tempInt64(), tempInt64());
  define(lir, ins);
}

// Try to mark an add or sub instruction as able to recover its input when
// bailing out.
template <typename S, typename T>
static void MaybeSetRecoversInput(S* mir, T* lir) {
  MOZ_ASSERT(lir->mirRaw() == mir);
  if (!mir->fallible() || !lir->snapshot()) {
    return;
  }

  if (lir->output()->policy() != LDefinition::MUST_REUSE_INPUT) {
    return;
  }

  // The original operands to an add or sub can't be recovered if they both
  // use the same register.
  if (lir->lhs()->isUse() && lir->rhs()->isUse() &&
      lir->lhs()->toUse()->virtualRegister() ==
          lir->rhs()->toUse()->virtualRegister()) {
    return;
  }

  // Add instructions that are on two different values can recover
  // the input they clobbered via MUST_REUSE_INPUT. Thus, a copy
  // of that input does not need to be kept alive in the snapshot
  // for the instruction.

  lir->setRecoversInput();

  const LUse* input = lir->getOperand(lir->output()->getReusedInput())->toUse();
  lir->snapshot()->rewriteRecoveredInput(*input);
}

void LIRGenerator::visitAdd(MAdd* ins) {
  MDefinition* lhs = ins->getOperand(0);
  MDefinition* rhs = ins->getOperand(1);

  MOZ_ASSERT(lhs->type() == rhs->type());
  MOZ_ASSERT(IsNumberType(ins->type()));

  if (ins->type() == MIRType::Int32) {
    MOZ_ASSERT(lhs->type() == MIRType::Int32);
    ReorderCommutative(&lhs, &rhs, ins);
    LAddI* lir = new (alloc()) LAddI;

    if (ins->fallible()) {
      assignSnapshot(lir, ins->bailoutKind());
    }

    lowerForALU(lir, ins, lhs, rhs);
    MaybeSetRecoversInput(ins, lir);
    return;
  }

  if (ins->type() == MIRType::Int64) {
    MOZ_ASSERT(lhs->type() == MIRType::Int64);
    ReorderCommutative(&lhs, &rhs, ins);
    LAddI64* lir = new (alloc()) LAddI64;
    lowerForALUInt64(lir, ins, lhs, rhs);
    return;
  }

  if (ins->type() == MIRType::Double) {
    MOZ_ASSERT(lhs->type() == MIRType::Double);
    ReorderCommutative(&lhs, &rhs, ins);
    lowerForFPU(new (alloc()) LMathD(JSOp::Add), ins, lhs, rhs);
    return;
  }

  if (ins->type() == MIRType::Float32) {
    MOZ_ASSERT(lhs->type() == MIRType::Float32);
    ReorderCommutative(&lhs, &rhs, ins);
    lowerForFPU(new (alloc()) LMathF(JSOp::Add), ins, lhs, rhs);
    return;
  }

  MOZ_CRASH("Unhandled number specialization");
}

void LIRGenerator::visitSub(MSub* ins) {
  MDefinition* lhs = ins->lhs();
  MDefinition* rhs = ins->rhs();

  MOZ_ASSERT(lhs->type() == rhs->type());
  MOZ_ASSERT(IsNumberType(ins->type()));

  if (ins->type() == MIRType::Int32) {
    MOZ_ASSERT(lhs->type() == MIRType::Int32);

    LSubI* lir = new (alloc()) LSubI;
    if (ins->fallible()) {
      assignSnapshot(lir, ins->bailoutKind());
    }

    // If our LHS is a constant 0 and we don't have to worry about results that
    // can't be represented as an int32, we can optimize to an LNegI.
    if (!ins->fallible() && lhs->isConstant() &&
        lhs->toConstant()->toInt32() == 0) {
      lowerNegI(ins, rhs);
      return;
    }

    lowerForALU(lir, ins, lhs, rhs);
    MaybeSetRecoversInput(ins, lir);
    return;
  }

  if (ins->type() == MIRType::Int64) {
    MOZ_ASSERT(lhs->type() == MIRType::Int64);

    // If our LHS is a constant 0, we can optimize to an LNegI64.
    if (lhs->isConstant() && lhs->toConstant()->toInt64() == 0) {
      lowerNegI64(ins, rhs);
      return;
    }

    LSubI64* lir = new (alloc()) LSubI64;
    lowerForALUInt64(lir, ins, lhs, rhs);
    return;
  }

  if (ins->type() == MIRType::Double) {
    MOZ_ASSERT(lhs->type() == MIRType::Double);
    lowerForFPU(new (alloc()) LMathD(JSOp::Sub), ins, lhs, rhs);
    return;
  }

  if (ins->type() == MIRType::Float32) {
    MOZ_ASSERT(lhs->type() == MIRType::Float32);
    lowerForFPU(new (alloc()) LMathF(JSOp::Sub), ins, lhs, rhs);
    return;
  }

  MOZ_CRASH("Unhandled number specialization");
}

void LIRGenerator::visitMul(MMul* ins) {
  MDefinition* lhs = ins->lhs();
  MDefinition* rhs = ins->rhs();
  MOZ_ASSERT(lhs->type() == rhs->type());
  MOZ_ASSERT(IsNumberType(ins->type()));

  if (ins->type() == MIRType::Int32) {
    MOZ_ASSERT(lhs->type() == MIRType::Int32);
    ReorderCommutative(&lhs, &rhs, ins);

    // If our RHS is a constant -1 and we don't have to worry about results that
    // can't be represented as an int32, we can optimize to an LNegI.
    if (!ins->fallible() && rhs->isConstant() &&
        rhs->toConstant()->toInt32() == -1) {
      lowerNegI(ins, lhs);
      return;
    }

    lowerMulI(ins, lhs, rhs);
    return;
  }

  if (ins->type() == MIRType::Int64) {
    MOZ_ASSERT(lhs->type() == MIRType::Int64);
    ReorderCommutative(&lhs, &rhs, ins);

    // If our RHS is a constant -1, we can optimize to an LNegI64.
    if (rhs->isConstant() && rhs->toConstant()->toInt64() == -1) {
      lowerNegI64(ins, lhs);
      return;
    }

    LMulI64* lir = new (alloc()) LMulI64;
    lowerForMulInt64(lir, ins, lhs, rhs);
    return;
  }

  if (ins->type() == MIRType::Double) {
    MOZ_ASSERT(lhs->type() == MIRType::Double);
    ReorderCommutative(&lhs, &rhs, ins);

    // If our RHS is a constant -1.0, we can optimize to an LNegD.
    if (!ins->mustPreserveNaN() && rhs->isConstant() &&
        rhs->toConstant()->toDouble() == -1.0) {
      defineReuseInput(new (alloc()) LNegD(useRegisterAtStart(lhs)), ins, 0);
      return;
    }

    lowerForFPU(new (alloc()) LMathD(JSOp::Mul), ins, lhs, rhs);
    return;
  }

  if (ins->type() == MIRType::Float32) {
    MOZ_ASSERT(lhs->type() == MIRType::Float32);
    ReorderCommutative(&lhs, &rhs, ins);

    // We apply the same optimizations as for doubles
    if (!ins->mustPreserveNaN() && rhs->isConstant() &&
        rhs->toConstant()->toFloat32() == -1.0f) {
      defineReuseInput(new (alloc()) LNegF(useRegisterAtStart(lhs)), ins, 0);
      return;
    }

    lowerForFPU(new (alloc()) LMathF(JSOp::Mul), ins, lhs, rhs);
    return;
  }

  MOZ_CRASH("Unhandled number specialization");
}

void LIRGenerator::visitDiv(MDiv* ins) {
  MDefinition* lhs = ins->lhs();
  MDefinition* rhs = ins->rhs();
  MOZ_ASSERT(lhs->type() == rhs->type());
  MOZ_ASSERT(IsNumberType(ins->type()));

  if (ins->type() == MIRType::Int32) {
    MOZ_ASSERT(lhs->type() == MIRType::Int32);
    lowerDivI(ins);
    return;
  }

  if (ins->type() == MIRType::Int64) {
    MOZ_ASSERT(lhs->type() == MIRType::Int64);
    lowerDivI64(ins);
    return;
  }

  if (ins->type() == MIRType::Double) {
    MOZ_ASSERT(lhs->type() == MIRType::Double);
    lowerForFPU(new (alloc()) LMathD(JSOp::Div), ins, lhs, rhs);
    return;
  }

  if (ins->type() == MIRType::Float32) {
    MOZ_ASSERT(lhs->type() == MIRType::Float32);
    lowerForFPU(new (alloc()) LMathF(JSOp::Div), ins, lhs, rhs);
    return;
  }

  MOZ_CRASH("Unhandled number specialization");
}

void LIRGenerator::visitWasmBuiltinDivI64(MWasmBuiltinDivI64* div) {
  lowerWasmBuiltinDivI64(div);
}

void LIRGenerator::visitWasmBuiltinModI64(MWasmBuiltinModI64* mod) {
  lowerWasmBuiltinModI64(mod);
}

void LIRGenerator::visitBuiltinInt64ToFloatingPoint(
    MBuiltinInt64ToFloatingPoint* ins) {
  lowerBuiltinInt64ToFloatingPoint(ins);
}

void LIRGenerator::visitWasmBuiltinTruncateToInt64(
    MWasmBuiltinTruncateToInt64* ins) {
  lowerWasmBuiltinTruncateToInt64(ins);
}

void LIRGenerator::visitWasmBuiltinModD(MWasmBuiltinModD* ins) {
  MOZ_ASSERT(gen->compilingWasm());
  LWasmBuiltinModD* lir = new (alloc()) LWasmBuiltinModD(
      useRegisterAtStart(ins->lhs()), useRegisterAtStart(ins->rhs()),
      useFixedAtStart(ins->instance(), InstanceReg));
  defineReturn(lir, ins);
}

void LIRGenerator::visitMod(MMod* ins) {
  MOZ_ASSERT(ins->lhs()->type() == ins->rhs()->type());
  MOZ_ASSERT(IsNumberType(ins->type()));

  if (ins->type() == MIRType::Int32) {
    MOZ_ASSERT(ins->type() == MIRType::Int32);
    MOZ_ASSERT(ins->lhs()->type() == MIRType::Int32);
    lowerModI(ins);
    return;
  }

  if (ins->type() == MIRType::Int64) {
    MOZ_ASSERT(ins->type() == MIRType::Int64);
    MOZ_ASSERT(ins->lhs()->type() == MIRType::Int64);
    lowerModI64(ins);
    return;
  }

  if (ins->type() == MIRType::Double) {
    MOZ_ASSERT(ins->lhs()->type() == MIRType::Double);
    MOZ_ASSERT(ins->rhs()->type() == MIRType::Double);

    MOZ_ASSERT(!gen->compilingWasm());

    if (Assembler::HasRoundInstruction(RoundingMode::TowardsZero)) {
      if (ins->rhs()->isConstant()) {
        double d = ins->rhs()->toConstant()->toDouble();
        int32_t div;
        if (mozilla::NumberIsInt32(d, &div) && div > 0 &&
            mozilla::IsPowerOfTwo(uint32_t(div))) {
          auto* lir = new (alloc()) LModPowTwoD(useRegister(ins->lhs()), div);
          define(lir, ins);
          return;
        }
      }
    }

    LModD* lir = new (alloc())
        LModD(useRegisterAtStart(ins->lhs()), useRegisterAtStart(ins->rhs()));
    defineReturn(lir, ins);
    return;
  }

  MOZ_CRASH("Unhandled number specialization");
}

void LIRGenerator::visitBigIntAdd(MBigIntAdd* ins) {
  auto* lir = new (alloc()) LBigIntAdd(useRegister(ins->lhs()),
                                       useRegister(ins->rhs()), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitBigIntSub(MBigIntSub* ins) {
  auto* lir = new (alloc()) LBigIntSub(useRegister(ins->lhs()),
                                       useRegister(ins->rhs()), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitBigIntMul(MBigIntMul* ins) {
  auto* lir = new (alloc()) LBigIntMul(useRegister(ins->lhs()),
                                       useRegister(ins->rhs()), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitBigIntDiv(MBigIntDiv* ins) { lowerBigIntDiv(ins); }

void LIRGenerator::visitBigIntMod(MBigIntMod* ins) { lowerBigIntMod(ins); }

void LIRGenerator::visitBigIntPow(MBigIntPow* ins) {
  auto* lir = new (alloc()) LBigIntPow(useRegister(ins->lhs()),
                                       useRegister(ins->rhs()), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitBigIntBitAnd(MBigIntBitAnd* ins) {
  auto* lir = new (alloc()) LBigIntBitAnd(
      useRegister(ins->lhs()), useRegister(ins->rhs()), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitBigIntBitOr(MBigIntBitOr* ins) {
  auto* lir = new (alloc()) LBigIntBitOr(
      useRegister(ins->lhs()), useRegister(ins->rhs()), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitBigIntBitXor(MBigIntBitXor* ins) {
  auto* lir = new (alloc()) LBigIntBitXor(
      useRegister(ins->lhs()), useRegister(ins->rhs()), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitBigIntLsh(MBigIntLsh* ins) { lowerBigIntLsh(ins); }

void LIRGenerator::visitBigIntRsh(MBigIntRsh* ins) { lowerBigIntRsh(ins); }

void LIRGenerator::visitBigIntIncrement(MBigIntIncrement* ins) {
  auto* lir =
      new (alloc()) LBigIntIncrement(useRegister(ins->input()), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitBigIntDecrement(MBigIntDecrement* ins) {
  auto* lir =
      new (alloc()) LBigIntDecrement(useRegister(ins->input()), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitBigIntNegate(MBigIntNegate* ins) {
  auto* lir = new (alloc()) LBigIntNegate(useRegister(ins->input()), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitBigIntBitNot(MBigIntBitNot* ins) {
  auto* lir =
      new (alloc()) LBigIntBitNot(useRegister(ins->input()), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitInt32ToStringWithBase(MInt32ToStringWithBase* ins) {
  MOZ_ASSERT(ins->input()->type() == MIRType::Int32);
  MOZ_ASSERT(ins->base()->type() == MIRType::Int32);

  int32_t baseInt =
      ins->base()->isConstant() ? ins->base()->toConstant()->toInt32() : 0;

  LAllocation base;
  if (2 <= baseInt && baseInt <= 36) {
    base = useRegisterOrConstant(ins->base());
  } else {
    base = useRegister(ins->base());
  }

  auto* lir = new (alloc())
      LInt32ToStringWithBase(useRegister(ins->input()), base, temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNumberParseInt(MNumberParseInt* ins) {
  MOZ_ASSERT(ins->string()->type() == MIRType::String);
  MOZ_ASSERT(ins->radix()->type() == MIRType::Int32);

  auto* lir = new (alloc()) LNumberParseInt(useRegisterAtStart(ins->string()),
                                            useRegisterAtStart(ins->radix()),
                                            tempFixed(CallTempReg0));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitDoubleParseInt(MDoubleParseInt* ins) {
  MOZ_ASSERT(ins->number()->type() == MIRType::Double);

  auto* lir =
      new (alloc()) LDoubleParseInt(useRegister(ins->number()), tempDouble());
  assignSnapshot(lir, ins->bailoutKind());
  define(lir, ins);
}

void LIRGenerator::visitConcat(MConcat* ins) {
  MDefinition* lhs = ins->getOperand(0);
  MDefinition* rhs = ins->getOperand(1);

  MOZ_ASSERT(lhs->type() == MIRType::String);
  MOZ_ASSERT(rhs->type() == MIRType::String);
  MOZ_ASSERT(ins->type() == MIRType::String);

  LConcat* lir = new (alloc()) LConcat(
      useFixedAtStart(lhs, CallTempReg0), useFixedAtStart(rhs, CallTempReg1),
      tempFixed(CallTempReg0), tempFixed(CallTempReg1), tempFixed(CallTempReg2),
      tempFixed(CallTempReg3), tempFixed(CallTempReg4));
  defineFixed(lir, ins, LAllocation(AnyRegister(CallTempReg5)));
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitLinearizeString(MLinearizeString* ins) {
  MDefinition* str = ins->string();
  MOZ_ASSERT(str->type() == MIRType::String);

  auto* lir = new (alloc()) LLinearizeString(useRegister(str));
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitLinearizeForCharAccess(MLinearizeForCharAccess* ins) {
  MDefinition* str = ins->string();
  MDefinition* idx = ins->index();

  MOZ_ASSERT(str->type() == MIRType::String);
  MOZ_ASSERT(idx->type() == MIRType::Int32);

  auto* lir =
      new (alloc()) LLinearizeForCharAccess(useRegister(str), useRegister(idx));
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitLinearizeForCodePointAccess(
    MLinearizeForCodePointAccess* ins) {
  MDefinition* str = ins->string();
  MDefinition* idx = ins->index();

  MOZ_ASSERT(str->type() == MIRType::String);
  MOZ_ASSERT(idx->type() == MIRType::Int32);

  auto* lir = new (alloc())
      LLinearizeForCodePointAccess(useRegister(str), useRegister(idx), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitToRelativeStringIndex(MToRelativeStringIndex* ins) {
  MDefinition* index = ins->index();
  MDefinition* length = ins->length();

  MOZ_ASSERT(index->type() == MIRType::Int32);
  MOZ_ASSERT(length->type() == MIRType::Int32);

  auto* lir = new (alloc())
      LToRelativeStringIndex(useRegister(index), useRegister(length));
  define(lir, ins);
}

void LIRGenerator::visitCharCodeAt(MCharCodeAt* ins) {
  MDefinition* str = ins->string();
  MDefinition* idx = ins->index();

  MOZ_ASSERT(str->type() == MIRType::String);
  MOZ_ASSERT(idx->type() == MIRType::Int32);

  auto* lir = new (alloc())
      LCharCodeAt(useRegister(str), useRegisterOrZero(idx), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCharCodeAtOrNegative(MCharCodeAtOrNegative* ins) {
  MDefinition* str = ins->string();
  MDefinition* idx = ins->index();

  MOZ_ASSERT(str->type() == MIRType::String);
  MOZ_ASSERT(idx->type() == MIRType::Int32);

  auto* lir = new (alloc()) LCharCodeAtOrNegative(
      useRegister(str), useRegisterOrZero(idx), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCodePointAt(MCodePointAt* ins) {
  MDefinition* str = ins->string();
  MDefinition* idx = ins->index();

  MOZ_ASSERT(str->type() == MIRType::String);
  MOZ_ASSERT(idx->type() == MIRType::Int32);

  auto* lir = new (alloc())
      LCodePointAt(useRegister(str), useRegister(idx), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCodePointAtOrNegative(MCodePointAtOrNegative* ins) {
  MDefinition* str = ins->string();
  MDefinition* idx = ins->index();

  MOZ_ASSERT(str->type() == MIRType::String);
  MOZ_ASSERT(idx->type() == MIRType::Int32);

  auto* lir = new (alloc()) LCodePointAtOrNegative(
      useRegister(str), useRegister(idx), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNegativeToNaN(MNegativeToNaN* ins) {
  MOZ_ASSERT(ins->input()->type() == MIRType::Int32);

  auto* lir = new (alloc()) LNegativeToNaN(useRegister(ins->input()));
  defineBox(lir, ins);
}

void LIRGenerator::visitNegativeToUndefined(MNegativeToUndefined* ins) {
  MOZ_ASSERT(ins->input()->type() == MIRType::Int32);

  auto* lir = new (alloc()) LNegativeToUndefined(useRegister(ins->input()));
  defineBox(lir, ins);
}

void LIRGenerator::visitFromCharCode(MFromCharCode* ins) {
  MDefinition* code = ins->code();

  MOZ_ASSERT(code->type() == MIRType::Int32);

  LFromCharCode* lir = new (alloc()) LFromCharCode(useRegister(code));
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitFromCharCodeEmptyIfNegative(
    MFromCharCodeEmptyIfNegative* ins) {
  MDefinition* code = ins->code();

  MOZ_ASSERT(code->type() == MIRType::Int32);

  auto* lir = new (alloc()) LFromCharCodeEmptyIfNegative(useRegister(code));
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitFromCharCodeUndefinedIfNegative(
    MFromCharCodeUndefinedIfNegative* ins) {
  MDefinition* code = ins->code();

  MOZ_ASSERT(code->type() == MIRType::Int32);

  auto* lir = new (alloc()) LFromCharCodeUndefinedIfNegative(useRegister(code));
  defineBox(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitFromCodePoint(MFromCodePoint* ins) {
  MDefinition* codePoint = ins->codePoint();

  MOZ_ASSERT(codePoint->type() == MIRType::Int32);

  LFromCodePoint* lir =
      new (alloc()) LFromCodePoint(useRegister(codePoint), temp(), temp());
  assignSnapshot(lir, ins->bailoutKind());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitStringIncludes(MStringIncludes* ins) {
  auto* string = ins->string();
  MOZ_ASSERT(string->type() == MIRType::String);

  auto* searchStr = ins->searchString();
  MOZ_ASSERT(searchStr->type() == MIRType::String);

  if (searchStr->isConstant()) {
    JSLinearString* linear = &searchStr->toConstant()->toString()->asLinear();
    size_t length = linear->length();
    if (length == 1 || length == 2) {
      LDefinition tempDef = LDefinition::BogusTemp();
      if (length > 1) {
        tempDef = temp();
      }

      auto* lir = new (alloc()) LStringIncludesSIMD(useRegister(string), temp(),
                                                    temp(), tempDef, linear);
      define(lir, ins);
      assignSafepoint(lir, ins);
      return;
    }
  }

  auto* lir = new (alloc()) LStringIncludes(useRegisterAtStart(string),
                                            useRegisterAtStart(searchStr));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitStringIndexOf(MStringIndexOf* ins) {
  auto* string = ins->string();
  MOZ_ASSERT(string->type() == MIRType::String);

  auto* searchStr = ins->searchString();
  MOZ_ASSERT(searchStr->type() == MIRType::String);

  if (searchStr->isConstant()) {
    JSLinearString* linear = &searchStr->toConstant()->toString()->asLinear();
    size_t length = linear->length();
    if (length == 1 || length == 2) {
      LDefinition tempDef = LDefinition::BogusTemp();
      if (length > 1) {
        tempDef = temp();
      }

      auto* lir = new (alloc()) LStringIndexOfSIMD(useRegister(string), temp(),
                                                   temp(), tempDef, linear);
      define(lir, ins);
      assignSafepoint(lir, ins);
      return;
    }
  }

  auto* lir = new (alloc())
      LStringIndexOf(useRegisterAtStart(string), useRegisterAtStart(searchStr));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitStringLastIndexOf(MStringLastIndexOf* ins) {
  auto* string = ins->string();
  MOZ_ASSERT(string->type() == MIRType::String);

  auto* searchStr = ins->searchString();
  MOZ_ASSERT(searchStr->type() == MIRType::String);

  auto* lir = new (alloc()) LStringLastIndexOf(useRegisterAtStart(string),
                                               useRegisterAtStart(searchStr));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitStringStartsWith(MStringStartsWith* ins) {
  auto* string = ins->string();
  MOZ_ASSERT(string->type() == MIRType::String);

  auto* searchStr = ins->searchString();
  MOZ_ASSERT(searchStr->type() == MIRType::String);

  if (searchStr->isConstant()) {
    JSLinearString* linear = &searchStr->toConstant()->toString()->asLinear();

    if (MacroAssembler::canCompareStringCharsInline(linear)) {
      auto* lir = new (alloc())
          LStringStartsWithInline(useRegister(string), temp(), linear);
      define(lir, ins);
      assignSafepoint(lir, ins);
      return;
    }
  }

  auto* lir = new (alloc()) LStringStartsWith(useRegisterAtStart(string),
                                              useRegisterAtStart(searchStr));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitStringEndsWith(MStringEndsWith* ins) {
  auto* string = ins->string();
  MOZ_ASSERT(string->type() == MIRType::String);

  auto* searchStr = ins->searchString();
  MOZ_ASSERT(searchStr->type() == MIRType::String);

  if (searchStr->isConstant()) {
    JSLinearString* linear = &searchStr->toConstant()->toString()->asLinear();

    if (MacroAssembler::canCompareStringCharsInline(linear)) {
      auto* lir = new (alloc())
          LStringEndsWithInline(useRegister(string), temp(), linear);
      define(lir, ins);
      assignSafepoint(lir, ins);
      return;
    }
  }

  auto* lir = new (alloc()) LStringEndsWith(useRegisterAtStart(string),
                                            useRegisterAtStart(searchStr));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitStringConvertCase(MStringConvertCase* ins) {
  MOZ_ASSERT(ins->string()->type() == MIRType::String);

  if (ins->mode() == MStringConvertCase::LowerCase) {
#ifdef JS_CODEGEN_X86
    // Due to lack of registers on x86, we reuse the string register as
    // temporary. As a result we only need four temporary registers and take a
    // bogus temporary as the fifth argument.
    LDefinition temp4 = LDefinition::BogusTemp();
#else
    LDefinition temp4 = temp();
#endif
    auto* lir = new (alloc())
        LStringToLowerCase(useRegister(ins->string()), temp(), temp(), temp(),
                           temp4, tempByteOpRegister());
    define(lir, ins);
    assignSafepoint(lir, ins);
  } else {
    auto* lir =
        new (alloc()) LStringToUpperCase(useRegisterAtStart(ins->string()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
  }
}

void LIRGenerator::visitCharCodeConvertCase(MCharCodeConvertCase* ins) {
  MOZ_ASSERT(ins->code()->type() == MIRType::Int32);

  if (ins->mode() == MCharCodeConvertCase::LowerCase) {
    auto* lir = new (alloc())
        LCharCodeToLowerCase(useRegister(ins->code()), tempByteOpRegister());
    define(lir, ins);
    assignSafepoint(lir, ins);
  } else {
    auto* lir = new (alloc())
        LCharCodeToUpperCase(useRegister(ins->code()), tempByteOpRegister());
    define(lir, ins);
    assignSafepoint(lir, ins);
  }
}

void LIRGenerator::visitStringTrimStartIndex(MStringTrimStartIndex* ins) {
  auto* string = ins->string();
  MOZ_ASSERT(string->type() == MIRType::String);

  auto* lir = new (alloc()) LStringTrimStartIndex(useRegister(string));
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitStringTrimEndIndex(MStringTrimEndIndex* ins) {
  auto* string = ins->string();
  MOZ_ASSERT(string->type() == MIRType::String);

  auto* start = ins->start();
  MOZ_ASSERT(start->type() == MIRType::Int32);

  auto* lir = new (alloc())
      LStringTrimEndIndex(useRegister(string), useRegister(start));
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitStart(MStart* start) {}

void LIRGenerator::visitNop(MNop* nop) {}

void LIRGenerator::visitLimitedTruncate(MLimitedTruncate* nop) {
  redefine(nop, nop->input());
}

void LIRGenerator::visitOsrEntry(MOsrEntry* entry) {
  LOsrEntry* lir = new (alloc()) LOsrEntry(temp());
  defineFixed(lir, entry, LAllocation(AnyRegister(OsrFrameReg)));
}

void LIRGenerator::visitOsrValue(MOsrValue* value) {
  LOsrValue* lir = new (alloc()) LOsrValue(useRegister(value->entry()));
  defineBox(lir, value);
}

void LIRGenerator::visitOsrReturnValue(MOsrReturnValue* value) {
  LOsrReturnValue* lir =
      new (alloc()) LOsrReturnValue(useRegister(value->entry()));
  defineBox(lir, value);
}

void LIRGenerator::visitOsrEnvironmentChain(MOsrEnvironmentChain* object) {
  LOsrEnvironmentChain* lir =
      new (alloc()) LOsrEnvironmentChain(useRegister(object->entry()));
  define(lir, object);
}

void LIRGenerator::visitOsrArgumentsObject(MOsrArgumentsObject* object) {
  LOsrArgumentsObject* lir =
      new (alloc()) LOsrArgumentsObject(useRegister(object->entry()));
  define(lir, object);
}

void LIRGenerator::visitToDouble(MToDouble* convert) {
  MDefinition* opd = convert->input();
  mozilla::DebugOnly<MToFPInstruction::ConversionKind> conversion =
      convert->conversion();

  switch (opd->type()) {
    case MIRType::Value: {
      LValueToDouble* lir = new (alloc()) LValueToDouble(useBox(opd));
      assignSnapshot(lir, convert->bailoutKind());
      define(lir, convert);
      break;
    }

    case MIRType::Null:
      MOZ_ASSERT(conversion == MToFPInstruction::NonStringPrimitives);
      lowerConstantDouble(0, convert);
      break;

    case MIRType::Undefined:
      MOZ_ASSERT(conversion == MToFPInstruction::NonStringPrimitives);
      lowerConstantDouble(GenericNaN(), convert);
      break;

    case MIRType::Boolean:
      MOZ_ASSERT(conversion == MToFPInstruction::NonStringPrimitives);
      [[fallthrough]];

    case MIRType::Int32: {
      LInt32ToDouble* lir =
          new (alloc()) LInt32ToDouble(useRegisterAtStart(opd));
      define(lir, convert);
      break;
    }

    case MIRType::Float32: {
      LFloat32ToDouble* lir =
          new (alloc()) LFloat32ToDouble(useRegisterAtStart(opd));
      define(lir, convert);
      break;
    }

    case MIRType::Double:
      redefine(convert, opd);
      break;

    default:
      // Objects might be effectful. Symbols will throw.
      // Strings are complicated - we don't handle them yet.
      MOZ_CRASH("unexpected type");
  }
}

void LIRGenerator::visitToFloat32(MToFloat32* convert) {
  MDefinition* opd = convert->input();
  mozilla::DebugOnly<MToFloat32::ConversionKind> conversion =
      convert->conversion();

  switch (opd->type()) {
    case MIRType::Value: {
      LValueToFloat32* lir = new (alloc()) LValueToFloat32(useBox(opd));
      assignSnapshot(lir, convert->bailoutKind());
      define(lir, convert);
      break;
    }

    case MIRType::Null:
      MOZ_ASSERT(conversion == MToFPInstruction::NonStringPrimitives);
      lowerConstantFloat32(0, convert);
      break;

    case MIRType::Undefined:
      MOZ_ASSERT(conversion == MToFPInstruction::NonStringPrimitives);
      lowerConstantFloat32(GenericNaN(), convert);
      break;

    case MIRType::Boolean:
      MOZ_ASSERT(conversion == MToFPInstruction::NonStringPrimitives);
      [[fallthrough]];

    case MIRType::Int32: {
      LInt32ToFloat32* lir =
          new (alloc()) LInt32ToFloat32(useRegisterAtStart(opd));
      define(lir, convert);
      break;
    }

    case MIRType::Double: {
      LDoubleToFloat32* lir =
          new (alloc()) LDoubleToFloat32(useRegisterAtStart(opd));
      define(lir, convert);
      break;
    }

    case MIRType::Float32:
      redefine(convert, opd);
      break;

    default:
      // Objects might be effectful. Symbols will throw.
      // Strings are complicated - we don't handle them yet.
      MOZ_CRASH("unexpected type");
  }
}

void LIRGenerator::visitToNumberInt32(MToNumberInt32* convert) {
  MDefinition* opd = convert->input();

  switch (opd->type()) {
    case MIRType::Value: {
      auto* lir = new (alloc()) LValueToInt32(useBox(opd), tempDouble(), temp(),
                                              LValueToInt32::NORMAL);
      assignSnapshot(lir, convert->bailoutKind());
      define(lir, convert);
      if (lir->mode() == LValueToInt32::TRUNCATE) {
        assignSafepoint(lir, convert);
      }
      break;
    }

    case MIRType::Null:
      MOZ_ASSERT(convert->conversion() == IntConversionInputKind::Any);
      define(new (alloc()) LInteger(0), convert);
      break;

    case MIRType::Boolean:
      MOZ_ASSERT(convert->conversion() == IntConversionInputKind::Any ||
                 convert->conversion() ==
                     IntConversionInputKind::NumbersOrBoolsOnly);
      redefine(convert, opd);
      break;

    case MIRType::Int32:
      redefine(convert, opd);
      break;

    case MIRType::Float32: {
      LFloat32ToInt32* lir = new (alloc()) LFloat32ToInt32(useRegister(opd));
      assignSnapshot(lir, convert->bailoutKind());
      define(lir, convert);
      break;
    }

    case MIRType::Double: {
      LDoubleToInt32* lir = new (alloc()) LDoubleToInt32(useRegister(opd));
      assignSnapshot(lir, convert->bailoutKind());
      define(lir, convert);
      break;
    }

    case MIRType::String:
    case MIRType::Symbol:
    case MIRType::BigInt:
    case MIRType::Object:
    case MIRType::Undefined:
      // Objects might be effectful. Symbols and BigInts throw. Undefined
      // coerces to NaN, not int32.
      MOZ_CRASH("ToInt32 invalid input type");

    default:
      MOZ_CRASH("unexpected type");
  }
}

void LIRGenerator::visitBooleanToInt32(MBooleanToInt32* convert) {
  MDefinition* opd = convert->input();
  MOZ_ASSERT(opd->type() == MIRType::Boolean);
  redefine(convert, opd);
}

void LIRGenerator::visitTruncateToInt32(MTruncateToInt32* truncate) {
  MDefinition* opd = truncate->input();

  switch (opd->type()) {
    case MIRType::Value: {
      LValueToInt32* lir = new (alloc()) LValueToInt32(
          useBox(opd), tempDouble(), temp(), LValueToInt32::TRUNCATE);
      assignSnapshot(lir, truncate->bailoutKind());
      define(lir, truncate);
      assignSafepoint(lir, truncate);
      break;
    }

    case MIRType::Null:
    case MIRType::Undefined:
      define(new (alloc()) LInteger(0), truncate);
      break;

    case MIRType::Int32:
    case MIRType::Boolean:
      redefine(truncate, opd);
      break;

    case MIRType::Double:
      // May call into JS::ToInt32() on the slow OOL path.
      gen->setNeedsStaticStackAlignment();
      lowerTruncateDToInt32(truncate);
      break;

    case MIRType::Float32:
      // May call into JS::ToInt32() on the slow OOL path.
      gen->setNeedsStaticStackAlignment();
      lowerTruncateFToInt32(truncate);
      break;

    default:
      // Objects might be effectful. Symbols throw.
      // Strings are complicated - we don't handle them yet.
      MOZ_CRASH("unexpected type");
  }
}

void LIRGenerator::visitInt32ToIntPtr(MInt32ToIntPtr* ins) {
  MDefinition* input = ins->input();
  MOZ_ASSERT(input->type() == MIRType::Int32);
  MOZ_ASSERT(ins->type() == MIRType::IntPtr);

#ifdef JS_64BIT
  // If the result is only used by instructions that expect a bounds-checked
  // index, we must have eliminated or hoisted a bounds check and we can assume
  // the index is non-negative. This lets us generate more efficient code.
  if (ins->canBeNegative()) {
    bool canBeNegative = false;
    for (MUseDefIterator iter(ins); iter; iter++) {
      if (!iter.def()->isSpectreMaskIndex() &&
          !iter.def()->isLoadUnboxedScalar() &&
          !iter.def()->isStoreUnboxedScalar() &&
          !iter.def()->isLoadDataViewElement() &&
          !iter.def()->isStoreDataViewElement()) {
        canBeNegative = true;
        break;
      }
    }
    if (!canBeNegative) {
      ins->setCanNotBeNegative();
    }
  }

  if (ins->canBeNegative()) {
    auto* lir = new (alloc()) LInt32ToIntPtr(useAnyAtStart(input));
    define(lir, ins);
  } else {
    redefine(ins, input);
  }
#else
  // On 32-bit platforms this is a no-op.
  redefine(ins, input);
#endif
}

void LIRGenerator::visitNonNegativeIntPtrToInt32(
    MNonNegativeIntPtrToInt32* ins) {
  MDefinition* input = ins->input();
  MOZ_ASSERT(input->type() == MIRType::IntPtr);
  MOZ_ASSERT(ins->type() == MIRType::Int32);

#ifdef JS_64BIT
  auto* lir =
      new (alloc()) LNonNegativeIntPtrToInt32(useRegisterAtStart(input));
  assignSnapshot(lir, ins->bailoutKind());
  defineReuseInput(lir, ins, 0);
#else
  // On 32-bit platforms this is a no-op.
  redefine(ins, input);
#endif
}

void LIRGenerator::visitWasmExtendU32Index(MWasmExtendU32Index* ins) {
#ifdef JS_64BIT
  // Technically this produces an Int64 register and I guess we could clean that
  // up, but it's a 64-bit only operation, so it doesn't actually matter.

  MDefinition* input = ins->input();
  MOZ_ASSERT(input->type() == MIRType::Int32);
  MOZ_ASSERT(ins->type() == MIRType::Int64);

  // Input reuse is OK even on ARM64 because this node *must* reuse its input in
  // order not to generate any code at all, as is the intent.
  auto* lir = new (alloc()) LWasmExtendU32Index(useRegisterAtStart(input));
  defineReuseInput(lir, ins, 0);
#else
  MOZ_CRASH("64-bit only");
#endif
}

void LIRGenerator::visitWasmWrapU32Index(MWasmWrapU32Index* ins) {
  MDefinition* input = ins->input();
  MOZ_ASSERT(input->type() == MIRType::Int64);
  MOZ_ASSERT(ins->type() == MIRType::Int32);

  // Tricky: On 64-bit, this just returns its input (except on MIPS64 there may
  // be a sign/zero extension).  On 32-bit, it returns the low register of the
  // input, and should generate no code.

  // If this assertion does not hold then using "input" unadorned as an alias
  // for the low register will not work.
#if defined(JS_NUNBOX32)
  static_assert(INT64LOW_INDEX == 0);
#endif

  auto* lir = new (alloc()) LWasmWrapU32Index(useRegisterAtStart(input));
  defineReuseInput(lir, ins, 0);
}

void LIRGenerator::visitIntPtrToDouble(MIntPtrToDouble* ins) {
  MDefinition* input = ins->input();
  MOZ_ASSERT(input->type() == MIRType::IntPtr);
  MOZ_ASSERT(ins->type() == MIRType::Double);

  auto* lir = new (alloc()) LIntPtrToDouble(useRegister(input));
  define(lir, ins);
}

void LIRGenerator::visitAdjustDataViewLength(MAdjustDataViewLength* ins) {
  MDefinition* input = ins->input();
  MOZ_ASSERT(input->type() == MIRType::IntPtr);

  auto* lir = new (alloc()) LAdjustDataViewLength(useRegisterAtStart(input));
  assignSnapshot(lir, ins->bailoutKind());
  defineReuseInput(lir, ins, 0);
}

void LIRGenerator::visitToBigInt(MToBigInt* ins) {
  MDefinition* opd = ins->input();

  switch (opd->type()) {
    case MIRType::Value: {
      auto* lir = new (alloc()) LValueToBigInt(useBox(opd));
      assignSnapshot(lir, ins->bailoutKind());
      define(lir, ins);
      assignSafepoint(lir, ins);
      break;
    }

    case MIRType::BigInt:
      redefine(ins, opd);
      break;

    default:
      MOZ_CRASH("unexpected type");
  }
}

void LIRGenerator::visitToInt64(MToInt64* ins) {
  MDefinition* opd = ins->input();

  switch (opd->type()) {
    case MIRType::Value: {
      auto* lir = new (alloc()) LValueToInt64(useBox(opd), temp());
      assignSnapshot(lir, ins->bailoutKind());
      defineInt64(lir, ins);
      assignSafepoint(lir, ins);
      break;
    }

    case MIRType::Boolean: {
      auto* lir = new (alloc()) LBooleanToInt64(useRegisterAtStart(opd));
      defineInt64(lir, ins);
      break;
    }

    case MIRType::String: {
      auto* lir = new (alloc()) LStringToInt64(useRegister(opd));
      defineInt64(lir, ins);
      assignSafepoint(lir, ins);
      break;
    }

    // An Int64 may be passed here from a BigInt to Int64 conversion.
    case MIRType::Int64: {
      redefine(ins, opd);
      break;
    }

    default:
      // Undefined, Null, Number, and Symbol throw.
      // Objects may be effectful.
      // BigInt operands are eliminated by the type policy.
      MOZ_CRASH("unexpected type");
  }
}

void LIRGenerator::visitTruncateBigIntToInt64(MTruncateBigIntToInt64* ins) {
  MOZ_ASSERT(ins->input()->type() == MIRType::BigInt);
  auto* lir = new (alloc()) LTruncateBigIntToInt64(useRegister(ins->input()));
  defineInt64(lir, ins);
}

void LIRGenerator::visitInt64ToBigInt(MInt64ToBigInt* ins) {
  MOZ_ASSERT(ins->input()->type() == MIRType::Int64);
  auto* lir =
      new (alloc()) LInt64ToBigInt(useInt64Register(ins->input()), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitWasmTruncateToInt32(MWasmTruncateToInt32* ins) {
  MDefinition* input = ins->input();
  switch (input->type()) {
    case MIRType::Double:
    case MIRType::Float32: {
      auto* lir = new (alloc()) LWasmTruncateToInt32(useRegisterAtStart(input));
      define(lir, ins);
      break;
    }
    default:
      MOZ_CRASH("unexpected type in WasmTruncateToInt32");
  }
}

void LIRGenerator::visitWasmBuiltinTruncateToInt32(
    MWasmBuiltinTruncateToInt32* truncate) {
  mozilla::DebugOnly<MDefinition*> opd = truncate->input();
  MOZ_ASSERT(opd->type() == MIRType::Double || opd->type() == MIRType::Float32);

  // May call into JS::ToInt32() on the slow OOL path.
  gen->setNeedsStaticStackAlignment();
  lowerWasmBuiltinTruncateToInt32(truncate);
}

void LIRGenerator::visitWasmAnyRefFromJSValue(MWasmAnyRefFromJSValue* ins) {
  LWasmAnyRefFromJSValue* lir =
      new (alloc()) LWasmAnyRefFromJSValue(useBox(ins->input()), tempDouble());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitWasmAnyRefFromJSObject(MWasmAnyRefFromJSObject* ins) {
  LWasmAnyRefFromJSObject* lir =
      new (alloc()) LWasmAnyRefFromJSObject(useRegisterAtStart(ins->input()));
  define(lir, ins);
}

void LIRGenerator::visitWasmAnyRefFromJSString(MWasmAnyRefFromJSString* ins) {
  LWasmAnyRefFromJSString* lir =
      new (alloc()) LWasmAnyRefFromJSString(useRegisterAtStart(ins->input()));
  define(lir, ins);
}

void LIRGenerator::visitWasmNewI31Ref(MWasmNewI31Ref* ins) {
  // If it's a constant, it will be put directly into the register.
  LWasmNewI31Ref* lir =
      new (alloc()) LWasmNewI31Ref(useRegisterOrConstant(ins->input()));
  define(lir, ins);
}

void LIRGenerator::visitWasmI31RefGet(MWasmI31RefGet* ins) {
  LWasmI31RefGet* lir = new (alloc()) LWasmI31RefGet(useRegister(ins->input()));
  define(lir, ins);
}

void LIRGenerator::visitWrapInt64ToInt32(MWrapInt64ToInt32* ins) {
  define(new (alloc()) LWrapInt64ToInt32(useInt64AtStart(ins->input())), ins);
}

void LIRGenerator::visitToString(MToString* ins) {
  MDefinition* opd = ins->input();

  switch (opd->type()) {
    case MIRType::Null: {
      const JSAtomState& names = gen->runtime->names();
      LPointer* lir = new (alloc()) LPointer(names.null);
      define(lir, ins);
      break;
    }

    case MIRType::Undefined: {
      const JSAtomState& names = gen->runtime->names();
      LPointer* lir = new (alloc()) LPointer(names.undefined);
      define(lir, ins);
      break;
    }

    case MIRType::Boolean: {
      LBooleanToString* lir = new (alloc()) LBooleanToString(useRegister(opd));
      define(lir, ins);
      break;
    }

    case MIRType::Double: {
      LDoubleToString* lir =
          new (alloc()) LDoubleToString(useRegister(opd), temp());

      define(lir, ins);
      assignSafepoint(lir, ins);
      break;
    }

    case MIRType::Int32: {
      LIntToString* lir = new (alloc()) LIntToString(useRegister(opd));

      define(lir, ins);
      assignSafepoint(lir, ins);
      break;
    }

    case MIRType::String:
      redefine(ins, ins->input());
      break;

    case MIRType::Value: {
      LValueToString* lir =
          new (alloc()) LValueToString(useBox(opd), tempToUnbox());
      if (ins->needsSnapshot()) {
        assignSnapshot(lir, ins->bailoutKind());
      }
      define(lir, ins);
      assignSafepoint(lir, ins);
      break;
    }

    default:
      // Float32, symbols, bigint, and objects are not supported.
      MOZ_CRASH("unexpected type");
  }
}

void LIRGenerator::visitRegExp(MRegExp* ins) {
  LRegExp* lir = new (alloc()) LRegExp(temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitRegExpMatcher(MRegExpMatcher* ins) {
  MOZ_ASSERT(ins->regexp()->type() == MIRType::Object);
  MOZ_ASSERT(ins->string()->type() == MIRType::String);
  MOZ_ASSERT(ins->lastIndex()->type() == MIRType::Int32);

  LRegExpMatcher* lir = new (alloc()) LRegExpMatcher(
      useFixedAtStart(ins->regexp(), RegExpMatcherRegExpReg),
      useFixedAtStart(ins->string(), RegExpMatcherStringReg),
      useFixedAtStart(ins->lastIndex(), RegExpMatcherLastIndexReg));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitRegExpSearcher(MRegExpSearcher* ins) {
  MOZ_ASSERT(ins->regexp()->type() == MIRType::Object);
  MOZ_ASSERT(ins->string()->type() == MIRType::String);
  MOZ_ASSERT(ins->lastIndex()->type() == MIRType::Int32);

  LRegExpSearcher* lir = new (alloc()) LRegExpSearcher(
      useFixedAtStart(ins->regexp(), RegExpSearcherRegExpReg),
      useFixedAtStart(ins->string(), RegExpSearcherStringReg),
      useFixedAtStart(ins->lastIndex(), RegExpSearcherLastIndexReg));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitRegExpSearcherLastLimit(MRegExpSearcherLastLimit* ins) {
  auto* lir = new (alloc()) LRegExpSearcherLastLimit(temp());
  define(lir, ins);
}

void LIRGenerator::visitRegExpExecMatch(MRegExpExecMatch* ins) {
  MOZ_ASSERT(ins->regexp()->type() == MIRType::Object);
  MOZ_ASSERT(ins->string()->type() == MIRType::String);

  auto* lir = new (alloc())
      LRegExpExecMatch(useFixedAtStart(ins->regexp(), RegExpMatcherRegExpReg),
                       useFixedAtStart(ins->string(), RegExpMatcherStringReg));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitRegExpExecTest(MRegExpExecTest* ins) {
  MOZ_ASSERT(ins->regexp()->type() == MIRType::Object);
  MOZ_ASSERT(ins->string()->type() == MIRType::String);

  auto* lir = new (alloc())
      LRegExpExecTest(useFixedAtStart(ins->regexp(), RegExpExecTestRegExpReg),
                      useFixedAtStart(ins->string(), RegExpExecTestStringReg));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitRegExpHasCaptureGroups(MRegExpHasCaptureGroups* ins) {
  MOZ_ASSERT(ins->regexp()->type() == MIRType::Object);
  MOZ_ASSERT(ins->input()->type() == MIRType::String);
  MOZ_ASSERT(ins->type() == MIRType::Boolean);

  auto* lir = new (alloc()) LRegExpHasCaptureGroups(useRegister(ins->regexp()),
                                                    useRegister(ins->input()));
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitRegExpPrototypeOptimizable(
    MRegExpPrototypeOptimizable* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::Boolean);
  LRegExpPrototypeOptimizable* lir = new (alloc())
      LRegExpPrototypeOptimizable(useRegister(ins->object()), temp());
  define(lir, ins);
}

void LIRGenerator::visitRegExpInstanceOptimizable(
    MRegExpInstanceOptimizable* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->proto()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::Boolean);
  LRegExpInstanceOptimizable* lir = new (alloc()) LRegExpInstanceOptimizable(
      useRegister(ins->object()), useRegister(ins->proto()), temp());
  define(lir, ins);
}

void LIRGenerator::visitGetFirstDollarIndex(MGetFirstDollarIndex* ins) {
  MOZ_ASSERT(ins->str()->type() == MIRType::String);
  MOZ_ASSERT(ins->type() == MIRType::Int32);
  LGetFirstDollarIndex* lir = new (alloc())
      LGetFirstDollarIndex(useRegister(ins->str()), temp(), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitStringReplace(MStringReplace* ins) {
  MOZ_ASSERT(ins->pattern()->type() == MIRType::String);
  MOZ_ASSERT(ins->string()->type() == MIRType::String);
  MOZ_ASSERT(ins->replacement()->type() == MIRType::String);

  LStringReplace* lir = new (alloc())
      LStringReplace(useRegisterOrConstantAtStart(ins->string()),
                     useRegisterAtStart(ins->pattern()),
                     useRegisterOrConstantAtStart(ins->replacement()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitBinaryCache(MBinaryCache* ins) {
  MDefinition* lhs = ins->getOperand(0);
  MDefinition* rhs = ins->getOperand(1);

  MOZ_ASSERT(ins->type() == MIRType::Value || ins->type() == MIRType::Boolean);
  LInstruction* lir;
  if (ins->type() == MIRType::Value) {
    LBinaryValueCache* valueLir = new (alloc()) LBinaryValueCache(
        useBox(lhs), useBox(rhs), tempFixed(FloatReg0), tempFixed(FloatReg1));
    defineBox(valueLir, ins);
    lir = valueLir;
  } else {
    MOZ_ASSERT(ins->type() == MIRType::Boolean);
    LBinaryBoolCache* boolLir = new (alloc()) LBinaryBoolCache(
        useBox(lhs), useBox(rhs), tempFixed(FloatReg0), tempFixed(FloatReg1));
    define(boolLir, ins);
    lir = boolLir;
  }
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitUnaryCache(MUnaryCache* ins) {
  MDefinition* input = ins->getOperand(0);
  MOZ_ASSERT(ins->type() == MIRType::Value);

  LUnaryCache* lir = new (alloc()) LUnaryCache(useBox(input));
  defineBox(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitModuleMetadata(MModuleMetadata* ins) {
  LModuleMetadata* lir = new (alloc()) LModuleMetadata();
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitDynamicImport(MDynamicImport* ins) {
  LDynamicImport* lir = new (alloc()) LDynamicImport(
      useBoxAtStart(ins->specifier()), useBoxAtStart(ins->options()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitLambda(MLambda* ins) {
  MOZ_ASSERT(ins->environmentChain()->type() == MIRType::Object);

  auto* lir =
      new (alloc()) LLambda(useRegister(ins->environmentChain()), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitFunctionWithProto(MFunctionWithProto* ins) {
  MOZ_ASSERT(ins->environmentChain()->type() == MIRType::Object);
  MOZ_ASSERT(ins->prototype()->type() == MIRType::Object);

  auto* lir = new (alloc())
      LFunctionWithProto(useRegisterAtStart(ins->environmentChain()),
                         useRegisterAtStart(ins->prototype()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitSetFunName(MSetFunName* ins) {
  MOZ_ASSERT(ins->fun()->type() == MIRType::Object);
  MOZ_ASSERT(ins->name()->type() == MIRType::Value);

  LSetFunName* lir = new (alloc())
      LSetFunName(useRegisterAtStart(ins->fun()), useBoxAtStart(ins->name()));
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNewLexicalEnvironmentObject(
    MNewLexicalEnvironmentObject* ins) {
  auto* lir = new (alloc()) LNewLexicalEnvironmentObject(temp());

  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNewClassBodyEnvironmentObject(
    MNewClassBodyEnvironmentObject* ins) {
  auto* lir = new (alloc()) LNewClassBodyEnvironmentObject(temp());

  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNewVarEnvironmentObject(MNewVarEnvironmentObject* ins) {
  auto* lir = new (alloc()) LNewVarEnvironmentObject(temp());

  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitKeepAliveObject(MKeepAliveObject* ins) {
  MDefinition* obj = ins->object();
  MOZ_ASSERT(obj->type() == MIRType::Object);

  add(new (alloc()) LKeepAliveObject(useKeepalive(obj)), ins);
}

void LIRGenerator::visitDebugEnterGCUnsafeRegion(
    MDebugEnterGCUnsafeRegion* ins) {
  add(new (alloc()) LDebugEnterGCUnsafeRegion(temp()), ins);
}

void LIRGenerator::visitDebugLeaveGCUnsafeRegion(
    MDebugLeaveGCUnsafeRegion* ins) {
  add(new (alloc()) LDebugLeaveGCUnsafeRegion(temp()), ins);
}

void LIRGenerator::visitSlots(MSlots* ins) {
  define(new (alloc()) LSlots(useRegisterAtStart(ins->object())), ins);
}

void LIRGenerator::visitElements(MElements* ins) {
  define(new (alloc()) LElements(useRegisterAtStart(ins->object())), ins);
}

void LIRGenerator::visitLoadDynamicSlot(MLoadDynamicSlot* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Value);
  if (ins->usedAsPropertyKey()) {
    auto* lir = new (alloc())
        LLoadDynamicSlotAndAtomize(useRegister(ins->slots()), temp());
    defineBox(lir, ins);
    assignSafepoint(lir, ins);
  } else {
    defineBox(new (alloc()) LLoadDynamicSlotV(useRegisterAtStart(ins->slots())),
              ins);
  }
}

void LIRGenerator::visitFunctionEnvironment(MFunctionEnvironment* ins) {
  define(new (alloc())
             LFunctionEnvironment(useRegisterAtStart(ins->function())),
         ins);
}

void LIRGenerator::visitHomeObject(MHomeObject* ins) {
  define(new (alloc()) LHomeObject(useRegisterAtStart(ins->function())), ins);
}

void LIRGenerator::visitHomeObjectSuperBase(MHomeObjectSuperBase* ins) {
  MOZ_ASSERT(ins->homeObject()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::Value);

  auto lir =
      new (alloc()) LHomeObjectSuperBase(useRegisterAtStart(ins->homeObject()));
  defineBox(lir, ins);
}

void LIRGenerator::visitInterruptCheck(MInterruptCheck* ins) {
  LInstruction* lir = new (alloc()) LInterruptCheck();
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitWasmInterruptCheck(MWasmInterruptCheck* ins) {
  auto* lir =
      new (alloc()) LWasmInterruptCheck(useRegisterAtStart(ins->instance()));
  add(lir, ins);
  assignWasmSafepoint(lir);
}

void LIRGenerator::visitWasmTrap(MWasmTrap* ins) {
  add(new (alloc()) LWasmTrap, ins);
}

void LIRGenerator::visitWasmTrapIfNull(MWasmTrapIfNull* ins) {
  auto* lir = new (alloc()) LWasmTrapIfNull(useRegister(ins->ref()));
  add(lir, ins);
}

void LIRGenerator::visitWasmReinterpret(MWasmReinterpret* ins) {
  if (ins->type() == MIRType::Int64) {
    defineInt64(new (alloc())
                    LWasmReinterpretToI64(useRegisterAtStart(ins->input())),
                ins);
  } else if (ins->input()->type() == MIRType::Int64) {
    define(new (alloc())
               LWasmReinterpretFromI64(useInt64RegisterAtStart(ins->input())),
           ins);
  } else {
    define(new (alloc()) LWasmReinterpret(useRegisterAtStart(ins->input())),
           ins);
  }
}

void LIRGenerator::visitStoreDynamicSlot(MStoreDynamicSlot* ins) {
  LInstruction* lir;

  switch (ins->value()->type()) {
    case MIRType::Value:
      lir = new (alloc())
          LStoreDynamicSlotV(useRegister(ins->slots()), useBox(ins->value()));
      add(lir, ins);
      break;

    case MIRType::Double:
      add(new (alloc()) LStoreDynamicSlotT(useRegister(ins->slots()),
                                           useRegister(ins->value())),
          ins);
      break;

    case MIRType::Float32:
      MOZ_CRASH("Float32 shouldn't be stored in a slot.");

    default:
      add(new (alloc()) LStoreDynamicSlotT(useRegister(ins->slots()),
                                           useRegisterOrConstant(ins->value())),
          ins);
      break;
  }
}

// Returns true iff |def| is a constant that's either not a GC thing or is not
// allocated in the nursery.
static bool IsNonNurseryConstant(MDefinition* def) {
  if (!def->isConstant()) {
    return false;
  }
  Value v = def->toConstant()->toJSValue();
  return !v.isGCThing() || !IsInsideNursery(v.toGCThing());
}

void LIRGenerator::visitPostWriteBarrier(MPostWriteBarrier* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  // LPostWriteBarrier assumes that if it has a constant object then that
  // object is tenured, and does not need to be tested for being in the
  // nursery. Ensure that assumption holds by lowering constant nursery
  // objects to a register.
  bool useConstantObject = IsNonNurseryConstant(ins->object());

  switch (ins->value()->type()) {
    case MIRType::Object: {
      LDefinition tmp =
          needTempForPostBarrier() ? temp() : LDefinition::BogusTemp();
      LPostWriteBarrierO* lir = new (alloc())
          LPostWriteBarrierO(useConstantObject ? useOrConstant(ins->object())
                                               : useRegister(ins->object()),
                             useRegister(ins->value()), tmp);
      add(lir, ins);
      assignSafepoint(lir, ins);
      break;
    }
    case MIRType::String: {
      LDefinition tmp =
          needTempForPostBarrier() ? temp() : LDefinition::BogusTemp();
      LPostWriteBarrierS* lir = new (alloc())
          LPostWriteBarrierS(useConstantObject ? useOrConstant(ins->object())
                                               : useRegister(ins->object()),
                             useRegister(ins->value()), tmp);
      add(lir, ins);
      assignSafepoint(lir, ins);
      break;
    }
    case MIRType::BigInt: {
      LDefinition tmp =
          needTempForPostBarrier() ? temp() : LDefinition::BogusTemp();
      auto* lir = new (alloc())
          LPostWriteBarrierBI(useConstantObject ? useOrConstant(ins->object())
                                                : useRegister(ins->object()),
                              useRegister(ins->value()), tmp);
      add(lir, ins);
      assignSafepoint(lir, ins);
      break;
    }
    case MIRType::Value: {
      LDefinition tmp =
          needTempForPostBarrier() ? temp() : LDefinition::BogusTemp();
      LPostWriteBarrierV* lir = new (alloc())
          LPostWriteBarrierV(useConstantObject ? useOrConstant(ins->object())
                                               : useRegister(ins->object()),
                             useBox(ins->value()), tmp);
      add(lir, ins);
      assignSafepoint(lir, ins);
      break;
    }
    default:
      // Currently, only objects and strings can be in the nursery. Other
      // instruction types cannot hold nursery pointers.
      break;
  }
}

void LIRGenerator::visitPostWriteElementBarrier(MPostWriteElementBarrier* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

  // LPostWriteElementBarrier assumes that if it has a constant object then that
  // object is tenured, and does not need to be tested for being in the
  // nursery. Ensure that assumption holds by lowering constant nursery
  // objects to a register.
  bool useConstantObject =
      ins->object()->isConstant() &&
      !IsInsideNursery(&ins->object()->toConstant()->toObject());

  switch (ins->value()->type()) {
    case MIRType::Object: {
      LDefinition tmp =
          needTempForPostBarrier() ? temp() : LDefinition::BogusTemp();
      LPostWriteElementBarrierO* lir = new (alloc()) LPostWriteElementBarrierO(
          useConstantObject ? useOrConstant(ins->object())
                            : useRegister(ins->object()),
          useRegister(ins->value()), useRegister(ins->index()), tmp);
      add(lir, ins);
      assignSafepoint(lir, ins);
      break;
    }
    case MIRType::String: {
      LDefinition tmp =
          needTempForPostBarrier() ? temp() : LDefinition::BogusTemp();
      LPostWriteElementBarrierS* lir = new (alloc()) LPostWriteElementBarrierS(
          useConstantObject ? useOrConstant(ins->object())
                            : useRegister(ins->object()),
          useRegister(ins->value()), useRegister(ins->index()), tmp);
      add(lir, ins);
      assignSafepoint(lir, ins);
      break;
    }
    case MIRType::BigInt: {
      LDefinition tmp =
          needTempForPostBarrier() ? temp() : LDefinition::BogusTemp();
      auto* lir = new (alloc()) LPostWriteElementBarrierBI(
          useConstantObject ? useOrConstant(ins->object())
                            : useRegister(ins->object()),
          useRegister(ins->value()), useRegister(ins->index()), tmp);
      add(lir, ins);
      assignSafepoint(lir, ins);
      break;
    }
    case MIRType::Value: {
      LDefinition tmp =
          needTempForPostBarrier() ? temp() : LDefinition::BogusTemp();
      LPostWriteElementBarrierV* lir = new (alloc()) LPostWriteElementBarrierV(
          useConstantObject ? useOrConstant(ins->object())
                            : useRegister(ins->object()),
          useRegister(ins->index()), useBox(ins->value()), tmp);
      add(lir, ins);
      assignSafepoint(lir, ins);
      break;
    }
    default:
      // Currently, only objects, strings, and bigints can be in the nursery.
      // Other instruction types cannot hold nursery pointers.
      break;
  }
}

void LIRGenerator::visitAssertCanElidePostWriteBarrier(
    MAssertCanElidePostWriteBarrier* ins) {
  auto* lir = new (alloc()) LAssertCanElidePostWriteBarrier(
      useRegister(ins->object()), useBox(ins->value()), temp());
  add(lir, ins);
}

void LIRGenerator::visitArrayLength(MArrayLength* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  auto* lir = new (alloc()) LArrayLength(useRegisterAtStart(ins->elements()));
  assignSnapshot(lir, ins->bailoutKind());
  define(lir, ins);
}

void LIRGenerator::visitSetArrayLength(MSetArrayLength* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

  MOZ_ASSERT(ins->index()->isConstant());
  add(new (alloc()) LSetArrayLength(useRegister(ins->elements()),
                                    useRegisterOrConstant(ins->index())),
      ins);
}

void LIRGenerator::visitFunctionLength(MFunctionLength* ins) {
  MOZ_ASSERT(ins->function()->type() == MIRType::Object);

  auto* lir = new (alloc()) LFunctionLength(useRegister(ins->function()));
  assignSnapshot(lir, ins->bailoutKind());
  define(lir, ins);
}

void LIRGenerator::visitFunctionName(MFunctionName* ins) {
  MOZ_ASSERT(ins->function()->type() == MIRType::Object);

  auto* lir = new (alloc()) LFunctionName(useRegister(ins->function()));
  assignSnapshot(lir, ins->bailoutKind());
  define(lir, ins);
}

void LIRGenerator::visitGetNextEntryForIterator(MGetNextEntryForIterator* ins) {
  MOZ_ASSERT(ins->iter()->type() == MIRType::Object);
  MOZ_ASSERT(ins->result()->type() == MIRType::Object);
  auto lir = new (alloc()) LGetNextEntryForIterator(useRegister(ins->iter()),
                                                    useRegister(ins->result()),
                                                    temp(), temp(), temp());
  define(lir, ins);
}

static auto SynchronizeLoad(MemoryBarrierRequirement requiresBarrier) {
  if (requiresBarrier == MemoryBarrierRequirement::Required) {
    return Synchronization::Load();
  }
  return Synchronization::None();
}

static auto SynchronizeStore(MemoryBarrierRequirement requiresBarrier) {
  if (requiresBarrier == MemoryBarrierRequirement::Required) {
    return Synchronization::Store();
  }
  return Synchronization::None();
}

void LIRGenerator::visitArrayBufferByteLength(MArrayBufferByteLength* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::IntPtr);

  auto* lir =
      new (alloc()) LArrayBufferByteLength(useRegisterAtStart(ins->object()));
  define(lir, ins);
}

void LIRGenerator::visitArrayBufferViewLength(MArrayBufferViewLength* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::IntPtr);

  auto* lir =
      new (alloc()) LArrayBufferViewLength(useRegisterAtStart(ins->object()));
  define(lir, ins);
}

void LIRGenerator::visitArrayBufferViewByteOffset(
    MArrayBufferViewByteOffset* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::IntPtr);

  auto* lir = new (alloc())
      LArrayBufferViewByteOffset(useRegisterAtStart(ins->object()));
  define(lir, ins);
}

void LIRGenerator::visitArrayBufferViewElements(MArrayBufferViewElements* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Elements);
  define(new (alloc())
             LArrayBufferViewElements(useRegisterAtStart(ins->object())),
         ins);
}

void LIRGenerator::visitTypedArrayElementSize(MTypedArrayElementSize* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  define(new (alloc())
             LTypedArrayElementSize(useRegisterAtStart(ins->object())),
         ins);
}

void LIRGenerator::visitResizableTypedArrayLength(
    MResizableTypedArrayLength* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::IntPtr);

  auto sync = SynchronizeLoad(ins->requiresMemoryBarrier());
  auto* lir = new (alloc())
      LResizableTypedArrayLength(useRegister(ins->object()), temp(), sync);
  define(lir, ins);
}

void LIRGenerator::visitResizableTypedArrayByteOffsetMaybeOutOfBounds(
    MResizableTypedArrayByteOffsetMaybeOutOfBounds* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::IntPtr);

  auto* lir = new (alloc()) LResizableTypedArrayByteOffsetMaybeOutOfBounds(
      useRegister(ins->object()), temp());
  define(lir, ins);
}

void LIRGenerator::visitResizableDataViewByteLength(
    MResizableDataViewByteLength* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::IntPtr);

  auto sync = SynchronizeLoad(ins->requiresMemoryBarrier());
  auto* lir = new (alloc())
      LResizableDataViewByteLength(useRegister(ins->object()), temp(), sync);
  define(lir, ins);
}

void LIRGenerator::visitGrowableSharedArrayBufferByteLength(
    MGrowableSharedArrayBufferByteLength* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::IntPtr);

  auto* lir = new (alloc())
      LGrowableSharedArrayBufferByteLength(useRegisterAtStart(ins->object()));
  define(lir, ins);
}

void LIRGenerator::visitGuardResizableArrayBufferViewInBounds(
    MGuardResizableArrayBufferViewInBounds* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  auto* lir = new (alloc()) LGuardResizableArrayBufferViewInBounds(
      useRegister(ins->object()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->object());
}

void LIRGenerator::visitGuardResizableArrayBufferViewInBoundsOrDetached(
    MGuardResizableArrayBufferViewInBoundsOrDetached* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  auto* lir = new (alloc()) LGuardResizableArrayBufferViewInBoundsOrDetached(
      useRegister(ins->object()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->object());
}

void LIRGenerator::visitGuardHasAttachedArrayBuffer(
    MGuardHasAttachedArrayBuffer* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  auto* lir = new (alloc())
      LGuardHasAttachedArrayBuffer(useRegister(ins->object()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->object());
}

void LIRGenerator::visitGuardNumberToIntPtrIndex(
    MGuardNumberToIntPtrIndex* ins) {
  MDefinition* input = ins->input();
  MOZ_ASSERT(input->type() == MIRType::Double);

  auto* lir = new (alloc()) LGuardNumberToIntPtrIndex(useRegister(input));
  if (!ins->supportOOB()) {
    assignSnapshot(lir, ins->bailoutKind());
  }
  define(lir, ins);
}

void LIRGenerator::visitInitializedLength(MInitializedLength* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  define(new (alloc()) LInitializedLength(useRegisterAtStart(ins->elements())),
         ins);
}

void LIRGenerator::visitSetInitializedLength(MSetInitializedLength* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

  MOZ_ASSERT(ins->index()->isConstant());
  add(new (alloc()) LSetInitializedLength(useRegister(ins->elements()),
                                          useRegisterOrConstant(ins->index())),
      ins);
}

void LIRGenerator::visitNot(MNot* ins) {
  MDefinition* op = ins->input();

  // String is converted to length of string in the type analysis phase (see
  // TestPolicy).
  MOZ_ASSERT(op->type() != MIRType::String);

  // - boolean: x xor 1
  // - int32: LCompare(x, 0)
  // - double: LCompare(x, 0)
  // - null or undefined: true
  // - symbol: false
  // - bigint: LNotBI(x)
  // - object: false if it never emulates undefined, else LNotO(x)
  switch (op->type()) {
    case MIRType::Boolean: {
      MConstant* cons = MConstant::New(alloc(), Int32Value(1));
      ins->block()->insertBefore(ins, cons);
      lowerForALU(new (alloc()) LBitOpI(JSOp::BitXor), ins, op, cons);
      break;
    }
    case MIRType::Int32:
      define(new (alloc()) LNotI(useRegisterAtStart(op)), ins);
      break;
    case MIRType::Int64:
      define(new (alloc()) LNotI64(useInt64RegisterAtStart(op)), ins);
      break;
    case MIRType::Double:
      define(new (alloc()) LNotD(useRegister(op)), ins);
      break;
    case MIRType::Float32:
      define(new (alloc()) LNotF(useRegister(op)), ins);
      break;
    case MIRType::Undefined:
    case MIRType::Null:
      define(new (alloc()) LInteger(1), ins);
      break;
    case MIRType::Symbol:
      define(new (alloc()) LInteger(0), ins);
      break;
    case MIRType::BigInt:
      define(new (alloc()) LNotBI(useRegisterAtStart(op)), ins);
      break;
    case MIRType::Object:
      define(new (alloc()) LNotO(useRegister(op)), ins);
      break;
    case MIRType::Value: {
      auto* lir = new (alloc()) LNotV(useBox(op), tempDouble(), tempToUnbox());
      define(lir, ins);
      break;
    }

    default:
      MOZ_CRASH("Unexpected MIRType.");
  }
}

void LIRGenerator::visitBoundsCheck(MBoundsCheck* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Int32 || ins->type() == MIRType::IntPtr);
  MOZ_ASSERT(ins->index()->type() == ins->type());
  MOZ_ASSERT(ins->length()->type() == ins->type());

  if (!ins->fallible()) {
    return;
  }

  LInstruction* check;
  if (ins->minimum() || ins->maximum()) {
    check = new (alloc())
        LBoundsCheckRange(useRegisterOrInt32Constant(ins->index()),
                          useAny(ins->length()), temp());
  } else {
    check = new (alloc()) LBoundsCheck(useRegisterOrInt32Constant(ins->index()),
                                       useAnyOrInt32Constant(ins->length()));
  }
  assignSnapshot(check, ins->bailoutKind());
  add(check, ins);
}

void LIRGenerator::visitSpectreMaskIndex(MSpectreMaskIndex* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Int32 || ins->type() == MIRType::IntPtr);
  MOZ_ASSERT(ins->index()->type() == ins->type());
  MOZ_ASSERT(ins->length()->type() == ins->type());

  auto* lir = new (alloc())
      LSpectreMaskIndex(useRegister(ins->index()), useAny(ins->length()));
  define(lir, ins);
}

void LIRGenerator::visitBoundsCheckLower(MBoundsCheckLower* ins) {
  MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

  if (!ins->fallible()) {
    return;
  }

  LInstruction* check =
      new (alloc()) LBoundsCheckLower(useRegister(ins->index()));
  assignSnapshot(check, ins->bailoutKind());
  add(check, ins);
}

void LIRGenerator::visitInArray(MInArray* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::Int32);
  MOZ_ASSERT(ins->initLength()->type() == MIRType::Int32);
  MOZ_ASSERT(ins->type() == MIRType::Boolean);

  auto* lir = new (alloc()) LInArray(useRegister(ins->elements()),
                                     useRegisterOrConstant(ins->index()),
                                     useRegister(ins->initLength()));
  if (ins->needsNegativeIntCheck()) {
    assignSnapshot(lir, ins->bailoutKind());
  }
  define(lir, ins);
}

void LIRGenerator::visitGuardElementNotHole(MGuardElementNotHole* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

  auto* guard = new (alloc())
      LGuardElementNotHole(useRegisterAtStart(ins->elements()),
                           useRegisterOrConstantAtStart(ins->index()));
  assignSnapshot(guard, ins->bailoutKind());
  add(guard, ins);
}

void LIRGenerator::visitLoadElement(MLoadElement* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::Int32);
  MOZ_ASSERT(ins->type() == MIRType::Value);

  auto* lir = new (alloc()) LLoadElementV(useRegister(ins->elements()),
                                          useRegisterOrConstant(ins->index()));
  assignSnapshot(lir, ins->bailoutKind());
  defineBox(lir, ins);
}

void LIRGenerator::visitLoadElementHole(MLoadElementHole* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::Int32);
  MOZ_ASSERT(ins->initLength()->type() == MIRType::Int32);
  MOZ_ASSERT(ins->type() == MIRType::Value);

  LLoadElementHole* lir = new (alloc())
      LLoadElementHole(useRegister(ins->elements()), useRegister(ins->index()),
                       useRegister(ins->initLength()));
  if (ins->needsNegativeIntCheck()) {
    assignSnapshot(lir, ins->bailoutKind());
  }
  defineBox(lir, ins);
}

void LIRGenerator::visitStoreElement(MStoreElement* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

  const LUse elements = useRegister(ins->elements());
  const LAllocation index = useRegisterOrConstant(ins->index());

  switch (ins->value()->type()) {
    case MIRType::Value: {
      LInstruction* lir =
          new (alloc()) LStoreElementV(elements, index, useBox(ins->value()));
      if (ins->fallible()) {
        assignSnapshot(lir, ins->bailoutKind());
      }
      add(lir, ins);
      break;
    }

    default: {
      const LAllocation value = useRegisterOrNonDoubleConstant(ins->value());
      LInstruction* lir = new (alloc()) LStoreElementT(elements, index, value);
      if (ins->fallible()) {
        assignSnapshot(lir, ins->bailoutKind());
      }
      add(lir, ins);
      break;
    }
  }
}

void LIRGenerator::visitStoreHoleValueElement(MStoreHoleValueElement* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

  auto* lir = new (alloc()) LStoreHoleValueElement(useRegister(ins->elements()),
                                                   useRegister(ins->index()));
  add(lir, ins);
}

static bool BoundsCheckNeedsSpectreTemp() {
  // On x86, spectreBoundsCheck32 can emit better code if it has a scratch
  // register and index masking is enabled.
#ifdef JS_CODEGEN_X86
  return JitOptions.spectreIndexMasking;
#else
  return false;
#endif
}

void LIRGenerator::visitStoreElementHole(MStoreElementHole* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

  const LUse object = useRegister(ins->object());
  const LUse elements = useRegister(ins->elements());
  const LAllocation index = useRegister(ins->index());

  LInstruction* lir;
  switch (ins->value()->type()) {
    case MIRType::Value:
      lir = new (alloc()) LStoreElementHoleV(object, elements, index,
                                             useBox(ins->value()), temp());
      break;

    default: {
      const LAllocation value = useRegisterOrNonDoubleConstant(ins->value());
      lir = new (alloc())
          LStoreElementHoleT(object, elements, index, value, temp());
      break;
    }
  }

  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitEffectiveAddress(MEffectiveAddress* ins) {
  define(new (alloc()) LEffectiveAddress(useRegister(ins->base()),
                                         useRegister(ins->index())),
         ins);
}

void LIRGenerator::visitArrayPopShift(MArrayPopShift* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Value);

  auto* lir =
      new (alloc()) LArrayPopShift(useRegister(ins->object()), temp(), temp());
  assignSnapshot(lir, ins->bailoutKind());
  defineBox(lir, ins);

  if (ins->mode() == MArrayPopShift::Shift) {
    assignSafepoint(lir, ins);
  }
}

void LIRGenerator::visitArrayPush(MArrayPush* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Int32);
  MOZ_ASSERT(ins->value()->type() == MIRType::Value);

  LUse object = useRegister(ins->object());

  LDefinition spectreTemp =
      BoundsCheckNeedsSpectreTemp() ? temp() : LDefinition::BogusTemp();

  auto* lir = new (alloc())
      LArrayPush(object, useBox(ins->value()), temp(), spectreTemp);
  // We will bailout before pushing if the length would overflow INT32_MAX.
  assignSnapshot(lir, ins->bailoutKind());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitArraySlice(MArraySlice* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Object);
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->begin()->type() == MIRType::Int32);
  MOZ_ASSERT(ins->end()->type() == MIRType::Int32);

  LArraySlice* lir = new (alloc()) LArraySlice(
      useRegisterAtStart(ins->object()), useRegisterAtStart(ins->begin()),
      useRegisterAtStart(ins->end()), tempFixed(CallTempReg0),
      tempFixed(CallTempReg1));
  assignSnapshot(lir, ins->bailoutKind());
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitArgumentsSlice(MArgumentsSlice* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Object);
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->begin()->type() == MIRType::Int32);
  MOZ_ASSERT(ins->end()->type() == MIRType::Int32);

  auto* lir = new (alloc()) LArgumentsSlice(
      useRegisterAtStart(ins->object()), useRegisterAtStart(ins->begin()),
      useRegisterAtStart(ins->end()), tempFixed(CallTempReg0),
      tempFixed(CallTempReg1));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitFrameArgumentsSlice(MFrameArgumentsSlice* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Object);
  MOZ_ASSERT(ins->begin()->type() == MIRType::Int32);
  MOZ_ASSERT(ins->count()->type() == MIRType::Int32);

  auto* lir = new (alloc()) LFrameArgumentsSlice(
      useRegister(ins->begin()), useRegister(ins->count()), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitInlineArgumentsSlice(MInlineArgumentsSlice* ins) {
  LAllocation begin = useRegisterOrConstant(ins->begin());
  LAllocation count = useRegisterOrConstant(ins->count());
  uint32_t numActuals = ins->numActuals();
  uint32_t numOperands =
      numActuals * BOX_PIECES + LInlineArgumentsSlice::NumNonArgumentOperands;

  auto* lir = allocateVariadic<LInlineArgumentsSlice>(numOperands, temp());
  if (!lir) {
    abort(AbortReason::Alloc, "OOM: LIRGenerator::visitInlineArgumentsSlice");
    return;
  }

  lir->setOperand(LInlineArgumentsSlice::Begin, begin);
  lir->setOperand(LInlineArgumentsSlice::Count, count);
  for (uint32_t i = 0; i < numActuals; i++) {
    MDefinition* arg = ins->getArg(i);
    uint32_t index = LInlineArgumentsSlice::ArgIndex(i);
    lir->setBoxOperand(index,
                       useBoxOrTypedOrConstant(arg, /*useConstant = */ true));
  }
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNormalizeSliceTerm(MNormalizeSliceTerm* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Int32);
  MOZ_ASSERT(ins->value()->type() == MIRType::Int32);
  MOZ_ASSERT(ins->length()->type() == MIRType::Int32);

  auto* lir = new (alloc()) LNormalizeSliceTerm(useRegister(ins->value()),
                                                useRegister(ins->length()));
  define(lir, ins);
}

void LIRGenerator::visitArrayJoin(MArrayJoin* ins) {
  MOZ_ASSERT(ins->type() == MIRType::String);
  MOZ_ASSERT(ins->array()->type() == MIRType::Object);
  MOZ_ASSERT(ins->sep()->type() == MIRType::String);

  auto* lir = new (alloc())
      LArrayJoin(useRegisterAtStart(ins->array()),
                 useRegisterAtStart(ins->sep()), tempFixed(CallTempReg0));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitObjectKeys(MObjectKeys* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::Object);

  auto* lir = new (alloc()) LObjectKeys(useRegisterAtStart(ins->object()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitObjectKeysLength(MObjectKeysLength* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::Int32);

  auto* lir =
      new (alloc()) LObjectKeysLength(useRegisterAtStart(ins->object()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitStringSplit(MStringSplit* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Object);
  MOZ_ASSERT(ins->string()->type() == MIRType::String);
  MOZ_ASSERT(ins->separator()->type() == MIRType::String);

  LStringSplit* lir = new (alloc()) LStringSplit(
      useRegisterAtStart(ins->string()), useRegisterAtStart(ins->separator()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitLoadUnboxedScalar(MLoadUnboxedScalar* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);
  MOZ_ASSERT(IsNumericType(ins->type()) || ins->type() == MIRType::Boolean);

  auto sync = SynchronizeLoad(ins->requiresMemoryBarrier());

  if (Scalar::isBigIntType(ins->storageType()) && !sync.isNone()) {
    lowerAtomicLoad64(ins);
    return;
  }

  const LUse elements = useRegister(ins->elements());
  const LAllocation index = useRegisterOrIndexConstant(
      ins->index(), ins->storageType(), ins->offsetAdjustment());

  // NOTE: the generated code must match the assembly code in gen_load in
  // GenerateAtomicOperations.py
  if (!sync.isNone()) {
    LMemoryBarrier* fence = new (alloc()) LMemoryBarrier(sync.barrierBefore);
    add(fence, ins);
  }

  if (!Scalar::isBigIntType(ins->storageType())) {
    // We need a temp register for Uint32Array with known double result.
    LDefinition tempDef = LDefinition::BogusTemp();
    if (ins->storageType() == Scalar::Uint32 &&
        IsFloatingPointType(ins->type())) {
      tempDef = temp();
    }

    auto* lir = new (alloc()) LLoadUnboxedScalar(elements, index, tempDef);
    if (ins->fallible()) {
      assignSnapshot(lir, ins->bailoutKind());
    }
    define(lir, ins);
  } else {
    MOZ_ASSERT(ins->type() == MIRType::BigInt);

    auto* lir =
        new (alloc()) LLoadUnboxedBigInt(elements, index, temp(), tempInt64());
    define(lir, ins);
    assignSafepoint(lir, ins);
  }

  if (!sync.isNone()) {
    LMemoryBarrier* fence = new (alloc()) LMemoryBarrier(sync.barrierAfter);
    add(fence, ins);
  }
}

void LIRGenerator::visitLoadDataViewElement(MLoadDataViewElement* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);

  MOZ_ASSERT(IsNumericType(ins->type()));

  const LUse elements = useRegister(ins->elements());
  const LUse index = useRegister(ins->index());
  const LAllocation littleEndian = useRegisterOrConstant(ins->littleEndian());

  // We need a temp register for:
  // - Uint32Array with known double result,
  // - Float32Array,
  // - and BigInt64Array and BigUint64Array.
  LDefinition tempDef = LDefinition::BogusTemp();
  if ((ins->storageType() == Scalar::Uint32 &&
       IsFloatingPointType(ins->type())) ||
      ins->storageType() == Scalar::Float32) {
    tempDef = temp();
  }
  if (Scalar::isBigIntType(ins->storageType())) {
#ifdef JS_CODEGEN_X86
    // There are not enough registers on x86.
    if (littleEndian.isConstant()) {
      tempDef = temp();
    }
#else
    tempDef = temp();
#endif
  }

  // We also need a separate 64-bit temp register for:
  // - Float64Array
  // - and BigInt64Array and BigUint64Array.
  LInt64Definition temp64Def = LInt64Definition::BogusTemp();
  if (Scalar::byteSize(ins->storageType()) == 8) {
    temp64Def = tempInt64();
  }

  auto* lir = new (alloc())
      LLoadDataViewElement(elements, index, littleEndian, tempDef, temp64Def);
  if (ins->fallible()) {
    assignSnapshot(lir, ins->bailoutKind());
  }
  define(lir, ins);
  if (Scalar::isBigIntType(ins->storageType())) {
    assignSafepoint(lir, ins);
  }
}

void LIRGenerator::visitClampToUint8(MClampToUint8* ins) {
  MDefinition* in = ins->input();

  switch (in->type()) {
    case MIRType::Boolean:
      redefine(ins, in);
      break;

    case MIRType::Int32:
      defineReuseInput(new (alloc()) LClampIToUint8(useRegisterAtStart(in)),
                       ins, 0);
      break;

    case MIRType::Double:
      // LClampDToUint8 clobbers its input register. Making it available as
      // a temp copy describes this behavior to the register allocator.
      define(new (alloc())
                 LClampDToUint8(useRegisterAtStart(in), tempCopy(in, 0)),
             ins);
      break;

    case MIRType::Value: {
      LClampVToUint8* lir =
          new (alloc()) LClampVToUint8(useBox(in), tempDouble());
      assignSnapshot(lir, ins->bailoutKind());
      define(lir, ins);
      assignSafepoint(lir, ins);
      break;
    }

    default:
      MOZ_CRASH("unexpected type");
  }
}

void LIRGenerator::visitLoadTypedArrayElementHole(
    MLoadTypedArrayElementHole* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);
  MOZ_ASSERT(ins->length()->type() == MIRType::IntPtr);

  MOZ_ASSERT(ins->type() == MIRType::Value);

  const LUse elements = useRegister(ins->elements());
  const LAllocation index = useRegister(ins->index());
  const LAllocation length = useRegister(ins->length());

  if (!Scalar::isBigIntType(ins->arrayType())) {
    auto* lir =
        new (alloc()) LLoadTypedArrayElementHole(elements, index, length);
    if (ins->fallible()) {
      assignSnapshot(lir, ins->bailoutKind());
    }
    defineBox(lir, ins);
  } else {
#ifdef JS_CODEGEN_X86
    LInt64Definition temp64 = LInt64Definition::BogusTemp();
#else
    LInt64Definition temp64 = tempInt64();
#endif

    auto* lir = new (alloc()) LLoadTypedArrayElementHoleBigInt(
        elements, index, length, temp(), temp64);
    defineBox(lir, ins);
    assignSafepoint(lir, ins);
  }
}

void LIRGenerator::visitStoreUnboxedScalar(MStoreUnboxedScalar* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);

  if (ins->isFloatWrite()) {
    MOZ_ASSERT_IF(ins->writeType() == Scalar::Float32,
                  ins->value()->type() == MIRType::Float32);
    MOZ_ASSERT_IF(ins->writeType() == Scalar::Float64,
                  ins->value()->type() == MIRType::Double);
  } else if (ins->isBigIntWrite()) {
    MOZ_ASSERT(ins->value()->type() == MIRType::BigInt);
  } else {
    MOZ_ASSERT(ins->value()->type() == MIRType::Int32);
  }

  auto sync = SynchronizeStore(ins->requiresMemoryBarrier());

  if (ins->isBigIntWrite() && !sync.isNone()) {
    lowerAtomicStore64(ins);
    return;
  }

  LUse elements = useRegister(ins->elements());
  LAllocation index =
      useRegisterOrIndexConstant(ins->index(), ins->writeType());
  LAllocation value;

  // For byte arrays, the value has to be in a byte register on x86.
  if (ins->isByteWrite()) {
    value = useByteOpRegisterOrNonDoubleConstant(ins->value());
  } else if (ins->isBigIntWrite()) {
    value = useRegister(ins->value());
  } else {
    value = useRegisterOrNonDoubleConstant(ins->value());
  }

  // Optimization opportunity for atomics: on some platforms there
  // is a store instruction that incorporates the necessary
  // barriers, and we could use that instead of separate barrier and
  // store instructions.  See bug #1077027.
  //
  // NOTE: the generated code must match the assembly code in gen_store in
  // GenerateAtomicOperations.py
  if (!sync.isNone()) {
    LMemoryBarrier* fence = new (alloc()) LMemoryBarrier(sync.barrierBefore);
    add(fence, ins);
  }
  if (!ins->isBigIntWrite()) {
    add(new (alloc()) LStoreUnboxedScalar(elements, index, value), ins);
  } else {
    add(new (alloc()) LStoreUnboxedBigInt(elements, index, value, tempInt64()),
        ins);
  }
  if (!sync.isNone()) {
    LMemoryBarrier* fence = new (alloc()) LMemoryBarrier(sync.barrierAfter);
    add(fence, ins);
  }
}

void LIRGenerator::visitStoreDataViewElement(MStoreDataViewElement* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);
  MOZ_ASSERT(ins->littleEndian()->type() == MIRType::Boolean);

  if (ins->isFloatWrite()) {
    MOZ_ASSERT_IF(ins->writeType() == Scalar::Float32,
                  ins->value()->type() == MIRType::Float32);
    MOZ_ASSERT_IF(ins->writeType() == Scalar::Float64,
                  ins->value()->type() == MIRType::Double);
  } else if (ins->isBigIntWrite()) {
    MOZ_ASSERT(ins->value()->type() == MIRType::BigInt);
  } else {
    MOZ_ASSERT(ins->value()->type() == MIRType::Int32);
  }

  LUse elements = useRegister(ins->elements());
  LUse index = useRegister(ins->index());
  LAllocation value;
  if (ins->isBigIntWrite()) {
    value = useRegister(ins->value());
  } else {
    value = useRegisterOrNonDoubleConstant(ins->value());
  }
  LAllocation littleEndian = useRegisterOrConstant(ins->littleEndian());

  LDefinition tempDef = LDefinition::BogusTemp();
  LInt64Definition temp64Def = LInt64Definition::BogusTemp();
  if (Scalar::byteSize(ins->writeType()) < 8) {
    tempDef = temp();
  } else {
    temp64Def = tempInt64();
  }

  add(new (alloc()) LStoreDataViewElement(elements, index, value, littleEndian,
                                          tempDef, temp64Def),
      ins);
}

void LIRGenerator::visitStoreTypedArrayElementHole(
    MStoreTypedArrayElementHole* ins) {
  MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
  MOZ_ASSERT(ins->index()->type() == MIRType::IntPtr);
  MOZ_ASSERT(ins->length()->type() == MIRType::IntPtr);

  if (ins->isFloatWrite()) {
    MOZ_ASSERT_IF(ins->arrayType() == Scalar::Float32,
                  ins->value()->type() == MIRType::Float32);
    MOZ_ASSERT_IF(ins->arrayType() == Scalar::Float64,
                  ins->value()->type() == MIRType::Double);
  } else if (ins->isBigIntWrite()) {
    MOZ_ASSERT(ins->value()->type() == MIRType::BigInt);
  } else {
    MOZ_ASSERT(ins->value()->type() == MIRType::Int32);
  }

  LUse elements = useRegister(ins->elements());
  LAllocation length = useAny(ins->length());
  LAllocation index = useRegister(ins->index());

  // For byte arrays, the value has to be in a byte register on x86.
  LAllocation value;
  if (ins->isByteWrite()) {
    value = useByteOpRegisterOrNonDoubleConstant(ins->value());
  } else if (ins->isBigIntWrite()) {
    value = useRegister(ins->value());
  } else {
    value = useRegisterOrNonDoubleConstant(ins->value());
  }

  if (!ins->isBigIntWrite()) {
    LDefinition spectreTemp =
        BoundsCheckNeedsSpectreTemp() ? temp() : LDefinition::BogusTemp();
    auto* lir = new (alloc()) LStoreTypedArrayElementHole(
        elements, length, index, value, spectreTemp);
    add(lir, ins);
  } else {
    auto* lir = new (alloc()) LStoreTypedArrayElementHoleBigInt(
        elements, length, index, value, tempInt64());
    add(lir, ins);
  }
}

void LIRGenerator::visitLoadScriptedProxyHandler(
    MLoadScriptedProxyHandler* ins) {
  LLoadScriptedProxyHandler* lir = new (alloc())
      LLoadScriptedProxyHandler(useRegisterAtStart(ins->object()));
  assignSnapshot(lir, ins->bailoutKind());
  define(lir, ins);
}

void LIRGenerator::visitIdToStringOrSymbol(MIdToStringOrSymbol* ins) {
  LIdToStringOrSymbol* lir =
      new (alloc()) LIdToStringOrSymbol(useBoxAtStart(ins->idVal()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  defineBox(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitLoadFixedSlot(MLoadFixedSlot* ins) {
  MDefinition* obj = ins->object();
  MOZ_ASSERT(obj->type() == MIRType::Object);

  MIRType type = ins->type();

  if (type == MIRType::Value) {
    if (ins->usedAsPropertyKey()) {
      LLoadFixedSlotAndAtomize* lir =
          new (alloc()) LLoadFixedSlotAndAtomize(useRegister(obj), temp());
      defineBox(lir, ins);
      assignSafepoint(lir, ins);
    } else {
      LLoadFixedSlotV* lir =
          new (alloc()) LLoadFixedSlotV(useRegisterAtStart(obj));
      defineBox(lir, ins);
    }
  } else {
    LLoadFixedSlotT* lir =
        new (alloc()) LLoadFixedSlotT(useRegisterForTypedLoad(obj, type));
    define(lir, ins);
  }
}

void LIRGenerator::visitLoadFixedSlotAndUnbox(MLoadFixedSlotAndUnbox* ins) {
  MDefinition* obj = ins->object();
  MOZ_ASSERT(obj->type() == MIRType::Object);

  if (ins->usedAsPropertyKey() && ins->type() == MIRType::String) {
    LLoadFixedSlotUnboxAndAtomize* lir =
        new (alloc()) LLoadFixedSlotUnboxAndAtomize(useRegister(obj));
    if (ins->fallible()) {
      assignSnapshot(lir, ins->bailoutKind());
    }
    define(lir, ins);
    assignSafepoint(lir, ins);
  } else {
    LLoadFixedSlotAndUnbox* lir =
        new (alloc()) LLoadFixedSlotAndUnbox(useRegisterAtStart(obj));
    if (ins->fallible()) {
      assignSnapshot(lir, ins->bailoutKind());
    }
    define(lir, ins);
  }
}

void LIRGenerator::visitLoadDynamicSlotAndUnbox(MLoadDynamicSlotAndUnbox* ins) {
  MDefinition* slots = ins->slots();
  MOZ_ASSERT(slots->type() == MIRType::Slots);

  if (ins->usedAsPropertyKey() && ins->type() == MIRType::String) {
    auto* lir =
        new (alloc()) LLoadDynamicSlotUnboxAndAtomize(useRegister(slots));
    if (ins->fallible()) {
      assignSnapshot(lir, ins->bailoutKind());
    }
    define(lir, ins);
    assignSafepoint(lir, ins);
  } else {
    auto* lir =
        new (alloc()) LLoadDynamicSlotAndUnbox(useRegisterAtStart(slots));
    if (ins->fallible()) {
      assignSnapshot(lir, ins->bailoutKind());
    }
    define(lir, ins);
  }
}

void LIRGenerator::visitLoadElementAndUnbox(MLoadElementAndUnbox* ins) {
  MDefinition* elements = ins->elements();
  MDefinition* index = ins->index();
  MOZ_ASSERT(elements->type() == MIRType::Elements);
  MOZ_ASSERT(index->type() == MIRType::Int32);

  auto* lir = new (alloc())
      LLoadElementAndUnbox(useRegister(elements), useRegisterOrConstant(index));
  if (ins->fallible()) {
    assignSnapshot(lir, ins->bailoutKind());
  }
  define(lir, ins);
}

void LIRGenerator::visitAddAndStoreSlot(MAddAndStoreSlot* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  LDefinition maybeTemp = LDefinition::BogusTemp();
  if (ins->kind() != MAddAndStoreSlot::Kind::FixedSlot) {
    maybeTemp = temp();
  }

  auto* lir = new (alloc()) LAddAndStoreSlot(useRegister(ins->object()),
                                             useBox(ins->value()), maybeTemp);
  add(lir, ins);
}

void LIRGenerator::visitAllocateAndStoreSlot(MAllocateAndStoreSlot* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  auto* lir = new (alloc()) LAllocateAndStoreSlot(
      useRegisterAtStart(ins->object()), useBoxAtStart(ins->value()),
      tempFixed(CallTempReg0), tempFixed(CallTempReg1));
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
}

void LIRGenerator::visitAddSlotAndCallAddPropHook(
    MAddSlotAndCallAddPropHook* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->value()->type() == MIRType::Value);

  auto* lir = new (alloc()) LAddSlotAndCallAddPropHook(
      useRegisterAtStart(ins->object()), useBoxAtStart(ins->value()));
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitStoreFixedSlot(MStoreFixedSlot* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  if (ins->value()->type() == MIRType::Value) {
    LStoreFixedSlotV* lir = new (alloc())
        LStoreFixedSlotV(useRegister(ins->object()), useBox(ins->value()));
    add(lir, ins);
  } else {
    LStoreFixedSlotT* lir = new (alloc()) LStoreFixedSlotT(
        useRegister(ins->object()), useRegisterOrConstant(ins->value()));
    add(lir, ins);
  }
}

void LIRGenerator::visitGetNameCache(MGetNameCache* ins) {
  MOZ_ASSERT(ins->envObj()->type() == MIRType::Object);

  // Emit an overrecursed check: this is necessary because the cache can
  // attach a scripted getter stub that calls this script recursively.
  gen->setNeedsOverrecursedCheck();

  LGetNameCache* lir =
      new (alloc()) LGetNameCache(useRegister(ins->envObj()), temp());
  defineBox(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCallGetIntrinsicValue(MCallGetIntrinsicValue* ins) {
  LCallGetIntrinsicValue* lir = new (alloc()) LCallGetIntrinsicValue();
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitGetPropSuperCache(MGetPropSuperCache* ins) {
  MDefinition* obj = ins->object();
  MDefinition* receiver = ins->receiver();
  MDefinition* id = ins->idval();

  gen->setNeedsOverrecursedCheck();

  bool useConstId =
      id->type() == MIRType::String || id->type() == MIRType::Symbol;

  auto* lir = new (alloc())
      LGetPropSuperCache(useRegister(obj), useBoxOrTyped(receiver),
                         useBoxOrTypedOrConstant(id, useConstId));
  defineBox(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitGetPropertyCache(MGetPropertyCache* ins) {
  MDefinition* value = ins->value();
  MOZ_ASSERT(value->type() == MIRType::Object ||
             value->type() == MIRType::Value);

  MDefinition* id = ins->idval();
  MOZ_ASSERT(id->type() == MIRType::String || id->type() == MIRType::Symbol ||
             id->type() == MIRType::Int32 || id->type() == MIRType::Value);

  // Emit an overrecursed check: this is necessary because the cache can
  // attach a scripted getter stub that calls this script recursively.
  gen->setNeedsOverrecursedCheck();

  // If this is a GetProp, the id is a constant string. Allow passing it as a
  // constant to reduce register allocation pressure.
  bool useConstId =
      id->type() == MIRType::String || id->type() == MIRType::Symbol;

  auto* lir = new (alloc()) LGetPropertyCache(
      useBoxOrTyped(value), useBoxOrTypedOrConstant(id, useConstId));
  defineBox(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitBindNameCache(MBindNameCache* ins) {
  MOZ_ASSERT(ins->envChain()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::Object);

  LBindNameCache* lir =
      new (alloc()) LBindNameCache(useRegister(ins->envChain()), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCallBindVar(MCallBindVar* ins) {
  MOZ_ASSERT(ins->environmentChain()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::Object);

  LCallBindVar* lir =
      new (alloc()) LCallBindVar(useRegister(ins->environmentChain()));
  define(lir, ins);
}

void LIRGenerator::visitGuardObjectIdentity(MGuardObjectIdentity* ins) {
  LGuardObjectIdentity* guard = new (alloc()) LGuardObjectIdentity(
      useRegister(ins->object()), useRegister(ins->expected()));
  assignSnapshot(guard, ins->bailoutKind());
  add(guard, ins);
  redefine(ins, ins->object());
}

void LIRGenerator::visitGuardSpecificFunction(MGuardSpecificFunction* ins) {
  auto* guard = new (alloc()) LGuardSpecificFunction(
      useRegister(ins->function()), useRegister(ins->expected()));
  assignSnapshot(guard, ins->bailoutKind());
  add(guard, ins);
  redefine(ins, ins->function());
}

void LIRGenerator::visitGuardSpecificAtom(MGuardSpecificAtom* ins) {
  auto* guard =
      new (alloc()) LGuardSpecificAtom(useRegister(ins->str()), temp());
  assignSnapshot(guard, ins->bailoutKind());
  add(guard, ins);
  redefine(ins, ins->str());
  assignSafepoint(guard, ins);
}

void LIRGenerator::visitGuardSpecificSymbol(MGuardSpecificSymbol* ins) {
  auto* guard = new (alloc()) LGuardSpecificSymbol(useRegister(ins->symbol()));
  assignSnapshot(guard, ins->bailoutKind());
  add(guard, ins);
  redefine(ins, ins->symbol());
}

void LIRGenerator::visitGuardSpecificInt32(MGuardSpecificInt32* ins) {
  auto* guard = new (alloc()) LGuardSpecificInt32(useRegister(ins->num()));
  assignSnapshot(guard, ins->bailoutKind());
  add(guard, ins);
  redefine(ins, ins->num());
}

void LIRGenerator::visitGuardStringToIndex(MGuardStringToIndex* ins) {
  MOZ_ASSERT(ins->string()->type() == MIRType::String);
  auto* guard = new (alloc()) LGuardStringToIndex(useRegister(ins->string()));
  assignSnapshot(guard, ins->bailoutKind());
  define(guard, ins);
  assignSafepoint(guard, ins);
}

void LIRGenerator::visitGuardStringToInt32(MGuardStringToInt32* ins) {
  MOZ_ASSERT(ins->string()->type() == MIRType::String);
  auto* guard =
      new (alloc()) LGuardStringToInt32(useRegister(ins->string()), temp());
  assignSnapshot(guard, ins->bailoutKind());
  define(guard, ins);
  assignSafepoint(guard, ins);
}

void LIRGenerator::visitGuardStringToDouble(MGuardStringToDouble* ins) {
  MOZ_ASSERT(ins->string()->type() == MIRType::String);
  auto* guard = new (alloc())
      LGuardStringToDouble(useRegister(ins->string()), temp(), temp());
  assignSnapshot(guard, ins->bailoutKind());
  define(guard, ins);
  assignSafepoint(guard, ins);
}

void LIRGenerator::visitGuardNoDenseElements(MGuardNoDenseElements* ins) {
  auto* guard =
      new (alloc()) LGuardNoDenseElements(useRegister(ins->object()), temp());
  assignSnapshot(guard, ins->bailoutKind());
  add(guard, ins);
  redefine(ins, ins->object());
}

void LIRGenerator::visitGuardShape(MGuardShape* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  if (JitOptions.spectreObjectMitigations) {
    auto* lir =
        new (alloc()) LGuardShape(useRegisterAtStart(ins->object()), temp());
    assignSnapshot(lir, ins->bailoutKind());
    defineReuseInput(lir, ins, 0);
  } else {
    auto* lir = new (alloc())
        LGuardShape(useRegister(ins->object()), LDefinition::BogusTemp());
    assignSnapshot(lir, ins->bailoutKind());
    add(lir, ins);
    redefine(ins, ins->object());
  }
}

void LIRGenerator::visitGuardMultipleShapes(MGuardMultipleShapes* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  if (JitOptions.spectreObjectMitigations) {
    auto* lir = new (alloc()) LGuardMultipleShapes(
        useRegisterAtStart(ins->object()), useRegister(ins->shapeList()),
        temp(), temp(), temp(), temp());
    assignSnapshot(lir, ins->bailoutKind());
    defineReuseInput(lir, ins, 0);
  } else {
    auto* lir = new (alloc()) LGuardMultipleShapes(
        useRegister(ins->object()), useRegister(ins->shapeList()), temp(),
        temp(), temp(), LDefinition::BogusTemp());
    assignSnapshot(lir, ins->bailoutKind());
    add(lir, ins);
    redefine(ins, ins->object());
  }
}

void LIRGenerator::visitGuardProto(MGuardProto* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->expected()->type() == MIRType::Object);

  auto* lir = new (alloc()) LGuardProto(useRegister(ins->object()),
                                        useRegister(ins->expected()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->object());
}

void LIRGenerator::visitGuardNullProto(MGuardNullProto* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  auto* lir = new (alloc()) LGuardNullProto(useRegister(ins->object()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->object());
}

void LIRGenerator::visitGuardIsNativeObject(MGuardIsNativeObject* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  auto* lir =
      new (alloc()) LGuardIsNativeObject(useRegister(ins->object()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->object());
}

void LIRGenerator::visitGuardGlobalGeneration(MGuardGlobalGeneration* ins) {
  auto* lir = new (alloc()) LGuardGlobalGeneration(temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
}

void LIRGenerator::visitGuardFuse(MGuardFuse* ins) {
  auto* lir = new (alloc()) LGuardFuse(temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
}

void LIRGenerator::visitGuardIsProxy(MGuardIsProxy* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  auto* lir = new (alloc()) LGuardIsProxy(useRegister(ins->object()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->object());
}

void LIRGenerator::visitGuardIsNotProxy(MGuardIsNotProxy* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  auto* lir =
      new (alloc()) LGuardIsNotProxy(useRegister(ins->object()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->object());
}

void LIRGenerator::visitGuardIsNotDOMProxy(MGuardIsNotDOMProxy* ins) {
  MOZ_ASSERT(ins->proxy()->type() == MIRType::Object);

  auto* lir =
      new (alloc()) LGuardIsNotDOMProxy(useRegister(ins->proxy()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->proxy());
}

void LIRGenerator::visitProxyGet(MProxyGet* ins) {
  MOZ_ASSERT(ins->proxy()->type() == MIRType::Object);
  auto* lir = new (alloc())
      LProxyGet(useRegisterAtStart(ins->proxy()), tempFixed(CallTempReg0));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitProxyGetByValue(MProxyGetByValue* ins) {
  MOZ_ASSERT(ins->proxy()->type() == MIRType::Object);
  MOZ_ASSERT(ins->idVal()->type() == MIRType::Value);
  auto* lir = new (alloc()) LProxyGetByValue(useRegisterAtStart(ins->proxy()),
                                             useBoxAtStart(ins->idVal()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitProxyHasProp(MProxyHasProp* ins) {
  MOZ_ASSERT(ins->proxy()->type() == MIRType::Object);
  MOZ_ASSERT(ins->idVal()->type() == MIRType::Value);
  auto* lir = new (alloc()) LProxyHasProp(useRegisterAtStart(ins->proxy()),
                                          useBoxAtStart(ins->idVal()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitProxySet(MProxySet* ins) {
  MOZ_ASSERT(ins->proxy()->type() == MIRType::Object);
  MOZ_ASSERT(ins->rhs()->type() == MIRType::Value);
  auto* lir = new (alloc())
      LProxySet(useRegisterAtStart(ins->proxy()), useBoxAtStart(ins->rhs()),
                tempFixed(CallTempReg0));
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitProxySetByValue(MProxySetByValue* ins) {
  MOZ_ASSERT(ins->proxy()->type() == MIRType::Object);
  MOZ_ASSERT(ins->idVal()->type() == MIRType::Value);
  MOZ_ASSERT(ins->rhs()->type() == MIRType::Value);
  auto* lir = new (alloc())
      LProxySetByValue(useRegisterAtStart(ins->proxy()),
                       useBoxAtStart(ins->idVal()), useBoxAtStart(ins->rhs()));
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCallSetArrayLength(MCallSetArrayLength* ins) {
  MOZ_ASSERT(ins->obj()->type() == MIRType::Object);
  MOZ_ASSERT(ins->rhs()->type() == MIRType::Value);
  auto* lir = new (alloc()) LCallSetArrayLength(useRegisterAtStart(ins->obj()),
                                                useBoxAtStart(ins->rhs()));
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitMegamorphicLoadSlot(MMegamorphicLoadSlot* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  auto* lir = new (alloc())
      LMegamorphicLoadSlot(useRegisterAtStart(ins->object()),
                           tempFixed(CallTempReg0), tempFixed(CallTempReg1),
                           tempFixed(CallTempReg2), tempFixed(CallTempReg3));
  assignSnapshot(lir, ins->bailoutKind());
  defineReturn(lir, ins);
}

void LIRGenerator::visitMegamorphicLoadSlotByValue(
    MMegamorphicLoadSlotByValue* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->idVal()->type() == MIRType::Value);
  auto* lir = new (alloc()) LMegamorphicLoadSlotByValue(
      useRegisterAtStart(ins->object()), useBoxAtStart(ins->idVal()),
      tempFixed(CallTempReg0), tempFixed(CallTempReg1),
      tempFixed(CallTempReg2));
  assignSnapshot(lir, ins->bailoutKind());
  defineReturn(lir, ins);
}

void LIRGenerator::visitMegamorphicStoreSlot(MMegamorphicStoreSlot* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->rhs()->type() == MIRType::Value);

#ifdef JS_CODEGEN_X86
  auto* lir = new (alloc()) LMegamorphicStoreSlot(
      useFixedAtStart(ins->object(), CallTempReg0),
      useBoxFixedAtStart(ins->rhs(), CallTempReg1, CallTempReg2),
      tempFixed(CallTempReg5));
#else
  auto* lir = new (alloc())
      LMegamorphicStoreSlot(useRegisterAtStart(ins->object()),
                            useBoxAtStart(ins->rhs()), tempFixed(CallTempReg0),
                            tempFixed(CallTempReg1), tempFixed(CallTempReg2));
#endif

  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitMegamorphicHasProp(MMegamorphicHasProp* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->idVal()->type() == MIRType::Value);
  auto* lir = new (alloc())
      LMegamorphicHasProp(useRegisterAtStart(ins->object()),
                          useBoxAtStart(ins->idVal()), tempFixed(CallTempReg0),
                          tempFixed(CallTempReg1), tempFixed(CallTempReg2));
  assignSnapshot(lir, ins->bailoutKind());
  defineReturn(lir, ins);
}

void LIRGenerator::visitSmallObjectVariableKeyHasProp(
    MSmallObjectVariableKeyHasProp* ins) {
  MOZ_ASSERT(ins->idStr()->type() == MIRType::String);
  auto* lir = new (alloc())
      LSmallObjectVariableKeyHasProp(useRegisterAtStart(ins->idStr()));
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitGuardIsNotArrayBufferMaybeShared(
    MGuardIsNotArrayBufferMaybeShared* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  auto* lir = new (alloc())
      LGuardIsNotArrayBufferMaybeShared(useRegister(ins->object()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->object());
}

void LIRGenerator::visitGuardIsTypedArray(MGuardIsTypedArray* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  auto* lir =
      new (alloc()) LGuardIsTypedArray(useRegister(ins->object()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->object());
}

void LIRGenerator::visitGuardIsFixedLengthTypedArray(
    MGuardIsFixedLengthTypedArray* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  auto* lir = new (alloc())
      LGuardIsFixedLengthTypedArray(useRegister(ins->object()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->object());
}

void LIRGenerator::visitGuardIsResizableTypedArray(
    MGuardIsResizableTypedArray* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  auto* lir = new (alloc())
      LGuardIsResizableTypedArray(useRegister(ins->object()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->object());
}

void LIRGenerator::visitGuardHasProxyHandler(MGuardHasProxyHandler* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  auto* lir = new (alloc()) LGuardHasProxyHandler(useRegister(ins->object()));
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->object());
}

void LIRGenerator::visitNurseryObject(MNurseryObject* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Object);

  auto* lir = new (alloc()) LNurseryObject();
  define(lir, ins);
}

void LIRGenerator::visitGuardValue(MGuardValue* ins) {
  MOZ_ASSERT(ins->value()->type() == MIRType::Value);
  auto* lir = new (alloc()) LGuardValue(useBox(ins->value()));
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->value());
}

void LIRGenerator::visitGuardNullOrUndefined(MGuardNullOrUndefined* ins) {
  MOZ_ASSERT(ins->value()->type() == MIRType::Value);
  auto* lir = new (alloc()) LGuardNullOrUndefined(useBox(ins->value()));
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->value());
}

void LIRGenerator::visitGuardIsNotObject(MGuardIsNotObject* ins) {
  MOZ_ASSERT(ins->value()->type() == MIRType::Value);
  auto* lir = new (alloc()) LGuardIsNotObject(useBox(ins->value()));
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->value());
}

void LIRGenerator::visitGuardFunctionFlags(MGuardFunctionFlags* ins) {
  MOZ_ASSERT(ins->function()->type() == MIRType::Object);

  auto* lir = new (alloc()) LGuardFunctionFlags(useRegister(ins->function()));
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->function());
}

void LIRGenerator::visitGuardFunctionIsNonBuiltinCtor(
    MGuardFunctionIsNonBuiltinCtor* ins) {
  MOZ_ASSERT(ins->function()->type() == MIRType::Object);

  auto* lir = new (alloc())
      LGuardFunctionIsNonBuiltinCtor(useRegister(ins->function()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->function());
}

void LIRGenerator::visitGuardFunctionKind(MGuardFunctionKind* ins) {
  MOZ_ASSERT(ins->function()->type() == MIRType::Object);

  auto* lir =
      new (alloc()) LGuardFunctionKind(useRegister(ins->function()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->function());
}

void LIRGenerator::visitGuardFunctionScript(MGuardFunctionScript* ins) {
  MOZ_ASSERT(ins->function()->type() == MIRType::Object);

  auto* lir = new (alloc()) LGuardFunctionScript(useRegister(ins->function()));
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->function());
}

void LIRGenerator::visitAssertRange(MAssertRange* ins) {
  MDefinition* input = ins->input();
  LInstruction* lir = nullptr;

  switch (input->type()) {
    case MIRType::Boolean:
    case MIRType::Int32:
    case MIRType::IntPtr:
      lir = new (alloc()) LAssertRangeI(useRegisterAtStart(input));
      break;

    case MIRType::Double:
      lir = new (alloc()) LAssertRangeD(useRegister(input), tempDouble());
      break;

    case MIRType::Float32:
      lir = new (alloc())
          LAssertRangeF(useRegister(input), tempDouble(), tempDouble());
      break;

    case MIRType::Value:
      lir = new (alloc()) LAssertRangeV(useBox(input), tempToUnbox(),
                                        tempDouble(), tempDouble());
      break;

    default:
      MOZ_CRASH("Unexpected Range for MIRType");
      break;
  }

  lir->setMir(ins);
  add(lir);
}

void LIRGenerator::visitAssertClass(MAssertClass* ins) {
  auto* lir =
      new (alloc()) LAssertClass(useRegisterAtStart(ins->input()), temp());
  add(lir, ins);
}

void LIRGenerator::visitAssertShape(MAssertShape* ins) {
  auto* lir = new (alloc()) LAssertShape(useRegisterAtStart(ins->input()));
  add(lir, ins);
}

void LIRGenerator::visitDeleteProperty(MDeleteProperty* ins) {
  LCallDeleteProperty* lir =
      new (alloc()) LCallDeleteProperty(useBoxAtStart(ins->value()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitDeleteElement(MDeleteElement* ins) {
  LCallDeleteElement* lir = new (alloc()) LCallDeleteElement(
      useBoxAtStart(ins->value()), useBoxAtStart(ins->index()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitObjectToIterator(MObjectToIterator* ins) {
  auto* lir = new (alloc())
      LObjectToIterator(useRegister(ins->object()), temp(), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitValueToIterator(MValueToIterator* ins) {
  auto* lir = new (alloc()) LValueToIterator(useBoxAtStart(ins->value()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitLoadSlotByIteratorIndex(MLoadSlotByIteratorIndex* ins) {
  auto* lir = new (alloc()) LLoadSlotByIteratorIndex(
      useRegisterAtStart(ins->object()), useRegisterAtStart(ins->iterator()),
      temp(), temp());
  defineBox(lir, ins);
}

void LIRGenerator::visitStoreSlotByIteratorIndex(
    MStoreSlotByIteratorIndex* ins) {
  auto* lir = new (alloc()) LStoreSlotByIteratorIndex(
      useRegister(ins->object()), useRegister(ins->iterator()),
      useBox(ins->value()), temp(), temp());
  add(lir, ins);
}

void LIRGenerator::visitIteratorHasIndices(MIteratorHasIndices* ins) {
  MOZ_ASSERT(ins->hasOneUse());
  emitAtUses(ins);
}

void LIRGenerator::visitSetPropertyCache(MSetPropertyCache* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);

  MDefinition* id = ins->idval();
  MOZ_ASSERT(id->type() == MIRType::String || id->type() == MIRType::Symbol ||
             id->type() == MIRType::Int32 || id->type() == MIRType::Value);

  // If this is a SetProp, the id is a constant string. Allow passing it as a
  // constant to reduce register allocation pressure.
  bool useConstId =
      id->type() == MIRType::String || id->type() == MIRType::Symbol;
  bool useConstValue = IsNonNurseryConstant(ins->value());

  // Emit an overrecursed check: this is necessary because the cache can
  // attach a scripted setter stub that calls this script recursively.
  gen->setNeedsOverrecursedCheck();

  // We need a double temp register for TypedArray stubs.
  LDefinition tempD = tempFixed(FloatReg0);

  LInstruction* lir = new (alloc()) LSetPropertyCache(
      useRegister(ins->object()), useBoxOrTypedOrConstant(id, useConstId),
      useBoxOrTypedOrConstant(ins->value(), useConstValue), temp(), tempD);
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitMegamorphicSetElement(MMegamorphicSetElement* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->index()->type() == MIRType::Value);
  MOZ_ASSERT(ins->value()->type() == MIRType::Value);

  // See comment in LIROps.yaml (x86 is short on registers)
#ifdef JS_CODEGEN_X86
  auto* lir = new (alloc()) LMegamorphicSetElement(
      useFixedAtStart(ins->object(), CallTempReg0),
      useBoxFixedAtStart(ins->index(), CallTempReg1, CallTempReg2),
      useBoxFixedAtStart(ins->value(), CallTempReg3, CallTempReg4),
      tempFixed(CallTempReg5));
#else
  auto* lir = new (alloc()) LMegamorphicSetElement(
      useRegisterAtStart(ins->object()), useBoxAtStart(ins->index()),
      useBoxAtStart(ins->value()), tempFixed(CallTempReg0),
      tempFixed(CallTempReg1), tempFixed(CallTempReg2));
#endif
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitGetIteratorCache(MGetIteratorCache* ins) {
  MDefinition* value = ins->value();
  MOZ_ASSERT(value->type() == MIRType::Object ||
             value->type() == MIRType::Value);

  LGetIteratorCache* lir =
      new (alloc()) LGetIteratorCache(useBoxOrTyped(value), temp(), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitOptimizeSpreadCallCache(MOptimizeSpreadCallCache* ins) {
  MDefinition* value = ins->value();
  MOZ_ASSERT(value->type() == MIRType::Value);

  auto* lir = new (alloc()) LOptimizeSpreadCallCache(useBox(value), temp());
  defineBox(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitIteratorMore(MIteratorMore* ins) {
  LIteratorMore* lir =
      new (alloc()) LIteratorMore(useRegister(ins->iterator()), temp());
  defineBox(lir, ins);
}

void LIRGenerator::visitIsNoIter(MIsNoIter* ins) {
  MOZ_ASSERT(ins->hasOneUse());
  emitAtUses(ins);
}

void LIRGenerator::visitIteratorEnd(MIteratorEnd* ins) {
  LIteratorEnd* lir = new (alloc())
      LIteratorEnd(useRegister(ins->iterator()), temp(), temp(), temp());
  add(lir, ins);
}

void LIRGenerator::visitCloseIterCache(MCloseIterCache* ins) {
  LCloseIterCache* lir =
      new (alloc()) LCloseIterCache(useRegister(ins->iter()), temp());
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitOptimizeGetIteratorCache(
    MOptimizeGetIteratorCache* ins) {
  MDefinition* value = ins->value();
  MOZ_ASSERT(value->type() == MIRType::Value);

  auto* lir = new (alloc()) LOptimizeGetIteratorCache(useBox(value), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitStringLength(MStringLength* ins) {
  MOZ_ASSERT(ins->string()->type() == MIRType::String);
  define(new (alloc()) LStringLength(useRegisterAtStart(ins->string())), ins);
}

void LIRGenerator::visitArgumentsLength(MArgumentsLength* ins) {
  define(new (alloc()) LArgumentsLength(), ins);
}

void LIRGenerator::visitGetFrameArgument(MGetFrameArgument* ins) {
  LGetFrameArgument* lir =
      new (alloc()) LGetFrameArgument(useRegisterOrConstant(ins->index()));
  defineBox(lir, ins);
}

void LIRGenerator::visitGetFrameArgumentHole(MGetFrameArgumentHole* ins) {
  LDefinition spectreTemp =
      BoundsCheckNeedsSpectreTemp() ? temp() : LDefinition::BogusTemp();

  auto* lir = new (alloc()) LGetFrameArgumentHole(
      useRegister(ins->index()), useRegister(ins->length()), spectreTemp);
  assignSnapshot(lir, ins->bailoutKind());
  defineBox(lir, ins);
}

void LIRGenerator::visitNewTarget(MNewTarget* ins) {
  LNewTarget* lir = new (alloc()) LNewTarget();
  defineBox(lir, ins);
}

void LIRGenerator::visitRest(MRest* ins) {
  MOZ_ASSERT(ins->numActuals()->type() == MIRType::Int32);

  LRest* lir =
      new (alloc()) LRest(useRegisterAtStart(ins->numActuals()),
                          tempFixed(CallTempReg0), tempFixed(CallTempReg1),
                          tempFixed(CallTempReg2), tempFixed(CallTempReg3));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitThrow(MThrow* ins) {
  MDefinition* value = ins->value();
  MOZ_ASSERT(value->type() == MIRType::Value);

  LThrow* lir = new (alloc()) LThrow(useBoxAtStart(value));
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitThrowWithStack(MThrowWithStack* ins) {
  MDefinition* value = ins->value();
  MOZ_ASSERT(value->type() == MIRType::Value);

  MDefinition* stack = ins->stack();
  MOZ_ASSERT(stack->type() == MIRType::Value);

  auto* lir =
      new (alloc()) LThrowWithStack(useBoxAtStart(value), useBoxAtStart(stack));
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitInCache(MInCache* ins) {
  MDefinition* lhs = ins->lhs();
  MDefinition* rhs = ins->rhs();

  MOZ_ASSERT(lhs->type() == MIRType::String || lhs->type() == MIRType::Symbol ||
             lhs->type() == MIRType::Int32 || lhs->type() == MIRType::Value);
  MOZ_ASSERT(rhs->type() == MIRType::Object);

  LInCache* lir =
      new (alloc()) LInCache(useBoxOrTyped(lhs), useRegister(rhs), temp());
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitHasOwnCache(MHasOwnCache* ins) {
  MDefinition* value = ins->value();
  MOZ_ASSERT(value->type() == MIRType::Object ||
             value->type() == MIRType::Value);

  MDefinition* id = ins->idval();
  MOZ_ASSERT(id->type() == MIRType::String || id->type() == MIRType::Symbol ||
             id->type() == MIRType::Int32 || id->type() == MIRType::Value);

  // Emit an overrecursed check: this is necessary because the cache can
  // attach a scripted getter stub that calls this script recursively.
  gen->setNeedsOverrecursedCheck();

  LHasOwnCache* lir =
      new (alloc()) LHasOwnCache(useBoxOrTyped(value), useBoxOrTyped(id));
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCheckPrivateFieldCache(MCheckPrivateFieldCache* ins) {
  MDefinition* value = ins->value();
  MOZ_ASSERT(value->type() == MIRType::Object ||
             value->type() == MIRType::Value);

  MDefinition* id = ins->idval();
  MOZ_ASSERT(id->type() == MIRType::String || id->type() == MIRType::Symbol ||
             id->type() == MIRType::Int32 || id->type() == MIRType::Value);

  LCheckPrivateFieldCache* lir = new (alloc())
      LCheckPrivateFieldCache(useBoxOrTyped(value), useBoxOrTyped(id));
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitNewPrivateName(MNewPrivateName* ins) {
  auto* lir = new (alloc()) LNewPrivateName();
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitInstanceOf(MInstanceOf* ins) {
  MDefinition* lhs = ins->lhs();
  MDefinition* rhs = ins->rhs();

  MOZ_ASSERT(lhs->type() == MIRType::Value || lhs->type() == MIRType::Object);
  MOZ_ASSERT(rhs->type() == MIRType::Object);

  if (lhs->type() == MIRType::Object) {
    auto* lir = new (alloc()) LInstanceOfO(useRegister(lhs), useRegister(rhs));
    define(lir, ins);
    assignSafepoint(lir, ins);
  } else {
    auto* lir = new (alloc()) LInstanceOfV(useBox(lhs), useRegister(rhs));
    define(lir, ins);
    assignSafepoint(lir, ins);
  }
}

void LIRGenerator::visitInstanceOfCache(MInstanceOfCache* ins) {
  MDefinition* lhs = ins->lhs();
  MDefinition* rhs = ins->rhs();

  MOZ_ASSERT(lhs->type() == MIRType::Value);
  MOZ_ASSERT(rhs->type() == MIRType::Object);

  LInstanceOfCache* lir =
      new (alloc()) LInstanceOfCache(useBox(lhs), useRegister(rhs));
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitIsArray(MIsArray* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Boolean);

  if (ins->value()->type() == MIRType::Object) {
    LIsArrayO* lir = new (alloc()) LIsArrayO(useRegister(ins->value()));
    define(lir, ins);
    assignSafepoint(lir, ins);
  } else {
    MOZ_ASSERT(ins->value()->type() == MIRType::Value);
    LIsArrayV* lir = new (alloc()) LIsArrayV(useBox(ins->value()), temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
  }
}

void LIRGenerator::visitIsTypedArray(MIsTypedArray* ins) {
  MOZ_ASSERT(ins->value()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::Boolean);

  auto* lir = new (alloc()) LIsTypedArray(useRegister(ins->value()));
  define(lir, ins);

  if (ins->isPossiblyWrapped()) {
    assignSafepoint(lir, ins);
  }
}

void LIRGenerator::visitIsCallable(MIsCallable* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Boolean);

  if (ins->object()->type() == MIRType::Object) {
    define(new (alloc()) LIsCallableO(useRegister(ins->object())), ins);
  } else {
    MOZ_ASSERT(ins->object()->type() == MIRType::Value);
    define(new (alloc()) LIsCallableV(useBox(ins->object()), temp()), ins);
  }
}

void LIRGenerator::visitIsConstructor(MIsConstructor* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::Boolean);
  define(new (alloc()) LIsConstructor(useRegister(ins->object())), ins);
}

void LIRGenerator::visitIsCrossRealmArrayConstructor(
    MIsCrossRealmArrayConstructor* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::Boolean);
  define(new (alloc())
             LIsCrossRealmArrayConstructor(useRegister(ins->object())),
         ins);
}

static bool CanEmitAtUseForSingleTest(MInstruction* ins) {
  if (!ins->canEmitAtUses()) {
    return false;
  }

  MUseIterator iter(ins->usesBegin());
  if (iter == ins->usesEnd()) {
    return false;
  }

  MNode* node = iter->consumer();
  if (!node->isDefinition()) {
    return false;
  }

  if (!node->toDefinition()->isTest()) {
    return false;
  }

  iter++;
  return iter == ins->usesEnd();
}

void LIRGenerator::visitIsObject(MIsObject* ins) {
  if (CanEmitAtUseForSingleTest(ins)) {
    emitAtUses(ins);
    return;
  }

  MDefinition* opd = ins->input();
  MOZ_ASSERT(opd->type() == MIRType::Value);
  LIsObject* lir = new (alloc()) LIsObject(useBoxAtStart(opd));
  define(lir, ins);
}

void LIRGenerator::visitIsNullOrUndefined(MIsNullOrUndefined* ins) {
  if (CanEmitAtUseForSingleTest(ins)) {
    emitAtUses(ins);
    return;
  }

  MDefinition* opd = ins->input();
  if (opd->type() == MIRType::Value) {
    auto* lir = new (alloc()) LIsNullOrUndefined(useBoxAtStart(opd));
    define(lir, ins);
  } else {
    define(new (alloc()) LInteger(IsNullOrUndefined(opd->type())), ins);
  }
}

void LIRGenerator::visitHasClass(MHasClass* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::Boolean);
  define(new (alloc()) LHasClass(useRegister(ins->object())), ins);
}

void LIRGenerator::visitGuardToClass(MGuardToClass* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::Object);
  LGuardToClass* lir =
      new (alloc()) LGuardToClass(useRegisterAtStart(ins->object()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  defineReuseInput(lir, ins, 0);
}

void LIRGenerator::visitGuardToEitherClass(MGuardToEitherClass* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::Object);
  auto* lir = new (alloc())
      LGuardToEitherClass(useRegisterAtStart(ins->object()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  defineReuseInput(lir, ins, 0);
}

void LIRGenerator::visitGuardToFunction(MGuardToFunction* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::Object);
  LGuardToFunction* lir =
      new (alloc()) LGuardToFunction(useRegisterAtStart(ins->object()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  defineReuseInput(lir, ins, 0);
}

void LIRGenerator::visitObjectClassToString(MObjectClassToString* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::String);
  auto* lir = new (alloc()) LObjectClassToString(
      useRegisterAtStart(ins->object()), tempFixed(CallTempReg0));
  assignSnapshot(lir, ins->bailoutKind());
  defineReturn(lir, ins);
}

void LIRGenerator::visitWasmAddOffset(MWasmAddOffset* ins) {
  MOZ_ASSERT(ins->offset());
  if (ins->base()->type() == MIRType::Int32) {
    MOZ_ASSERT(ins->type() == MIRType::Int32);
    MOZ_ASSERT(ins->offset() <= UINT32_MAX);  // Because memory32
    define(new (alloc()) LWasmAddOffset(useRegisterAtStart(ins->base())), ins);
  } else {
    MOZ_ASSERT(ins->type() == MIRType::Int64);
#ifdef JS_64BIT
    defineInt64(new (alloc())
                    LWasmAddOffset64(useInt64RegisterAtStart(ins->base())),
                ins);
#else
    // Avoid situation where the input is (a,b) and the output is (b,a).
    defineInt64ReuseInput(
        new (alloc()) LWasmAddOffset64(useInt64RegisterAtStart(ins->base())),
        ins, 0);
#endif
  }
}

void LIRGenerator::visitWasmLoadInstance(MWasmLoadInstance* ins) {
  if (ins->type() == MIRType::Int64) {
#ifdef JS_PUNBOX64
    LAllocation instance = useRegisterAtStart(ins->instance());
#else
    // Avoid reusing instance for a 64-bit output pair as the load clobbers the
    // first half of that pair before loading the second half.
    LAllocation instance = useRegister(ins->instance());
#endif
    auto* lir = new (alloc()) LWasmLoadInstance64(instance);
    defineInt64(lir, ins);
  } else {
    auto* lir =
        new (alloc()) LWasmLoadInstance(useRegisterAtStart(ins->instance()));
    define(lir, ins);
  }
}

void LIRGenerator::visitWasmStoreInstance(MWasmStoreInstance* ins) {
  MDefinition* value = ins->value();
  if (value->type() == MIRType::Int64) {
#ifdef JS_PUNBOX64
    LAllocation instance = useRegisterAtStart(ins->instance());
    LInt64Allocation valueAlloc = useInt64RegisterAtStart(value);
#else
    LAllocation instance = useRegister(ins->instance());
    LInt64Allocation valueAlloc = useInt64Register(value);
#endif
    add(new (alloc()) LWasmStoreSlotI64(valueAlloc, instance, ins->offset(),
                                        mozilla::Nothing()),
        ins);
  } else {
    MOZ_ASSERT(value->type() != MIRType::WasmAnyRef);
    LAllocation instance = useRegisterAtStart(ins->instance());
    LAllocation valueAlloc = useRegisterAtStart(value);
    add(new (alloc())
            LWasmStoreSlot(valueAlloc, instance, ins->offset(), value->type(),
                           MNarrowingOp::None, mozilla::Nothing()),
        ins);
  }
}

void LIRGenerator::visitWasmHeapReg(MWasmHeapReg* ins) {
#ifdef WASM_HAS_HEAPREG
  auto* lir = new (alloc()) LWasmHeapReg();
  define(lir, ins);
#else
  MOZ_CRASH();
#endif
}

void LIRGenerator::visitWasmBoundsCheck(MWasmBoundsCheck* ins) {
  MOZ_ASSERT(!ins->isRedundant());

  MDefinition* index = ins->index();
  MDefinition* boundsCheckLimit = ins->boundsCheckLimit();

  MOZ_ASSERT(boundsCheckLimit->type() == index->type());

  if (index->type() == MIRType::Int64) {
    if (JitOptions.spectreIndexMasking) {
      auto* lir = new (alloc()) LWasmBoundsCheck64(
          useInt64RegisterAtStart(index), useInt64Register(boundsCheckLimit));
      defineInt64ReuseInput(lir, ins, 0);
    } else {
      auto* lir = new (alloc())
          LWasmBoundsCheck64(useInt64RegisterAtStart(index),
                             useInt64RegisterAtStart(boundsCheckLimit));
      add(lir, ins);
    }
  } else {
    MOZ_ASSERT(index->type() == MIRType::Int32);

    if (JitOptions.spectreIndexMasking) {
      auto* lir = new (alloc()) LWasmBoundsCheck(useRegisterAtStart(index),
                                                 useRegister(boundsCheckLimit));
      defineReuseInput(lir, ins, 0);
    } else {
      auto* lir = new (alloc()) LWasmBoundsCheck(
          useRegisterAtStart(index), useRegisterAtStart(boundsCheckLimit));
      add(lir, ins);
    }
  }
}

void LIRGenerator::visitWasmBoundsCheckRange32(MWasmBoundsCheckRange32* ins) {
  MDefinition* index = ins->index();
  MDefinition* length = ins->length();
  MDefinition* limit = ins->limit();

  MOZ_ASSERT(index->type() == MIRType::Int32);
  MOZ_ASSERT(length->type() == MIRType::Int32);
  MOZ_ASSERT(limit->type() == MIRType::Int32);

  add(new (alloc()) LWasmBoundsCheckRange32(
          useRegister(index), useRegister(length), useRegister(limit), temp()),
      ins);
}

void LIRGenerator::visitWasmAlignmentCheck(MWasmAlignmentCheck* ins) {
  MDefinition* index = ins->index();
  if (index->type() == MIRType::Int64) {
    auto* lir =
        new (alloc()) LWasmAlignmentCheck64(useInt64RegisterAtStart(index));
    add(lir, ins);
  } else {
    auto* lir = new (alloc()) LWasmAlignmentCheck(useRegisterAtStart(index));
    add(lir, ins);
  }
}

void LIRGenerator::visitWasmLoadInstanceDataField(
    MWasmLoadInstanceDataField* ins) {
  size_t offs = wasm::Instance::offsetInData(ins->instanceDataOffset());
  if (ins->type() == MIRType::Int64) {
#ifdef JS_PUNBOX64
    LAllocation instance = useRegisterAtStart(ins->instance());
#else
    // Avoid reusing instance for the output pair as the load clobbers the first
    // half of that pair before loading the second half.
    LAllocation instance = useRegister(ins->instance());
#endif
    defineInt64(new (alloc())
                    LWasmLoadSlotI64(instance, offs, mozilla::Nothing()),
                ins);
  } else {
    LAllocation instance = useRegisterAtStart(ins->instance());
    define(new (alloc()) LWasmLoadSlot(instance, offs, ins->type(),
                                       MWideningOp::None, mozilla::Nothing()),
           ins);
  }
}

void LIRGenerator::visitWasmLoadGlobalCell(MWasmLoadGlobalCell* ins) {
  if (ins->type() == MIRType::Int64) {
#ifdef JS_PUNBOX64
    LAllocation cellPtr = useRegisterAtStart(ins->cellPtr());
#else
    // Avoid reusing cellPtr for the output pair as the load clobbers the first
    // half of that pair before loading the second half.
    LAllocation cellPtr = useRegister(ins->cellPtr());
#endif
    defineInt64(new (alloc())
                    LWasmLoadSlotI64(cellPtr, /*offset=*/0, mozilla::Nothing()),
                ins);
  } else {
    LAllocation cellPtr = useRegisterAtStart(ins->cellPtr());
    define(new (alloc()) LWasmLoadSlot(cellPtr, /*offset=*/0, ins->type(),
                                       MWideningOp::None, mozilla::Nothing()),
           ins);
  }
}

void LIRGenerator::visitWasmLoadTableElement(MWasmLoadTableElement* ins) {
  LAllocation elements = useRegisterAtStart(ins->elements());
  LAllocation index = useRegisterAtStart(ins->index());
  define(new (alloc()) LWasmLoadTableElement(elements, index), ins);
}

void LIRGenerator::visitWasmStoreInstanceDataField(
    MWasmStoreInstanceDataField* ins) {
  MDefinition* value = ins->value();
  size_t offs = wasm::Instance::offsetInData(ins->instanceDataOffset());
  if (value->type() == MIRType::Int64) {
#ifdef JS_PUNBOX64
    LAllocation instance = useRegisterAtStart(ins->instance());
    LInt64Allocation valueAlloc = useInt64RegisterAtStart(value);
#else
    LAllocation instance = useRegister(ins->instance());
    LInt64Allocation valueAlloc = useInt64Register(value);
#endif
    add(new (alloc())
            LWasmStoreSlotI64(valueAlloc, instance, offs, mozilla::Nothing()),
        ins);
  } else {
    MOZ_ASSERT(value->type() != MIRType::WasmAnyRef);
    LAllocation instance = useRegisterAtStart(ins->instance());
    LAllocation valueAlloc = useRegisterAtStart(value);
    add(new (alloc()) LWasmStoreSlot(valueAlloc, instance, offs, value->type(),
                                     MNarrowingOp::None, mozilla::Nothing()),
        ins);
  }
}

void LIRGenerator::visitWasmStoreGlobalCell(MWasmStoreGlobalCell* ins) {
  MDefinition* value = ins->value();
  size_t offs = 0;
  if (value->type() == MIRType::Int64) {
#ifdef JS_PUNBOX64
    LAllocation cellPtr = useRegisterAtStart(ins->cellPtr());
    LInt64Allocation valueAlloc = useInt64RegisterAtStart(value);
#else
    LAllocation cellPtr = useRegister(ins->cellPtr());
    LInt64Allocation valueAlloc = useInt64Register(value);
#endif
    add(new (alloc())
            LWasmStoreSlotI64(valueAlloc, cellPtr, offs, mozilla::Nothing()));
  } else {
    MOZ_ASSERT(value->type() != MIRType::WasmAnyRef);
    LAllocation cellPtr = useRegisterAtStart(ins->cellPtr());
    LAllocation valueAlloc = useRegisterAtStart(value);
    add(new (alloc()) LWasmStoreSlot(valueAlloc, cellPtr, offs, value->type(),
                                     MNarrowingOp::None, mozilla::Nothing()));
  }
}

void LIRGenerator::visitWasmStoreStackResult(MWasmStoreStackResult* ins) {
  MDefinition* stackResultArea = ins->stackResultArea();
  MDefinition* value = ins->value();
  size_t offs = ins->offset();
  LInstruction* lir;
  if (value->type() == MIRType::Int64) {
    lir = new (alloc())
        LWasmStoreSlotI64(useInt64Register(value), useRegister(stackResultArea),
                          offs, mozilla::Nothing());
  } else {
    MOZ_ASSERT(value->type() != MIRType::WasmAnyRef);
    lir = new (alloc())
        LWasmStoreSlot(useRegister(value), useRegister(stackResultArea), offs,
                       value->type(), MNarrowingOp::None, mozilla::Nothing());
  }
  add(lir, ins);
}

void LIRGenerator::visitWasmDerivedPointer(MWasmDerivedPointer* ins) {
  LAllocation base = useRegisterAtStart(ins->base());
  define(new (alloc()) LWasmDerivedPointer(base), ins);
}

void LIRGenerator::visitWasmDerivedIndexPointer(MWasmDerivedIndexPointer* ins) {
  LAllocation base = useRegisterAtStart(ins->base());
  LAllocation index = useRegisterAtStart(ins->index());
  define(new (alloc()) LWasmDerivedIndexPointer(base, index), ins);
}

void LIRGenerator::visitWasmStoreRef(MWasmStoreRef* ins) {
  LAllocation instance = useRegister(ins->instance());
  LAllocation valueBase = useFixed(ins->valueBase(), PreBarrierReg);
  LAllocation value = useRegister(ins->value());
  uint32_t valueOffset = ins->offset();
  add(new (alloc())
          LWasmStoreRef(instance, valueBase, value, temp(), valueOffset,
                        mozilla::Nothing(), ins->preBarrierKind()),
      ins);
}

void LIRGenerator::visitWasmPostWriteBarrierImmediate(
    MWasmPostWriteBarrierImmediate* ins) {
  LWasmPostWriteBarrierImmediate* lir =
      new (alloc()) LWasmPostWriteBarrierImmediate(
          useFixed(ins->instance(), InstanceReg), useRegister(ins->object()),
          useRegister(ins->valueBase()), useRegister(ins->value()), temp(),
          ins->valueOffset());
  add(lir, ins);
  assignWasmSafepoint(lir);
}

void LIRGenerator::visitWasmPostWriteBarrierIndex(
    MWasmPostWriteBarrierIndex* ins) {
  LWasmPostWriteBarrierIndex* lir = new (alloc()) LWasmPostWriteBarrierIndex(
      useFixed(ins->instance(), InstanceReg), useRegister(ins->object()),
      useRegister(ins->valueBase()), useRegister(ins->index()),
      useRegister(ins->value()), temp(), ins->elemSize());
  add(lir, ins);
  assignWasmSafepoint(lir);
}

void LIRGenerator::visitWasmParameter(MWasmParameter* ins) {
  ABIArg abi = ins->abi();
  if (ins->type() == MIRType::StackResults) {
    // Functions that return stack results receive an extra incoming parameter
    // with type MIRType::StackResults.  This value is a pointer to fresh
    // memory.  Here we treat it as if it were in fact MIRType::Pointer.
    auto* lir = new (alloc()) LWasmParameter;
    LDefinition def(LDefinition::TypeFrom(MIRType::Pointer),
                    LDefinition::FIXED);
    def.setOutput(abi.argInRegister() ? LAllocation(abi.reg())
                                      : LArgument(abi.offsetFromArgBase()));
    define(lir, ins, def);
    return;
  }
  if (abi.argInRegister()) {
#if defined(JS_NUNBOX32)
    if (abi.isGeneralRegPair()) {
      defineInt64Fixed(
          new (alloc()) LWasmParameterI64, ins,
          LInt64Allocation(LAllocation(AnyRegister(abi.gpr64().high)),
                           LAllocation(AnyRegister(abi.gpr64().low))));
      return;
    }
#endif
    defineFixed(new (alloc()) LWasmParameter, ins, LAllocation(abi.reg()));
    return;
  }
  if (ins->type() == MIRType::Int64) {
    MOZ_ASSERT(!abi.argInRegister());
    defineInt64Fixed(
        new (alloc()) LWasmParameterI64, ins,
#if defined(JS_NUNBOX32)
        LInt64Allocation(LArgument(abi.offsetFromArgBase() + INT64HIGH_OFFSET),
                         LArgument(abi.offsetFromArgBase() + INT64LOW_OFFSET))
#else
        LInt64Allocation(LArgument(abi.offsetFromArgBase()))
#endif
    );
  } else {
    MOZ_ASSERT(IsNumberType(ins->type()) || ins->type() == MIRType::WasmAnyRef
#ifdef ENABLE_WASM_SIMD
               || ins->type() == MIRType::Simd128
#endif
    );
    defineFixed(new (alloc()) LWasmParameter, ins,
                LArgument(abi.offsetFromArgBase()));
  }
}

void LIRGenerator::visitWasmReturn(MWasmReturn* ins) {
  MDefinition* rval = ins->getOperand(0);
  MDefinition* instance = ins->getOperand(1);

  if (rval->type() == MIRType::Int64) {
    add(new (alloc()) LWasmReturnI64(useInt64Fixed(rval, ReturnReg64),
                                     useFixed(instance, InstanceReg)));
    return;
  }

  LAllocation returnReg;
  if (rval->type() == MIRType::Float32) {
    returnReg = useFixed(rval, ReturnFloat32Reg);
  } else if (rval->type() == MIRType::Double) {
    returnReg = useFixed(rval, ReturnDoubleReg);
#ifdef ENABLE_WASM_SIMD
  } else if (rval->type() == MIRType::Simd128) {
    returnReg = useFixed(rval, ReturnSimd128Reg);
#endif
  } else if (rval->type() == MIRType::Int32 ||
             rval->type() == MIRType::WasmAnyRef) {
    returnReg = useFixed(rval, ReturnReg);
  } else {
    MOZ_CRASH("Unexpected wasm return type");
  }

  LWasmReturn* lir =
      new (alloc()) LWasmReturn(useFixed(instance, InstanceReg), returnReg);
  add(lir);
}

void LIRGenerator::visitWasmReturnVoid(MWasmReturnVoid* ins) {
  MDefinition* instance = ins->getOperand(0);
  LWasmReturnVoid* lir =
      new (alloc()) LWasmReturnVoid(useFixed(instance, InstanceReg));
  add(lir);
}

void LIRGenerator::visitWasmStackArg(MWasmStackArg* ins) {
  if (ins->arg()->type() == MIRType::Int64) {
    add(new (alloc())
            LWasmStackArgI64(useInt64RegisterOrConstantAtStart(ins->arg())),
        ins);
  } else if (IsFloatingPointType(ins->arg()->type())) {
    MOZ_ASSERT(!ins->arg()->isEmittedAtUses());
    add(new (alloc()) LWasmStackArg(useRegisterAtStart(ins->arg())), ins);
  } else {
    add(new (alloc()) LWasmStackArg(useRegisterOrConstantAtStart(ins->arg())),
        ins);
  }
}

void LIRGenerator::visitWasmRegisterResult(MWasmRegisterResult* ins) {
  auto* lir = new (alloc()) LWasmRegisterResult();
  uint32_t vreg = getVirtualRegister();
  MOZ_ASSERT(ins->type() != MIRType::Int64);
  auto type = LDefinition::TypeFrom(ins->type());
  lir->setDef(0, LDefinition(vreg, type, LGeneralReg(ins->loc())));
  ins->setVirtualRegister(vreg);
  add(lir, ins);
}

void LIRGenerator::visitWasmFloatRegisterResult(MWasmFloatRegisterResult* ins) {
  auto* lir = new (alloc()) LWasmRegisterResult();
  uint32_t vreg = getVirtualRegister();
  auto type = LDefinition::TypeFrom(ins->type());
  lir->setDef(0, LDefinition(vreg, type, LFloatReg(ins->loc())));
  ins->setVirtualRegister(vreg);
  add(lir, ins);
}

void LIRGenerator::visitWasmRegister64Result(MWasmRegister64Result* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Int64);
  uint32_t vreg = getVirtualRegister();

#if defined(JS_NUNBOX32)
  auto* lir = new (alloc()) LWasmRegisterPairResult();
  lir->setDef(INT64LOW_INDEX,
              LDefinition(vreg + INT64LOW_INDEX, LDefinition::GENERAL,
                          LGeneralReg(ins->loc().low)));
  lir->setDef(INT64HIGH_INDEX,
              LDefinition(vreg + INT64HIGH_INDEX, LDefinition::GENERAL,
                          LGeneralReg(ins->loc().high)));
  getVirtualRegister();
#elif defined(JS_PUNBOX64)
  auto* lir = new (alloc()) LWasmRegisterResult();
  lir->setDef(
      0, LDefinition(vreg, LDefinition::GENERAL, LGeneralReg(ins->loc().reg)));
#else
#  error expected either JS_NUNBOX32 or JS_PUNBOX64
#endif

  ins->setVirtualRegister(vreg);
  add(lir, ins);
}

void LIRGenerator::visitWasmStackResultArea(MWasmStackResultArea* ins) {
  MOZ_ASSERT(ins->type() == MIRType::StackResults);
  auto* lir = new (alloc()) LWasmStackResultArea(temp());
  uint32_t vreg = getVirtualRegister();
  lir->setDef(0,
              LDefinition(vreg, LDefinition::STACKRESULTS, LDefinition::STACK));
  ins->setVirtualRegister(vreg);
  add(lir, ins);
}

void LIRGenerator::visitWasmStackResult(MWasmStackResult* ins) {
  MWasmStackResultArea* area = ins->resultArea()->toWasmStackResultArea();
  LDefinition::Policy pol = LDefinition::STACK;

  if (ins->type() == MIRType::Int64) {
    auto* lir = new (alloc()) LWasmStackResult64;
    lir->setOperand(0, use(area, LUse(LUse::STACK, /* usedAtStart = */ true)));
    uint32_t vreg = getVirtualRegister();
    LDefinition::Type typ = LDefinition::GENERAL;
#if defined(JS_NUNBOX32)
    getVirtualRegister();
    lir->setDef(INT64LOW_INDEX, LDefinition(vreg + INT64LOW_INDEX, typ, pol));
    lir->setDef(INT64HIGH_INDEX, LDefinition(vreg + INT64HIGH_INDEX, typ, pol));
#else
    lir->setDef(0, LDefinition(vreg, typ, pol));
#endif
    ins->setVirtualRegister(vreg);
    add(lir, ins);
    return;
  }

  auto* lir = new (alloc()) LWasmStackResult;
  lir->setOperand(0, use(area, LUse(LUse::STACK, /* usedAtStart = */ true)));
  uint32_t vreg = getVirtualRegister();
  LDefinition::Type typ = LDefinition::TypeFrom(ins->type());
  lir->setDef(0, LDefinition(vreg, typ, pol));
  ins->setVirtualRegister(vreg);
  add(lir, ins);
}

void LIRGenerator::visitWasmStackSwitchToSuspendable(
    MWasmStackSwitchToSuspendable* ins) {
#ifdef ENABLE_WASM_JSPI
  auto* lir = new (alloc()) LWasmStackSwitchToSuspendable(
      useFixedAtStart(ins->suspender(), ABINonArgReg0),
      useFixedAtStart(ins->fn(), ABINonArgReg1),
      useFixedAtStart(ins->data(), ABINonArgReg2));

  add(lir, ins);
  assignWasmSafepoint(lir);
#else
  MOZ_CRASH("NYI");
#endif
}

void LIRGenerator::visitWasmStackSwitchToMain(MWasmStackSwitchToMain* ins) {
#ifdef ENABLE_WASM_JSPI
  auto* lir = new (alloc())
      LWasmStackSwitchToMain(useFixedAtStart(ins->suspender(), ABINonArgReg0),
                             useFixedAtStart(ins->fn(), ABINonArgReg1),
                             useFixedAtStart(ins->data(), ABINonArgReg2));

  add(lir, ins);
  assignWasmSafepoint(lir);
#else
  MOZ_CRASH("NYI");
#endif
}

void LIRGenerator::visitWasmStackContinueOnSuspendable(
    MWasmStackContinueOnSuspendable* ins) {
#ifdef ENABLE_WASM_JSPI
  auto* lir = new (alloc()) LWasmStackContinueOnSuspendable(
      useFixedAtStart(ins->suspender(), ABINonArgReg0));

  add(lir, ins);
  assignWasmSafepoint(lir);
#else
  MOZ_CRASH("NYI");
#endif
}

template <class MWasmCallT>
void LIRGenerator::visitWasmCall(MWasmCallT ins) {
  bool needsBoundsCheck = true;
  mozilla::Maybe<uint32_t> tableSize;

  if (ins->callee().isTable()) {
    MDefinition* index = ins->getOperand(ins->numArgs());

    if (ins->callee().which() == wasm::CalleeDesc::WasmTable) {
      uint32_t minLength = ins->callee().wasmTableMinLength();
      mozilla::Maybe<uint32_t> maxLength = ins->callee().wasmTableMaxLength();
      if (index->isConstant() &&
          uint32_t(index->toConstant()->toInt32()) < minLength) {
        needsBoundsCheck = false;
      }
      if (maxLength.isSome() && *maxLength == minLength) {
        tableSize = maxLength;
      }
    }
  }

  auto* lir = allocateVariadic<LWasmCall>(ins->numOperands(), needsBoundsCheck,
                                          tableSize);
  if (!lir) {
    abort(AbortReason::Alloc, "OOM: LIRGenerator::lowerWasmCall");
    return;
  }

  for (unsigned i = 0; i < ins->numArgs(); i++) {
    lir->setOperand(
        i, useFixedAtStart(ins->getOperand(i), ins->registerForArg(i)));
  }

  if (ins->callee().isTable()) {
    MDefinition* index = ins->getOperand(ins->numArgs());
    lir->setOperand(ins->numArgs(),
                    useFixedAtStart(index, WasmTableCallIndexReg));
  }
  if (ins->callee().isFuncRef()) {
    MDefinition* ref = ins->getOperand(ins->numArgs());
    lir->setOperand(ins->numArgs(), useFixedAtStart(ref, WasmCallRefReg));
  }

  add(lir, ins);
  assignWasmSafepoint(lir);

  // WasmCall with WasmTable has two call instructions, and they both need a
  // safepoint associated with them.  Create a second safepoint here; the node
  // otherwise does nothing, and codegen for it only marks the safepoint at the
  // node.
  if ((ins->callee().which() == wasm::CalleeDesc::WasmTable ||
       ins->callee().which() == wasm::CalleeDesc::FuncRef) &&
      !ins->isWasmReturnCall()) {
    auto* adjunctSafepoint = new (alloc()) LWasmCallIndirectAdjunctSafepoint();
    add(adjunctSafepoint);
    assignWasmSafepoint(adjunctSafepoint);
    lir->setAdjunctSafepoint(adjunctSafepoint);
  }
}

void LIRGenerator::visitWasmCallCatchable(MWasmCallCatchable* ins) {
  visitWasmCall(ins);
}

void LIRGenerator::visitWasmCallUncatchable(MWasmCallUncatchable* ins) {
  visitWasmCall(ins);
}

void LIRGenerator::visitWasmReturnCall(MWasmReturnCall* ins) {
  visitWasmCall(ins);
}

void LIRGenerator::visitWasmCallLandingPrePad(MWasmCallLandingPrePad* ins) {
  add(new (alloc()) LWasmCallLandingPrePad, ins);
}

void LIRGenerator::visitSetDOMProperty(MSetDOMProperty* ins) {
  MDefinition* val = ins->value();

  Register cxReg, objReg, privReg, valueReg;
  GetTempRegForIntArg(0, 0, &cxReg);
  GetTempRegForIntArg(1, 0, &objReg);
  GetTempRegForIntArg(2, 0, &privReg);
  GetTempRegForIntArg(3, 0, &valueReg);

  // Keep using GetTempRegForIntArg, since we want to make sure we
  // don't clobber registers we're already using.
  Register tempReg1, tempReg2;
  GetTempRegForIntArg(4, 0, &tempReg1);
  mozilla::DebugOnly<bool> ok = GetTempRegForIntArg(5, 0, &tempReg2);
  MOZ_ASSERT(ok, "How can we not have six temp registers?");

  LSetDOMProperty* lir = new (alloc())
      LSetDOMProperty(tempFixed(cxReg), useFixedAtStart(ins->object(), objReg),
                      useBoxFixedAtStart(val, tempReg1, tempReg2),
                      tempFixed(privReg), tempFixed(valueReg));
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitGetDOMProperty(MGetDOMProperty* ins) {
  Register cxReg, objReg, privReg, valueReg;
  GetTempRegForIntArg(0, 0, &cxReg);
  GetTempRegForIntArg(1, 0, &objReg);
  GetTempRegForIntArg(2, 0, &privReg);
  mozilla::DebugOnly<bool> ok = GetTempRegForIntArg(3, 0, &valueReg);
  MOZ_ASSERT(ok, "How can we not have four temp registers?");
  LGetDOMProperty* lir = new (alloc())
      LGetDOMProperty(tempFixed(cxReg), useFixedAtStart(ins->object(), objReg),
                      tempFixed(privReg), tempFixed(valueReg));

  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitGetDOMMember(MGetDOMMember* ins) {
  MOZ_ASSERT(ins->isDomMovable(), "Members had better be movable");
  // We wish we could assert that ins->domAliasSet() == JSJitInfo::AliasNone,
  // but some MGetDOMMembers are for [Pure], not [Constant] properties, whose
  // value can in fact change as a result of DOM setters and method calls.
  MOZ_ASSERT(ins->domAliasSet() != JSJitInfo::AliasEverything,
             "Member gets had better not alias the world");

  MDefinition* obj = ins->object();
  MOZ_ASSERT(obj->type() == MIRType::Object);

  MIRType type = ins->type();

  if (type == MIRType::Value) {
    LGetDOMMemberV* lir = new (alloc()) LGetDOMMemberV(useRegisterAtStart(obj));
    defineBox(lir, ins);
  } else {
    LGetDOMMemberT* lir =
        new (alloc()) LGetDOMMemberT(useRegisterForTypedLoad(obj, type));
    define(lir, ins);
  }
}

void LIRGenerator::visitLoadDOMExpandoValue(MLoadDOMExpandoValue* ins) {
  MOZ_ASSERT(ins->proxy()->type() == MIRType::Object);
  auto* lir =
      new (alloc()) LLoadDOMExpandoValue(useRegisterAtStart(ins->proxy()));
  defineBox(lir, ins);
}

void LIRGenerator::visitLoadDOMExpandoValueGuardGeneration(
    MLoadDOMExpandoValueGuardGeneration* ins) {
  MOZ_ASSERT(ins->proxy()->type() == MIRType::Object);
  auto* lir = new (alloc())
      LLoadDOMExpandoValueGuardGeneration(useRegisterAtStart(ins->proxy()));
  assignSnapshot(lir, ins->bailoutKind());
  defineBox(lir, ins);
}

void LIRGenerator::visitLoadDOMExpandoValueIgnoreGeneration(
    MLoadDOMExpandoValueIgnoreGeneration* ins) {
  MOZ_ASSERT(ins->proxy()->type() == MIRType::Object);
  auto* lir = new (alloc())
      LLoadDOMExpandoValueIgnoreGeneration(useRegisterAtStart(ins->proxy()));
  defineBox(lir, ins);
}

void LIRGenerator::visitGuardDOMExpandoMissingOrGuardShape(
    MGuardDOMExpandoMissingOrGuardShape* ins) {
  MOZ_ASSERT(ins->expando()->type() == MIRType::Value);
  auto* lir = new (alloc())
      LGuardDOMExpandoMissingOrGuardShape(useBox(ins->expando()), temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->expando());
}

void LIRGenerator::visitIncrementWarmUpCounter(MIncrementWarmUpCounter* ins) {
  LIncrementWarmUpCounter* lir = new (alloc()) LIncrementWarmUpCounter(temp());
  add(lir, ins);
}

void LIRGenerator::visitLexicalCheck(MLexicalCheck* ins) {
  MDefinition* input = ins->input();
  MOZ_ASSERT(input->type() == MIRType::Value);
  LLexicalCheck* lir = new (alloc()) LLexicalCheck(useBox(input));
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, input);
}

void LIRGenerator::visitThrowRuntimeLexicalError(
    MThrowRuntimeLexicalError* ins) {
  LThrowRuntimeLexicalError* lir = new (alloc()) LThrowRuntimeLexicalError();
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitThrowMsg(MThrowMsg* ins) {
  LThrowMsg* lir = new (alloc()) LThrowMsg();
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitGlobalDeclInstantiation(MGlobalDeclInstantiation* ins) {
  LGlobalDeclInstantiation* lir = new (alloc()) LGlobalDeclInstantiation();
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitDebugger(MDebugger* ins) {
  auto* lir = new (alloc()) LDebugger(tempFixed(CallTempReg0));
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
}

void LIRGenerator::visitAtomicIsLockFree(MAtomicIsLockFree* ins) {
  define(new (alloc()) LAtomicIsLockFree(useRegister(ins->input())), ins);
}

void LIRGenerator::visitCheckReturn(MCheckReturn* ins) {
  MDefinition* retVal = ins->returnValue();
  MDefinition* thisVal = ins->thisValue();
  MOZ_ASSERT(retVal->type() == MIRType::Value);
  MOZ_ASSERT(thisVal->type() == MIRType::Value);

  auto* lir =
      new (alloc()) LCheckReturn(useBoxAtStart(retVal), useBoxAtStart(thisVal));
  defineBox(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCheckIsObj(MCheckIsObj* ins) {
  MDefinition* input = ins->input();
  MOZ_ASSERT(input->type() == MIRType::Value);

  LCheckIsObj* lir = new (alloc()) LCheckIsObj(useBox(input));
  define(lir, ins);
  assignSafepoint(lir, ins);
}

#ifdef JS_PUNBOX64
void LIRGenerator::visitCheckScriptedProxyGetResult(
    MCheckScriptedProxyGetResult* ins) {
  MDefinition* target = ins->target();
  MDefinition* id = ins->id();
  MDefinition* value = ins->value();

  LCheckScriptedProxyGetResult* lir =
      new (alloc()) LCheckScriptedProxyGetResult(useBox(target), useBox(id),
                                                 useBox(value), temp(), temp());
  add(lir, ins);
  assignSafepoint(lir, ins);
}
#endif

void LIRGenerator::visitCheckObjCoercible(MCheckObjCoercible* ins) {
  MDefinition* checkVal = ins->checkValue();
  MOZ_ASSERT(checkVal->type() == MIRType::Value);

  auto* lir = new (alloc()) LCheckObjCoercible(useBoxAtStart(checkVal));
  redefine(ins, checkVal);
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCheckClassHeritage(MCheckClassHeritage* ins) {
  MDefinition* heritage = ins->heritage();
  MOZ_ASSERT(heritage->type() == MIRType::Value);

  auto* lir =
      new (alloc()) LCheckClassHeritage(useBox(heritage), temp(), temp());
  redefine(ins, heritage);
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCheckThis(MCheckThis* ins) {
  MDefinition* thisValue = ins->thisValue();
  MOZ_ASSERT(thisValue->type() == MIRType::Value);

  auto* lir = new (alloc()) LCheckThis(useBoxAtStart(thisValue));
  redefine(ins, thisValue);
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCheckThisReinit(MCheckThisReinit* ins) {
  MDefinition* thisValue = ins->thisValue();
  MOZ_ASSERT(thisValue->type() == MIRType::Value);

  auto* lir = new (alloc()) LCheckThisReinit(useBoxAtStart(thisValue));
  redefine(ins, thisValue);
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitGenerator(MGenerator* ins) {
  auto* lir =
      new (alloc()) LGenerator(useRegisterAtStart(ins->callee()),
                               useRegisterAtStart(ins->environmentChain()),
                               useRegisterAtStart(ins->argsObject()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitAsyncResolve(MAsyncResolve* ins) {
  auto* lir = new (alloc()) LAsyncResolve(useRegisterAtStart(ins->generator()),
                                          useBoxAtStart(ins->value()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitAsyncReject(MAsyncReject* ins) {
  auto* lir = new (alloc())
      LAsyncReject(useRegisterAtStart(ins->generator()),
                   useBoxAtStart(ins->reason()), useBoxAtStart(ins->stack()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitAsyncAwait(MAsyncAwait* ins) {
  MOZ_ASSERT(ins->generator()->type() == MIRType::Object);
  auto* lir = new (alloc()) LAsyncAwait(useBoxAtStart(ins->value()),
                                        useRegisterAtStart(ins->generator()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCanSkipAwait(MCanSkipAwait* ins) {
  auto* lir = new (alloc()) LCanSkipAwait(useBoxAtStart(ins->value()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitMaybeExtractAwaitValue(MMaybeExtractAwaitValue* ins) {
  auto* lir = new (alloc()) LMaybeExtractAwaitValue(
      useBoxAtStart(ins->value()), useRegisterAtStart(ins->canSkip()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitDebugCheckSelfHosted(MDebugCheckSelfHosted* ins) {
  MDefinition* checkVal = ins->checkValue();
  MOZ_ASSERT(checkVal->type() == MIRType::Value);

  LDebugCheckSelfHosted* lir =
      new (alloc()) LDebugCheckSelfHosted(useBoxAtStart(checkVal));
  redefine(ins, checkVal);
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitIsPackedArray(MIsPackedArray* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::Boolean);

  auto lir = new (alloc()) LIsPackedArray(useRegister(ins->object()), temp());
  define(lir, ins);
}

void LIRGenerator::visitGuardArrayIsPacked(MGuardArrayIsPacked* ins) {
  MOZ_ASSERT(ins->array()->type() == MIRType::Object);

  auto* lir = new (alloc())
      LGuardArrayIsPacked(useRegister(ins->array()), temp(), temp());
  assignSnapshot(lir, ins->bailoutKind());
  add(lir, ins);
  redefine(ins, ins->array());
}

void LIRGenerator::visitGetPrototypeOf(MGetPrototypeOf* ins) {
  MOZ_ASSERT(ins->target()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::Value);

  auto lir = new (alloc()) LGetPrototypeOf(useRegister(ins->target()));
  defineBox(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitObjectWithProto(MObjectWithProto* ins) {
  MOZ_ASSERT(ins->prototype()->type() == MIRType::Value);
  MOZ_ASSERT(ins->type() == MIRType::Object);

  auto* lir = new (alloc()) LObjectWithProto(useBoxAtStart(ins->prototype()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitObjectStaticProto(MObjectStaticProto* ins) {
  MOZ_ASSERT(ins->object()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::Object);

  auto* lir =
      new (alloc()) LObjectStaticProto(useRegisterAtStart(ins->object()));
  define(lir, ins);
};

void LIRGenerator::visitBuiltinObject(MBuiltinObject* ins) {
  MOZ_ASSERT(ins->type() == MIRType::Object);

  auto* lir = new (alloc()) LBuiltinObject();
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitReturn(MReturn* ret) {
  return visitReturnImpl(ret->getOperand(0));
}

void LIRGenerator::visitGeneratorReturn(MGeneratorReturn* ret) {
  return visitReturnImpl(ret->getOperand(0), true);
}

void LIRGenerator::visitSuperFunction(MSuperFunction* ins) {
  MOZ_ASSERT(ins->callee()->type() == MIRType::Object);
  MOZ_ASSERT(ins->type() == MIRType::Value);

  auto* lir = new (alloc()) LSuperFunction(useRegister(ins->callee()), temp());
  defineBox(lir, ins);
}

void LIRGenerator::visitInitHomeObject(MInitHomeObject* ins) {
  MDefinition* function = ins->function();
  MOZ_ASSERT(function->type() == MIRType::Object);

  MDefinition* homeObject = ins->homeObject();
  MOZ_ASSERT(homeObject->type() == MIRType::Value);

  MOZ_ASSERT(ins->type() == MIRType::Object);

  auto* lir = new (alloc())
      LInitHomeObject(useRegisterAtStart(function), useBoxAtStart(homeObject));
  redefine(ins, function);
  add(lir, ins);
}

void LIRGenerator::visitIsTypedArrayConstructor(MIsTypedArrayConstructor* ins) {
  MDefinition* object = ins->object();
  MOZ_ASSERT(object->type() == MIRType::Object);

  auto* lir = new (alloc()) LIsTypedArrayConstructor(useRegister(object));
  define(lir, ins);
}

void LIRGenerator::visitLoadValueTag(MLoadValueTag* ins) {
  MDefinition* value = ins->value();
  MOZ_ASSERT(value->type() == MIRType::Value);

  define(new (alloc()) LLoadValueTag(useBoxAtStart(value)), ins);
}

void LIRGenerator::visitGuardTagNotEqual(MGuardTagNotEqual* ins) {
  MDefinition* lhs = ins->lhs();
  MOZ_ASSERT(lhs->type() == MIRType::Int32);

  MDefinition* rhs = ins->rhs();
  MOZ_ASSERT(rhs->type() == MIRType::Int32);

  auto* guard =
      new (alloc()) LGuardTagNotEqual(useRegister(lhs), useRegister(rhs));
  assignSnapshot(guard, ins->bailoutKind());
  add(guard, ins);
}

void LIRGenerator::visitLoadWrapperTarget(MLoadWrapperTarget* ins) {
  MDefinition* object = ins->object();
  MOZ_ASSERT(object->type() == MIRType::Object);

  auto* lir = new (alloc()) LLoadWrapperTarget(useRegisterAtStart(object));
  if (ins->fallible()) {
    assignSnapshot(lir, ins->bailoutKind());
  }
  define(lir, ins);
}

void LIRGenerator::visitGuardHasGetterSetter(MGuardHasGetterSetter* ins) {
  MDefinition* object = ins->object();
  MOZ_ASSERT(object->type() == MIRType::Object);

  auto* guard = new (alloc())
      LGuardHasGetterSetter(useRegisterAtStart(object), tempFixed(CallTempReg0),
                            tempFixed(CallTempReg1), tempFixed(CallTempReg2));
  assignSnapshot(guard, ins->bailoutKind());
  add(guard, ins);
  redefine(ins, object);
}

void LIRGenerator::visitGuardIsExtensible(MGuardIsExtensible* ins) {
  MDefinition* object = ins->object();
  MOZ_ASSERT(object->type() == MIRType::Object);

  auto* guard = new (alloc()) LGuardIsExtensible(useRegister(object), temp());
  assignSnapshot(guard, ins->bailoutKind());
  add(guard, ins);
  redefine(ins, object);
}

void LIRGenerator::visitGuardInt32IsNonNegative(MGuardInt32IsNonNegative* ins) {
  MDefinition* index = ins->index();
  MOZ_ASSERT(index->type() == MIRType::Int32);

  auto* guard = new (alloc()) LGuardInt32IsNonNegative(useRegister(index));
  assignSnapshot(guard, ins->bailoutKind());
  add(guard, ins);
  redefine(ins, index);
}

void LIRGenerator::visitGuardInt32Range(MGuardInt32Range* ins) {
  MDefinition* input = ins->input();
  MOZ_ASSERT(input->type() == MIRType::Int32);

  auto* guard = new (alloc()) LGuardInt32Range(useRegister(input));
  assignSnapshot(guard, ins->bailoutKind());
  add(guard, ins);
  redefine(ins, input);
}

void LIRGenerator::visitGuardIndexIsNotDenseElement(
    MGuardIndexIsNotDenseElement* ins) {
  MDefinition* object = ins->object();
  MOZ_ASSERT(object->type() == MIRType::Object);

  MDefinition* index = ins->index();
  MOZ_ASSERT(index->type() == MIRType::Int32);

  LDefinition spectreTemp =
      BoundsCheckNeedsSpectreTemp() ? temp() : LDefinition::BogusTemp();

  auto* guard = new (alloc()) LGuardIndexIsNotDenseElement(
      useRegister(object), useRegister(index), temp(), spectreTemp);
  assignSnapshot(guard, ins->bailoutKind());
  add(guard, ins);
  redefine(ins, index);
}

void LIRGenerator::visitGuardIndexIsValidUpdateOrAdd(
    MGuardIndexIsValidUpdateOrAdd* ins) {
  MDefinition* object = ins->object();
  MOZ_ASSERT(object->type() == MIRType::Object);

  MDefinition* index = ins->index();
  MOZ_ASSERT(index->type() == MIRType::Int32);

  LDefinition spectreTemp =
      BoundsCheckNeedsSpectreTemp() ? temp() : LDefinition::BogusTemp();

  auto* guard = new (alloc()) LGuardIndexIsValidUpdateOrAdd(
      useRegister(object), useRegister(index), temp(), spectreTemp);
  assignSnapshot(guard, ins->bailoutKind());
  add(guard, ins);
  redefine(ins, index);
}

void LIRGenerator::visitCallAddOrUpdateSparseElement(
    MCallAddOrUpdateSparseElement* ins) {
  MDefinition* object = ins->object();
  MOZ_ASSERT(object->type() == MIRType::Object);

  MDefinition* index = ins->index();
  MOZ_ASSERT(index->type() == MIRType::Int32);

  MDefinition* value = ins->value();
  MOZ_ASSERT(value->type() == MIRType::Value);

  auto* lir = new (alloc()) LCallAddOrUpdateSparseElement(
      useRegisterAtStart(object), useRegisterAtStart(index),
      useBoxAtStart(value));
  add(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCallGetSparseElement(MCallGetSparseElement* ins) {
  MDefinition* object = ins->object();
  MOZ_ASSERT(object->type() == MIRType::Object);

  MDefinition* index = ins->index();
  MOZ_ASSERT(index->type() == MIRType::Int32);

  auto* lir = new (alloc()) LCallGetSparseElement(useRegisterAtStart(object),
                                                  useRegisterAtStart(index));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCallNativeGetElement(MCallNativeGetElement* ins) {
  MDefinition* object = ins->object();
  MOZ_ASSERT(object->type() == MIRType::Object);

  MDefinition* index = ins->index();
  MOZ_ASSERT(index->type() == MIRType::Int32);

  auto* lir = new (alloc()) LCallNativeGetElement(useRegisterAtStart(object),
                                                  useRegisterAtStart(index));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCallNativeGetElementSuper(
    MCallNativeGetElementSuper* ins) {
  MDefinition* object = ins->object();
  MOZ_ASSERT(object->type() == MIRType::Object);

  MDefinition* index = ins->index();
  MOZ_ASSERT(index->type() == MIRType::Int32);

  MDefinition* receiver = ins->receiver();

  auto* lir = new (alloc()) LCallNativeGetElementSuper(
      useRegisterAtStart(object), useRegisterAtStart(index),
      useBoxAtStart(receiver));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitCallObjectHasSparseElement(
    MCallObjectHasSparseElement* ins) {
  MDefinition* object = ins->object();
  MOZ_ASSERT(object->type() == MIRType::Object);

  MDefinition* index = ins->index();
  MOZ_ASSERT(index->type() == MIRType::Int32);

  auto* lir = new (alloc()) LCallObjectHasSparseElement(
      useRegisterAtStart(object), useRegisterAtStart(index),
      tempFixed(CallTempReg0), tempFixed(CallTempReg1));
  assignSnapshot(lir, ins->bailoutKind());
  defineReturn(lir, ins);
}

void LIRGenerator::visitBigIntAsIntN(MBigIntAsIntN* ins) {
  MOZ_ASSERT(ins->bits()->type() == MIRType::Int32);
  MOZ_ASSERT(ins->input()->type() == MIRType::BigInt);

  if (ins->bits()->isConstant()) {
    int32_t bits = ins->bits()->toConstant()->toInt32();
    if (bits == 64) {
      auto* lir = new (alloc())
          LBigIntAsIntN64(useRegister(ins->input()), temp(), tempInt64());
      define(lir, ins);
      assignSafepoint(lir, ins);
      return;
    }
    if (bits == 32) {
      auto* lir = new (alloc())
          LBigIntAsIntN32(useRegister(ins->input()), temp(), tempInt64());
      define(lir, ins);
      assignSafepoint(lir, ins);
      return;
    }
  }

  auto* lir = new (alloc()) LBigIntAsIntN(useRegisterAtStart(ins->bits()),
                                          useRegisterAtStart(ins->input()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitBigIntAsUintN(MBigIntAsUintN* ins) {
  MOZ_ASSERT(ins->bits()->type() == MIRType::Int32);
  MOZ_ASSERT(ins->input()->type() == MIRType::BigInt);

  if (ins->bits()->isConstant()) {
    int32_t bits = ins->bits()->toConstant()->toInt32();
    if (bits == 64) {
      auto* lir = new (alloc())
          LBigIntAsUintN64(useRegister(ins->input()), temp(), tempInt64());
      define(lir, ins);
      assignSafepoint(lir, ins);
      return;
    }
    if (bits == 32) {
      auto* lir = new (alloc())
          LBigIntAsUintN32(useRegister(ins->input()), temp(), tempInt64());
      define(lir, ins);
      assignSafepoint(lir, ins);
      return;
    }
  }

  auto* lir = new (alloc()) LBigIntAsUintN(useRegisterAtStart(ins->bits()),
                                           useRegisterAtStart(ins->input()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitGuardNonGCThing(MGuardNonGCThing* ins) {
  MDefinition* input = ins->input();

  auto* guard = new (alloc()) LGuardNonGCThing(useBox(input));
  assignSnapshot(guard, ins->bailoutKind());
  add(guard, ins);
  redefine(ins, input);
}

void LIRGenerator::visitToHashableNonGCThing(MToHashableNonGCThing* ins) {
  auto* lir =
      new (alloc()) LToHashableNonGCThing(useBox(ins->input()), tempDouble());
  defineBox(lir, ins);
}

void LIRGenerator::visitToHashableString(MToHashableString* ins) {
  auto* lir = new (alloc()) LToHashableString(useRegister(ins->input()));
  define(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitToHashableValue(MToHashableValue* ins) {
  auto* lir =
      new (alloc()) LToHashableValue(useBox(ins->input()), tempDouble());
  defineBox(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitHashNonGCThing(MHashNonGCThing* ins) {
  auto* lir = new (alloc()) LHashNonGCThing(useBox(ins->input()), temp());
  define(lir, ins);
}

void LIRGenerator::visitHashString(MHashString* ins) {
  auto* lir = new (alloc()) LHashString(useRegister(ins->input()), temp());
  define(lir, ins);
}

void LIRGenerator::visitHashSymbol(MHashSymbol* ins) {
  auto* lir = new (alloc()) LHashSymbol(useRegister(ins->input()));
  define(lir, ins);
}

void LIRGenerator::visitHashBigInt(MHashBigInt* ins) {
  auto* lir = new (alloc())
      LHashBigInt(useRegister(ins->input()), temp(), temp(), temp());
  define(lir, ins);
}

void LIRGenerator::visitHashObject(MHashObject* ins) {
  auto* lir =
      new (alloc()) LHashObject(useRegister(ins->set()), useBox(ins->input()),
                                temp(), temp(), temp(), temp());
  define(lir, ins);
}

void LIRGenerator::visitHashValue(MHashValue* ins) {
  auto* lir =
      new (alloc()) LHashValue(useRegister(ins->set()), useBox(ins->input()),
                               temp(), temp(), temp(), temp());
  define(lir, ins);
}

void LIRGenerator::visitSetObjectHasNonBigInt(MSetObjectHasNonBigInt* ins) {
  auto* lir = new (alloc())
      LSetObjectHasNonBigInt(useRegister(ins->set()), useBox(ins->value()),
                             useRegister(ins->hash()), temp(), temp());
  define(lir, ins);
}

void LIRGenerator::visitSetObjectHasBigInt(MSetObjectHasBigInt* ins) {
  auto* lir = new (alloc()) LSetObjectHasBigInt(
      useRegister(ins->set()), useBox(ins->value()), useRegister(ins->hash()),
      temp(), temp(), temp(), temp());
  define(lir, ins);
}

void LIRGenerator::visitSetObjectHasValue(MSetObjectHasValue* ins) {
  auto* lir = new (alloc()) LSetObjectHasValue(
      useRegister(ins->set()), useBox(ins->value()), useRegister(ins->hash()),
      temp(), temp(), temp(), temp());
  define(lir, ins);
}

void LIRGenerator::visitSetObjectHasValueVMCall(MSetObjectHasValueVMCall* ins) {
  auto* lir = new (alloc()) LSetObjectHasValueVMCall(
      useRegisterAtStart(ins->set()), useBoxAtStart(ins->value()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitSetObjectSize(MSetObjectSize* ins) {
  auto* lir = new (alloc()) LSetObjectSize(useRegisterAtStart(ins->set()));
  define(lir, ins);
}

void LIRGenerator::visitMapObjectHasNonBigInt(MMapObjectHasNonBigInt* ins) {
  auto* lir = new (alloc())
      LMapObjectHasNonBigInt(useRegister(ins->map()), useBox(ins->value()),
                             useRegister(ins->hash()), temp(), temp());
  define(lir, ins);
}

void LIRGenerator::visitMapObjectHasBigInt(MMapObjectHasBigInt* ins) {
  auto* lir = new (alloc()) LMapObjectHasBigInt(
      useRegister(ins->map()), useBox(ins->value()), useRegister(ins->hash()),
      temp(), temp(), temp(), temp());
  define(lir, ins);
}

void LIRGenerator::visitMapObjectHasValue(MMapObjectHasValue* ins) {
  auto* lir = new (alloc()) LMapObjectHasValue(
      useRegister(ins->map()), useBox(ins->value()), useRegister(ins->hash()),
      temp(), temp(), temp(), temp());
  define(lir, ins);
}

void LIRGenerator::visitMapObjectHasValueVMCall(MMapObjectHasValueVMCall* ins) {
  auto* lir = new (alloc()) LMapObjectHasValueVMCall(
      useRegisterAtStart(ins->map()), useBoxAtStart(ins->value()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitMapObjectGetNonBigInt(MMapObjectGetNonBigInt* ins) {
  auto* lir = new (alloc())
      LMapObjectGetNonBigInt(useRegister(ins->map()), useBox(ins->value()),
                             useRegister(ins->hash()), temp(), temp());
  defineBox(lir, ins);
}

void LIRGenerator::visitMapObjectGetBigInt(MMapObjectGetBigInt* ins) {
  auto* lir = new (alloc()) LMapObjectGetBigInt(
      useRegister(ins->map()), useBox(ins->value()), useRegister(ins->hash()),
      temp(), temp(), temp(), temp());
  defineBox(lir, ins);
}

void LIRGenerator::visitMapObjectGetValue(MMapObjectGetValue* ins) {
  auto* lir = new (alloc()) LMapObjectGetValue(
      useRegister(ins->map()), useBox(ins->value()), useRegister(ins->hash()),
      temp(), temp(), temp(), temp());
  defineBox(lir, ins);
}

void LIRGenerator::visitMapObjectGetValueVMCall(MMapObjectGetValueVMCall* ins) {
  auto* lir = new (alloc()) LMapObjectGetValueVMCall(
      useRegisterAtStart(ins->map()), useBoxAtStart(ins->value()));
  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitMapObjectSize(MMapObjectSize* ins) {
  auto* lir = new (alloc()) LMapObjectSize(useRegisterAtStart(ins->map()));
  define(lir, ins);
}

void LIRGenerator::visitPostIntPtrConversion(MPostIntPtrConversion* ins) {
  // This operation is a no-op.
  redefine(ins, ins->input());
}

void LIRGenerator::visitConstant(MConstant* ins) {
  if (!IsFloatingPointType(ins->type()) && ins->canEmitAtUses()) {
    emitAtUses(ins);
    return;
  }

  switch (ins->type()) {
    case MIRType::Double:
      define(new (alloc()) LDouble(ins->toDouble()), ins);
      break;
    case MIRType::Float32:
      define(new (alloc()) LFloat32(ins->toFloat32()), ins);
      break;
    case MIRType::Boolean:
      define(new (alloc()) LInteger(ins->toBoolean()), ins);
      break;
    case MIRType::Int32:
      define(new (alloc()) LInteger(ins->toInt32()), ins);
      break;
    case MIRType::Int64:
      defineInt64(new (alloc()) LInteger64(ins->toInt64()), ins);
      break;
    case MIRType::IntPtr:
#ifdef JS_64BIT
      defineInt64(new (alloc()) LInteger64(ins->toIntPtr()), ins);
#else
      define(new (alloc()) LInteger(ins->toIntPtr()), ins);
#endif
      break;
    case MIRType::String:
      define(new (alloc()) LPointer(ins->toString()), ins);
      break;
    case MIRType::Symbol:
      define(new (alloc()) LPointer(ins->toSymbol()), ins);
      break;
    case MIRType::BigInt:
      define(new (alloc()) LPointer(ins->toBigInt()), ins);
      break;
    case MIRType::Object:
      define(new (alloc()) LPointer(&ins->toObject()), ins);
      break;
    case MIRType::Shape:
      MOZ_ASSERT(ins->isEmittedAtUses());
      break;
    default:
      // Constants of special types (undefined, null) should never flow into
      // here directly. Operations blindly consuming them require a Box.
      MOZ_CRASH("unexpected constant type");
  }
}

void LIRGenerator::visitConstantProto(MConstantProto* ins) {
  JSObject* obj = &ins->protoObject()->toConstant()->toObject();
  define(new (alloc()) LPointer(obj), ins);
}

void LIRGenerator::visitWasmNullConstant(MWasmNullConstant* ins) {
  define(new (alloc()) LWasmNullConstant(), ins);
}

void LIRGenerator::visitWasmFloatConstant(MWasmFloatConstant* ins) {
  switch (ins->type()) {
    case MIRType::Double:
      define(new (alloc()) LDouble(ins->toDouble()), ins);
      break;
    case MIRType::Float32:
      define(new (alloc()) LFloat32(ins->toFloat32()), ins);
      break;
#ifdef ENABLE_WASM_SIMD
    case MIRType::Simd128:
      define(new (alloc()) LSimd128(ins->toSimd128()), ins);
      break;
#endif
    default:
      MOZ_CRASH("unexpected constant type");
  }
}

#ifdef JS_JITSPEW
static void SpewResumePoint(MBasicBlock* block, MInstruction* ins,
                            MResumePoint* resumePoint) {
  Fprinter& out = JitSpewPrinter();
  out.printf("Current resume point %p details:\n", (void*)resumePoint);
  out.printf("    frame count: %u\n", resumePoint->frameCount());

  if (ins) {
    out.printf("    taken after: ");
    ins->printName(out);
  } else {
    out.printf("    taken at block %u entry", block->id());
  }
  out.printf("\n");

  out.printf("    pc: %p (script: %p, offset: %d)\n", (void*)resumePoint->pc(),
             (void*)resumePoint->block()->info().script(),
             int(resumePoint->block()->info().script()->pcToOffset(
                 resumePoint->pc())));

  for (size_t i = 0, e = resumePoint->numOperands(); i < e; i++) {
    MDefinition* in = resumePoint->getOperand(i);
    out.printf("    slot%u: ", (unsigned)i);
    in->printName(out);
    out.printf("\n");
  }
}
#endif

void LIRGenerator::visitInstructionDispatch(MInstruction* ins) {
#ifdef JS_CODEGEN_NONE
  // Don't compile the switch-statement below so that we don't have to define
  // the platform-specific visit* methods for the none-backend.
  MOZ_CRASH();
#else
  switch (ins->op()) {
#  define MIR_OP(op)              \
    case MDefinition::Opcode::op: \
      visit##op(ins->to##op());   \
      break;
    MIR_OPCODE_LIST(MIR_OP)
#  undef MIR_OP
    default:
      MOZ_CRASH("Invalid instruction");
  }
#endif
}

void LIRGeneratorShared::visitEmittedAtUses(MInstruction* ins) {
  static_cast<LIRGenerator*>(this)->visitInstructionDispatch(ins);
}

bool LIRGenerator::visitInstruction(MInstruction* ins) {
  MOZ_ASSERT(!errored());

  if (ins->isRecoveredOnBailout()) {
    MOZ_ASSERT(!JitOptions.disableRecoverIns);
    return true;
  }

  if (!gen->ensureBallast()) {
    return false;
  }
  visitInstructionDispatch(ins);

  if (ins->resumePoint()) {
    updateResumeState(ins);
  }

#ifdef DEBUG
  ins->setInWorklistUnchecked();
#endif

  // If no safepoint was created, there's no need for an OSI point.
  if (LOsiPoint* osiPoint = popOsiPoint()) {
    add(osiPoint);
  }

  return !errored();
}

bool LIRGenerator::definePhis() {
  size_t lirIndex = 0;
  MBasicBlock* block = current->mir();
  for (MPhiIterator phi(block->phisBegin()); phi != block->phisEnd(); phi++) {
    if (phi->type() == MIRType::Value) {
      defineUntypedPhi(*phi, lirIndex);
      lirIndex += BOX_PIECES;
    } else if (phi->type() == MIRType::Int64) {
      defineInt64Phi(*phi, lirIndex);
      lirIndex += INT64_PIECES;
    } else {
      defineTypedPhi(*phi, lirIndex);
      lirIndex += 1;
    }
  }
  return !errored();
}

void LIRGenerator::updateResumeState(MInstruction* ins) {
  lastResumePoint_ = ins->resumePoint();
#ifdef JS_JITSPEW
  if (JitSpewEnabled(JitSpew_IonSnapshots) && lastResumePoint_) {
    SpewResumePoint(nullptr, ins, lastResumePoint_);
  }
#endif
}

void LIRGenerator::updateResumeState(MBasicBlock* block) {
  // Note: RangeAnalysis can flag blocks as unreachable, but they are only
  // removed iff GVN (including UCE) is enabled.
  MOZ_ASSERT_IF(!mir()->compilingWasm() && !block->unreachable(),
                block->entryResumePoint());
  MOZ_ASSERT_IF(block->unreachable(), !mir()->optimizationInfo().gvnEnabled());
  lastResumePoint_ = block->entryResumePoint();
#ifdef JS_JITSPEW
  if (JitSpewEnabled(JitSpew_IonSnapshots) && lastResumePoint_) {
    SpewResumePoint(block, nullptr, lastResumePoint_);
  }
#endif
}

bool LIRGenerator::visitBlock(MBasicBlock* block) {
  current = block->lir();
  updateResumeState(block);

  if (!definePhis()) {
    return false;
  }

  MOZ_ASSERT_IF(block->unreachable(), !mir()->optimizationInfo().gvnEnabled());
  for (MInstructionIterator iter = block->begin(); *iter != block->lastIns();
       iter++) {
    if (!visitInstruction(*iter)) {
      return false;
    }
  }

  if (block->successorWithPhis()) {
    // If we have a successor with phis, lower the phi input now that we
    // are approaching the join point.
    MBasicBlock* successor = block->successorWithPhis();
    uint32_t position = block->positionInPhiSuccessor();
    size_t lirIndex = 0;
    for (MPhiIterator phi(successor->phisBegin()); phi != successor->phisEnd();
         phi++) {
      if (!gen->ensureBallast()) {
        return false;
      }

      MDefinition* opd = phi->getOperand(position);
      ensureDefined(opd);

      MOZ_ASSERT(opd->type() == phi->type());

      if (phi->type() == MIRType::Value) {
        lowerUntypedPhiInput(*phi, position, successor->lir(), lirIndex);
        lirIndex += BOX_PIECES;
      } else if (phi->type() == MIRType::Int64) {
        lowerInt64PhiInput(*phi, position, successor->lir(), lirIndex);
        lirIndex += INT64_PIECES;
      } else {
        lowerTypedPhiInput(*phi, position, successor->lir(), lirIndex);
        lirIndex += 1;
      }
    }
  }

  // Now emit the last instruction, which is some form of branch.
  if (!visitInstruction(block->lastIns())) {
    return false;
  }

  return true;
}

void LIRGenerator::visitNaNToZero(MNaNToZero* ins) {
  MDefinition* input = ins->input();

  if (ins->operandIsNeverNaN() && ins->operandIsNeverNegativeZero()) {
    redefine(ins, input);
    return;
  }
  LNaNToZero* lir =
      new (alloc()) LNaNToZero(useRegisterAtStart(input), tempDouble());
  defineReuseInput(lir, ins, 0);
}

bool LIRGenerator::generate() {
  // Create all blocks and prep all phis beforehand.
  for (ReversePostorderIterator block(graph.rpoBegin());
       block != graph.rpoEnd(); block++) {
    if (gen->shouldCancel("Lowering (preparation loop)")) {
      return false;
    }

    if (!lirGraph_.initBlock(*block)) {
      return false;
    }
  }

  for (ReversePostorderIterator block(graph.rpoBegin());
       block != graph.rpoEnd(); block++) {
    if (gen->shouldCancel("Lowering (main loop)")) {
      return false;
    }

    if (!visitBlock(*block)) {
      return false;
    }
  }

  lirGraph_.setArgumentSlotCount(maxargslots_);
  return true;
}

void LIRGenerator::visitPhi(MPhi* phi) {
  // Phi nodes are not lowered because they are only meaningful for the register
  // allocator.
  MOZ_CRASH("Unexpected Phi node during Lowering.");
}

void LIRGenerator::visitBeta(MBeta* beta) {
  // Beta nodes are supposed to be removed before because they are
  // only used to carry the range information for Range analysis
  MOZ_CRASH("Unexpected Beta node during Lowering.");
}

void LIRGenerator::visitObjectState(MObjectState* objState) {
  // ObjectState nodes are always recovered on bailouts
  MOZ_CRASH("Unexpected ObjectState node during Lowering.");
}

void LIRGenerator::visitArrayState(MArrayState* objState) {
  // ArrayState nodes are always recovered on bailouts
  MOZ_CRASH("Unexpected ArrayState node during Lowering.");
}

void LIRGenerator::visitIonToWasmCall(MIonToWasmCall* ins) {
  // The instruction needs a temp register:
  // - that's not the FramePointer, since wasm is going to use it in the
  // function.
  // - that's not aliasing an input register.
  LDefinition scratch = tempFixed(ABINonArgReg0);

  // Note that since this is a LIR call instruction, regalloc will prevent
  // the use*AtStart below from reusing any of the temporaries.

  LInstruction* lir;
  if (ins->type() == MIRType::Value) {
    lir = allocateVariadic<LIonToWasmCallV>(ins->numOperands(), scratch);
  } else if (ins->type() == MIRType::Int64) {
    lir = allocateVariadic<LIonToWasmCallI64>(ins->numOperands(), scratch);
  } else {
    lir = allocateVariadic<LIonToWasmCall>(ins->numOperands(), scratch);
  }
  if (!lir) {
    abort(AbortReason::Alloc, "OOM: LIRGenerator::visitIonToWasmCall");
    return;
  }

  ABIArgGenerator abi;
  for (unsigned i = 0; i < ins->numOperands(); i++) {
    MDefinition* argDef = ins->getOperand(i);
    ABIArg arg = abi.next(ToMIRType(argDef->type()));
    switch (arg.kind()) {
      case ABIArg::GPR:
      case ABIArg::FPU:
        lir->setOperand(i, useFixedAtStart(argDef, arg.reg()));
        break;
      case ABIArg::Stack:
        lir->setOperand(i, useAtStart(argDef));
        break;
#ifdef JS_CODEGEN_REGISTER_PAIR
      case ABIArg::GPR_PAIR:
        MOZ_CRASH(
            "no way to pass i64, and wasm uses hardfp for function calls");
#endif
      case ABIArg::Uninitialized:
        MOZ_CRASH("Uninitialized ABIArg kind");
    }
  }

  defineReturn(lir, ins);
  assignSafepoint(lir, ins);
}

void LIRGenerator::visitWasmSelect(MWasmSelect* ins) {
  MDefinition* condExpr = ins->condExpr();

  // Pick off specific cases that we can do with LWasmCompareAndSelect to avoid
  // generating a boolean that we then have to test again.
  if (condExpr->isCompare() && condExpr->isEmittedAtUses()) {
    MCompare* comp = condExpr->toCompare();
    MCompare::CompareType compTy = comp->compareType();
    if (canSpecializeWasmCompareAndSelect(compTy, ins->type())) {
      JSOp jsop = comp->jsop();
      // We don't currently generate any other JSOPs for the comparison, and if
      // that changes, we want to know about it.  Hence this assertion.
      MOZ_ASSERT(jsop == JSOp::Eq || jsop == JSOp::Ne || jsop == JSOp::Lt ||
                 jsop == JSOp::Gt || jsop == JSOp::Le || jsop == JSOp::Ge);
      MDefinition* lhs = comp->lhs();
      MDefinition* rhs = comp->rhs();
      jsop = ReorderComparison(jsop, &lhs, &rhs);
      lowerWasmCompareAndSelect(ins, lhs, rhs, compTy, jsop);
      return;
    }
  }
  // Fall through to code that generates a boolean and selects on that.

  if (ins->type() == MIRType::Int64) {
    lowerWasmSelectI64(ins);
    return;
  }

  lowerWasmSelectI(ins);
}

void LIRGenerator::visitWasmFence(MWasmFence* ins) {
  add(new (alloc()) LWasmFence, ins);
}

void LIRGenerator::visitWasmLoadField(MWasmLoadField* ins) {
  uint32_t offs = ins->offset();
  LAllocation obj = useRegister(ins->obj());
  MWideningOp wideningOp = ins->wideningOp();
  if (ins->type() == MIRType::Int64) {
    MOZ_RELEASE_ASSERT(wideningOp == MWideningOp::None);
    defineInt64(new (alloc()) LWasmLoadSlotI64(obj, offs, ins->maybeTrap()),
                ins);
  } else {
    define(new (alloc()) LWasmLoadSlot(obj, offs, ins->type(), wideningOp,
                                       ins->maybeTrap()),
           ins);
  }
}

void LIRGenerator::visitWasmLoadFieldKA(MWasmLoadFieldKA* ins) {
  uint32_t offs = ins->offset();
  LAllocation obj = useRegister(ins->obj());
  MWideningOp wideningOp = ins->wideningOp();
  if (ins->type() == MIRType::Int64) {
    MOZ_RELEASE_ASSERT(wideningOp == MWideningOp::None);
    defineInt64(new (alloc()) LWasmLoadSlotI64(obj, offs, ins->maybeTrap()),
                ins);
  } else {
    define(new (alloc()) LWasmLoadSlot(obj, offs, ins->type(), wideningOp,
                                       ins->maybeTrap()),
           ins);
  }
  add(new (alloc()) LKeepAliveObject(useKeepalive(ins->ka())), ins);
}

void LIRGenerator::visitWasmLoadElementKA(MWasmLoadElementKA* ins) {
  LAllocation base = useRegister(ins->base());
  LAllocation index = useRegister(ins->index());
  MWideningOp wideningOp = ins->wideningOp();
  Scale scale = ins->scale();
  if (ins->type() == MIRType::Int64) {
    MOZ_RELEASE_ASSERT(wideningOp == MWideningOp::None);
    defineInt64(
        new (alloc()) LWasmLoadElementI64(base, index, ins->maybeTrap()), ins);
  } else {
    LDefinition tmp =
        ins->type() == MIRType::Simd128 ? temp() : LDefinition::BogusTemp();
    define(new (alloc()) LWasmLoadElement(base, index, tmp, ins->type(),
                                          wideningOp, scale, ins->maybeTrap()),
           ins);
  }
  add(new (alloc()) LKeepAliveObject(useKeepalive(ins->ka())), ins);
}

void LIRGenerator::visitWasmStoreFieldKA(MWasmStoreFieldKA* ins) {
  MDefinition* value = ins->value();
  uint32_t offs = ins->offset();
  MNarrowingOp narrowingOp = ins->narrowingOp();
  LAllocation obj = useRegister(ins->obj());
  LInstruction* lir;
  if (value->type() == MIRType::Int64) {
    MOZ_RELEASE_ASSERT(narrowingOp == MNarrowingOp::None);
    lir = new (alloc())
        LWasmStoreSlotI64(useInt64Register(value), obj, offs, ins->maybeTrap());
  } else {
    lir = new (alloc())
        LWasmStoreSlot(useRegister(value), obj, offs, value->type(),
                       narrowingOp, ins->maybeTrap());
  }
  add(lir, ins);
  add(new (alloc()) LKeepAliveObject(useKeepalive(ins->ka())), ins);
}

void LIRGenerator::visitWasmStoreFieldRefKA(MWasmStoreFieldRefKA* ins) {
  LAllocation instance = useRegister(ins->instance());
  LAllocation obj = useFixed(ins->obj(), PreBarrierReg);
  LAllocation value = useRegister(ins->value());
  uint32_t offset = ins->offset();
  add(new (alloc()) LWasmStoreRef(instance, obj, value, temp(), offset,
                                  ins->maybeTrap(), ins->preBarrierKind()),
      ins);
  add(new (alloc()) LKeepAliveObject(useKeepalive(ins->ka())), ins);
}

void LIRGenerator::visitWasmStoreElementKA(MWasmStoreElementKA* ins) {
  LAllocation base = useRegister(ins->base());
  LAllocation index = useRegister(ins->index());
  MDefinition* value = ins->value();
  MNarrowingOp narrowingOp = ins->narrowingOp();
  Scale scale = ins->scale();
  LInstruction* lir;
  if (value->type() == MIRType::Int64) {
    MOZ_RELEASE_ASSERT(narrowingOp == MNarrowingOp::None);
    lir = new (alloc()) LWasmStoreElementI64(
        base, index, useInt64Register(value), ins->maybeTrap());
  } else {
    LDefinition tmp =
        value->type() == MIRType::Simd128 ? temp() : LDefinition::BogusTemp();
    lir = new (alloc())
        LWasmStoreElement(base, index, useRegister(value), tmp, value->type(),
                          narrowingOp, scale, ins->maybeTrap());
  }
  add(lir, ins);
  add(new (alloc()) LKeepAliveObject(useKeepalive(ins->ka())), ins);
}

void LIRGenerator::visitWasmStoreElementRefKA(MWasmStoreElementRefKA* ins) {
  LAllocation instance = useRegister(ins->instance());
  LAllocation base = useFixed(ins->base(), PreBarrierReg);
  LAllocation index = useRegister(ins->index());
  LAllocation value = useRegister(ins->value());
  bool needTemps = ins->preBarrierKind() == WasmPreBarrierKind::Normal;
  LDefinition temp0 = needTemps ? temp() : LDefinition::BogusTemp();
  LDefinition temp1 = needTemps ? temp() : LDefinition::BogusTemp();
  add(new (alloc())
          LWasmStoreElementRef(instance, base, index, value, temp0, temp1,
                               ins->maybeTrap(), ins->preBarrierKind()),
      ins);
  add(new (alloc()) LKeepAliveObject(useKeepalive(ins->ka())), ins);
}

WasmRefIsSubtypeDefs LIRGenerator::useWasmRefIsSubtype(wasm::RefType destType,
                                                       MDefinition* superSTV) {
  BranchWasmRefIsSubtypeRegisters needs =
      MacroAssembler::regsForBranchWasmRefIsSubtype(destType);
  return WasmRefIsSubtypeDefs{
      .superSTV = needs.needSuperSTV ? useRegister(superSTV) : LAllocation(),
      .scratch1 = needs.needScratch1 ? temp() : LDefinition(),
      .scratch2 = needs.needScratch2 ? temp() : LDefinition(),
  };
}

void LIRGenerator::visitWasmRefIsSubtypeOfAbstract(
    MWasmRefIsSubtypeOfAbstract* ins) {
  if (CanEmitAtUseForSingleTest(ins)) {
    emitAtUses(ins);
    return;
  }

  LAllocation ref = useRegister(ins->ref());
  WasmRefIsSubtypeDefs regs =
      useWasmRefIsSubtype(ins->destType(), /*superSTV=*/nullptr);
  define(new (alloc()) LWasmRefIsSubtypeOfAbstract(ref, regs.scratch1), ins);
}

void LIRGenerator::visitWasmRefIsSubtypeOfConcrete(
    MWasmRefIsSubtypeOfConcrete* ins) {
  if (CanEmitAtUseForSingleTest(ins)) {
    emitAtUses(ins);
    return;
  }

  LAllocation ref = useRegister(ins->ref());
  WasmRefIsSubtypeDefs regs =
      useWasmRefIsSubtype(ins->destType(), ins->superSTV());
  define(new (alloc()) LWasmRefIsSubtypeOfConcrete(
             ref, regs.superSTV, regs.scratch1, regs.scratch2),
         ins);
}

void LIRGenerator::visitWasmNewStructObject(MWasmNewStructObject* ins) {
  LWasmNewStructObject* lir = new (alloc())
      LWasmNewStructObject(useFixed(ins->instance(), InstanceReg),
                           useRegister(ins->typeDefData()), temp(), temp());
  define(lir, ins);
  assignWasmSafepoint(lir);
}

void LIRGenerator::visitWasmNewArrayObject(MWasmNewArrayObject* ins) {
  LWasmNewArrayObject* lir = new (alloc())
      LWasmNewArrayObject(useFixed(ins->instance(), InstanceReg),
                          useRegisterOrConstant(ins->numElements()),
                          useRegister(ins->typeDefData()), temp(), temp());
  define(lir, ins);
  assignWasmSafepoint(lir);
}

#ifdef FUZZING_JS_FUZZILLI
void LIRGenerator::visitFuzzilliHash(MFuzzilliHash* ins) {
  MDefinition* value = ins->getOperand(0);

  if (value->type() == MIRType::Undefined || value->type() == MIRType::Null) {
    define(new (alloc()) LFuzzilliHashT(LAllocation(), temp(), tempDouble()),
           ins);
  } else if (value->type() == MIRType::Int32 ||
             value->type() == MIRType::Double ||
             value->type() == MIRType::Float32 ||
             value->type() == MIRType::Boolean ||
             value->type() == MIRType::BigInt) {
    define(new (alloc())
               LFuzzilliHashT(useRegister(value), temp(), tempDouble()),
           ins);
  } else if (value->type() == MIRType::Object) {
    LFuzzilliHashT* lir =
        new (alloc()) LFuzzilliHashT(useRegister(value), temp(), tempDouble());
    define(lir, ins);
    assignSafepoint(lir, ins);
  } else if (value->type() == MIRType::Value) {
    LFuzzilliHashV* lir =
        new (alloc()) LFuzzilliHashV(useBox(value), temp(), tempDouble());
    define(lir, ins);
    assignSafepoint(lir, ins);
  } else {
    define(new (alloc()) LInteger(0), ins);
  }
}

void LIRGenerator::visitFuzzilliHashStore(MFuzzilliHashStore* ins) {
  MDefinition* value = ins->getOperand(0);
  MOZ_ASSERT(value->type() == MIRType::Int32);
  add(new (alloc()) LFuzzilliHashStore(useRegister(value), temp(), temp()),
      ins);
}
#endif

static_assert(!std::is_polymorphic_v<LIRGenerator>,
              "LIRGenerator should not have any virtual methods");

#ifdef JS_CODEGEN_NONE
void LIRGenerator::visitReturnImpl(MDefinition*, bool) { MOZ_CRASH(); }
#endif
