/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Atomics.h"

#ifdef JS_CODEGEN_ARM
#  include "jit/arm/Architecture-arm.h"
#endif
#include "jit/AtomicOperations.h"
#include "jit/IonTypes.h"
#include "jit/MacroAssembler.h"
#include "jit/RegisterSets.h"
#include "js/ScalarType.h"  // js::Scalar::Type
#include "util/Poison.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

// Assigned registers must follow these rules:
//
//  - if they overlap the argument registers (for arguments we use) then they
//
//                     M   M   U   U   SSSS  TTTTT
//          ====\      MM MM   U   U  S        T      /====
//          =====>     M M M   U   U   SSS     T     <=====
//          ====/      M   M   U   U      S    T      \====
//                     M   M    UUU   SSSS     T
//
//    require no register movement, even for 64-bit registers.  (If this becomes
//    too complex to handle then we need to create an abstraction that uses the
//    MoveResolver, see comments on bug 1394420.)
//
//  - they should be volatile when possible so that we don't have to save and
//    restore them.
//
// Note that the functions we're generating have a very limited number of
// signatures, and the register assignments need only work for these signatures.
// The signatures are these:
//
//   ()
//   (ptr)
//   (ptr, val/val64)
//   (ptr, ptr)
//   (ptr, val/val64, val/val64)
//
// It would be nice to avoid saving and restoring all the nonvolatile registers
// for all the operations, and instead save and restore only the registers used
// by each specific operation, but the amount of protocol needed to accomplish
// that probably does not pay for itself.

#if defined(JS_CODEGEN_X64)

// Selected registers match the argument registers exactly, and none of them
// overlap the result register.

static const LiveRegisterSet AtomicNonVolatileRegs;

static constexpr Register AtomicPtrReg = IntArgReg0;
static constexpr Register AtomicPtr2Reg = IntArgReg1;
static constexpr Register AtomicValReg = IntArgReg1;
static constexpr Register64 AtomicValReg64(IntArgReg1);
static constexpr Register AtomicVal2Reg = IntArgReg2;
static constexpr Register64 AtomicVal2Reg64(IntArgReg2);
static constexpr Register AtomicTemp = IntArgReg3;
static constexpr Register64 AtomicTemp64(IntArgReg3);

static constexpr Register64 AtomicReturnReg64 = ReturnReg64;

#elif defined(JS_CODEGEN_ARM64)

// Selected registers match the argument registers, except that the Ptr is not
// in IntArgReg0 so as not to conflict with the result register.

static const LiveRegisterSet AtomicNonVolatileRegs;

static constexpr Register AtomicPtrReg = IntArgReg4;
static constexpr Register AtomicPtr2Reg = IntArgReg1;
static constexpr Register AtomicValReg = IntArgReg1;
static constexpr Register64 AtomicValReg64(IntArgReg1);
static constexpr Register AtomicVal2Reg = IntArgReg2;
static constexpr Register64 AtomicVal2Reg64(IntArgReg2);
static constexpr Register AtomicTemp = IntArgReg3;
static constexpr Register64 AtomicTemp64(IntArgReg3);

static constexpr Register64 AtomicReturnReg64 = ReturnReg64;

#elif defined(JS_CODEGEN_ARM)

// Assigned registers except temp are disjoint from the argument registers,
// since accounting for both 32-bit and 64-bit arguments and constraints on the
// result register is much too messy.  The temp is in an argument register since
// it won't be used until we've moved all arguments to other registers.
//
// Save LR because it's the second scratch register.  The first scratch register
// is r12 (IP).  The atomics implementation in the MacroAssembler uses both.

static const LiveRegisterSet AtomicNonVolatileRegs = LiveRegisterSet(
    GeneralRegisterSet(
        (uint32_t(1) << Registers::r4) | (uint32_t(1) << Registers::r5) |
        (uint32_t(1) << Registers::r6) | (uint32_t(1) << Registers::r7) |
        (uint32_t(1) << Registers::r8) | (uint32_t(1) << Registers::lr)),
    FloatRegisterSet(0));

static constexpr Register AtomicPtrReg = r8;
static constexpr Register AtomicPtr2Reg = r6;
static constexpr Register AtomicTemp = r3;
static constexpr Register AtomicValReg = r6;
static constexpr Register64 AtomicValReg64(r7, r6);
static constexpr Register AtomicVal2Reg = r4;
static constexpr Register64 AtomicVal2Reg64(r5, r4);

static constexpr Register64 AtomicReturnReg64 = ReturnReg64;

#elif defined(JS_CODEGEN_X86)

// There are no argument registers.

static const LiveRegisterSet AtomicNonVolatileRegs = LiveRegisterSet(
    GeneralRegisterSet((1 << X86Encoding::rbx) | (1 << X86Encoding::rsi)),
    FloatRegisterSet(0));

static constexpr Register AtomicPtrReg = esi;
static constexpr Register AtomicPtr2Reg = ebx;
static constexpr Register AtomicValReg = ebx;
static constexpr Register AtomicVal2Reg = ecx;
static constexpr Register AtomicTemp = edx;

// 64-bit registers for cmpxchg8b.  ValReg/Val2Reg/Temp are not used in this
// case.

static constexpr Register64 AtomicValReg64(edx, eax);
static constexpr Register64 AtomicVal2Reg64(ecx, ebx);

// AtomicReturnReg64 is unused on x86.

#else
#  error "Unsupported platform"
#endif

// These are useful shorthands and hide the meaningless uint/int distinction.

