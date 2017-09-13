/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_ProfilingFrameIterator_h
#define js_ProfilingFrameIterator_h

#include "mozilla/Alignment.h"
#include "mozilla/Maybe.h"

#include "jsbytecode.h"
#include "js/TypeDecls.h"
#include "js/Utility.h"

struct JSRuntime;
class JSScript;

namespace js {
    class Activation;
    class AsmJSProfilingFrameIterator;
    namespace jit {
        class JitActivation;
        class JitProfilingFrameIterator;
        class JitcodeGlobalEntry;
    } // namespace jit
} // namespace js

namespace JS {

struct ForEachTrackedOptimizationAttemptOp;
struct ForEachTrackedOptimizationTypeInfoOp;

// This iterator can be used to walk the stack of a thread suspended at an
// arbitrary pc. To provide acurate results, profiling must have been enabled
// (via EnableRuntimeProfilingStack) before executing the callstack being
// unwound.
class JS_PUBLIC_API(ProfilingFrameIterator)
{
    JSRuntime* rt_;
    uint32_t sampleBufferGen_;
    js::Activation* activation_;

    // When moving past a JitActivation, we need to save the prevJitTop
    // from it to use as the exit-frame pointer when the next caller jit
    // activation (if any) comes around.
    void* savedPrevJitTop_;

    static const unsigned StorageSpace = 8 * sizeof(void*);
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

    bool hasSampleBufferGen() const {
        return sampleBufferGen_ != UINT32_MAX;
    }

  public:
    struct RegisterState
    {
        RegisterState() : pc(nullptr), sp(nullptr), lr(nullptr) {}
        void* pc;
        void* sp;
        void* lr;
    };

    ProfilingFrameIterator(JSRuntime* rt, const RegisterState& state,
                           uint32_t sampleBufferGen = UINT32_MAX);
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
    };

    bool isAsmJS() const;
    bool isJit() const;

    uint32_t extractStack(Frame* frames, uint32_t offset, uint32_t end) const;

    mozilla::Maybe<Frame> getPhysicalFrameWithoutLabel() const;

  private:
    mozilla::Maybe<Frame> getPhysicalFrameAndEntry(js::jit::JitcodeGlobalEntry* entry) const;

    void iteratorConstruct(const RegisterState& state);
    void iteratorConstruct();
    void iteratorDestroy();
    bool iteratorDone();
};

JS_FRIEND_API(bool)
IsProfilingEnabledForRuntime(JSRuntime* runtime);

/**
 * After each sample run, this method should be called with the latest sample
 * buffer generation, and the lapCount.  It will update corresponding fields on
 * JSRuntime.
 *
 * See fields |profilerSampleBufferGen|, |profilerSampleBufferLapCount| on
 * JSRuntime for documentation about what these values are used for.
 */
JS_FRIEND_API(void)
UpdateJSRuntimeProfilerSampleBufferGen(JSRuntime* runtime, uint32_t generation,
                                       uint32_t lapCount);

struct ForEachProfiledFrameOp
{
    // A handle to the underlying JitcodeGlobalEntry, so as to avoid repeated
    // lookups on JitcodeGlobalTable.
    class MOZ_STACK_CLASS FrameHandle
    {
        friend JS_PUBLIC_API(void) ForEachProfiledFrame(JSRuntime* rt, void* addr,
                                                        ForEachProfiledFrameOp& op);

        JSRuntime* rt_;
        js::jit::JitcodeGlobalEntry& entry_;
        void* addr_;
        void* canonicalAddr_;
        const char* label_;
        uint32_t depth_;
        mozilla::Maybe<uint8_t> optsIndex_;

        FrameHandle(JSRuntime* rt, js::jit::JitcodeGlobalEntry& entry, void* addr,
                    const char* label, uint32_t depth);

        void updateHasTrackedOptimizations();

      public:
        const char* label() const { return label_; }
        uint32_t depth() const { return depth_; }
        bool hasTrackedOptimizations() const { return optsIndex_.isSome(); }
        void* canonicalAddress() const { return canonicalAddr_; }

        ProfilingFrameIterator::FrameKind frameKind() const;
        void forEachOptimizationAttempt(ForEachTrackedOptimizationAttemptOp& op,
                                        JSScript** scriptOut, jsbytecode** pcOut) const;
        void forEachOptimizationTypeInfo(ForEachTrackedOptimizationTypeInfoOp& op) const;
    };

    // Called once per frame.
    virtual void operator()(const FrameHandle& frame) = 0;
};

JS_PUBLIC_API(void)
ForEachProfiledFrame(JSRuntime* rt, void* addr, ForEachProfiledFrameOp& op);

} // namespace JS

#endif  /* js_ProfilingFrameIterator_h */
