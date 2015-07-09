/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/shared/MacroAssembler-x86-shared.h"

#include "jit/JitFrames.h"
#include "jit/MacroAssembler.h"

using namespace js;
using namespace js::jit;

void
MacroAssembler::PushRegsInMask(RegisterSet set, FloatRegisterSet simdSet)
{
    FloatRegisterSet doubleSet(FloatRegisterSet::Subtract(set.fpus(), simdSet));
    MOZ_ASSERT_IF(simdSet.empty(), doubleSet == set.fpus());
    unsigned numSimd = simdSet.size();
    unsigned numDouble = doubleSet.size();
    int32_t diffF = numDouble * sizeof(double) + numSimd * Simd128DataSize;
    int32_t diffG = set.gprs().size() * sizeof(intptr_t);

    // On x86, always use push to push the integer registers, as it's fast
    // on modern hardware and it's a small instruction.
    for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); iter++) {
        diffG -= sizeof(intptr_t);
        Push(*iter);
    }
    MOZ_ASSERT(diffG == 0);

    reserveStack(diffF);
    for (FloatRegisterBackwardIterator iter(doubleSet); iter.more(); iter++) {
        diffF -= sizeof(double);
        numDouble -= 1;
        storeDouble(*iter, Address(StackPointer, diffF));
    }
    MOZ_ASSERT(numDouble == 0);
    for (FloatRegisterBackwardIterator iter(simdSet); iter.more(); iter++) {
        diffF -= Simd128DataSize;
        numSimd -= 1;
        // XXX how to choose the right move type?
        storeUnalignedInt32x4(*iter, Address(StackPointer, diffF));
    }
    MOZ_ASSERT(numSimd == 0);
    MOZ_ASSERT(diffF == 0);
}

void
MacroAssembler::PopRegsInMaskIgnore(RegisterSet set, RegisterSet ignore, FloatRegisterSet simdSet)
{
    FloatRegisterSet doubleSet(FloatRegisterSet::Subtract(set.fpus(), simdSet));
    MOZ_ASSERT_IF(simdSet.empty(), doubleSet == set.fpus());
    unsigned numSimd = simdSet.size();
    unsigned numDouble = doubleSet.size();
    int32_t diffG = set.gprs().size() * sizeof(intptr_t);
    int32_t diffF = numDouble * sizeof(double) + numSimd * Simd128DataSize;
    const int32_t reservedG = diffG;
    const int32_t reservedF = diffF;

    for (FloatRegisterBackwardIterator iter(simdSet); iter.more(); iter++) {
        diffF -= Simd128DataSize;
        numSimd -= 1;
        if (!ignore.has(*iter))
            // XXX how to choose the right move type?
            loadUnalignedInt32x4(Address(StackPointer, diffF), *iter);
    }
    MOZ_ASSERT(numSimd == 0);
    for (FloatRegisterBackwardIterator iter(doubleSet); iter.more(); iter++) {
        diffF -= sizeof(double);
        numDouble -= 1;
        if (!ignore.has(*iter))
            loadDouble(Address(StackPointer, diffF), *iter);
    }
    freeStack(reservedF);
    MOZ_ASSERT(numDouble == 0);
    MOZ_ASSERT(diffF == 0);

    // On x86, use pop to pop the integer registers, if we're not going to
    // ignore any slots, as it's fast on modern hardware and it's a small
    // instruction.
    if (ignore.empty(false)) {
        for (GeneralRegisterForwardIterator iter(set.gprs()); iter.more(); iter++) {
            diffG -= sizeof(intptr_t);
            Pop(*iter);
        }
    } else {
        for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); iter++) {
            diffG -= sizeof(intptr_t);
            if (!ignore.has(*iter))
                loadPtr(Address(StackPointer, diffG), *iter);
        }
        freeStack(reservedG);
    }
    MOZ_ASSERT(diffG == 0);
}