static constexpr Scalar::Type SIZE8 = Scalar::Uint8;
static constexpr Scalar::Type SIZE16 = Scalar::Uint16;
static constexpr Scalar::Type SIZE32 = Scalar::Uint32;
static constexpr Scalar::Type SIZE64 = Scalar::Int64;
#ifdef JS_64BIT
static constexpr Scalar::Type SIZEWORD = SIZE64;
#else
static constexpr Scalar::Type SIZEWORD = SIZE32;
#endif

// A "block" is a sequence of bytes that is a reasonable quantum to copy to
// amortize call overhead when implementing memcpy and memmove.  A block will
// not fit in registers on all platforms and copying it without using
// intermediate memory will therefore be sensitive to overlap.
//
// A "word" is an item that we can copy using only register intermediate storage
// on all platforms; words can be individually copied without worrying about
// overlap.
//
// Blocks and words can be aligned or unaligned; specific (generated) copying
// functions handle this in platform-specific ways.

static constexpr size_t WORDSIZE =
    sizeof(uintptr_t);                             // Also see SIZEWORD above
static constexpr size_t BLOCKSIZE = 8 * WORDSIZE;  // Must be a power of 2

static_assert(BLOCKSIZE % WORDSIZE == 0,
              "A block is an integral number of words");

static constexpr size_t WORDMASK = WORDSIZE - 1;
static constexpr size_t BLOCKMASK = BLOCKSIZE - 1;

struct ArgIterator {
  ABIArgGenerator abi;
  unsigned argBase = 0;
};

static void GenGprArg(MacroAssembler& masm, MIRType t, ArgIterator* iter,
                      Register reg) {
  MOZ_ASSERT(t == MIRType::Pointer || t == MIRType::Int32);
  ABIArg arg = iter->abi.next(t);
  switch (arg.kind()) {
    case ABIArg::GPR: {
      if (arg.gpr() != reg) {
        masm.movePtr(arg.gpr(), reg);
      }
      break;
    }
    case ABIArg::Stack: {
      Address src(masm.getStackPointer(),
                  iter->argBase + arg.offsetFromArgBase());
      masm.loadPtr(src, reg);
      break;
    }
    default: {
      MOZ_CRASH("Not possible");
    }
  }
}

static void GenGpr64Arg(MacroAssembler& masm, ArgIterator* iter,
                        Register64 reg) {
  ABIArg arg = iter->abi.next(MIRType::Int64);
  switch (arg.kind()) {
    case ABIArg::GPR: {
      if (arg.gpr64() != reg) {
        masm.move64(arg.gpr64(), reg);
      }
      break;
    }
    case ABIArg::Stack: {
      Address src(masm.getStackPointer(),
                  iter->argBase + arg.offsetFromArgBase());
#ifdef JS_64BIT
      masm.load64(src, reg);
#else
      masm.load32(LowWord(src), reg.low);
      masm.load32(HighWord(src), reg.high);
#endif
      break;
    }
#if defined(JS_CODEGEN_REGISTER_PAIR)
    case ABIArg::GPR_PAIR: {
      if (arg.gpr64() != reg) {
        masm.move32(arg.oddGpr(), reg.high);
        masm.move32(arg.evenGpr(), reg.low);
      }
      break;
    }
#endif
    default: {
      MOZ_CRASH("Not possible");
    }
  }
}

static uint32_t GenPrologue(MacroAssembler& masm, ArgIterator* iter) {
  masm.assumeUnreachable("Shouldn't get here");
  masm.flushBuffer();
  masm.haltingAlign(CodeAlignment);
  masm.setFramePushed(0);
  uint32_t start = masm.currentOffset();
  masm.PushRegsInMask(AtomicNonVolatileRegs);
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
  // The return address is among the nonvolatile registers, if pushed at all.
  iter->argBase = masm.framePushed();
#elif defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  // The return address is pushed separately.
  iter->argBase = sizeof(void*) + masm.framePushed();
#else
#  error "Unsupported platform"
#endif
  return start;
}

static void GenEpilogue(MacroAssembler& masm) {
  masm.PopRegsInMask(AtomicNonVolatileRegs);
  MOZ_ASSERT(masm.framePushed() == 0);
#if defined(JS_CODEGEN_ARM64)
  masm.Ret();
#elif defined(JS_CODEGEN_ARM)
  masm.mov(lr, pc);
#elif defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  masm.ret();
#endif
}

#ifndef JS_64BIT
static uint32_t GenNop(MacroAssembler& masm) {
  ArgIterator iter;
  uint32_t start = GenPrologue(masm, &iter);
  GenEpilogue(masm);
  return start;
}
#endif

static uint32_t GenFenceSeqCst(MacroAssembler& masm) {
  ArgIterator iter;
  uint32_t start = GenPrologue(masm, &iter);
  masm.memoryBarrier(MembarFull);
  GenEpilogue(masm);
  return start;
}

static uint32_t GenLoad(MacroAssembler& masm, Scalar::Type size,
                        Synchronization sync) {
  ArgIterator iter;
  uint32_t start = GenPrologue(masm, &iter);
  GenGprArg(masm, MIRType::Pointer, &iter, AtomicPtrReg);

  masm.memoryBarrier(sync.barrierBefore);
  Address addr(AtomicPtrReg, 0);
  switch (size) {
    case SIZE8:
      masm.load8ZeroExtend(addr, ReturnReg);
      break;
    case SIZE16:
      masm.load16ZeroExtend(addr, ReturnReg);
      break;
    case SIZE32:
      masm.load32(addr, ReturnReg);
      break;
    case SIZE64:
#if defined(JS_64BIT)
      masm.load64(addr, AtomicReturnReg64);
      break;
#else
      MOZ_CRASH("64-bit atomic load not available on this platform");
#endif
    default:
      MOZ_CRASH("Unknown size");
  }
  masm.memoryBarrier(sync.barrierAfter);

  GenEpilogue(masm);
  return start;
}

