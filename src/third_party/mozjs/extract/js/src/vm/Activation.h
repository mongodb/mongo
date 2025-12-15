/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Activation_h
#define vm_Activation_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_RAII

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t, uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "jit/CalleeToken.h"  // js::jit::CalleeToken
#include "js/RootingAPI.h"    // JS::Handle, JS::Rooted
#include "js/TypeDecls.h"     // jsbytecode
#include "js/Value.h"         // JS::Value
#include "vm/SavedFrame.h"    // js::SavedFrame
#include "vm/Stack.h"         // js::InterpreterRegs

struct JS_PUBLIC_API JSContext;

class JSFunction;
class JSObject;
class JSScript;

namespace JS {

class CallArgs;
class JS_PUBLIC_API Compartment;

}  // namespace JS

namespace js {

class InterpreterActivation;

namespace jit {
class JitActivation;
}  // namespace jit

// [SMDOC] LiveSavedFrameCache: SavedFrame caching to minimize stack walking
//
// Since each SavedFrame object includes a 'parent' pointer to the SavedFrame
// for its caller, if we could easily find the right SavedFrame for a given
// stack frame, we wouldn't need to walk the rest of the stack. Traversing deep
// stacks can be expensive, and when we're profiling or instrumenting code, we
// may want to capture JavaScript stacks frequently, so such cases would benefit
// if we could avoid walking the entire stack.
//
// We could have a cache mapping frame addresses to their SavedFrame objects,
// but invalidating its entries would be a challenge. Popping a stack frame is
// extremely performance-sensitive, and SpiderMonkey stack frames can be OSR'd,
// thrown, rematerialized, and perhaps meet other fates; we would rather our
// cache not depend on handling so many tricky cases.
//
// It turns out that we can keep the cache accurate by reserving a single bit in
// the stack frame, which must be clear on any newly pushed frame. When we
// insert an entry into the cache mapping a given frame address to its
// SavedFrame, we set the bit in the frame. Then, we take care to probe the
// cache only for frames whose bit is set; the bit tells us that the frame has
// never left the stack, so its cache entry must be accurate, at least about
// which function the frame is executing (the line may have changed; more about
// that below). The code refers to this bit as the 'hasCachedSavedFrame' flag.
//
// We could manage such a cache replacing least-recently used entries, but we
// can do better than that: the cache can be a stack, of which we need examine
// only entries from the top.
//
// First, observe that stacks are walked from the youngest frame to the oldest,
// but SavedFrame chains are built from oldest to youngest, to ensure common
// tails are shared. This means that capturing a stack is necessarily a
// two-phase process: walk the stack, and then build the SavedFrames.
//
// Naturally, the first time we capture the stack, the cache is empty, and we
// must traverse the entire stack. As we build each SavedFrame, we push an entry
// associating the frame's address to its SavedFrame on the cache, and set the
// frame's bit. At the end, every frame has its bit set and an entry in the
// cache.
//
// Then the program runs some more. Some, none, or all of the frames are popped.
// Any new frames are pushed with their bit clear. Any frame with its bit set
// has never left the stack. The cache is left untouched.
//
// For the next capture, we walk the stack up to the first frame with its bit
// set, if there is one. Call it F; it must have a cache entry. We pop entries
// from the cache - all invalid, because they are above F's entry, and hence
// younger - until we find the entry matching F's address. Since F's bit is set,
// we know it never left the stack, and hence that no younger frame could have
// had a colliding address. And since the frame's bit was set when we pushed the
// cache entry, we know the entry is still valid.
//
// F's cache entry's SavedFrame covers the rest of the stack, so we don't need
// to walk the stack any further. Now we begin building SavedFrame objects for
// the new frames, pushing cache entries, and setting bits on the frames. By the
// end, the cache again covers the full stack, and every frame's bit is set.
//
// If we walk the stack to the end, and find no frame with its bit set, then the
// entire cache is invalid. At this point, it must be emptied, so that the new
// entries we are about to push are the only frames in the cache.
//
// For example, suppose we have the following stack (let 'A > B' mean "A called
// B", so the frames are listed oldest first):
//
//     P  > Q  > R  > S          Initial stack, bits not set.
//     P* > Q* > R* > S*         Capture a SavedFrame stack, set bits.
//                               The cache now holds: P > Q > R > S.
//     P* > Q* > R*              Return from S.
//     P* > Q*                   Return from R.
//     P* > Q* > T  > U          Call T and U. New frames have clear bits.
//
// If we capture the stack now, the cache still holds:
//
//     P  > Q  > R  > S
//
// As we traverse the stack, we'll cross U and T, and then find Q with its bit
// set. We pop entries from the cache until we find the entry for Q; this
// removes entries R and S, which were indeed invalid. In Q's cache entry, we
// find the SavedFrame representing the stack P > Q. Now we build SavedFrames
// for the new portion of the stack, pushing an entry for T and setting the bit
// on the frame, and then doing the same for U. In the end, the call stack again
// has bits set on all its frames:
//
//     P* > Q* > T* > U*         All frames are now in the cache.
//
// And the cache again holds entries for the entire stack:
//
//     P  > Q  > T  > U
//
// Details:
//
// - When we find a cache entry whose frame address matches our frame F, we know
//   that F has never left the stack, but it may certainly be the case that
//   execution took place in that frame, and that the current source position
//   within F's function has changed. This means that the entry's SavedFrame,
//   which records the source line and column as well as the function, is not
//   correct. To detect this case, when we push a cache entry, we record the
//   frame's pc. When consulting the cache, if a frame's address matches but its
//   pc does not, then we pop the cache entry, clear the frame's bit, and
//   continue walking the stack. The next stack frame will definitely hit: since
//   its callee frame never left the stack, the calling frame never got the
//   chance to execute.
//
// - Generators, at least conceptually, have long-lived stack frames that
//   disappear from the stack when the generator yields, and reappear on the
//   stack when the generator's 'next' method is called. When a generator's
//   frame is placed again atop the stack, its bit must be cleared - for the
//   purposes of the cache, treating the frame as a new frame - to respect the
//   invariants we used to justify the algorithm above. Async function
//   activations usually appear atop empty stacks, since they are invoked as a
//   promise callback, but the same rule applies.
//
// - SpiderMonkey has many types of stack frames, and not all have a place to
//   store a bit indicating a cached SavedFrame. But as long as we don't create
//   cache entries for frames we can't mark, simply omitting them from the cache
//   is harmless. Uncacheable frame types include inlined Ion frames and
//   non-Debug wasm frames. The LiveSavedFrameCache::FramePtr type represents
//   only pointers to frames that can be cached, so if you have a FramePtr, you
//   don't need to further check the frame for cachability. FramePtr provides
//   access to the hasCachedSavedFrame bit.
//
// - We actually break up the cache into one cache per Activation. Popping an
//   activation invalidates all its cache entries, simply by freeing the cache
//   altogether.
//
// - The entire chain of SavedFrames for a given stack capture is created in the
//   compartment of the code that requested the capture, *not* in that of the
//   frames it represents, so in general, different compartments may have
//   different SavedFrame objects representing the same actual stack frame. The
//   LiveSavedFrameCache simply records whichever SavedFrames were used in the
//   most recent captures. When we find a cache hit, we check the entry's
//   SavedFrame's compartment against the current compartment; if they do not
//   match, we clear the entire cache.
//
//   This means that it is not always true that, if a frame's
//   hasCachedSavedFrame bit is set, it must have an entry in the cache. The
//   actual invariant is: either the cache is completely empty, or the frames'
//   bits are trustworthy. This invariant holds even though capture can be
//   interrupted at many places by OOM failures. Clearing the cache is a single,
//   uninterruptible step. When we try to look up a frame whose bit is set and
//   find an empty cache, we clear the frame's bit. And we only add the first
//   frame to an empty cache once we've walked the stack all the way, so we know
//   that all frames' bits are cleared by that point.
//
// - When the Debugger API evaluates an expression in some frame (the 'target
//   frame'), it's SpiderMonkey's convention that the target frame be treated as
//   the parent of the eval frame. In reality, of course, the eval frame is
//   pushed on the top of the stack like any other frame, but stack captures
//   simply jump straight over the intervening frames, so that the '.parent'
//   property of a SavedFrame for the eval is the SavedFrame for the target.
//   This is arranged by giving the eval frame an 'evalInFramePrev` link
//   pointing to the target, which an ordinary FrameIter will notice and
//   respect.
//
//   If the LiveSavedFrameCache were presented with stack traversals that
//   skipped frames in this way, it would cause havoc. First, with no debugger
//   eval frames present, capture the stack, populating the cache. Then push a
//   debugger eval frame and capture again; the skipped frames to appear to be
//   absent from the stack. Now pop the debugger eval frame, and capture a third
//   time: the no-longer-skipped frames seem to reappear on the stack, with
//   their cached bits still set.
//
//   The LiveSavedFrameCache assumes that the stack it sees is used in a
//   stack-like fashion: if a frame has its bit set, it has never left the
//   stack. To support this assumption, when the cache is in use, we do not skip
//   the frames between a debugger eval frame an its target; we always traverse
//   the entire stack, invalidating and populating the cache in the usual way.
//   Instead, when we construct a SavedFrame for a debugger eval frame, we
//   select the appropriate parent at that point: rather than the next-older
//   frame, we find the SavedFrame for the eval's target frame. The skip appears
//   in the SavedFrame chains, even as the traversal covers all the frames.
//
// - Rematerialized frames (see ../jit/RematerializedFrame.h) are always created
//   with their hasCachedSavedFrame bits clear: although there may be extant
//   SavedFrames built from the original IonMonkey frame, the Rematerialized
//   frames will not have cache entries for them until they are traversed in a
//   capture themselves.
//
//   This means that, oddly, it is not always true that, once we reach a frame
//   with its hasCachedSavedFrame bit set, all its parents will have the bit set
//   as well. However, clear bits under younger set bits will only occur on
//   Rematerialized frames.
class LiveSavedFrameCache {
 public:
  // The address of a live frame for which we can cache SavedFrames: it has a
  // 'hasCachedSavedFrame' bit we can examine and set, and can be converted to
  // a Key to index the cache.
  class FramePtr {
    // We use jit::CommonFrameLayout for both Baseline frames and Ion
    // physical frames.
    using Ptr = mozilla::Variant<InterpreterFrame*, jit::CommonFrameLayout*,
                                 jit::RematerializedFrame*, wasm::DebugFrame*>;

