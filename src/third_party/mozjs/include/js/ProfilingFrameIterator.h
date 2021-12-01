/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_ProfilingFrameIterator_h
#define js_ProfilingFrameIterator_h

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include "js/GCAPI.h"
#include "js/TypeDecls.h"
#include "js/Utility.h"

namespace js {
    class Activation;
    namespace jit {
        class JitActivation;
        class JSJitProfilingFrameIterator;
        class JitcodeGlobalEntry;
    } // namespace jit
    namespace wasm {
        class ProfilingFrameIterator;
    } // namespace wasm
} // namespace js

namespace JS {

struct ForEachTrackedOptimizationAttemptOp;
struct ForEachTrackedOptimizationTypeInfoOp;

// This iterator can be used to walk the stack of a thread suspended at an
// arbitrary pc. To provide accurate results, profiling must have been enabled
// (via EnableRuntimeProfilingStack) before executing the callstack being
// unwound.
//
// Note that the caller must not do anything that could cause GC to happen while
// the iterator is alive, since this could invalidate Ion code and cause its
// contents to become out of date.
class MOZ_NON_PARAM JS_PUBLIC_API(ProfilingFrameIterator)
{
  public:
    enum class Kind : bool {
        JSJit,
        Wasm
    };

  private:
    JSContext* cx_;
    mozilla::Maybe<uint64_t> samplePositionInProfilerBuffer_;
    js::Activation* activation_;
    Kind kind_;

    static const unsigned StorageSpace = 8 * sizeof(void*);
    alignas(void*) unsigned char storage_[StorageSpace];

    void* storage() { return storage_; }
    const void* storage() const { return storage_; }

    js::wasm::ProfilingFrameIterator& wasmIter() {
        MOZ_ASSERT(!done());
        MOZ_ASSERT(isWasm());
        return *static_cast<js::wasm::ProfilingFrameIterator*>(storage());
    }
    const js::wasm::ProfilingFrameIterator& wasmIter() const {
        MOZ_ASSERT(!done());
        MOZ_ASSERT(isWasm());
        return *static_cast<const js::wasm::ProfilingFrameIterator*>(storage());
    }

    js::jit::JSJitProfilingFrameIterator& jsJitIter() {
        MOZ_ASSERT(!done());
        MOZ_ASSERT(isJSJit());
        return *static_cast<js::jit::JSJitProfilingFrameIterator*>(storage());
    }

    const js::jit::JSJitProfilingFrameIterator& jsJitIter() const {
        MOZ_ASSERT(!done());
        MOZ_ASSERT(isJSJit());
        return *static_cast<const js::jit::JSJitProfilingFrameIterator*>(storage());
    }

    void settleFrames();
    void settle();

  public:
    struct RegisterState
    {
        RegisterState() : pc(nullptr), sp(nullptr), fp(nullptr), lr(nullptr) {}
        void* pc;
        void* sp;
        void* fp;
        void* lr;
    };

    ProfilingFrameIterator(JSContext* cx, const RegisterState& state,
                           const mozilla::Maybe<uint64_t>& samplePositionInProfilerBuffer = mozilla::Nothing());
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
      Frame_Wasm
    };

    struct Frame
    {
        FrameKind kind;
        void* stackAddress;
        void* returnAddress;
        void* activation;
        const char* label;
    } JS_HAZ_GC_INVALIDATED;

    bool isWasm() const;
    bool isJSJit() const;

    uint32_t extractStack(Frame* frames, uint32_t offset, uint32_t end) const;

    mozilla::Maybe<Frame> getPhysicalFrameWithoutLabel() const;

  private:
    mozilla::Maybe<Frame> getPhysicalFrameAndEntry(js::jit::JitcodeGlobalEntry* entry) const;

    void iteratorConstruct(const RegisterState& state);
    void iteratorConstruct();
    void iteratorDestroy();
    bool iteratorDone();
} JS_HAZ_GC_INVALIDATED;

JS_FRIEND_API(bool)
IsProfilingEnabledForContext(JSContext* cx);

/**
 * After each sample run, this method should be called with the current buffer
 * position at which the buffer contents start. This will update the
 * corresponding field on the JSRuntime.
 *
 * See the field |profilerSampleBufferRangeStart| on JSRuntime for documentation
 * about what this value is used for.
 */
JS_FRIEND_API(void)
SetJSContextProfilerSampleBufferRangeStart(JSContext* cx, uint64_t rangeStart);

class ProfiledFrameRange;

// A handle to the underlying JitcodeGlobalEntry, so as to avoid repeated
// lookups on JitcodeGlobalTable.
class MOZ_STACK_CLASS ProfiledFrameHandle
{
    friend class ProfiledFrameRange;

    JSRuntime* rt_;
    js::jit::JitcodeGlobalEntry& entry_;
    void* addr_;
    void* canonicalAddr_;
    const char* label_;
    uint32_t depth_;
    mozilla::Maybe<uint8_t> optsIndex_;

    ProfiledFrameHandle(JSRuntime* rt, js::jit::JitcodeGlobalEntry& entry,
                        void* addr, const char* label, uint32_t depth);

    void updateHasTrackedOptimizations();

public:
    const char* label() const { return label_; }
    uint32_t depth() const { return depth_; }
    bool hasTrackedOptimizations() const { return optsIndex_.isSome(); }
    void* canonicalAddress() const { return canonicalAddr_; }

    JS_PUBLIC_API(ProfilingFrameIterator::FrameKind) frameKind() const;
    JS_PUBLIC_API(void) forEachOptimizationAttempt(ForEachTrackedOptimizationAttemptOp& op,
                                                    JSScript** scriptOut,
                                                    jsbytecode** pcOut) const;

    JS_PUBLIC_API(void)
    forEachOptimizationTypeInfo(ForEachTrackedOptimizationTypeInfoOp& op) const;
};

class ProfiledFrameRange
{
public:
    class Iter final
    {
    public:
        Iter(const ProfiledFrameRange& range, uint32_t index)
          : range_(range)
          , index_(index)
        {}

        JS_PUBLIC_API(ProfiledFrameHandle) operator*() const;

        // Provide the bare minimum of iterator methods that are needed for
        // C++ ranged for loops.
        Iter& operator++() { ++index_; return *this; }
        bool operator==(const Iter& rhs) { return index_ == rhs.index_; }
        bool operator!=(const Iter& rhs) { return !(*this == rhs); }

    private:
        const ProfiledFrameRange& range_;
        uint32_t index_;
    };

    Iter begin() const { return Iter(*this, 0); }
    Iter end() const { return Iter(*this, depth_); }

private:
    friend JS_PUBLIC_API(ProfiledFrameRange) GetProfiledFrames(JSContext* cx,
                                                               void* addr);

    ProfiledFrameRange(JSRuntime* rt, void* addr, js::jit::JitcodeGlobalEntry* entry)
      : rt_(rt)
      , addr_(addr)
      , entry_(entry)
      , depth_(0)
    {}

    JSRuntime* rt_;
    void* addr_;
    js::jit::JitcodeGlobalEntry* entry_;
    // Assume maximum inlining depth is <64
    const char* labels_[64];
    uint32_t depth_;
};

// Returns a range that can be iterated over using C++ ranged for loops.
JS_PUBLIC_API(ProfiledFrameRange)
GetProfiledFrames(JSContext* cx, void* addr);

} // namespace JS

#endif  /* js_ProfilingFrameIterator_h */
