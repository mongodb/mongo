/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2016 Mozilla Foundation
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

#include "wasm/WasmBCClass.h"
#include "wasm/WasmBCDefs.h"
#include "wasm/WasmBCRegDefs.h"

#include "jit/MacroAssembler-inl.h"

#include "wasm/WasmBCClass-inl.h"
#include "wasm/WasmBCCodegen-inl.h"
#include "wasm/WasmBCRegDefs-inl.h"
#include "wasm/WasmBCRegMgmt-inl.h"
#include "wasm/WasmBCStkMgmt-inl.h"

namespace js {
namespace wasm {

//////////////////////////////////////////////////////////////////////////////
//
// Heap access subroutines.

// Bounds check elimination.
//
// We perform BCE on two kinds of address expressions: on constant heap pointers
// that are known to be in the heap or will be handled by the out-of-bounds trap
// handler; and on local variables that have been checked in dominating code
// without being updated since.
//
// For an access through a constant heap pointer + an offset we can eliminate
// the bounds check if the sum of the address and offset is below the sum of the
// minimum memory length and the offset guard length.
//
// For an access through a local variable + an offset we can eliminate the
// bounds check if the local variable has already been checked and has not been
// updated since, and the offset is less than the guard limit.
//
// To track locals for which we can eliminate checks we use a bit vector
// bceSafe_ that has a bit set for those locals whose bounds have been checked
// and which have not subsequently been set.  Initially this vector is zero.
//
// In straight-line code a bit is set when we perform a bounds check on an
// access via the local and is reset when the variable is updated.
//
// In control flow, the bit vector is manipulated as follows.  Each ControlItem
// has a value bceSafeOnEntry, which is the value of bceSafe_ on entry to the
// item, and a value bceSafeOnExit, which is initially ~0.  On a branch (br,
// brIf, brTable), we always AND the branch target's bceSafeOnExit with the
// value of bceSafe_ at the branch point.  On exiting an item by falling out of
// it, provided we're not in dead code, we AND the current value of bceSafe_
// into the item's bceSafeOnExit.  Additional processing depends on the item
// type:
//
//  - After a block, set bceSafe_ to the block's bceSafeOnExit.
//
//  - On loop entry, after pushing the ControlItem, set bceSafe_ to zero; the
//    back edges would otherwise require us to iterate to a fixedpoint.
//
//  - After a loop, the bceSafe_ is left unchanged, because only fallthrough
//    control flow will reach that point and the bceSafe_ value represents the
//    correct state of the fallthrough path.
//
//  - Set bceSafe_ to the ControlItem's bceSafeOnEntry at both the 'then' branch
//    and the 'else' branch.
//
//  - After an if-then-else, set bceSafe_ to the if-then-else's bceSafeOnExit.
//
//  - After an if-then, set bceSafe_ to the if-then's bceSafeOnExit AND'ed with
//    the if-then's bceSafeOnEntry.
//
// Finally, when the debugger allows locals to be mutated we must disable BCE
// for references via a local, by returning immediately from bceCheckLocal if
// compilerEnv_.debugEnabled() is true.

void BaseCompiler::bceCheckLocal(MemoryAccessDesc* access, AccessCheck* check,
                                 uint32_t local) {
  // We only eliminate bounds checks for memory 0
  if (access->memoryIndex() != 0) {
    return;
  }

  if (local >= sizeof(BCESet) * 8) {
    return;
  }

  uint32_t offsetGuardLimit =
      GetMaxOffsetGuardLimit(moduleEnv_.hugeMemoryEnabled(0));

  if ((bceSafe_ & (BCESet(1) << local)) &&
      access->offset64() < offsetGuardLimit) {
    check->omitBoundsCheck = true;
  }

  // The local becomes safe even if the offset is beyond the guard limit.
  bceSafe_ |= (BCESet(1) << local);
}

void BaseCompiler::bceLocalIsUpdated(uint32_t local) {
  if (local >= sizeof(BCESet) * 8) {
    return;
  }

  bceSafe_ &= ~(BCESet(1) << local);
}

// Alignment check elimination.
//
// Alignment checks for atomic operations can be omitted if the pointer is a
// constant and the pointer + offset is aligned.  Alignment checking that can't
// be omitted can still be simplified by checking only the pointer if the offset
// is aligned.
//
// (In addition, alignment checking of the pointer can be omitted if the pointer
// has been checked in dominating code, but we don't do that yet.)

template <>
RegI32 BaseCompiler::popConstMemoryAccess<RegI32>(MemoryAccessDesc* access,
                                                  AccessCheck* check) {
  int32_t addrTemp;
  MOZ_ALWAYS_TRUE(popConst(&addrTemp));
  uint32_t addr = addrTemp;

  uint32_t offsetGuardLimit = GetMaxOffsetGuardLimit(
      moduleEnv_.hugeMemoryEnabled(access->memoryIndex()));

  uint64_t ea = uint64_t(addr) + uint64_t(access->offset());
  uint64_t limit =
      moduleEnv_.memories[access->memoryIndex()].initialLength32() +
      offsetGuardLimit;

  check->omitBoundsCheck = ea < limit;
  check->omitAlignmentCheck = (ea & (access->byteSize() - 1)) == 0;

  // Fold the offset into the pointer if we can, as this is always
  // beneficial.
  if (ea <= UINT32_MAX) {
    addr = uint32_t(ea);
    access->clearOffset();
  }

  RegI32 r = needI32();
  moveImm32(int32_t(addr), r);
  return r;
}

#ifdef ENABLE_WASM_MEMORY64
template <>
RegI64 BaseCompiler::popConstMemoryAccess<RegI64>(MemoryAccessDesc* access,
                                                  AccessCheck* check) {
  int64_t addrTemp;
  MOZ_ALWAYS_TRUE(popConst(&addrTemp));
  uint64_t addr = addrTemp;

  uint32_t offsetGuardLimit = GetMaxOffsetGuardLimit(
      moduleEnv_.hugeMemoryEnabled(access->memoryIndex()));

  uint64_t ea = addr + access->offset64();
  bool overflow = ea < addr;
  uint64_t limit =
      moduleEnv_.memories[access->memoryIndex()].initialLength64() +
      offsetGuardLimit;

  if (!overflow) {
    check->omitBoundsCheck = ea < limit;
    check->omitAlignmentCheck = (ea & (access->byteSize() - 1)) == 0;

    // Fold the offset into the pointer if we can, as this is always
    // beneficial.
    addr = uint64_t(ea);
    access->clearOffset();
  }

  RegI64 r = needI64();
  moveImm64(int64_t(addr), r);
  return r;
}
#endif

template <typename RegType>
RegType BaseCompiler::popMemoryAccess(MemoryAccessDesc* access,
                                      AccessCheck* check) {
  check->onlyPointerAlignment =
      (access->offset64() & (access->byteSize() - 1)) == 0;

  // If there's a constant it will have the correct type for RegType.
  if (hasConst()) {
    return popConstMemoryAccess<RegType>(access, check);
  }

  // If there's a local it will have the correct type for RegType.
  uint32_t local;
  if (peekLocal(&local)) {
    bceCheckLocal(access, check, local);
  }

  return pop<RegType>();
}

#ifdef JS_64BIT
static inline RegI64 RegPtrToRegIntptr(RegPtr r) {
  return RegI64(Register64(Register(r)));
}

#  ifndef WASM_HAS_HEAPREG
static inline RegPtr RegIntptrToRegPtr(RegI64 r) {
  return RegPtr(Register64(r).reg);
}
#  endif
#else
static inline RegI32 RegPtrToRegIntptr(RegPtr r) { return RegI32(Register(r)); }

#  ifndef WASM_HAS_HEAPREG
static inline RegPtr RegIntptrToRegPtr(RegI32 r) { return RegPtr(Register(r)); }
#  endif
#endif

void BaseCompiler::pushHeapBase(uint32_t memoryIndex) {
  RegPtr heapBase = need<RegPtr>();

#ifdef WASM_HAS_HEAPREG
  if (memoryIndex == 0) {
    move(RegPtr(HeapReg), heapBase);
    push(RegPtrToRegIntptr(heapBase));
    return;
  }
#endif

#ifdef RABALDR_PIN_INSTANCE
  movePtr(RegPtr(InstanceReg), heapBase);
#else
  fr.loadInstancePtr(heapBase);
#endif

  uint32_t offset = instanceOffsetOfMemoryBase(memoryIndex);
  masm.loadPtr(Address(heapBase, offset), heapBase);
  push(RegPtrToRegIntptr(heapBase));
}

void BaseCompiler::branchAddNoOverflow(uint64_t offset, RegI32 ptr, Label* ok) {
  // The invariant holds because ptr is RegI32 - this is m32.
  MOZ_ASSERT(offset <= UINT32_MAX);
  masm.branchAdd32(Assembler::CarryClear, Imm32(uint32_t(offset)), ptr, ok);
}

#ifdef ENABLE_WASM_MEMORY64
void BaseCompiler::branchAddNoOverflow(uint64_t offset, RegI64 ptr, Label* ok) {
#  if defined(JS_64BIT)
  masm.branchAddPtr(Assembler::CarryClear, ImmWord(offset), Register64(ptr).reg,
                    ok);
#  else
  masm.branchAdd64(Assembler::CarryClear, Imm64(offset), ptr, ok);
#  endif
}
#endif

void BaseCompiler::branchTestLowZero(RegI32 ptr, Imm32 mask, Label* ok) {
  masm.branchTest32(Assembler::Zero, ptr, mask, ok);
}

#ifdef ENABLE_WASM_MEMORY64
void BaseCompiler::branchTestLowZero(RegI64 ptr, Imm32 mask, Label* ok) {
#  ifdef JS_64BIT
  masm.branchTestPtr(Assembler::Zero, Register64(ptr).reg, mask, ok);
#  else
  masm.branchTestPtr(Assembler::Zero, ptr.low, mask, ok);
#  endif
}
#endif

void BaseCompiler::boundsCheck4GBOrLargerAccess(uint32_t memoryIndex,
                                                RegPtr instance, RegI32 ptr,
                                                Label* ok) {
#ifdef JS_64BIT
  // Extend the value to 64 bits, check the 64-bit value against the 64-bit
  // bound, then chop back to 32 bits.  On most platform the extending and
  // chopping are no-ops.  It's important that the value we end up with has
  // flowed through the Spectre mask

  // Note, ptr and ptr64 are the same register.
  RegI64 ptr64 = fromI32(ptr);

  // In principle there may be non-zero bits in the upper bits of the
  // register; clear them.
#  ifdef RABALDR_ZERO_EXTENDS
  masm.debugAssertCanonicalInt32(ptr);
#  else
  masm.move32To64ZeroExtend(ptr, ptr64);
#  endif

  boundsCheck4GBOrLargerAccess(memoryIndex, instance, ptr64, ok);

  // Restore the value to the canonical form for a 32-bit value in a
  // 64-bit register and/or the appropriate form for further use in the
  // indexing instruction.
#  ifdef RABALDR_ZERO_EXTENDS
  // The canonical value is zero-extended; we already have that.
#  else
  masm.move64To32(ptr64, ptr);
#  endif
#else
  // No support needed, we have max 2GB heap on 32-bit
  MOZ_CRASH("No 32-bit support");
#endif
}

void BaseCompiler::boundsCheckBelow4GBAccess(uint32_t memoryIndex,
                                             RegPtr instance, RegI32 ptr,
                                             Label* ok) {
  // If the memory's max size is known to be smaller than 64K pages exactly,
  // we can use a 32-bit check and avoid extension and wrapping.
  masm.wasmBoundsCheck32(
      Assembler::Below, ptr,
      Address(instance, instanceOffsetOfBoundsCheckLimit(memoryIndex)), ok);
}

void BaseCompiler::boundsCheck4GBOrLargerAccess(uint32_t memoryIndex,
                                                RegPtr instance, RegI64 ptr,
                                                Label* ok) {
  // Any Spectre mitigation will appear to update the ptr64 register.
  masm.wasmBoundsCheck64(
      Assembler::Below, ptr,
      Address(instance, instanceOffsetOfBoundsCheckLimit(memoryIndex)), ok);
}

void BaseCompiler::boundsCheckBelow4GBAccess(uint32_t memoryIndex,
                                             RegPtr instance, RegI64 ptr,
                                             Label* ok) {
  // The bounds check limit is valid to 64 bits, so there's no sense in doing
  // anything complicated here.  There may be optimization paths here in the
  // future and they may differ on 32-bit and 64-bit.
  boundsCheck4GBOrLargerAccess(memoryIndex, instance, ptr, ok);
}

// Make sure the ptr could be used as an index register.
static inline void ToValidIndex(MacroAssembler& masm, RegI32 ptr) {
#if defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_LOONG64) || \
    defined(JS_CODEGEN_RISCV64)
  // When ptr is used as an index, it will be added to a 64-bit register.
  // So we should explicitly promote ptr to 64-bit. Since now ptr holds a
  // unsigned 32-bit value, we zero-extend it to 64-bit here.
  masm.move32To64ZeroExtend(ptr, Register64(ptr));
#endif
}

#if defined(ENABLE_WASM_MEMORY64)
static inline void ToValidIndex(MacroAssembler& masm, RegI64 ptr) {}
#endif

// RegIndexType is RegI32 for Memory32 and RegI64 for Memory64.
template <typename RegIndexType>
void BaseCompiler::prepareMemoryAccess(MemoryAccessDesc* access,
                                       AccessCheck* check, RegPtr instance,
                                       RegIndexType ptr) {
  uint32_t offsetGuardLimit = GetMaxOffsetGuardLimit(
      moduleEnv_.hugeMemoryEnabled(access->memoryIndex()));

  // Fold offset if necessary for further computations.
  if (access->offset64() >= offsetGuardLimit ||
      access->offset64() > UINT32_MAX ||
      (access->isAtomic() && !check->omitAlignmentCheck &&
       !check->onlyPointerAlignment)) {
    Label ok;
    branchAddNoOverflow(access->offset64(), ptr, &ok);
    masm.wasmTrap(Trap::OutOfBounds, bytecodeOffset());
    masm.bind(&ok);
    access->clearOffset();
    check->onlyPointerAlignment = true;
  }

  // Alignment check if required.

  if (access->isAtomic() && !check->omitAlignmentCheck) {
    MOZ_ASSERT(check->onlyPointerAlignment);
    // We only care about the low pointer bits here.
    Label ok;
    branchTestLowZero(ptr, Imm32(access->byteSize() - 1), &ok);
    masm.wasmTrap(Trap::UnalignedAccess, bytecodeOffset());
    masm.bind(&ok);
  }

  // Ensure no instance if we don't need it.

  if (moduleEnv_.hugeMemoryEnabled(access->memoryIndex()) &&
      access->memoryIndex() == 0) {
    // We have HeapReg and no bounds checking and need load neither
    // memoryBase nor boundsCheckLimit from instance.
    MOZ_ASSERT_IF(check->omitBoundsCheck, instance.isInvalid());
  }
#ifdef WASM_HAS_HEAPREG
  // We have HeapReg and don't need to load the memoryBase from instance.
  MOZ_ASSERT_IF(check->omitBoundsCheck && access->memoryIndex() == 0,
                instance.isInvalid());
#endif

  // Bounds check if required.

  if (!moduleEnv_.hugeMemoryEnabled(access->memoryIndex()) &&
      !check->omitBoundsCheck) {
    Label ok;
#ifdef JS_64BIT
    // The checking depends on how many bits are in the pointer and how many
    // bits are in the bound.
    static_assert(0x100000000 % PageSize == 0);
    if (!moduleEnv_.memories[access->memoryIndex()]
             .boundsCheckLimitIs32Bits() &&
        MaxMemoryPages(
            moduleEnv_.memories[access->memoryIndex()].indexType()) >=
            Pages(0x100000000 / PageSize)) {
      boundsCheck4GBOrLargerAccess(access->memoryIndex(), instance, ptr, &ok);
    } else {
      boundsCheckBelow4GBAccess(access->memoryIndex(), instance, ptr, &ok);
    }
#else
    boundsCheckBelow4GBAccess(access->memoryIndex(), instance, ptr, &ok);
#endif
    masm.wasmTrap(Trap::OutOfBounds, bytecodeOffset());
    masm.bind(&ok);
  }

  ToValidIndex(masm, ptr);
}

template <typename RegIndexType>
void BaseCompiler::computeEffectiveAddress(MemoryAccessDesc* access) {
  if (access->offset()) {
    Label ok;
    RegIndexType ptr = pop<RegIndexType>();
    branchAddNoOverflow(access->offset64(), ptr, &ok);
    masm.wasmTrap(Trap::OutOfBounds, bytecodeOffset());
    masm.bind(&ok);
    access->clearOffset();
    push(ptr);
  }
}

RegPtr BaseCompiler::maybeLoadMemoryBaseForAccess(
    RegPtr instance, const MemoryAccessDesc* access) {
#ifdef JS_CODEGEN_X86
  // x86 adds the memory base to the wasm pointer directly using an addressing
  // mode and doesn't require the memory base to be loaded to a register.
  return RegPtr();
#endif

#ifdef WASM_HAS_HEAPREG
  if (access->memoryIndex() == 0) {
    return RegPtr(HeapReg);
  }
#endif
  RegPtr memoryBase = needPtr();
  uint32_t offset = instanceOffsetOfMemoryBase(access->memoryIndex());
  masm.loadPtr(Address(instance, offset), memoryBase);
  return memoryBase;
}

bool BaseCompiler::needInstanceForAccess(const MemoryAccessDesc* access,
                                         const AccessCheck& check) {
#ifndef WASM_HAS_HEAPREG
  // Platform requires instance for memory base.
  return true;
#else
  if (access->memoryIndex() != 0) {
    // Need instance to load the memory base
    return true;
  }
  return !moduleEnv_.hugeMemoryEnabled(access->memoryIndex()) &&
         !check.omitBoundsCheck;
#endif
}

RegPtr BaseCompiler::maybeLoadInstanceForAccess(const MemoryAccessDesc* access,
                                                const AccessCheck& check) {
  if (needInstanceForAccess(access, check)) {
#ifdef RABALDR_PIN_INSTANCE
    // NOTE, returning InstanceReg here depends for correctness on *ALL*
    // clients not attempting to free this register and not push it on the value
    // stack.
    //
    // We have assertions in place to guard against that, so the risk of the
    // leaky abstraction is acceptable.  performRegisterLeakCheck() will ensure
    // that after every bytecode, the union of available registers from the
    // regalloc and used registers from the stack equals the set of allocatable
    // registers at startup.  Thus if the instance is freed incorrectly it will
    // end up in that union via the regalloc, and if it is pushed incorrectly it
    // will end up in the union via the stack.
    return RegPtr(InstanceReg);
#else
    RegPtr instance = need<RegPtr>();
    fr.loadInstancePtr(instance);
    return instance;
#endif
  }
  return RegPtr::Invalid();
}

RegPtr BaseCompiler::maybeLoadInstanceForAccess(const MemoryAccessDesc* access,
                                                const AccessCheck& check,
                                                RegPtr specific) {
  if (needInstanceForAccess(access, check)) {
#ifdef RABALDR_PIN_INSTANCE
    movePtr(RegPtr(InstanceReg), specific);
#else
    fr.loadInstancePtr(specific);
#endif
    return specific;
  }
  return RegPtr::Invalid();
}

//////////////////////////////////////////////////////////////////////////////
//
// Load and store.

void BaseCompiler::executeLoad(MemoryAccessDesc* access, AccessCheck* check,
                               RegPtr instance, RegPtr memoryBase, RegI32 ptr,
                               AnyReg dest, RegI32 temp) {
  // Emit the load.  At this point, 64-bit offsets will have been resolved.
#if defined(JS_CODEGEN_X64)
  MOZ_ASSERT(temp.isInvalid());
  Operand srcAddr(memoryBase, ptr, TimesOne, access->offset());

  if (dest.tag == AnyReg::I64) {
    masm.wasmLoadI64(*access, srcAddr, dest.i64());
  } else {
    masm.wasmLoad(*access, srcAddr, dest.any());
  }
#elif defined(JS_CODEGEN_X86)
  MOZ_ASSERT(memoryBase.isInvalid() && temp.isInvalid());
  masm.addPtr(
      Address(instance, instanceOffsetOfMemoryBase(access->memoryIndex())),
      ptr);
  Operand srcAddr(ptr, access->offset());

  if (dest.tag == AnyReg::I64) {
    MOZ_ASSERT(dest.i64() == specific_.abiReturnRegI64);
    masm.wasmLoadI64(*access, srcAddr, dest.i64());
  } else {
    // For 8 bit loads, this will generate movsbl or movzbl, so
    // there's no constraint on what the output register may be.
    masm.wasmLoad(*access, srcAddr, dest.any());
  }
#elif defined(JS_CODEGEN_MIPS64)
  if (IsUnaligned(*access)) {
    switch (dest.tag) {
      case AnyReg::I64:
        masm.wasmUnalignedLoadI64(*access, memoryBase, ptr, ptr, dest.i64(),
                                  temp);
        break;
      case AnyReg::F32:
        masm.wasmUnalignedLoadFP(*access, memoryBase, ptr, ptr, dest.f32(),
                                 temp);
        break;
      case AnyReg::F64:
        masm.wasmUnalignedLoadFP(*access, memoryBase, ptr, ptr, dest.f64(),
                                 temp);
        break;
      case AnyReg::I32:
        masm.wasmUnalignedLoad(*access, memoryBase, ptr, ptr, dest.i32(), temp);
        break;
      default:
        MOZ_CRASH("Unexpected type");
    }
  } else {
    if (dest.tag == AnyReg::I64) {
      masm.wasmLoadI64(*access, memoryBase, ptr, ptr, dest.i64());
    } else {
      masm.wasmLoad(*access, memoryBase, ptr, ptr, dest.any());
    }
  }
#elif defined(JS_CODEGEN_ARM)
  MOZ_ASSERT(temp.isInvalid());
  if (dest.tag == AnyReg::I64) {
    masm.wasmLoadI64(*access, memoryBase, ptr, ptr, dest.i64());
  } else {
    masm.wasmLoad(*access, memoryBase, ptr, ptr, dest.any());
  }
#elif defined(JS_CODEGEN_ARM64)
  MOZ_ASSERT(temp.isInvalid());
  if (dest.tag == AnyReg::I64) {
    masm.wasmLoadI64(*access, memoryBase, ptr, dest.i64());
  } else {
    masm.wasmLoad(*access, memoryBase, ptr, dest.any());
  }
#elif defined(JS_CODEGEN_LOONG64)
  MOZ_ASSERT(temp.isInvalid());
  if (dest.tag == AnyReg::I64) {
    masm.wasmLoadI64(*access, memoryBase, ptr, ptr, dest.i64());
  } else {
    masm.wasmLoad(*access, memoryBase, ptr, ptr, dest.any());
  }
#elif defined(JS_CODEGEN_RISCV64)
  MOZ_ASSERT(temp.isInvalid());
  if (dest.tag == AnyReg::I64) {
    masm.wasmLoadI64(*access, memoryBase, ptr, ptr, dest.i64());
  } else {
    masm.wasmLoad(*access, memoryBase, ptr, ptr, dest.any());
  }
#else
  MOZ_CRASH("BaseCompiler platform hook: load");
#endif
}

// ptr and dest may be the same iff dest is I32.
// This may destroy ptr even if ptr and dest are not the same.
void BaseCompiler::load(MemoryAccessDesc* access, AccessCheck* check,
                        RegPtr instance, RegPtr memoryBase, RegI32 ptr,
                        AnyReg dest, RegI32 temp) {
  prepareMemoryAccess(access, check, instance, ptr);
  executeLoad(access, check, instance, memoryBase, ptr, dest, temp);
}

#ifdef ENABLE_WASM_MEMORY64
void BaseCompiler::load(MemoryAccessDesc* access, AccessCheck* check,
                        RegPtr instance, RegPtr memoryBase, RegI64 ptr,
                        AnyReg dest, RegI64 temp) {
  prepareMemoryAccess(access, check, instance, ptr);

#  if !defined(JS_64BIT)
  // On 32-bit systems we have a maximum 2GB heap and bounds checking has
  // been applied to ensure that the 64-bit pointer is valid.
  return executeLoad(access, check, instance, memoryBase, RegI32(ptr.low), dest,
                     maybeFromI64(temp));
#  elif defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM64)
  // On x64 and arm64 the 32-bit code simply assumes that the high bits of the
  // 64-bit pointer register are zero and performs a 64-bit add.  Thus the code
  // generated is the same for the 64-bit and the 32-bit case.
  return executeLoad(access, check, instance, memoryBase, RegI32(ptr.reg), dest,
                     maybeFromI64(temp));
#  elif defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_LOONG64)
  // On mips64 and loongarch64, the 'prepareMemoryAccess' function will make
  // sure that ptr holds a valid 64-bit index value. Thus the code generated in
  // 'executeLoad' is the same for the 64-bit and the 32-bit case.
  return executeLoad(access, check, instance, memoryBase, RegI32(ptr.reg), dest,
                     maybeFromI64(temp));
#  elif defined(JS_CODEGEN_RISCV64)
  // RISCV the 'prepareMemoryAccess' function will make
  // sure that ptr holds a valid 64-bit index value. Thus the code generated in
  // 'executeLoad' is the same for the 64-bit and the 32-bit case.
  return executeLoad(access, check, instance, memoryBase, RegI32(ptr.reg), dest,
                     maybeFromI64(temp));
#  else
  MOZ_CRASH("Missing platform hook");
#  endif
}
#endif

void BaseCompiler::executeStore(MemoryAccessDesc* access, AccessCheck* check,
                                RegPtr instance, RegPtr memoryBase, RegI32 ptr,
                                AnyReg src, RegI32 temp) {
  // Emit the store.  At this point, 64-bit offsets will have been resolved.
#if defined(JS_CODEGEN_X64)
  MOZ_ASSERT(temp.isInvalid());
  Operand dstAddr(memoryBase, ptr, TimesOne, access->offset());

  masm.wasmStore(*access, src.any(), dstAddr);
#elif defined(JS_CODEGEN_X86)
  MOZ_ASSERT(memoryBase.isInvalid() && temp.isInvalid());
  masm.addPtr(
      Address(instance, instanceOffsetOfMemoryBase(access->memoryIndex())),
      ptr);
  Operand dstAddr(ptr, access->offset());

  if (access->type() == Scalar::Int64) {
    masm.wasmStoreI64(*access, src.i64(), dstAddr);
  } else {
    AnyRegister value;
    ScratchI8 scratch(*this);
    if (src.tag == AnyReg::I64) {
      if (access->byteSize() == 1 && !ra.isSingleByteI32(src.i64().low)) {
        masm.mov(src.i64().low, scratch);
        value = AnyRegister(scratch);
      } else {
        value = AnyRegister(src.i64().low);
      }
    } else if (access->byteSize() == 1 && !ra.isSingleByteI32(src.i32())) {
      masm.mov(src.i32(), scratch);
      value = AnyRegister(scratch);
    } else {
      value = src.any();
    }

    masm.wasmStore(*access, value, dstAddr);
  }
#elif defined(JS_CODEGEN_ARM)
  MOZ_ASSERT(temp.isInvalid());
  if (access->type() == Scalar::Int64) {
    masm.wasmStoreI64(*access, src.i64(), memoryBase, ptr, ptr);
  } else if (src.tag == AnyReg::I64) {
    masm.wasmStore(*access, AnyRegister(src.i64().low), memoryBase, ptr, ptr);
  } else {
    masm.wasmStore(*access, src.any(), memoryBase, ptr, ptr);
  }
#elif defined(JS_CODEGEN_MIPS64)
  if (IsUnaligned(*access)) {
    switch (src.tag) {
      case AnyReg::I64:
        masm.wasmUnalignedStoreI64(*access, src.i64(), memoryBase, ptr, ptr,
                                   temp);
        break;
      case AnyReg::F32:
        masm.wasmUnalignedStoreFP(*access, src.f32(), memoryBase, ptr, ptr,
                                  temp);
        break;
      case AnyReg::F64:
        masm.wasmUnalignedStoreFP(*access, src.f64(), memoryBase, ptr, ptr,
                                  temp);
        break;
      case AnyReg::I32:
        masm.wasmUnalignedStore(*access, src.i32(), memoryBase, ptr, ptr, temp);
        break;
      default:
        MOZ_CRASH("Unexpected type");
    }
  } else {
    if (src.tag == AnyReg::I64) {
      masm.wasmStoreI64(*access, src.i64(), memoryBase, ptr, ptr);
    } else {
      masm.wasmStore(*access, src.any(), memoryBase, ptr, ptr);
    }
  }
#elif defined(JS_CODEGEN_ARM64)
  MOZ_ASSERT(temp.isInvalid());
  if (access->type() == Scalar::Int64) {
    masm.wasmStoreI64(*access, src.i64(), memoryBase, ptr);
  } else {
    masm.wasmStore(*access, src.any(), memoryBase, ptr);
  }
#elif defined(JS_CODEGEN_LOONG64)
  MOZ_ASSERT(temp.isInvalid());
  if (access->type() == Scalar::Int64) {
    masm.wasmStoreI64(*access, src.i64(), memoryBase, ptr, ptr);
  } else {
    masm.wasmStore(*access, src.any(), memoryBase, ptr, ptr);
  }
#elif defined(JS_CODEGEN_RISCV64)
  MOZ_ASSERT(temp.isInvalid());
  if (access->type() == Scalar::Int64) {
    masm.wasmStoreI64(*access, src.i64(), memoryBase, ptr, ptr);
  } else {
    masm.wasmStore(*access, src.any(), memoryBase, ptr, ptr);
  }
#else
  MOZ_CRASH("BaseCompiler platform hook: store");
#endif
}

// ptr and src must not be the same register.
// This may destroy ptr and src.
void BaseCompiler::store(MemoryAccessDesc* access, AccessCheck* check,
                         RegPtr instance, RegPtr memoryBase, RegI32 ptr,
                         AnyReg src, RegI32 temp) {
  prepareMemoryAccess(access, check, instance, ptr);
  executeStore(access, check, instance, memoryBase, ptr, src, temp);
}

#ifdef ENABLE_WASM_MEMORY64
void BaseCompiler::store(MemoryAccessDesc* access, AccessCheck* check,
                         RegPtr instance, RegPtr memoryBase, RegI64 ptr,
                         AnyReg src, RegI64 temp) {
  prepareMemoryAccess(access, check, instance, ptr);
  // See comments in load()
#  if !defined(JS_64BIT)
  return executeStore(access, check, instance, memoryBase, RegI32(ptr.low), src,
                      maybeFromI64(temp));
#  elif defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM64) ||    \
      defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_LOONG64) || \
      defined(JS_CODEGEN_RISCV64)
  return executeStore(access, check, instance, memoryBase, RegI32(ptr.reg), src,
                      maybeFromI64(temp));
#  else
  MOZ_CRASH("Missing platform hook");
#  endif
}
#endif

template <typename RegType>
void BaseCompiler::doLoadCommon(MemoryAccessDesc* access, AccessCheck check,
                                ValType type) {
  RegPtr instance;
  RegPtr memoryBase;
  RegType temp;
#if defined(JS_CODEGEN_MIPS64)
  temp = need<RegType>();
#endif

  switch (type.kind()) {
    case ValType::I32: {
      RegType rp = popMemoryAccess<RegType>(access, &check);
      RegI32 rv = needI32();
      instance = maybeLoadInstanceForAccess(access, check);
      memoryBase = maybeLoadMemoryBaseForAccess(instance, access);
      load(access, &check, instance, memoryBase, rp, AnyReg(rv), temp);
      push(rv);
      free(rp);
      break;
    }
    case ValType::I64: {
      RegI64 rv;
      RegType rp;
#ifdef JS_CODEGEN_X86
      rv = specific_.abiReturnRegI64;
      needI64(rv);
      rp = popMemoryAccess<RegType>(access, &check);
#else
      rp = popMemoryAccess<RegType>(access, &check);
      rv = needI64();
#endif
      instance = maybeLoadInstanceForAccess(access, check);
      memoryBase = maybeLoadMemoryBaseForAccess(instance, access);
      load(access, &check, instance, memoryBase, rp, AnyReg(rv), temp);
      push(rv);
      free(rp);
      break;
    }
    case ValType::F32: {
      RegType rp = popMemoryAccess<RegType>(access, &check);
      RegF32 rv = needF32();
      instance = maybeLoadInstanceForAccess(access, check);
      memoryBase = maybeLoadMemoryBaseForAccess(instance, access);
      load(access, &check, instance, memoryBase, rp, AnyReg(rv), temp);
      push(rv);
      free(rp);
      break;
    }
    case ValType::F64: {
      RegType rp = popMemoryAccess<RegType>(access, &check);
      RegF64 rv = needF64();
      instance = maybeLoadInstanceForAccess(access, check);
      memoryBase = maybeLoadMemoryBaseForAccess(instance, access);
      load(access, &check, instance, memoryBase, rp, AnyReg(rv), temp);
      push(rv);
      free(rp);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case ValType::V128: {
      RegType rp = popMemoryAccess<RegType>(access, &check);
      RegV128 rv = needV128();
      instance = maybeLoadInstanceForAccess(access, check);
      memoryBase = maybeLoadMemoryBaseForAccess(instance, access);
      load(access, &check, instance, memoryBase, rp, AnyReg(rv), temp);
      push(rv);
      free(rp);
      break;
    }
#endif
    default:
      MOZ_CRASH("load type");
      break;
  }

#ifndef RABALDR_PIN_INSTANCE
  maybeFree(instance);
#endif
#ifdef WASM_HAS_HEAPREG
  if (memoryBase != HeapReg) {
    maybeFree(memoryBase);
  }
#else
  maybeFree(memoryBase);
#endif
  maybeFree(temp);
}

void BaseCompiler::loadCommon(MemoryAccessDesc* access, AccessCheck check,
                              ValType type) {
  if (isMem32(access->memoryIndex())) {
    doLoadCommon<RegI32>(access, check, type);
  } else {
#ifdef ENABLE_WASM_MEMORY64
    doLoadCommon<RegI64>(access, check, type);
#else
    MOZ_CRASH("Memory64 not enabled / supported on this platform");
#endif
  }
}

template <typename RegType>
void BaseCompiler::doStoreCommon(MemoryAccessDesc* access, AccessCheck check,
                                 ValType resultType) {
  RegPtr instance;
  RegPtr memoryBase;
  RegType temp;
#if defined(JS_CODEGEN_MIPS64)
  temp = need<RegType>();
#endif

  switch (resultType.kind()) {
    case ValType::I32: {
      RegI32 rv = popI32();
      RegType rp = popMemoryAccess<RegType>(access, &check);
      instance = maybeLoadInstanceForAccess(access, check);
      memoryBase = maybeLoadMemoryBaseForAccess(instance, access);
      store(access, &check, instance, memoryBase, rp, AnyReg(rv), temp);
      free(rp);
      free(rv);
      break;
    }
    case ValType::I64: {
      RegI64 rv = popI64();
      RegType rp = popMemoryAccess<RegType>(access, &check);
      instance = maybeLoadInstanceForAccess(access, check);
      memoryBase = maybeLoadMemoryBaseForAccess(instance, access);
      store(access, &check, instance, memoryBase, rp, AnyReg(rv), temp);
      free(rp);
      free(rv);
      break;
    }
    case ValType::F32: {
      RegF32 rv = popF32();
      RegType rp = popMemoryAccess<RegType>(access, &check);
      instance = maybeLoadInstanceForAccess(access, check);
      memoryBase = maybeLoadMemoryBaseForAccess(instance, access);
      store(access, &check, instance, memoryBase, rp, AnyReg(rv), temp);
      free(rp);
      free(rv);
      break;
    }
    case ValType::F64: {
      RegF64 rv = popF64();
      RegType rp = popMemoryAccess<RegType>(access, &check);
      instance = maybeLoadInstanceForAccess(access, check);
      memoryBase = maybeLoadMemoryBaseForAccess(instance, access);
      store(access, &check, instance, memoryBase, rp, AnyReg(rv), temp);
      free(rp);
      free(rv);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case ValType::V128: {
      RegV128 rv = popV128();
      RegType rp = popMemoryAccess<RegType>(access, &check);
      instance = maybeLoadInstanceForAccess(access, check);
      memoryBase = maybeLoadMemoryBaseForAccess(instance, access);
      store(access, &check, instance, memoryBase, rp, AnyReg(rv), temp);
      free(rp);
      free(rv);
      break;
    }
#endif
    default:
      MOZ_CRASH("store type");
      break;
  }

#ifndef RABALDR_PIN_INSTANCE
  maybeFree(instance);
#endif
#ifdef WASM_HAS_HEAPREG
  if (memoryBase != HeapReg) {
    maybeFree(memoryBase);
  }
#else
  maybeFree(memoryBase);
#endif
  maybeFree(temp);
}

void BaseCompiler::storeCommon(MemoryAccessDesc* access, AccessCheck check,
                               ValType type) {
  if (isMem32(access->memoryIndex())) {
    doStoreCommon<RegI32>(access, check, type);
  } else {
#ifdef ENABLE_WASM_MEMORY64
    doStoreCommon<RegI64>(access, check, type);
#else
    MOZ_CRASH("Memory64 not enabled / supported on this platform");
#endif
  }
}

// Convert something that may contain a heap index into a Register that can be
// used in an access.

static inline Register ToRegister(RegI32 r) { return Register(r); }
#ifdef ENABLE_WASM_MEMORY64
#  ifdef JS_PUNBOX64
static inline Register ToRegister(RegI64 r) { return r.reg; }
#  else
static inline Register ToRegister(RegI64 r) { return r.low; }
#  endif
#endif

//////////////////////////////////////////////////////////////////////////////
//
// Atomic operations.
//
// The atomic operations have very diverse per-platform needs for register
// allocation and temps.  To handle that, the implementations are structured as
// a per-operation framework method that calls into platform-specific helpers
// (usually called PopAndAllocate, Perform, and Deallocate) in a per-operation
// namespace.  This structure results in a little duplication and boilerplate
// but is otherwise clean and flexible and keeps code and supporting definitions
// entirely co-located.

// Some consumers depend on the returned Address not incorporating instance, as
// instance may be the scratch register.
//
// RegIndexType is RegI32 for Memory32 and RegI64 for Memory64.
template <typename RegIndexType>
Address BaseCompiler::prepareAtomicMemoryAccess(MemoryAccessDesc* access,
                                                AccessCheck* check,
                                                RegPtr instance,
                                                RegIndexType ptr) {
  MOZ_ASSERT(needInstanceForAccess(access, *check) == instance.isValid());
  prepareMemoryAccess(access, check, instance, ptr);

#ifdef WASM_HAS_HEAPREG
  if (access->memoryIndex() == 0) {
    masm.addPtr(HeapReg, ToRegister(ptr));
  } else {
    masm.addPtr(
        Address(instance, instanceOffsetOfMemoryBase(access->memoryIndex())),
        ToRegister(ptr));
  }
#else
  masm.addPtr(
      Address(instance, instanceOffsetOfMemoryBase(access->memoryIndex())),
      ToRegister(ptr));
#endif

  // At this point, 64-bit offsets will have been resolved.
  return Address(ToRegister(ptr), access->offset());
}

#ifndef WASM_HAS_HEAPREG
#  ifdef JS_CODEGEN_X86
using ScratchAtomicNoHeapReg = ScratchEBX;
#  else
#    error "Unimplemented porting interface"
#  endif
#endif

//////////////////////////////////////////////////////////////////////////////
//
// Atomic load and store.

namespace atomic_load64 {

#ifdef JS_CODEGEN_ARM

static void Allocate(BaseCompiler* bc, RegI64* rd, RegI64*) {
  *rd = bc->needI64Pair();
}

static void Deallocate(BaseCompiler* bc, RegI64) {}

#elif defined JS_CODEGEN_X86

static void Allocate(BaseCompiler* bc, RegI64* rd, RegI64* temp) {
  // The result is in edx:eax, and we need ecx:ebx as a temp.  But ebx will also
  // be used as a scratch, so don't manage that here.
  bc->needI32(bc->specific_.ecx);
  *temp = bc->specific_.ecx_ebx;
  bc->needI64(bc->specific_.edx_eax);
  *rd = bc->specific_.edx_eax;
}

static void Deallocate(BaseCompiler* bc, RegI64 temp) {
  // See comment above.
  MOZ_ASSERT(temp.high == js::jit::ecx);
  bc->freeI32(bc->specific_.ecx);
}

#elif defined(__wasi__) || (defined(JS_CODEGEN_NONE) && !defined(JS_64BIT))

static void Allocate(BaseCompiler*, RegI64*, RegI64*) {}
static void Deallocate(BaseCompiler*, RegI64) {}

#endif

}  // namespace atomic_load64

#if !defined(JS_64BIT)
template <typename RegIndexType>
void BaseCompiler::atomicLoad64(MemoryAccessDesc* access) {
  RegI64 rd, temp;
  atomic_load64::Allocate(this, &rd, &temp);

  AccessCheck check;
  RegIndexType rp = popMemoryAccess<RegIndexType>(access, &check);

#  ifdef WASM_HAS_HEAPREG
  RegPtr instance = maybeLoadInstanceForAccess(access, check);
  auto memaddr = prepareAtomicMemoryAccess(access, &check, instance, rp);
  masm.wasmAtomicLoad64(*access, memaddr, temp, rd);
#    ifndef RABALDR_PIN_INSTANCE
  maybeFree(instance);
#    endif
#  else
  ScratchAtomicNoHeapReg scratch(*this);
  RegPtr instance =
      maybeLoadInstanceForAccess(access, check, RegIntptrToRegPtr(scratch));
  auto memaddr = prepareAtomicMemoryAccess(access, &check, instance, rp);
  masm.wasmAtomicLoad64(*access, memaddr, temp, rd);
  MOZ_ASSERT(instance == scratch);
#  endif

  free(rp);
  atomic_load64::Deallocate(this, temp);
  pushI64(rd);
}
#endif

void BaseCompiler::atomicLoad(MemoryAccessDesc* access, ValType type) {
  Scalar::Type viewType = access->type();
  if (Scalar::byteSize(viewType) <= sizeof(void*)) {
    loadCommon(access, AccessCheck(), type);
    return;
  }

  MOZ_ASSERT(type == ValType::I64 && Scalar::byteSize(viewType) == 8);

#if !defined(JS_64BIT)
  if (isMem32(access->memoryIndex())) {
    atomicLoad64<RegI32>(access);
  } else {
#  ifdef ENABLE_WASM_MEMORY64
    atomicLoad64<RegI64>(access);
#  else
    MOZ_CRASH("Memory64 not enabled / supported on this platform");
#  endif
  }
#else
  MOZ_CRASH("Should not happen");
#endif
}

void BaseCompiler::atomicStore(MemoryAccessDesc* access, ValType type) {
  Scalar::Type viewType = access->type();

  if (Scalar::byteSize(viewType) <= sizeof(void*)) {
    storeCommon(access, AccessCheck(), type);
    return;
  }

  MOZ_ASSERT(type == ValType::I64 && Scalar::byteSize(viewType) == 8);

#if !defined(JS_64BIT)
  if (isMem32(access->memoryIndex())) {
    atomicXchg64<RegI32>(access, WantResult(false));
  } else {
#  ifdef ENABLE_WASM_MEMORY64
    atomicXchg64<RegI64>(access, WantResult(false));
#  else
    MOZ_CRASH("Memory64 not enabled / supported on this platform");
#  endif
  }
#else
  MOZ_CRASH("Should not happen");
#endif
}

//////////////////////////////////////////////////////////////////////////////
//
// Atomic RMW op= operations.

void BaseCompiler::atomicRMW(MemoryAccessDesc* access, ValType type,
                             AtomicOp op) {
  Scalar::Type viewType = access->type();
  if (Scalar::byteSize(viewType) <= 4) {
    if (isMem32(access->memoryIndex())) {
      atomicRMW32<RegI32>(access, type, op);
    } else {
#ifdef ENABLE_WASM_MEMORY64
      atomicRMW32<RegI64>(access, type, op);
#else
      MOZ_CRASH("Memory64 not enabled / supported on this platform");
#endif
    }
  } else {
    MOZ_ASSERT(type == ValType::I64 && Scalar::byteSize(viewType) == 8);
    if (isMem32(access->memoryIndex())) {
      atomicRMW64<RegI32>(access, type, op);
    } else {
#ifdef ENABLE_WASM_MEMORY64
      atomicRMW64<RegI64>(access, type, op);
#else
      MOZ_CRASH("Memory64 not enabled / supported on this platform");
#endif
    }
  }
}

namespace atomic_rmw32 {

#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)

struct Temps {
  // On x86 we use the ScratchI32 for the temp, otherwise we'd run out of
  // registers for 64-bit operations.
#  if defined(JS_CODEGEN_X64)
  RegI32 t0;
#  endif
};

static void PopAndAllocate(BaseCompiler* bc, ValType type,
                           Scalar::Type viewType, AtomicOp op, RegI32* rd,
                           RegI32* rv, Temps* temps) {
  bc->needI32(bc->specific_.eax);
  if (op == AtomicOp::Add || op == AtomicOp::Sub) {
    // We use xadd, so source and destination are the same.  Using
    // eax here is overconstraining, but for byte operations on x86
    // we do need something with a byte register.
    if (type == ValType::I64) {
      *rv = bc->popI64ToSpecificI32(bc->specific_.eax);
    } else {
      *rv = bc->popI32ToSpecific(bc->specific_.eax);
    }
    *rd = *rv;
  } else {
    // We use a cmpxchg loop.  The output must be eax; the input
    // must be in a separate register since it may be used several
    // times.
    if (type == ValType::I64) {
      *rv = bc->popI64ToI32();
    } else {
      *rv = bc->popI32();
    }
    *rd = bc->specific_.eax;
#  ifdef JS_CODEGEN_X64
    temps->t0 = bc->needI32();
#  endif
  }
}

template <typename T>
static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access, T srcAddr,
                    AtomicOp op, RegI32 rv, RegI32 rd, const Temps& temps) {
#  ifdef JS_CODEGEN_X64
  RegI32 temp = temps.t0;
#  else
  RegI32 temp;
  ScratchI32 scratch(*bc);
  if (op != AtomicOp::Add && op != AtomicOp::Sub) {
    temp = scratch;
  }
#  endif
  bc->masm.wasmAtomicFetchOp(access, op, rv, srcAddr, temp, rd);
}

static void Deallocate(BaseCompiler* bc, RegI32 rv, const Temps& temps) {
  if (rv != bc->specific_.eax) {
    bc->freeI32(rv);
  }
#  ifdef JS_CODEGEN_X64
  bc->maybeFree(temps.t0);
#  endif
}

#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)

struct Temps {
  RegI32 t0;
};

static void PopAndAllocate(BaseCompiler* bc, ValType type,
                           Scalar::Type viewType, AtomicOp op, RegI32* rd,
                           RegI32* rv, Temps* temps) {
  *rv = type == ValType::I64 ? bc->popI64ToI32() : bc->popI32();
  temps->t0 = bc->needI32();
  *rd = bc->needI32();
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, AtomicOp op, RegI32 rv, RegI32 rd,
                    const Temps& temps) {
  bc->masm.wasmAtomicFetchOp(access, op, rv, srcAddr, temps.t0, rd);
}

static void Deallocate(BaseCompiler* bc, RegI32 rv, const Temps& temps) {
  bc->freeI32(rv);
  bc->freeI32(temps.t0);
}

#elif defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_LOONG64)

struct Temps {
  RegI32 t0, t1, t2;
};

static void PopAndAllocate(BaseCompiler* bc, ValType type,
                           Scalar::Type viewType, AtomicOp op, RegI32* rd,
                           RegI32* rv, Temps* temps) {
  *rv = type == ValType::I64 ? bc->popI64ToI32() : bc->popI32();
  if (Scalar::byteSize(viewType) < 4) {
    temps->t0 = bc->needI32();
    temps->t1 = bc->needI32();
    temps->t2 = bc->needI32();
  }
  *rd = bc->needI32();
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, AtomicOp op, RegI32 rv, RegI32 rd,
                    const Temps& temps) {
  bc->masm.wasmAtomicFetchOp(access, op, rv, srcAddr, temps.t0, temps.t1,
                             temps.t2, rd);
}

static void Deallocate(BaseCompiler* bc, RegI32 rv, const Temps& temps) {
  bc->freeI32(rv);
  bc->maybeFree(temps.t0);
  bc->maybeFree(temps.t1);
  bc->maybeFree(temps.t2);
}

#elif defined(JS_CODEGEN_RISCV64)

struct Temps {
  RegI32 t0, t1, t2;
};

static void PopAndAllocate(BaseCompiler* bc, ValType type,
                           Scalar::Type viewType, AtomicOp op, RegI32* rd,
                           RegI32* rv, Temps* temps) {
  *rv = type == ValType::I64 ? bc->popI64ToI32() : bc->popI32();
  if (Scalar::byteSize(viewType) < 4) {
    temps->t0 = bc->needI32();
    temps->t1 = bc->needI32();
    temps->t2 = bc->needI32();
  }
  *rd = bc->needI32();
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, AtomicOp op, RegI32 rv, RegI32 rd,
                    const Temps& temps) {
  bc->masm.wasmAtomicFetchOp(access, op, rv, srcAddr, temps.t0, temps.t1,
                             temps.t2, rd);
}

static void Deallocate(BaseCompiler* bc, RegI32 rv, const Temps& temps) {
  bc->freeI32(rv);
  bc->maybeFree(temps.t0);
  bc->maybeFree(temps.t1);
  bc->maybeFree(temps.t2);
}

#elif defined(JS_CODEGEN_NONE) || defined(JS_CODEGEN_WASM32)

using Temps = Nothing;

static void PopAndAllocate(BaseCompiler*, ValType, Scalar::Type, AtomicOp,
                           RegI32*, RegI32*, Temps*) {}

static void Perform(BaseCompiler*, const MemoryAccessDesc&, Address, AtomicOp,
                    RegI32, RegI32, const Temps&) {}

static void Deallocate(BaseCompiler*, RegI32, const Temps&) {}

#endif

}  // namespace atomic_rmw32

template <typename RegIndexType>
void BaseCompiler::atomicRMW32(MemoryAccessDesc* access, ValType type,
                               AtomicOp op) {
  Scalar::Type viewType = access->type();
  RegI32 rd, rv;
  atomic_rmw32::Temps temps;
  atomic_rmw32::PopAndAllocate(this, type, viewType, op, &rd, &rv, &temps);

  AccessCheck check;
  RegIndexType rp = popMemoryAccess<RegIndexType>(access, &check);
  RegPtr instance = maybeLoadInstanceForAccess(access, check);

  auto memaddr = prepareAtomicMemoryAccess(access, &check, instance, rp);
  atomic_rmw32::Perform(this, *access, memaddr, op, rv, rd, temps);

#ifndef RABALDR_PIN_INSTANCE
  maybeFree(instance);
#endif
  atomic_rmw32::Deallocate(this, rv, temps);
  free(rp);

  if (type == ValType::I64) {
    pushU32AsI64(rd);
  } else {
    pushI32(rd);
  }
}

namespace atomic_rmw64 {

#if defined(JS_CODEGEN_X64)

static void PopAndAllocate(BaseCompiler* bc, AtomicOp op, RegI64* rd,
                           RegI64* rv, RegI64* temp) {
  if (op == AtomicOp::Add || op == AtomicOp::Sub) {
    // We use xaddq, so input and output must be the same register.
    *rv = bc->popI64();
    *rd = *rv;
  } else {
    // We use a cmpxchgq loop, so the output must be rax and we need a temp.
    bc->needI64(bc->specific_.rax);
    *rd = bc->specific_.rax;
    *rv = bc->popI64();
    *temp = bc->needI64();
  }
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, AtomicOp op, RegI64 rv, RegI64 temp,
                    RegI64 rd) {
  bc->masm.wasmAtomicFetchOp64(access, op, rv, srcAddr, temp, rd);
}

static void Deallocate(BaseCompiler* bc, AtomicOp op, RegI64 rv, RegI64 temp) {
  bc->maybeFree(temp);
  if (op != AtomicOp::Add && op != AtomicOp::Sub) {
    bc->freeI64(rv);
  }
}

#elif defined(JS_CODEGEN_X86)

// Register allocation is tricky, see comments at atomic_xchg64 below.
//
// - Initially rv=ecx:edx and eax is reserved, rd=unallocated.
// - Then rp is popped into esi+edi because those are the only available.
// - The Setup operation makes rd=edx:eax.
// - Deallocation then frees only the ecx part of rv.
//
// The temp is unused here.

static void PopAndAllocate(BaseCompiler* bc, AtomicOp op, RegI64* rd,
                           RegI64* rv, RegI64*) {
  bc->needI32(bc->specific_.eax);
  bc->needI32(bc->specific_.ecx);
  bc->needI32(bc->specific_.edx);
  *rv = RegI64(Register64(bc->specific_.ecx, bc->specific_.edx));
  bc->popI64ToSpecific(*rv);
}

static void Setup(BaseCompiler* bc, RegI64* rd) { *rd = bc->specific_.edx_eax; }

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, AtomicOp op, RegI64 rv, RegI64, RegI64 rd,
                    const ScratchAtomicNoHeapReg& scratch) {
  MOZ_ASSERT(rv.high == bc->specific_.ecx);
  MOZ_ASSERT(Register(scratch) == js::jit::ebx);

  bc->fr.pushGPR(rv.high);
  bc->fr.pushGPR(rv.low);
  Address value(StackPointer, 0);

  bc->masm.wasmAtomicFetchOp64(access, op, value, srcAddr,
                               bc->specific_.ecx_ebx, rd);

  bc->fr.popBytes(8);
}

static void Deallocate(BaseCompiler* bc, AtomicOp, RegI64, RegI64) {
  bc->freeI32(bc->specific_.ecx);
}

#elif defined(JS_CODEGEN_ARM)

static void PopAndAllocate(BaseCompiler* bc, AtomicOp op, RegI64* rd,
                           RegI64* rv, RegI64* temp) {
  // We use a ldrex/strexd loop so the temp and the output must be
  // odd/even pairs.
  *rv = bc->popI64();
  *temp = bc->needI64Pair();
  *rd = bc->needI64Pair();
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, AtomicOp op, RegI64 rv, RegI64 temp,
                    RegI64 rd) {
  bc->masm.wasmAtomicFetchOp64(access, op, rv, srcAddr, temp, rd);
}

static void Deallocate(BaseCompiler* bc, AtomicOp op, RegI64 rv, RegI64 temp) {
  bc->freeI64(rv);
  bc->freeI64(temp);
}

#elif defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_MIPS64) || \
    defined(JS_CODEGEN_LOONG64)

static void PopAndAllocate(BaseCompiler* bc, AtomicOp op, RegI64* rd,
                           RegI64* rv, RegI64* temp) {
  *rv = bc->popI64();
  *temp = bc->needI64();
  *rd = bc->needI64();
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, AtomicOp op, RegI64 rv, RegI64 temp,
                    RegI64 rd) {
  bc->masm.wasmAtomicFetchOp64(access, op, rv, srcAddr, temp, rd);
}

static void Deallocate(BaseCompiler* bc, AtomicOp op, RegI64 rv, RegI64 temp) {
  bc->freeI64(rv);
  bc->freeI64(temp);
}
#elif defined(JS_CODEGEN_RISCV64)

static void PopAndAllocate(BaseCompiler* bc, AtomicOp op, RegI64* rd,
                           RegI64* rv, RegI64* temp) {
  *rv = bc->popI64();
  *temp = bc->needI64();
  *rd = bc->needI64();
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, AtomicOp op, RegI64 rv, RegI64 temp,
                    RegI64 rd) {
  bc->masm.wasmAtomicFetchOp64(access, op, rv, srcAddr, temp, rd);
}

static void Deallocate(BaseCompiler* bc, AtomicOp op, RegI64 rv, RegI64 temp) {
  bc->freeI64(rv);
  bc->freeI64(temp);
}

#elif defined(JS_CODEGEN_NONE) || defined(JS_CODEGEN_WASM32)

static void PopAndAllocate(BaseCompiler*, AtomicOp, RegI64*, RegI64*, RegI64*) {
}

static void Perform(BaseCompiler*, const MemoryAccessDesc&, Address,
                    AtomicOp op, RegI64, RegI64, RegI64) {}

static void Deallocate(BaseCompiler*, AtomicOp, RegI64, RegI64) {}

#endif

}  // namespace atomic_rmw64

template <typename RegIndexType>
void BaseCompiler::atomicRMW64(MemoryAccessDesc* access, ValType type,
                               AtomicOp op) {
  RegI64 rd, rv, temp;
  atomic_rmw64::PopAndAllocate(this, op, &rd, &rv, &temp);

  AccessCheck check;
  RegIndexType rp = popMemoryAccess<RegIndexType>(access, &check);

#if defined(WASM_HAS_HEAPREG)
  RegPtr instance = maybeLoadInstanceForAccess(access, check);
  auto memaddr = prepareAtomicMemoryAccess(access, &check, instance, rp);
  atomic_rmw64::Perform(this, *access, memaddr, op, rv, temp, rd);
#  ifndef RABALDR_PIN_INSTANCE
  maybeFree(instance);
#  endif
#else
  ScratchAtomicNoHeapReg scratch(*this);
  RegPtr instance =
      maybeLoadInstanceForAccess(access, check, RegIntptrToRegPtr(scratch));
  auto memaddr = prepareAtomicMemoryAccess(access, &check, instance, rp);
  atomic_rmw64::Setup(this, &rd);
  atomic_rmw64::Perform(this, *access, memaddr, op, rv, temp, rd, scratch);
  MOZ_ASSERT(instance == scratch);
#endif

  free(rp);
  atomic_rmw64::Deallocate(this, op, rv, temp);

  pushI64(rd);
}

//////////////////////////////////////////////////////////////////////////////
//
// Atomic exchange (also used for atomic store in some cases).

void BaseCompiler::atomicXchg(MemoryAccessDesc* access, ValType type) {
  Scalar::Type viewType = access->type();
  if (Scalar::byteSize(viewType) <= 4) {
    if (isMem32(access->memoryIndex())) {
      atomicXchg32<RegI32>(access, type);
    } else {
#ifdef ENABLE_WASM_MEMORY64
      atomicXchg32<RegI64>(access, type);
#else
      MOZ_CRASH("Memory64 not enabled / supported on this platform");
#endif
    }
  } else {
    MOZ_ASSERT(type == ValType::I64 && Scalar::byteSize(viewType) == 8);
    if (isMem32(access->memoryIndex())) {
      atomicXchg64<RegI32>(access, WantResult(true));
    } else {
#ifdef ENABLE_WASM_MEMORY64
      atomicXchg64<RegI64>(access, WantResult(true));
#else
      MOZ_CRASH("Memory64 not enabled / supported on this platform");
#endif
    }
  }
}

namespace atomic_xchg32 {

#if defined(JS_CODEGEN_X64)

using Temps = Nothing;

static void PopAndAllocate(BaseCompiler* bc, ValType type,
                           Scalar::Type viewType, RegI32* rd, RegI32* rv,
                           Temps*) {
  // The xchg instruction reuses rv as rd.
  *rv = (type == ValType::I64) ? bc->popI64ToI32() : bc->popI32();
  *rd = *rv;
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, RegI32 rv, RegI32 rd, const Temps&) {
  bc->masm.wasmAtomicExchange(access, srcAddr, rv, rd);
}

static void Deallocate(BaseCompiler* bc, RegI32, const Temps&) {}

#elif defined(JS_CODEGEN_X86)

using Temps = Nothing;

static void PopAndAllocate(BaseCompiler* bc, ValType type,
                           Scalar::Type viewType, RegI32* rd, RegI32* rv,
                           Temps*) {
  // The xchg instruction reuses rv as rd.
  *rv = (type == ValType::I64) ? bc->popI64ToI32() : bc->popI32();
  *rd = *rv;
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, RegI32 rv, RegI32 rd, const Temps&) {
  if (access.type() == Scalar::Uint8 && !bc->ra.isSingleByteI32(rd)) {
    ScratchI8 scratch(*bc);
    // The output register must have a byte persona.
    bc->masm.wasmAtomicExchange(access, srcAddr, rv, scratch);
    bc->masm.movl(scratch, rd);
  } else {
    bc->masm.wasmAtomicExchange(access, srcAddr, rv, rd);
  }
}

static void Deallocate(BaseCompiler* bc, RegI32, const Temps&) {}

#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)

using Temps = Nothing;

static void PopAndAllocate(BaseCompiler* bc, ValType type,
                           Scalar::Type viewType, RegI32* rd, RegI32* rv,
                           Temps*) {
  *rv = (type == ValType::I64) ? bc->popI64ToI32() : bc->popI32();
  *rd = bc->needI32();
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, RegI32 rv, RegI32 rd, const Temps&) {
  bc->masm.wasmAtomicExchange(access, srcAddr, rv, rd);
}

static void Deallocate(BaseCompiler* bc, RegI32 rv, const Temps&) {
  bc->freeI32(rv);
}

#elif defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_LOONG64)

struct Temps {
  RegI32 t0, t1, t2;
};

static void PopAndAllocate(BaseCompiler* bc, ValType type,
                           Scalar::Type viewType, RegI32* rd, RegI32* rv,
                           Temps* temps) {
  *rv = (type == ValType::I64) ? bc->popI64ToI32() : bc->popI32();
  if (Scalar::byteSize(viewType) < 4) {
    temps->t0 = bc->needI32();
    temps->t1 = bc->needI32();
    temps->t2 = bc->needI32();
  }
  *rd = bc->needI32();
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, RegI32 rv, RegI32 rd, const Temps& temps) {
  bc->masm.wasmAtomicExchange(access, srcAddr, rv, temps.t0, temps.t1, temps.t2,
                              rd);
}

static void Deallocate(BaseCompiler* bc, RegI32 rv, const Temps& temps) {
  bc->freeI32(rv);
  bc->maybeFree(temps.t0);
  bc->maybeFree(temps.t1);
  bc->maybeFree(temps.t2);
}

#elif defined(JS_CODEGEN_RISCV64)

struct Temps {
  RegI32 t0, t1, t2;
};

static void PopAndAllocate(BaseCompiler* bc, ValType type,
                           Scalar::Type viewType, RegI32* rd, RegI32* rv,
                           Temps* temps) {
  *rv = (type == ValType::I64) ? bc->popI64ToI32() : bc->popI32();
  if (Scalar::byteSize(viewType) < 4) {
    temps->t0 = bc->needI32();
    temps->t1 = bc->needI32();
    temps->t2 = bc->needI32();
  }
  *rd = bc->needI32();
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, RegI32 rv, RegI32 rd, const Temps& temps) {
  bc->masm.wasmAtomicExchange(access, srcAddr, rv, temps.t0, temps.t1, temps.t2,
                              rd);
}

static void Deallocate(BaseCompiler* bc, RegI32 rv, const Temps& temps) {
  bc->freeI32(rv);
  bc->maybeFree(temps.t0);
  bc->maybeFree(temps.t1);
  bc->maybeFree(temps.t2);
}

#elif defined(JS_CODEGEN_NONE) || defined(JS_CODEGEN_WASM32)

using Temps = Nothing;

static void PopAndAllocate(BaseCompiler*, ValType, Scalar::Type, RegI32*,
                           RegI32*, Temps*) {}
static void Perform(BaseCompiler*, const MemoryAccessDesc&, Address, RegI32,
                    RegI32, const Temps&) {}
static void Deallocate(BaseCompiler*, RegI32, const Temps&) {}

#endif

}  // namespace atomic_xchg32

template <typename RegIndexType>
void BaseCompiler::atomicXchg32(MemoryAccessDesc* access, ValType type) {
  Scalar::Type viewType = access->type();

  RegI32 rd, rv;
  atomic_xchg32::Temps temps;
  atomic_xchg32::PopAndAllocate(this, type, viewType, &rd, &rv, &temps);

  AccessCheck check;

  RegIndexType rp = popMemoryAccess<RegIndexType>(access, &check);
  RegPtr instance = maybeLoadInstanceForAccess(access, check);

  auto memaddr = prepareAtomicMemoryAccess(access, &check, instance, rp);
  atomic_xchg32::Perform(this, *access, memaddr, rv, rd, temps);

#ifndef RABALDR_PIN_INSTANCE
  maybeFree(instance);
#endif
  free(rp);
  atomic_xchg32::Deallocate(this, rv, temps);

  if (type == ValType::I64) {
    pushU32AsI64(rd);
  } else {
    pushI32(rd);
  }
}

namespace atomic_xchg64 {

#if defined(JS_CODEGEN_X64)

static void PopAndAllocate(BaseCompiler* bc, RegI64* rd, RegI64* rv) {
  *rv = bc->popI64();
  *rd = *rv;
}

static void Deallocate(BaseCompiler* bc, RegI64 rd, RegI64) {
  bc->maybeFree(rd);
}

#elif defined(JS_CODEGEN_X86)

// Register allocation is tricky in several ways.
//
// - For a 64-bit access on memory64 we need six registers for rd, rv, and rp,
//   but have only five (as the temp ebx is needed too), so we target all
//   registers explicitly to make sure there's space.
//
// - We'll be using cmpxchg8b, and when we do the operation, rv must be in
//   ecx:ebx, and rd must be edx:eax.  We can't use ebx for rv initially because
//   we need ebx for a scratch also, so use a separate temp and move the value
//   to ebx just before the operation.
//
// In sum:
//
// - Initially rv=ecx:edx and eax is reserved, rd=unallocated.
// - Then rp is popped into esi+edi because those are the only available.
// - The Setup operation makes rv=ecx:ebx and rd=edx:eax and moves edx->ebx.
// - Deallocation then frees only the ecx part of rv.

static void PopAndAllocate(BaseCompiler* bc, RegI64* rd, RegI64* rv) {
  bc->needI32(bc->specific_.ecx);
  bc->needI32(bc->specific_.edx);
  bc->needI32(bc->specific_.eax);
  *rv = RegI64(Register64(bc->specific_.ecx, bc->specific_.edx));
  bc->popI64ToSpecific(*rv);
}

static void Setup(BaseCompiler* bc, RegI64* rv, RegI64* rd,
                  const ScratchAtomicNoHeapReg& scratch) {
  MOZ_ASSERT(rv->high == bc->specific_.ecx);
  MOZ_ASSERT(Register(scratch) == js::jit::ebx);
  bc->masm.move32(rv->low, scratch);
  *rv = bc->specific_.ecx_ebx;
  *rd = bc->specific_.edx_eax;
}

static void Deallocate(BaseCompiler* bc, RegI64 rd, RegI64 rv) {
  MOZ_ASSERT(rd == bc->specific_.edx_eax || rd == RegI64::Invalid());
  bc->maybeFree(rd);
  bc->freeI32(bc->specific_.ecx);
}

#elif defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_MIPS64) || \
    defined(JS_CODEGEN_LOONG64)

static void PopAndAllocate(BaseCompiler* bc, RegI64* rd, RegI64* rv) {
  *rv = bc->popI64();
  *rd = bc->needI64();
}

static void Deallocate(BaseCompiler* bc, RegI64 rd, RegI64 rv) {
  bc->freeI64(rv);
  bc->maybeFree(rd);
}

#elif defined(JS_CODEGEN_ARM)

static void PopAndAllocate(BaseCompiler* bc, RegI64* rd, RegI64* rv) {
  // Both rv and rd must be odd/even pairs.
  *rv = bc->popI64ToSpecific(bc->needI64Pair());
  *rd = bc->needI64Pair();
}

static void Deallocate(BaseCompiler* bc, RegI64 rd, RegI64 rv) {
  bc->freeI64(rv);
  bc->maybeFree(rd);
}

#elif defined(JS_CODEGEN_RISCV64)

static void PopAndAllocate(BaseCompiler* bc, RegI64* rd, RegI64* rv) {
  *rv = bc->popI64();
  *rd = bc->needI64();
}

static void Deallocate(BaseCompiler* bc, RegI64 rd, RegI64 rv) {
  bc->freeI64(rv);
  bc->maybeFree(rd);
}
#elif defined(JS_CODEGEN_NONE) || defined(JS_CODEGEN_WASM32)

static void PopAndAllocate(BaseCompiler*, RegI64*, RegI64*) {}
static void Deallocate(BaseCompiler*, RegI64, RegI64) {}

#endif

}  // namespace atomic_xchg64

template <typename RegIndexType>
void BaseCompiler::atomicXchg64(MemoryAccessDesc* access,
                                WantResult wantResult) {
  RegI64 rd, rv;
  atomic_xchg64::PopAndAllocate(this, &rd, &rv);

  AccessCheck check;
  RegIndexType rp = popMemoryAccess<RegIndexType>(access, &check);

#ifdef WASM_HAS_HEAPREG
  RegPtr instance = maybeLoadInstanceForAccess(access, check);
  auto memaddr =
      prepareAtomicMemoryAccess<RegIndexType>(access, &check, instance, rp);
  masm.wasmAtomicExchange64(*access, memaddr, rv, rd);
#  ifndef RABALDR_PIN_INSTANCE
  maybeFree(instance);
#  endif
#else
  ScratchAtomicNoHeapReg scratch(*this);
  RegPtr instance =
      maybeLoadInstanceForAccess(access, check, RegIntptrToRegPtr(scratch));
  Address memaddr = prepareAtomicMemoryAccess(access, &check, instance, rp);
  atomic_xchg64::Setup(this, &rv, &rd, scratch);
  masm.wasmAtomicExchange64(*access, memaddr, rv, rd);
  MOZ_ASSERT(instance == scratch);
#endif

  free(rp);
  if (wantResult) {
    pushI64(rd);
    rd = RegI64::Invalid();
  }
  atomic_xchg64::Deallocate(this, rd, rv);
}

//////////////////////////////////////////////////////////////////////////////
//
// Atomic compare-exchange.

void BaseCompiler::atomicCmpXchg(MemoryAccessDesc* access, ValType type) {
  Scalar::Type viewType = access->type();
  if (Scalar::byteSize(viewType) <= 4) {
    if (isMem32(access->memoryIndex())) {
      atomicCmpXchg32<RegI32>(access, type);
    } else {
#ifdef ENABLE_WASM_MEMORY64
      atomicCmpXchg32<RegI64>(access, type);
#else
      MOZ_CRASH("Memory64 not enabled / supported on this platform");
#endif
    }
  } else {
    MOZ_ASSERT(type == ValType::I64 && Scalar::byteSize(viewType) == 8);
    if (isMem32(access->memoryIndex())) {
      atomicCmpXchg64<RegI32>(access, type);
    } else {
#ifdef ENABLE_WASM_MEMORY64
      atomicCmpXchg64<RegI64>(access, type);
#else
      MOZ_CRASH("Memory64 not enabled / supported on this platform");
#endif
    }
  }
}

namespace atomic_cmpxchg32 {

#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)

using Temps = Nothing;

static void PopAndAllocate(BaseCompiler* bc, ValType type,
                           Scalar::Type viewType, RegI32* rexpect, RegI32* rnew,
                           RegI32* rd, Temps*) {
  // For cmpxchg, the expected value and the result are both in eax.
  bc->needI32(bc->specific_.eax);
  if (type == ValType::I64) {
    *rnew = bc->popI64ToI32();
    *rexpect = bc->popI64ToSpecificI32(bc->specific_.eax);
  } else {
    *rnew = bc->popI32();
    *rexpect = bc->popI32ToSpecific(bc->specific_.eax);
  }
  *rd = *rexpect;
}

template <typename T>
static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access, T srcAddr,
                    RegI32 rexpect, RegI32 rnew, RegI32 rd, const Temps&) {
#  if defined(JS_CODEGEN_X86)
  ScratchI8 scratch(*bc);
  if (access.type() == Scalar::Uint8) {
    MOZ_ASSERT(rd == bc->specific_.eax);
    if (!bc->ra.isSingleByteI32(rnew)) {
      // The replacement value must have a byte persona.
      bc->masm.movl(rnew, scratch);
      rnew = scratch;
    }
  }
#  endif
  bc->masm.wasmCompareExchange(access, srcAddr, rexpect, rnew, rd);
}

static void Deallocate(BaseCompiler* bc, RegI32, RegI32 rnew, const Temps&) {
  bc->freeI32(rnew);
}

#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)

using Temps = Nothing;

static void PopAndAllocate(BaseCompiler* bc, ValType type,
                           Scalar::Type viewType, RegI32* rexpect, RegI32* rnew,
                           RegI32* rd, Temps*) {
  if (type == ValType::I64) {
    *rnew = bc->popI64ToI32();
    *rexpect = bc->popI64ToI32();
  } else {
    *rnew = bc->popI32();
    *rexpect = bc->popI32();
  }
  *rd = bc->needI32();
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, RegI32 rexpect, RegI32 rnew, RegI32 rd,
                    const Temps&) {
  bc->masm.wasmCompareExchange(access, srcAddr, rexpect, rnew, rd);
}

static void Deallocate(BaseCompiler* bc, RegI32 rexpect, RegI32 rnew,
                       const Temps&) {
  bc->freeI32(rnew);
  bc->freeI32(rexpect);
}

#elif defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_LOONG64)

struct Temps {
  RegI32 t0, t1, t2;
};

static void PopAndAllocate(BaseCompiler* bc, ValType type,
                           Scalar::Type viewType, RegI32* rexpect, RegI32* rnew,
                           RegI32* rd, Temps* temps) {
  if (type == ValType::I64) {
    *rnew = bc->popI64ToI32();
    *rexpect = bc->popI64ToI32();
  } else {
    *rnew = bc->popI32();
    *rexpect = bc->popI32();
  }
  if (Scalar::byteSize(viewType) < 4) {
    temps->t0 = bc->needI32();
    temps->t1 = bc->needI32();
    temps->t2 = bc->needI32();
  }
  *rd = bc->needI32();
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, RegI32 rexpect, RegI32 rnew, RegI32 rd,
                    const Temps& temps) {
  bc->masm.wasmCompareExchange(access, srcAddr, rexpect, rnew, temps.t0,
                               temps.t1, temps.t2, rd);
}

static void Deallocate(BaseCompiler* bc, RegI32 rexpect, RegI32 rnew,
                       const Temps& temps) {
  bc->freeI32(rnew);
  bc->freeI32(rexpect);
  bc->maybeFree(temps.t0);
  bc->maybeFree(temps.t1);
  bc->maybeFree(temps.t2);
}

#elif defined(JS_CODEGEN_RISCV64)

struct Temps {
  RegI32 t0, t1, t2;
};

static void PopAndAllocate(BaseCompiler* bc, ValType type,
                           Scalar::Type viewType, RegI32* rexpect, RegI32* rnew,
                           RegI32* rd, Temps* temps) {
  if (type == ValType::I64) {
    *rnew = bc->popI64ToI32();
    *rexpect = bc->popI64ToI32();
  } else {
    *rnew = bc->popI32();
    *rexpect = bc->popI32();
  }
  if (Scalar::byteSize(viewType) < 4) {
    temps->t0 = bc->needI32();
    temps->t1 = bc->needI32();
    temps->t2 = bc->needI32();
  }
  *rd = bc->needI32();
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, RegI32 rexpect, RegI32 rnew, RegI32 rd,
                    const Temps& temps) {
  bc->masm.wasmCompareExchange(access, srcAddr, rexpect, rnew, temps.t0,
                               temps.t1, temps.t2, rd);
}

static void Deallocate(BaseCompiler* bc, RegI32 rexpect, RegI32 rnew,
                       const Temps& temps) {
  bc->freeI32(rnew);
  bc->freeI32(rexpect);
  bc->maybeFree(temps.t0);
  bc->maybeFree(temps.t1);
  bc->maybeFree(temps.t2);
}

#elif defined(JS_CODEGEN_NONE) || defined(JS_CODEGEN_WASM32)

using Temps = Nothing;

static void PopAndAllocate(BaseCompiler*, ValType, Scalar::Type, RegI32*,
                           RegI32*, RegI32*, Temps*) {}

static void Perform(BaseCompiler*, const MemoryAccessDesc&, Address, RegI32,
                    RegI32, RegI32, const Temps& temps) {}

static void Deallocate(BaseCompiler*, RegI32, RegI32, const Temps&) {}

#endif

}  // namespace atomic_cmpxchg32

