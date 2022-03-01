/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sw=2 et tw=80:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Pretenuring.h"

#include "mozilla/Sprintf.h"

#include "gc/GCInternals.h"
#include "gc/PublicIterators.h"
#include "jit/Invalidation.h"

#include "gc/PrivateIterators-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::gc;

// The number of nursery allocations at which to pay attention to an allocation
// site. This must be large enough to ensure we have enough information to infer
// the lifetime and also large enough to avoid pretenuring low volume allocation
// sites.
static constexpr size_t AllocSiteAttentionThreshold = 500;

// The maximum number of alloc sites to create between each minor
// collection. Stop tracking allocation after this limit is reached. This
// prevents unbounded time traversing the list during minor GC.
static constexpr size_t MaxAllocSitesPerMinorGC = 500;

// The maximum number of times to invalidate JIT code for a site. After this we
// leave the site's state as Unknown and don't pretenure allocations.
static constexpr size_t MaxInvalidationCount = 5;

// The minimum number of allocated cells needed to determine the survival rate
// of cells in newly created arenas.
static constexpr size_t MinCellsRequiredForSurvivalRate = 100;

// The young survival rate below which a major collection is determined to have
// a low young survival rate.
static constexpr double LowYoungSurvivalThreshold = 0.05;

// The number of consecutive major collections with a low young survival rate
// that must occur before recovery is attempted.
static constexpr size_t LowYoungSurvivalCountBeforeRecovery = 2;

// The proportion of the nursery that must be tenured above which a minor
// collection may be determined to have a high nursery survival rate.
static constexpr double HighNurserySurvivalPromotionThreshold = 0.6;

// The number of nursery allocations made by optimized JIT code that must be
// tenured above which a minor collection may be determined to have a high
// nursery survival rate.
static constexpr size_t HighNurserySurvivalOptimizedAllocThreshold = 10000;

// The number of consecutive minor collections with a high nursery survival rate
// that must occur before recovery is attempted.
static constexpr size_t HighNurserySurvivalCountBeforeRecovery = 2;

AllocSite* const AllocSite::EndSentinel = reinterpret_cast<AllocSite*>(1);

static bool SiteBasedPretenuringEnabled = true;

JS_PUBLIC_API void JS::SetSiteBasedPretenuringEnabled(bool enable) {
  SiteBasedPretenuringEnabled = enable;
}

bool PretenuringNursery::canCreateAllocSite() {
  MOZ_ASSERT(allocSitesCreated <= MaxAllocSitesPerMinorGC);
  return SiteBasedPretenuringEnabled &&
         allocSitesCreated < MaxAllocSitesPerMinorGC;
}

