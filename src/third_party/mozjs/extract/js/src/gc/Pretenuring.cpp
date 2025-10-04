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
#include "jit/BaselineJIT.h"
#include "jit/Invalidation.h"
#include "js/Prefs.h"

#include "gc/Marking-inl.h"
#include "gc/PrivateIterators-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::gc;

// The maximum number of alloc sites to create between each minor
// collection. Stop tracking allocation after this limit is reached. This
// prevents unbounded time traversing the list during minor GC.
static constexpr size_t MaxAllocSitesPerMinorGC = 600;

// The maximum number of times to invalidate JIT code for a site. After this we
// leave the site's state as Unknown and don't pretenure allocations.
// Note we use 4 bits to store the invalidation count.
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

// The proportion of the nursery that must be promoted above which a minor
// collection may be determined to have a high nursery survival rate.
static constexpr double HighNurserySurvivalPromotionThreshold = 0.6;

// The number of nursery allocations made by optimized JIT code that must be
// promoted above which a minor collection may be determined to have a high
// nursery survival rate.
static constexpr size_t HighNurserySurvivalOptimizedAllocThreshold = 10000;

// The number of consecutive minor collections with a high nursery survival rate
// that must occur before recovery is attempted.
static constexpr size_t HighNurserySurvivalCountBeforeRecovery = 2;

AllocSite* const AllocSite::EndSentinel = reinterpret_cast<AllocSite*>(1);
JSScript* const AllocSite::WasmScript =
    reinterpret_cast<JSScript*>(AllocSite::STATE_MASK + 1);

/* static */
void AllocSite::staticAsserts() {
  static_assert(jit::BaselineMaxScriptLength <= MaxValidPCOffset);
}

bool PretenuringNursery::canCreateAllocSite() {
  MOZ_ASSERT(allocSitesCreated <= MaxAllocSitesPerMinorGC);
  return JS::Prefs::site_based_pretenuring() &&
         allocSitesCreated < MaxAllocSitesPerMinorGC;
}