    Ptr ptr;

    template <typename Frame>
    explicit FramePtr(Frame ptr) : ptr(ptr) {}

    struct HasCachedMatcher;
    struct SetHasCachedMatcher;
    struct ClearHasCachedMatcher;

   public:
    // If iter's frame is of a type that can be cached, construct a FramePtr
    // for its frame. Otherwise, return Nothing.
    static inline mozilla::Maybe<FramePtr> create(const FrameIter& iter);

    inline bool hasCachedSavedFrame() const;
    inline void setHasCachedSavedFrame();
    inline void clearHasCachedSavedFrame();

    // Return true if this FramePtr refers to an interpreter frame.
    inline bool isInterpreterFrame() const {
      return ptr.is<InterpreterFrame*>();
    }

    // If this FramePtr is an interpreter frame, return a pointer to it.
    inline InterpreterFrame& asInterpreterFrame() const {
      return *ptr.as<InterpreterFrame*>();
    }

    // Return true if this FramePtr refers to a rematerialized frame.
    inline bool isRematerializedFrame() const {
      return ptr.is<jit::RematerializedFrame*>();
    }

    bool operator==(const FramePtr& rhs) const { return rhs.ptr == this->ptr; }
    bool operator!=(const FramePtr& rhs) const { return !(rhs == *this); }
  };