size_t PretenuringNursery::doPretenuring(GCRuntime* gc, bool validPromotionRate,
                                         double promotionRate,
                                         bool reportInfo) {
  mozilla::Maybe<AutoGCSession> session;

  size_t sitesActive = 0;
  size_t sitesPretenured = 0;
  size_t sitesInvalidated = 0;
  size_t zonesWithHighNurserySurvival = 0;

  // Check whether previously optimized code has changed its behaviour and
  // needs to be recompiled so that it can pretenure its allocations.
  if (validPromotionRate) {
    for (ZonesIter zone(gc, SkipAtoms); !zone.done(); zone.next()) {
      bool highNurserySurvivalRate =
          promotionRate > HighNurserySurvivalPromotionThreshold &&
          zone->optimizedAllocSite()->nurseryTenuredCount >=
              HighNurserySurvivalOptimizedAllocThreshold;
      zone->pretenuring.noteHighNurserySurvivalRate(highNurserySurvivalRate);
      if (highNurserySurvivalRate) {
        zonesWithHighNurserySurvival++;
      }
    }
  }

  if (reportInfo) {
    AllocSite::printInfoHeader();
  }

  AllocSite* site = allocatedSites;
  allocatedSites = AllocSite::EndSentinel;
  while (site != AllocSite::EndSentinel) {
    AllocSite* next = site->nextNurseryAllocated;
    site->nextNurseryAllocated = nullptr;

    MOZ_ASSERT_IF(site->hasScript(),
                  site->nurseryAllocCount >= site->nurseryTenuredCount);

    bool hasPromotionRate = false;
    double promotionRate = 0.0;
    bool wasInvalidated = false;
    if (site->hasScript()) {
      sitesActive++;

      if (site->nurseryAllocCount > AllocSiteAttentionThreshold) {
        promotionRate =
            double(site->nurseryTenuredCount) / double(site->nurseryAllocCount);
        hasPromotionRate = true;

        AllocSite::State prevState = site->state();
        site->updateStateOnMinorGC(promotionRate);
        AllocSite::State newState = site->state();

        if (prevState == AllocSite::State::Unknown &&
            newState == AllocSite::State::LongLived) {
          sitesPretenured++;

          // We can optimize JIT code before we realise that a site should be
          // pretenured. Make sure we invalidate any existing optimized code.

          if (!session.isSome()) {
            session.emplace(gc, JS::HeapState::MinorCollecting);
          }

          wasInvalidated = site->invalidateScript(gc);
          if (wasInvalidated) {
            sitesInvalidated++;
          }
        }
      }
    }

    if (reportInfo) {
      site->printInfo(hasPromotionRate, promotionRate, wasInvalidated);
    }

    site->resetNurseryAllocations();

    site = next;
  }

  // Optimized sites don't end up on the list if it is only used from optimized
  // JIT code so process them here.
  for (ZonesIter zone(gc, SkipAtoms); !zone.done(); zone.next()) {
    AllocSite* site = zone->optimizedAllocSite();
    if (site->hasNurseryAllocations()) {
      if (reportInfo) {
        site->printInfo(false, 0.0, false);
      }
      site->resetNurseryAllocations();
    }
  }

  if (reportInfo) {
    AllocSite::printInfoFooter(allocSitesCreated, sitesActive, sitesPretenured,
                               sitesInvalidated);
    if (zonesWithHighNurserySurvival) {
      fprintf(stderr, "  %zu zones with high nursery survival rate\n",
              zonesWithHighNurserySurvival);
    }
  }

  allocSitesCreated = 0;

  return sitesPretenured;
}

bool AllocSite::invalidateScript(GCRuntime* gc) {
  CancelOffThreadIonCompile(script_);

  if (!script_->hasIonScript()) {
    return false;
  }

  if (invalidationLimitReached()) {
    MOZ_ASSERT(state_ == State::Unknown);
    return false;
  }

  invalidationCount++;
  if (invalidationLimitReached()) {
    state_ = State::Unknown;
  }

  JSContext* cx = gc->rt->mainContextFromOwnThread();
  jit::Invalidate(cx, script_,
                  /* resetUses = */ false,
                  /* cancelOffThread = */ true);
  return true;
}

bool AllocSite::invalidationLimitReached() const {
  MOZ_ASSERT(invalidationCount <= MaxInvalidationCount);
  return invalidationCount == MaxInvalidationCount;
}

void PretenuringNursery::maybeStopPretenuring(GCRuntime* gc) {
  for (GCZonesIter zone(gc); !zone.done(); zone.next()) {
    double rate;
    if (zone->pretenuring.calculateYoungTenuredSurvivalRate(&rate)) {
      bool lowYoungSurvivalRate = rate < LowYoungSurvivalThreshold;
      zone->pretenuring.noteLowYoungTenuredSurvivalRate(lowYoungSurvivalRate);
    }
  }
}

AllocSite::Kind AllocSite::kind() const {
  if (hasScript()) {
    return Kind::Normal;
  }

  if (this == zone()->unknownAllocSite()) {
    return Kind::Unknown;
  }

  MOZ_ASSERT(this == zone()->optimizedAllocSite());
  return Kind::Optimized;
}

void AllocSite::updateStateOnMinorGC(double promotionRate) {
  // The state changes based on whether the promotion rate is deemed high
  // (greater that 90%):
  //
  //                      high                          high
  //               ------------------>           ------------------>
  //   ShortLived                       Unknown                        LongLived
  //               <------------------           <------------------
  //                      !high                         !high
  //
  // The nursery is used to allocate if the site's state is Unknown or
  // ShortLived. There are no direct transition between ShortLived and LongLived
  // to avoid pretenuring sites that we've recently observed being short-lived.

  if (invalidationLimitReached()) {
    MOZ_ASSERT(state_ == State::Unknown);
    return;
  }

  bool highPromotionRate = promotionRate >= 0.9;

  switch (state_) {
    case State::Unknown:
      if (highPromotionRate) {
        state_ = State::LongLived;
      } else {
        state_ = State::ShortLived;
      }
      break;

    case State::ShortLived: {
      if (highPromotionRate) {
        state_ = State::Unknown;
      }
      break;
    }

    case State::LongLived: {
      if (!highPromotionRate) {
        state_ = State::Unknown;
      }
      break;
    }
  }
}

