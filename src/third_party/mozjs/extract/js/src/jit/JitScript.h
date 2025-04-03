/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitScript_h
#define jit_JitScript_h

#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"

#include <stddef.h>
#include <stdint.h>

#include "jstypes.h"
#include "NamespaceImports.h"

#include "gc/Barrier.h"
#include "jit/BaselineIC.h"
#include "jit/ICStubSpace.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
#include "js/Vector.h"
#include "util/TrailingArray.h"
#include "vm/EnvironmentObject.h"

class JS_PUBLIC_API JSScript;
class JS_PUBLIC_API JSTracer;
struct JS_PUBLIC_API JSContext;

namespace JS {
class Zone;
}

namespace js {

class SystemAllocPolicy;

namespace gc {
class AllocSite;
}

namespace jit {

class BaselineScript;
class InliningRoot;
class IonScript;
class JitScript;
class JitZone;

// Magic BaselineScript value indicating Baseline compilation has been disabled.
static constexpr uintptr_t BaselineDisabledScript = 0x1;

static BaselineScript* const BaselineDisabledScriptPtr =
    reinterpret_cast<BaselineScript*>(BaselineDisabledScript);

// Magic IonScript values indicating Ion compilation has been disabled or the
// script is being Ion-compiled off-thread.
static constexpr uintptr_t IonDisabledScript = 0x1;
static constexpr uintptr_t IonCompilingScript = 0x2;

static IonScript* const IonDisabledScriptPtr =
    reinterpret_cast<IonScript*>(IonDisabledScript);
static IonScript* const IonCompilingScriptPtr =
    reinterpret_cast<IonScript*>(IonCompilingScript);

/* [SMDOC] ICScript Lifetimes
 *
 * An ICScript owns an array of ICEntries, each of which owns a linked
 * list of ICStubs.
 *
 * A JitScript contains an embedded ICScript. If it has done any trial
 * inlining, it also owns an InliningRoot. The InliningRoot owns all
 * of the ICScripts that have been created for inlining into the
 * corresponding JitScript. This ties the lifetime of the inlined
 * ICScripts to the lifetime of the JitScript itself.
 *
 * We store pointers to ICScripts in two other places: on the stack in
 * BaselineFrame, and in IC stubs for CallInlinedFunction.
 *
 * The ICScript pointer in a BaselineFrame either points to the
 * ICScript embedded in the JitScript for that frame, or to an inlined
 * ICScript owned by a caller. In each case, there must be a frame on
 * the stack corresponding to the JitScript that owns the current
 * ICScript, which will keep the ICScript alive.
 *
 * Each ICStub is owned by an ICScript and, indirectly, a
 * JitScript. An ICStub that uses CallInlinedFunction contains an
 * ICScript for use by the callee. The ICStub and the callee ICScript
 * are always owned by the same JitScript, so the callee ICScript will
 * not be freed while the ICStub is alive.
 *
 * The lifetime of an ICScript is independent of the lifetimes of the
 * BaselineScript and IonScript/WarpScript to which it
 * corresponds. They can be destroyed and recreated, and the ICScript
 * will remain valid.
 */

class alignas(uintptr_t) ICScript final : public TrailingArray {
 public:
  ICScript(uint32_t warmUpCount, Offset fallbackStubsOffset, Offset endOffset,
           uint32_t depth, InliningRoot* inliningRoot = nullptr)
      : inliningRoot_(inliningRoot),
        warmUpCount_(warmUpCount),
        fallbackStubsOffset_(fallbackStubsOffset),
        endOffset_(endOffset),
        depth_(depth) {}

  bool isInlined() const { return depth_ > 0; }

  void initICEntries(JSContext* cx, JSScript* script);

  ICEntry& icEntry(size_t index) {
    MOZ_ASSERT(index < numICEntries());
    return icEntries()[index];
  }

  ICFallbackStub* fallbackStub(size_t index) {
    MOZ_ASSERT(index < numICEntries());
    return fallbackStubs() + index;
  }

