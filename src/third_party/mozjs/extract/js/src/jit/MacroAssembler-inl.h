/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MacroAssembler_inl_h
#define jit_MacroAssembler_inl_h

#include "jit/MacroAssembler.h"

#include "mozilla/FloatingPoint.h"
#include "mozilla/MathAlgorithms.h"

#include "gc/Zone.h"
#include "jit/CalleeToken.h"
#include "jit/CompileWrappers.h"
#include "jit/JitFrames.h"
#include "jit/JSJitFrameIter.h"
#include "util/DifferentialTesting.h"
#include "vm/BigIntType.h"
#include "vm/JSObject.h"
#include "vm/ProxyObject.h"
#include "vm/Runtime.h"
#include "vm/StringType.h"

#include "jit/ABIFunctionList-inl.h"

#if defined(JS_CODEGEN_X86)
#  include "jit/x86/MacroAssembler-x86-inl.h"
#elif defined(JS_CODEGEN_X64)
#  include "jit/x64/MacroAssembler-x64-inl.h"
#elif defined(JS_CODEGEN_ARM)
#  include "jit/arm/MacroAssembler-arm-inl.h"
#elif defined(JS_CODEGEN_ARM64)
#  include "jit/arm64/MacroAssembler-arm64-inl.h"
#elif defined(JS_CODEGEN_MIPS32)
#  include "jit/mips32/MacroAssembler-mips32-inl.h"
#elif defined(JS_CODEGEN_MIPS64)
#  include "jit/mips64/MacroAssembler-mips64-inl.h"
#elif defined(JS_CODEGEN_LOONG64)
#  include "jit/loong64/MacroAssembler-loong64-inl.h"
#elif defined(JS_CODEGEN_RISCV64)
#  include "jit/riscv64/MacroAssembler-riscv64-inl.h"
#elif defined(JS_CODEGEN_WASM32)
#  include "jit/wasm32/MacroAssembler-wasm32-inl.h"
#elif !defined(JS_CODEGEN_NONE)
#  error "Unknown architecture!"
#endif

#include "wasm/WasmBuiltins.h"

