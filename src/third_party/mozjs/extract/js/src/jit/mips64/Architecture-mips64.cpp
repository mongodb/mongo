/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips64/Architecture-mips64.h"

#include "jit/RegisterSets.h"

namespace js {
namespace jit {

const char* const Registers::RegNames[] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "a4", "a5", "a6",
    "a7",   "t0", "t1", "t2", "t3", "s0", "s1", "s2", "s3", "s4", "s5",
    "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

const uint32_t Allocatable = 22;

const Registers::SetType Registers::ArgRegMask =
    Registers::SharedArgRegMask | (1 << a4) | (1 << a5) | (1 << a6) | (1 << a7);

const Registers::SetType Registers::JSCallMask = (1 << Registers::v1);

const Registers::SetType Registers::CallMask = (1 << Registers::v0);

FloatRegisters::Encoding FloatRegisters::FromName(const char* name) {
  for (size_t i = 0; i < Total; i++) {
    if (strcmp(GetName(Encoding(i)), name) == 0) {
      return Encoding(i);
    }
  }

  return Invalid;
}

FloatRegister FloatRegister::singleOverlay() const {
  MOZ_ASSERT(!isInvalid());
  if (kind_ == Codes::Double) {
    return FloatRegister(reg_, Codes::Single);
  }
  return *this;
}

FloatRegister FloatRegister::doubleOverlay() const {
  MOZ_ASSERT(!isInvalid());
  if (kind_ != Codes::Double) {
    return FloatRegister(reg_, Codes::Double);
  }
  return *this;
}

FloatRegisterSet FloatRegister::ReduceSetForPush(const FloatRegisterSet& s) {
#ifdef ENABLE_WASM_SIMD
#  error "Needs more careful logic if SIMD is enabled"
#endif

  LiveFloatRegisterSet mod;
  for (FloatRegisterIterator iter(s); iter.more(); ++iter) {
    if ((*iter).isSingle()) {
      // Even for single size registers save complete double register.
      mod.addUnchecked((*iter).doubleOverlay());
    } else {
      mod.addUnchecked(*iter);
    }
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
  MOZ_ASSERT((bits & 0xffffffff) == 0);
  uint32_t ret = mozilla::CountPopulation32(bits >> 32) * sizeof(double);
  return ret;
}
uint32_t FloatRegister::getRegisterDumpOffsetInBytes() {
  return id() * sizeof(double);
}

}  // namespace jit
}  // namespace js
