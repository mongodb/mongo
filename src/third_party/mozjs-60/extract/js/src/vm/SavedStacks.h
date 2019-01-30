/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SavedStacks_h
#define vm_SavedStacks_h

#include "mozilla/Attributes.h"
#include "mozilla/FastBernoulliTrial.h"

#include "jsmath.h"

#include "js/HashTable.h"
#include "js/Wrapper.h"
#include "vm/JSContext.h"
#include "vm/SavedFrame.h"
#include "vm/Stack.h"

namespace js {

// # Saved Stacks
//
// The `SavedStacks` class provides a compact way to capture and save JS stacks
// as `SavedFrame` `JSObject` subclasses. A single `SavedFrame` object
// represents one frame that was on the stack, and has a strong reference to its
// parent `SavedFrame` (the next youngest frame). This reference is null when
// the `SavedFrame` object is the oldest frame that was on the stack.
//
// This comment documents implementation. For usage documentation, see the
// `js/src/doc/SavedFrame/SavedFrame.md` file and relevant `SavedFrame`
// functions in `js/src/jsapi.h`.
//
// ## Compact
//
// Older saved stack frame tails are shared via hash consing, to deduplicate
// structurally identical data. `SavedStacks` contains a hash table of weakly
// held `SavedFrame` objects, and when the owning compartment is swept, it
// removes entries from this table that aren't held alive in any other way. When
// saving new stacks, we use this table to find pre-existing `SavedFrame`
// objects. If such an object is already extant, it is reused; otherwise a new
// `SavedFrame` is allocated and inserted into the table.
//
//    Naive         |   Hash Consing
//    --------------+------------------
//    c -> b -> a   |   c -> b -> a
//                  |        ^
//    d -> b -> a   |   d ---|
//                  |        |
//    e -> b -> a   |   e ---'
//
// This technique is effective because of the nature of the events that trigger
// capturing the stack. Currently, these events consist primarily of `JSObject`
// allocation (when an observing `Debugger` has such tracking), `Promise`
// settlement, and `Error` object creation. While these events may occur many
// times, they tend to occur only at a few locations in the JS source. For
// example, if we enable Object allocation tracking and run the esprima
// JavaScript parser on its own JavaScript source, there are approximately 54700
// total `Object` allocations, but just ~1400 unique JS stacks at allocation
// time. There's only ~200 allocation sites if we capture only the youngest
// stack frame.
//
// ## Security and Wrappers
//
// We save every frame on the stack, regardless of whether the `SavedStack`'s
// compartment's principals subsume the frame's compartment's principals or
// not. This gives us maximum flexibility down the line when accessing and
// presenting captured stacks, but at the price of some complication involved in
// preventing the leakage of privileged stack frames to unprivileged callers.
//
// When a `SavedFrame` method or accessor is called, we compare the caller's
// compartment's principals to each `SavedFrame`'s captured principals. We avoid
// using the usual `CallNonGenericMethod` and `nativeCall` machinery which
// enters the `SavedFrame` object's compartment before we can check these
// principals, because we need access to the original caller's compartment's
// principals (unlike other `CallNonGenericMethod` users) to determine what view
// of the stack to present. Instead, we take a similar approach to that used by
// DOM methods, and manually unwrap wrappers until we get the underlying
// `SavedFrame` object, find the first `SavedFrame` in its stack whose captured
// principals are subsumed by the caller's principals, access the reserved slots
// we care about, and then rewrap return values as necessary.
//
// Consider the following diagram:
//
//                                              Content Compartment
//                                    +---------------------------------------+
//                                    |                                       |
//                                    |           +------------------------+  |
//      Chrome Compartment            |           |                        |  |
//    +--------------------+          |           | SavedFrame C (content) |  |
//    |                    |          |           |                        |  |
//    |                  +--------------+         +------------------------+  |
//    |                  |              |                    ^                |
//    |     var x -----> | Xray Wrapper |-----.              |                |
//    |                  |              |     |              |                |
//    |                  +--------------+     |   +------------------------+  |
//    |                    |          |       |   |                        |  |
//    |                  +--------------+     |   | SavedFrame B (content) |  |
//    |                  |              |     |   |                        |  |
//    |     var y -----> | CCW (waived) |--.  |   +------------------------+  |
//    |                  |              |  |  |              ^                |
//    |                  +--------------+  |  |              |                |
//    |                    |          |    |  |              |                |
//    +--------------------+          |    |  |   +------------------------+  |
//                                    |    |  '-> |                        |  |
//                                    |    |      | SavedFrame A (chrome)  |  |
//                                    |    '----> |                        |  |
//                                    |           +------------------------+  |
//                                    |                      ^                |
//                                    |                      |                |
//                                    |           var z -----'                |
//                                    |                                       |
//                                    +---------------------------------------+
//
// CCW is a plain cross-compartment wrapper, yielded by waiving Xray vision. A
// is the youngest `SavedFrame` and represents a frame that was from the chrome
// compartment, while B and C are from frames from the content compartment. C is
// the oldest frame.
//
// Note that it is always safe to waive an Xray around a SavedFrame object,
// because SavedFrame objects and the SavedFrame prototype are always frozen you
// will never run untrusted code.
//
// Depending on who the caller is, the view of the stack will be different, and
// is summarized in the table below.
//
//    Var  | View
//    -----+------------
//    x    | A -> B -> C
//    y, z | B -> C
//
// In the case of x, the `SavedFrame` accessors are called with an Xray wrapper
// around the `SavedFrame` object as the `this` value, and the chrome
// compartment as the cx's current principals. Because the chrome compartment's
// principals subsume both itself and the content compartment's principals, x
// has the complete view of the stack.
//
// In the case of y, the cross-compartment machinery automatically enters the
// content compartment, and calls the `SavedFrame` accessors with the wrapped
// `SavedFrame` object as the `this` value. Because the cx's current compartment
// is the content compartment, and the content compartment's principals do not
// subsume the chrome compartment's principals, it can only see the B and C
// frames.
//
// In the case of z, the `SavedFrame` accessors are called with the `SavedFrame`
// object in the `this` value, and the content compartment as the cx's current
// compartment. Similar to the case of y, only the B and C frames are exposed
// because the cx's current compartment's principals do not subsume A's captured
// principals.

class SavedStacks {
    friend class SavedFrame;
    friend bool JS::ubi::ConstructSavedFrameStackSlow(JSContext* cx,
                                                      JS::ubi::StackFrame& ubiFrame,
                                                      MutableHandleObject outSavedFrameStack);

