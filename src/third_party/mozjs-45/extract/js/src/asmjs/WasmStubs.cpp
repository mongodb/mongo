/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2015 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "asmjs/WasmStubs.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/EnumeratedRange.h"

#include "asmjs/AsmJSModule.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::ArrayLength;
using mozilla::MakeEnumeratedRange;

typedef Vector<MIRType, 8, SystemAllocPolicy> MIRTypeVector;
typedef ABIArgIter<MIRTypeVector> ABIArgMIRTypeIter;
typedef ABIArgIter<MallocSig::ArgVector> ABIArgValTypeIter;

static void
AssertStackAlignment(MacroAssembler& masm, uint32_t alignment, uint32_t addBeforeAssert = 0)
{
    MOZ_ASSERT((sizeof(AsmJSFrame) + masm.framePushed() + addBeforeAssert) % alignment == 0);
    masm.assertStackAlignment(alignment, addBeforeAssert);
}

static unsigned
StackDecrementForCall(MacroAssembler& masm, uint32_t alignment, unsigned bytesToPush)
{
    return StackDecrementForCall(alignment, sizeof(AsmJSFrame) + masm.framePushed(), bytesToPush);
}

template <class VectorT>
static unsigned
StackArgBytes(const VectorT& args)
{
    ABIArgIter<VectorT> iter(args);
    while (!iter.done())
        iter++;
    return iter.stackBytesConsumedSoFar();
}

template <class VectorT>
static unsigned
StackDecrementForCall(MacroAssembler& masm, uint32_t alignment, const VectorT& args,
                      unsigned extraBytes = 0)
{
    return StackDecrementForCall(masm, alignment, StackArgBytes(args) + extraBytes);
}

#if defined(JS_CODEGEN_ARM)
// The ARM system ABI also includes d15 & s31 in the non volatile float registers.
// Also exclude lr (a.k.a. r14) as we preserve it manually)
static const LiveRegisterSet NonVolatileRegs =
    LiveRegisterSet(GeneralRegisterSet(Registers::NonVolatileMask&
                                       ~(uint32_t(1) << Registers::lr)),
                    FloatRegisterSet(FloatRegisters::NonVolatileMask
                                     | (1ULL << FloatRegisters::d15)
                                     | (1ULL << FloatRegisters::s31)));
#else
static const LiveRegisterSet NonVolatileRegs =
    LiveRegisterSet(GeneralRegisterSet(Registers::NonVolatileMask),
                    FloatRegisterSet(FloatRegisters::NonVolatileMask));
#endif

#if defined(JS_CODEGEN_MIPS32)
// Mips is using one more double slot due to stack alignment for double values.
// Look at MacroAssembler::PushRegsInMask(RegisterSet set)
static const unsigned FramePushedAfterSave = NonVolatileRegs.gprs().size() * sizeof(intptr_t) +
                                             NonVolatileRegs.fpus().getPushSizeInBytes() +
                                             sizeof(double);
#elif defined(JS_CODEGEN_NONE)
static const unsigned FramePushedAfterSave = 0;
#else
static const unsigned FramePushedAfterSave = NonVolatileRegs.gprs().size() * sizeof(intptr_t)
                                           + NonVolatileRegs.fpus().getPushSizeInBytes();
#endif
static const unsigned FramePushedForEntrySP = FramePushedAfterSave + sizeof(void*);