  ICEntry* icEntryForStub(const ICFallbackStub* stub) {
    size_t index = stub - fallbackStubs();
    MOZ_ASSERT(index < numICEntries());
    return &icEntry(index);
  }
  ICFallbackStub* fallbackStubForICEntry(const ICEntry* entry) {
    size_t index = entry - icEntries();
    MOZ_ASSERT(index < numICEntries());
    return fallbackStub(index);
  }

  InliningRoot* inliningRoot() const { return inliningRoot_; }
  uint32_t depth() const { return depth_; }

  void resetWarmUpCount(uint32_t count) { warmUpCount_ = count; }

  static constexpr size_t offsetOfFirstStub(uint32_t entryIndex) {
    return sizeof(ICScript) + entryIndex * sizeof(ICEntry) +
           ICEntry::offsetOfFirstStub();
  }

  static constexpr Offset offsetOfWarmUpCount() {
    return offsetof(ICScript, warmUpCount_);
  }
  static constexpr Offset offsetOfDepth() { return offsetof(ICScript, depth_); }

  static constexpr Offset offsetOfICEntries() { return sizeof(ICScript); }
  uint32_t numICEntries() const {
    return numElements<ICEntry>(icEntriesOffset(), fallbackStubsOffset());
  }

  ICEntry* interpreterICEntryFromPCOffset(uint32_t pcOffset);

  ICEntry& icEntryFromPCOffset(uint32_t pcOffset);

  [[nodiscard]] bool addInlinedChild(JSContext* cx,
                                     js::UniquePtr<ICScript> child,
                                     uint32_t pcOffset);
  ICScript* findInlinedChild(uint32_t pcOffset);
  void removeInlinedChild(uint32_t pcOffset);
  bool hasInlinedChild(uint32_t pcOffset);

  JitScriptICStubSpace* jitScriptStubSpace();
  void purgeOptimizedStubs(Zone* zone);

  void trace(JSTracer* trc);

#ifdef DEBUG
  mozilla::HashNumber hash();
#endif

 private:
  class CallSite {
   public:
    CallSite(ICScript* callee, uint32_t pcOffset)
        : callee_(callee), pcOffset_(pcOffset) {}
    ICScript* callee_;
    uint32_t pcOffset_;
  };

  // If this ICScript was created for trial inlining or has another
  // ICScript inlined into it, a pointer to the root of the inlining
  // tree. Otherwise, nullptr.
  InliningRoot* inliningRoot_ = nullptr;

  // ICScripts that have been inlined into this ICScript.
  js::UniquePtr<Vector<CallSite>> inlinedChildren_;

  // Number of times this copy of the script has been called or has had
  // backedges taken.  Reset if the script's JIT code is forcibly discarded.
  // See also the ScriptWarmUpData class.
  mozilla::Atomic<uint32_t, mozilla::Relaxed> warmUpCount_ = {};

  // The offset of the ICFallbackStub array.
  Offset fallbackStubsOffset_;

  // The size of this allocation.
  Offset endOffset_;

  // The inlining depth of this ICScript. 0 for the inlining root.
  uint32_t depth_;

  Offset icEntriesOffset() const { return offsetOfICEntries(); }
  Offset fallbackStubsOffset() const { return fallbackStubsOffset_; }
  Offset endOffset() const { return endOffset_; }

  ICEntry* icEntries() { return offsetToPointer<ICEntry>(icEntriesOffset()); }

  ICFallbackStub* fallbackStubs() {
    return offsetToPointer<ICFallbackStub>(fallbackStubsOffset());
  }

  JitScript* outerJitScript();