template <typename RegIndexType>
void BaseCompiler::atomicCmpXchg32(MemoryAccessDesc* access, ValType type) {
  Scalar::Type viewType = access->type();
  RegI32 rexpect, rnew, rd;
  atomic_cmpxchg32::Temps temps;
  atomic_cmpxchg32::PopAndAllocate(this, type, viewType, &rexpect, &rnew, &rd,
                                   &temps);

  AccessCheck check;
  RegIndexType rp = popMemoryAccess<RegIndexType>(access, &check);
  RegPtr instance = maybeLoadInstanceForAccess(access, check);

  auto memaddr = prepareAtomicMemoryAccess(access, &check, instance, rp);
  atomic_cmpxchg32::Perform(this, *access, memaddr, rexpect, rnew, rd, temps);

#ifndef RABALDR_PIN_INSTANCE
  maybeFree(instance);
#endif
  free(rp);
  atomic_cmpxchg32::Deallocate(this, rexpect, rnew, temps);

  if (type == ValType::I64) {
    pushU32AsI64(rd);
  } else {
    pushI32(rd);
  }
}

namespace atomic_cmpxchg64 {

// The templates are needed for x86 code generation, which needs complicated
// register allocation for memory64.

template <typename RegIndexType>
static void PopAndAllocate(BaseCompiler* bc, RegI64* rexpect, RegI64* rnew,
                           RegI64* rd);

template <typename RegIndexType>
static void Deallocate(BaseCompiler* bc, RegI64 rexpect, RegI64 rnew);

#if defined(JS_CODEGEN_X64)

template <typename RegIndexType>
static void PopAndAllocate(BaseCompiler* bc, RegI64* rexpect, RegI64* rnew,
                           RegI64* rd) {
  // For cmpxchg, the expected value and the result are both in rax.
  bc->needI64(bc->specific_.rax);
  *rnew = bc->popI64();
  *rexpect = bc->popI64ToSpecific(bc->specific_.rax);
  *rd = *rexpect;
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, RegI64 rexpect, RegI64 rnew, RegI64 rd) {
  bc->masm.wasmCompareExchange64(access, srcAddr, rexpect, rnew, rd);
}

template <typename RegIndexType>
static void Deallocate(BaseCompiler* bc, RegI64 rexpect, RegI64 rnew) {
  bc->freeI64(rnew);
}

#elif defined(JS_CODEGEN_X86)

template <typename RegIndexType>
static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, RegI64 rexpect, RegI64 rnew, RegI64 rd,
                    ScratchAtomicNoHeapReg& scratch);