 private:
  // A key in the cache: the address of a frame, live or dead, for which we
  // can cache SavedFrames. Since the pointer may not be live, the only
  // operation this type permits is comparison.
  class Key {
    FramePtr framePtr;

   public:
    MOZ_IMPLICIT Key(const FramePtr& framePtr) : framePtr(framePtr) {}

    bool operator==(const Key& rhs) const {
      return rhs.framePtr == this->framePtr;
    }
    bool operator!=(const Key& rhs) const { return !(rhs == *this); }
  };

  struct Entry {
    const Key key;
    const jsbytecode* pc;
    HeapPtr<SavedFrame*> savedFrame;

    Entry(const Key& key, const jsbytecode* pc, SavedFrame* savedFrame)
        : key(key), pc(pc), savedFrame(savedFrame) {}
  };

  using EntryVector = Vector<Entry, 0, SystemAllocPolicy>;
  EntryVector* frames;

  LiveSavedFrameCache(const LiveSavedFrameCache&) = delete;
  LiveSavedFrameCache& operator=(const LiveSavedFrameCache&) = delete;

 public:
  explicit LiveSavedFrameCache() : frames(nullptr) {}

  LiveSavedFrameCache(LiveSavedFrameCache&& rhs) : frames(rhs.frames) {
    MOZ_ASSERT(this != &rhs, "self-move disallowed");
    rhs.frames = nullptr;
  }