namespace js {
namespace jit {

template <typename Sig>
DynFn DynamicFunction(Sig fun) {
  ABIFunctionSignature<Sig> sig;
  return DynFn{sig.address(fun)};
}

// Helper for generatePreBarrier.
inline DynFn JitPreWriteBarrier(MIRType type) {
  switch (type) {
    case MIRType::Value: {
      using Fn = void (*)(JSRuntime * rt, Value * vp);
      return DynamicFunction<Fn>(JitValuePreWriteBarrier);
    }
    case MIRType::String: {
      using Fn = void (*)(JSRuntime * rt, JSString * *stringp);
      return DynamicFunction<Fn>(JitStringPreWriteBarrier);
    }
    case MIRType::Object: {
      using Fn = void (*)(JSRuntime * rt, JSObject * *objp);
      return DynamicFunction<Fn>(JitObjectPreWriteBarrier);
    }
    case MIRType::Shape: {
      using Fn = void (*)(JSRuntime * rt, Shape * *shapep);
      return DynamicFunction<Fn>(JitShapePreWriteBarrier);
    }
    default:
      MOZ_CRASH();
  }
}

//{{{ check_macroassembler_style
// ===============================================================
// Stack manipulation functions.

CodeOffset MacroAssembler::PushWithPatch(ImmWord word) {
  framePushed_ += sizeof(word.value);
  return pushWithPatch(word);
}

CodeOffset MacroAssembler::PushWithPatch(ImmPtr imm) {
  return PushWithPatch(ImmWord(uintptr_t(imm.value)));
}

// ===============================================================
// Simple call functions.

void MacroAssembler::call(TrampolinePtr code) { call(ImmPtr(code.value)); }

CodeOffset MacroAssembler::call(const wasm::CallSiteDesc& desc,
                                const Register reg) {
  CodeOffset l = call(reg);
  append(desc, l);
  return l;
}

CodeOffset MacroAssembler::call(const wasm::CallSiteDesc& desc,
                                uint32_t funcIndex) {
  CodeOffset l = callWithPatch();
  append(desc, l, funcIndex);
  return l;
}

void MacroAssembler::call(const wasm::CallSiteDesc& desc, wasm::Trap trap) {
  CodeOffset l = callWithPatch();
  append(desc, l, trap);
}

CodeOffset MacroAssembler::call(const wasm::CallSiteDesc& desc,
                                wasm::SymbolicAddress imm) {
  MOZ_ASSERT(wasm::NeedsBuiltinThunk(imm),
             "only for functions which may appear in profiler");
  CodeOffset raOffset = call(imm);
  append(desc, raOffset);
  return raOffset;
}

// ===============================================================
// ABI function calls.

void MacroAssembler::passABIArg(Register reg) {
  passABIArg(MoveOperand(reg), MoveOp::GENERAL);
}

void MacroAssembler::passABIArg(FloatRegister reg, MoveOp::Type type) {
  passABIArg(MoveOperand(reg), type);
}

void MacroAssembler::callWithABI(DynFn fun, MoveOp::Type result,
                                 CheckUnsafeCallWithABI check) {
  AutoProfilerCallInstrumentation profiler(*this);
  callWithABINoProfiler(fun.address, result, check);
}

template <typename Sig, Sig fun>
void MacroAssembler::callWithABI(MoveOp::Type result,
                                 CheckUnsafeCallWithABI check) {
  ABIFunction<Sig, fun> abiFun;
  AutoProfilerCallInstrumentation profiler(*this);
  callWithABINoProfiler(abiFun.address(), result, check);
}

void MacroAssembler::callWithABI(Register fun, MoveOp::Type result) {
  AutoProfilerCallInstrumentation profiler(*this);
  callWithABINoProfiler(fun, result);
}

void MacroAssembler::callWithABI(const Address& fun, MoveOp::Type result) {
  AutoProfilerCallInstrumentation profiler(*this);
  callWithABINoProfiler(fun, result);
}

void MacroAssembler::appendSignatureType(MoveOp::Type type) {
#ifdef JS_SIMULATOR
  signature_ <<= ArgType_Shift;
  switch (type) {
    case MoveOp::GENERAL:
      signature_ |= ArgType_General;
      break;
    case MoveOp::DOUBLE:
      signature_ |= ArgType_Float64;
      break;
    case MoveOp::FLOAT32:
      signature_ |= ArgType_Float32;
      break;
    default:
      MOZ_CRASH("Invalid argument type");
  }
#endif
}

ABIFunctionType MacroAssembler::signature() const {
#ifdef JS_SIMULATOR
#  ifdef DEBUG
  switch (signature_) {
    case Args_General0:
    case Args_General1:
    case Args_General2:
    case Args_General3:
    case Args_General4:
    case Args_General5:
    case Args_General6:
    case Args_General7:
    case Args_General8:
    case Args_Double_None:
    case Args_Int_Double:
    case Args_Float32_Float32:
    case Args_Int_Float32:
    case Args_Double_Double:
    case Args_Double_Int:
    case Args_Double_DoubleInt:
    case Args_Double_DoubleDouble:
    case Args_Double_IntDouble:
    case Args_Int_IntDouble:
    case Args_Int_DoubleInt:
    case Args_Int_DoubleIntInt:
    case Args_Int_IntDoubleIntInt:
    case Args_Double_DoubleDoubleDouble:
    case Args_Double_DoubleDoubleDoubleDouble:
      break;
    default:
      MOZ_CRASH("Unexpected type");
  }
#  endif  // DEBUG

  return ABIFunctionType(signature_);
#else
  // No simulator enabled.
  MOZ_CRASH("Only available for making calls within a simulator.");
#endif
}

// ===============================================================
// Jit Frames.

uint32_t MacroAssembler::callJitNoProfiler(Register callee) {
#ifdef JS_USE_LINK_REGISTER
  // The return address is pushed by the callee.
  call(callee);
#else
  callAndPushReturnAddress(callee);
#endif
  return currentOffset();
}

uint32_t MacroAssembler::callJit(Register callee) {
  AutoProfilerCallInstrumentation profiler(*this);
  uint32_t ret = callJitNoProfiler(callee);
  return ret;
}

uint32_t MacroAssembler::callJit(JitCode* callee) {
  AutoProfilerCallInstrumentation profiler(*this);
  call(callee);
  return currentOffset();
}

uint32_t MacroAssembler::callJit(TrampolinePtr code) {
  AutoProfilerCallInstrumentation profiler(*this);
  call(code);
  return currentOffset();
}

uint32_t MacroAssembler::callJit(ImmPtr callee) {
  AutoProfilerCallInstrumentation profiler(*this);
  call(callee);
  return currentOffset();
}

void MacroAssembler::pushFrameDescriptor(FrameType type) {
  uint32_t descriptor = MakeFrameDescriptor(type);
  push(Imm32(descriptor));
}

void MacroAssembler::PushFrameDescriptor(FrameType type) {
  uint32_t descriptor = MakeFrameDescriptor(type);
  Push(Imm32(descriptor));
}

void MacroAssembler::pushFrameDescriptorForJitCall(FrameType type,
                                                   uint32_t argc) {
  uint32_t descriptor = MakeFrameDescriptorForJitCall(type, argc);
  push(Imm32(descriptor));
}

void MacroAssembler::PushFrameDescriptorForJitCall(FrameType type,
                                                   uint32_t argc) {
  uint32_t descriptor = MakeFrameDescriptorForJitCall(type, argc);
  Push(Imm32(descriptor));
}

void MacroAssembler::pushFrameDescriptorForJitCall(FrameType type,
                                                   Register argc,
                                                   Register scratch) {
  if (argc != scratch) {
    mov(argc, scratch);
  }
  lshift32(Imm32(NUMACTUALARGS_SHIFT), scratch);
  or32(Imm32(int32_t(type)), scratch);
  push(scratch);
}

void MacroAssembler::PushFrameDescriptorForJitCall(FrameType type,
                                                   Register argc,
                                                   Register scratch) {
  pushFrameDescriptorForJitCall(type, argc, scratch);
  framePushed_ += sizeof(uintptr_t);
}

void MacroAssembler::loadNumActualArgs(Register framePtr, Register dest) {
  loadPtr(Address(framePtr, JitFrameLayout::offsetOfDescriptor()), dest);
  rshift32(Imm32(NUMACTUALARGS_SHIFT), dest);
}

void MacroAssembler::PushCalleeToken(Register callee, bool constructing) {
  if (constructing) {
    orPtr(Imm32(CalleeToken_FunctionConstructing), callee);
    Push(callee);
    andPtr(Imm32(uint32_t(CalleeTokenMask)), callee);
  } else {
    static_assert(CalleeToken_Function == 0,
                  "Non-constructing call requires no tagging");
    Push(callee);
  }
}

void MacroAssembler::loadFunctionFromCalleeToken(Address token, Register dest) {
#ifdef DEBUG
  Label ok;
  loadPtr(token, dest);
  andPtr(Imm32(uint32_t(~CalleeTokenMask)), dest);
  branchPtr(Assembler::Equal, dest, Imm32(CalleeToken_Function), &ok);
  branchPtr(Assembler::Equal, dest, Imm32(CalleeToken_FunctionConstructing),
            &ok);
  assumeUnreachable("Unexpected CalleeToken tag");
  bind(&ok);
#endif
  loadPtr(token, dest);
  andPtr(Imm32(uint32_t(CalleeTokenMask)), dest);
}

uint32_t MacroAssembler::buildFakeExitFrame(Register scratch) {
  mozilla::DebugOnly<uint32_t> initialDepth = framePushed();

  PushFrameDescriptor(FrameType::IonJS);
  uint32_t retAddr = pushFakeReturnAddress(scratch);
  Push(FramePointer);

  MOZ_ASSERT(framePushed() == initialDepth + ExitFrameLayout::Size());
  return retAddr;
}

// ===============================================================
// Exit frame footer.

void MacroAssembler::enterExitFrame(Register cxreg, Register scratch,
                                    const VMFunctionData* f) {
  MOZ_ASSERT(f);
  linkExitFrame(cxreg, scratch);
  // Push VMFunction pointer, to mark arguments.
  Push(ImmPtr(f));
}

void MacroAssembler::enterFakeExitFrame(Register cxreg, Register scratch,
                                        ExitFrameType type) {
  linkExitFrame(cxreg, scratch);
  Push(Imm32(int32_t(type)));
}

void MacroAssembler::enterFakeExitFrameForNative(Register cxreg,
                                                 Register scratch,
                                                 bool isConstructing) {
  enterFakeExitFrame(cxreg, scratch,
                     isConstructing ? ExitFrameType::ConstructNative
                                    : ExitFrameType::CallNative);
}

void MacroAssembler::leaveExitFrame(size_t extraFrame) {
  freeStack(ExitFooterFrame::Size() + extraFrame);
}

// ===============================================================
// Move instructions

void MacroAssembler::moveValue(const ConstantOrRegister& src,
                               const ValueOperand& dest) {
  if (src.constant()) {
    moveValue(src.value(), dest);
    return;
  }

  moveValue(src.reg(), dest);
}

// ===============================================================
// Arithmetic functions

void MacroAssembler::addPtr(ImmPtr imm, Register dest) {
  addPtr(ImmWord(uintptr_t(imm.value)), dest);
}

// ===============================================================
// Branch functions

template <class L>
void MacroAssembler::branchIfFalseBool(Register reg, L label) {
  // Note that C++ bool is only 1 byte, so ignore the higher-order bits.
  branchTest32(Assembler::Zero, reg, Imm32(0xFF), label);
}

void MacroAssembler::branchIfTrueBool(Register reg, Label* label) {
  // Note that C++ bool is only 1 byte, so ignore the higher-order bits.
  branchTest32(Assembler::NonZero, reg, Imm32(0xFF), label);
}

void MacroAssembler::branchIfRope(Register str, Label* label) {
  Address flags(str, JSString::offsetOfFlags());
  branchTest32(Assembler::Zero, flags, Imm32(JSString::LINEAR_BIT), label);
}

void MacroAssembler::branchIfNotRope(Register str, Label* label) {
  Address flags(str, JSString::offsetOfFlags());
  branchTest32(Assembler::NonZero, flags, Imm32(JSString::LINEAR_BIT), label);
}

void MacroAssembler::branchLatin1String(Register string, Label* label) {
  branchTest32(Assembler::NonZero, Address(string, JSString::offsetOfFlags()),
               Imm32(JSString::LATIN1_CHARS_BIT), label);
}

void MacroAssembler::branchTwoByteString(Register string, Label* label) {
  branchTest32(Assembler::Zero, Address(string, JSString::offsetOfFlags()),
               Imm32(JSString::LATIN1_CHARS_BIT), label);
}

void MacroAssembler::branchIfBigIntIsNegative(Register bigInt, Label* label) {
  branchTest32(Assembler::NonZero, Address(bigInt, BigInt::offsetOfFlags()),
               Imm32(BigInt::signBitMask()), label);
}

void MacroAssembler::branchIfBigIntIsNonNegative(Register bigInt,
                                                 Label* label) {
  branchTest32(Assembler::Zero, Address(bigInt, BigInt::offsetOfFlags()),
               Imm32(BigInt::signBitMask()), label);
}

void MacroAssembler::branchIfBigIntIsZero(Register bigInt, Label* label) {
  branch32(Assembler::Equal, Address(bigInt, BigInt::offsetOfLength()),
           Imm32(0), label);
}

void MacroAssembler::branchIfBigIntIsNonZero(Register bigInt, Label* label) {
  branch32(Assembler::NotEqual, Address(bigInt, BigInt::offsetOfLength()),
           Imm32(0), label);
}

void MacroAssembler::branchTestFunctionFlags(Register fun, uint32_t flags,
                                             Condition cond, Label* label) {
  Address address(fun, JSFunction::offsetOfFlagsAndArgCount());
  branchTest32(cond, address, Imm32(flags), label);
}

void MacroAssembler::branchIfNotFunctionIsNonBuiltinCtor(Register fun,
                                                         Register scratch,
                                                         Label* label) {
  // Guard the function has the BASESCRIPT and CONSTRUCTOR flags and does NOT
  // have the SELF_HOSTED flag.
  // This is equivalent to JSFunction::isNonBuiltinConstructor.
  constexpr int32_t mask = FunctionFlags::BASESCRIPT |
                           FunctionFlags::SELF_HOSTED |
                           FunctionFlags::CONSTRUCTOR;
  constexpr int32_t expected =
      FunctionFlags::BASESCRIPT | FunctionFlags::CONSTRUCTOR;

  load32(Address(fun, JSFunction::offsetOfFlagsAndArgCount()), scratch);
  and32(Imm32(mask), scratch);
  branch32(Assembler::NotEqual, scratch, Imm32(expected), label);
}

void MacroAssembler::branchIfFunctionHasNoJitEntry(Register fun,
                                                   bool isConstructing,
                                                   Label* label) {
  uint16_t flags = FunctionFlags::HasJitEntryFlags(isConstructing);
  branchTestFunctionFlags(fun, flags, Assembler::Zero, label);
}

void MacroAssembler::branchIfFunctionHasJitEntry(Register fun,
                                                 bool isConstructing,
                                                 Label* label) {
  uint16_t flags = FunctionFlags::HasJitEntryFlags(isConstructing);
  branchTestFunctionFlags(fun, flags, Assembler::NonZero, label);
}

void MacroAssembler::branchIfScriptHasJitScript(Register script, Label* label) {
  static_assert(ScriptWarmUpData::JitScriptTag == 0,
                "Code below depends on tag value");
  branchTestPtr(Assembler::Zero,
                Address(script, JSScript::offsetOfWarmUpData()),
                Imm32(ScriptWarmUpData::TagMask), label);
}

void MacroAssembler::branchIfScriptHasNoJitScript(Register script,
                                                  Label* label) {
  static_assert(ScriptWarmUpData::JitScriptTag == 0,
                "Code below depends on tag value");
  static_assert(BaseScript::offsetOfWarmUpData() ==
                    SelfHostedLazyScript::offsetOfWarmUpData(),
                "SelfHostedLazyScript and BaseScript must use same layout for "
                "warmUpData_");
  branchTestPtr(Assembler::NonZero,
                Address(script, JSScript::offsetOfWarmUpData()),
                Imm32(ScriptWarmUpData::TagMask), label);
}

void MacroAssembler::loadJitScript(Register script, Register dest) {
#ifdef DEBUG
  Label ok;
  branchIfScriptHasJitScript(script, &ok);
  assumeUnreachable("Script has no JitScript!");
  bind(&ok);
#endif

  static_assert(ScriptWarmUpData::JitScriptTag == 0,
                "Code below depends on tag value");
  loadPtr(Address(script, JSScript::offsetOfWarmUpData()), dest);
}

void MacroAssembler::loadFunctionArgCount(Register func, Register output) {
  load32(Address(func, JSFunction::offsetOfFlagsAndArgCount()), output);
  rshift32(Imm32(JSFunction::ArgCountShift), output);
}

void MacroAssembler::branchIfObjectEmulatesUndefined(Register objReg,
                                                     Register scratch,
                                                     Label* slowCheck,
                                                     Label* label) {
  // The branches to out-of-line code here implement a conservative version
  // of the JSObject::isWrapper test performed in EmulatesUndefined.
  loadObjClassUnsafe(objReg, scratch);

  branchTestClassIsProxy(true, scratch, slowCheck);

  Address flags(scratch, JSClass::offsetOfFlags());
  branchTest32(Assembler::NonZero, flags, Imm32(JSCLASS_EMULATES_UNDEFINED),
               label);
}

void MacroAssembler::branchFunctionKind(Condition cond,
                                        FunctionFlags::FunctionKind kind,
                                        Register fun, Register scratch,
                                        Label* label) {
  Address address(fun, JSFunction::offsetOfFlagsAndArgCount());
  load32(address, scratch);
  and32(Imm32(FunctionFlags::FUNCTION_KIND_MASK), scratch);
  branch32(cond, scratch, Imm32(kind), label);
}

void MacroAssembler::branchTestObjClass(Condition cond, Register obj,
                                        const JSClass* clasp, Register scratch,
                                        Register spectreRegToZero,
                                        Label* label) {
  MOZ_ASSERT(obj != scratch);
  MOZ_ASSERT(scratch != spectreRegToZero);

  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch);
  loadPtr(Address(scratch, Shape::offsetOfBaseShape()), scratch);
  branchPtr(cond, Address(scratch, BaseShape::offsetOfClasp()), ImmPtr(clasp),
            label);