// Memory32: For cmpxchg8b, the expected value and the result are both in
// edx:eax, and the replacement value is in ecx:ebx.  But we can't allocate ebx
// initially because we need it later for a scratch, so instead we allocate a
// temp to hold the low word of 'new'.

template <>
void PopAndAllocate<RegI32>(BaseCompiler* bc, RegI64* rexpect, RegI64* rnew,
                            RegI64* rd) {
  bc->needI64(bc->specific_.edx_eax);
  bc->needI32(bc->specific_.ecx);
  RegI32 tmp = bc->needI32();
  *rnew = bc->popI64ToSpecific(RegI64(Register64(bc->specific_.ecx, tmp)));
  *rexpect = bc->popI64ToSpecific(bc->specific_.edx_eax);
  *rd = *rexpect;
}

template <>
void Perform<RegI32>(BaseCompiler* bc, const MemoryAccessDesc& access,
                     Address srcAddr, RegI64 rexpect, RegI64 rnew, RegI64 rd,
                     ScratchAtomicNoHeapReg& scratch) {
  MOZ_ASSERT(Register(scratch) == js::jit::ebx);
  MOZ_ASSERT(rnew.high == bc->specific_.ecx);
  bc->masm.move32(rnew.low, ebx);
  bc->masm.wasmCompareExchange64(access, srcAddr, rexpect,
                                 bc->specific_.ecx_ebx, rd);
}

