/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Pretenuring.
 *
 * Some kinds of GC cells can be allocated in either the nursery or the tenured
 * heap. The pretenuring system decides where to allocate such cells based on
 * their expected lifetime with the aim of minimising total collection time.
 *
 * Lifetime is predicted based on data gathered about the cells' allocation
 * site. This data is gathered in the middle JIT tiers, after code has stopped
 * executing in the interpreter and before we generate fully optimized code.
 */

#ifndef gc_Pretenuring_h
#define gc_Pretenuring_h

#include <algorithm>

#include "gc/AllocKind.h"
#include "js/TypeDecls.h"

class JS_PUBLIC_API JSTracer;

namespace JS {
enum class GCReason;
}  // namespace JS

namespace js {
namespace gc {

class GCRuntime;
class PretenuringNursery;

// Number of trace kinds supportd by the nursery. These are arranged at the
// start of JS::TraceKind.
static constexpr size_t NurseryTraceKinds = 3;

enum class CatchAllAllocSite { Unknown, Optimized };

// Information about an allocation site.
//
// Nursery cells contain a pointer to one of these in their cell header (stored
// before the cell). The site can relate to either a specific JS bytecode
// instruction, a specific WebAssembly type, or can be a catch-all instance for
// unknown sites or JS JIT optimized code.
class AllocSite {
 public:
  enum class State : uint32_t { ShortLived = 0, Unknown = 1, LongLived = 2 };

  // The JIT depends on being able to tell the states apart by checking a single
  // bit.
  static constexpr int32_t LONG_LIVED_BIT = int32_t(State::LongLived);
  static_assert((LONG_LIVED_BIT & int32_t(State::Unknown)) == 0);
  static_assert((AllocSite::LONG_LIVED_BIT & int32_t(State::ShortLived)) == 0);

 private:
  JS::Zone* zone_ = nullptr;

  // Word storing JSScript pointer and site state.
  //
  // The script pointer is the script that owns this allocation site, a special
  // sentinel script for wasm sites, or null for unknown sites. This is used
  // when we need to invalidate the script.
  uintptr_t scriptAndState = uintptr_t(State::Unknown);
  static constexpr uintptr_t STATE_MASK = BitMask(2);

  // Next pointer forming a linked list of sites at which nursery allocation
  // happened since the last nursery collection.
  AllocSite* nextNurseryAllocated = nullptr;

  // Number of nursery allocations at this site since last nursery collection.
  uint32_t nurseryAllocCount = 0;

  // Number of nursery allocations that survived. Used during collection.
  uint32_t nurseryTenuredCount : 24;

  // Number of times the script has been invalidated.
  uint32_t invalidationCount : 4;

  // The trace kind of the allocation. Only kinds up to NurseryTraceKinds are
  // allowed.
  uint32_t traceKind_ : 4;

  static AllocSite* const EndSentinel;

  // Sentinel script for wasm sites.
  static JSScript* const WasmScript;

  friend class PretenuringZone;
  friend class PretenuringNursery;

  uintptr_t rawScript() const { return scriptAndState & ~STATE_MASK; }

 public:
  AllocSite() : nurseryTenuredCount(0), invalidationCount(0), traceKind_(0) {}

  // Create a dummy site to use for unknown allocations.
  explicit AllocSite(JS::Zone* zone, JS::TraceKind kind)
      : zone_(zone),
        nurseryTenuredCount(0),
        invalidationCount(0),
        traceKind_(uint32_t(kind)) {
    MOZ_ASSERT(traceKind_ < NurseryTraceKinds);
  }

  // Create a site for an opcode in the given script.
  AllocSite(JS::Zone* zone, JSScript* script, JS::TraceKind kind)
      : AllocSite(zone, kind) {
    MOZ_ASSERT(script != WasmScript);
    setScript(script);
  }

  void initUnknownSite(JS::Zone* zone, JS::TraceKind kind) {
    MOZ_ASSERT(!zone_ && scriptAndState == uintptr_t(State::Unknown));
    zone_ = zone;
    nurseryTenuredCount = 0;
    invalidationCount = 0;
    traceKind_ = uint32_t(kind);
    MOZ_ASSERT(traceKind_ < NurseryTraceKinds);
  }

