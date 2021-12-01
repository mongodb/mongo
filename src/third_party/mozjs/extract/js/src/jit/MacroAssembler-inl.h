/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MacroAssembler_inl_h
#define jit_MacroAssembler_inl_h

#include "jit/MacroAssembler.h"

#include "mozilla/MathAlgorithms.h"

#if defined(JS_CODEGEN_X86)
# include "jit/x86/MacroAssembler-x86-inl.h"
#elif defined(JS_CODEGEN_X64)
# include "jit/x64/MacroAssembler-x64-inl.h"
#elif defined(JS_CODEGEN_ARM)
# include "jit/arm/MacroAssembler-arm-inl.h"
#elif defined(JS_CODEGEN_ARM64)
# include "jit/arm64/MacroAssembler-arm64-inl.h"
#elif defined(JS_CODEGEN_MIPS32)
# include "jit/mips32/MacroAssembler-mips32-inl.h"
#elif defined(JS_CODEGEN_MIPS64)
# include "jit/mips64/MacroAssembler-mips64-inl.h"
#elif !defined(JS_CODEGEN_NONE)
# error "Unknown architecture!"
#endif

#include "wasm/WasmBuiltins.h"

namespace js {
namespace jit {

//{{{ check_macroassembler_style
// ===============================================================
// Stack manipulation functions.

CodeOffset
MacroAssembler::PushWithPatch(ImmWord word)
{
    framePushed_ += sizeof(word.value);
    return pushWithPatch(word);
}

CodeOffset
MacroAssembler::PushWithPatch(ImmPtr imm)
{
    return PushWithPatch(ImmWord(uintptr_t(imm.value)));
}

// ===============================================================
// Simple call functions.

void
MacroAssembler::call(TrampolinePtr code)
{
    call(ImmPtr(code.value));
}

void
MacroAssembler::call(const wasm::CallSiteDesc& desc, const Register reg)
{
    CodeOffset l = call(reg);
    append(desc, l);
}

void
MacroAssembler::call(const wasm::CallSiteDesc& desc, uint32_t funcIndex)
{
    CodeOffset l = callWithPatch();
    append(desc, l, funcIndex);
}

void
MacroAssembler::call(const wasm::CallSiteDesc& desc, wasm::Trap trap)
{
    CodeOffset l = callWithPatch();
    append(desc, l, trap);
}

void
MacroAssembler::call(const wasm::CallSiteDesc& desc, wasm::SymbolicAddress imm)
{
    MOZ_ASSERT(wasm::NeedsBuiltinThunk(imm), "only for functions which may appear in profiler");
    call(imm);
    append(desc, CodeOffset(currentOffset()));
}

// ===============================================================
// ABI function calls.

void
MacroAssembler::passABIArg(Register reg)
{
    passABIArg(MoveOperand(reg), MoveOp::GENERAL);
}

void
MacroAssembler::passABIArg(FloatRegister reg, MoveOp::Type type)
{
    passABIArg(MoveOperand(reg), type);
}

void
MacroAssembler::callWithABI(void* fun, MoveOp::Type result, CheckUnsafeCallWithABI check)
{
    AutoProfilerCallInstrumentation profiler(*this);
    callWithABINoProfiler(fun, result, check);
}

void
MacroAssembler::callWithABI(Register fun, MoveOp::Type result)
{
    AutoProfilerCallInstrumentation profiler(*this);
    callWithABINoProfiler(fun, result);
}

void
MacroAssembler::callWithABI(const Address& fun, MoveOp::Type result)
{
    AutoProfilerCallInstrumentation profiler(*this);
    callWithABINoProfiler(fun, result);
}

void
MacroAssembler::appendSignatureType(MoveOp::Type type)
{
#ifdef JS_SIMULATOR
    signature_ <<= ArgType_Shift;
    switch (type) {
      case MoveOp::GENERAL: signature_ |= ArgType_General; break;
      case MoveOp::DOUBLE:  signature_ |= ArgType_Double;  break;
      case MoveOp::FLOAT32: signature_ |= ArgType_Float32; break;
      default: MOZ_CRASH("Invalid argument type");
    }
#endif
}

ABIFunctionType
MacroAssembler::signature() const
{
#ifdef JS_SIMULATOR
#ifdef DEBUG
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
      case Args_Double_Double:
      case Args_Double_Int:
      case Args_Double_DoubleInt:
      case Args_Double_DoubleDouble:
      case Args_Double_IntDouble:
      case Args_Int_IntDouble:
      case Args_Int_DoubleIntInt:
      case Args_Int_IntDoubleIntInt:
      case Args_Double_DoubleDoubleDouble:
      case Args_Double_DoubleDoubleDoubleDouble:
        break;
      default:
        MOZ_CRASH("Unexpected type");
    }
#endif // DEBUG

