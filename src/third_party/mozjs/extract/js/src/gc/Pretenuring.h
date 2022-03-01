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

#include "mozilla/Atomics.h"
#include "mozilla/Maybe.h"

#include "gc/AllocKind.h"
#include "js/TypeDecls.h"

class JS_PUBLIC_API JSTracer;

namespace js {
namespace gc {

class GCRuntime;
class PretenuringNursery;

enum class CatchAllAllocSite { Unknown, Optimized };

// Information about an allocation site.
//
// Nursery cells contain a pointer to one of these in their cell header (stored
// before the cell). The site can relate to either for a specific bytecode
// instruction or can be a catch-all instance for unknown sites or optimized
// code.
class AllocSite {
 public:
  enum class State : uint32_t { ShortLived, Unknown, LongLived };

 private:
  JS::Zone* const zone_;

  // Pointer to the JSScript that owns this allocation site or null for unknown
  // sites. This is used when we need to invalidate the script.
  JSScript* script_ = nullptr;

  // Next pointer forming a linked list of sites at which nursery allocation
  // happened since the last nursery collection.
  AllocSite* nextNurseryAllocated = nullptr;

  // Number of nursery allocations at this site since last nursery collection.
  uint32_t nurseryAllocCount = 0;

  // Number of nursery allocations that survived. Used during collection.
  uint32_t nurseryTenuredCount = 0;

  // The state is atomic as it can be read off-thread by warp transpilation.
  mozilla::Atomic<State, mozilla::ReleaseAcquire> state_{State::Unknown};

  // Number of times the script has been invalidated.
  uint32_t invalidationCount = 0;

  static AllocSite* const EndSentinel;

  friend class PretenuringZone;
  friend class PretenuringNursery;

 public:
  // Create a dummy site to use for unknown allocations.
  explicit AllocSite(JS::Zone* zone) : zone_(zone) {}

  // Create a site for an opcode in the given script.
  AllocSite(JS::Zone* zone, JSScript* script) : zone_(zone), script_(script) {}

  JS::Zone* zone() const { return zone_; }
  bool hasScript() const { return script_; }
  JSScript* script() const { return script_; }

  enum class Kind : uint32_t { Normal, Unknown, Optimized };
  Kind kind() const;

  bool isInAllocatedList() const { return nextNurseryAllocated; }

  State state() const { return state_; }

  // Whether allocations at this site should be allocated in the nursery or the
  // tenured heap.
  InitialHeap initialHeap() const {
    return state_ == State::LongLived ? TenuredHeap : DefaultHeap;
  }

  bool hasNurseryAllocations() const {
    return nurseryAllocCount != 0 || nurseryTenuredCount != 0;
  }
  void resetNurseryAllocations() {
    nurseryAllocCount = 0;
    nurseryTenuredCount = 0;
  }

  void incAllocCount() { nurseryAllocCount++; }
  void incTenuredCount() { nurseryTenuredCount++; }

  void updateStateOnMinorGC(double promotionRate);

  // Reset the state to 'Unknown' unless we have reached the invalidation limit
  // for this site. Return whether the state was reset.
  bool maybeResetState();

  bool invalidationLimitReached() const;
  bool invalidateScript(GCRuntime* gc);

  void trace(JSTracer* trc);

  static void printInfoHeader();
  static void printInfoFooter(size_t sitesCreated, size_t sitesActive,
                              size_t sitesPretenured, size_t sitesInvalidated);
  void printInfo(bool hasPromotionRate, double promotionRate,
                 bool wasInvalidated) const;

  static constexpr size_t offsetOfState() {
    return offsetof(AllocSite, state_);
  }
  static constexpr size_t offsetOfNurseryAllocCount() {
    return offsetof(AllocSite, nurseryAllocCount);
  }
  static constexpr size_t offsetOfNextNurseryAllocated() {
    return offsetof(AllocSite, nextNurseryAllocated);
  }

 private:
  const char* stateName() const;
};

// Pretenuring information stored per zone.
class PretenuringZone {
 public:
  explicit PretenuringZone(JS::Zone* zone)
      : unknownAllocSite(zone), optimizedAllocSite(zone) {}

  // Catch-all allocation site instance used when the actual site is unknown, or
  // when optimized JIT code allocates a GC thing that's not handled by the
  // pretenuring system.
  AllocSite unknownAllocSite;

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
};

// Pretenuring information stored as part of the the GC nursery.
class PretenuringNursery {
  gc::AllocSite* allocatedSites;

  size_t allocSitesCreated = 0;

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

  size_t doPretenuring(GCRuntime* gc, bool validPromotionRate,
                       double promotionRate, bool reportInfo);

  void maybeStopPretenuring(GCRuntime* gc);

  void* addressOfAllocatedSites() { return &allocatedSites; }
};

}  // namespace gc
}  // namespace js

#endif /* gc_Pretenuring_h */