template <>
void Deallocate<RegI32>(BaseCompiler* bc, RegI64 rexpect, RegI64 rnew) {
  bc->freeI64(rnew);
}

// Memory64: Register allocation is particularly hairy here.  With memory64, we
// have up to seven live values: i64 expected-value, i64 new-value, i64 pointer,
// and instance.  The instance can use the scratch but there's no avoiding that
// we'll run out of registers.
//
// Unlike for the rmw ops, we can't use edx as the rnew.low since it's used
// for the rexpect.high.  And we can't push anything onto the stack while we're
// popping the memory address because the memory address may be on the stack.

#  ifdef ENABLE_WASM_MEMORY64
template <>
void PopAndAllocate<RegI64>(BaseCompiler* bc, RegI64* rexpect, RegI64* rnew,
                            RegI64* rd) {
  // We reserve these (and ebx).  The 64-bit pointer will end up in esi+edi.
  bc->needI32(bc->specific_.eax);
  bc->needI32(bc->specific_.ecx);
  bc->needI32(bc->specific_.edx);

  // Pop the 'new' value and stash it in the instance scratch area.  Do not
  // initialize *rnew to anything.
  RegI64 tmp(Register64(bc->specific_.ecx, bc->specific_.edx));
  bc->popI64ToSpecific(tmp);
  {
    ScratchPtr instanceScratch(*bc);
    bc->stashI64(instanceScratch, tmp);
  }

  *rexpect = bc->popI64ToSpecific(bc->specific_.edx_eax);
  *rd = *rexpect;
}

