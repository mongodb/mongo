/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_none_SharedICHelpers_none_h
#define jit_none_SharedICHelpers_none_h

namespace js {
namespace jit {

static const size_t ICStackValueOffset = 0;

inline void EmitRestoreTailCallReg(MacroAssembler&) { MOZ_CRASH(); }
inline void EmitRepushTailCallReg(MacroAssembler&) { MOZ_CRASH(); }
inline void EmitCallIC(CodeOffset*, MacroAssembler&) { MOZ_CRASH(); }
inline void EmitEnterTypeMonitorIC(MacroAssembler&, size_t v = 0) { MOZ_CRASH(); }
inline void EmitReturnFromIC(MacroAssembler&) { MOZ_CRASH(); }
inline void EmitChangeICReturnAddress(MacroAssembler&, Register) { MOZ_CRASH(); }
inline void EmitBaselineLeaveStubFrame(MacroAssembler&, bool v = false) { MOZ_CRASH(); }
inline void EmitStubGuardFailure(MacroAssembler&) { MOZ_CRASH(); }

template <typename T> inline void EmitPreBarrier(MacroAssembler&, T, MIRType) { MOZ_CRASH(); }

} // namespace jit
} // namespace js

#endif /* jit_none_SharedICHelpers_none_h */
