/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips32_Bailouts_mips32_h
#define jit_mips32_Bailouts_mips32_h

#include "jit/Bailouts.h"
#include "jit/JitCompartment.h"

namespace js {
namespace jit {

class BailoutStack
{
    uintptr_t frameClassId_;
    // This is pushed in the bailout handler. Both entry points into the
    // handler inserts their own value int lr, which is then placed onto the
    // stack along with frameClassId_ above. This should be migrated to ip.
  public:
    union {
        uintptr_t frameSize_;
        uintptr_t tableOffset_;
    };

  protected:
    RegisterDump::FPUArray fpregs_;
    RegisterDump::GPRArray regs_;

    uintptr_t snapshotOffset_;
    uintptr_t padding_;

  public:
    FrameSizeClass frameClass() const {
        return FrameSizeClass::FromClass(frameClassId_);
    }
    uintptr_t tableOffset() const {
        MOZ_ASSERT(frameClass() != FrameSizeClass::None());
        return tableOffset_;
    }
    uint32_t frameSize() const {
        if (frameClass() == FrameSizeClass::None())
            return frameSize_;
        return frameClass().frameSize();
    }
    MachineState machine() {
        return MachineState::FromBailout(regs_, fpregs_);
    }
    SnapshotOffset snapshotOffset() const {
        MOZ_ASSERT(frameClass() == FrameSizeClass::None());
        return snapshotOffset_;
    }
    uint8_t* parentStackPointer() const {
        if (frameClass() == FrameSizeClass::None())
            return (uint8_t*)this + sizeof(BailoutStack);
        return (uint8_t*)this + offsetof(BailoutStack, snapshotOffset_);
    }
    static size_t offsetOfFrameClass() {
        return offsetof(BailoutStack, frameClassId_);
    }
    static size_t offsetOfFrameSize() {
        return offsetof(BailoutStack, frameSize_);
    }
    static size_t offsetOfFpRegs() {
        return offsetof(BailoutStack, fpregs_);
    }
    static size_t offsetOfRegs() {
        return offsetof(BailoutStack, regs_);
    }
};

} // namespace jit
} // namespace js

#endif /* jit_mips32_Bailouts_mips32_h */
