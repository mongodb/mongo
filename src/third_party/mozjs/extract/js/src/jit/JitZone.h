/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitZone_h
#define jit_JitZone_h

#include "mozilla/Assertions.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/LinkedList.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"

#include <stddef.h>
#include <stdint.h>
#include <utility>

#include "gc/Barrier.h"
#include "gc/Marking.h"
#include "jit/ExecutableAllocator.h"
#include "jit/ICStubSpace.h"
#include "jit/Invalidation.h"
#include "jit/JitScript.h"
#include "js/AllocPolicy.h"
#include "js/GCHashTable.h"
#include "js/HashTable.h"
#include "js/TracingAPI.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "threading/ProtectedData.h"

namespace JS {
struct CodeSizes;
}

namespace js {
namespace jit {

enum class CacheKind : uint8_t;
class CacheIRStubInfo;
class JitCode;
class JitScript;

enum class ICStubEngine : uint8_t {
  // Baseline IC, see BaselineIC.h.
  Baseline = 0,

  // Ion IC, see IonIC.h.
  IonIC
};

struct CacheIRStubKey : public DefaultHasher<CacheIRStubKey> {
  struct Lookup {
    CacheKind kind;
    ICStubEngine engine;
    const uint8_t* code;
    uint32_t length;

    Lookup(CacheKind kind, ICStubEngine engine, const uint8_t* code,
           uint32_t length)
        : kind(kind), engine(engine), code(code), length(length) {}
  };

  static HashNumber hash(const Lookup& l);
  static bool match(const CacheIRStubKey& entry, const Lookup& l);

  UniquePtr<CacheIRStubInfo, JS::FreePolicy> stubInfo;

  explicit CacheIRStubKey(CacheIRStubInfo* info) : stubInfo(info) {}
  CacheIRStubKey(CacheIRStubKey&& other)
      : stubInfo(std::move(other.stubInfo)) {}

  void operator=(CacheIRStubKey&& other) {
    stubInfo = std::move(other.stubInfo);
  }
};

struct BaselineCacheIRStubCodeMapGCPolicy {
  static bool traceWeak(JSTracer* trc, CacheIRStubKey*,
                        WeakHeapPtr<JitCode*>* value) {
    return TraceWeakEdge(trc, value, "traceWeak");
  }
};

enum JitScriptFilter : bool { SkipDyingScripts, IncludeDyingScripts };

class JitZone {
  // Allocated space for CacheIR stubs.
  ICStubSpace stubSpace_;

  // Set of CacheIRStubInfo instances used by Ion stubs in this Zone.
  using IonCacheIRStubInfoSet =
      HashSet<CacheIRStubKey, CacheIRStubKey, SystemAllocPolicy>;
  IonCacheIRStubInfoSet ionCacheIRStubInfoSet_;

  // Map CacheIRStubKey to shared JitCode objects.
  using BaselineCacheIRStubCodeMap =
      GCHashMap<CacheIRStubKey, WeakHeapPtr<JitCode*>, CacheIRStubKey,
                SystemAllocPolicy, BaselineCacheIRStubCodeMapGCPolicy>;
  BaselineCacheIRStubCodeMap baselineCacheIRStubCodes_;

  // Executable allocator for all code except wasm code.
  MainThreadData<ExecutableAllocator> execAlloc_;

  // HashMap that maps scripts to compilations inlining those scripts.
  using InlinedScriptMap =
      GCHashMap<WeakHeapPtr<BaseScript*>, RecompileInfoVector,
                StableCellHasher<WeakHeapPtr<BaseScript*>>, SystemAllocPolicy>;
  InlinedScriptMap inlinedCompilations_;

  mozilla::LinkedList<JitScript> jitScripts_;

  // The following two fields are a pair of associated scripts. If they are
  // non-null, the child has been inlined into the parent, and we have bailed
  // out due to a MonomorphicInlinedStubFolding bailout. If it wasn't
  // trial-inlined, we need to track for the parent if we attach a new case to
  // the corresponding folded stub which belongs to the child.
  WeakHeapPtr<JSScript*> lastStubFoldingBailoutChild_;
  WeakHeapPtr<JSScript*> lastStubFoldingBailoutParent_;

  // The JitZone stores stubs to concatenate strings inline and perform RegExp
  // calls inline. These bake in zone specific pointers and can't be stored in
  // JitRuntime. They also are dependent on the value of 'initialStringHeap' and
  // must be flushed when its value changes.
  //
  // These are weak pointers, but they can by accessed during off-thread Ion
  // compilation and therefore can't use the usual read barrier. Instead, we
  // record which stubs have been read and perform the appropriate barriers in
  // CodeGenerator::link().

  enum StubIndex : uint32_t {
    StringConcat = 0,
    RegExpMatcher,
    RegExpSearcher,
    RegExpExecMatch,
    RegExpExecTest,
    Count
  };

  mozilla::EnumeratedArray<StubIndex, WeakHeapPtr<JitCode*>,
                           size_t(StubIndex::Count)>
      stubs_;

  mozilla::Maybe<IonCompilationId> currentCompilationId_;
  bool keepJitScripts_ = false;

