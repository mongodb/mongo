/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jscntxt.h"
#include "jscompartment.h"

#include "jit/arm/Assembler-arm.h"
#include "jit/Bailouts.h"
#include "jit/JitCompartment.h"

using namespace js;
using namespace js::jit;

namespace js {
namespace jit {

class BailoutStack
{
    uintptr_t frameClassId_;
    // This is pushed in the bailout handler. Both entry points into the handler
    // inserts their own value int lr, which is then placed onto the stack along
    // with frameClassId_ above. This should be migrated to ip.
  public:
    union {
        uintptr_t frameSize_;
        uintptr_t tableOffset_;
    };

  protected: // Silence Clang warning about unused private fields.
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
};

// Make sure the compiler doesn't add extra padding.
static_assert((sizeof(BailoutStack) % 8) == 0, "BailoutStack should be 8-byte aligned.");

} // namespace jit
} // namespace js

BailoutFrameInfo::BailoutFrameInfo(const JitActivationIterator& activations,
                                   BailoutStack* bailout)
  : machine_(bailout->machine())
{
    uint8_t* sp = bailout->parentStackPointer();
    framePointer_ = sp + bailout->frameSize();
    topFrameSize_ = framePointer_ - sp;

    JSScript* script = ScriptFromCalleeToken(((JitFrameLayout*) framePointer_)->calleeToken());
    JitActivation* activation = activations.activation()->asJit();
    topIonScript_ = script->ionScript();

    attachOnJitActivation(activations);

    if (bailout->frameClass() == FrameSizeClass::None()) {
        snapshotOffset_ = bailout->snapshotOffset();
        return;
    }

    // Compute the snapshot offset from the bailout ID.
    JSRuntime* rt = activation->compartment()->runtimeFromMainThread();
    JitCode* code = rt->jitRuntime()->getBailoutTable(bailout->frameClass());
    uintptr_t tableOffset = bailout->tableOffset();
    uintptr_t tableStart = reinterpret_cast<uintptr_t>(Assembler::BailoutTableStart(code->raw()));

    MOZ_ASSERT(tableOffset >= tableStart &&
               tableOffset < tableStart + code->instructionsSize());
    MOZ_ASSERT((tableOffset - tableStart) % BAILOUT_TABLE_ENTRY_SIZE == 0);

    uint32_t bailoutId = ((tableOffset - tableStart) / BAILOUT_TABLE_ENTRY_SIZE) - 1;
    MOZ_ASSERT(bailoutId < BAILOUT_TABLE_SIZE);

    snapshotOffset_ = topIonScript_->bailoutToSnapshot(bailoutId);
}

BailoutFrameInfo::BailoutFrameInfo(const JitActivationIterator& activations,
                                   InvalidationBailoutStack* bailout)
  : machine_(bailout->machine())
{
    framePointer_ = (uint8_t*) bailout->fp();
    topFrameSize_ = framePointer_ - bailout->sp();
    topIonScript_ = bailout->ionScript();
    attachOnJitActivation(activations);

    uint8_t* returnAddressToFp_ = bailout->osiPointReturnAddress();
    const OsiIndex* osiIndex = topIonScript_->getOsiIndex(returnAddressToFp_);
    snapshotOffset_ = osiIndex->snapshotOffset();
}
