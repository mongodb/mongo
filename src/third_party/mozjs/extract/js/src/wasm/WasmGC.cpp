/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2019 Mozilla Foundation
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

#include "wasm/WasmGC.h"
#include "wasm/WasmInstance.h"
#include "jit/MacroAssembler-inl.h"

using mozilla::DebugOnly;

using namespace js;
using namespace js::jit;
using namespace js::wasm;

// Generate a stackmap for a function's stack-overflow-at-entry trap, with
// the structure:
//
//    <reg dump area>
//    |       ++ <space reserved before trap, if any>
//    |               ++ <space for Frame>
//    |                       ++ <inbound arg area>
//    |                                           |
//    Lowest Addr                                 Highest Addr
//
// The caller owns the resulting stackmap.  This assumes a grow-down stack.
//
// For non-debug builds, if the stackmap would contain no pointers, no
// stackmap is created, and nullptr is returned.  For a debug build, a
// stackmap is always created and returned.
//
// The "space reserved before trap" is the space reserved by
// MacroAssembler::wasmReserveStackChecked, in the case where the frame is
// "small", as determined by that function.
bool wasm::CreateStackMapForFunctionEntryTrap(
    const wasm::ArgTypeVector& argTypes, const RegisterOffsets& trapExitLayout,
    size_t trapExitLayoutWords, size_t nBytesReservedBeforeTrap,
    size_t nInboundStackArgBytes, wasm::StackMap** result) {
  // Ensure this is defined on all return paths.
  *result = nullptr;

  // The size of the wasm::Frame itself.
  const size_t nFrameBytes = sizeof(wasm::Frame);

  // The size of the register dump (trap) area.
  const size_t trapExitLayoutBytes = trapExitLayoutWords * sizeof(void*);

  // The stack map owns any alignment padding for incoming stack args.
  MOZ_ASSERT(nInboundStackArgBytes % sizeof(void*) == 0);
  const size_t nInboundStackArgBytesAligned =
      AlignStackArgAreaSize(nInboundStackArgBytes);
  const size_t numStackArgWords = nInboundStackArgBytesAligned / sizeof(void*);

  // This is the total number of bytes covered by the map.
  const size_t nTotalBytes = trapExitLayoutBytes + nBytesReservedBeforeTrap +
                             nFrameBytes + nInboundStackArgBytesAligned;

#ifndef DEBUG
  bool hasRefs = false;
  for (WasmABIArgIter i(argTypes); !i.done(); i++) {
    if (i.mirType() == MIRType::WasmAnyRef) {
      hasRefs = true;
      break;
    }
  }

  // There are no references, and this is a non-debug build, so don't bother
  // building the stackmap.
  if (!hasRefs) {
    return true;
  }
#endif

  wasm::StackMap* stackMap =
      wasm::StackMap::create(nTotalBytes / sizeof(void*));
  if (!stackMap) {
    return false;
  }
  stackMap->setExitStubWords(trapExitLayoutWords);
  stackMap->setFrameOffsetFromTop(nFrameBytes / sizeof(void*) +
                                  numStackArgWords);

  // REG DUMP AREA
  wasm::ExitStubMapVector trapExitExtras;
  if (!GenerateStackmapEntriesForTrapExit(
          argTypes, trapExitLayout, trapExitLayoutWords, &trapExitExtras)) {
    return false;
  }
  MOZ_ASSERT(trapExitExtras.length() == trapExitLayoutWords);

  for (size_t i = 0; i < trapExitLayoutWords; i++) {
    if (trapExitExtras[i]) {
      stackMap->set(i, wasm::StackMap::AnyRef);
    }
  }

  // INBOUND ARG AREA
  const size_t stackArgOffset =
      (trapExitLayoutBytes + nBytesReservedBeforeTrap + nFrameBytes) /
      sizeof(void*);
  for (WasmABIArgIter i(argTypes); !i.done(); i++) {
    ABIArg argLoc = *i;
    if (argLoc.kind() == ABIArg::Stack &&
        argTypes[i.index()] == MIRType::WasmAnyRef) {
      uint32_t offset = argLoc.offsetFromArgBase();
      MOZ_ASSERT(offset < nInboundStackArgBytes);
      MOZ_ASSERT(offset % sizeof(void*) == 0);
      stackMap->set(stackArgOffset + offset / sizeof(void*),
                    wasm::StackMap::AnyRef);
    }
  }

#ifdef DEBUG
  for (uint32_t i = 0; i < nFrameBytes / sizeof(void*); i++) {
    MOZ_ASSERT(stackMap->get(stackMap->header.numMappedWords -
                             stackMap->header.frameOffsetFromTop + i) ==
               StackMap::Kind::POD);
  }
#endif

  *result = stackMap;
  return true;
}

