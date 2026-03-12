/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips_shared_LIR_mips_shared_h
#define jit_mips_shared_LIR_mips_shared_h

namespace js {
namespace jit {

class LUDivOrMod : public LBinaryMath<0> {
 public:
  LIR_HEADER(UDivOrMod);

  LUDivOrMod() : LBinaryMath(classOpcode) {}

  MBinaryArithInstruction* mir() const {
    MOZ_ASSERT(mir_->isDiv() || mir_->isMod());
    return static_cast<MBinaryArithInstruction*>(mir_);
  }

  bool canBeDivideByZero() const {
    if (mir_->isMod()) {
      return mir_->toMod()->canBeDivideByZero();
    }
    return mir_->toDiv()->canBeDivideByZero();
  }

  bool trapOnError() const {
    if (mir_->isMod()) {
      return mir_->toMod()->trapOnError();
    }
    return mir_->toDiv()->trapOnError();
  }

  wasm::TrapSiteDesc trapSiteDesc() const {
    MOZ_ASSERT(mir_->isDiv() || mir_->isMod());
    if (mir_->isMod()) {
      return mir_->toMod()->trapSiteDesc();
    }
    return mir_->toDiv()->trapSiteDesc();
  }
};

}  // namespace jit
}  // namespace js

#endif /* jit_mips_shared_LIR_mips_shared_h */