static uint32_t GenStore(MacroAssembler& masm, Scalar::Type size,
                         Synchronization sync) {
  ArgIterator iter;
  uint32_t start = GenPrologue(masm, &iter);
  GenGprArg(masm, MIRType::Pointer, &iter, AtomicPtrReg);

  masm.memoryBarrier(sync.barrierBefore);
  Address addr(AtomicPtrReg, 0);
  switch (size) {
    case SIZE8:
      GenGprArg(masm, MIRType::Int32, &iter, AtomicValReg);
      masm.store8(AtomicValReg, addr);
      break;
    case SIZE16:
      GenGprArg(masm, MIRType::Int32, &iter, AtomicValReg);
      masm.store16(AtomicValReg, addr);
      break;
    case SIZE32:
      GenGprArg(masm, MIRType::Int32, &iter, AtomicValReg);
      masm.store32(AtomicValReg, addr);
      break;
    case SIZE64:
#if defined(JS_64BIT)
      GenGpr64Arg(masm, &iter, AtomicValReg64);
      masm.store64(AtomicValReg64, addr);
      break;
#else
      MOZ_CRASH("64-bit atomic store not available on this platform");
#endif
    default:
      MOZ_CRASH("Unknown size");
  }
  masm.memoryBarrier(sync.barrierAfter);

  GenEpilogue(masm);
  return start;
}

enum class CopyDir {
  DOWN,  // Move data down, ie, iterate toward higher addresses
  UP     // The other way
};

static uint32_t GenCopy(MacroAssembler& masm, Scalar::Type size,
                        uint32_t unroll, CopyDir direction) {
  ArgIterator iter;
  uint32_t start = GenPrologue(masm, &iter);

  Register dest = AtomicPtrReg;
  Register src = AtomicPtr2Reg;

  GenGprArg(masm, MIRType::Pointer, &iter, dest);
  GenGprArg(masm, MIRType::Pointer, &iter, src);

  uint32_t offset = direction == CopyDir::DOWN ? 0 : unroll - 1;
  for (uint32_t i = 0; i < unroll; i++) {
    switch (size) {
      case SIZE8:
        masm.load8ZeroExtend(Address(src, offset), AtomicTemp);
        masm.store8(AtomicTemp, Address(dest, offset));
        break;
      case SIZE16:
        masm.load16ZeroExtend(Address(src, offset * 2), AtomicTemp);
        masm.store16(AtomicTemp, Address(dest, offset * 2));
        break;
      case SIZE32:
        masm.load32(Address(src, offset * 4), AtomicTemp);
        masm.store32(AtomicTemp, Address(dest, offset * 4));
        break;
      case SIZE64:
#if defined(JS_64BIT)
        masm.load64(Address(src, offset * 8), AtomicTemp64);
        masm.store64(AtomicTemp64, Address(dest, offset * 8));
        break;
#else
        MOZ_CRASH("64-bit atomic load/store not available on this platform");
#endif
      default:
        MOZ_CRASH("Unknown size");
    }
    offset += direction == CopyDir::DOWN ? 1 : -1;
  }

  GenEpilogue(masm);
  return start;
}

static uint32_t GenCmpxchg(MacroAssembler& masm, Scalar::Type size,
                           Synchronization sync) {
  ArgIterator iter;
  uint32_t start = GenPrologue(masm, &iter);
  GenGprArg(masm, MIRType::Pointer, &iter, AtomicPtrReg);

  Address addr(AtomicPtrReg, 0);
  switch (size) {
    case SIZE8:
    case SIZE16:
    case SIZE32:
      GenGprArg(masm, MIRType::Int32, &iter, AtomicValReg);
      GenGprArg(masm, MIRType::Int32, &iter, AtomicVal2Reg);
      masm.compareExchange(size, sync, addr, AtomicValReg, AtomicVal2Reg,
                           ReturnReg);
      break;
    case SIZE64:
      GenGpr64Arg(masm, &iter, AtomicValReg64);
      GenGpr64Arg(masm, &iter, AtomicVal2Reg64);
#if defined(JS_CODEGEN_X86)
      static_assert(AtomicValReg64 == Register64(edx, eax));
      static_assert(AtomicVal2Reg64 == Register64(ecx, ebx));

      // The return register edx:eax is a compiler/ABI assumption that is *not*
      // the same as ReturnReg64, so it's correct not to use that here.
      masm.lock_cmpxchg8b(edx, eax, ecx, ebx, Operand(addr));
#else
      masm.compareExchange64(sync, addr, AtomicValReg64, AtomicVal2Reg64,
                             AtomicReturnReg64);
#endif
      break;
    default:
      MOZ_CRASH("Unknown size");
  }

  GenEpilogue(masm);
  return start;
}

