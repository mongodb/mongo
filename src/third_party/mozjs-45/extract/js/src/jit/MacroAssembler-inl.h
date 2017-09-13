/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MacroAssembler_inl_h
#define jit_MacroAssembler_inl_h

#include "jit/MacroAssembler.h"

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

namespace js {
namespace jit {

//{{{ check_macroassembler_style
// ===============================================================
// Frame manipulation functions.

uint32_t
MacroAssembler::framePushed() const
{
    return framePushed_;
}

void
MacroAssembler::setFramePushed(uint32_t framePushed)
{
    framePushed_ = framePushed;
}

void
MacroAssembler::adjustFrame(int32_t value)
{
    MOZ_ASSERT_IF(value < 0, framePushed_ >= uint32_t(-value));
    setFramePushed(framePushed_ + value);
}

void
MacroAssembler::implicitPop(uint32_t bytes)
{
    MOZ_ASSERT(bytes % sizeof(intptr_t) == 0);
    MOZ_ASSERT(bytes <= INT32_MAX);
    adjustFrame(-int32_t(bytes));
}

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
MacroAssembler::call(const wasm::CallSiteDesc& desc, const Register reg)
{
    CodeOffset l = call(reg);
    append(desc, l, framePushed());
}

void
MacroAssembler::call(const wasm::CallSiteDesc& desc, Label* label)
{
    CodeOffset l = call(label);
    append(desc, l, framePushed());
}

void
MacroAssembler::call(const wasm::CallSiteDesc& desc, AsmJSInternalCallee callee)
{
    CodeOffset l = callWithPatch();
    append(desc, l, framePushed(), callee.index);
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

template <typename T> void
MacroAssembler::callWithABI(const T& fun, MoveOp::Type result)
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

void
MacroAssembler::makeFrameDescriptor(Register frameSizeReg, FrameType type)
{
    // See JitFrames.h for a description of the frame descriptor format.

    lshiftPtr(Imm32(FRAMESIZE_SHIFT), frameSizeReg);
    // The saved-frame bit is zero for new frames. See js::SavedStacks.
    orPtr(Imm32(type), frameSizeReg);
}

void
MacroAssembler::pushStaticFrameDescriptor(FrameType type)
{
    uint32_t descriptor = MakeFrameDescriptor(framePushed(), type);
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

    pushStaticFrameDescriptor(JitFrame_IonJS);
    uint32_t retAddr = pushFakeReturnAddress(scratch);

    MOZ_ASSERT(framePushed() == initialDepth + ExitFrameLayout::Size());
    return retAddr;
}

// ===============================================================
// Exit frame footer.

void
MacroAssembler::PushStubCode()
{
    // Make sure that we do not erase an existing self-reference.
    MOZ_ASSERT(!hasSelfReference());
    selfReferencePatch_ = PushWithPatch(ImmWord(-1));
}

void
MacroAssembler::enterExitFrame(const VMFunction* f)
{
    linkExitFrame();
    // Push the JitCode pointer. (Keep the code alive, when on the stack)
    PushStubCode();
    // Push VMFunction pointer, to mark arguments.
    Push(ImmPtr(f));
}

void
MacroAssembler::enterFakeExitFrame(enum ExitFrameTokenValues token)
{
    linkExitFrame();
    Push(Imm32(token));
    Push(ImmPtr(nullptr));
}

void
MacroAssembler::enterFakeExitFrameForNative(bool isConstructing)
{
    enterFakeExitFrame(isConstructing ? ConstructNativeExitFrameLayoutToken
                                      : CallNativeExitFrameLayoutToken);
}

void
MacroAssembler::leaveExitFrame(size_t extraFrame)
{
    freeStack(ExitFooterFrame::Size() + extraFrame);
}

bool
MacroAssembler::hasSelfReference() const
{
    return selfReferencePatch_.bound();
}

//}}} check_macroassembler_style
// ===============================================================

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

} // namespace jit
} // namespace js

#endif /* jit_MacroAssembler_inl_h */