  public:
    SavedStacks()
      : frames(),
        bernoulliSeeded(false),
        bernoulli(1.0, 0x59fdad7f6b4cc573, 0x91adf38db96a9354),
        creatingSavedFrame(false)
    { }

    MOZ_MUST_USE bool init();
    bool initialized() const { return frames.initialized(); }
    MOZ_MUST_USE bool saveCurrentStack(JSContext* cx, MutableHandleSavedFrame frame,
                                       JS::StackCapture&& capture = JS::StackCapture(JS::AllFrames()));
    MOZ_MUST_USE bool copyAsyncStack(JSContext* cx, HandleObject asyncStack,
                                     HandleString asyncCause,
                                     MutableHandleSavedFrame adoptedStack,
                                     const Maybe<size_t>& maxFrameCount);
    void sweep();
    void trace(JSTracer* trc);
    uint32_t count();
    void clear();
    void chooseSamplingProbability(JSCompartment*);

    // Set the sampling random number generator's state to |state0| and
    // |state1|. One or the other must be non-zero. See the comments for
    // mozilla::non_crypto::XorShift128PlusRNG::setState for details.
    void setRNGState(uint64_t state0, uint64_t state1) { bernoulli.setRandomState(state0, state1); }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf);

    // An alloction metadata builder that marks cells with the JavaScript stack
    // at which they were allocated.
    struct MetadataBuilder : public AllocationMetadataBuilder {
        MetadataBuilder() : AllocationMetadataBuilder() { }
        virtual JSObject* build(JSContext *cx, HandleObject obj,
                                AutoEnterOOMUnsafeRegion& oomUnsafe) const override;
    };

    static const MetadataBuilder metadataBuilder;

  private:
    SavedFrame::Set frames;
    bool bernoulliSeeded;
    mozilla::FastBernoulliTrial bernoulli;
    bool creatingSavedFrame;

    // Similar to mozilla::ReentrancyGuard, but instead of asserting against
    // reentrancy, just change the behavior of SavedStacks::saveCurrentStack to
    // return a nullptr SavedFrame.
    struct MOZ_RAII AutoReentrancyGuard {
        MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER;
        SavedStacks& stacks;

        explicit AutoReentrancyGuard(SavedStacks& stacks MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
            : stacks(stacks)
        {
            MOZ_GUARD_OBJECT_NOTIFIER_INIT;
            stacks.creatingSavedFrame = true;
        }

        ~AutoReentrancyGuard()
        {
            stacks.creatingSavedFrame = false;
        }
    };