bool wasm::GenerateStackmapEntriesForTrapExit(
    const ArgTypeVector& args, const RegisterOffsets& trapExitLayout,
    const size_t trapExitLayoutNumWords, ExitStubMapVector* extras) {
  MOZ_ASSERT(extras->empty());

  if (!extras->appendN(false, trapExitLayoutNumWords)) {
    return false;
  }

  for (WasmABIArgIter i(args); !i.done(); i++) {
    if (!i->argInRegister() || i.mirType() != MIRType::WasmAnyRef) {
      continue;
    }

    size_t offsetFromTop = trapExitLayout.getOffset(i->gpr());

    // If this doesn't hold, the associated register wasn't saved by
    // the trap exit stub.  Better to crash now than much later, in
    // some obscure place, and possibly with security consequences.
    MOZ_RELEASE_ASSERT(offsetFromTop < trapExitLayoutNumWords);

    // offsetFromTop is an offset in words down from the highest
    // address in the exit stub save area.  Switch it around to be an
    // offset up from the bottom of the (integer register) save area.
    size_t offsetFromBottom = trapExitLayoutNumWords - 1 - offsetFromTop;

    (*extras)[offsetFromBottom] = true;
  }

  return true;
}

template <class Addr>
void wasm::EmitWasmPreBarrierGuard(MacroAssembler& masm, Register instance,
                                   Register scratch, Addr addr,
                                   Label* skipBarrier,
                                   BytecodeOffset* trapOffset) {
  // If no incremental GC has started, we don't need the barrier.
  masm.loadPtr(
      Address(instance, Instance::offsetOfAddressOfNeedsIncrementalBarrier()),
      scratch);
  masm.branchTest32(Assembler::Zero, Address(scratch, 0), Imm32(0x1),
                    skipBarrier);

  // If the previous value is not a GC thing, we don't need the barrier.
  FaultingCodeOffset fco = masm.loadPtr(addr, scratch);
  masm.branchWasmAnyRefIsGCThing(false, scratch, skipBarrier);

  // Emit metadata for a potential null access when reading the previous value.
  if (trapOffset) {
    masm.append(wasm::Trap::NullPointerDereference,
                wasm::TrapSite(TrapMachineInsnForLoadWord(), fco, *trapOffset));
  }
}

template void wasm::EmitWasmPreBarrierGuard<Address>(
    MacroAssembler& masm, Register instance, Register scratch, Address addr,
    Label* skipBarrier, BytecodeOffset* trapOffset);
template void wasm::EmitWasmPreBarrierGuard<BaseIndex>(
    MacroAssembler& masm, Register instance, Register scratch, BaseIndex addr,
    Label* skipBarrier, BytecodeOffset* trapOffset);

void wasm::EmitWasmPreBarrierCallImmediate(MacroAssembler& masm,
                                           Register instance, Register scratch,
                                           Register valueAddr,
                                           size_t valueOffset) {
  MOZ_ASSERT(valueAddr == PreBarrierReg);

  // Add the offset to the PreBarrierReg, if any.
  if (valueOffset != 0) {
    masm.addPtr(Imm32(valueOffset), valueAddr);
  }

#if defined(DEBUG) && defined(JS_CODEGEN_ARM64)
  // The prebarrier assumes that x28 == sp.
  Label ok;
  masm.Cmp(sp, vixl::Operand(x28));
  masm.B(&ok, Assembler::Equal);
  masm.breakpoint();
  masm.bind(&ok);
#endif

  // Load and call the pre-write barrier code. It will preserve all volatile
  // registers.
  masm.loadPtr(Address(instance, Instance::offsetOfPreBarrierCode()), scratch);
  masm.call(scratch);

  // Remove the offset we folded into PreBarrierReg, if any.
  if (valueOffset != 0) {
    masm.subPtr(Imm32(valueOffset), valueAddr);
  }
}

