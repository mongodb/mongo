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

namespace js {
namespace wasm {

wasm::StackMap* ConvertStackMapBoolVectorToStackMap(
    const StackMapBoolVector& vec, bool hasRefs) {
  wasm::StackMap* stackMap = wasm::StackMap::create(vec.length());
  if (!stackMap) {
    return nullptr;
  }

  bool hasRefsObserved = false;
  size_t i = 0;
  for (bool b : vec) {
    if (b) {
      stackMap->setBit(i);
      hasRefsObserved = true;
    }
    i++;
  }
  MOZ_RELEASE_ASSERT(hasRefs == hasRefsObserved);

  return stackMap;
}

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
bool CreateStackMapForFunctionEntryTrap(const wasm::ArgTypeVector& argTypes,
                                        const MachineState& trapExitLayout,
                                        size_t trapExitLayoutWords,
                                        size_t nBytesReservedBeforeTrap,
                                        size_t nInboundStackArgBytes,
                                        wasm::StackMap** result) {
  // Ensure this is defined on all return paths.
  *result = nullptr;

  // The size of the wasm::Frame itself.
  const size_t nFrameBytes = sizeof(wasm::Frame);

  // The size of the register dump (trap) area.
  const size_t trapExitLayoutBytes = trapExitLayoutWords * sizeof(void*);

  // This is the total number of bytes covered by the map.
  const DebugOnly<size_t> nTotalBytes = trapExitLayoutBytes +
                                        nBytesReservedBeforeTrap + nFrameBytes +
                                        nInboundStackArgBytes;

  // Create the stackmap initially in this vector.  Since most frames will
  // contain 128 or fewer words, heap allocation is avoided in the majority of
  // cases.  vec[0] is for the lowest address in the map, vec[N-1] is for the
  // highest address in the map.
  StackMapBoolVector vec;

  // Keep track of whether we've actually seen any refs.
  bool hasRefs = false;

  // REG DUMP AREA
  wasm::ExitStubMapVector trapExitExtras;
  if (!GenerateStackmapEntriesForTrapExit(
          argTypes, trapExitLayout, trapExitLayoutWords, &trapExitExtras)) {
    return false;
  }
  MOZ_ASSERT(trapExitExtras.length() == trapExitLayoutWords);

  if (!vec.appendN(false, trapExitLayoutWords)) {
    return false;
  }
  for (size_t i = 0; i < trapExitLayoutWords; i++) {
    vec[i] = trapExitExtras[i];
    hasRefs |= vec[i];
  }

  // SPACE RESERVED BEFORE TRAP
  MOZ_ASSERT(nBytesReservedBeforeTrap % sizeof(void*) == 0);
  if (!vec.appendN(false, nBytesReservedBeforeTrap / sizeof(void*))) {
    return false;
  }

  // SPACE FOR FRAME
  if (!vec.appendN(false, nFrameBytes / sizeof(void*))) {
    return false;
  }

  // INBOUND ARG AREA
  MOZ_ASSERT(nInboundStackArgBytes % sizeof(void*) == 0);
  const size_t numStackArgWords = nInboundStackArgBytes / sizeof(void*);

  const size_t wordsSoFar = vec.length();
  if (!vec.appendN(false, numStackArgWords)) {
    return false;
  }

  for (WasmABIArgIter i(argTypes); !i.done(); i++) {
    ABIArg argLoc = *i;
    if (argLoc.kind() == ABIArg::Stack &&
        argTypes[i.index()] == MIRType::RefOrNull) {
      uint32_t offset = argLoc.offsetFromArgBase();
      MOZ_ASSERT(offset < nInboundStackArgBytes);
      MOZ_ASSERT(offset % sizeof(void*) == 0);
      vec[wordsSoFar + offset / sizeof(void*)] = true;
      hasRefs = true;
    }
  }

#ifndef DEBUG
  // We saw no references, and this is a non-debug build, so don't bother
  // building the stackmap.
  if (!hasRefs) {
    return true;
  }
#endif

  // Convert vec into a wasm::StackMap.
  MOZ_ASSERT(vec.length() * sizeof(void*) == nTotalBytes);
  wasm::StackMap* stackMap = ConvertStackMapBoolVectorToStackMap(vec, hasRefs);
  if (!stackMap) {
    return false;
  }
  stackMap->setExitStubWords(trapExitLayoutWords);

  stackMap->setFrameOffsetFromTop(nFrameBytes / sizeof(void*) +
                                  numStackArgWords);
#ifdef DEBUG
  for (uint32_t i = 0; i < nFrameBytes / sizeof(void*); i++) {
    MOZ_ASSERT(stackMap->getBit(stackMap->numMappedWords -
                                stackMap->frameOffsetFromTop + i) == 0);
  }
#endif

  *result = stackMap;
  return true;
}

bool GenerateStackmapEntriesForTrapExit(const ArgTypeVector& args,
                                        const MachineState& trapExitLayout,
                                        const size_t trapExitLayoutNumWords,
                                        ExitStubMapVector* extras) {
  MOZ_ASSERT(extras->empty());

  // If this doesn't hold, we can't distinguish saved and not-saved
  // registers in the MachineState.  See MachineState::MachineState().
  MOZ_ASSERT(trapExitLayoutNumWords < 0x100);

  if (!extras->appendN(false, trapExitLayoutNumWords)) {
    return false;
  }

  for (WasmABIArgIter i(args); !i.done(); i++) {
    if (!i->argInRegister() || i.mirType() != MIRType::RefOrNull) {
      continue;
    }

    size_t offsetFromTop =
        reinterpret_cast<size_t>(trapExitLayout.address(i->gpr()));

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

void EmitWasmPreBarrierGuard(MacroAssembler& masm, Register tls,
                             Register scratch, Register valueAddr,
                             Label* skipBarrier) {
  // If no incremental GC has started, we don't need the barrier.
  masm.loadPtr(
      Address(tls, offsetof(TlsData, addressOfNeedsIncrementalBarrier)),
      scratch);
  masm.branchTest32(Assembler::Zero, Address(scratch, 0), Imm32(0x1),
                    skipBarrier);

  // If the previous value is null, we don't need the barrier.
  masm.loadPtr(Address(valueAddr, 0), scratch);
  masm.branchTestPtr(Assembler::Zero, scratch, scratch, skipBarrier);
}

void EmitWasmPreBarrierCall(MacroAssembler& masm, Register tls,
                            Register scratch, Register valueAddr) {
  MOZ_ASSERT(valueAddr == PreBarrierReg);

  masm.loadPtr(Address(tls, offsetof(TlsData, instance)), scratch);
  masm.loadPtr(Address(scratch, Instance::offsetOfPreBarrierCode()), scratch);
#if defined(DEBUG) && defined(JS_CODEGEN_ARM64)
  // The prebarrier assumes that x28 == sp.
  Label ok;
  masm.Cmp(sp, vixl::Operand(x28));
  masm.B(&ok, Assembler::Equal);
  masm.breakpoint();
  masm.bind(&ok);
#endif
  masm.call(scratch);
}

void EmitWasmPostBarrierGuard(MacroAssembler& masm,
                              const Maybe<Register>& object,
                              Register otherScratch, Register setValue,
                              Label* skipBarrier) {
  // If the pointer being stored is null, no barrier.
  masm.branchTestPtr(Assembler::Zero, setValue, setValue, skipBarrier);

  // If there is a containing object and it is in the nursery, no barrier.
  if (object) {
    masm.branchPtrInNurseryChunk(Assembler::Equal, *object, otherScratch,
                                 skipBarrier);
  }

  // If the pointer being stored is to a tenured object, no barrier.
  masm.branchPtrInNurseryChunk(Assembler::NotEqual, setValue, otherScratch,
                               skipBarrier);
}

#ifdef DEBUG
bool IsValidStackMapKey(bool debugEnabled, const uint8_t* nextPC) {
#  if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
  const uint8_t* insn = nextPC;
  return (insn[-2] == 0x0F && insn[-1] == 0x0B) ||           // ud2
         (insn[-2] == 0xFF && (insn[-1] & 0xF8) == 0xD0) ||  // call *%r_
         insn[-5] == 0xE8 ||                                 // call simm32
         (debugEnabled && insn[-5] == 0x0F && insn[-4] == 0x1F &&
          insn[-3] == 0x44 && insn[-2] == 0x00 &&
          insn[-1] == 0x00);  // nop_five

#  elif defined(JS_CODEGEN_ARM)
  const uint32_t* insn = (const uint32_t*)nextPC;
  return ((uintptr_t(insn) & 3) == 0) &&              // must be ARM, not Thumb
         (insn[-1] == 0xe7f000f0 ||                   // udf
          (insn[-1] & 0xfffffff0) == 0xe12fff30 ||    // blx reg (ARM, enc A1)
          (insn[-1] & 0xff000000) == 0xeb000000 ||    // bl simm24 (ARM, enc A1)
          (debugEnabled && insn[-1] == 0xe320f000));  // "as_nop"

#  elif defined(JS_CODEGEN_ARM64)
  const uint32_t hltInsn = 0xd4a00000;
  const uint32_t* insn = (const uint32_t*)nextPC;
  return ((uintptr_t(insn) & 3) == 0) &&
         (insn[-1] == hltInsn ||                      // hlt
          (insn[-1] & 0xfffffc1f) == 0xd63f0000 ||    // blr reg
          (insn[-1] & 0xfc000000) == 0x94000000 ||    // bl simm26
          (debugEnabled && insn[-1] == 0xd503201f));  // nop

#  elif defined(JS_CODEGEN_MIPS64)
  // TODO (bug 1699696): Implement this.  As for the platforms above, we need to
  // enumerate all code sequences that can precede the stackmap location.
  return true;
#  else
  MOZ_CRASH("IsValidStackMapKey: requires implementation on this platform");
#  endif
}
#endif

}  // namespace wasm
}  // namespace js
