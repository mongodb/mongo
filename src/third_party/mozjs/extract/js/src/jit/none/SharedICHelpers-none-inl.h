/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_none_SharedICHelpers_none_inl_h
#define jit_none_SharedICHelpers_none_inl_h

#include "jit/SharedICHelpers.h"

namespace js {
namespace jit {

inline void EmitBaselineTailCallVM(TrampolinePtr, MacroAssembler&, uint32_t) { MOZ_CRASH(); }
inline void EmitIonTailCallVM(TrampolinePtr, MacroAssembler&, uint32_t) { MOZ_CRASH(); }
inline void EmitBaselineCreateStubFrameDescriptor(MacroAssembler&, Register, uint32_t) { MOZ_CRASH(); }
inline void EmitBaselineCallVM(TrampolinePtr, MacroAssembler&) { MOZ_CRASH(); }

static const uint32_t STUB_FRAME_SIZE = 0;
static const uint32_t STUB_FRAME_SAVED_STUB_OFFSET = 0;

inline void EmitBaselineEnterStubFrame(MacroAssembler&, Register) { MOZ_CRASH(); }

} // namespace jit
} // namespace js

#endif /* jit_none_SharedICHelpers_none_inl_h */