  if (JitOptions.spectreObjectMitigations) {
    spectreZeroRegister(cond, scratch, spectreRegToZero);
  }
}

void MacroAssembler::branchTestObjClassNoSpectreMitigations(
    Condition cond, Register obj, const JSClass* clasp, Register scratch,
    Label* label) {
  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch);
  loadPtr(Address(scratch, Shape::offsetOfBaseShape()), scratch);
  branchPtr(cond, Address(scratch, BaseShape::offsetOfClasp()), ImmPtr(clasp),
            label);
}

void MacroAssembler::branchTestObjClass(Condition cond, Register obj,
                                        const Address& clasp, Register scratch,
                                        Register spectreRegToZero,
                                        Label* label) {
  MOZ_ASSERT(obj != scratch);
  MOZ_ASSERT(scratch != spectreRegToZero);

  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch);
  loadPtr(Address(scratch, Shape::offsetOfBaseShape()), scratch);
  loadPtr(Address(scratch, BaseShape::offsetOfClasp()), scratch);
  branchPtr(cond, clasp, scratch, label);

  if (JitOptions.spectreObjectMitigations) {
    spectreZeroRegister(cond, scratch, spectreRegToZero);
  }
}

void MacroAssembler::branchTestObjClassNoSpectreMitigations(
    Condition cond, Register obj, const Address& clasp, Register scratch,
    Label* label) {
  MOZ_ASSERT(obj != scratch);
  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch);
  loadPtr(Address(scratch, Shape::offsetOfBaseShape()), scratch);
  loadPtr(Address(scratch, BaseShape::offsetOfClasp()), scratch);
  branchPtr(cond, clasp, scratch, label);
}

