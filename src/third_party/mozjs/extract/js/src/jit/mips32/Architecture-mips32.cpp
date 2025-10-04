/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips32/Architecture-mips32.h"

#include "jit/RegisterSets.h"

namespace js {
namespace jit {

const char* const Registers::RegNames[] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
    "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
    "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

const uint32_t Allocatable = 14;

const Registers::SetType Registers::ArgRegMask = Registers::SharedArgRegMask;

const Registers::SetType Registers::JSCallMask =
    (1 << Registers::a2) | (1 << Registers::a3);

const Registers::SetType Registers::CallMask =
    (1 << Registers::v0) |
    (1 << Registers::v1);  // used for double-size returns

FloatRegisters::Encoding FloatRegisters::FromName(const char* name) {
  for (size_t i = 0; i < RegisterIdLimit; i++) {
    if (strcmp(GetName(i), name) == 0) {
      return Encoding(i);
    }
  }

  return Invalid;
}

FloatRegister FloatRegister::doubleOverlay() const {
  MOZ_ASSERT(isNotOdd());
  if (isSingle()) {
    return FloatRegister(code_, Double);
  }
  return *this;
}

FloatRegister FloatRegister::singleOverlay() const {
  MOZ_ASSERT(isNotOdd());
  if (isDouble()) {
    return FloatRegister(code_, Single);
  }
  return *this;
}

FloatRegisterSet FloatRegister::ReduceSetForPush(const FloatRegisterSet& s) {
#ifdef ENABLE_WASM_SIMD
#  error "Needs more careful logic if SIMD is enabled"
#endif

  LiveFloatRegisterSet mod;
  for (FloatRegisterIterator iter(s); iter.more(); ++iter) {
    // Even for single size registers save complete double register.
    mod.addUnchecked((*iter).doubleOverlay());
  }
  return mod.set();
}

uint32_t FloatRegister::GetPushSizeInBytes(const FloatRegisterSet& s) {
#ifdef ENABLE_WASM_SIMD
#  error "Needs more careful logic if SIMD is enabled"
#endif

  FloatRegisterSet ss = s.reduceSetForPush();
  uint64_t bits = ss.bits();
  // We are only pushing double registers.
  MOZ_ASSERT((bits & 0xFFFF) == 0);
  uint32_t ret = mozilla::CountPopulation32(bits) * sizeof(double);

  // Additional space needed by MacroAssembler::PushRegsInMask to ensure
  // correct alignment of double values.
  if (ret) {
    ret += sizeof(double);
  }

  return ret;
}
uint32_t FloatRegister::getRegisterDumpOffsetInBytes() {
  MOZ_ASSERT(isNotOdd());
  return id() * sizeof(float);
}

}  // namespace jit
}  // namespace js