size_t PretenuringNursery::doPretenuring(GCRuntime* gc, JS::GCReason reason,
                                         bool validPromotionRate,
                                         double promotionRate,
                                         const AllocSiteFilter& reportFilter) {
  size_t sitesActive = 0;
  size_t sitesPretenured = 0;
  size_t sitesInvalidated = 0;
  size_t zonesWithHighNurserySurvival = 0;

  // Zero allocation counts.
  totalAllocCount_ = 0;
  for (ZonesIter zone(gc, SkipAtoms); !zone.done(); zone.next()) {
    for (auto& count : zone->pretenuring.nurseryAllocCounts) {
      count = 0;
    }
  }

  // Check whether previously optimized code has changed its behaviour and
  // needs to be recompiled so that it can pretenure its allocations.
  if (validPromotionRate) {
    for (ZonesIter zone(gc, SkipAtoms); !zone.done(); zone.next()) {
      bool highNurserySurvivalRate =
          promotionRate > HighNurserySurvivalPromotionThreshold &&
          zone->optimizedAllocSite()->nurseryPromotedCount >=
              HighNurserySurvivalOptimizedAllocThreshold;
      zone->pretenuring.noteHighNurserySurvivalRate(highNurserySurvivalRate);
      if (highNurserySurvivalRate) {
        zonesWithHighNurserySurvival++;
      }
    }
  }

  if (reportFilter.enabled) {
    AllocSite::printInfoHeader(gc, reason, promotionRate);
  }

  AllocSite* site = allocatedSites;
  allocatedSites = AllocSite::EndSentinel;
  while (site != AllocSite::EndSentinel) {
    AllocSite* next = site->nextNurseryAllocated;
    site->nextNurseryAllocated = nullptr;

    if (site->isNormal()) {
      sitesActive++;
      updateTotalAllocCounts(site);
      auto result =
          site->processSite(gc, NormalSiteAttentionThreshold, reportFilter);
      if (result == AllocSite::WasPretenured ||
          result == AllocSite::WasPretenuredAndInvalidated) {
        sitesPretenured++;
        if (site->hasScript()) {
          site->script()->realm()->numAllocSitesPretenured++;
        }
      }
      if (result == AllocSite::WasPretenuredAndInvalidated) {
        sitesInvalidated++;
      }
    } else if (site->isMissing()) {
      sitesActive++;
      updateTotalAllocCounts(site);
      site->processMissingSite(reportFilter);
    }

    site = next;
  }

  // Catch-all sites don't end up on the list if they are only used from
  // optimized JIT code, so process them here.

  for (ZonesIter zone(gc, SkipAtoms); !zone.done(); zone.next()) {
    for (auto& site : zone->pretenuring.unknownAllocSites) {
      updateTotalAllocCounts(&site);
      if (site.traceKind() == JS::TraceKind::Object) {
        site.processCatchAllSite(reportFilter);
      } else {
        site.processSite(gc, UnknownSiteAttentionThreshold, reportFilter);
      }
      // Result checked in Nursery::doPretenuring.
    }
    updateTotalAllocCounts(zone->optimizedAllocSite());
    zone->optimizedAllocSite()->processCatchAllSite(reportFilter);

    // The data from the promoted alloc sites is never used so clear them here.
    for (AllocSite& site : zone->pretenuring.promotedAllocSites) {
      site.resetNurseryAllocations();
    }
  }

  if (reportFilter.enabled) {
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

AllocSite::SiteResult AllocSite::processSite(
    GCRuntime* gc, size_t attentionThreshold,
    const AllocSiteFilter& reportFilter) {
  MOZ_ASSERT(isNormal() || isUnknown());
  MOZ_ASSERT(nurseryAllocCount >= nurseryPromotedCount);

  SiteResult result = NoChange;

  bool hasPromotionRate = false;
  double promotionRate = 0.0;
  bool wasInvalidated = false;

  if (nurseryAllocCount > attentionThreshold) {
    promotionRate = double(nurseryPromotedCount) / double(nurseryAllocCount);
    hasPromotionRate = true;

    AllocSite::State prevState = state();
    updateStateOnMinorGC(promotionRate);
    AllocSite::State newState = state();

    if (prevState == AllocSite::State::Unknown &&
        newState == AllocSite::State::LongLived) {
      result = WasPretenured;

      // We can optimize JIT code before we realise that a site should be
      // pretenured. Make sure we invalidate any existing optimized code.
      if (isNormal() && hasScript()) {
        wasInvalidated = invalidateScript(gc);
        if (wasInvalidated) {
          result = WasPretenuredAndInvalidated;
        }
      }
    }
  }

  if (reportFilter.matches(*this)) {
    printInfo(hasPromotionRate, promotionRate, wasInvalidated);
  }

  resetNurseryAllocations();

  return result;
}

void AllocSite::processMissingSite(const AllocSiteFilter& reportFilter) {
  MOZ_ASSERT(isMissing());
  MOZ_ASSERT(nurseryAllocCount >= nurseryPromotedCount);

  // Forward counts from missing sites to the relevant unknown site.
  AllocSite* unknownSite = zone()->unknownAllocSite(traceKind());
  unknownSite->nurseryAllocCount += nurseryAllocCount;
  unknownSite->nurseryPromotedCount += nurseryPromotedCount;

  // Update state but only so we can report it.
  bool hasPromotionRate = false;
  double promotionRate = 0.0;
  if (nurseryAllocCount > NormalSiteAttentionThreshold) {
    promotionRate = double(nurseryPromotedCount) / double(nurseryAllocCount);
    hasPromotionRate = true;
    updateStateOnMinorGC(promotionRate);
  }

  if (reportFilter.matches(*this)) {
    printInfo(hasPromotionRate, promotionRate, false);
  }

  resetNurseryAllocations();
}

void AllocSite::processCatchAllSite(const AllocSiteFilter& reportFilter) {
  MOZ_ASSERT(isUnknown() || isOptimized());

  if (!hasNurseryAllocations()) {
    return;
  }

  if (reportFilter.matches(*this)) {
    printInfo(false, 0.0, false);
  }

  resetNurseryAllocations();
}

void PretenuringNursery::updateTotalAllocCounts(AllocSite* site) {
  JS::TraceKind kind = site->traceKind();
  totalAllocCount_ += site->nurseryAllocCount;
  PretenuringZone& zone = site->zone()->pretenuring;
  zone.nurseryAllocCount(kind) += site->nurseryAllocCount;
}

bool AllocSite::invalidateScript(GCRuntime* gc) {
  CancelOffThreadIonCompile(script());

  if (!script()->hasIonScript()) {
    return false;
  }

  if (invalidationLimitReached()) {
    MOZ_ASSERT(state() == State::Unknown);
    return false;
  }

  invalidationCount++;
  if (invalidationLimitReached()) {
    setState(State::Unknown);
  }

  JSContext* cx = gc->rt->mainContextFromOwnThread();
  jit::Invalidate(cx, script(),
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
    MOZ_ASSERT(state() == State::Unknown);
    return;
  }

  bool highPromotionRate = promotionRate >= 0.9;

  switch (state()) {
    case State::Unknown:
      if (highPromotionRate) {
        setState(State::LongLived);
      } else {
        setState(State::ShortLived);
      }
      break;

    case State::ShortLived: {
      if (highPromotionRate) {
        setState(State::Unknown);
      }
      break;
    }

    case State::LongLived: {
      if (!highPromotionRate) {
        setState(State::Unknown);
      }
      break;
    }
  }
}

bool AllocSite::maybeResetState() {
  if (invalidationLimitReached()) {
    MOZ_ASSERT(state() == State::Unknown);
    return false;
  }

  invalidationCount++;
  setState(State::Unknown);
  return true;
}

void AllocSite::trace(JSTracer* trc) {
  if (hasScript()) {
    JSScript* s = script();
    TraceManuallyBarrieredEdge(trc, &s, "AllocSite script");
    if (s != script()) {
      setScript(s);
    }
  }
}

bool AllocSite::traceWeak(JSTracer* trc) {
  if (hasScript()) {
    JSScript* s = script();
    if (!TraceManuallyBarrieredWeakEdge(trc, &s, "AllocSite script")) {
      return false;
    }
    if (s != script()) {
      setScript(s);
    }
  }

  return true;
}

bool AllocSite::needsSweep(JSTracer* trc) const {
  if (hasScript()) {
    JSScript* s = script();
    return IsAboutToBeFinalizedUnbarriered(s);
  }

  return false;
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

static const char* AllocSiteKindName(AllocSite::Kind kind) {
  switch (kind) {
    case AllocSite::Kind::Normal:
      return "normal";
    case AllocSite::Kind::Unknown:
      return "unknown";
    case AllocSite::Kind::Optimized:
      return "optimized";
    case AllocSite::Kind::Missing:
      return "missing";
    default:
      MOZ_CRASH("Bad AllocSite kind");
  }
}

/* static */
void AllocSite::printInfoHeader(GCRuntime* gc, JS::GCReason reason,
                                double promotionRate) {
  fprintf(stderr,
          "Pretenuring info after minor GC %zu for %s reason with promotion "
          "rate %4.1f%%:\n",
          size_t(gc->minorGCCount()), ExplainGCReason(reason),
          promotionRate * 100.0);
  fprintf(stderr, "  %-16s %-16s %-20s %-12s %-9s %-9s %-8s %-8s %-6s %-10s\n",
          "Site", "Zone", "Location", "BytecodeOp", "SiteKind", "TraceKind",
          "NAllocs", "Promotes", "PRate", "State");
}

static const char* FindBaseName(const char* filename) {
#ifdef XP_WIN
  constexpr char PathSeparator = '\\';
#else
  constexpr char PathSeparator = '/';
#endif

  const char* lastSep = strrchr(filename, PathSeparator);
  if (!lastSep) {
    return filename;
  }

  return lastSep + 1;
}

void AllocSite::printInfo(bool hasPromotionRate, double promotionRate,
                          bool wasInvalidated) const {
  // Zone.
  fprintf(stderr, "  %16p %16p", this, zone());

  // Location and bytecode op (not present for catch-all sites).
  char location[21] = {'\0'};
  char opName[13] = {'\0'};
  if (hasScript()) {
    uint32_t line = PCToLineNumber(script(), script()->offsetToPC(pcOffset()));
    const char* scriptName = FindBaseName(script()->filename());
    SprintfLiteral(location, "%s:%u", scriptName, line);
    BytecodeLocation location = script()->offsetToLocation(pcOffset());
    SprintfLiteral(opName, "%s", CodeName(location.getOp()));
  }
  fprintf(stderr, " %-20s %-12s", location, opName);

  // Which kind of site this is.
  fprintf(stderr, " %-9s", AllocSiteKindName(kind()));

  // Trace kind, except for optimized sites.
  const char* traceKindName = "";
  if (!isOptimized()) {
    traceKindName = JS::GCTraceKindToAscii(traceKind());
  }
  fprintf(stderr, " %-9s", traceKindName);

  // Nursery allocation count, missing for optimized sites.
  char buffer[16] = {'\0'};
  if (!isOptimized()) {
    SprintfLiteral(buffer, "%8" PRIu32, nurseryAllocCount);
  }
  fprintf(stderr, " %8s", buffer);

  // Nursery promotion count.
  fprintf(stderr, " %8" PRIu32, nurseryPromotedCount);

  // Promotion rate, if there were enough allocations.
  buffer[0] = '\0';
  if (hasPromotionRate) {
    SprintfLiteral(buffer, "%5.1f%%", std::min(1.0, promotionRate) * 100);
  }
  fprintf(stderr, " %6s", buffer);

  // Current state where applicable.
  const char* state = "";
  if (!isOptimized()) {
    state = stateName();
  }
  fprintf(stderr, " %-10s", state);

  // Whether the associated script was invalidated.
  if (wasInvalidated) {
    fprintf(stderr, " invalidated");
  }

  fprintf(stderr, "\n");
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

const char* AllocSite::stateName() const {
  switch (state()) {
    case State::ShortLived:
      return "ShortLived";
    case State::Unknown:
      return "Unknown";
    case State::LongLived:
      return "LongLived";
  }

  MOZ_CRASH("Unknown state");
}

static bool StringIsPrefix(const CharRange& prefix, const char* whole) {
  MOZ_ASSERT(prefix.length() != 0);
  return strncmp(prefix.begin().get(), whole, prefix.length()) == 0;
}

/* static */
bool AllocSiteFilter::readFromString(const char* string,
                                     AllocSiteFilter* filter) {
  *filter = AllocSiteFilter();

  CharRangeVector parts;
  if (!SplitStringBy(string, ',', &parts)) {
    MOZ_CRASH("OOM parsing AllocSiteFilter");
  }

  for (const auto& part : parts) {
    if (StringIsPrefix(part, "normal")) {
      filter->siteKindMask |= 1 << size_t(AllocSite::Kind::Normal);
    } else if (StringIsPrefix(part, "unknown")) {
      filter->siteKindMask |= 1 << size_t(AllocSite::Kind::Unknown);
    } else if (StringIsPrefix(part, "optimized")) {
      filter->siteKindMask |= 1 << size_t(AllocSite::Kind::Optimized);
    } else if (StringIsPrefix(part, "missing")) {
      filter->siteKindMask |= 1 << size_t(AllocSite::Kind::Missing);
    } else if (StringIsPrefix(part, "object")) {
      filter->traceKindMask |= 1 << size_t(JS::TraceKind::Object);
    } else if (StringIsPrefix(part, "string")) {
      filter->traceKindMask |= 1 << size_t(JS::TraceKind::String);
    } else if (StringIsPrefix(part, "bigint")) {
      filter->traceKindMask |= 1 << size_t(JS::TraceKind::BigInt);
    } else if (StringIsPrefix(part, "longlived")) {
      filter->stateMask |= 1 << size_t(AllocSite::State::LongLived);
    } else if (StringIsPrefix(part, "shortlived")) {
      filter->stateMask |= 1 << size_t(AllocSite::State::ShortLived);
    } else {
      char* end;
      filter->allocThreshold = strtol(part.begin().get(), &end, 10);
      if (end < part.end().get()) {
        return false;
      }
    }
  }

  filter->enabled = true;

  return true;
}

template <typename Enum>
static bool MaskFilterMatches(uint8_t mask, Enum value) {
  static_assert(std::is_enum_v<Enum>);

  if (mask == 0) {
    return true;  // Match if filter not specified.
  }

  MOZ_ASSERT(size_t(value) < 8);
  uint8_t bit = 1 << size_t(value);
  return (mask & bit) != 0;
}

bool AllocSiteFilter::matches(const AllocSite& site) const {
  // The state is not relevant for other kinds so skip filter.
  bool matchState = site.isNormal() || site.isMissing();

  return enabled &&
         (allocThreshold == 0 || site.allocCount() >= allocThreshold) &&
         MaskFilterMatches(siteKindMask, site.kind()) &&
         MaskFilterMatches(traceKindMask, site.traceKind()) &&
         (!matchState || MaskFilterMatches(stateMask, site.state()));
}

#ifdef JS_GC_ZEAL

AllocSite* js::gc::GetOrCreateMissingAllocSite(JSContext* cx, JSScript* script,
                                               uint32_t pcOffset,
                                               JS::TraceKind traceKind) {
  // Doesn't increment allocSitesCreated so as not to disturb pretenuring.

  Zone* zone = cx->zone();
  auto& missingSites = zone->missingSites;
  if (!missingSites) {
    missingSites = MakeUnique<MissingAllocSites>(zone);
    if (!missingSites) {
      return nullptr;
    }
  }

  auto scriptPtr = missingSites->scriptMap.lookupForAdd(script);
  if (!scriptPtr && !missingSites->scriptMap.add(
                        scriptPtr, script, MissingAllocSites::SiteMap())) {
    return nullptr;
  }
  auto& siteMap = scriptPtr->value();

  auto sitePtr = siteMap.lookupForAdd(pcOffset);
  if (!sitePtr) {
    UniquePtr<AllocSite> site = MakeUnique<AllocSite>(
        zone, script, pcOffset, traceKind, AllocSite::Kind::Missing);
    if (!site || !siteMap.add(sitePtr, pcOffset, std::move(site))) {
      return nullptr;
    }
  }

  return sitePtr->value().get();
}

#endif  // JS_GC_ZEAL