void MacroAssembler::branchTestObjClass(Condition cond, Register obj,
                                        Register clasp, Register scratch,
                                        Register spectreRegToZero,
                                        Label* label) {
  MOZ_ASSERT(obj != scratch);
  MOZ_ASSERT(scratch != spectreRegToZero);

  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch);
  loadPtr(Address(scratch, Shape::offsetOfBaseShape()), scratch);
  loadPtr(Address(scratch, BaseShape::offsetOfClasp()), scratch);
  branchPtr(cond, clasp, scratch, label);

  if (JitOptions.spectreObjectMitigations) {
    spectreZeroRegister(cond, scratch, spectreRegToZero);
  }
}

void MacroAssembler::branchTestClassIsFunction(Condition cond, Register clasp,
                                               Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

  if (cond == Assembler::Equal) {
    branchPtr(Assembler::Equal, clasp, ImmPtr(&FunctionClass), label);
    branchPtr(Assembler::Equal, clasp, ImmPtr(&ExtendedFunctionClass), label);
    return;
  }

  Label isFunction;
  branchPtr(Assembler::Equal, clasp, ImmPtr(&FunctionClass), &isFunction);
  branchPtr(Assembler::NotEqual, clasp, ImmPtr(&ExtendedFunctionClass), label);
  bind(&isFunction);
}