  gc::Heap initialStringHeap = gc::Heap::Tenured;

  JitCode* generateStringConcatStub(JSContext* cx);
  JitCode* generateRegExpMatcherStub(JSContext* cx);
  JitCode* generateRegExpSearcherStub(JSContext* cx);
  JitCode* generateRegExpExecMatchStub(JSContext* cx);
  JitCode* generateRegExpExecTestStub(JSContext* cx);

  JitCode* getStubNoBarrier(StubIndex stub,
                            uint32_t* requiredBarriersOut) const {
    MOZ_ASSERT(CurrentThreadIsIonCompiling());
    *requiredBarriersOut |= 1 << uint32_t(stub);
    return stubs_[stub].unbarrieredGet();
  }

 public:
  explicit JitZone(bool zoneHasNurseryStrings) {
    setStringsCanBeInNursery(zoneHasNurseryStrings);
  }
  ~JitZone() {
    MOZ_ASSERT(jitScripts_.isEmpty());
    MOZ_ASSERT(!keepJitScripts_);
  }

  void traceWeak(JSTracer* trc, Zone* zone);

  void addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf,
                              JS::CodeSizes* code, size_t* jitZone,
                              size_t* cacheIRStubs) const;

  ICStubSpace* stubSpace() { return &stubSpace_; }

  JitCode* getBaselineCacheIRStubCode(const CacheIRStubKey::Lookup& key,
                                      CacheIRStubInfo** stubInfo) {
    auto p = baselineCacheIRStubCodes_.lookup(key);
    if (p) {
      *stubInfo = p->key().stubInfo.get();
      return p->value();
    }
    *stubInfo = nullptr;
    return nullptr;
  }
  [[nodiscard]] bool putBaselineCacheIRStubCode(
      const CacheIRStubKey::Lookup& lookup, CacheIRStubKey& key,
      JitCode* stubCode) {
    auto p = baselineCacheIRStubCodes_.lookupForAdd(lookup);
    MOZ_ASSERT(!p);
    return baselineCacheIRStubCodes_.add(p, std::move(key), stubCode);
  }

  CacheIRStubInfo* getIonCacheIRStubInfo(const CacheIRStubKey::Lookup& key) {
    IonCacheIRStubInfoSet::Ptr p = ionCacheIRStubInfoSet_.lookup(key);
    return p ? p->stubInfo.get() : nullptr;
  }
  [[nodiscard]] bool putIonCacheIRStubInfo(const CacheIRStubKey::Lookup& lookup,
                                           CacheIRStubKey& key) {
    IonCacheIRStubInfoSet::AddPtr p =
        ionCacheIRStubInfoSet_.lookupForAdd(lookup);
    MOZ_ASSERT(!p);
    return ionCacheIRStubInfoSet_.add(p, std::move(key));
  }
  void purgeIonCacheIRStubInfo() { ionCacheIRStubInfoSet_.clearAndCompact(); }

  ExecutableAllocator& execAlloc() { return execAlloc_.ref(); }
  const ExecutableAllocator& execAlloc() const { return execAlloc_.ref(); }

  [[nodiscard]] bool addInlinedCompilation(const RecompileInfo& info,
                                           JSScript* inlined);

  RecompileInfoVector* maybeInlinedCompilations(JSScript* inlined) {
    auto p = inlinedCompilations_.lookup(inlined);
    return p ? &p->value() : nullptr;
  }

  void removeInlinedCompilations(JSScript* inlined) {
    inlinedCompilations_.remove(inlined);
  }

  void noteStubFoldingBailout(JSScript* child, JSScript* parent) {
    lastStubFoldingBailoutChild_ = child;
    lastStubFoldingBailoutParent_ = parent;
  }
  bool hasStubFoldingBailoutData(JSScript* child) const {
    return lastStubFoldingBailoutChild_ &&
           lastStubFoldingBailoutChild_.get() == child &&
           lastStubFoldingBailoutParent_;
  }
  JSScript* stubFoldingBailoutParent() const {
    MOZ_ASSERT(lastStubFoldingBailoutChild_);
    return lastStubFoldingBailoutParent_.get();
  }
  void clearStubFoldingBailoutData() {
    lastStubFoldingBailoutChild_ = nullptr;
    lastStubFoldingBailoutParent_ = nullptr;
  }

  void registerJitScript(JitScript* script) { jitScripts_.insertBack(script); }

  // Iterate over all JitScripts in this zone calling |f| on each, allowing |f|
  // to remove the script. The template parameter |filter| controls whether to
  // include dying JitScripts during GC sweeping. Be careful when using this not
  // to let GC things reachable from the JitScript escape - they may be gray.
  template <JitScriptFilter filter = SkipDyingScripts, typename F>
  void forEachJitScript(F&& f) {
    JitScript* script = jitScripts_.getFirst();
    while (script) {
      JitScript* next = script->getNext();
      if (filter == IncludeDyingScripts ||
          !gc::IsAboutToBeFinalizedUnbarriered(script->owningScript())) {
        f(script);
      }
      script = next;
    }
  }