template <>
void Perform<RegI64>(BaseCompiler* bc, const MemoryAccessDesc& access,
                     Address srcAddr, RegI64 rexpect, RegI64 rnew, RegI64 rd,
                     ScratchAtomicNoHeapReg& scratch) {
  MOZ_ASSERT(rnew.isInvalid());
  rnew = bc->specific_.ecx_ebx;

  bc->unstashI64(RegPtr(Register(bc->specific_.ecx)), rnew);
  bc->masm.wasmCompareExchange64(access, srcAddr, rexpect, rnew, rd);
}

template <>
void Deallocate<RegI64>(BaseCompiler* bc, RegI64 rexpect, RegI64 rnew) {
  // edx:ebx have been pushed as the result, and the pointer was freed
  // separately in the caller, so just free ecx.
  bc->free(bc->specific_.ecx);
}
#  endif

#elif defined(JS_CODEGEN_ARM)

template <typename RegIndexType>
static void PopAndAllocate(BaseCompiler* bc, RegI64* rexpect, RegI64* rnew,
                           RegI64* rd) {
  // The replacement value and the result must both be odd/even pairs.
  *rnew = bc->popI64Pair();
  *rexpect = bc->popI64();
  *rd = bc->needI64Pair();
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, RegI64 rexpect, RegI64 rnew, RegI64 rd) {
  bc->masm.wasmCompareExchange64(access, srcAddr, rexpect, rnew, rd);
}

