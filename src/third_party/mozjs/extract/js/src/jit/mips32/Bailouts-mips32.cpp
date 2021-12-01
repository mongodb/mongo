/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips32/Bailouts-mips32.h"

#include "vm/JSCompartment.h"
#include "vm/JSContext.h"

using namespace js;
using namespace js::jit;

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