void MacroAssembler::branchTestObjIsFunction(Condition cond, Register obj,
                                             Register scratch,
                                             Register spectreRegToZero,
                                             Label* label) {
  MOZ_ASSERT(scratch != spectreRegToZero);

  branchTestObjIsFunctionNoSpectreMitigations(cond, obj, scratch, label);

  if (JitOptions.spectreObjectMitigations) {
    spectreZeroRegister(cond, scratch, spectreRegToZero);
  }
}

void MacroAssembler::branchTestObjIsFunctionNoSpectreMitigations(
    Condition cond, Register obj, Register scratch, Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
  MOZ_ASSERT(obj != scratch);

  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch);
  loadPtr(Address(scratch, Shape::offsetOfBaseShape()), scratch);
  loadPtr(Address(scratch, BaseShape::offsetOfClasp()), scratch);
  branchTestClassIsFunction(cond, scratch, label);
}

void MacroAssembler::branchTestObjShape(Condition cond, Register obj,
                                        const Shape* shape, Register scratch,
                                        Register spectreRegToZero,
                                        Label* label) {
  MOZ_ASSERT(obj != scratch);
  MOZ_ASSERT(spectreRegToZero != scratch);

  if (JitOptions.spectreObjectMitigations) {
    move32(Imm32(0), scratch);
  }

  branchPtr(cond, Address(obj, JSObject::offsetOfShape()), ImmGCPtr(shape),
            label);

  if (JitOptions.spectreObjectMitigations) {
    spectreMovePtr(cond, scratch, spectreRegToZero);
  }
}

void MacroAssembler::branchTestObjShapeNoSpectreMitigations(Condition cond,
                                                            Register obj,
                                                            const Shape* shape,
                                                            Label* label) {
  branchPtr(cond, Address(obj, JSObject::offsetOfShape()), ImmGCPtr(shape),
            label);
}

void MacroAssembler::branchTestObjShape(Condition cond, Register obj,
                                        Register shape, Register scratch,
                                        Register spectreRegToZero,
                                        Label* label) {
  MOZ_ASSERT(obj != scratch);
  MOZ_ASSERT(obj != shape);
  MOZ_ASSERT(spectreRegToZero != scratch);

  if (JitOptions.spectreObjectMitigations) {
    move32(Imm32(0), scratch);
  }

  branchPtr(cond, Address(obj, JSObject::offsetOfShape()), shape, label);

  if (JitOptions.spectreObjectMitigations) {
    spectreMovePtr(cond, scratch, spectreRegToZero);
  }
}