template <typename RegIndexType>
static void Deallocate(BaseCompiler* bc, RegI64 rexpect, RegI64 rnew) {
  bc->freeI64(rexpect);
  bc->freeI64(rnew);
}

#elif defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_MIPS64) || \
    defined(JS_CODEGEN_LOONG64)

template <typename RegIndexType>
static void PopAndAllocate(BaseCompiler* bc, RegI64* rexpect, RegI64* rnew,
                           RegI64* rd) {
  *rnew = bc->popI64();
  *rexpect = bc->popI64();
  *rd = bc->needI64();
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, RegI64 rexpect, RegI64 rnew, RegI64 rd) {
  bc->masm.wasmCompareExchange64(access, srcAddr, rexpect, rnew, rd);
}

template <typename RegIndexType>
static void Deallocate(BaseCompiler* bc, RegI64 rexpect, RegI64 rnew) {
  bc->freeI64(rexpect);
  bc->freeI64(rnew);
}

#elif defined(JS_CODEGEN_RISCV64)

template <typename RegIndexType>
static void PopAndAllocate(BaseCompiler* bc, RegI64* rexpect, RegI64* rnew,
                           RegI64* rd) {
  *rnew = bc->popI64();
  *rexpect = bc->popI64();
  *rd = bc->needI64();
}

