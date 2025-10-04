/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm64/Architecture-arm64.h"

#include <cstring>

#include "jit/arm64/vixl/Cpu-vixl.h"
#include "jit/FlushICache.h"  // js::jit::FlushICache
#include "jit/RegisterSets.h"

namespace js {
namespace jit {

Registers::Code Registers::FromName(const char* name) {
  // Check for some register aliases first.
  if (strcmp(name, "ip0") == 0) {
    return ip0;
  }
  if (strcmp(name, "ip1") == 0) {
    return ip1;
  }
  if (strcmp(name, "fp") == 0) {
    return fp;
  }

  for (uint32_t i = 0; i < Total; i++) {
    if (strcmp(GetName(i), name) == 0) {
      return Code(i);
    }
  }

  return Invalid;
}

FloatRegisters::Code FloatRegisters::FromName(const char* name) {
  for (size_t i = 0; i < Total; i++) {
    if (strcmp(GetName(i), name) == 0) {
      return Code(i);
    }
  }

  return Invalid;
}

// This must sync with GetPushSizeInBytes just below and also with
// MacroAssembler::PushRegsInMask.
FloatRegisterSet FloatRegister::ReduceSetForPush(const FloatRegisterSet& s) {
  SetType all = s.bits();
  SetType set128b =
      (all & FloatRegisters::AllSimd128Mask) >> FloatRegisters::ShiftSimd128;
  SetType doubleSet =
      (all & FloatRegisters::AllDoubleMask) >> FloatRegisters::ShiftDouble;
  SetType singleSet =
      (all & FloatRegisters::AllSingleMask) >> FloatRegisters::ShiftSingle;

  // See GetPushSizeInBytes.
  SetType set64b = (singleSet | doubleSet) & ~set128b;

  SetType reduced = (set128b << FloatRegisters::ShiftSimd128) |
                    (set64b << FloatRegisters::ShiftDouble);
  return FloatRegisterSet(reduced);
}

// Compute the size of the dump area for |s.ReduceSetForPush()|, as defined by
// MacroAssembler::PushRegsInMask for this target.
uint32_t FloatRegister::GetPushSizeInBytes(const FloatRegisterSet& s) {
  SetType all = s.bits();
  SetType set128b =
      (all & FloatRegisters::AllSimd128Mask) >> FloatRegisters::ShiftSimd128;
  SetType doubleSet =
      (all & FloatRegisters::AllDoubleMask) >> FloatRegisters::ShiftDouble;
  SetType singleSet =
      (all & FloatRegisters::AllSingleMask) >> FloatRegisters::ShiftSingle;

  // PushRegsInMask pushes singles as if they were doubles.  Also we need to
  // remove singles or doubles which are also pushed as part of a vector
  // register.
  SetType set64b = (singleSet | doubleSet) & ~set128b;

  // The "+ 1) & ~1" is to take into account the alignment hole below the
  // double-reg dump area.  See MacroAssembler::PushRegsInMaskSizeInBytes.
  return ((set64b.size() + 1) & ~1) * sizeof(double) +
         set128b.size() * SizeOfSimd128;
}

uint32_t FloatRegister::getRegisterDumpOffsetInBytes() {
  // See block comment in MacroAssembler.h for further required invariants.
  static_assert(sizeof(jit::FloatRegisters::RegisterContent) == 16);
  return encoding() * sizeof(jit::FloatRegisters::RegisterContent);
}

// For N in 0..31, if any of sN, dN or qN is a member of `s`, the returned set
// will contain all of sN, dN and qN.
FloatRegisterSet FloatRegister::BroadcastToAllSizes(const FloatRegisterSet& s) {
  SetType all = s.bits();
  SetType set128b =
      (all & FloatRegisters::AllSimd128Mask) >> FloatRegisters::ShiftSimd128;
  SetType doubleSet =
      (all & FloatRegisters::AllDoubleMask) >> FloatRegisters::ShiftDouble;
  SetType singleSet =
      (all & FloatRegisters::AllSingleMask) >> FloatRegisters::ShiftSingle;

  SetType merged = set128b | doubleSet | singleSet;
  SetType broadcasted = (merged << FloatRegisters::ShiftSimd128) |
                        (merged << FloatRegisters::ShiftDouble) |
                        (merged << FloatRegisters::ShiftSingle);

  return FloatRegisterSet(broadcasted);
}

uint32_t GetARM64Flags() { return 0; }

// CPU flags handling on ARM64 is currently different from other platforms:
// the flags are computed and stored per-assembler and are thus "always
// computed".
bool CPUFlagsHaveBeenComputed() { return true; }

void FlushICache(void* code, size_t size) {
  vixl::CPU::EnsureIAndDCacheCoherency(code, size);
}

void FlushExecutionContext() { vixl::CPU::FlushExecutionContext(); }

}  // namespace jit
}  // namespace js