void MacroAssembler::branchTestObjShapeNoSpectreMitigations(Condition cond,
                                                            Register obj,
                                                            Register shape,
                                                            Label* label) {
  branchPtr(cond, Address(obj, JSObject::offsetOfShape()), shape, label);
}

void MacroAssembler::branchTestObjShapeUnsafe(Condition cond, Register obj,
                                              Register shape, Label* label) {
  branchTestObjShapeNoSpectreMitigations(cond, obj, shape, label);
}

void MacroAssembler::branchTestClassIsProxy(bool proxy, Register clasp,
                                            Label* label) {
  branchTest32(proxy ? Assembler::NonZero : Assembler::Zero,
               Address(clasp, JSClass::offsetOfFlags()),
               Imm32(JSCLASS_IS_PROXY), label);
}

void MacroAssembler::branchTestObjectIsProxy(bool proxy, Register object,
                                             Register scratch, Label* label) {
  constexpr uint32_t ShiftedMask = (Shape::kindMask() << Shape::kindShift());
  static_assert(uint32_t(Shape::Kind::Proxy) == 0,
                "branchTest32 below depends on proxy kind being 0");
  loadPtr(Address(object, JSObject::offsetOfShape()), scratch);
  branchTest32(proxy ? Assembler::Zero : Assembler::NonZero,
               Address(scratch, Shape::offsetOfImmutableFlags()),
               Imm32(ShiftedMask), label);
}

void MacroAssembler::branchTestObjectIsWasmGcObject(bool isGcObject,
                                                    Register object,
                                                    Register scratch,
                                                    Label* label) {
  constexpr uint32_t ShiftedMask = (Shape::kindMask() << Shape::kindShift());
  constexpr uint32_t ShiftedKind =
      (uint32_t(Shape::Kind::WasmGC) << Shape::kindShift());
  MOZ_ASSERT(object != scratch);

  loadPtr(Address(object, JSObject::offsetOfShape()), scratch);
  load32(Address(scratch, Shape::offsetOfImmutableFlags()), scratch);
  and32(Imm32(ShiftedMask), scratch);
  branch32(isGcObject ? Assembler::Equal : Assembler::NotEqual, scratch,
           Imm32(ShiftedKind), label);
}

void MacroAssembler::branchTestProxyHandlerFamily(Condition cond,
                                                  Register proxy,
                                                  Register scratch,
                                                  const void* handlerp,
                                                  Label* label) {
#ifdef DEBUG
  Label ok;
  branchTestObjectIsProxy(true, proxy, scratch, &ok);
  assumeUnreachable("Expected ProxyObject in branchTestProxyHandlerFamily");
  bind(&ok);
#endif

  Address handlerAddr(proxy, ProxyObject::offsetOfHandler());
  loadPtr(handlerAddr, scratch);
  Address familyAddr(scratch, BaseProxyHandler::offsetOfFamily());
  branchPtr(cond, familyAddr, ImmPtr(handlerp), label);
}

void MacroAssembler::branchTestNeedsIncrementalBarrier(Condition cond,
                                                       Label* label) {
  MOZ_ASSERT(cond == Zero || cond == NonZero);
  CompileZone* zone = realm()->zone();
  const uint32_t* needsBarrierAddr = zone->addressOfNeedsIncrementalBarrier();
  branchTest32(cond, AbsoluteAddress(needsBarrierAddr), Imm32(0x1), label);
}

void MacroAssembler::branchTestNeedsIncrementalBarrierAnyZone(
    Condition cond, Label* label, Register scratch) {
  MOZ_ASSERT(cond == Zero || cond == NonZero);
  if (maybeRealm_) {
    branchTestNeedsIncrementalBarrier(cond, label);
  } else {
    // We are compiling the interpreter or another runtime-wide trampoline, so
    // we have to load cx->zone.
    loadPtr(AbsoluteAddress(runtime()->addressOfZone()), scratch);
    Address needsBarrierAddr(scratch, Zone::offsetOfNeedsIncrementalBarrier());
    branchTest32(cond, needsBarrierAddr, Imm32(0x1), label);
  }
}

void MacroAssembler::branchTestMagicValue(Condition cond,
                                          const ValueOperand& val,
                                          JSWhyMagic why, Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  branchTestValue(cond, val, MagicValue(why), label);
}

void MacroAssembler::branchDoubleNotInInt64Range(Address src, Register temp,
                                                 Label* fail) {
  using mozilla::FloatingPoint;

  // Tests if double is in [INT64_MIN; INT64_MAX] range
  uint32_t EXPONENT_MASK = 0x7ff00000;
  uint32_t EXPONENT_SHIFT = FloatingPoint<double>::kExponentShift - 32;
  uint32_t TOO_BIG_EXPONENT = (FloatingPoint<double>::kExponentBias + 63)
                              << EXPONENT_SHIFT;

  load32(Address(src.base, src.offset + sizeof(int32_t)), temp);
  and32(Imm32(EXPONENT_MASK), temp);
  branch32(Assembler::GreaterThanOrEqual, temp, Imm32(TOO_BIG_EXPONENT), fail);
}