  // Initialize a site to be a wasm site.
  void initWasm(JS::Zone* zone) {
    MOZ_ASSERT(!zone_ && scriptAndState == uintptr_t(State::Unknown));
    zone_ = zone;
    setScript(WasmScript);
    nurseryTenuredCount = 0;
    invalidationCount = 0;
    traceKind_ = uint32_t(JS::TraceKind::Object);
  }

  JS::Zone* zone() const { return zone_; }

  JS::TraceKind traceKind() const { return JS::TraceKind(traceKind_); }

  State state() const { return State(scriptAndState & STATE_MASK); }

  // Whether this site has a script associated with it. This is not true if
  // this site is for a wasm site.
  bool hasScript() const { return rawScript() != uintptr_t(WasmScript); }
  JSScript* script() const {
    MOZ_ASSERT(hasScript());
    return reinterpret_cast<JSScript*>(rawScript());
  }

  // Whether this site is not an unknown or optimized site.
  bool isNormal() const { return rawScript() != 0; }

  enum class Kind : uint32_t { Normal, Unknown, Optimized };
  Kind kind() const;

  bool isInAllocatedList() const { return nextNurseryAllocated; }

  // Whether allocations at this site should be allocated in the nursery or the
  // tenured heap.
  Heap initialHeap() const {
    return state() == State::LongLived ? Heap::Tenured : Heap::Default;
  }

  bool hasNurseryAllocations() const {
    return nurseryAllocCount != 0 || nurseryTenuredCount != 0;
  }
  void resetNurseryAllocations() {
    nurseryAllocCount = 0;
    nurseryTenuredCount = 0;
  }

  uint32_t incAllocCount() { return ++nurseryAllocCount; }
  uint32_t* nurseryAllocCountAddress() { return &nurseryAllocCount; }

  void incTenuredCount() {
    // The nursery is not large enough for this to overflow.
    nurseryTenuredCount++;
    MOZ_ASSERT(nurseryTenuredCount != 0);
  }

  size_t allocCount() const {
    return std::max(nurseryAllocCount, nurseryTenuredCount);
  }

  void updateStateOnMinorGC(double promotionRate);

  // Reset the state to 'Unknown' unless we have reached the invalidation limit
  // for this site. Return whether the state was reset.
  bool maybeResetState();

  bool invalidationLimitReached() const;
  bool invalidateScript(GCRuntime* gc);

  void trace(JSTracer* trc);

  static void printInfoHeader(JS::GCReason reason, double promotionRate);
  static void printInfoFooter(size_t sitesCreated, size_t sitesActive,
                              size_t sitesPretenured, size_t sitesInvalidated);
  void printInfo(bool hasPromotionRate, double promotionRate,
                 bool wasInvalidated) const;

  static constexpr size_t offsetOfScriptAndState() {
    return offsetof(AllocSite, scriptAndState);
  }
  static constexpr size_t offsetOfNurseryAllocCount() {
    return offsetof(AllocSite, nurseryAllocCount);
  }
  static constexpr size_t offsetOfNextNurseryAllocated() {
    return offsetof(AllocSite, nextNurseryAllocated);
  }

 private:
  void setScript(JSScript* newScript) {
    MOZ_ASSERT((uintptr_t(newScript) & STATE_MASK) == 0);
    scriptAndState = uintptr_t(newScript) | uintptr_t(state());
  }

  void setState(State newState) {
    MOZ_ASSERT((uintptr_t(newState) & ~STATE_MASK) == 0);
    scriptAndState = rawScript() | uintptr_t(newState);
  }

  const char* stateName() const;
};

// Pretenuring information stored per zone.
class PretenuringZone {
 public:
  // Catch-all allocation site instance used when the actual site is unknown, or
  // when optimized JIT code allocates a GC thing that's not handled by the
  // pretenuring system.
  AllocSite unknownAllocSites[NurseryTraceKinds];

  // Catch-all allocation instance used by optimized JIT code when allocating GC
  // things that are handled by the pretenuring system.  Allocation counts are
  // not recorded by optimized JIT code.
  AllocSite optimizedAllocSite;