void wasm::EmitWasmPreBarrierCallIndex(MacroAssembler& masm, Register instance,
                                       Register scratch1, Register scratch2,
                                       BaseIndex addr) {
  MOZ_ASSERT(addr.base == PreBarrierReg);

  // Save the original base so we can restore it later.
  masm.movePtr(AsRegister(addr.base), scratch2);

  // Compute the final address into PrebarrierReg, as the barrier expects it
  // there.
  masm.computeEffectiveAddress(addr, PreBarrierReg);

#if defined(DEBUG) && defined(JS_CODEGEN_ARM64)
  // The prebarrier assumes that x28 == sp.
  Label ok;
  masm.Cmp(sp, vixl::Operand(x28));
  masm.B(&ok, Assembler::Equal);
  masm.breakpoint();
  masm.bind(&ok);
#endif

  // Load and call the pre-write barrier code. It will preserve all volatile
  // registers.
  masm.loadPtr(Address(instance, Instance::offsetOfPreBarrierCode()), scratch1);
  masm.call(scratch1);

  // Restore the original base
  masm.movePtr(scratch2, AsRegister(addr.base));
}

void wasm::EmitWasmPostBarrierGuard(MacroAssembler& masm,
                                    const Maybe<Register>& object,
                                    Register otherScratch, Register setValue,
                                    Label* skipBarrier) {
  // If there is a containing object and it is in the nursery, no barrier.
  if (object) {
    masm.branchPtrInNurseryChunk(Assembler::Equal, *object, otherScratch,
                                 skipBarrier);
  }

  // If the pointer being stored is to a tenured object, no barrier.
  masm.branchWasmAnyRefIsNurseryCell(false, setValue, otherScratch,
                                     skipBarrier);
}

#ifdef DEBUG
bool wasm::IsPlausibleStackMapKey(const uint8_t* nextPC) {
#  if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
  const uint8_t* insn = nextPC;
  return (insn[-2] == 0x0F && insn[-1] == 0x0B) ||           // ud2
         (insn[-2] == 0xFF && (insn[-1] & 0xF8) == 0xD0) ||  // call *%r_
         insn[-5] == 0xE8;                                   // call simm32

#  elif defined(JS_CODEGEN_ARM)
  const uint32_t* insn = (const uint32_t*)nextPC;
  return ((uintptr_t(insn) & 3) == 0) &&            // must be ARM, not Thumb
         (insn[-1] == 0xe7f000f0 ||                 // udf
          (insn[-1] & 0xfffffff0) == 0xe12fff30 ||  // blx reg (ARM, enc A1)
          (insn[-1] & 0x0f000000) == 0x0b000000);  // bl.cc simm24 (ARM, enc A1)

#  elif defined(JS_CODEGEN_ARM64)
  const uint32_t hltInsn = 0xd4a00000;
  const uint32_t* insn = (const uint32_t*)nextPC;
  return ((uintptr_t(insn) & 3) == 0) &&
         (insn[-1] == hltInsn ||                    // hlt
          (insn[-1] & 0xfffffc1f) == 0xd63f0000 ||  // blr reg
          (insn[-1] & 0xfc000000) == 0x94000000);   // bl simm26

#  elif defined(JS_CODEGEN_MIPS64)
  // TODO (bug 1699696): Implement this.  As for the platforms above, we need to
  // enumerate all code sequences that can precede the stackmap location.
  return true;
#  elif defined(JS_CODEGEN_LOONG64)
  // TODO(loong64): Implement IsValidStackMapKey.
  return true;
#  elif defined(JS_CODEGEN_RISCV64)
  const uint32_t* insn = (const uint32_t*)nextPC;
  return (((uintptr_t(insn) & 3) == 0) &&
          ((insn[-1] == 0x00006037 && insn[-2] == 0x00100073) ||  // break;
           ((insn[-1] & kBaseOpcodeMask) == JALR) ||
           ((insn[-1] & kBaseOpcodeMask) == JAL) ||
           (insn[-1] == 0x00100073 &&
            (insn[-2] & kITypeMask) == RO_CSRRWI)));  // wasm trap
#  else
  MOZ_CRASH("IsValidStackMapKey: requires implementation on this platform");
#  endif
}
#endif