static uint32_t GenExchange(MacroAssembler& masm, Scalar::Type size,
                            Synchronization sync) {
  ArgIterator iter;
  uint32_t start = GenPrologue(masm, &iter);
  GenGprArg(masm, MIRType::Pointer, &iter, AtomicPtrReg);

  Address addr(AtomicPtrReg, 0);
  switch (size) {
    case SIZE8:
    case SIZE16:
    case SIZE32:
      GenGprArg(masm, MIRType::Int32, &iter, AtomicValReg);
      masm.atomicExchange(size, sync, addr, AtomicValReg, ReturnReg);
      break;
    case SIZE64:
#if defined(JS_64BIT)
      GenGpr64Arg(masm, &iter, AtomicValReg64);
      masm.atomicExchange64(sync, addr, AtomicValReg64, AtomicReturnReg64);
      break;
#else
      MOZ_CRASH("64-bit atomic exchange not available on this platform");
#endif
    default:
      MOZ_CRASH("Unknown size");
  }

  GenEpilogue(masm);
  return start;
}

static uint32_t GenFetchOp(MacroAssembler& masm, Scalar::Type size, AtomicOp op,
                           Synchronization sync) {
  ArgIterator iter;
  uint32_t start = GenPrologue(masm, &iter);
  GenGprArg(masm, MIRType::Pointer, &iter, AtomicPtrReg);

  Address addr(AtomicPtrReg, 0);
  switch (size) {
    case SIZE8:
    case SIZE16:
    case SIZE32: {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
      Register tmp = op == AtomicFetchAddOp || op == AtomicFetchSubOp
                         ? Register::Invalid()
                         : AtomicTemp;
#else
      Register tmp = AtomicTemp;
#endif
      GenGprArg(masm, MIRType::Int32, &iter, AtomicValReg);
      masm.atomicFetchOp(size, sync, op, AtomicValReg, addr, tmp, ReturnReg);
      break;
    }
    case SIZE64: {
#if defined(JS_64BIT)
#  if defined(JS_CODEGEN_X64)
      Register64 tmp = op == AtomicFetchAddOp || op == AtomicFetchSubOp
                           ? Register64::Invalid()
                           : AtomicTemp64;
#  else
      Register64 tmp = AtomicTemp64;
#  endif
      GenGpr64Arg(masm, &iter, AtomicValReg64);
      masm.atomicFetchOp64(sync, op, AtomicValReg64, addr, tmp,
                           AtomicReturnReg64);
      break;
#else
      MOZ_CRASH("64-bit atomic fetchOp not available on this platform");
#endif
    }
    default:
      MOZ_CRASH("Unknown size");
  }

  GenEpilogue(masm);
  return start;
}