// Generate a stub that enters wasm from a C++ caller via the native ABI.
// The signature of the entry point is AsmJSModule::CodePtr. The exported wasm
// function has an ABI derived from its specific signature, so this function
// must map from the ABI of CodePtr to the export's signature's ABI.
static bool
GenerateEntry(MacroAssembler& masm, AsmJSModule& module, unsigned exportIndex,
              const FuncOffsetVector& funcOffsets)
{
    AsmJSModule::ExportedFunction& exp = module.exportedFunction(exportIndex);
    if (exp.isChangeHeap())
        return true;

    masm.haltingAlign(CodeAlignment);

    AsmJSOffsets offsets;
    offsets.begin = masm.currentOffset();

    // Save the return address if it wasn't already saved by the call insn.
#if defined(JS_CODEGEN_ARM)
    masm.push(lr);
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    masm.push(ra);
#elif defined(JS_CODEGEN_X86)
    static const unsigned EntryFrameSize = sizeof(void*);
#endif

    // Save all caller non-volatile registers before we clobber them here and in
    // the asm.js callee (which does not preserve non-volatile registers).
    masm.setFramePushed(0);
    masm.PushRegsInMask(NonVolatileRegs);
    MOZ_ASSERT(masm.framePushed() == FramePushedAfterSave);

    // ARM and MIPS/MIPS64 have a globally-pinned GlobalReg (x64 uses RIP-relative
    // addressing, x86 uses immediates in effective addresses). For the
    // AsmJSGlobalRegBias addition, see Assembler-(mips,arm).h.
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    masm.movePtr(IntArgReg1, GlobalReg);
    masm.addPtr(Imm32(AsmJSGlobalRegBias), GlobalReg);
#endif

    // ARM, MIPS/MIPS64 and x64 have a globally-pinned HeapReg (x86 uses immediates in
    // effective addresses). Loading the heap register depends on the global
    // register already having been loaded.
    masm.loadAsmJSHeapRegisterFromGlobalData();

    // Put the 'argv' argument into a non-argument/return register so that we
    // can use 'argv' while we fill in the arguments for the asm.js callee.
    // Also, save 'argv' on the stack so that we can recover it after the call.
    // Use a second non-argument/return register as temporary scratch.
    Register argv = ABIArgGenerator::NonArgReturnReg0;
    Register scratch = ABIArgGenerator::NonArgReturnReg1;
#if defined(JS_CODEGEN_X86)
    masm.loadPtr(Address(masm.getStackPointer(), EntryFrameSize + masm.framePushed()), argv);
#else
    masm.movePtr(IntArgReg0, argv);
#endif
    masm.Push(argv);

    // Save the stack pointer to the saved non-volatile registers. We will use
    // this on two paths: normal return and exceptional return. Since
    // loadAsmJSActivation uses GlobalReg, we must do this after loading
    // GlobalReg.
    MOZ_ASSERT(masm.framePushed() == FramePushedForEntrySP);
    masm.loadAsmJSActivation(scratch);
    masm.storeStackPtr(Address(scratch, AsmJSActivation::offsetOfEntrySP()));

    // Dynamically align the stack since ABIStackAlignment is not necessarily
    // AsmJSStackAlignment. We'll use entrySP to recover the original stack
    // pointer on return.
    masm.andToStackPtr(Imm32(~(AsmJSStackAlignment - 1)));

    // Bump the stack for the call.
    masm.reserveStack(AlignBytes(StackArgBytes(exp.sig().args()), AsmJSStackAlignment));

    // Copy parameters out of argv and into the registers/stack-slots specified by
    // the system ABI.
    for (ABIArgValTypeIter iter(exp.sig().args()); !iter.done(); iter++) {
        unsigned argOffset = iter.index() * sizeof(AsmJSModule::EntryArg);
        Address src(argv, argOffset);
        MIRType type = iter.mirType();
        switch (iter->kind()) {
          case ABIArg::GPR:
            masm.load32(src, iter->gpr());
            break;
#ifdef JS_CODEGEN_REGISTER_PAIR
          case ABIArg::GPR_PAIR:
            MOZ_CRASH("AsmJS uses hardfp for function calls.");
            break;
#endif
          case ABIArg::FPU: {
            static_assert(sizeof(AsmJSModule::EntryArg) >= jit::Simd128DataSize,
                          "EntryArg must be big enough to store SIMD values");
            switch (type) {
              case MIRType_Int32x4:
                masm.loadUnalignedInt32x4(src, iter->fpu());
                break;
              case MIRType_Float32x4:
                masm.loadUnalignedFloat32x4(src, iter->fpu());
                break;
              case MIRType_Double:
                masm.loadDouble(src, iter->fpu());
                break;
              case MIRType_Float32:
                masm.loadFloat32(src, iter->fpu());
                break;
              default:
                MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("unexpected FPU type");
                break;
            }
            break;
          }
          case ABIArg::Stack:
            switch (type) {
              case MIRType_Int32:
                masm.load32(src, scratch);
                masm.storePtr(scratch, Address(masm.getStackPointer(), iter->offsetFromArgBase()));
                break;
              case MIRType_Double:
                masm.loadDouble(src, ScratchDoubleReg);
                masm.storeDouble(ScratchDoubleReg, Address(masm.getStackPointer(), iter->offsetFromArgBase()));
                break;
              case MIRType_Float32:
                masm.loadFloat32(src, ScratchFloat32Reg);
                masm.storeFloat32(ScratchFloat32Reg, Address(masm.getStackPointer(), iter->offsetFromArgBase()));
                break;
              case MIRType_Int32x4:
                masm.loadUnalignedInt32x4(src, ScratchSimd128Reg);
                masm.storeAlignedInt32x4(ScratchSimd128Reg,
                                         Address(masm.getStackPointer(), iter->offsetFromArgBase()));
                break;
              case MIRType_Float32x4:
                masm.loadUnalignedFloat32x4(src, ScratchSimd128Reg);
                masm.storeAlignedFloat32x4(ScratchSimd128Reg,
                                           Address(masm.getStackPointer(), iter->offsetFromArgBase()));
                break;
              default:
                MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("unexpected stack arg type");
            }
            break;
        }
    }

    // Call into the real function.
    masm.assertStackAlignment(AsmJSStackAlignment);
    Label target;
    target.bind(funcOffsets[exp.funcIndex()]);
    masm.call(CallSiteDesc(CallSiteDesc::Relative), &target);

    // Recover the stack pointer value before dynamic alignment.
    masm.loadAsmJSActivation(scratch);
    masm.loadStackPtr(Address(scratch, AsmJSActivation::offsetOfEntrySP()));
    masm.setFramePushed(FramePushedForEntrySP);

    // Recover the 'argv' pointer which was saved before aligning the stack.
    masm.Pop(argv);

    // Store the return value in argv[0]
    switch (exp.sig().ret()) {
      case ExprType::Void:
        break;
      case ExprType::I32:
        masm.storeValue(JSVAL_TYPE_INT32, ReturnReg, Address(argv, 0));
        break;
      case ExprType::I64:
        MOZ_CRASH("no int64 in asm.js");
      case ExprType::F32:
        masm.convertFloat32ToDouble(ReturnFloat32Reg, ReturnDoubleReg);
        // Fall through as ReturnDoubleReg now contains a Double
      case ExprType::F64:
        masm.canonicalizeDouble(ReturnDoubleReg);
        masm.storeDouble(ReturnDoubleReg, Address(argv, 0));
        break;
      case ExprType::I32x4:
        // We don't have control on argv alignment, do an unaligned access.
        masm.storeUnalignedInt32x4(ReturnSimd128Reg, Address(argv, 0));
        break;
      case ExprType::F32x4:
        // We don't have control on argv alignment, do an unaligned access.
        masm.storeUnalignedFloat32x4(ReturnSimd128Reg, Address(argv, 0));
        break;
    }

    // Restore clobbered non-volatile registers of the caller.
    masm.PopRegsInMask(NonVolatileRegs);
    MOZ_ASSERT(masm.framePushed() == 0);

    masm.move32(Imm32(true), ReturnReg);
    masm.ret();

    if (masm.oom())
        return false;

    exp.initCodeOffset(offsets.begin);
    offsets.end = masm.currentOffset();
    return module.addCodeRange(AsmJSModule::CodeRange::Entry, offsets);
}

// Generate a thunk that updates fp before calling the given builtin so that
// both the builtin and the calling function show up in profiler stacks. (This
// thunk is dynamically patched in when profiling is enabled.) Since the thunk
// pushes an AsmJSFrame on the stack, that means we must rebuild the stack
// frame. Fortunately, these are low arity functions and everything is passed in
// regs on everything but x86 anyhow.
//
// NB: Since this thunk is being injected at system ABI callsites, it must
//     preserve the argument registers (going in) and the return register
//     (coming out) and preserve non-volatile registers.
static bool
GenerateBuiltinThunk(MacroAssembler& masm, AsmJSModule& module, Builtin builtin)
{
    MIRTypeVector args;
    switch (builtin) {
      case Builtin::ToInt32:
        MOZ_ALWAYS_TRUE(args.append(MIRType_Int32));
        break;
#if defined(JS_CODEGEN_ARM)
      case Builtin::aeabi_idivmod:
      case Builtin::aeabi_uidivmod:
        MOZ_ALWAYS_TRUE(args.append(MIRType_Int32));
        MOZ_ALWAYS_TRUE(args.append(MIRType_Int32));
        break;
      case Builtin::AtomicCmpXchg:
        MOZ_ALWAYS_TRUE(args.append(MIRType_Int32));
        MOZ_ALWAYS_TRUE(args.append(MIRType_Int32));
        MOZ_ALWAYS_TRUE(args.append(MIRType_Int32));
        MOZ_ALWAYS_TRUE(args.append(MIRType_Int32));
        break;
      case Builtin::AtomicXchg:
      case Builtin::AtomicFetchAdd:
      case Builtin::AtomicFetchSub:
      case Builtin::AtomicFetchAnd:
      case Builtin::AtomicFetchOr:
      case Builtin::AtomicFetchXor:
        MOZ_ALWAYS_TRUE(args.append(MIRType_Int32));
        MOZ_ALWAYS_TRUE(args.append(MIRType_Int32));
        MOZ_ALWAYS_TRUE(args.append(MIRType_Int32));
        break;
#endif
      case Builtin::SinD:
      case Builtin::CosD:
      case Builtin::TanD:
      case Builtin::ASinD:
      case Builtin::ACosD:
      case Builtin::ATanD:
      case Builtin::CeilD:
      case Builtin::FloorD:
      case Builtin::ExpD:
      case Builtin::LogD:
        MOZ_ALWAYS_TRUE(args.append(MIRType_Double));
        break;
      case Builtin::ModD:
      case Builtin::PowD:
      case Builtin::ATan2D:
        MOZ_ALWAYS_TRUE(args.append(MIRType_Double));
        MOZ_ALWAYS_TRUE(args.append(MIRType_Double));
        break;
      case Builtin::CeilF:
      case Builtin::FloorF:
        MOZ_ALWAYS_TRUE(args.append(MIRType_Float32));
        break;
      case Builtin::Limit:
        MOZ_CRASH("Bad builtin");
    }

    MOZ_ASSERT(args.length() <= 4);
    static_assert(MIRTypeVector::InlineLength >= 4, "infallibility of append");

    MOZ_ASSERT(masm.framePushed() == 0);
    uint32_t framePushed = StackDecrementForCall(masm, ABIStackAlignment, args);

    AsmJSProfilingOffsets offsets;
    GenerateAsmJSExitPrologue(masm, framePushed, ExitReason(builtin), &offsets);

    for (ABIArgMIRTypeIter i(args); !i.done(); i++) {
        if (i->kind() != ABIArg::Stack)
            continue;
#if !defined(JS_CODEGEN_ARM)
        unsigned offsetToCallerStackArgs = sizeof(AsmJSFrame) + masm.framePushed();
        Address srcAddr(masm.getStackPointer(), offsetToCallerStackArgs + i->offsetFromArgBase());
        Address dstAddr(masm.getStackPointer(), i->offsetFromArgBase());
        if (i.mirType() == MIRType_Int32 || i.mirType() == MIRType_Float32) {
            masm.load32(srcAddr, ABIArgGenerator::NonArg_VolatileReg);
            masm.store32(ABIArgGenerator::NonArg_VolatileReg, dstAddr);
        } else {
            MOZ_ASSERT(i.mirType() == MIRType_Double);
            masm.loadDouble(srcAddr, ScratchDoubleReg);
            masm.storeDouble(ScratchDoubleReg, dstAddr);
        }
#else
        MOZ_CRASH("Architecture should have enough registers for all builtin calls");
#endif
    }

    AssertStackAlignment(masm, ABIStackAlignment);
    masm.call(BuiltinToImmediate(builtin));

    GenerateAsmJSExitEpilogue(masm, framePushed, ExitReason(builtin), &offsets);

    if (masm.oom())
        return false;

    offsets.end = masm.currentOffset();
    return module.addBuiltinThunkCodeRange(builtin, offsets);
}