  // Like forEachJitScript above, but abort if |f| returns false.
  template <JitScriptFilter filter = SkipDyingScripts, typename F>
  bool forEachJitScriptFallible(F&& f) {
    JitScript* script = jitScripts_.getFirst();
    while (script) {
      JitScript* next = script->getNext();
      if (filter == IncludeDyingScripts ||
          !gc::IsAboutToBeFinalizedUnbarriered(script->owningScript())) {
        if (!f(script)) {
          return false;
        }
      }
      script = next;
    }
    return true;
  }

  bool keepJitScripts() const { return keepJitScripts_; }
  void setKeepJitScripts(bool keep) { keepJitScripts_ = keep; }

  mozilla::Maybe<IonCompilationId> currentCompilationId() const {
    return currentCompilationId_;
  }
  mozilla::Maybe<IonCompilationId>& currentCompilationIdRef() {
    return currentCompilationId_;
  }

  // Initialize code stubs only used by Ion, not Baseline.
  [[nodiscard]] bool ensureIonStubsExist(JSContext* cx) {
    if (stubs_[StringConcat]) {
      return true;
    }
    stubs_[StringConcat] = generateStringConcatStub(cx);
    return stubs_[StringConcat];
  }

  void traceWeak(JSTracer* trc, JS::Realm* realm);

  void discardStubs() {
    for (WeakHeapPtr<JitCode*>& stubRef : stubs_) {
      stubRef = nullptr;
    }
  }

  bool hasStubs() const {
    for (const WeakHeapPtr<JitCode*>& stubRef : stubs_) {
      if (stubRef) {
        return true;
      }
    }
    return false;
  }

  void setStringsCanBeInNursery(bool allow) {
    MOZ_ASSERT(!hasStubs());
    initialStringHeap = allow ? gc::Heap::Default : gc::Heap::Tenured;
  }

  JitCode* stringConcatStubNoBarrier(uint32_t* requiredBarriersOut) const {
    return getStubNoBarrier(StringConcat, requiredBarriersOut);
  }

  JitCode* regExpMatcherStubNoBarrier(uint32_t* requiredBarriersOut) const {
    return getStubNoBarrier(RegExpMatcher, requiredBarriersOut);
  }

  [[nodiscard]] JitCode* ensureRegExpMatcherStubExists(JSContext* cx) {
    if (JitCode* code = stubs_[RegExpMatcher]) {
      return code;
    }
    stubs_[RegExpMatcher] = generateRegExpMatcherStub(cx);
    return stubs_[RegExpMatcher];
  }

  JitCode* regExpSearcherStubNoBarrier(uint32_t* requiredBarriersOut) const {
    return getStubNoBarrier(RegExpSearcher, requiredBarriersOut);
  }

  [[nodiscard]] JitCode* ensureRegExpSearcherStubExists(JSContext* cx) {
    if (JitCode* code = stubs_[RegExpSearcher]) {
      return code;
    }
    stubs_[RegExpSearcher] = generateRegExpSearcherStub(cx);
    return stubs_[RegExpSearcher];
  }

  JitCode* regExpExecMatchStubNoBarrier(uint32_t* requiredBarriersOut) const {
    return getStubNoBarrier(RegExpExecMatch, requiredBarriersOut);
  }

  [[nodiscard]] JitCode* ensureRegExpExecMatchStubExists(JSContext* cx) {
    if (JitCode* code = stubs_[RegExpExecMatch]) {
      return code;
    }
    stubs_[RegExpExecMatch] = generateRegExpExecMatchStub(cx);
    return stubs_[RegExpExecMatch];
  }

  JitCode* regExpExecTestStubNoBarrier(uint32_t* requiredBarriersOut) const {
    return getStubNoBarrier(RegExpExecTest, requiredBarriersOut);
  }

  [[nodiscard]] JitCode* ensureRegExpExecTestStubExists(JSContext* cx) {
    if (JitCode* code = stubs_[RegExpExecTest]) {
      return code;
    }
    stubs_[RegExpExecTest] = generateRegExpExecTestStub(cx);
    return stubs_[RegExpExecTest];
  }

  // Perform the necessary read barriers on stubs described by the bitmasks
  // passed in. This function can only be called from the main thread.
  //
  // The stub pointers must still be valid by the time these methods are
  // called. This is arranged by cancelling off-thread Ion compilation at the
  // start of GC and at the start of sweeping.
  void performStubReadBarriers(uint32_t stubsToBarrier) const;

  static constexpr size_t offsetOfRegExpMatcherStub() {
    return offsetof(JitZone, stubs_) + RegExpMatcher * sizeof(uintptr_t);
  }
  static constexpr size_t offsetOfRegExpSearcherStub() {
    return offsetof(JitZone, stubs_) + RegExpSearcher * sizeof(uintptr_t);
  }
  static constexpr size_t offsetOfRegExpExecMatchStub() {
    return offsetof(JitZone, stubs_) + RegExpExecMatch * sizeof(uintptr_t);
  }
  static constexpr size_t offsetOfRegExpExecTestStub() {
    return offsetof(JitZone, stubs_) + RegExpExecTest * sizeof(uintptr_t);
  }
};

}  // namespace jit
}  // namespace js

#endif /* jit_JitZone_h */
