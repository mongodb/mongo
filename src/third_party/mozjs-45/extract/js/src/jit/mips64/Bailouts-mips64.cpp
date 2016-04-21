/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips64/Bailouts-mips64.h"

#include "jscntxt.h"
#include "jscompartment.h"

using namespace js;
using namespace js::jit;

BailoutFrameInfo::BailoutFrameInfo(const JitActivationIterator& activations,
                                   BailoutStack* bailout)
  : machine_(bailout->machineState())
{
    uint8_t* sp = bailout->parentStackPointer();
    framePointer_ = sp + bailout->frameSize();
    topFrameSize_ = framePointer_ - sp;

    JSScript* script = ScriptFromCalleeToken(((JitFrameLayout*) framePointer_)->calleeToken());
    topIonScript_ = script->ionScript();

    attachOnJitActivation(activations);
    snapshotOffset_ = bailout->snapshotOffset();
}