  friend class JitScript;
};

// [SMDOC] JitScript
//
// JitScript stores type inference data, Baseline ICs and other JIT-related data
// for a script. Scripts with a JitScript can run in the Baseline Interpreter.
//
// IC Data
// =======
// All IC data for Baseline (Interpreter and JIT) is stored in an ICScript. Each
// JitScript contains an ICScript as the last field. Additional free-standing
// ICScripts may be created during trial inlining. Ion has its own IC chains
// stored in IonScript.
//
// For each IC we store an ICEntry, which points to the first ICStub in the
// chain, and an ICFallbackStub. Note that multiple stubs in the same zone can
// share Baseline IC code. This works because the stub data is stored in the
// ICStub instead of baked in in the stub code.
//
// Storing this separate from BaselineScript allows us to use the same ICs in
// the Baseline Interpreter and Baseline JIT. It also simplifies debug mode OSR
// because the JitScript can be reused when we have to recompile the
// BaselineScript.
//
// The JitScript contains a stub space. This stores the "can GC" CacheIR stubs.
// These stubs are never purged before destroying the JitScript. Other stubs are
// stored in the optimized stub space stored in JitZone and can be purged more
// eagerly. See JitScript::purgeOptimizedStubs.
//
// An ICScript contains a list of IC entries and a list of fallback stubs.
// There's one ICEntry and ICFallbackStub for each JOF_IC bytecode op.
//
// The ICScript also contains the warmUpCount for the script.
//
// Inlining Data
// =============
// JitScript also contains a list of Warp compilations inlining this script, for
// invalidation.
//
// Memory Layout
// =============
// JitScript contains an ICScript as the last field. ICScript has trailing
// (variable length) arrays for ICEntry and ICFallbackStub. The memory layout is
// as follows:
//
//  Item                    | Offset
//  ------------------------+------------------------
//  JitScript               | 0
//  -->ICScript  (field)    |
//     ICEntry[]            | icEntriesOffset()
//     ICFallbackStub[]     | fallbackStubsOffset()
//
// These offsets are also used to compute numICEntries.
class alignas(uintptr_t) JitScript final : public TrailingArray {
  friend class ::JSScript;

  // Allocated space for Can-GC CacheIR stubs.
  JitScriptICStubSpace jitScriptStubSpace_ = {};

  // Profile string used by the profiler for Baseline Interpreter frames.
  const char* profileString_ = nullptr;

  // Baseline code for the script. Either nullptr, BaselineDisabledScriptPtr or
  // a valid BaselineScript*.
  GCStructPtr<BaselineScript*> baselineScript_;

  // Ion code for this script. Either nullptr, IonDisabledScriptPtr,
  // IonCompilingScriptPtr or a valid IonScript*.
  GCStructPtr<IonScript*> ionScript_;

  // For functions that need a CallObject and/or NamedLambdaObject, the template
  // objects used by the Baseline JIT and Ion. If the function needs both a
  // named lambda object and a call object, the named lambda object template is
  // linked via the call object's enclosing environment. This field is set the
  // first time the Baseline JIT compiles this script.
  mozilla::Maybe<HeapPtr<EnvironmentObject*>> templateEnv_;

  // Analysis data computed lazily the first time this script is compiled or
  // inlined by WarpBuilder.
  mozilla::Maybe<bool> usesEnvironmentChain_;

  // The size of this allocation.
  Offset endOffset_ = 0;

  struct Flags {
    // Flag set when discarding JIT code to indicate this script is on the stack
    // and type information and JIT code should not be discarded.
    bool active : 1;

    // True if this script entered Ion via OSR at a loop header.
    bool hadIonOSR : 1;
  };
  Flags flags_ = {};  // Zero-initialize flags.

  js::UniquePtr<InliningRoot> inliningRoot_;

#ifdef DEBUG
  // If the last warp compilation invalidated because of TranspiledCacheIR
  // bailouts, this is a hash of the ICScripts used in that compilation.
  // When recompiling, we assert that the hash has changed.
  mozilla::Maybe<mozilla::HashNumber> failedICHash_;

  // To avoid pathological cases, we skip the check if we have purged
  // stubs due to GC pressure.
  bool hasPurgedStubs_ = false;
#endif

  // List of allocation sites referred to by ICs in this script.
  Vector<gc::AllocSite*, 0, SystemAllocPolicy> allocSites_;