    MOZ_MUST_USE bool insertFrames(JSContext* cx, MutableHandleSavedFrame frame,
                                   JS::StackCapture&& capture);
    MOZ_MUST_USE bool adoptAsyncStack(JSContext* cx, MutableHandleSavedFrame asyncStack,
                                      HandleAtom asyncCause,
                                      const Maybe<size_t>& maxFrameCount);
    MOZ_MUST_USE bool checkForEvalInFramePrev(JSContext* cx, SavedFrame::HandleLookup lookup);
    SavedFrame* getOrCreateSavedFrame(JSContext* cx, SavedFrame::HandleLookup lookup);
    SavedFrame* createFrameFromLookup(JSContext* cx, SavedFrame::HandleLookup lookup);

    // Cache for memoizing PCToLineNumber lookups.

    struct PCKey {
        PCKey(JSScript* script, jsbytecode* pc) : script(script), pc(pc) { }

        HeapPtr<JSScript*> script;
        jsbytecode* pc;

        void trace(JSTracer* trc) { /* PCKey is weak. */ }
        bool needsSweep() { return IsAboutToBeFinalized(&script); }
    };

  public:
    struct LocationValue {
        LocationValue() : source(nullptr), line(0), column(0) { }
        LocationValue(JSAtom* source, size_t line, uint32_t column)
            : source(source), line(line), column(column)
        { }

        void trace(JSTracer* trc) {
            TraceNullableEdge(trc, &source, "SavedStacks::LocationValue::source");
        }

        bool needsSweep() {
            // LocationValue is always held strongly, but in a weak map.
            // Assert that it has been marked already, but allow it to be
            // ejected from the map when the key dies.
            MOZ_ASSERT(source);
            MOZ_ASSERT(!IsAboutToBeFinalized(&source));
            return true;
        }

        HeapPtr<JSAtom*> source;
        size_t line;
        uint32_t column;
    };

  private:
    struct PCLocationHasher : public DefaultHasher<PCKey> {
        using ScriptPtrHasher = DefaultHasher<JSScript*>;
        using BytecodePtrHasher = DefaultHasher<jsbytecode*>;

        static HashNumber hash(const PCKey& key) {
            return mozilla::AddToHash(ScriptPtrHasher::hash(key.script),
                                      BytecodePtrHasher::hash(key.pc));
        }

        static bool match(const PCKey& l, const PCKey& k) {
            return ScriptPtrHasher::match(l.script, k.script) &&
                   BytecodePtrHasher::match(l.pc, k.pc);
        }
    };

    // We eagerly Atomize the script source stored in LocationValue because
    // wasm does not always have a JSScript and the source might not be
    // available when we need it later. However, since the JSScript does not
    // actually hold this atom, we have to trace it strongly to keep it alive.
    // Thus, it takes two GC passes to fully clean up this table: the first GC
    // removes the dead script; the second will clear out the source atom since
    // it is no longer held by the table.
    using PCLocationMap = GCHashMap<PCKey, LocationValue, PCLocationHasher, SystemAllocPolicy>;
    PCLocationMap pcLocationMap;

    MOZ_MUST_USE bool getLocation(JSContext* cx, const FrameIter& iter,
                                  MutableHandle<LocationValue> locationp);
};

template <typename Wrapper>
struct WrappedPtrOperations<SavedStacks::LocationValue, Wrapper>
{
    JSAtom* source() const { return loc().source; }
    size_t line() const { return loc().line; }
    uint32_t column() const { return loc().column; }

  private:
    const SavedStacks::LocationValue& loc() const {
        return static_cast<const Wrapper*>(this)->get();
    }
};

template <typename Wrapper>
struct MutableWrappedPtrOperations<SavedStacks::LocationValue, Wrapper>
    : public WrappedPtrOperations<SavedStacks::LocationValue, Wrapper>
{
    void setSource(JSAtom* v) { loc().source = v; }
    void setLine(size_t v) { loc().line = v; }
    void setColumn(uint32_t v) { loc().column = v; }

  private:
    SavedStacks::LocationValue& loc() {
        return static_cast<Wrapper*>(this)->get();
    }
};

UTF8CharsZ
BuildUTF8StackString(JSContext* cx, HandleObject stack);

} /* namespace js */

#endif /* vm_SavedStacks_h */