void MacroAssembler::branchDoubleNotInUInt64Range(Address src, Register temp,
                                                  Label* fail) {
  using mozilla::FloatingPoint;

  // Note: returns failure on -0.0
  // Tests if double is in [0; UINT64_MAX] range
  // Take the sign also in the equation. That way we can compare in one test?
  uint32_t EXPONENT_MASK = 0xfff00000;
  uint32_t EXPONENT_SHIFT = FloatingPoint<double>::kExponentShift - 32;
  uint32_t TOO_BIG_EXPONENT = (FloatingPoint<double>::kExponentBias + 64)
                              << EXPONENT_SHIFT;

  load32(Address(src.base, src.offset + sizeof(int32_t)), temp);
  and32(Imm32(EXPONENT_MASK), temp);
  branch32(Assembler::AboveOrEqual, temp, Imm32(TOO_BIG_EXPONENT), fail);
}

void MacroAssembler::branchFloat32NotInInt64Range(Address src, Register temp,
                                                  Label* fail) {
  using mozilla::FloatingPoint;

  // Tests if float is in [INT64_MIN; INT64_MAX] range
  uint32_t EXPONENT_MASK = 0x7f800000;
  uint32_t EXPONENT_SHIFT = FloatingPoint<float>::kExponentShift;
  uint32_t TOO_BIG_EXPONENT = (FloatingPoint<float>::kExponentBias + 63)
                              << EXPONENT_SHIFT;

  load32(src, temp);
  and32(Imm32(EXPONENT_MASK), temp);
  branch32(Assembler::GreaterThanOrEqual, temp, Imm32(TOO_BIG_EXPONENT), fail);
}

void MacroAssembler::branchFloat32NotInUInt64Range(Address src, Register temp,
                                                   Label* fail) {
  using mozilla::FloatingPoint;

  // Note: returns failure on -0.0
  // Tests if float is in [0; UINT64_MAX] range
  // Take the sign also in the equation. That way we can compare in one test?
  uint32_t EXPONENT_MASK = 0xff800000;
  uint32_t EXPONENT_SHIFT = FloatingPoint<float>::kExponentShift;
  uint32_t TOO_BIG_EXPONENT = (FloatingPoint<float>::kExponentBias + 64)
                              << EXPONENT_SHIFT;

  load32(src, temp);
  and32(Imm32(EXPONENT_MASK), temp);
  branch32(Assembler::AboveOrEqual, temp, Imm32(TOO_BIG_EXPONENT), fail);
}

// ========================================================================
// Canonicalization primitives.
void MacroAssembler::canonicalizeFloat(FloatRegister reg) {
  Label notNaN;
  branchFloat(DoubleOrdered, reg, reg, &notNaN);
  loadConstantFloat32(float(JS::GenericNaN()), reg);
  bind(&notNaN);
}

void MacroAssembler::canonicalizeFloatIfDeterministic(FloatRegister reg) {
  // See the comment in TypedArrayObjectTemplate::getElement.
  if (js::SupportDifferentialTesting()) {
    canonicalizeFloat(reg);
  }
}

void MacroAssembler::canonicalizeDouble(FloatRegister reg) {
  Label notNaN;
  branchDouble(DoubleOrdered, reg, reg, &notNaN);
  loadConstantDouble(JS::GenericNaN(), reg);
  bind(&notNaN);
}

void MacroAssembler::canonicalizeDoubleIfDeterministic(FloatRegister reg) {
  // See the comment in TypedArrayObjectTemplate::getElement.
  if (js::SupportDifferentialTesting()) {
    canonicalizeDouble(reg);
  }
}

// ========================================================================
// Memory access primitives.
template <class T>
void MacroAssembler::storeDouble(FloatRegister src, const T& dest) {
  canonicalizeDoubleIfDeterministic(src);
  storeUncanonicalizedDouble(src, dest);
}

template void MacroAssembler::storeDouble(FloatRegister src,
                                          const Address& dest);
template void MacroAssembler::storeDouble(FloatRegister src,
                                          const BaseIndex& dest);

template <class T>
void MacroAssembler::boxDouble(FloatRegister src, const T& dest) {
  storeDouble(src, dest);
}

template <class T>
void MacroAssembler::storeFloat32(FloatRegister src, const T& dest) {
  canonicalizeFloatIfDeterministic(src);
  storeUncanonicalizedFloat32(src, dest);
}

template void MacroAssembler::storeFloat32(FloatRegister src,
                                           const Address& dest);
template void MacroAssembler::storeFloat32(FloatRegister src,
                                           const BaseIndex& dest);

template <typename T>
void MacroAssembler::fallibleUnboxInt32(const T& src, Register dest,
                                        Label* fail) {
  // Int32Value can be unboxed efficiently with unboxInt32, so use that.
  branchTestInt32(Assembler::NotEqual, src, fail);
  unboxInt32(src, dest);
}

template <typename T>
void MacroAssembler::fallibleUnboxBoolean(const T& src, Register dest,
                                          Label* fail) {
  // BooleanValue can be unboxed efficiently with unboxBoolean, so use that.
  branchTestBoolean(Assembler::NotEqual, src, fail);
  unboxBoolean(src, dest);
}