static void
FillArgumentArray(MacroAssembler& masm, const MallocSig::ArgVector& args, unsigned argOffset,
                  unsigned offsetToCallerStackArgs, Register scratch)
{
    for (ABIArgValTypeIter i(args); !i.done(); i++) {
        Address dstAddr(masm.getStackPointer(), argOffset + i.index() * sizeof(Value));
        switch (i->kind()) {
          case ABIArg::GPR:
            masm.storeValue(JSVAL_TYPE_INT32, i->gpr(), dstAddr);
            break;
#ifdef JS_CODEGEN_REGISTER_PAIR
          case ABIArg::GPR_PAIR:
            MOZ_CRASH("AsmJS uses hardfp for function calls.");
            break;
#endif
          case ABIArg::FPU:
            masm.canonicalizeDouble(i->fpu());
            masm.storeDouble(i->fpu(), dstAddr);
            break;
          case ABIArg::Stack:
            if (i.mirType() == MIRType_Int32) {
                Address src(masm.getStackPointer(), offsetToCallerStackArgs + i->offsetFromArgBase());
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
                masm.load32(src, scratch);
                masm.storeValue(JSVAL_TYPE_INT32, scratch, dstAddr);
#else
                masm.memIntToValue(src, dstAddr);
#endif
            } else {
                MOZ_ASSERT(i.mirType() == MIRType_Double);
                Address src(masm.getStackPointer(), offsetToCallerStackArgs + i->offsetFromArgBase());
                masm.loadDouble(src, ScratchDoubleReg);
                masm.canonicalizeDouble(ScratchDoubleReg);
                masm.storeDouble(ScratchDoubleReg, dstAddr);
            }
            break;
        }
    }
}

// If an FFI detaches its heap (viz., via ArrayBuffer.transfer), it must
// call change-heap to another heap (viz., the new heap returned by transfer)
// before returning to asm.js code. If the application fails to do this (if the
// heap pointer is null), jump to a stub.
static void
CheckForHeapDetachment(MacroAssembler& masm, const AsmJSModule& module, Register scratch,
                       Label* onDetached)
{
    if (!module.hasArrayView())
        return;

    MOZ_ASSERT(int(masm.framePushed()) >= int(ShadowStackSpace));
    AssertStackAlignment(masm, ABIStackAlignment);
#if defined(JS_CODEGEN_X86)
    CodeOffset offset = masm.movlWithPatch(PatchedAbsoluteAddress(), scratch);
    masm.append(AsmJSGlobalAccess(offset, HeapGlobalDataOffset));
    masm.branchTestPtr(Assembler::Zero, scratch, scratch, onDetached);
#else
    masm.branchTestPtr(Assembler::Zero, HeapReg, HeapReg, onDetached);
#endif
}