  // Count of tenured cell allocations made between each major collection and
  // how many survived.
  uint32_t allocCountInNewlyCreatedArenas = 0;
  uint32_t survivorCountInNewlyCreatedArenas = 0;

  // Count of successive collections that had a low young tenured survival
  // rate. Used to discard optimized code if we get the pretenuring decision
  // wrong.
  uint32_t lowYoungTenuredSurvivalCount = 0;

  // Count of successive nursery collections that had a high survival rate for
  // objects allocated by optimized code. Used to discard optimized code if we
  // get the pretenuring decision wrong.
  uint32_t highNurserySurvivalCount = 0;

  // Total allocation count by trace kind (ignoring optimized
  // allocations). Calculated during nursery collection.
  uint32_t nurseryAllocCounts[NurseryTraceKinds] = {0};

  explicit PretenuringZone(JS::Zone* zone)
      : optimizedAllocSite(zone, JS::TraceKind::Object) {
    for (uint32_t i = 0; i < NurseryTraceKinds; i++) {
      unknownAllocSites[i].initUnknownSite(zone, JS::TraceKind(i));
    }
  }

  AllocSite& unknownAllocSite(JS::TraceKind kind) {
    size_t i = size_t(kind);
    MOZ_ASSERT(i < NurseryTraceKinds);
    return unknownAllocSites[i];
  }

  void clearCellCountsInNewlyCreatedArenas() {
    allocCountInNewlyCreatedArenas = 0;
    survivorCountInNewlyCreatedArenas = 0;
  }
  void updateCellCountsInNewlyCreatedArenas(uint32_t allocCount,
                                            uint32_t survivorCount) {
    allocCountInNewlyCreatedArenas += allocCount;
    survivorCountInNewlyCreatedArenas += survivorCount;
  }

  bool calculateYoungTenuredSurvivalRate(double* rateOut);

  void noteLowYoungTenuredSurvivalRate(bool lowYoungSurvivalRate);
  void noteHighNurserySurvivalRate(bool highNurserySurvivalRate);

  // Recovery: if code behaviour change we may need to reset allocation site
  // state and invalidate JIT code.
  bool shouldResetNurseryAllocSites();
  bool shouldResetPretenuredAllocSites();

  uint32_t& nurseryAllocCount(JS::TraceKind kind) {
    size_t i = size_t(kind);
    MOZ_ASSERT(i < NurseryTraceKinds);
    return nurseryAllocCounts[i];
  }
  uint32_t nurseryAllocCount(JS::TraceKind kind) const {
    return const_cast<PretenuringZone*>(this)->nurseryAllocCount(kind);
  }
};

// Pretenuring information stored as part of the the GC nursery.
class PretenuringNursery {
  gc::AllocSite* allocatedSites;

  size_t allocSitesCreated = 0;

  uint32_t totalAllocCount_ = 0;

 public:
  PretenuringNursery() : allocatedSites(AllocSite::EndSentinel) {}

  bool hasAllocatedSites() const {
    return allocatedSites != AllocSite::EndSentinel;
  }

  bool canCreateAllocSite();
  void noteAllocSiteCreated() { allocSitesCreated++; }

  void insertIntoAllocatedList(AllocSite* site) {
    MOZ_ASSERT(!site->isInAllocatedList());
    site->nextNurseryAllocated = allocatedSites;
    allocatedSites = site;
  }

  size_t doPretenuring(GCRuntime* gc, JS::GCReason reason,
                       bool validPromotionRate, double promotionRate,
                       bool reportInfo, size_t reportThreshold);

  void maybeStopPretenuring(GCRuntime* gc);

  uint32_t totalAllocCount() const { return totalAllocCount_; }

  void* addressOfAllocatedSites() { return &allocatedSites; }

 private:
  void processSite(GCRuntime* gc, AllocSite* site, size_t& sitesActive,
                   size_t& sitesPretenured, size_t& sitesInvalidated,
                   bool reportInfo, size_t reportThreshold);
  void processCatchAllSite(AllocSite* site, bool reportInfo,
                           size_t reportThreshold);
  void updateAllocCounts(AllocSite* site);
};

}  // namespace gc
}  // namespace js

#endif /* gc_Pretenuring_h */