static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, RegI64 rexpect, RegI64 rnew, RegI64 rd) {
  bc->masm.wasmCompareExchange64(access, srcAddr, rexpect, rnew, rd);
}

template <typename RegIndexType>
static void Deallocate(BaseCompiler* bc, RegI64 rexpect, RegI64 rnew) {
  bc->freeI64(rexpect);
  bc->freeI64(rnew);
}

#elif defined(JS_CODEGEN_NONE) || defined(JS_CODEGEN_WASM32)

template <typename RegIndexType>
static void PopAndAllocate(BaseCompiler* bc, RegI64* rexpect, RegI64* rnew,
                           RegI64* rd) {}
static void Perform(BaseCompiler* bc, const MemoryAccessDesc& access,
                    Address srcAddr, RegI64 rexpect, RegI64 rnew, RegI64 rd) {}
template <typename RegIndexType>
static void Deallocate(BaseCompiler* bc, RegI64 rexpect, RegI64 rnew) {}

#endif

}  // namespace atomic_cmpxchg64

template <typename RegIndexType>
void BaseCompiler::atomicCmpXchg64(MemoryAccessDesc* access, ValType type) {
  RegI64 rexpect, rnew, rd;
  atomic_cmpxchg64::PopAndAllocate<RegIndexType>(this, &rexpect, &rnew, &rd);

  AccessCheck check;
  RegIndexType rp = popMemoryAccess<RegIndexType>(access, &check);

#ifdef WASM_HAS_HEAPREG
  RegPtr instance = maybeLoadInstanceForAccess(access, check);
  auto memaddr = prepareAtomicMemoryAccess(access, &check, instance, rp);
  atomic_cmpxchg64::Perform(this, *access, memaddr, rexpect, rnew, rd);
#  ifndef RABALDR_PIN_INSTANCE
  maybeFree(instance);
#  endif
#else
  ScratchAtomicNoHeapReg scratch(*this);
  RegPtr instance =
      maybeLoadInstanceForAccess(access, check, RegIntptrToRegPtr(scratch));
  Address memaddr = prepareAtomicMemoryAccess(access, &check, instance, rp);
  atomic_cmpxchg64::Perform<RegIndexType>(this, *access, memaddr, rexpect, rnew,
                                          rd, scratch);
  MOZ_ASSERT(instance == scratch);
#endif

  free(rp);
  atomic_cmpxchg64::Deallocate<RegIndexType>(this, rexpect, rnew);

  pushI64(rd);
}

//////////////////////////////////////////////////////////////////////////////
//
// Synchronization.

bool BaseCompiler::atomicWait(ValType type, MemoryAccessDesc* access) {
  switch (type.kind()) {
    case ValType::I32: {
      RegI64 timeout = popI64();
      RegI32 val = popI32();

      if (isMem32(access->memoryIndex())) {
        computeEffectiveAddress<RegI32>(access);
      } else {
#ifdef ENABLE_WASM_MEMORY64
        computeEffectiveAddress<RegI64>(access);
#else
        MOZ_CRASH("Memory64 not enabled / supported on this platform");
#endif
      }

      pushI32(val);
      pushI64(timeout);
      pushI32(access->memoryIndex());

      if (!emitInstanceCall(isMem32(access->memoryIndex()) ? SASigWaitI32M32
                                                           : SASigWaitI32M64)) {
        return false;
      }
      break;
    }
    case ValType::I64: {
      RegI64 timeout = popI64();
      RegI64 val = popI64();

      if (isMem32(access->memoryIndex())) {
        computeEffectiveAddress<RegI32>(access);
      } else {
#ifdef ENABLE_WASM_MEMORY64
#  ifdef JS_CODEGEN_X86
        {
          ScratchPtr scratch(*this);
          stashI64(scratch, val);
          freeI64(val);
        }
#  endif
        computeEffectiveAddress<RegI64>(access);
#  ifdef JS_CODEGEN_X86
        {
          ScratchPtr scratch(*this);
          val = needI64();
          unstashI64(scratch, val);
        }
#  endif
#else
        MOZ_CRASH("Memory64 not enabled / supported on this platform");
#endif
      }

      pushI64(val);
      pushI64(timeout);
      pushI32(access->memoryIndex());

      if (!emitInstanceCall(isMem32(access->memoryIndex()) ? SASigWaitI64M32
                                                           : SASigWaitI64M64)) {
        return false;
      }
      break;
    }
    default:
      MOZ_CRASH();
  }

  return true;
}

bool BaseCompiler::atomicWake(MemoryAccessDesc* access) {
  RegI32 count = popI32();

  if (isMem32(access->memoryIndex())) {
    computeEffectiveAddress<RegI32>(access);
    RegI32 byteOffset = popI32();
    pushI32(byteOffset);
  } else {
#ifdef ENABLE_WASM_MEMORY64
    computeEffectiveAddress<RegI64>(access);
    RegI64 byteOffset = popI64();
    pushI64(byteOffset);
#else
    MOZ_CRASH("Memory64 not enabled / supported on this platform");
#endif
  }

  pushI32(count);
  pushI32(access->memoryIndex());
  return emitInstanceCall(isMem32(access->memoryIndex()) ? SASigWakeM32
                                                         : SASigWakeM64);
}

//////////////////////////////////////////////////////////////////////////////
//
// Bulk memory.