bool AllocSite::maybeResetState() {
  if (invalidationLimitReached()) {
    MOZ_ASSERT(state_ == State::Unknown);
    return false;
  }

  invalidationCount++;
  state_ = State::Unknown;
  return true;
}

void AllocSite::trace(JSTracer* trc) {
  if (script_) {
    TraceManuallyBarrieredEdge(trc, &script_, "AllocSite script");
  }
}

bool PretenuringZone::calculateYoungTenuredSurvivalRate(double* rateOut) {
  MOZ_ASSERT(allocCountInNewlyCreatedArenas >=
             survivorCountInNewlyCreatedArenas);
  if (allocCountInNewlyCreatedArenas < MinCellsRequiredForSurvivalRate) {
    return false;
  }

  *rateOut = double(survivorCountInNewlyCreatedArenas) /
             double(allocCountInNewlyCreatedArenas);
  return true;
}

void PretenuringZone::noteLowYoungTenuredSurvivalRate(
    bool lowYoungSurvivalRate) {
  if (lowYoungSurvivalRate) {
    lowYoungTenuredSurvivalCount++;
  } else {
    lowYoungTenuredSurvivalCount = 0;
  }
}

void PretenuringZone::noteHighNurserySurvivalRate(
    bool highNurserySurvivalRate) {
  if (highNurserySurvivalRate) {
    highNurserySurvivalCount++;
  } else {
    highNurserySurvivalCount = 0;
  }
}

bool PretenuringZone::shouldResetNurseryAllocSites() {
  bool shouldReset =
      highNurserySurvivalCount >= HighNurserySurvivalCountBeforeRecovery;
  if (shouldReset) {
    highNurserySurvivalCount = 0;
  }
  return shouldReset;
}

bool PretenuringZone::shouldResetPretenuredAllocSites() {
  bool shouldReset =
      lowYoungTenuredSurvivalCount >= LowYoungSurvivalCountBeforeRecovery;
  if (shouldReset) {
    lowYoungTenuredSurvivalCount = 0;
  }
  return shouldReset;
}

/* static */
void AllocSite::printInfoHeader() {
  fprintf(stderr, "Pretenuring info after minor GC:\n");
}

/* static */
void AllocSite::printInfoFooter(size_t sitesCreated, size_t sitesActive,
                                size_t sitesPretenured,
                                size_t sitesInvalidated) {
  fprintf(stderr,
          "  %zu alloc sites created, %zu active, %zu pretenured, %zu "
          "invalidated\n",
          sitesCreated, sitesActive, sitesPretenured, sitesInvalidated);
}

void AllocSite::printInfo(bool hasPromotionRate, double promotionRate,
                          bool wasInvalidated) const {
  // Zone.
  fprintf(stderr, "  %p %p", this, zone());

  // Script, or which kind of catch-all site this is.
  if (!hasScript()) {
    fprintf(stderr, " %16s",
            kind() == Kind::Optimized ? "optimized" : "unknown");
  } else {
    fprintf(stderr, " %16p", script());
  }

  // Nursery allocation count, missing for optimized sites.
  char buffer[16] = {'\0'};
  if (kind() != Kind::Optimized) {
    SprintfLiteral(buffer, "%8" PRIu32, nurseryAllocCount);
  }
  fprintf(stderr, " %8s", buffer);

  // Nursery tenure count.
  fprintf(stderr, " %8" PRIu32, nurseryTenuredCount);

  // Promotion rate, if there were enough allocations.
  buffer[0] = '\0';
  if (hasPromotionRate) {
    SprintfLiteral(buffer, "%5.1f%%", std::min(1.0, promotionRate) * 100);
  }
  fprintf(stderr, " %6s", buffer);

  // Current state for sites associated with a script.
  const char* state = hasScript() ? stateName() : "";
  fprintf(stderr, " %10s", state);

  // Whether the associated script was invalidated.
  if (wasInvalidated) {
    fprintf(stderr, " invalidated");
  }

  fprintf(stderr, "\n");
}

const char* AllocSite::stateName() const {
  switch (state_) {
    case State::ShortLived:
      return "ShortLived";
    case State::Unknown:
      return "Unknown";
    case State::LongLived:
      return "LongLived";
  }

  MOZ_CRASH("Unknown state");
}
