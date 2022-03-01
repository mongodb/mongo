/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitRealm_h
#define jit_JitRealm_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/MemoryReporting.h"

#include <stddef.h>
#include <stdint.h>

#include "gc/Barrier.h"
#include "gc/ZoneAllocator.h"
#include "js/GCHashTable.h"
#include "js/RootingAPI.h"
#include "js/TracingAPI.h"
#include "js/TypeDecls.h"

namespace js {

MOZ_COLD void ReportOutOfMemory(JSContext* cx);

namespace jit {

class JitCode;

struct IcStubCodeMapGCPolicy {
  static bool traceWeak(JSTracer* trc, uint32_t*, WeakHeapPtrJitCode* value) {
    return TraceWeakEdge(trc, value, "traceWeak");
  }
};

class JitRealm {
  friend class JitActivation;

  // Map ICStub keys to ICStub shared code objects.
  using ICStubCodeMap =
      GCHashMap<uint32_t, WeakHeapPtrJitCode, DefaultHasher<uint32_t>,
                ZoneAllocPolicy, IcStubCodeMapGCPolicy>;
  ICStubCodeMap* stubCodes_;

  // The JitRealm stores stubs to concatenate strings inline and perform RegExp
  // calls inline. These bake in zone and realm specific pointers and can't be
  // stored in JitRuntime. They also are dependent on the value of
  // 'stringsCanBeInNursery' and must be flushed when its value changes.
  //
  // These are weak pointers, but they can by accessed during off-thread Ion
  // compilation and therefore can't use the usual read barrier. Instead, we
  // record which stubs have been read and perform the appropriate barriers in
  // CodeGenerator::link().

  enum StubIndex : uint32_t {
    StringConcat = 0,
    RegExpMatcher,
    RegExpSearcher,
    RegExpTester,
    Count
  };

  mozilla::EnumeratedArray<StubIndex, StubIndex::Count, WeakHeapPtrJitCode>
      stubs_;

  bool stringsCanBeInNursery;

  JitCode* generateStringConcatStub(JSContext* cx);
  JitCode* generateRegExpMatcherStub(JSContext* cx);
  JitCode* generateRegExpSearcherStub(JSContext* cx);
  JitCode* generateRegExpTesterStub(JSContext* cx);

  JitCode* getStubNoBarrier(StubIndex stub,
                            uint32_t* requiredBarriersOut) const {
    MOZ_ASSERT(CurrentThreadIsIonCompiling());
    *requiredBarriersOut |= 1 << uint32_t(stub);
    return stubs_[stub].unbarrieredGet();
  }

 public:
  JitCode* getStubCode(uint32_t key) {
    ICStubCodeMap::Ptr p = stubCodes_->lookup(key);
    if (p) {
      return p->value();
    }
    return nullptr;
  }
  [[nodiscard]] bool putStubCode(JSContext* cx, uint32_t key,
                                 Handle<JitCode*> stubCode) {
    MOZ_ASSERT(stubCode);
    if (!stubCodes_->putNew(key, stubCode.get())) {
      ReportOutOfMemory(cx);
      return false;
    }
    return true;
  }

  JitRealm();
  ~JitRealm();

  [[nodiscard]] bool initialize(JSContext* cx, bool zoneHasNurseryStrings);

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
    for (WeakHeapPtrJitCode& stubRef : stubs_) {
      stubRef = nullptr;
    }
  }

  bool hasStubs() const {
    for (const WeakHeapPtrJitCode& stubRef : stubs_) {
      if (stubRef) {
        return true;
      }
    }
    return false;
  }

  void setStringsCanBeInNursery(bool allow) {
    MOZ_ASSERT(!hasStubs());
    stringsCanBeInNursery = allow;
  }

  JitCode* stringConcatStubNoBarrier(uint32_t* requiredBarriersOut) const {
    return getStubNoBarrier(StringConcat, requiredBarriersOut);
  }

  JitCode* regExpMatcherStubNoBarrier(uint32_t* requiredBarriersOut) const {
    return getStubNoBarrier(RegExpMatcher, requiredBarriersOut);
  }

  [[nodiscard]] bool ensureRegExpMatcherStubExists(JSContext* cx) {
    if (stubs_[RegExpMatcher]) {
      return true;
    }
    stubs_[RegExpMatcher] = generateRegExpMatcherStub(cx);
    return stubs_[RegExpMatcher];
  }

  JitCode* regExpSearcherStubNoBarrier(uint32_t* requiredBarriersOut) const {
    return getStubNoBarrier(RegExpSearcher, requiredBarriersOut);
  }

  [[nodiscard]] bool ensureRegExpSearcherStubExists(JSContext* cx) {
    if (stubs_[RegExpSearcher]) {
      return true;
    }
    stubs_[RegExpSearcher] = generateRegExpSearcherStub(cx);
    return stubs_[RegExpSearcher];
  }

  JitCode* regExpTesterStubNoBarrier(uint32_t* requiredBarriersOut) const {
    return getStubNoBarrier(RegExpTester, requiredBarriersOut);
  }

  [[nodiscard]] bool ensureRegExpTesterStubExists(JSContext* cx) {
    if (stubs_[RegExpTester]) {
      return true;
    }
    stubs_[RegExpTester] = generateRegExpTesterStub(cx);
    return stubs_[RegExpTester];
  }

  // Perform the necessary read barriers on stubs described by the bitmasks
  // passed in. This function can only be called from the main thread.
  //
  // The stub pointers must still be valid by the time these methods are
  // called. This is arranged by cancelling off-thread Ion compilation at the
  // start of GC and at the start of sweeping.
  void performStubReadBarriers(uint32_t stubsToBarrier) const;

  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

}  // namespace jit
}  // namespace js

#endif /* jit_JitRealm_h */