// Generate a stub that is called via the internal ABI derived from the
// signature of the exit and calls into an appropriate InvokeFromAsmJS_* C++
// function, having boxed all the ABI arguments into a homogeneous Value array.
static bool
GenerateInterpExit(MacroAssembler& masm, AsmJSModule& module, unsigned exitIndex,
                   Label* throwLabel, Label* onDetached)
{
    AsmJSModule::Exit& exit = module.exit(exitIndex);

    masm.setFramePushed(0);

    // Argument types for InvokeFromAsmJS_*:
    static const MIRType typeArray[] = { MIRType_Pointer,   // exitDatum
                                         MIRType_Int32,     // argc
                                         MIRType_Pointer }; // argv
    MIRTypeVector invokeArgTypes;
    MOZ_ALWAYS_TRUE(invokeArgTypes.append(typeArray, ArrayLength(typeArray)));

    // At the point of the call, the stack layout shall be (sp grows to the left):
    //   | stack args | padding | Value argv[] | padding | retaddr | caller stack args |
    // The padding between stack args and argv ensures that argv is aligned. The
    // padding between argv and retaddr ensures that sp is aligned.
    unsigned argOffset = AlignBytes(StackArgBytes(invokeArgTypes), sizeof(double));
    unsigned argBytes = Max<size_t>(1, exit.sig().args().length()) * sizeof(Value);
    unsigned framePushed = StackDecrementForCall(masm, ABIStackAlignment, argOffset + argBytes);

    AsmJSProfilingOffsets offsets;
    GenerateAsmJSExitPrologue(masm, framePushed, ExitReason::Slow, &offsets);

    // Fill the argument array.
    unsigned offsetToCallerStackArgs = sizeof(AsmJSFrame) + masm.framePushed();
    Register scratch = ABIArgGenerator::NonArgReturnReg0;
    FillArgumentArray(masm, exit.sig().args(), argOffset, offsetToCallerStackArgs, scratch);

    // Prepare the arguments for the call to InvokeFromAsmJS_*.
    ABIArgMIRTypeIter i(invokeArgTypes);

    // argument 0: exitIndex
    if (i->kind() == ABIArg::GPR)
        masm.mov(ImmWord(exitIndex), i->gpr());
    else
        masm.store32(Imm32(exitIndex), Address(masm.getStackPointer(), i->offsetFromArgBase()));
    i++;

    // argument 1: argc
    unsigned argc = exit.sig().args().length();
    if (i->kind() == ABIArg::GPR)
        masm.mov(ImmWord(argc), i->gpr());
    else
        masm.store32(Imm32(argc), Address(masm.getStackPointer(), i->offsetFromArgBase()));
    i++;

    // argument 2: argv
    Address argv(masm.getStackPointer(), argOffset);
    if (i->kind() == ABIArg::GPR) {
        masm.computeEffectiveAddress(argv, i->gpr());
    } else {
        masm.computeEffectiveAddress(argv, scratch);
        masm.storePtr(scratch, Address(masm.getStackPointer(), i->offsetFromArgBase()));
    }
    i++;
    MOZ_ASSERT(i.done());

    // Make the call, test whether it succeeded, and extract the return value.
    AssertStackAlignment(masm, ABIStackAlignment);
    switch (exit.sig().ret()) {
      case ExprType::Void:
        masm.call(SymbolicAddress::InvokeFromAsmJS_Ignore);
        masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, throwLabel);
        break;
      case ExprType::I32:
        masm.call(SymbolicAddress::InvokeFromAsmJS_ToInt32);
        masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, throwLabel);
        masm.unboxInt32(argv, ReturnReg);
        break;
      case ExprType::I64:
        MOZ_CRASH("no int64 in asm.js");
      case ExprType::F32:
        MOZ_CRASH("Float32 shouldn't be returned from a FFI");
      case ExprType::F64:
        masm.call(SymbolicAddress::InvokeFromAsmJS_ToNumber);
        masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, throwLabel);
        masm.loadDouble(argv, ReturnDoubleReg);
        break;
      case ExprType::I32x4:
      case ExprType::F32x4:
        MOZ_CRASH("SIMD types shouldn't be returned from a FFI");
    }

    // The heap pointer may have changed during the FFI, so reload it and test
    // for detachment.
    masm.loadAsmJSHeapRegisterFromGlobalData();
    CheckForHeapDetachment(masm, module, ABIArgGenerator::NonReturn_VolatileReg0, onDetached);

    GenerateAsmJSExitEpilogue(masm, framePushed, ExitReason::Slow, &offsets);

    if (masm.oom())
        return false;

    offsets.end = masm.currentOffset();
    exit.initInterpOffset(offsets.begin);
    return module.addCodeRange(AsmJSModule::CodeRange::SlowFFI, offsets);
}

#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
static const unsigned MaybeSavedGlobalReg = sizeof(void*);
#else
static const unsigned MaybeSavedGlobalReg = 0;
#endif

// Generate a stub that is called via the internal ABI derived from the
// signature of the exit and calls into a compatible Ion-compiled JIT function,
// having boxed all the ABI arguments into the Ion stack frame layout.
static bool
GenerateIonExit(MacroAssembler& masm, AsmJSModule& module, unsigned exitIndex,
                Label* throwLabel, Label* onDetached)
{
    AsmJSModule::Exit& exit = module.exit(exitIndex);

    masm.setFramePushed(0);

    // Ion calls use the following stack layout (sp grows to the left):
    //   | retaddr | descriptor | callee | argc | this | arg1..N |
    // After the Ion frame, the global register (if present) is saved since Ion
    // does not preserve non-volatile regs. Also, unlike most ABIs, Ion requires
    // that sp be JitStackAlignment-aligned *after* pushing the return address.
    static_assert(AsmJSStackAlignment >= JitStackAlignment, "subsumes");
    unsigned sizeOfRetAddr = sizeof(void*);
    unsigned ionFrameBytes = 3 * sizeof(void*) + (1 + exit.sig().args().length()) * sizeof(Value);
    unsigned totalIonBytes = sizeOfRetAddr + ionFrameBytes + MaybeSavedGlobalReg;
    unsigned ionFramePushed = StackDecrementForCall(masm, JitStackAlignment, totalIonBytes) -
                              sizeOfRetAddr;

    AsmJSProfilingOffsets offsets;
    GenerateAsmJSExitPrologue(masm, ionFramePushed, ExitReason::Jit, &offsets);

    // 1. Descriptor
    size_t argOffset = 0;
    uint32_t descriptor = MakeFrameDescriptor(ionFramePushed, JitFrame_Entry);
    masm.storePtr(ImmWord(uintptr_t(descriptor)), Address(masm.getStackPointer(), argOffset));
    argOffset += sizeof(size_t);

    // 2. Callee
    Register callee = ABIArgGenerator::NonArgReturnReg0;   // live until call
    Register scratch = ABIArgGenerator::NonArgReturnReg1;  // repeatedly clobbered

    // 2.1. Get ExitDatum
    unsigned globalDataOffset = module.exit(exitIndex).globalDataOffset();
#if defined(JS_CODEGEN_X64)
    masm.append(AsmJSGlobalAccess(masm.leaRipRelative(callee), globalDataOffset));
#elif defined(JS_CODEGEN_X86)
    masm.append(AsmJSGlobalAccess(masm.movlWithPatch(Imm32(0), callee), globalDataOffset));
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    masm.computeEffectiveAddress(Address(GlobalReg, globalDataOffset - AsmJSGlobalRegBias), callee);
#endif

    // 2.2. Get callee
    masm.loadPtr(Address(callee, offsetof(AsmJSModule::ExitDatum, fun)), callee);

    // 2.3. Save callee
    masm.storePtr(callee, Address(masm.getStackPointer(), argOffset));
    argOffset += sizeof(size_t);

    // 2.4. Load callee executable entry point
    masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), callee);
    masm.loadBaselineOrIonNoArgCheck(callee, callee, nullptr);

    // 3. Argc
    unsigned argc = exit.sig().args().length();
    masm.storePtr(ImmWord(uintptr_t(argc)), Address(masm.getStackPointer(), argOffset));
    argOffset += sizeof(size_t);

    // 4. |this| value
    masm.storeValue(UndefinedValue(), Address(masm.getStackPointer(), argOffset));
    argOffset += sizeof(Value);

    // 5. Fill the arguments
    unsigned offsetToCallerStackArgs = ionFramePushed + sizeof(AsmJSFrame);
    FillArgumentArray(masm, exit.sig().args(), argOffset, offsetToCallerStackArgs, scratch);
    argOffset += exit.sig().args().length() * sizeof(Value);
    MOZ_ASSERT(argOffset == ionFrameBytes);

    // 6. Jit code will clobber all registers, even non-volatiles. GlobalReg and
    //    HeapReg are removed from the general register set for asm.js code, so
    //    these will not have been saved by the caller like all other registers,
    //    so they must be explicitly preserved. Only save GlobalReg since
    //    HeapReg must be reloaded (from global data) after the call since the
    //    heap may change during the FFI call.
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    static_assert(MaybeSavedGlobalReg == sizeof(void*), "stack frame accounting");
    masm.storePtr(GlobalReg, Address(masm.getStackPointer(), ionFrameBytes));