// Note: this function clobbers the input register.
void
MacroAssembler::clampDoubleToUint8(FloatRegister input, Register output)
{
    MOZ_ASSERT(input != ScratchDoubleReg);
    Label positive, done;

    // <= 0 or NaN --> 0
    zeroDouble(ScratchDoubleReg);
    branchDouble(DoubleGreaterThan, input, ScratchDoubleReg, &positive);
    {
        move32(Imm32(0), output);
        jump(&done);
    }

    bind(&positive);

    // Add 0.5 and truncate.
    loadConstantDouble(0.5, ScratchDoubleReg);
    addDouble(ScratchDoubleReg, input);

    Label outOfRange;

    // Truncate to int32 and ensure the result <= 255. This relies on the
    // processor setting output to a value > 255 for doubles outside the int32
    // range (for instance 0x80000000).
    vcvttsd2si(input, output);
    branch32(Assembler::Above, output, Imm32(255), &outOfRange);
    {
        // Check if we had a tie.
        convertInt32ToDouble(output, ScratchDoubleReg);
        branchDouble(DoubleNotEqual, input, ScratchDoubleReg, &done);

        // It was a tie. Mask out the ones bit to get an even value.
        // See also js_TypedArray_uint8_clamp_double.
        and32(Imm32(~1), output);
        jump(&done);
    }

    // > 255 --> 255
    bind(&outOfRange);
    {
        move32(Imm32(255), output);
    }

    bind(&done);
}

// Builds an exit frame on the stack, with a return address to an internal
// non-function. Returns offset to be passed to markSafepointAt().
void
MacroAssemblerX86Shared::buildFakeExitFrame(Register scratch, uint32_t* offset)
{
    mozilla::DebugOnly<uint32_t> initialDepth = framePushed();

    CodeLabel cl;
    mov(cl.dest(), scratch);

    uint32_t descriptor = MakeFrameDescriptor(framePushed(), JitFrame_IonJS);
    Push(Imm32(descriptor));
    Push(scratch);

    bind(cl.src());
    *offset = currentOffset();

    MOZ_ASSERT(framePushed() == initialDepth + ExitFrameLayout::Size());
    addCodeLabel(cl);
}

void
MacroAssemblerX86Shared::callWithExitFrame(Label* target)
{
    uint32_t descriptor = MakeFrameDescriptor(framePushed(), JitFrame_IonJS);
    Push(Imm32(descriptor));
    call(target);
}

void
MacroAssemblerX86Shared::callWithExitFrame(JitCode* target)
{
    uint32_t descriptor = MakeFrameDescriptor(framePushed(), JitFrame_IonJS);
    Push(Imm32(descriptor));
    call(target);
}

void
MacroAssembler::alignFrameForICArguments(AfterICSaveLive& aic)
{
    // Exists for MIPS compatibility.
}

void
MacroAssembler::restoreFrameAlignmentForICArguments(AfterICSaveLive& aic)
{
    // Exists for MIPS compatibility.
}

bool
MacroAssemblerX86Shared::buildOOLFakeExitFrame(void* fakeReturnAddr)
{
    uint32_t descriptor = MakeFrameDescriptor(framePushed(), JitFrame_IonJS);
    Push(Imm32(descriptor));
    Push(ImmPtr(fakeReturnAddr));
    return true;
}

void
MacroAssemblerX86Shared::branchNegativeZero(FloatRegister reg,
                                            Register scratch,
                                            Label* label,
                                            bool maybeNonZero)
{
    // Determines whether the low double contained in the XMM register reg
    // is equal to -0.0.

#if defined(JS_CODEGEN_X86)
    Label nonZero;

    // if not already compared to zero
    if (maybeNonZero) {
        // Compare to zero. Lets through {0, -0}.
        zeroDouble(ScratchDoubleReg);

        // If reg is non-zero, jump to nonZero.
        branchDouble(DoubleNotEqual, reg, ScratchDoubleReg, &nonZero);
    }
    // Input register is either zero or negative zero. Retrieve sign of input.
    vmovmskpd(reg, scratch);

    // If reg is 1 or 3, input is negative zero.
    // If reg is 0 or 2, input is a normal zero.
    branchTest32(NonZero, scratch, Imm32(1), label);

    bind(&nonZero);
#elif defined(JS_CODEGEN_X64)
    vmovq(reg, scratch);
    cmpq(Imm32(1), scratch);
    j(Overflow, label);
#endif
}

void
MacroAssemblerX86Shared::branchNegativeZeroFloat32(FloatRegister reg,
                                                   Register scratch,
                                                   Label* label)
{
    vmovd(reg, scratch);
    cmp32(scratch, Imm32(1));
    j(Overflow, label);
}
