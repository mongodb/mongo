/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineDebugModeOSR_h
#define jit_BaselineDebugModeOSR_h

#include "jit/BaselineFrame.h"
#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/JSJitFrameIter.h"

#include "vm/Debugger.h"

namespace js {
namespace jit {

// Note that this file and the corresponding .cpp implement debug mode
// on-stack recompilation. This is to be distinguished from ordinary
// Baseline->Ion OSR, which is used to jump into compiled loops.

//
// A volatile location due to recompilation of an on-stack baseline script
// (e.g., for debug mode toggling).
//
// It is usually used in fallback stubs which may trigger on-stack
// recompilation by calling out into the VM. Example use:
//
//     DebugModeOSRVolatileStub<FallbackStubT*> stub(frame, stub_)
//
//     // Call out to the VM
//     // Other effectful operations like TypeScript::Monitor
//
//     if (stub.invalid())
//         return true;
//
//     // First use of stub after VM call.
//
template <typename T>
class DebugModeOSRVolatileStub
{
    ICStubCompiler::Engine engine_;
    T stub_;
    BaselineFrame* frame_;
    uint32_t pcOffset_;

  public:
    DebugModeOSRVolatileStub(ICStubCompiler::Engine engine, BaselineFrame* frame,
                             ICFallbackStub* stub)
      : engine_(engine),
        stub_(static_cast<T>(stub)),
        frame_(frame),
        pcOffset_(stub->icEntry()->pcOffset())
    { }

    DebugModeOSRVolatileStub(BaselineFrame* frame, ICFallbackStub* stub)
      : engine_(ICStubCompiler::Engine::Baseline),
        stub_(static_cast<T>(stub)),
        frame_(frame),
        pcOffset_(stub->icEntry()->pcOffset())
    { }

    bool invalid() const {
        if (engine_ == ICStubCompiler::Engine::IonSharedIC)
            return stub_->invalid();
        MOZ_ASSERT(!frame_->isHandlingException());
        ICEntry& entry = frame_->script()->baselineScript()->icEntryFromPCOffset(pcOffset_);
        return stub_ != entry.fallbackStub();
    }

    operator const T&() const { MOZ_ASSERT(!invalid()); return stub_; }
    T operator->() const { MOZ_ASSERT(!invalid()); return stub_; }
    T* address() { MOZ_ASSERT(!invalid()); return &stub_; }
    const T* address() const { MOZ_ASSERT(!invalid()); return &stub_; }
    T& get() { MOZ_ASSERT(!invalid()); return stub_; }
    const T& get() const { MOZ_ASSERT(!invalid()); return stub_; }

    bool operator!=(const T& other) const { MOZ_ASSERT(!invalid()); return stub_ != other; }
    bool operator==(const T& other) const { MOZ_ASSERT(!invalid()); return stub_ == other; }
};

//
// A JitFrameIter that updates internal JSJitFrameIter in case of
// recompilation of an on-stack baseline script.
//

class DebugModeOSRVolatileJitFrameIter : public JitFrameIter
{
    DebugModeOSRVolatileJitFrameIter** stack;
    DebugModeOSRVolatileJitFrameIter* prev;

  public:
    explicit DebugModeOSRVolatileJitFrameIter(JSContext* cx)
      : JitFrameIter(cx->activation()->asJit(), /* mustUnwindActivation */ true)
    {
        stack = &cx->liveVolatileJitFrameIter_.ref();
        prev = *stack;
        *stack = this;
    }

    ~DebugModeOSRVolatileJitFrameIter() {
        MOZ_ASSERT(*stack == this);
        *stack = prev;
    }

    static void forwardLiveIterators(const CooperatingContext& target,
                                     uint8_t* oldAddr, uint8_t* newAddr);
};

//
// Auxiliary info to help the DebugModeOSRHandler fix up state.
//
struct BaselineDebugModeOSRInfo
{
    uint8_t* resumeAddr;
    jsbytecode* pc;
    PCMappingSlotInfo slotInfo;
    ICEntry::Kind frameKind;

    // Filled in by SyncBaselineDebugModeOSRInfo.
    uintptr_t stackAdjust;
    Value valueR0;
    Value valueR1;

    BaselineDebugModeOSRInfo(jsbytecode* pc, ICEntry::Kind kind)
      : resumeAddr(nullptr),
        pc(pc),
        slotInfo(0),
        frameKind(kind),
        stackAdjust(0),
        valueR0(UndefinedValue()),
        valueR1(UndefinedValue())
    { }

    void popValueInto(PCMappingSlotInfo::SlotLocation loc, Value* vp);
};

MOZ_MUST_USE bool
RecompileOnStackBaselineScriptsForDebugMode(JSContext* cx,
                                            const Debugger::ExecutionObservableSet& obs,
                                            Debugger::IsObserving observing);

} // namespace jit
} // namespace js

#endif // jit_BaselineDebugModeOSR_h