#endif

    {
        // Enable Activation.
        //
        // This sequence requires four registers, and needs to preserve the 'callee'
        // register, so there are five live registers.
        MOZ_ASSERT(callee == AsmJSIonExitRegCallee);
        Register reg0 = AsmJSIonExitRegE0;
        Register reg1 = AsmJSIonExitRegE1;
        Register reg2 = AsmJSIonExitRegE2;
        Register reg3 = AsmJSIonExitRegE3;

        // The following is inlined:
        //   JSContext* cx = activation->cx();
        //   Activation* act = cx->runtime()->activation();
        //   act.active_ = true;
        //   act.prevJitTop_ = cx->runtime()->jitTop;
        //   act.prevJitJSContext_ = cx->runtime()->jitJSContext;
        //   cx->runtime()->jitJSContext = cx;
        //   act.prevJitActivation_ = cx->runtime()->jitActivation;
        //   cx->runtime()->jitActivation = act;
        //   act.prevProfilingActivation_ = cx->runtime()->profilingActivation;
        //   cx->runtime()->profilingActivation_ = act;
        // On the ARM store8() uses the secondScratchReg (lr) as a temp.
        size_t offsetOfActivation = JSRuntime::offsetOfActivation();
        size_t offsetOfJitTop = offsetof(JSRuntime, jitTop);
        size_t offsetOfJitJSContext = offsetof(JSRuntime, jitJSContext);
        size_t offsetOfJitActivation = offsetof(JSRuntime, jitActivation);
        size_t offsetOfProfilingActivation = JSRuntime::offsetOfProfilingActivation();
        masm.loadAsmJSActivation(reg0);
        masm.loadPtr(Address(reg0, AsmJSActivation::offsetOfContext()), reg3);
        masm.loadPtr(Address(reg3, JSContext::offsetOfRuntime()), reg0);
        masm.loadPtr(Address(reg0, offsetOfActivation), reg1);

        //   act.active_ = true;
        masm.store8(Imm32(1), Address(reg1, JitActivation::offsetOfActiveUint8()));

        //   act.prevJitTop_ = cx->runtime()->jitTop;
        masm.loadPtr(Address(reg0, offsetOfJitTop), reg2);
        masm.storePtr(reg2, Address(reg1, JitActivation::offsetOfPrevJitTop()));

        //   act.prevJitJSContext_ = cx->runtime()->jitJSContext;
        masm.loadPtr(Address(reg0, offsetOfJitJSContext), reg2);
        masm.storePtr(reg2, Address(reg1, JitActivation::offsetOfPrevJitJSContext()));
        //   cx->runtime()->jitJSContext = cx;
        masm.storePtr(reg3, Address(reg0, offsetOfJitJSContext));

        //   act.prevJitActivation_ = cx->runtime()->jitActivation;
        masm.loadPtr(Address(reg0, offsetOfJitActivation), reg2);
        masm.storePtr(reg2, Address(reg1, JitActivation::offsetOfPrevJitActivation()));
        //   cx->runtime()->jitActivation = act;
        masm.storePtr(reg1, Address(reg0, offsetOfJitActivation));

        //   act.prevProfilingActivation_ = cx->runtime()->profilingActivation;
        masm.loadPtr(Address(reg0, offsetOfProfilingActivation), reg2);
        masm.storePtr(reg2, Address(reg1, Activation::offsetOfPrevProfiling()));
        //   cx->runtime()->profilingActivation_ = act;
        masm.storePtr(reg1, Address(reg0, offsetOfProfilingActivation));
    }

    AssertStackAlignment(masm, JitStackAlignment, sizeOfRetAddr);
    masm.callJitNoProfiler(callee);
    AssertStackAlignment(masm, JitStackAlignment, sizeOfRetAddr);

    {
        // Disable Activation.
        //
        // This sequence needs three registers, and must preserve the JSReturnReg_Data and
        // JSReturnReg_Type, so there are five live registers.
        MOZ_ASSERT(JSReturnReg_Data == AsmJSIonExitRegReturnData);
        MOZ_ASSERT(JSReturnReg_Type == AsmJSIonExitRegReturnType);
        Register reg0 = AsmJSIonExitRegD0;
        Register reg1 = AsmJSIonExitRegD1;
        Register reg2 = AsmJSIonExitRegD2;

        // The following is inlined:
        //   rt->profilingActivation = prevProfilingActivation_;
        //   rt->activation()->active_ = false;
        //   rt->jitTop = prevJitTop_;
        //   rt->jitJSContext = prevJitJSContext_;
        //   rt->jitActivation = prevJitActivation_;
        // On the ARM store8() uses the secondScratchReg (lr) as a temp.
        size_t offsetOfActivation = JSRuntime::offsetOfActivation();
        size_t offsetOfJitTop = offsetof(JSRuntime, jitTop);
        size_t offsetOfJitJSContext = offsetof(JSRuntime, jitJSContext);
        size_t offsetOfJitActivation = offsetof(JSRuntime, jitActivation);
        size_t offsetOfProfilingActivation = JSRuntime::offsetOfProfilingActivation();

        masm.movePtr(SymbolicAddress::Runtime, reg0);
        masm.loadPtr(Address(reg0, offsetOfActivation), reg1);

        //   rt->jitTop = prevJitTop_;
        masm.loadPtr(Address(reg1, JitActivation::offsetOfPrevJitTop()), reg2);
        masm.storePtr(reg2, Address(reg0, offsetOfJitTop));

        //   rt->profilingActivation = rt->activation()->prevProfiling_;
        masm.loadPtr(Address(reg1, Activation::offsetOfPrevProfiling()), reg2);
        masm.storePtr(reg2, Address(reg0, offsetOfProfilingActivation));

        //   rt->activation()->active_ = false;
        masm.store8(Imm32(0), Address(reg1, JitActivation::offsetOfActiveUint8()));

        //   rt->jitJSContext = prevJitJSContext_;
        masm.loadPtr(Address(reg1, JitActivation::offsetOfPrevJitJSContext()), reg2);
        masm.storePtr(reg2, Address(reg0, offsetOfJitJSContext));

        //   rt->jitActivation = prevJitActivation_;
        masm.loadPtr(Address(reg1, JitActivation::offsetOfPrevJitActivation()), reg2);
        masm.storePtr(reg2, Address(reg0, offsetOfJitActivation));
    }

    // Reload the global register since Ion code can clobber any register.
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    static_assert(MaybeSavedGlobalReg == sizeof(void*), "stack frame accounting");
    masm.loadPtr(Address(masm.getStackPointer(), ionFrameBytes), GlobalReg);