namespace js {
namespace jit {

void (*AtomicFenceSeqCst)();

#ifndef JS_64BIT
void (*AtomicCompilerFence)();
#endif

uint8_t (*AtomicLoad8SeqCst)(const uint8_t* addr);
uint16_t (*AtomicLoad16SeqCst)(const uint16_t* addr);
uint32_t (*AtomicLoad32SeqCst)(const uint32_t* addr);
#ifdef JS_64BIT
uint64_t (*AtomicLoad64SeqCst)(const uint64_t* addr);
#endif

uint8_t (*AtomicLoad8Unsynchronized)(const uint8_t* addr);
uint16_t (*AtomicLoad16Unsynchronized)(const uint16_t* addr);
uint32_t (*AtomicLoad32Unsynchronized)(const uint32_t* addr);
#ifdef JS_64BIT
uint64_t (*AtomicLoad64Unsynchronized)(const uint64_t* addr);
#endif

uint8_t (*AtomicStore8SeqCst)(uint8_t* addr, uint8_t val);
uint16_t (*AtomicStore16SeqCst)(uint16_t* addr, uint16_t val);
uint32_t (*AtomicStore32SeqCst)(uint32_t* addr, uint32_t val);
#ifdef JS_64BIT
uint64_t (*AtomicStore64SeqCst)(uint64_t* addr, uint64_t val);
#endif

uint8_t (*AtomicStore8Unsynchronized)(uint8_t* addr, uint8_t val);
uint16_t (*AtomicStore16Unsynchronized)(uint16_t* addr, uint16_t val);
uint32_t (*AtomicStore32Unsynchronized)(uint32_t* addr, uint32_t val);
#ifdef JS_64BIT
uint64_t (*AtomicStore64Unsynchronized)(uint64_t* addr, uint64_t val);
#endif

// See the definitions of BLOCKSIZE and WORDSIZE earlier.  The "unaligned"
// functions perform individual byte copies (and must always be "down" or "up").
// The others ignore alignment issues, and thus either depend on unaligned
// accesses being OK or not being invoked on unaligned addresses.
//
// src and dest point to the lower addresses of the respective data areas
// irrespective of "up" or "down".

static void (*AtomicCopyUnalignedBlockDownUnsynchronized)(uint8_t* dest,
                                                          const uint8_t* src);
static void (*AtomicCopyUnalignedBlockUpUnsynchronized)(uint8_t* dest,
                                                        const uint8_t* src);
static void (*AtomicCopyUnalignedWordDownUnsynchronized)(uint8_t* dest,
                                                         const uint8_t* src);
static void (*AtomicCopyUnalignedWordUpUnsynchronized)(uint8_t* dest,
                                                       const uint8_t* src);

static void (*AtomicCopyBlockDownUnsynchronized)(uint8_t* dest,
                                                 const uint8_t* src);
static void (*AtomicCopyBlockUpUnsynchronized)(uint8_t* dest,
                                               const uint8_t* src);
static void (*AtomicCopyWordUnsynchronized)(uint8_t* dest, const uint8_t* src);
static void (*AtomicCopyByteUnsynchronized)(uint8_t* dest, const uint8_t* src);

uint8_t (*AtomicCmpXchg8SeqCst)(uint8_t* addr, uint8_t oldval, uint8_t newval);
uint16_t (*AtomicCmpXchg16SeqCst)(uint16_t* addr, uint16_t oldval,
                                  uint16_t newval);
uint32_t (*AtomicCmpXchg32SeqCst)(uint32_t* addr, uint32_t oldval,
                                  uint32_t newval);
uint64_t (*AtomicCmpXchg64SeqCst)(uint64_t* addr, uint64_t oldval,
                                  uint64_t newval);

uint8_t (*AtomicExchange8SeqCst)(uint8_t* addr, uint8_t val);
uint16_t (*AtomicExchange16SeqCst)(uint16_t* addr, uint16_t val);
uint32_t (*AtomicExchange32SeqCst)(uint32_t* addr, uint32_t val);
#ifdef JS_64BIT
uint64_t (*AtomicExchange64SeqCst)(uint64_t* addr, uint64_t val);
#endif

uint8_t (*AtomicAdd8SeqCst)(uint8_t* addr, uint8_t val);
uint16_t (*AtomicAdd16SeqCst)(uint16_t* addr, uint16_t val);
uint32_t (*AtomicAdd32SeqCst)(uint32_t* addr, uint32_t val);
#ifdef JS_64BIT
uint64_t (*AtomicAdd64SeqCst)(uint64_t* addr, uint64_t val);
#endif

uint8_t (*AtomicAnd8SeqCst)(uint8_t* addr, uint8_t val);
uint16_t (*AtomicAnd16SeqCst)(uint16_t* addr, uint16_t val);
uint32_t (*AtomicAnd32SeqCst)(uint32_t* addr, uint32_t val);
#ifdef JS_64BIT
uint64_t (*AtomicAnd64SeqCst)(uint64_t* addr, uint64_t val);
#endif

uint8_t (*AtomicOr8SeqCst)(uint8_t* addr, uint8_t val);
uint16_t (*AtomicOr16SeqCst)(uint16_t* addr, uint16_t val);
uint32_t (*AtomicOr32SeqCst)(uint32_t* addr, uint32_t val);
#ifdef JS_64BIT
uint64_t (*AtomicOr64SeqCst)(uint64_t* addr, uint64_t val);
#endif

uint8_t (*AtomicXor8SeqCst)(uint8_t* addr, uint8_t val);
uint16_t (*AtomicXor16SeqCst)(uint16_t* addr, uint16_t val);
uint32_t (*AtomicXor32SeqCst)(uint32_t* addr, uint32_t val);
#ifdef JS_64BIT
uint64_t (*AtomicXor64SeqCst)(uint64_t* addr, uint64_t val);
#endif

static bool UnalignedAccessesAreOK() {
#ifdef DEBUG
  const char* flag = getenv("JS_NO_UNALIGNED_MEMCPY");
  if (flag && *flag == '1') return false;
#endif
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  return true;
#elif defined(JS_CODEGEN_ARM)
  return !HasAlignmentFault();
#elif defined(JS_CODEGEN_ARM64)
  // This is not necessarily true but it's the best guess right now.
  return true;
#else
#  error "Unsupported platform"
#endif
}

void AtomicMemcpyDownUnsynchronized(uint8_t* dest, const uint8_t* src,
                                    size_t nbytes) {
  const uint8_t* lim = src + nbytes;

  // Set up bulk copying.  The cases are ordered the way they are on the
  // assumption that if we can achieve aligned copies even with a little
  // preprocessing then that is better than unaligned copying on a platform
  // that supports it.

  if (nbytes >= WORDSIZE) {
    void (*copyBlock)(uint8_t * dest, const uint8_t* src);
    void (*copyWord)(uint8_t * dest, const uint8_t* src);

    if (((uintptr_t(dest) ^ uintptr_t(src)) & WORDMASK) == 0) {
      const uint8_t* cutoff = (const uint8_t*)RoundUp(uintptr_t(src), WORDSIZE);
      MOZ_ASSERT(cutoff <= lim);  // because nbytes >= WORDSIZE
      while (src < cutoff) {
        AtomicCopyByteUnsynchronized(dest++, src++);
      }
      copyBlock = AtomicCopyBlockDownUnsynchronized;
      copyWord = AtomicCopyWordUnsynchronized;
    } else if (UnalignedAccessesAreOK()) {
      copyBlock = AtomicCopyBlockDownUnsynchronized;
      copyWord = AtomicCopyWordUnsynchronized;
    } else {
      copyBlock = AtomicCopyUnalignedBlockDownUnsynchronized;
      copyWord = AtomicCopyUnalignedWordDownUnsynchronized;
    }

    // Bulk copy, first larger blocks and then individual words.

    const uint8_t* blocklim = src + ((lim - src) & ~BLOCKMASK);
    while (src < blocklim) {
      copyBlock(dest, src);
      dest += BLOCKSIZE;
      src += BLOCKSIZE;
    }

    const uint8_t* wordlim = src + ((lim - src) & ~WORDMASK);
    while (src < wordlim) {
      copyWord(dest, src);
      dest += WORDSIZE;
      src += WORDSIZE;
    }
  }

  // Byte copy any remaining tail.

  while (src < lim) {
    AtomicCopyByteUnsynchronized(dest++, src++);
  }
}

void AtomicMemcpyUpUnsynchronized(uint8_t* dest, const uint8_t* src,
                                  size_t nbytes) {
  const uint8_t* lim = src;

  src += nbytes;
  dest += nbytes;

  if (nbytes >= WORDSIZE) {
    void (*copyBlock)(uint8_t * dest, const uint8_t* src);
    void (*copyWord)(uint8_t * dest, const uint8_t* src);

    if (((uintptr_t(dest) ^ uintptr_t(src)) & WORDMASK) == 0) {
      const uint8_t* cutoff = (const uint8_t*)(uintptr_t(src) & ~WORDMASK);
      MOZ_ASSERT(cutoff >= lim);  // Because nbytes >= WORDSIZE
      while (src > cutoff) {
        AtomicCopyByteUnsynchronized(--dest, --src);
      }
      copyBlock = AtomicCopyBlockUpUnsynchronized;
      copyWord = AtomicCopyWordUnsynchronized;
    } else if (UnalignedAccessesAreOK()) {
      copyBlock = AtomicCopyBlockUpUnsynchronized;
      copyWord = AtomicCopyWordUnsynchronized;
    } else {
      copyBlock = AtomicCopyUnalignedBlockUpUnsynchronized;
      copyWord = AtomicCopyUnalignedWordUpUnsynchronized;
    }

    const uint8_t* blocklim = src - ((src - lim) & ~BLOCKMASK);
    while (src > blocklim) {
      dest -= BLOCKSIZE;
      src -= BLOCKSIZE;
      copyBlock(dest, src);
    }

    const uint8_t* wordlim = src - ((src - lim) & ~WORDMASK);
    while (src > wordlim) {
      dest -= WORDSIZE;
      src -= WORDSIZE;
      copyWord(dest, src);
    }
  }

  while (src > lim) {
    AtomicCopyByteUnsynchronized(--dest, --src);
  }
}

// These will be read and written only by the main thread during startup and
// shutdown.

static uint8_t* codeSegment;
static uint32_t codeSegmentSize;

bool InitializeJittedAtomics() {
  // We should only initialize once.
  MOZ_ASSERT(!codeSegment);

  LifoAlloc lifo(4096);
  TempAllocator alloc(&lifo);
  JitContext jcx(&alloc);
  StackMacroAssembler masm;

  uint32_t fenceSeqCst = GenFenceSeqCst(masm);

#ifndef JS_64BIT
  uint32_t nop = GenNop(masm);
#endif

  Synchronization Full = Synchronization::Full();
  Synchronization None = Synchronization::None();

  uint32_t load8SeqCst = GenLoad(masm, SIZE8, Full);
  uint32_t load16SeqCst = GenLoad(masm, SIZE16, Full);
  uint32_t load32SeqCst = GenLoad(masm, SIZE32, Full);
#ifdef JS_64BIT
  uint32_t load64SeqCst = GenLoad(masm, SIZE64, Full);
#endif

  uint32_t load8Unsynchronized = GenLoad(masm, SIZE8, None);
  uint32_t load16Unsynchronized = GenLoad(masm, SIZE16, None);
  uint32_t load32Unsynchronized = GenLoad(masm, SIZE32, None);
#ifdef JS_64BIT
  uint32_t load64Unsynchronized = GenLoad(masm, SIZE64, None);
#endif

  uint32_t store8SeqCst = GenStore(masm, SIZE8, Full);
  uint32_t store16SeqCst = GenStore(masm, SIZE16, Full);
  uint32_t store32SeqCst = GenStore(masm, SIZE32, Full);
#ifdef JS_64BIT
  uint32_t store64SeqCst = GenStore(masm, SIZE64, Full);
#endif

  uint32_t store8Unsynchronized = GenStore(masm, SIZE8, None);
  uint32_t store16Unsynchronized = GenStore(masm, SIZE16, None);
  uint32_t store32Unsynchronized = GenStore(masm, SIZE32, None);
#ifdef JS_64BIT
  uint32_t store64Unsynchronized = GenStore(masm, SIZE64, None);
#endif

  uint32_t copyUnalignedBlockDownUnsynchronized =
      GenCopy(masm, SIZE8, BLOCKSIZE, CopyDir::DOWN);
  uint32_t copyUnalignedBlockUpUnsynchronized =
      GenCopy(masm, SIZE8, BLOCKSIZE, CopyDir::UP);
  uint32_t copyUnalignedWordDownUnsynchronized =
      GenCopy(masm, SIZE8, WORDSIZE, CopyDir::DOWN);
  uint32_t copyUnalignedWordUpUnsynchronized =
      GenCopy(masm, SIZE8, WORDSIZE, CopyDir::UP);

  uint32_t copyBlockDownUnsynchronized =
      GenCopy(masm, SIZEWORD, BLOCKSIZE / WORDSIZE, CopyDir::DOWN);
  uint32_t copyBlockUpUnsynchronized =
      GenCopy(masm, SIZEWORD, BLOCKSIZE / WORDSIZE, CopyDir::UP);
  uint32_t copyWordUnsynchronized = GenCopy(masm, SIZEWORD, 1, CopyDir::DOWN);
  uint32_t copyByteUnsynchronized = GenCopy(masm, SIZE8, 1, CopyDir::DOWN);

  uint32_t cmpxchg8SeqCst = GenCmpxchg(masm, SIZE8, Full);
  uint32_t cmpxchg16SeqCst = GenCmpxchg(masm, SIZE16, Full);
  uint32_t cmpxchg32SeqCst = GenCmpxchg(masm, SIZE32, Full);
  uint32_t cmpxchg64SeqCst = GenCmpxchg(masm, SIZE64, Full);

  uint32_t exchange8SeqCst = GenExchange(masm, SIZE8, Full);
  uint32_t exchange16SeqCst = GenExchange(masm, SIZE16, Full);
  uint32_t exchange32SeqCst = GenExchange(masm, SIZE32, Full);
#ifdef JS_64BIT
  uint32_t exchange64SeqCst = GenExchange(masm, SIZE64, Full);
#endif

  uint32_t add8SeqCst = GenFetchOp(masm, SIZE8, AtomicFetchAddOp, Full);
  uint32_t add16SeqCst = GenFetchOp(masm, SIZE16, AtomicFetchAddOp, Full);
  uint32_t add32SeqCst = GenFetchOp(masm, SIZE32, AtomicFetchAddOp, Full);
#ifdef JS_64BIT
  uint32_t add64SeqCst = GenFetchOp(masm, SIZE64, AtomicFetchAddOp, Full);
#endif

  uint32_t and8SeqCst = GenFetchOp(masm, SIZE8, AtomicFetchAndOp, Full);
  uint32_t and16SeqCst = GenFetchOp(masm, SIZE16, AtomicFetchAndOp, Full);
  uint32_t and32SeqCst = GenFetchOp(masm, SIZE32, AtomicFetchAndOp, Full);
#ifdef JS_64BIT
  uint32_t and64SeqCst = GenFetchOp(masm, SIZE64, AtomicFetchAndOp, Full);
#endif

  uint32_t or8SeqCst = GenFetchOp(masm, SIZE8, AtomicFetchOrOp, Full);
  uint32_t or16SeqCst = GenFetchOp(masm, SIZE16, AtomicFetchOrOp, Full);
  uint32_t or32SeqCst = GenFetchOp(masm, SIZE32, AtomicFetchOrOp, Full);
#ifdef JS_64BIT
  uint32_t or64SeqCst = GenFetchOp(masm, SIZE64, AtomicFetchOrOp, Full);
#endif

  uint32_t xor8SeqCst = GenFetchOp(masm, SIZE8, AtomicFetchXorOp, Full);
  uint32_t xor16SeqCst = GenFetchOp(masm, SIZE16, AtomicFetchXorOp, Full);
  uint32_t xor32SeqCst = GenFetchOp(masm, SIZE32, AtomicFetchXorOp, Full);
#ifdef JS_64BIT
  uint32_t xor64SeqCst = GenFetchOp(masm, SIZE64, AtomicFetchXorOp, Full);
#endif

  masm.finish();
  if (masm.oom()) {
    return false;
  }

  // Allocate executable memory.
  uint32_t codeLength = masm.bytesNeeded();
  size_t roundedCodeLength = RoundUp(codeLength, ExecutableCodePageSize);
  uint8_t* code = (uint8_t*)AllocateExecutableMemory(
      roundedCodeLength, ProtectionSetting::Writable,
      MemCheckKind::MakeUndefined);
  if (!code) {
    return false;
  }

  // Zero the padding.
  memset(code + codeLength, 0, roundedCodeLength - codeLength);

  // Copy the code into place.
  masm.executableCopy(code);

  // Reprotect the whole region to avoid having separate RW and RX mappings.
  if (!ExecutableAllocator::makeExecutableAndFlushICache(
          FlushICacheSpec::LocalThreadOnly, code, roundedCodeLength)) {
    DeallocateExecutableMemory(code, roundedCodeLength);
    return false;
  }

  // Create the function pointers.

  AtomicFenceSeqCst = (void (*)())(code + fenceSeqCst);

#ifndef JS_64BIT
  AtomicCompilerFence = (void (*)())(code + nop);
#endif

  AtomicLoad8SeqCst = (uint8_t(*)(const uint8_t* addr))(code + load8SeqCst);
  AtomicLoad16SeqCst = (uint16_t(*)(const uint16_t* addr))(code + load16SeqCst);
  AtomicLoad32SeqCst = (uint32_t(*)(const uint32_t* addr))(code + load32SeqCst);
#ifdef JS_64BIT
  AtomicLoad64SeqCst = (uint64_t(*)(const uint64_t* addr))(code + load64SeqCst);
#endif

  AtomicLoad8Unsynchronized =
      (uint8_t(*)(const uint8_t* addr))(code + load8Unsynchronized);
  AtomicLoad16Unsynchronized =
      (uint16_t(*)(const uint16_t* addr))(code + load16Unsynchronized);
  AtomicLoad32Unsynchronized =
      (uint32_t(*)(const uint32_t* addr))(code + load32Unsynchronized);
#ifdef JS_64BIT
  AtomicLoad64Unsynchronized =
      (uint64_t(*)(const uint64_t* addr))(code + load64Unsynchronized);
#endif

  AtomicStore8SeqCst =
      (uint8_t(*)(uint8_t * addr, uint8_t val))(code + store8SeqCst);
  AtomicStore16SeqCst =
      (uint16_t(*)(uint16_t * addr, uint16_t val))(code + store16SeqCst);
  AtomicStore32SeqCst =
      (uint32_t(*)(uint32_t * addr, uint32_t val))(code + store32SeqCst);
#ifdef JS_64BIT
  AtomicStore64SeqCst =
      (uint64_t(*)(uint64_t * addr, uint64_t val))(code + store64SeqCst);
#endif

  AtomicStore8Unsynchronized =
      (uint8_t(*)(uint8_t * addr, uint8_t val))(code + store8Unsynchronized);
  AtomicStore16Unsynchronized = (uint16_t(*)(uint16_t * addr, uint16_t val))(
      code + store16Unsynchronized);
  AtomicStore32Unsynchronized = (uint32_t(*)(uint32_t * addr, uint32_t val))(
      code + store32Unsynchronized);
#ifdef JS_64BIT
  AtomicStore64Unsynchronized = (uint64_t(*)(uint64_t * addr, uint64_t val))(
      code + store64Unsynchronized);
#endif

  AtomicCopyUnalignedBlockDownUnsynchronized =
      (void (*)(uint8_t * dest, const uint8_t* src))(
          code + copyUnalignedBlockDownUnsynchronized);
  AtomicCopyUnalignedBlockUpUnsynchronized =
      (void (*)(uint8_t * dest, const uint8_t* src))(
          code + copyUnalignedBlockUpUnsynchronized);
  AtomicCopyUnalignedWordDownUnsynchronized =
      (void (*)(uint8_t * dest, const uint8_t* src))(
          code + copyUnalignedWordDownUnsynchronized);
  AtomicCopyUnalignedWordUpUnsynchronized =
      (void (*)(uint8_t * dest, const uint8_t* src))(
          code + copyUnalignedWordUpUnsynchronized);

  AtomicCopyBlockDownUnsynchronized = (void (*)(
      uint8_t * dest, const uint8_t* src))(code + copyBlockDownUnsynchronized);
  AtomicCopyBlockUpUnsynchronized = (void (*)(
      uint8_t * dest, const uint8_t* src))(code + copyBlockUpUnsynchronized);
  AtomicCopyWordUnsynchronized = (void (*)(uint8_t * dest, const uint8_t* src))(
      code + copyWordUnsynchronized);
  AtomicCopyByteUnsynchronized = (void (*)(uint8_t * dest, const uint8_t* src))(
      code + copyByteUnsynchronized);

  AtomicCmpXchg8SeqCst = (uint8_t(*)(uint8_t * addr, uint8_t oldval,
                                     uint8_t newval))(code + cmpxchg8SeqCst);
  AtomicCmpXchg16SeqCst =
      (uint16_t(*)(uint16_t * addr, uint16_t oldval, uint16_t newval))(
          code + cmpxchg16SeqCst);
  AtomicCmpXchg32SeqCst =
      (uint32_t(*)(uint32_t * addr, uint32_t oldval, uint32_t newval))(
          code + cmpxchg32SeqCst);
  AtomicCmpXchg64SeqCst =
      (uint64_t(*)(uint64_t * addr, uint64_t oldval, uint64_t newval))(
          code + cmpxchg64SeqCst);

  AtomicExchange8SeqCst =
      (uint8_t(*)(uint8_t * addr, uint8_t val))(code + exchange8SeqCst);
  AtomicExchange16SeqCst =
      (uint16_t(*)(uint16_t * addr, uint16_t val))(code + exchange16SeqCst);
  AtomicExchange32SeqCst =
      (uint32_t(*)(uint32_t * addr, uint32_t val))(code + exchange32SeqCst);
#ifdef JS_64BIT
  AtomicExchange64SeqCst =
      (uint64_t(*)(uint64_t * addr, uint64_t val))(code + exchange64SeqCst);
#endif

  AtomicAdd8SeqCst =
      (uint8_t(*)(uint8_t * addr, uint8_t val))(code + add8SeqCst);
  AtomicAdd16SeqCst =
      (uint16_t(*)(uint16_t * addr, uint16_t val))(code + add16SeqCst);
  AtomicAdd32SeqCst =
      (uint32_t(*)(uint32_t * addr, uint32_t val))(code + add32SeqCst);
#ifdef JS_64BIT
  AtomicAdd64SeqCst =
      (uint64_t(*)(uint64_t * addr, uint64_t val))(code + add64SeqCst);
#endif

  AtomicAnd8SeqCst =
      (uint8_t(*)(uint8_t * addr, uint8_t val))(code + and8SeqCst);
  AtomicAnd16SeqCst =
      (uint16_t(*)(uint16_t * addr, uint16_t val))(code + and16SeqCst);
  AtomicAnd32SeqCst =
      (uint32_t(*)(uint32_t * addr, uint32_t val))(code + and32SeqCst);
#ifdef JS_64BIT
  AtomicAnd64SeqCst =
      (uint64_t(*)(uint64_t * addr, uint64_t val))(code + and64SeqCst);
#endif

  AtomicOr8SeqCst = (uint8_t(*)(uint8_t * addr, uint8_t val))(code + or8SeqCst);
  AtomicOr16SeqCst =
      (uint16_t(*)(uint16_t * addr, uint16_t val))(code + or16SeqCst);
  AtomicOr32SeqCst =
      (uint32_t(*)(uint32_t * addr, uint32_t val))(code + or32SeqCst);
#ifdef JS_64BIT
  AtomicOr64SeqCst =
      (uint64_t(*)(uint64_t * addr, uint64_t val))(code + or64SeqCst);
#endif

  AtomicXor8SeqCst =
      (uint8_t(*)(uint8_t * addr, uint8_t val))(code + xor8SeqCst);
  AtomicXor16SeqCst =
      (uint16_t(*)(uint16_t * addr, uint16_t val))(code + xor16SeqCst);
  AtomicXor32SeqCst =
      (uint32_t(*)(uint32_t * addr, uint32_t val))(code + xor32SeqCst);
#ifdef JS_64BIT
  AtomicXor64SeqCst =
      (uint64_t(*)(uint64_t * addr, uint64_t val))(code + xor64SeqCst);
#endif

  codeSegment = code;
  codeSegmentSize = roundedCodeLength;

  return true;
}

void ShutDownJittedAtomics() {
  // Must have been initialized.
  MOZ_ASSERT(codeSegment);

  DeallocateExecutableMemory(codeSegment, codeSegmentSize);
  codeSegment = nullptr;
  codeSegmentSize = 0;
}

}  // namespace jit
}  // namespace js