  ICScript icScript_;
  // End of fields.

  Offset endOffset() const { return endOffset_; }

 public:
  JitScript(JSScript* script, Offset fallbackStubsOffset, Offset endOffset,
            const char* profileString);

  ~JitScript();

  [[nodiscard]] bool ensureHasCachedBaselineJitData(JSContext* cx,
                                                    HandleScript script);
  [[nodiscard]] bool ensureHasCachedIonData(JSContext* cx, HandleScript script);

  void setHadIonOSR() { flags_.hadIonOSR = true; }
  bool hadIonOSR() const { return flags_.hadIonOSR; }

  uint32_t numICEntries() const { return icScript_.numICEntries(); }

  bool active() const { return flags_.active; }
  void setActive() { flags_.active = true; }
  void resetActive() { flags_.active = false; }

  void ensureProfileString(JSContext* cx, JSScript* script);

  const char* profileString() const {
    MOZ_ASSERT(profileString_);
    return profileString_;
  }

  static void Destroy(Zone* zone, JitScript* script);

  static constexpr Offset offsetOfICEntries() { return sizeof(JitScript); }

  static constexpr size_t offsetOfBaselineScript() {
    return offsetof(JitScript, baselineScript_);
  }
  static constexpr size_t offsetOfIonScript() {
    return offsetof(JitScript, ionScript_);
  }
  static constexpr size_t offsetOfICScript() {
    return offsetof(JitScript, icScript_);
  }
  static constexpr size_t offsetOfWarmUpCount() {
    return offsetOfICScript() + ICScript::offsetOfWarmUpCount();
  }

  uint32_t warmUpCount() const { return icScript_.warmUpCount_; }
  void incWarmUpCount() { icScript_.warmUpCount_++; }
  void resetWarmUpCount(uint32_t count);

  void prepareForDestruction(Zone* zone);

  JitScriptICStubSpace* jitScriptStubSpace() { return &jitScriptStubSpace_; }

  void addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf, size_t* data,
                              size_t* fallbackStubs) const {
    *data += mallocSizeOf(this);

    // |data| already includes the ICStubSpace itself, so use
    // sizeOfExcludingThis.
    *fallbackStubs += jitScriptStubSpace_.sizeOfExcludingThis(mallocSizeOf);
  }

  ICEntry& icEntry(size_t index) { return icScript_.icEntry(index); }

  ICFallbackStub* fallbackStub(size_t index) {
    return icScript_.fallbackStub(index);
  }

  ICEntry* icEntryForStub(const ICFallbackStub* stub) {
    return icScript_.icEntryForStub(stub);
  }
  ICFallbackStub* fallbackStubForICEntry(const ICEntry* entry) {
    return icScript_.fallbackStubForICEntry(entry);
  }

  void trace(JSTracer* trc);
  void purgeOptimizedStubs(JSScript* script);

  ICEntry& icEntryFromPCOffset(uint32_t pcOffset) {
    return icScript_.icEntryFromPCOffset(pcOffset);
  };

  size_t allocBytes() const { return endOffset(); }

  EnvironmentObject* templateEnvironment() const { return templateEnv_.ref(); }

  bool usesEnvironmentChain() const { return *usesEnvironmentChain_; }

  gc::AllocSite* createAllocSite(JSScript* script);

  bool resetAllocSites(bool resetNurserySites, bool resetPretenuredSites);

 private:
  // Methods to set baselineScript_ to a BaselineScript*, nullptr, or
  // BaselineDisabledScriptPtr.
  void setBaselineScriptImpl(JSScript* script, BaselineScript* baselineScript);
  void setBaselineScriptImpl(JS::GCContext* gcx, JSScript* script,
                             BaselineScript* baselineScript);

