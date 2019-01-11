/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Bailouts.h"
#include "jit/JitCompartment.h"
#include "vm/JSCompartment.h"
#include "vm/JSContext.h"

using namespace js;
using namespace js::jit;

#if defined(_WIN32)
# pragma pack(push, 1)
#endif

namespace js {
namespace jit {

class BailoutStack
{
    uintptr_t frameClassId_;
    RegisterDump::FPUArray fpregs_;
    RegisterDump::GPRArray regs_;
    union {
        uintptr_t frameSize_;
        uintptr_t tableOffset_;
    };
    uintptr_t snapshotOffset_;

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

} // namespace jit
} // namespace js

#if defined(_WIN32)
# pragma pack(pop)
#endif

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
    JSRuntime* rt = activation->compartment()->runtimeFromActiveCooperatingThread();
    TrampolinePtr code = rt->jitRuntime()->getBailoutTable(bailout->frameClass());
#ifdef DEBUG
    uint32_t tableSize = rt->jitRuntime()->getBailoutTableSize(bailout->frameClass());
#endif
    uintptr_t tableOffset = bailout->tableOffset();
    uintptr_t tableStart = reinterpret_cast<uintptr_t>(code.value);

    MOZ_ASSERT(tableOffset >= tableStart &&
               tableOffset < tableStart + tableSize);
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
