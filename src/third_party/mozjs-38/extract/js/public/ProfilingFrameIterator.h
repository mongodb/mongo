/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_ProfilingFrameIterator_h
#define js_ProfilingFrameIterator_h

#include "mozilla/Alignment.h"

#include <stdint.h>

#include "js/Utility.h"

class JSAtom;
struct JSRuntime;

namespace js {
    class Activation;
    class AsmJSProfilingFrameIterator;
    namespace jit {
        class JitActivation;
        class JitProfilingFrameIterator;
    }
}

namespace JS {

// This iterator can be used to walk the stack of a thread suspended at an
// arbitrary pc. To provide acurate results, profiling must have been enabled
// (via EnableRuntimeProfilingStack) before executing the callstack being
// unwound.
class JS_PUBLIC_API(ProfilingFrameIterator)
{
    JSRuntime* rt_;
    js::Activation* activation_;

    // When moving past a JitActivation, we need to save the prevJitTop
    // from it to use as the exit-frame pointer when the next caller jit
    // activation (if any) comes around.
    void* savedPrevJitTop_;

    static const unsigned StorageSpace = 6 * sizeof(void*);
    mozilla::AlignedStorage<StorageSpace> storage_;
    js::AsmJSProfilingFrameIterator& asmJSIter() {
        MOZ_ASSERT(!done());
        MOZ_ASSERT(isAsmJS());
        return *reinterpret_cast<js::AsmJSProfilingFrameIterator*>(storage_.addr());
    }
    const js::AsmJSProfilingFrameIterator& asmJSIter() const {
        MOZ_ASSERT(!done());
        MOZ_ASSERT(isAsmJS());
        return *reinterpret_cast<const js::AsmJSProfilingFrameIterator*>(storage_.addr());
    }

    js::jit::JitProfilingFrameIterator& jitIter() {
        MOZ_ASSERT(!done());
        MOZ_ASSERT(isJit());
        return *reinterpret_cast<js::jit::JitProfilingFrameIterator*>(storage_.addr());
    }

    const js::jit::JitProfilingFrameIterator& jitIter() const {
        MOZ_ASSERT(!done());
        MOZ_ASSERT(isJit());
        return *reinterpret_cast<const js::jit::JitProfilingFrameIterator*>(storage_.addr());
    }

    void settle();

  public:
    struct RegisterState
    {
        RegisterState() : pc(nullptr), sp(nullptr), lr(nullptr) {}
        void* pc;
        void* sp;
        void* lr;
    };

    ProfilingFrameIterator(JSRuntime* rt, const RegisterState& state);
    ~ProfilingFrameIterator();
    void operator++();
    bool done() const { return !activation_; }

    // Assuming the stack grows down (we do), the return value:
    //  - always points into the stack
    //  - is weakly monotonically increasing (may be equal for successive frames)
    //  - will compare greater than newer native and psuedo-stack frame addresses
    //    and less than older native and psuedo-stack frame addresses
    void* stackAddress() const;

    enum FrameKind
    {
      Frame_Baseline,
      Frame_Ion,
      Frame_AsmJS
    };

    struct Frame
    {
        FrameKind kind;
        void* stackAddress;
        void* returnAddress;
        void* activation;
        const char* label;
        bool hasTrackedOptimizations;
    };
    uint32_t extractStack(Frame* frames, uint32_t offset, uint32_t end) const;

  private:
    void iteratorConstruct(const RegisterState& state);
    void iteratorConstruct();
    void iteratorDestroy();
    bool iteratorDone();

    bool isAsmJS() const;
    bool isJit() const;
};

JS_FRIEND_API(bool)
IsProfilingEnabledForRuntime(JSRuntime* runtime);

} // namespace JS

#endif  /* js_ProfilingFrameIterator_h */
