/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips64_LIR_mips64_h
#define jit_mips64_LIR_mips64_h

namespace js {
namespace jit {

class LUnbox : public LInstructionHelper<1, 1, 0>
{
  public:
    LIR_HEADER(Unbox);

    explicit LUnbox(const LAllocation& input) {
        setOperand(0, input);
    }

    static const size_t Input = 0;

    MUnbox* mir() const {
        return mir_->toUnbox();
    }
    const char* extraName() const {
        return StringFromMIRType(mir()->type());
    }
};

class LUnboxFloatingPoint : public LUnbox
{
    MIRType type_;

  public:
    LIR_HEADER(UnboxFloatingPoint);

    LUnboxFloatingPoint(const LAllocation& input, MIRType type)
      : LUnbox(input),
        type_(type)
    { }

    MIRType type() const {
        return type_;
    }
};

} // namespace jit
} // namespace js

#endif /* jit_mips64_LIR_mips64_h */