void BaseCompiler::memCopyInlineM32() {
  MOZ_ASSERT(MaxInlineMemoryCopyLength != 0);

  // This function assumes a memory index of zero
  uint32_t memoryIndex = 0;
  int32_t signedLength;
  MOZ_ALWAYS_TRUE(popConst(&signedLength));
  uint32_t length = signedLength;
  MOZ_ASSERT(length != 0 && length <= MaxInlineMemoryCopyLength);

  RegI32 src = popI32();
  RegI32 dest = popI32();

  // Compute the number of copies of each width we will need to do
  size_t remainder = length;
#ifdef ENABLE_WASM_SIMD
  size_t numCopies16 = 0;
  if (MacroAssembler::SupportsFastUnalignedFPAccesses()) {
    numCopies16 = remainder / sizeof(V128);
    remainder %= sizeof(V128);
  }
#endif
#ifdef JS_64BIT
  size_t numCopies8 = remainder / sizeof(uint64_t);
  remainder %= sizeof(uint64_t);
#endif
  size_t numCopies4 = remainder / sizeof(uint32_t);
  remainder %= sizeof(uint32_t);
  size_t numCopies2 = remainder / sizeof(uint16_t);
  remainder %= sizeof(uint16_t);
  size_t numCopies1 = remainder;

  // Load all source bytes onto the value stack from low to high using the
  // widest transfer width we can for the system. We will trap without writing
  // anything if any source byte is out-of-bounds.
  bool omitBoundsCheck = false;
  size_t offset = 0;

#ifdef ENABLE_WASM_SIMD
  for (uint32_t i = 0; i < numCopies16; i++) {
    RegI32 temp = needI32();
    moveI32(src, temp);
    pushI32(temp);

    MemoryAccessDesc access(memoryIndex, Scalar::Simd128, 1, offset,
                            bytecodeOffset(), hugeMemoryEnabled(memoryIndex));
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    loadCommon(&access, check, ValType::V128);

    offset += sizeof(V128);
    omitBoundsCheck = true;
  }
#endif

#ifdef JS_64BIT
  for (uint32_t i = 0; i < numCopies8; i++) {
    RegI32 temp = needI32();
    moveI32(src, temp);
    pushI32(temp);

    MemoryAccessDesc access(memoryIndex, Scalar::Int64, 1, offset,
                            bytecodeOffset(), hugeMemoryEnabled(memoryIndex));
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    loadCommon(&access, check, ValType::I64);

    offset += sizeof(uint64_t);
    omitBoundsCheck = true;
  }
#endif

  for (uint32_t i = 0; i < numCopies4; i++) {
    RegI32 temp = needI32();
    moveI32(src, temp);
    pushI32(temp);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint32, 1, offset,
                            bytecodeOffset(), hugeMemoryEnabled(memoryIndex));
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    loadCommon(&access, check, ValType::I32);

    offset += sizeof(uint32_t);
    omitBoundsCheck = true;
  }

  if (numCopies2) {
    RegI32 temp = needI32();
    moveI32(src, temp);
    pushI32(temp);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint16, 1, offset,
                            bytecodeOffset(), hugeMemoryEnabled(memoryIndex));
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    loadCommon(&access, check, ValType::I32);

    offset += sizeof(uint16_t);
    omitBoundsCheck = true;
  }

  if (numCopies1) {
    RegI32 temp = needI32();
    moveI32(src, temp);
    pushI32(temp);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint8, 1, offset,
                            bytecodeOffset(), hugeMemoryEnabled(memoryIndex));
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    loadCommon(&access, check, ValType::I32);
  }

  // Store all source bytes from the value stack to the destination from
  // high to low. We will trap without writing anything on the first store
  // if any dest byte is out-of-bounds.
  offset = length;
  omitBoundsCheck = false;

  if (numCopies1) {
    offset -= sizeof(uint8_t);

    RegI32 value = popI32();
    RegI32 temp = needI32();
    moveI32(dest, temp);
    pushI32(temp);
    pushI32(value);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint8, 1, offset,
                            bytecodeOffset(), hugeMemoryEnabled(memoryIndex));
    AccessCheck check;
    storeCommon(&access, check, ValType::I32);

    omitBoundsCheck = true;
  }

  if (numCopies2) {
    offset -= sizeof(uint16_t);

    RegI32 value = popI32();
    RegI32 temp = needI32();
    moveI32(dest, temp);
    pushI32(temp);
    pushI32(value);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint16, 1, offset,
                            bytecodeOffset(), hugeMemoryEnabled(memoryIndex));
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    storeCommon(&access, check, ValType::I32);

    omitBoundsCheck = true;
  }

  for (uint32_t i = 0; i < numCopies4; i++) {
    offset -= sizeof(uint32_t);

    RegI32 value = popI32();
    RegI32 temp = needI32();
    moveI32(dest, temp);
    pushI32(temp);
    pushI32(value);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint32, 1, offset,
                            bytecodeOffset(), hugeMemoryEnabled(memoryIndex));
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    storeCommon(&access, check, ValType::I32);

    omitBoundsCheck = true;
  }

#ifdef JS_64BIT
  for (uint32_t i = 0; i < numCopies8; i++) {
    offset -= sizeof(uint64_t);

    RegI64 value = popI64();
    RegI32 temp = needI32();
    moveI32(dest, temp);
    pushI32(temp);
    pushI64(value);

    MemoryAccessDesc access(memoryIndex, Scalar::Int64, 1, offset,
                            bytecodeOffset(), hugeMemoryEnabled(memoryIndex));
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    storeCommon(&access, check, ValType::I64);

    omitBoundsCheck = true;
  }
#endif

#ifdef ENABLE_WASM_SIMD
  for (uint32_t i = 0; i < numCopies16; i++) {
    offset -= sizeof(V128);

    RegV128 value = popV128();
    RegI32 temp = needI32();
    moveI32(dest, temp);
    pushI32(temp);
    pushV128(value);

    MemoryAccessDesc access(memoryIndex, Scalar::Simd128, 1, offset,
                            bytecodeOffset(), hugeMemoryEnabled(memoryIndex));
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    storeCommon(&access, check, ValType::V128);

    omitBoundsCheck = true;
  }
#endif

  freeI32(dest);
  freeI32(src);
}

void BaseCompiler::memFillInlineM32() {
  MOZ_ASSERT(MaxInlineMemoryFillLength != 0);

  // This function assumes a memory index of zero
  uint32_t memoryIndex = 0;
  int32_t signedLength;
  int32_t signedValue;
  MOZ_ALWAYS_TRUE(popConst(&signedLength));
  MOZ_ALWAYS_TRUE(popConst(&signedValue));
  uint32_t length = uint32_t(signedLength);
  uint32_t value = uint32_t(signedValue);
  MOZ_ASSERT(length != 0 && length <= MaxInlineMemoryFillLength);

  RegI32 dest = popI32();

  // Compute the number of copies of each width we will need to do
  size_t remainder = length;
#ifdef ENABLE_WASM_SIMD
  size_t numCopies16 = 0;
  if (MacroAssembler::SupportsFastUnalignedFPAccesses()) {
    numCopies16 = remainder / sizeof(V128);
    remainder %= sizeof(V128);
  }
#endif
#ifdef JS_64BIT
  size_t numCopies8 = remainder / sizeof(uint64_t);
  remainder %= sizeof(uint64_t);
#endif
  size_t numCopies4 = remainder / sizeof(uint32_t);
  remainder %= sizeof(uint32_t);
  size_t numCopies2 = remainder / sizeof(uint16_t);
  remainder %= sizeof(uint16_t);
  size_t numCopies1 = remainder;

  MOZ_ASSERT(numCopies2 <= 1 && numCopies1 <= 1);

  // Generate splatted definitions for wider fills as needed
#ifdef ENABLE_WASM_SIMD
  V128 val16(value);
#endif
#ifdef JS_64BIT
  uint64_t val8 = SplatByteToUInt<uint64_t>(value, 8);
#endif
  uint32_t val4 = SplatByteToUInt<uint32_t>(value, 4);
  uint32_t val2 = SplatByteToUInt<uint32_t>(value, 2);
  uint32_t val1 = value;

  // Store the fill value to the destination from high to low. We will trap
  // without writing anything on the first store if any dest byte is
  // out-of-bounds.
  size_t offset = length;
  bool omitBoundsCheck = false;

  if (numCopies1) {
    offset -= sizeof(uint8_t);

    RegI32 temp = needI32();
    moveI32(dest, temp);
    pushI32(temp);
    pushI32(val1);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint8, 1, offset,
                            bytecodeOffset(), hugeMemoryEnabled(memoryIndex));
    AccessCheck check;
    storeCommon(&access, check, ValType::I32);

    omitBoundsCheck = true;
  }

  if (numCopies2) {
    offset -= sizeof(uint16_t);

    RegI32 temp = needI32();
    moveI32(dest, temp);
    pushI32(temp);
    pushI32(val2);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint16, 1, offset,
                            bytecodeOffset(), hugeMemoryEnabled(memoryIndex));
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    storeCommon(&access, check, ValType::I32);

    omitBoundsCheck = true;
  }

  for (uint32_t i = 0; i < numCopies4; i++) {
    offset -= sizeof(uint32_t);

    RegI32 temp = needI32();
    moveI32(dest, temp);
    pushI32(temp);
    pushI32(val4);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint32, 1, offset,
                            bytecodeOffset(), hugeMemoryEnabled(memoryIndex));
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    storeCommon(&access, check, ValType::I32);

    omitBoundsCheck = true;
  }

#ifdef JS_64BIT
  for (uint32_t i = 0; i < numCopies8; i++) {
    offset -= sizeof(uint64_t);

    RegI32 temp = needI32();
    moveI32(dest, temp);
    pushI32(temp);
    pushI64(val8);

    MemoryAccessDesc access(memoryIndex, Scalar::Int64, 1, offset,
                            bytecodeOffset(), hugeMemoryEnabled(memoryIndex));
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    storeCommon(&access, check, ValType::I64);

    omitBoundsCheck = true;
  }
#endif

#ifdef ENABLE_WASM_SIMD
  for (uint32_t i = 0; i < numCopies16; i++) {
    offset -= sizeof(V128);

    RegI32 temp = needI32();
    moveI32(dest, temp);
    pushI32(temp);
    pushV128(val16);

    MemoryAccessDesc access(memoryIndex, Scalar::Simd128, 1, offset,
                            bytecodeOffset(), hugeMemoryEnabled(memoryIndex));
    AccessCheck check;
    check.omitBoundsCheck = omitBoundsCheck;
    storeCommon(&access, check, ValType::V128);

    omitBoundsCheck = true;
  }
#endif

  freeI32(dest);
}

//////////////////////////////////////////////////////////////////////////////
//
// SIMD and Relaxed SIMD.

#ifdef ENABLE_WASM_SIMD
void BaseCompiler::loadSplat(MemoryAccessDesc* access) {
  // We can implement loadSplat mostly as load + splat because the push of the
  // result onto the value stack in loadCommon normally will not generate any
  // code, it will leave the value in a register which we will consume.

  // We use uint types when we can on the general assumption that unsigned loads
  // might be smaller/faster on some platforms, because no sign extension needs
  // to be done after the sub-register load.
  RegV128 rd = needV128();
  switch (access->type()) {
    case Scalar::Uint8: {
      loadCommon(access, AccessCheck(), ValType::I32);
      RegI32 rs = popI32();
      masm.splatX16(rs, rd);
      free(rs);
      break;
    }
    case Scalar::Uint16: {
      loadCommon(access, AccessCheck(), ValType::I32);
      RegI32 rs = popI32();
      masm.splatX8(rs, rd);
      free(rs);
      break;
    }
    case Scalar::Uint32: {
      loadCommon(access, AccessCheck(), ValType::I32);
      RegI32 rs = popI32();
      masm.splatX4(rs, rd);
      free(rs);
      break;
    }
    case Scalar::Int64: {
      loadCommon(access, AccessCheck(), ValType::I64);
      RegI64 rs = popI64();
      masm.splatX2(rs, rd);
      free(rs);
      break;
    }
    default:
      MOZ_CRASH();
  }
  pushV128(rd);
}

void BaseCompiler::loadZero(MemoryAccessDesc* access) {
  access->setZeroExtendSimd128Load();
  loadCommon(access, AccessCheck(), ValType::V128);
}

void BaseCompiler::loadExtend(MemoryAccessDesc* access, Scalar::Type viewType) {
  loadCommon(access, AccessCheck(), ValType::I64);

  RegI64 rs = popI64();
  RegV128 rd = needV128();
  masm.moveGPR64ToDouble(rs, rd);
  switch (viewType) {
    case Scalar::Int8:
      masm.widenLowInt8x16(rd, rd);
      break;
    case Scalar::Uint8:
      masm.unsignedWidenLowInt8x16(rd, rd);
      break;
    case Scalar::Int16:
      masm.widenLowInt16x8(rd, rd);
      break;
    case Scalar::Uint16:
      masm.unsignedWidenLowInt16x8(rd, rd);
      break;
    case Scalar::Int32:
      masm.widenLowInt32x4(rd, rd);
      break;
    case Scalar::Uint32:
      masm.unsignedWidenLowInt32x4(rd, rd);
      break;
    default:
      MOZ_CRASH();
  }
  freeI64(rs);
  pushV128(rd);
}

void BaseCompiler::loadLane(MemoryAccessDesc* access, uint32_t laneIndex) {
  ValType type = access->type() == Scalar::Int64 ? ValType::I64 : ValType::I32;

  RegV128 rsd = popV128();
  loadCommon(access, AccessCheck(), type);

  if (type == ValType::I32) {
    RegI32 rs = popI32();
    switch (access->type()) {
      case Scalar::Uint8:
        masm.replaceLaneInt8x16(laneIndex, rs, rsd);
        break;
      case Scalar::Uint16:
        masm.replaceLaneInt16x8(laneIndex, rs, rsd);
        break;
      case Scalar::Int32:
        masm.replaceLaneInt32x4(laneIndex, rs, rsd);
        break;
      default:
        MOZ_CRASH("unsupported access type");
    }
    freeI32(rs);
  } else {
    MOZ_ASSERT(type == ValType::I64);
    RegI64 rs = popI64();
    masm.replaceLaneInt64x2(laneIndex, rs, rsd);
    freeI64(rs);
  }

  pushV128(rsd);
}

void BaseCompiler::storeLane(MemoryAccessDesc* access, uint32_t laneIndex) {
  ValType type = access->type() == Scalar::Int64 ? ValType::I64 : ValType::I32;

  RegV128 rs = popV128();
  if (type == ValType::I32) {
    RegI32 tmp = needI32();
    switch (access->type()) {
      case Scalar::Uint8:
        masm.extractLaneInt8x16(laneIndex, rs, tmp);
        break;
      case Scalar::Uint16:
        masm.extractLaneInt16x8(laneIndex, rs, tmp);
        break;
      case Scalar::Int32:
        masm.extractLaneInt32x4(laneIndex, rs, tmp);
        break;
      default:
        MOZ_CRASH("unsupported laneSize");
    }
    pushI32(tmp);
  } else {
    MOZ_ASSERT(type == ValType::I64);
    RegI64 tmp = needI64();
    masm.extractLaneInt64x2(laneIndex, rs, tmp);
    pushI64(tmp);
  }
  freeV128(rs);

  storeCommon(access, AccessCheck(), type);
}
#endif  // ENABLE_WASM_SIMD

}  // namespace wasm
}  // namespace js