#endif

    // As explained above, the frame was aligned for Ion such that
    //   (sp + sizeof(void*)) % JitStackAlignment == 0
    // But now we possibly want to call one of several different C++ functions,
    // so subtract the sizeof(void*) so that sp is aligned for an ABI call.
    static_assert(ABIStackAlignment <= JitStackAlignment, "subsumes");
    masm.reserveStack(sizeOfRetAddr);
    unsigned nativeFramePushed = masm.framePushed();
    AssertStackAlignment(masm, ABIStackAlignment);

    masm.branchTestMagic(Assembler::Equal, JSReturnOperand, throwLabel);

    Label oolConvert;
    switch (exit.sig().ret()) {
      case ExprType::Void:
        break;
      case ExprType::I32:
        masm.convertValueToInt32(JSReturnOperand, ReturnDoubleReg, ReturnReg, &oolConvert,
                                 /* -0 check */ false);
        break;
      case ExprType::I64:
        MOZ_CRASH("no int64 in asm.js");
      case ExprType::F32:
        MOZ_CRASH("Float shouldn't be returned from a FFI");
      case ExprType::F64:
        masm.convertValueToDouble(JSReturnOperand, ReturnDoubleReg, &oolConvert);
        break;
      case ExprType::I32x4:
      case ExprType::F32x4:
        MOZ_CRASH("SIMD types shouldn't be returned from a FFI");
    }

    Label done;
    masm.bind(&done);

    // The heap pointer has to be reloaded anyway since Ion could have clobbered
    // it. Additionally, the FFI may have detached the heap buffer.
    masm.loadAsmJSHeapRegisterFromGlobalData();
    CheckForHeapDetachment(masm, module, ABIArgGenerator::NonReturn_VolatileReg0, onDetached);

    GenerateAsmJSExitEpilogue(masm, masm.framePushed(), ExitReason::Jit, &offsets);

    if (oolConvert.used()) {
        masm.bind(&oolConvert);
        masm.setFramePushed(nativeFramePushed);

        // Coercion calls use the following stack layout (sp grows to the left):
        //   | args | padding | Value argv[1] | padding | exit AsmJSFrame |
        MIRTypeVector coerceArgTypes;
        JS_ALWAYS_TRUE(coerceArgTypes.append(MIRType_Pointer));
        unsigned offsetToCoerceArgv = AlignBytes(StackArgBytes(coerceArgTypes), sizeof(Value));
        MOZ_ASSERT(nativeFramePushed >= offsetToCoerceArgv + sizeof(Value));
        AssertStackAlignment(masm, ABIStackAlignment);

        // Store return value into argv[0]
        masm.storeValue(JSReturnOperand, Address(masm.getStackPointer(), offsetToCoerceArgv));

        // argument 0: argv
        ABIArgMIRTypeIter i(coerceArgTypes);
        Address argv(masm.getStackPointer(), offsetToCoerceArgv);
        if (i->kind() == ABIArg::GPR) {
            masm.computeEffectiveAddress(argv, i->gpr());
        } else {
            masm.computeEffectiveAddress(argv, scratch);
            masm.storePtr(scratch, Address(masm.getStackPointer(), i->offsetFromArgBase()));
        }
        i++;
        MOZ_ASSERT(i.done());

        // Call coercion function
        AssertStackAlignment(masm, ABIStackAlignment);
        switch (exit.sig().ret()) {
          case ExprType::I32:
            masm.call(SymbolicAddress::CoerceInPlace_ToInt32);
            masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, throwLabel);
            masm.unboxInt32(Address(masm.getStackPointer(), offsetToCoerceArgv), ReturnReg);
            break;
          case ExprType::F64:
            masm.call(SymbolicAddress::CoerceInPlace_ToNumber);
            masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, throwLabel);
            masm.loadDouble(Address(masm.getStackPointer(), offsetToCoerceArgv), ReturnDoubleReg);
            break;
          default:
            MOZ_CRASH("Unsupported convert type");
        }

        masm.jump(&done);
        masm.setFramePushed(0);
    }

    MOZ_ASSERT(masm.framePushed() == 0);

    if (masm.oom())
        return false;

    offsets.end = masm.currentOffset();
    exit.initJitOffset(offsets.begin);
    return module.addCodeRange(AsmJSModule::CodeRange::JitFFI, offsets);
}

// Generate a stub that is called when returning from an exit where the module's
// buffer has been detached. This stub first calls a C++ function to report an
// exception and then jumps to the generic throw stub to pop everything off the
// stack.
static bool
GenerateOnDetachedExit(MacroAssembler& masm, AsmJSModule& module, Label* onDetached,
                       Label* throwLabel)
{
    masm.haltingAlign(CodeAlignment);
    AsmJSOffsets offsets;
    offsets.begin = masm.currentOffset();
    masm.bind(onDetached);

    // For now, OnDetached always throws (see OnDetached comment).
    masm.assertStackAlignment(ABIStackAlignment);
    masm.call(SymbolicAddress::OnDetached);
    masm.jump(throwLabel);

    if (masm.oom())
        return false;

    offsets.end = masm.currentOffset();
    return module.addCodeRange(AsmJSModule::CodeRange::Inline, offsets);
}

// Generate a stub that is called immediately after the prologue when there is a
// stack overflow. This stub calls a C++ function to report the error and then
// jumps to the throw stub to pop the activation.
static bool
GenerateStackOverflowExit(MacroAssembler& masm, AsmJSModule& module, Label* throwLabel)
{
    masm.haltingAlign(CodeAlignment);
    AsmJSOffsets offsets;
    offsets.begin = masm.currentOffset();
    masm.bind(masm.asmStackOverflowLabel());

    // If we reach here via the non-profiling prologue, AsmJSActivation::fp has
    // not been updated. To enable stack unwinding from C++, store to it now. If
    // we reached here via the profiling prologue, we'll just store the same
    // value again. Do not update AsmJSFrame::callerFP as it is not necessary in
    // the non-profiling case (there is no return path from this point) and, in
    // the profiling case, it is already correct.
    Register activation = ABIArgGenerator::NonArgReturnReg0;
    masm.loadAsmJSActivation(activation);
    masm.storePtr(masm.getStackPointer(), Address(activation, AsmJSActivation::offsetOfFP()));

    // Prepare the stack for calling C++.
    if (uint32_t d = StackDecrementForCall(ABIStackAlignment, sizeof(AsmJSFrame), ShadowStackSpace))
        masm.subFromStackPtr(Imm32(d));

    // No need to restore the stack; the throw stub pops everything.
    masm.assertStackAlignment(ABIStackAlignment);
    masm.call(SymbolicAddress::ReportOverRecursed);
    masm.jump(throwLabel);

    if (masm.oom())
        return false;

    offsets.end = masm.currentOffset();
    return module.addCodeRange(AsmJSModule::CodeRange::Inline, offsets);
}