 public:
  // Methods for getting/setting/clearing a BaselineScript*.
  bool hasBaselineScript() const {
    bool res = baselineScript_ && baselineScript_ != BaselineDisabledScriptPtr;
    MOZ_ASSERT_IF(!res, !hasIonScript());
    return res;
  }
  BaselineScript* baselineScript() const {
    MOZ_ASSERT(hasBaselineScript());
    return baselineScript_;
  }
  void setBaselineScript(JSScript* script, BaselineScript* baselineScript) {
    MOZ_ASSERT(!hasBaselineScript());
    setBaselineScriptImpl(script, baselineScript);
    MOZ_ASSERT(hasBaselineScript());
  }
  [[nodiscard]] BaselineScript* clearBaselineScript(JS::GCContext* gcx,
                                                    JSScript* script) {
    BaselineScript* baseline = baselineScript();
    setBaselineScriptImpl(gcx, script, nullptr);
    return baseline;
  }

 private:
  // Methods to set ionScript_ to an IonScript*, nullptr, or one of the special
  // Ion{Disabled,Compiling}ScriptPtr values.
  void setIonScriptImpl(JS::GCContext* gcx, JSScript* script,
                        IonScript* ionScript);
  void setIonScriptImpl(JSScript* script, IonScript* ionScript);

 public:
  // Methods for getting/setting/clearing an IonScript*.
  bool hasIonScript() const {
    bool res = ionScript_ && ionScript_ != IonDisabledScriptPtr &&
               ionScript_ != IonCompilingScriptPtr;
    MOZ_ASSERT_IF(res, baselineScript_);
    return res;
  }
  IonScript* ionScript() const {
    MOZ_ASSERT(hasIonScript());
    return ionScript_;
  }
  void setIonScript(JSScript* script, IonScript* ionScript) {
    MOZ_ASSERT(!hasIonScript());
    setIonScriptImpl(script, ionScript);
    MOZ_ASSERT(hasIonScript());
  }
  [[nodiscard]] IonScript* clearIonScript(JS::GCContext* gcx,
                                          JSScript* script) {
    IonScript* ion = ionScript();
    setIonScriptImpl(gcx, script, nullptr);
    return ion;
  }

  // Methods for off-thread compilation.
  bool isIonCompilingOffThread() const {
    return ionScript_ == IonCompilingScriptPtr;
  }
  void setIsIonCompilingOffThread(JSScript* script) {
    MOZ_ASSERT(ionScript_ == nullptr);
    setIonScriptImpl(script, IonCompilingScriptPtr);
  }
  void clearIsIonCompilingOffThread(JSScript* script) {
    MOZ_ASSERT(isIonCompilingOffThread());
    setIonScriptImpl(script, nullptr);
  }
  ICScript* icScript() { return &icScript_; }

  bool hasInliningRoot() const { return !!inliningRoot_; }
  InliningRoot* inliningRoot() const { return inliningRoot_.get(); }
  InliningRoot* getOrCreateInliningRoot(JSContext* cx, JSScript* script);

#ifdef DEBUG
  bool hasFailedICHash() const { return failedICHash_.isSome(); }
  mozilla::HashNumber getFailedICHash() { return failedICHash_.extract(); }
  void setFailedICHash(mozilla::HashNumber hash) {
    MOZ_ASSERT(failedICHash_.isNothing());
    if (!hasPurgedStubs_) {
      failedICHash_.emplace(hash);
    }
  }
#endif
};

// Ensures no JitScripts are purged in the current zone.
class MOZ_RAII AutoKeepJitScripts {
  jit::JitZone* zone_;
  bool prev_;

  AutoKeepJitScripts(const AutoKeepJitScripts&) = delete;
  void operator=(const AutoKeepJitScripts&) = delete;

 public:
  explicit inline AutoKeepJitScripts(JSContext* cx);
  inline ~AutoKeepJitScripts();
};

// Mark JitScripts on the stack as active, so that they are not discarded
// during GC.
void MarkActiveJitScripts(Zone* zone);

#ifdef JS_STRUCTURED_SPEW
void JitSpewBaselineICStats(JSScript* script, const char* dumpReason);
#endif

}  // namespace jit
}  // namespace js

#endif /* jit_JitScript_h */