    return ABIFunctionType(signature_);
#else
    // No simulator enabled.
    MOZ_CRASH("Only available for making calls within a simulator.");
#endif
}

// ===============================================================
// Jit Frames.

uint32_t
MacroAssembler::callJitNoProfiler(Register callee)
{
#ifdef JS_USE_LINK_REGISTER
    // The return address is pushed by the callee.
    call(callee);
#else
    callAndPushReturnAddress(callee);
#endif
    return currentOffset();
}

uint32_t
MacroAssembler::callJit(Register callee)
{
    AutoProfilerCallInstrumentation profiler(*this);
    uint32_t ret = callJitNoProfiler(callee);
    return ret;
}

uint32_t
MacroAssembler::callJit(JitCode* callee)
{
    AutoProfilerCallInstrumentation profiler(*this);
    call(callee);
    return currentOffset();
}

uint32_t
MacroAssembler::callJit(TrampolinePtr code)
{
    AutoProfilerCallInstrumentation profiler(*this);
    call(code);
    return currentOffset();
}

void
MacroAssembler::makeFrameDescriptor(Register frameSizeReg, FrameType type, uint32_t headerSize)
{
    // See JitFrames.h for a description of the frame descriptor format.
    // The saved-frame bit is zero for new frames. See js::SavedStacks.

    lshiftPtr(Imm32(FRAMESIZE_SHIFT), frameSizeReg);

    headerSize = EncodeFrameHeaderSize(headerSize);
    orPtr(Imm32((headerSize << FRAME_HEADER_SIZE_SHIFT) | type), frameSizeReg);
}

void
MacroAssembler::pushStaticFrameDescriptor(FrameType type, uint32_t headerSize)
{
    uint32_t descriptor = MakeFrameDescriptor(framePushed(), type, headerSize);
    Push(Imm32(descriptor));
}

void
MacroAssembler::PushCalleeToken(Register callee, bool constructing)
{
    if (constructing) {
        orPtr(Imm32(CalleeToken_FunctionConstructing), callee);
        Push(callee);
        andPtr(Imm32(uint32_t(CalleeTokenMask)), callee);
    } else {
        static_assert(CalleeToken_Function == 0, "Non-constructing call requires no tagging");
        Push(callee);
    }
}

void
MacroAssembler::loadFunctionFromCalleeToken(Address token, Register dest)
{
#ifdef DEBUG
    Label ok;
    loadPtr(token, dest);
    andPtr(Imm32(uint32_t(~CalleeTokenMask)), dest);
    branchPtr(Assembler::Equal, dest, Imm32(CalleeToken_Function), &ok);
    branchPtr(Assembler::Equal, dest, Imm32(CalleeToken_FunctionConstructing), &ok);
    assumeUnreachable("Unexpected CalleeToken tag");
    bind(&ok);
#endif
    loadPtr(token, dest);
    andPtr(Imm32(uint32_t(CalleeTokenMask)), dest);
}

uint32_t
MacroAssembler::buildFakeExitFrame(Register scratch)
{
    mozilla::DebugOnly<uint32_t> initialDepth = framePushed();

    pushStaticFrameDescriptor(JitFrame_IonJS, ExitFrameLayout::Size());
    uint32_t retAddr = pushFakeReturnAddress(scratch);

    MOZ_ASSERT(framePushed() == initialDepth + ExitFrameLayout::Size());
    return retAddr;
}

// ===============================================================
// Exit frame footer.

void
MacroAssembler::enterExitFrame(Register cxreg, Register scratch, const VMFunction* f)
{
    MOZ_ASSERT(f);
    linkExitFrame(cxreg, scratch);
    // Push VMFunction pointer, to mark arguments.
    Push(ImmPtr(f));
}

void
MacroAssembler::enterFakeExitFrame(Register cxreg, Register scratch, ExitFrameType type)
{
    linkExitFrame(cxreg, scratch);
    Push(Imm32(int32_t(type)));
}

void
MacroAssembler::enterFakeExitFrameForNative(Register cxreg, Register scratch, bool isConstructing)
{
    enterFakeExitFrame(cxreg, scratch, isConstructing ? ExitFrameType::ConstructNative
                                                      : ExitFrameType::CallNative);
}

void
MacroAssembler::leaveExitFrame(size_t extraFrame)
{
    freeStack(ExitFooterFrame::Size() + extraFrame);
}

// ===============================================================
// Move instructions

void
MacroAssembler::moveValue(const ConstantOrRegister& src, const ValueOperand& dest)
{
    if (src.constant()) {
        moveValue(src.value(), dest);
        return;
    }

    moveValue(src.reg(), dest);
}

// ===============================================================
// Arithmetic functions

void
MacroAssembler::addPtr(ImmPtr imm, Register dest)
{
    addPtr(ImmWord(uintptr_t(imm.value)), dest);
}

// ===============================================================
// Branch functions

template <class L>
void
MacroAssembler::branchIfFalseBool(Register reg, L label)
{
    // Note that C++ bool is only 1 byte, so ignore the higher-order bits.
    branchTest32(Assembler::Zero, reg, Imm32(0xFF), label);
}

void
MacroAssembler::branchIfTrueBool(Register reg, Label* label)
{
    // Note that C++ bool is only 1 byte, so ignore the higher-order bits.
    branchTest32(Assembler::NonZero, reg, Imm32(0xFF), label);
}

void
MacroAssembler::branchIfRope(Register str, Label* label)
{
    Address flags(str, JSString::offsetOfFlags());
    branchTest32(Assembler::Zero, flags, Imm32(JSString::LINEAR_BIT), label);
}

void
MacroAssembler::branchIfRopeOrExternal(Register str, Register temp, Label* label)
{
    Address flags(str, JSString::offsetOfFlags());
    move32(Imm32(JSString::TYPE_FLAGS_MASK), temp);
    and32(flags, temp);

    branchTest32(Assembler::Zero, temp, Imm32(JSString::LINEAR_BIT), label);
    branch32(Assembler::Equal, temp, Imm32(JSString::EXTERNAL_FLAGS), label);
}

void
MacroAssembler::branchIfNotRope(Register str, Label* label)
{
    Address flags(str, JSString::offsetOfFlags());
    branchTest32(Assembler::NonZero, flags, Imm32(JSString::LINEAR_BIT), label);
}

void
MacroAssembler::branchLatin1String(Register string, Label* label)
{
    branchTest32(Assembler::NonZero, Address(string, JSString::offsetOfFlags()),
                 Imm32(JSString::LATIN1_CHARS_BIT), label);
}

void
MacroAssembler::branchTwoByteString(Register string, Label* label)
{
    branchTest32(Assembler::Zero, Address(string, JSString::offsetOfFlags()),
                 Imm32(JSString::LATIN1_CHARS_BIT), label);
}

void
MacroAssembler::branchIfFunctionHasNoJitEntry(Register fun, bool isConstructing, Label* label)
{
    // 16-bit loads are slow and unaligned 32-bit loads may be too so
    // perform an aligned 32-bit load and adjust the bitmask accordingly.

    static_assert(JSFunction::offsetOfNargs() % sizeof(uint32_t) == 0,
                  "The code in this function and the ones below must change");
    static_assert(JSFunction::offsetOfFlags() == JSFunction::offsetOfNargs() + 2,
                  "The code in this function and the ones below must change");

    Address address(fun, JSFunction::offsetOfNargs());
    int32_t bit = JSFunction::INTERPRETED;
    if (!isConstructing)
        bit |= JSFunction::WASM_OPTIMIZED;
    bit = IMM32_16ADJ(bit);
    branchTest32(Assembler::Zero, address, Imm32(bit), label);
}

void
MacroAssembler::branchIfInterpreted(Register fun, Label* label)
{
    // 16-bit loads are slow and unaligned 32-bit loads may be too so
    // perform an aligned 32-bit load and adjust the bitmask accordingly.
    Address address(fun, JSFunction::offsetOfNargs());
    int32_t bit = IMM32_16ADJ(JSFunction::INTERPRETED);
    branchTest32(Assembler::NonZero, address, Imm32(bit), label);
}

void
MacroAssembler::branchIfObjectEmulatesUndefined(Register objReg, Register scratch,
                                                Label* slowCheck, Label* label)
{
    // The branches to out-of-line code here implement a conservative version
    // of the JSObject::isWrapper test performed in EmulatesUndefined.
    loadObjClassUnsafe(objReg, scratch);

    branchTestClassIsProxy(true, scratch, slowCheck);

    Address flags(scratch, Class::offsetOfFlags());
    branchTest32(Assembler::NonZero, flags, Imm32(JSCLASS_EMULATES_UNDEFINED), label);
}

void
MacroAssembler::branchFunctionKind(Condition cond, JSFunction::FunctionKind kind, Register fun,
                                   Register scratch, Label* label)
{
    // 16-bit loads are slow and unaligned 32-bit loads may be too so
    // perform an aligned 32-bit load and adjust the bitmask accordingly.
    MOZ_ASSERT(JSFunction::offsetOfNargs() % sizeof(uint32_t) == 0);
    MOZ_ASSERT(JSFunction::offsetOfFlags() == JSFunction::offsetOfNargs() + 2);
    Address address(fun, JSFunction::offsetOfNargs());
    int32_t mask = IMM32_16ADJ(JSFunction::FUNCTION_KIND_MASK);
    int32_t bit = IMM32_16ADJ(kind << JSFunction::FUNCTION_KIND_SHIFT);
    load32(address, scratch);
    and32(Imm32(mask), scratch);
    branch32(cond, scratch, Imm32(bit), label);
}

void
MacroAssembler::branchTestObjClass(Condition cond, Register obj, const js::Class* clasp,
                                   Register scratch, Register spectreRegToZero, Label* label)
{
    MOZ_ASSERT(obj != scratch);
    MOZ_ASSERT(scratch != spectreRegToZero);

    loadPtr(Address(obj, JSObject::offsetOfGroup()), scratch);
    branchPtr(cond, Address(scratch, ObjectGroup::offsetOfClasp()), ImmPtr(clasp), label);

    if (JitOptions.spectreObjectMitigationsMisc)
        spectreZeroRegister(cond, scratch, spectreRegToZero);
}

void
MacroAssembler::branchTestObjClassNoSpectreMitigations(Condition cond, Register obj,
                                                       const js::Class* clasp,
                                                       Register scratch, Label* label)
{
    loadPtr(Address(obj, JSObject::offsetOfGroup()), scratch);
    branchPtr(cond, Address(scratch, ObjectGroup::offsetOfClasp()), ImmPtr(clasp), label);
}

void
MacroAssembler::branchTestObjClass(Condition cond, Register obj, const Address& clasp,
                                   Register scratch, Register spectreRegToZero, Label* label)
{
    MOZ_ASSERT(obj != scratch);
    MOZ_ASSERT(scratch != spectreRegToZero);

    loadPtr(Address(obj, JSObject::offsetOfGroup()), scratch);
    loadPtr(Address(scratch, ObjectGroup::offsetOfClasp()), scratch);
    branchPtr(cond, clasp, scratch, label);

    if (JitOptions.spectreObjectMitigationsMisc)
        spectreZeroRegister(cond, scratch, spectreRegToZero);
}

void
MacroAssembler::branchTestObjClassNoSpectreMitigations(Condition cond, Register obj,
                                                       const Address& clasp, Register scratch,
                                                       Label* label)
{
    MOZ_ASSERT(obj != scratch);
    loadPtr(Address(obj, JSObject::offsetOfGroup()), scratch);
    loadPtr(Address(scratch, ObjectGroup::offsetOfClasp()), scratch);
    branchPtr(cond, clasp, scratch, label);
}

void
MacroAssembler::branchTestObjShape(Condition cond, Register obj, const Shape* shape, Register scratch,
                                   Register spectreRegToZero, Label* label)
{
    MOZ_ASSERT(obj != scratch);
    MOZ_ASSERT(spectreRegToZero != scratch);

    if (JitOptions.spectreObjectMitigationsMisc)
        move32(Imm32(0), scratch);

    branchPtr(cond, Address(obj, ShapedObject::offsetOfShape()), ImmGCPtr(shape), label);

    if (JitOptions.spectreObjectMitigationsMisc)
        spectreMovePtr(cond, scratch, spectreRegToZero);
}

void
MacroAssembler::branchTestObjShapeNoSpectreMitigations(Condition cond, Register obj,
                                                       const Shape* shape, Label* label)
{
    branchPtr(cond, Address(obj, ShapedObject::offsetOfShape()), ImmGCPtr(shape), label);
}

void
MacroAssembler::branchTestObjShape(Condition cond, Register obj, Register shape, Register scratch,
                                   Register spectreRegToZero, Label* label)
{
    MOZ_ASSERT(obj != scratch);
    MOZ_ASSERT(obj != shape);
    MOZ_ASSERT(spectreRegToZero != scratch);

    if (JitOptions.spectreObjectMitigationsMisc)
        move32(Imm32(0), scratch);

    branchPtr(cond, Address(obj, ShapedObject::offsetOfShape()), shape, label);

    if (JitOptions.spectreObjectMitigationsMisc)
        spectreMovePtr(cond, scratch, spectreRegToZero);
}

void
MacroAssembler::branchTestObjShapeNoSpectreMitigations(Condition cond, Register obj, Register shape,
                                                       Label* label)
{
    branchPtr(cond, Address(obj, ShapedObject::offsetOfShape()), shape, label);
}

void
MacroAssembler::branchTestObjShapeUnsafe(Condition cond, Register obj, Register shape,
                                         Label* label)
{
    branchTestObjShapeNoSpectreMitigations(cond, obj, shape, label);
}

void
MacroAssembler::branchTestObjGroup(Condition cond, Register obj, const ObjectGroup* group,
                                   Register scratch, Register spectreRegToZero, Label* label)
{
    MOZ_ASSERT(obj != scratch);
    MOZ_ASSERT(spectreRegToZero != scratch);

    if (JitOptions.spectreObjectMitigationsMisc)
        move32(Imm32(0), scratch);

    branchPtr(cond, Address(obj, JSObject::offsetOfGroup()), ImmGCPtr(group), label);

    if (JitOptions.spectreObjectMitigationsMisc)
        spectreMovePtr(cond, scratch, spectreRegToZero);
}

void
MacroAssembler::branchTestObjGroupNoSpectreMitigations(Condition cond, Register obj,
                                                       const ObjectGroup* group, Label* label)
{
    branchPtr(cond, Address(obj, JSObject::offsetOfGroup()), ImmGCPtr(group), label);
}

void
MacroAssembler::branchTestObjGroupUnsafe(Condition cond, Register obj, const ObjectGroup* group,
                                         Label* label)
{
    branchTestObjGroupNoSpectreMitigations(cond, obj, group, label);
}

void
MacroAssembler::branchTestObjGroup(Condition cond, Register obj, Register group, Register scratch,
                                   Register spectreRegToZero, Label* label)
{
    MOZ_ASSERT(obj != scratch);
    MOZ_ASSERT(obj != group);
    MOZ_ASSERT(spectreRegToZero != scratch);

    if (JitOptions.spectreObjectMitigationsMisc)
        move32(Imm32(0), scratch);

    branchPtr(cond, Address(obj, JSObject::offsetOfGroup()), group, label);

    if (JitOptions.spectreObjectMitigationsMisc)
        spectreMovePtr(cond, scratch, spectreRegToZero);
}

void
MacroAssembler::branchTestObjGroupNoSpectreMitigations(Condition cond, Register obj, Register group,
                                                       Label* label)
{
    MOZ_ASSERT(obj != group);
    branchPtr(cond, Address(obj, JSObject::offsetOfGroup()), group, label);
}

void
MacroAssembler::branchTestClassIsProxy(bool proxy, Register clasp, Label* label)
{
    branchTest32(proxy ? Assembler::NonZero : Assembler::Zero,
                 Address(clasp, Class::offsetOfFlags()),
                 Imm32(JSCLASS_IS_PROXY), label);
}

void
MacroAssembler::branchTestObjectIsProxy(bool proxy, Register object, Register scratch, Label* label)
{
    loadObjClassUnsafe(object, scratch);
    branchTestClassIsProxy(proxy, scratch, label);
}

void
MacroAssembler::branchTestProxyHandlerFamily(Condition cond, Register proxy, Register scratch,
                                             const void* handlerp, Label* label)
{
    Address handlerAddr(proxy, ProxyObject::offsetOfHandler());
    loadPtr(handlerAddr, scratch);
    Address familyAddr(scratch, BaseProxyHandler::offsetOfFamily());
    branchPtr(cond, familyAddr, ImmPtr(handlerp), label);
}

void
MacroAssembler::branchTestNeedsIncrementalBarrier(Condition cond, Label* label)
{
    MOZ_ASSERT(cond == Zero || cond == NonZero);
    CompileZone* zone = GetJitContext()->compartment->zone();
    AbsoluteAddress needsBarrierAddr(zone->addressOfNeedsIncrementalBarrier());
    branchTest32(cond, needsBarrierAddr, Imm32(0x1), label);
}

void
MacroAssembler::branchTestMagicValue(Condition cond, const ValueOperand& val, JSWhyMagic why,
                                     Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    branchTestValue(cond, val, MagicValue(why), label);
}

void
MacroAssembler::branchDoubleNotInInt64Range(Address src, Register temp, Label* fail)
{
    // Tests if double is in [INT64_MIN; INT64_MAX] range
    uint32_t EXPONENT_MASK = 0x7ff00000;
    uint32_t EXPONENT_SHIFT = FloatingPoint<double>::kExponentShift - 32;
    uint32_t TOO_BIG_EXPONENT = (FloatingPoint<double>::kExponentBias + 63) << EXPONENT_SHIFT;

    load32(Address(src.base, src.offset + sizeof(int32_t)), temp);
    and32(Imm32(EXPONENT_MASK), temp);
    branch32(Assembler::GreaterThanOrEqual, temp, Imm32(TOO_BIG_EXPONENT), fail);
}

void
MacroAssembler::branchDoubleNotInUInt64Range(Address src, Register temp, Label* fail)
{
    // Note: returns failure on -0.0
    // Tests if double is in [0; UINT64_MAX] range
    // Take the sign also in the equation. That way we can compare in one test?
    uint32_t EXPONENT_MASK = 0xfff00000;
    uint32_t EXPONENT_SHIFT = FloatingPoint<double>::kExponentShift - 32;
    uint32_t TOO_BIG_EXPONENT = (FloatingPoint<double>::kExponentBias + 64) << EXPONENT_SHIFT;

    load32(Address(src.base, src.offset + sizeof(int32_t)), temp);
    and32(Imm32(EXPONENT_MASK), temp);
    branch32(Assembler::AboveOrEqual, temp, Imm32(TOO_BIG_EXPONENT), fail);
}

void
MacroAssembler::branchFloat32NotInInt64Range(Address src, Register temp, Label* fail)
{
    // Tests if float is in [INT64_MIN; INT64_MAX] range
    uint32_t EXPONENT_MASK = 0x7f800000;
    uint32_t EXPONENT_SHIFT = FloatingPoint<float>::kExponentShift;
    uint32_t TOO_BIG_EXPONENT = (FloatingPoint<float>::kExponentBias + 63) << EXPONENT_SHIFT;

    load32(src, temp);
    and32(Imm32(EXPONENT_MASK), temp);
    branch32(Assembler::GreaterThanOrEqual, temp, Imm32(TOO_BIG_EXPONENT), fail);
}

void
MacroAssembler::branchFloat32NotInUInt64Range(Address src, Register temp, Label* fail)
{
    // Note: returns failure on -0.0
    // Tests if float is in [0; UINT64_MAX] range
    // Take the sign also in the equation. That way we can compare in one test?
    uint32_t EXPONENT_MASK = 0xff800000;
    uint32_t EXPONENT_SHIFT = FloatingPoint<float>::kExponentShift;
    uint32_t TOO_BIG_EXPONENT = (FloatingPoint<float>::kExponentBias + 64) << EXPONENT_SHIFT;

    load32(src, temp);
    and32(Imm32(EXPONENT_MASK), temp);
    branch32(Assembler::AboveOrEqual, temp, Imm32(TOO_BIG_EXPONENT), fail);
}

// ========================================================================
// Canonicalization primitives.
void
MacroAssembler::canonicalizeFloat(FloatRegister reg)
{
    Label notNaN;
    branchFloat(DoubleOrdered, reg, reg, &notNaN);
    loadConstantFloat32(float(JS::GenericNaN()), reg);
    bind(&notNaN);
}

void
MacroAssembler::canonicalizeFloatIfDeterministic(FloatRegister reg)
{
#ifdef JS_MORE_DETERMINISTIC
    // See the comment in TypedArrayObjectTemplate::getIndexValue.
    canonicalizeFloat(reg);
#endif // JS_MORE_DETERMINISTIC
}

void
MacroAssembler::canonicalizeDouble(FloatRegister reg)
{
    Label notNaN;
    branchDouble(DoubleOrdered, reg, reg, &notNaN);
    loadConstantDouble(JS::GenericNaN(), reg);
    bind(&notNaN);
}

void
MacroAssembler::canonicalizeDoubleIfDeterministic(FloatRegister reg)
{
#ifdef JS_MORE_DETERMINISTIC
    // See the comment in TypedArrayObjectTemplate::getIndexValue.
    canonicalizeDouble(reg);
#endif // JS_MORE_DETERMINISTIC
}

// ========================================================================
// Memory access primitives.
template<class T> void
MacroAssembler::storeDouble(FloatRegister src, const T& dest)
{
    canonicalizeDoubleIfDeterministic(src);
    storeUncanonicalizedDouble(src, dest);
}

template void MacroAssembler::storeDouble(FloatRegister src, const Address& dest);
template void MacroAssembler::storeDouble(FloatRegister src, const BaseIndex& dest);

void
MacroAssembler::boxDouble(FloatRegister src, const Address& dest)
{
    storeDouble(src, dest);
}

template<class T> void
MacroAssembler::storeFloat32(FloatRegister src, const T& dest)
{
    canonicalizeFloatIfDeterministic(src);
    storeUncanonicalizedFloat32(src, dest);
}

template void MacroAssembler::storeFloat32(FloatRegister src, const Address& dest);
template void MacroAssembler::storeFloat32(FloatRegister src, const BaseIndex& dest);

//}}} check_macroassembler_style
// ===============================================================

#ifndef JS_CODEGEN_ARM64

template <typename T>
void
MacroAssembler::branchTestStackPtr(Condition cond, T t, Label* label)
{
    branchTestPtr(cond, getStackPointer(), t, label);
}

template <typename T>
void
MacroAssembler::branchStackPtr(Condition cond, T rhs, Label* label)
{
    branchPtr(cond, getStackPointer(), rhs, label);
}

template <typename T>
void
MacroAssembler::branchStackPtrRhs(Condition cond, T lhs, Label* label)
{
    branchPtr(cond, lhs, getStackPointer(), label);
}

template <typename T> void
MacroAssembler::addToStackPtr(T t)
{
    addPtr(t, getStackPointer());
}

template <typename T> void
MacroAssembler::addStackPtrTo(T t)
{
    addPtr(getStackPointer(), t);
}

void
MacroAssembler::reserveStack(uint32_t amount)
{
    subFromStackPtr(Imm32(amount));
    adjustFrame(amount);
}
#endif // !JS_CODEGEN_ARM64

template <typename EmitPreBarrier>
void
MacroAssembler::storeObjGroup(Register group, Register obj, EmitPreBarrier emitPreBarrier)
{
    MOZ_ASSERT(group != obj);
    Address groupAddr(obj, JSObject::offsetOfGroup());
    emitPreBarrier(*this, groupAddr);
    storePtr(group, groupAddr);
}

template <typename EmitPreBarrier>
void
MacroAssembler::storeObjGroup(ObjectGroup* group, Register obj, EmitPreBarrier emitPreBarrier)
{
    Address groupAddr(obj, JSObject::offsetOfGroup());
    emitPreBarrier(*this, groupAddr);
    storePtr(ImmGCPtr(group), groupAddr);
}

template <typename EmitPreBarrier>
void
MacroAssembler::storeObjShape(Register shape, Register obj, EmitPreBarrier emitPreBarrier)
{
    MOZ_ASSERT(shape != obj);
    Address shapeAddr(obj, ShapedObject::offsetOfShape());
    emitPreBarrier(*this, shapeAddr);
    storePtr(shape, shapeAddr);
}

template <typename EmitPreBarrier>
void
MacroAssembler::storeObjShape(Shape* shape, Register obj, EmitPreBarrier emitPreBarrier)
{
    Address shapeAddr(obj, ShapedObject::offsetOfShape());
    emitPreBarrier(*this, shapeAddr);
    storePtr(ImmGCPtr(shape), shapeAddr);
}

template <typename T>
void
MacroAssembler::storeObjectOrNull(Register src, const T& dest)
{
    Label notNull, done;
    branchTestPtr(Assembler::NonZero, src, src, &notNull);
    storeValue(NullValue(), dest);
    jump(&done);
    bind(&notNull);
    storeValue(JSVAL_TYPE_OBJECT, src, dest);
    bind(&done);
}

void
MacroAssembler::assertStackAlignment(uint32_t alignment, int32_t offset /* = 0 */)
{
#ifdef DEBUG
    Label ok, bad;
    MOZ_ASSERT(mozilla::IsPowerOfTwo(alignment));

    // Wrap around the offset to be a non-negative number.
    offset %= alignment;
    if (offset < 0)
        offset += alignment;

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

void
MacroAssembler::storeCallBoolResult(Register reg)
{
    if (reg != ReturnReg)
        mov(ReturnReg, reg);
    // C++ compilers like to only use the bottom byte for bools, but we
    // need to maintain the entire register.
    and32(Imm32(0xFF), reg);
}

void
MacroAssembler::storeCallInt32Result(Register reg)
{
#if JS_BITS_PER_WORD == 32
    storeCallPointerResult(reg);
#else
    // Ensure the upper 32 bits are cleared.
    move32(ReturnReg, reg);
#endif
}

void
MacroAssembler::storeCallResultValue(AnyRegister dest, JSValueType type)
{
    unboxValue(JSReturnOperand, dest, type);
}

void
MacroAssembler::storeCallResultValue(TypedOrValueRegister dest)
{
    if (dest.hasValue())
        storeCallResultValue(dest.valueReg());
    else
        storeCallResultValue(dest.typedReg(), ValueTypeFromMIRType(dest.type()));
}

} // namespace jit
} // namespace js

#endif /* jit_MacroAssembler_inl_h */