  ~LiveSavedFrameCache() {
    if (frames) {
      js_delete(frames);
      frames = nullptr;
    }
  }

  bool initialized() const { return !!frames; }
  bool init(JSContext* cx) {
    frames = js_new<EntryVector>();
    if (!frames) {
      JS_ReportOutOfMemory(cx);
      return false;
    }
    return true;
  }

  void trace(JSTracer* trc);

  // Set |frame| to the cached SavedFrame corresponding to |framePtr| at |pc|.
  // |framePtr|'s hasCachedSavedFrame bit must be set. Remove all cache
  // entries for frames younger than that one.
  //
  // This may set |frame| to nullptr if |pc| is different from the pc supplied
  // when the cache entry was inserted. In this case, the cached SavedFrame
  // (probably) has the wrong source position. Entries for younger frames are
  // still removed. The next frame, if any, will be a cache hit.
  //
  // This may also set |frame| to nullptr if the cache was populated with
  // SavedFrame objects for a different compartment than cx's current
  // compartment. In this case, the entire cache is flushed.
  void find(JSContext* cx, FramePtr& framePtr, const jsbytecode* pc,
            MutableHandle<SavedFrame*> frame) const;

  // Search the cache for a frame matching |framePtr|, without removing any
  // entries. Return the matching saved frame, or nullptr if none is found.
  // This is used for resolving |evalInFramePrev| links.
  void findWithoutInvalidation(const FramePtr& framePtr,
                               MutableHandle<SavedFrame*> frame) const;

  // Push a cache entry mapping |framePtr| and |pc| to |savedFrame| on the top
  // of the cache's stack. You must insert entries for frames from oldest to
  // youngest. They must all be younger than the frame that the |find| method
  // found a hit for; or you must have cleared the entire cache with the
  // |clear| method.
  bool insert(JSContext* cx, FramePtr&& framePtr, const jsbytecode* pc,
              Handle<SavedFrame*> savedFrame);

  // Remove all entries from the cache.
  void clear() {
    if (frames) frames->clear();
  }
};

static_assert(
    sizeof(LiveSavedFrameCache) == sizeof(uintptr_t),
    "Every js::Activation has a LiveSavedFrameCache, so we need to be pretty "
    "careful "
    "about avoiding bloat. If you're adding members to LiveSavedFrameCache, "
    "maybe you "
    "should consider figuring out a way to make js::Activation have a "
    "LiveSavedFrameCache* instead of a Rooted<LiveSavedFrameCache>.");

class Activation {
 protected:
  JSContext* cx_;
  JS::Compartment* compartment_;
  Activation* prev_;
  Activation* prevProfiling_;

  // Counter incremented by JS::HideScriptedCaller and decremented by
  // JS::UnhideScriptedCaller. If > 0 for the top activation,
  // DescribeScriptedCaller will return null instead of querying that
  // activation, which should prompt the caller to consult embedding-specific
  // data structures instead.
  size_t hideScriptedCallerCount_;

  // The cache of SavedFrame objects we have already captured when walking
  // this activation's stack.
  JS::Rooted<LiveSavedFrameCache> frameCache_;

  // Youngest saved frame of an async stack that will be iterated during stack
  // capture in place of the actual stack of previous activations. Note that
  // the stack of this activation is captured entirely before this is used.
  //
  // Usually this is nullptr, meaning that normal stack capture will occur.
  // When this is set, the stack of any previous activation is ignored.
  JS::Rooted<SavedFrame*> asyncStack_;

  // Value of asyncCause to be attached to asyncStack_.
  const char* asyncCause_;

  // True if the async call was explicitly requested, e.g. via
  // callFunctionWithAsyncStack.
  bool asyncCallIsExplicit_;