// Generate a stub that is called from the synchronous, inline interrupt checks
// when the interrupt flag is set. This stub calls the C++ function to handle
// the interrupt which returns whether execution has been interrupted.
static bool
GenerateSyncInterruptExit(MacroAssembler& masm, AsmJSModule& module, Label* throwLabel)
{
    masm.setFramePushed(0);
    unsigned framePushed = StackDecrementForCall(masm, ABIStackAlignment, ShadowStackSpace);

    AsmJSProfilingOffsets offsets;
    GenerateAsmJSExitPrologue(masm, framePushed, ExitReason::Interrupt, &offsets,
                              masm.asmSyncInterruptLabel());

    AssertStackAlignment(masm, ABIStackAlignment);
    masm.call(SymbolicAddress::HandleExecutionInterrupt);
    masm.branchIfFalseBool(ReturnReg, throwLabel);

    GenerateAsmJSExitEpilogue(masm, framePushed, ExitReason::Interrupt, &offsets);

    if (masm.oom())
        return false;

    offsets.end = masm.currentOffset();
    return module.addCodeRange(AsmJSModule::CodeRange::Interrupt, offsets);
}

// Generate a stub that is jumped to from an out-of-bounds heap access when
// there are throwing semantics. This stub calls a C++ function to report an
// error and then jumps to the throw stub to pop the activation.
static bool
GenerateConversionErrorExit(MacroAssembler& masm, AsmJSModule& module, Label* throwLabel)
{
    masm.haltingAlign(CodeAlignment);
    AsmJSOffsets offsets;
    offsets.begin = masm.currentOffset();
    masm.bind(masm.asmOnConversionErrorLabel());

    // sp can be anything at this point, so ensure it is aligned when calling
    // into C++.  We unconditionally jump to throw so don't worry about restoring sp.
    masm.andToStackPtr(Imm32(~(ABIStackAlignment - 1)));

    // OnOutOfBounds always throws.
    masm.assertStackAlignment(ABIStackAlignment);
    masm.call(SymbolicAddress::OnImpreciseConversion);
    masm.jump(throwLabel);

    if (masm.oom())
        return false;

    offsets.end = masm.currentOffset();
    module.setOnOutOfBoundsExitOffset(offsets.begin);
    return module.addCodeRange(AsmJSModule::CodeRange::Inline, offsets);
}

// Generate a stub that is jumped to from an out-of-bounds heap access when
// there are throwing semantics. This stub calls a C++ function to report an
// error and then jumps to the throw stub to pop the activation.
static bool
GenerateOutOfBoundsExit(MacroAssembler& masm, AsmJSModule& module, Label* throwLabel)
{
    masm.haltingAlign(CodeAlignment);
    AsmJSOffsets offsets;
    offsets.begin = masm.currentOffset();
    masm.bind(masm.asmOnOutOfBoundsLabel());

    // sp can be anything at this point, so ensure it is aligned when calling
    // into C++.  We unconditionally jump to throw so don't worry about restoring sp.
    masm.andToStackPtr(Imm32(~(ABIStackAlignment - 1)));

    // OnOutOfBounds always throws.
    masm.assertStackAlignment(ABIStackAlignment);
    masm.call(SymbolicAddress::OnOutOfBounds);
    masm.jump(throwLabel);

    if (masm.oom())
        return false;

    offsets.end = masm.currentOffset();
    module.setOnOutOfBoundsExitOffset(offsets.begin);
    return module.addCodeRange(AsmJSModule::CodeRange::Inline, offsets);
}

static const LiveRegisterSet AllRegsExceptSP(
    GeneralRegisterSet(Registers::AllMask&
                       ~(uint32_t(1) << Registers::StackPointer)),
    FloatRegisterSet(FloatRegisters::AllMask));

// The async interrupt-callback exit is called from arbitrarily-interrupted asm.js
// code. That means we must first save *all* registers and restore *all*
// registers (except the stack pointer) when we resume. The address to resume to
// (assuming that js::HandleExecutionInterrupt doesn't indicate that the
// execution should be aborted) is stored in AsmJSActivation::resumePC_.
// Unfortunately, loading this requires a scratch register which we don't have
// after restoring all registers. To hack around this, push the resumePC on the
// stack so that it can be popped directly into PC.
static bool
GenerateAsyncInterruptExit(MacroAssembler& masm, AsmJSModule& module, Label* throwLabel)
{
    masm.haltingAlign(CodeAlignment);
    AsmJSOffsets offsets;
    offsets.begin = masm.currentOffset();

#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    // Be very careful here not to perturb the machine state before saving it
    // to the stack. In particular, add/sub instructions may set conditions in
    // the flags register.
    masm.push(Imm32(0));            // space for resumePC
    masm.pushFlags();               // after this we are safe to use sub
    masm.setFramePushed(0);         // set to zero so we can use masm.framePushed() below
    masm.PushRegsInMask(AllRegsExceptSP); // save all GP/FP registers (except SP)

    Register scratch = ABIArgGenerator::NonArgReturnReg0;

    // Store resumePC into the reserved space.
    masm.loadAsmJSActivation(scratch);
    masm.loadPtr(Address(scratch, AsmJSActivation::offsetOfResumePC()), scratch);
    masm.storePtr(scratch, Address(masm.getStackPointer(), masm.framePushed() + sizeof(void*)));

    // We know that StackPointer is word-aligned, but not necessarily
    // stack-aligned, so we need to align it dynamically.
    masm.moveStackPtrTo(ABIArgGenerator::NonVolatileReg);
    masm.andToStackPtr(Imm32(~(ABIStackAlignment - 1)));
    if (ShadowStackSpace)
        masm.subFromStackPtr(Imm32(ShadowStackSpace));

    masm.assertStackAlignment(ABIStackAlignment);
    masm.call(SymbolicAddress::HandleExecutionInterrupt);

    masm.branchIfFalseBool(ReturnReg, throwLabel);

    // Restore the StackPointer to its position before the call.
    masm.moveToStackPtr(ABIArgGenerator::NonVolatileReg);

    // Restore the machine state to before the interrupt.
    masm.PopRegsInMask(AllRegsExceptSP); // restore all GP/FP registers (except SP)
    masm.popFlags();              // after this, nothing that sets conditions
    masm.ret();                   // pop resumePC into PC
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    // Reserve space to store resumePC.
    masm.subFromStackPtr(Imm32(sizeof(intptr_t)));
    // set to zero so we can use masm.framePushed() below.
    masm.setFramePushed(0);
    // When this platform supports SIMD extensions, we'll need to push high lanes
    // of SIMD registers as well.
    JS_STATIC_ASSERT(!SupportsSimd);
    // save all registers,except sp. After this stack is alligned.
    masm.PushRegsInMask(AllRegsExceptSP);

    // Save the stack pointer in a non-volatile register.
    masm.moveStackPtrTo(s0);
    // Align the stack.
    masm.ma_and(StackPointer, StackPointer, Imm32(~(ABIStackAlignment - 1)));

    // Store resumePC into the reserved space.
    masm.loadAsmJSActivation(IntArgReg0);
    masm.loadPtr(Address(IntArgReg0, AsmJSActivation::offsetOfResumePC()), IntArgReg1);
    masm.storePtr(IntArgReg1, Address(s0, masm.framePushed()));

    // MIPS ABI requires rewserving stack for registes $a0 to $a3.
    masm.subFromStackPtr(Imm32(4 * sizeof(intptr_t)));

    masm.assertStackAlignment(ABIStackAlignment);
    masm.call(SymbolicAddress::HandleExecutionInterrupt);

    masm.addToStackPtr(Imm32(4 * sizeof(intptr_t)));

    masm.branchIfFalseBool(ReturnReg, throwLabel);

    // This will restore stack to the address before the call.
    masm.moveToStackPtr(s0);
    masm.PopRegsInMask(AllRegsExceptSP);

    // Pop resumePC into PC. Clobber HeapReg to make the jump and restore it
    // during jump delay slot.
    masm.pop(HeapReg);
    masm.as_jr(HeapReg);
    masm.loadAsmJSHeapRegisterFromGlobalData();
#elif defined(JS_CODEGEN_ARM)
    masm.setFramePushed(0);         // set to zero so we can use masm.framePushed() below

    // Save all GPR, except the stack pointer.
    masm.PushRegsInMask(LiveRegisterSet(
                            GeneralRegisterSet(Registers::AllMask & ~(1<<Registers::sp)),
                            FloatRegisterSet(uint32_t(0))));

    // Save both the APSR and FPSCR in non-volatile registers.
    masm.as_mrs(r4);
    masm.as_vmrs(r5);
    // Save the stack pointer in a non-volatile register.
    masm.mov(sp,r6);
    // Align the stack.
    masm.ma_and(Imm32(~7), sp, sp);

    // Store resumePC into the return PC stack slot.
    masm.loadAsmJSActivation(IntArgReg0);
    masm.loadPtr(Address(IntArgReg0, AsmJSActivation::offsetOfResumePC()), IntArgReg1);
    masm.storePtr(IntArgReg1, Address(r6, 14 * sizeof(uint32_t*)));

    // When this platform supports SIMD extensions, we'll need to push and pop
    // high lanes of SIMD registers as well.

    // Save all FP registers
    JS_STATIC_ASSERT(!SupportsSimd);
    masm.PushRegsInMask(LiveRegisterSet(GeneralRegisterSet(0),
                                        FloatRegisterSet(FloatRegisters::AllDoubleMask)));

    masm.assertStackAlignment(ABIStackAlignment);
    masm.call(SymbolicAddress::HandleExecutionInterrupt);

    masm.branchIfFalseBool(ReturnReg, throwLabel);

    // Restore the machine state to before the interrupt. this will set the pc!

    // Restore all FP registers
    masm.PopRegsInMask(LiveRegisterSet(GeneralRegisterSet(0),
                                       FloatRegisterSet(FloatRegisters::AllDoubleMask)));
    masm.mov(r6,sp);
    masm.as_vmsr(r5);
    masm.as_msr(r4);
    // Restore all GP registers
    masm.startDataTransferM(IsLoad, sp, IA, WriteBack);
    masm.transferReg(r0);
    masm.transferReg(r1);
    masm.transferReg(r2);
    masm.transferReg(r3);
    masm.transferReg(r4);
    masm.transferReg(r5);
    masm.transferReg(r6);
    masm.transferReg(r7);
    masm.transferReg(r8);
    masm.transferReg(r9);
    masm.transferReg(r10);
    masm.transferReg(r11);
    masm.transferReg(r12);
    masm.transferReg(lr);
    masm.finishDataTransfer();
    masm.ret();
#elif defined(JS_CODEGEN_ARM64)
    MOZ_CRASH();
#elif defined (JS_CODEGEN_NONE)
    MOZ_CRASH();
#else
# error "Unknown architecture!"
#endif

    if (masm.oom())
        return false;

    offsets.end = masm.currentOffset();
    module.setAsyncInterruptOffset(offsets.begin);
    return module.addCodeRange(AsmJSModule::CodeRange::Inline, offsets);
}

