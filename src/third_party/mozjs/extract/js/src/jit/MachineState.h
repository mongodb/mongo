/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MachineState_h
#define jit_MachineState_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Variant.h"

#include <stdint.h>

#include "jit/Registers.h"
#include "jit/RegisterSets.h"

namespace js::jit {

// Information needed to recover machine register state. This supports two
// different modes:
//
// * Bailouts: all registers are pushed on the stack as part of the bailout
//   process, so MachineState simply points to these FPU/GPR arrays.
//   See RegisterDump and BailoutStack.
//
// * Safepoints: live registers are pushed on the stack before a VM call, so
//   MachineState stores the register sets and a pointer to the stack memory
//   where these registers were pushed. This is also used by exception bailouts.
class MOZ_STACK_CLASS MachineState {
  struct NullState {};

  struct BailoutState {
    RegisterDump::FPUArray& floatRegs;
    RegisterDump::GPRArray& regs;

    BailoutState(RegisterDump::FPUArray& floatRegs,
                 RegisterDump::GPRArray& regs)
        : floatRegs(floatRegs), regs(regs) {}
  };

  struct SafepointState {
    FloatRegisterSet floatRegs;
    GeneralRegisterSet regs;
    // Pointers to the start of the pushed |floatRegs| and |regs| on the stack.
    // This is the value of the stack pointer right before the first register
    // was pushed.
    char* floatSpillBase;
    uintptr_t* spillBase;

    SafepointState(const FloatRegisterSet& floatRegs,
                   const GeneralRegisterSet& regs, char* floatSpillBase,
                   uintptr_t* spillBase)
        : floatRegs(floatRegs),
          regs(regs),
          floatSpillBase(floatSpillBase),
          spillBase(spillBase) {}
    uintptr_t* addressOfRegister(Register reg) const;
    char* addressOfRegister(FloatRegister reg) const;
  };
  using State = mozilla::Variant<NullState, BailoutState, SafepointState>;
  State state_{NullState()};

 public:
  MachineState() = default;
  MachineState(const MachineState& other) = default;
  MachineState& operator=(const MachineState& other) = default;

  static MachineState FromBailout(RegisterDump::GPRArray& regs,
                                  RegisterDump::FPUArray& fpregs) {
    MachineState res;
    res.state_.emplace<BailoutState>(fpregs, regs);
    return res;
  }

  static MachineState FromSafepoint(const FloatRegisterSet& floatRegs,
                                    const GeneralRegisterSet& regs,
                                    char* floatSpillBase,
                                    uintptr_t* spillBase) {
    MachineState res;
    res.state_.emplace<SafepointState>(floatRegs, regs, floatSpillBase,
                                       spillBase);
    return res;
  }

  bool has(Register reg) const {
    if (state_.is<BailoutState>()) {
      return true;
    }
    return state_.as<SafepointState>().regs.hasRegisterIndex(reg);
  }
  bool has(FloatRegister reg) const {
    if (state_.is<BailoutState>()) {
      return true;
    }
    return state_.as<SafepointState>().floatRegs.hasRegisterIndex(reg);
  }

  uintptr_t read(Register reg) const;
  template <typename T>
  T read(FloatRegister reg) const;

  // Used by moving GCs to update pointers.
  void write(Register reg, uintptr_t value) const;
};

}  // namespace js::jit

#endif /* jit_MachineState_h */