  enum Kind { Interpreter, Jit };
  Kind kind_;

  inline Activation(JSContext* cx, Kind kind);
  inline ~Activation();

 public:
  JSContext* cx() const { return cx_; }
  JS::Compartment* compartment() const { return compartment_; }
  Activation* prev() const { return prev_; }
  Activation* prevProfiling() const { return prevProfiling_; }
  inline Activation* mostRecentProfiling();

  bool isInterpreter() const { return kind_ == Interpreter; }
  bool isJit() const { return kind_ == Jit; }
  inline bool hasWasmExitFP() const;

  inline bool isProfiling() const;
  void registerProfiling();
  void unregisterProfiling();

  InterpreterActivation* asInterpreter() const {
    MOZ_ASSERT(isInterpreter());
    return (InterpreterActivation*)this;
  }
  jit::JitActivation* asJit() const {
    MOZ_ASSERT(isJit());
    return (jit::JitActivation*)this;
  }

  void hideScriptedCaller() { hideScriptedCallerCount_++; }
  void unhideScriptedCaller() {
    MOZ_ASSERT(hideScriptedCallerCount_ > 0);
    hideScriptedCallerCount_--;
  }
  bool scriptedCallerIsHidden() const { return hideScriptedCallerCount_ > 0; }

  SavedFrame* asyncStack() { return asyncStack_; }

  const char* asyncCause() const { return asyncCause_; }

  bool asyncCallIsExplicit() const { return asyncCallIsExplicit_; }

  inline LiveSavedFrameCache* getLiveSavedFrameCache(JSContext* cx);
  void clearLiveSavedFrameCache() { frameCache_.get().clear(); }

 private:
  Activation(const Activation& other) = delete;
  void operator=(const Activation& other) = delete;
};

// This variable holds a special opcode value which is greater than all normal
// opcodes, and is chosen such that the bitwise or of this value with any
// opcode is this value.
constexpr jsbytecode EnableInterruptsPseudoOpcode = -1;

static_assert(EnableInterruptsPseudoOpcode >= JSOP_LIMIT,
              "EnableInterruptsPseudoOpcode must be greater than any opcode");
static_assert(
    EnableInterruptsPseudoOpcode == jsbytecode(-1),
    "EnableInterruptsPseudoOpcode must be the maximum jsbytecode value");

class InterpreterFrameIterator;
class RunState;

class InterpreterActivation : public Activation {
  friend class js::InterpreterFrameIterator;

  InterpreterRegs regs_;
  InterpreterFrame* entryFrame_;
  size_t opMask_;  // For debugger interrupts, see js::Interpret.

#ifdef DEBUG
  size_t oldFrameCount_;
#endif

 public:
  inline InterpreterActivation(RunState& state, JSContext* cx,
                               InterpreterFrame* entryFrame);
  inline ~InterpreterActivation();

  inline bool pushInlineFrame(const JS::CallArgs& args,
                              JS::Handle<JSScript*> script,
                              MaybeConstruct constructing);
  inline void popInlineFrame(InterpreterFrame* frame);

  inline bool resumeGeneratorFrame(JS::Handle<JSFunction*> callee,
                                   JS::Handle<JSObject*> envChain);

  InterpreterFrame* current() const { return regs_.fp(); }
  InterpreterRegs& regs() { return regs_; }
  InterpreterFrame* entryFrame() const { return entryFrame_; }
  size_t opMask() const { return opMask_; }

  bool isProfiling() const { return false; }

  // If this js::Interpret frame is running |script|, enable interrupts.
  void enableInterruptsIfRunning(JSScript* script) {
    if (regs_.fp()->script() == script) {
      enableInterruptsUnconditionally();
    }
  }
  void enableInterruptsUnconditionally() {
    opMask_ = EnableInterruptsPseudoOpcode;
  }
  void clearInterruptsMask() { opMask_ = 0; }
};

// Iterates over a thread's activation list.
class ActivationIterator {
 protected:
  Activation* activation_;

 public:
  explicit ActivationIterator(JSContext* cx);

  ActivationIterator& operator++();

  Activation* operator->() const { return activation_; }
  Activation* activation() const { return activation_; }
  bool done() const { return activation_ == nullptr; }
};

}  // namespace js

#endif  // vm_Activation_h