// If an exception is thrown, simply pop all frames (since asm.js does not
// contain try/catch). To do this:
//  1. Restore 'sp' to it's value right after the PushRegsInMask in GenerateEntry.
//  2. PopRegsInMask to restore the caller's non-volatile registers.
//  3. Return (to CallAsmJS).
static bool
GenerateThrowStub(MacroAssembler& masm, AsmJSModule& module, Label* throwLabel)
{
    masm.haltingAlign(CodeAlignment);
    AsmJSOffsets offsets;
    offsets.begin = masm.currentOffset();
    masm.bind(throwLabel);

    // We are about to pop all frames in this AsmJSActivation. Set fp to null to
    // maintain the invariant that fp is either null or pointing to a valid
    // frame.
    Register scratch = ABIArgGenerator::NonArgReturnReg0;
    masm.loadAsmJSActivation(scratch);
    masm.storePtr(ImmWord(0), Address(scratch, AsmJSActivation::offsetOfFP()));

    masm.setFramePushed(FramePushedForEntrySP);
    masm.loadStackPtr(Address(scratch, AsmJSActivation::offsetOfEntrySP()));
    masm.Pop(scratch);
    masm.PopRegsInMask(NonVolatileRegs);
    MOZ_ASSERT(masm.framePushed() == 0);

    masm.mov(ImmWord(0), ReturnReg);
    masm.ret();

    if (masm.oom())
        return false;

    offsets.end = masm.currentOffset();
    return module.addCodeRange(AsmJSModule::CodeRange::Inline, offsets);
}

bool
wasm::GenerateStubs(MacroAssembler& masm, AsmJSModule& module, const FuncOffsetVector& funcOffsets)
{
    for (unsigned i = 0; i < module.numExportedFunctions(); i++) {
        if (!GenerateEntry(masm, module, i, funcOffsets))
            return false;
    }

    for (auto builtin : MakeEnumeratedRange(Builtin::Limit)) {
        if (!GenerateBuiltinThunk(masm, module, builtin))
            return false;
    }

    Label onThrow;

    {
        Label onDetached;

        for (size_t i = 0; i < module.numExits(); i++) {
            if (!GenerateInterpExit(masm, module, i, &onThrow, &onDetached))
                return false;
            if (!GenerateIonExit(masm, module, i, &onThrow, &onDetached))
                return false;
        }

        if (onDetached.used()) {
            if (!GenerateOnDetachedExit(masm, module, &onDetached, &onThrow))
                return false;
        }
    }

    if (masm.asmStackOverflowLabel()->used()) {
        if (!GenerateStackOverflowExit(masm, module, &onThrow))
            return false;
    }

    if (masm.asmSyncInterruptLabel()->used()) {
        if (!GenerateSyncInterruptExit(masm, module, &onThrow))
            return false;
    }

    if (masm.asmOnConversionErrorLabel()->used()) {
        if (!GenerateConversionErrorExit(masm, module, &onThrow))
            return false;
    }

    // Generate unconditionally: the out-of-bounds exit may be used later even
    // if signal handling isn't used for out-of-bounds at the moment.
    if (!GenerateOutOfBoundsExit(masm, module, &onThrow))
        return false;

    // Generate unconditionally: the async interrupt may be taken at any time.
    if (!GenerateAsyncInterruptExit(masm, module, &onThrow))
        return false;

    if (onThrow.used()) {
        if (!GenerateThrowStub(masm, module, &onThrow))
            return false;
    }

    return true;
}

