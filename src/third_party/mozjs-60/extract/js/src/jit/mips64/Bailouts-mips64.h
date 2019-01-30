/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips64_Bailouts_mips64_h
#define jit_mips64_Bailouts_mips64_h

#include "jit/Bailouts.h"
#include "jit/JitCompartment.h"

namespace js {
namespace jit {

class BailoutStack
{
    RegisterDump::FPUArray fpregs_;
    RegisterDump::GPRArray regs_;
    uintptr_t frameSize_;
    uintptr_t snapshotOffset_;

  public:
    MachineState machineState() {
        return MachineState::FromBailout(regs_, fpregs_);
    }
    uint32_t snapshotOffset() const {
        return snapshotOffset_;
    }
    uint32_t frameSize() const {
        return frameSize_;
    }
    uint8_t* parentStackPointer() {
        return (uint8_t*)this + sizeof(BailoutStack);
    }
    static size_t offsetOfFrameSize() {
        return offsetof(BailoutStack, frameSize_);
    }
};

} // namespace jit
} // namespace js

#endif /* jit_mips64_Bailouts_mips64_h */