template <typename T>
void MacroAssembler::fallibleUnboxObject(const T& src, Register dest,
                                         Label* fail) {
  fallibleUnboxPtr(src, dest, JSVAL_TYPE_OBJECT, fail);
}

template <typename T>
void MacroAssembler::fallibleUnboxString(const T& src, Register dest,
                                         Label* fail) {
  fallibleUnboxPtr(src, dest, JSVAL_TYPE_STRING, fail);
}

template <typename T>
void MacroAssembler::fallibleUnboxSymbol(const T& src, Register dest,
                                         Label* fail) {
  fallibleUnboxPtr(src, dest, JSVAL_TYPE_SYMBOL, fail);
}

template <typename T>
void MacroAssembler::fallibleUnboxBigInt(const T& src, Register dest,
                                         Label* fail) {
  fallibleUnboxPtr(src, dest, JSVAL_TYPE_BIGINT, fail);
}

//}}} check_macroassembler_style
// ===============================================================

#ifndef JS_CODEGEN_ARM64

template <typename T>
void MacroAssembler::branchTestStackPtr(Condition cond, T t, Label* label) {
  branchTestPtr(cond, getStackPointer(), t, label);
}

template <typename T>
void MacroAssembler::branchStackPtr(Condition cond, T rhs, Label* label) {
  branchPtr(cond, getStackPointer(), rhs, label);
}

template <typename T>
void MacroAssembler::branchStackPtrRhs(Condition cond, T lhs, Label* label) {
  branchPtr(cond, lhs, getStackPointer(), label);
}

template <typename T>
void MacroAssembler::addToStackPtr(T t) {
  addPtr(t, getStackPointer());
}

template <typename T>
void MacroAssembler::addStackPtrTo(T t) {
  addPtr(getStackPointer(), t);
}

void MacroAssembler::reserveStack(uint32_t amount) {
  subFromStackPtr(Imm32(amount));
  adjustFrame(amount);
}
#endif  // !JS_CODEGEN_ARM64

void MacroAssembler::loadObjClassUnsafe(Register obj, Register dest) {
  loadPtr(Address(obj, JSObject::offsetOfShape()), dest);
  loadPtr(Address(dest, Shape::offsetOfBaseShape()), dest);
  loadPtr(Address(dest, BaseShape::offsetOfClasp()), dest);
}

template <typename EmitPreBarrier>
void MacroAssembler::storeObjShape(Register shape, Register obj,
                                   EmitPreBarrier emitPreBarrier) {
  MOZ_ASSERT(shape != obj);
  Address shapeAddr(obj, JSObject::offsetOfShape());
  emitPreBarrier(*this, shapeAddr);
  storePtr(shape, shapeAddr);
}

template <typename EmitPreBarrier>
void MacroAssembler::storeObjShape(Shape* shape, Register obj,
                                   EmitPreBarrier emitPreBarrier) {
  Address shapeAddr(obj, JSObject::offsetOfShape());
  emitPreBarrier(*this, shapeAddr);
  storePtr(ImmGCPtr(shape), shapeAddr);
}

void MacroAssembler::loadObjProto(Register obj, Register dest) {
  loadPtr(Address(obj, JSObject::offsetOfShape()), dest);
  loadPtr(Address(dest, Shape::offsetOfBaseShape()), dest);
  loadPtr(Address(dest, BaseShape::offsetOfProto()), dest);
}

void MacroAssembler::loadStringLength(Register str, Register dest) {
  load32(Address(str, JSString::offsetOfLength()), dest);
}

void MacroAssembler::assertStackAlignment(uint32_t alignment,
                                          int32_t offset /* = 0 */) {
#ifdef DEBUG
  Label ok, bad;
  MOZ_ASSERT(mozilla::IsPowerOfTwo(alignment));

  // Wrap around the offset to be a non-negative number.
  offset %= alignment;
  if (offset < 0) {
    offset += alignment;
  }

  // Test if each bit from offset is set.
  uint32_t off = offset;
  while (off) {
    uint32_t lowestBit = 1 << mozilla::CountTrailingZeroes32(off);
    branchTestStackPtr(Assembler::Zero, Imm32(lowestBit), &bad);
    off ^= lowestBit;
  }

  // Check that all remaining bits are zero.
  branchTestStackPtr(Assembler::Zero, Imm32((alignment - 1) ^ offset), &ok);

  bind(&bad);
  breakpoint();
  bind(&ok);
#endif
}

void MacroAssembler::storeCallBoolResult(Register reg) {
  convertBoolToInt32(ReturnReg, reg);
}

void MacroAssembler::storeCallInt32Result(Register reg) {
#if JS_BITS_PER_WORD == 32
  storeCallPointerResult(reg);
#else
  // Ensure the upper 32 bits are cleared.
  move32(ReturnReg, reg);
#endif
}

void MacroAssembler::storeCallResultValue(AnyRegister dest, JSValueType type) {
  unboxValue(JSReturnOperand, dest, type);
}

void MacroAssembler::storeCallResultValue(TypedOrValueRegister dest) {
  if (dest.hasValue()) {
    storeCallResultValue(dest.valueReg());
  } else {
    storeCallResultValue(dest.typedReg(), ValueTypeFromMIRType(dest.type()));
  }
}

}  // namespace jit
}  // namespace js

#endif /* jit_MacroAssembler_inl_h */
