/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Scheduling.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/TimeStamp.h"

#include <algorithm>

#include "gc/Nursery.h"
#include "gc/RelocationOverlay.h"
#include "gc/ZoneAllocator.h"
#include "vm/MutexIDs.h"

using namespace js;
using namespace js::gc;

using mozilla::CheckedInt;
using mozilla::TimeDuration;

/*
 * We may start to collect a zone before its trigger threshold is reached if
 * GCRuntime::maybeGC() is called for that zone or we start collecting other
 * zones. These eager threshold factors are not configurable.
 */
static constexpr double HighFrequencyEagerAllocTriggerFactor = 0.85;
static constexpr double LowFrequencyEagerAllocTriggerFactor = 0.9;

/*
 * Don't allow heap growth factors to be set so low that eager collections could
 * reduce the trigger threshold.
 */
static constexpr double MinHeapGrowthFactor =
    1.0f / std::min(HighFrequencyEagerAllocTriggerFactor,
                    LowFrequencyEagerAllocTriggerFactor);

GCSchedulingTunables::GCSchedulingTunables()
    : gcMaxBytes_(0),
      gcMinNurseryBytes_(Nursery::roundSize(TuningDefaults::GCMinNurseryBytes)),
      gcMaxNurseryBytes_(Nursery::roundSize(JS::DefaultNurseryMaxBytes)),
      gcZoneAllocThresholdBase_(TuningDefaults::GCZoneAllocThresholdBase),
      smallHeapIncrementalLimit_(TuningDefaults::SmallHeapIncrementalLimit),
      largeHeapIncrementalLimit_(TuningDefaults::LargeHeapIncrementalLimit),
      zoneAllocDelayBytes_(TuningDefaults::ZoneAllocDelayBytes),
      highFrequencyThreshold_(
          TimeDuration::FromSeconds(TuningDefaults::HighFrequencyThreshold)),
      smallHeapSizeMaxBytes_(TuningDefaults::SmallHeapSizeMaxBytes),
      largeHeapSizeMinBytes_(TuningDefaults::LargeHeapSizeMinBytes),
      highFrequencySmallHeapGrowth_(
          TuningDefaults::HighFrequencySmallHeapGrowth),
      highFrequencyLargeHeapGrowth_(
          TuningDefaults::HighFrequencyLargeHeapGrowth),
      lowFrequencyHeapGrowth_(TuningDefaults::LowFrequencyHeapGrowth),
      minEmptyChunkCount_(TuningDefaults::MinEmptyChunkCount),
      maxEmptyChunkCount_(TuningDefaults::MaxEmptyChunkCount),
      nurseryFreeThresholdForIdleCollection_(
          TuningDefaults::NurseryFreeThresholdForIdleCollection),
      nurseryFreeThresholdForIdleCollectionFraction_(
          TuningDefaults::NurseryFreeThresholdForIdleCollectionFraction),
      nurseryTimeoutForIdleCollection_(TimeDuration::FromMilliseconds(
          TuningDefaults::NurseryTimeoutForIdleCollectionMS)),
      pretenureThreshold_(TuningDefaults::PretenureThreshold),
      pretenureGroupThreshold_(TuningDefaults::PretenureGroupThreshold),
      pretenureStringThreshold_(TuningDefaults::PretenureStringThreshold),
      stopPretenureStringThreshold_(
          TuningDefaults::StopPretenureStringThreshold),
      minLastDitchGCPeriod_(
          TimeDuration::FromSeconds(TuningDefaults::MinLastDitchGCPeriod)),
      mallocThresholdBase_(TuningDefaults::MallocThresholdBase),
      mallocGrowthFactor_(TuningDefaults::MallocGrowthFactor) {}

bool GCSchedulingTunables::setParameter(JSGCParamKey key, uint32_t value,
                                        const AutoLockGC& lock) {
  // Limit heap growth factor to one hundred times size of current heap.
  const double MaxHeapGrowthFactor = 100;
  const size_t MaxNurseryBytes = 128 * 1024 * 1024;

  switch (key) {
    case JSGC_MAX_BYTES:
      gcMaxBytes_ = value;
      break;
    case JSGC_MIN_NURSERY_BYTES:
      if (value < ArenaSize || value >= MaxNurseryBytes) {
        return false;
      }
      value = Nursery::roundSize(value);
      if (value > gcMaxNurseryBytes_) {
        return false;
      }
      gcMinNurseryBytes_ = value;
      break;
    case JSGC_MAX_NURSERY_BYTES:
      if (value < ArenaSize || value >= MaxNurseryBytes) {
        return false;
      }
      value = Nursery::roundSize(value);
      if (value < gcMinNurseryBytes_) {
        return false;
      }
      gcMaxNurseryBytes_ = value;
      break;
    case JSGC_HIGH_FREQUENCY_TIME_LIMIT:
      highFrequencyThreshold_ = TimeDuration::FromMilliseconds(value);
      break;
    case JSGC_SMALL_HEAP_SIZE_MAX: {
      CheckedInt<size_t> newLimit = CheckedInt<size_t>(value) * 1024 * 1024;
      if (!newLimit.isValid()) {
        return false;
      }
      setSmallHeapSizeMaxBytes(newLimit.value());
      break;
    }
    case JSGC_LARGE_HEAP_SIZE_MIN: {
      size_t newLimit = (size_t)value * 1024 * 1024;
      if (newLimit == 0) {
        return false;
      }
      setLargeHeapSizeMinBytes(newLimit);
      break;
    }
    case JSGC_HIGH_FREQUENCY_SMALL_HEAP_GROWTH: {
      double newGrowth = value / 100.0;
      if (newGrowth < MinHeapGrowthFactor || newGrowth > MaxHeapGrowthFactor) {
        return false;
      }
      setHighFrequencySmallHeapGrowth(newGrowth);
      break;
    }
    case JSGC_HIGH_FREQUENCY_LARGE_HEAP_GROWTH: {
      double newGrowth = value / 100.0;
      if (newGrowth < MinHeapGrowthFactor || newGrowth > MaxHeapGrowthFactor) {
        return false;
      }
      setHighFrequencyLargeHeapGrowth(newGrowth);
      break;
    }
    case JSGC_LOW_FREQUENCY_HEAP_GROWTH: {
      double newGrowth = value / 100.0;
      if (newGrowth < MinHeapGrowthFactor || newGrowth > MaxHeapGrowthFactor) {
        return false;
      }
      setLowFrequencyHeapGrowth(newGrowth);
      break;
    }
    case JSGC_ALLOCATION_THRESHOLD:
      gcZoneAllocThresholdBase_ = value * 1024 * 1024;
      break;
    case JSGC_SMALL_HEAP_INCREMENTAL_LIMIT: {
      double newFactor = value / 100.0;
      if (newFactor < 1.0f || newFactor > MaxHeapGrowthFactor) {
        return false;
      }
      smallHeapIncrementalLimit_ = newFactor;
      break;
    }
    case JSGC_LARGE_HEAP_INCREMENTAL_LIMIT: {
      double newFactor = value / 100.0;
      if (newFactor < 1.0f || newFactor > MaxHeapGrowthFactor) {
        return false;
      }
      largeHeapIncrementalLimit_ = newFactor;
      break;
    }
    case JSGC_MIN_EMPTY_CHUNK_COUNT:
      setMinEmptyChunkCount(value);
      break;
    case JSGC_MAX_EMPTY_CHUNK_COUNT:
      setMaxEmptyChunkCount(value);
      break;
    case JSGC_NURSERY_FREE_THRESHOLD_FOR_IDLE_COLLECTION:
      if (value > gcMaxNurseryBytes()) {
        value = gcMaxNurseryBytes();
      }
      nurseryFreeThresholdForIdleCollection_ = value;
      break;
    case JSGC_NURSERY_FREE_THRESHOLD_FOR_IDLE_COLLECTION_PERCENT:
      if (value == 0 || value > 100) {
        return false;
      }
      nurseryFreeThresholdForIdleCollectionFraction_ = value / 100.0;
      break;
    case JSGC_NURSERY_TIMEOUT_FOR_IDLE_COLLECTION_MS:
      nurseryTimeoutForIdleCollection_ = TimeDuration::FromMilliseconds(value);
      break;
    case JSGC_PRETENURE_THRESHOLD: {
      // 100 disables pretenuring
      if (value == 0 || value > 100) {
        return false;
      }
      pretenureThreshold_ = value / 100.0;
      break;
    }
    case JSGC_PRETENURE_GROUP_THRESHOLD:
      if (value <= 0) {
        return false;
      }
      pretenureGroupThreshold_ = value;
      break;
    case JSGC_PRETENURE_STRING_THRESHOLD:
      // 100 disables pretenuring
      if (value == 0 || value > 100) {
        return false;
      }
      pretenureStringThreshold_ = value / 100.0;
      break;
    case JSGC_STOP_PRETENURE_STRING_THRESHOLD:
      if (value == 0 || value > 100) {
        return false;
      }
      stopPretenureStringThreshold_ = value / 100.0;
      break;
    case JSGC_MIN_LAST_DITCH_GC_PERIOD:
      minLastDitchGCPeriod_ = TimeDuration::FromSeconds(value);
      break;
    case JSGC_ZONE_ALLOC_DELAY_KB:
      zoneAllocDelayBytes_ = value * 1024;
      break;
    case JSGC_MALLOC_THRESHOLD_BASE:
      mallocThresholdBase_ = value * 1024 * 1024;
      break;
    case JSGC_MALLOC_GROWTH_FACTOR: {
      double newGrowth = value / 100.0;
      if (newGrowth < MinHeapGrowthFactor || newGrowth > MaxHeapGrowthFactor) {
        return false;
      }
      mallocGrowthFactor_ = newGrowth;
      break;
    }
    default:
      MOZ_CRASH("Unknown GC parameter.");
  }

  return true;
}

void GCSchedulingTunables::setSmallHeapSizeMaxBytes(size_t value) {
  smallHeapSizeMaxBytes_ = value;
  if (smallHeapSizeMaxBytes_ >= largeHeapSizeMinBytes_) {
    largeHeapSizeMinBytes_ = smallHeapSizeMaxBytes_ + 1;
  }
  MOZ_ASSERT(largeHeapSizeMinBytes_ > smallHeapSizeMaxBytes_);
}

void GCSchedulingTunables::setLargeHeapSizeMinBytes(size_t value) {
  largeHeapSizeMinBytes_ = value;
  if (largeHeapSizeMinBytes_ <= smallHeapSizeMaxBytes_) {
    smallHeapSizeMaxBytes_ = largeHeapSizeMinBytes_ - 1;
  }
  MOZ_ASSERT(largeHeapSizeMinBytes_ > smallHeapSizeMaxBytes_);
}

void GCSchedulingTunables::setHighFrequencyLargeHeapGrowth(double value) {
  highFrequencyLargeHeapGrowth_ = value;
  if (highFrequencyLargeHeapGrowth_ > highFrequencySmallHeapGrowth_) {
    highFrequencySmallHeapGrowth_ = highFrequencyLargeHeapGrowth_;
  }
  MOZ_ASSERT(highFrequencyLargeHeapGrowth_ >= MinHeapGrowthFactor);
  MOZ_ASSERT(highFrequencyLargeHeapGrowth_ <= highFrequencySmallHeapGrowth_);
}

void GCSchedulingTunables::setHighFrequencySmallHeapGrowth(double value) {
  highFrequencySmallHeapGrowth_ = value;
  if (highFrequencySmallHeapGrowth_ < highFrequencyLargeHeapGrowth_) {
    highFrequencyLargeHeapGrowth_ = highFrequencySmallHeapGrowth_;
  }
  MOZ_ASSERT(highFrequencyLargeHeapGrowth_ >= MinHeapGrowthFactor);
  MOZ_ASSERT(highFrequencyLargeHeapGrowth_ <= highFrequencySmallHeapGrowth_);
}

void GCSchedulingTunables::setLowFrequencyHeapGrowth(double value) {
  lowFrequencyHeapGrowth_ = value;
  MOZ_ASSERT(lowFrequencyHeapGrowth_ >= MinHeapGrowthFactor);
}

void GCSchedulingTunables::setMinEmptyChunkCount(uint32_t value) {
  minEmptyChunkCount_ = value;
  if (minEmptyChunkCount_ > maxEmptyChunkCount_) {
    maxEmptyChunkCount_ = minEmptyChunkCount_;
  }
  MOZ_ASSERT(maxEmptyChunkCount_ >= minEmptyChunkCount_);
}

void GCSchedulingTunables::setMaxEmptyChunkCount(uint32_t value) {
  maxEmptyChunkCount_ = value;
  if (minEmptyChunkCount_ > maxEmptyChunkCount_) {
    minEmptyChunkCount_ = maxEmptyChunkCount_;
  }
  MOZ_ASSERT(maxEmptyChunkCount_ >= minEmptyChunkCount_);
}

void GCSchedulingTunables::resetParameter(JSGCParamKey key,
                                          const AutoLockGC& lock) {
  switch (key) {
    case JSGC_MAX_BYTES:
      gcMaxBytes_ = 0xffffffff;
      break;
    case JSGC_MIN_NURSERY_BYTES:
    case JSGC_MAX_NURSERY_BYTES:
      // Reset these togeather to maintain their min <= max invariant.
      gcMinNurseryBytes_ = TuningDefaults::GCMinNurseryBytes;
      gcMaxNurseryBytes_ = JS::DefaultNurseryMaxBytes;
      break;
    case JSGC_HIGH_FREQUENCY_TIME_LIMIT:
      highFrequencyThreshold_ =
          TimeDuration::FromSeconds(TuningDefaults::HighFrequencyThreshold);
      break;
    case JSGC_SMALL_HEAP_SIZE_MAX:
      setSmallHeapSizeMaxBytes(TuningDefaults::SmallHeapSizeMaxBytes);
      break;
    case JSGC_LARGE_HEAP_SIZE_MIN:
      setLargeHeapSizeMinBytes(TuningDefaults::LargeHeapSizeMinBytes);
      break;
    case JSGC_HIGH_FREQUENCY_SMALL_HEAP_GROWTH:
      setHighFrequencySmallHeapGrowth(
          TuningDefaults::HighFrequencySmallHeapGrowth);
      break;
    case JSGC_HIGH_FREQUENCY_LARGE_HEAP_GROWTH:
      setHighFrequencyLargeHeapGrowth(
          TuningDefaults::HighFrequencyLargeHeapGrowth);
      break;
    case JSGC_LOW_FREQUENCY_HEAP_GROWTH:
      setLowFrequencyHeapGrowth(TuningDefaults::LowFrequencyHeapGrowth);
      break;
    case JSGC_ALLOCATION_THRESHOLD:
      gcZoneAllocThresholdBase_ = TuningDefaults::GCZoneAllocThresholdBase;
      break;
    case JSGC_SMALL_HEAP_INCREMENTAL_LIMIT:
      smallHeapIncrementalLimit_ = TuningDefaults::SmallHeapIncrementalLimit;
      break;
    case JSGC_LARGE_HEAP_INCREMENTAL_LIMIT:
      largeHeapIncrementalLimit_ = TuningDefaults::LargeHeapIncrementalLimit;
      break;
    case JSGC_MIN_EMPTY_CHUNK_COUNT:
      setMinEmptyChunkCount(TuningDefaults::MinEmptyChunkCount);
      break;
    case JSGC_MAX_EMPTY_CHUNK_COUNT:
      setMaxEmptyChunkCount(TuningDefaults::MaxEmptyChunkCount);
      break;
    case JSGC_NURSERY_FREE_THRESHOLD_FOR_IDLE_COLLECTION:
      nurseryFreeThresholdForIdleCollection_ =
          TuningDefaults::NurseryFreeThresholdForIdleCollection;
      break;
    case JSGC_NURSERY_FREE_THRESHOLD_FOR_IDLE_COLLECTION_PERCENT:
      nurseryFreeThresholdForIdleCollectionFraction_ =
          TuningDefaults::NurseryFreeThresholdForIdleCollectionFraction;
      break;
    case JSGC_NURSERY_TIMEOUT_FOR_IDLE_COLLECTION_MS:
      nurseryTimeoutForIdleCollection_ = TimeDuration::FromMilliseconds(
          TuningDefaults::NurseryTimeoutForIdleCollectionMS);
      break;
    case JSGC_PRETENURE_THRESHOLD:
      pretenureThreshold_ = TuningDefaults::PretenureThreshold;
      break;
    case JSGC_PRETENURE_GROUP_THRESHOLD:
      pretenureGroupThreshold_ = TuningDefaults::PretenureGroupThreshold;
      break;
    case JSGC_PRETENURE_STRING_THRESHOLD:
      pretenureStringThreshold_ = TuningDefaults::PretenureStringThreshold;
      break;
    case JSGC_MIN_LAST_DITCH_GC_PERIOD:
      minLastDitchGCPeriod_ =
          TimeDuration::FromSeconds(TuningDefaults::MinLastDitchGCPeriod);
      break;
    case JSGC_MALLOC_THRESHOLD_BASE:
      mallocThresholdBase_ = TuningDefaults::MallocThresholdBase;
      break;
    case JSGC_MALLOC_GROWTH_FACTOR:
      mallocGrowthFactor_ = TuningDefaults::MallocGrowthFactor;
      break;
    default:
      MOZ_CRASH("Unknown GC parameter.");
  }
}

// GC thresholds may exceed the range of size_t on 32-bit platforms, so these
// are calculated using 64-bit integers and clamped.
static inline size_t ToClampedSize(uint64_t bytes) {
  return std::min(bytes, uint64_t(SIZE_MAX));
}

void HeapThreshold::setIncrementalLimitFromStartBytes(
    size_t retainedBytes, const GCSchedulingTunables& tunables) {
  // Calculate the incremental limit for a heap based on its size and start
  // threshold.
  //
  // This effectively classifies the heap size into small, medium or large, and
  // uses the small heap incremental limit paramer, the large heap incremental
  // limit parameter or an interpolation between them.
  //
  // The incremental limit is always set greater than the start threshold by at
  // least the maximum nursery size to reduce the chance that tenuring a full
  // nursery will send us straight into non-incremental collection.

  MOZ_ASSERT(tunables.smallHeapIncrementalLimit() >=
             tunables.largeHeapIncrementalLimit());

  double factor = LinearInterpolate(
      retainedBytes, tunables.smallHeapSizeMaxBytes(),
      tunables.smallHeapIncrementalLimit(), tunables.largeHeapSizeMinBytes(),
      tunables.largeHeapIncrementalLimit());

  uint64_t bytes =
      std::max(uint64_t(double(startBytes_) * factor),
               uint64_t(startBytes_) + tunables.gcMaxNurseryBytes());
  incrementalLimitBytes_ = ToClampedSize(bytes);
  MOZ_ASSERT(incrementalLimitBytes_ >= startBytes_);
}

double HeapThreshold::eagerAllocTrigger(bool highFrequencyGC) const {
  double eagerTriggerFactor = highFrequencyGC
                                  ? HighFrequencyEagerAllocTriggerFactor
                                  : LowFrequencyEagerAllocTriggerFactor;
  return eagerTriggerFactor * startBytes();
}

void HeapThreshold::setSliceThreshold(ZoneAllocator* zone,
                                      const HeapSize& heapSize,
                                      const GCSchedulingTunables& tunables) {
  sliceBytes_ = ToClampedSize(
      std::min(uint64_t(heapSize.bytes()) + tunables.zoneAllocDelayBytes(),
               uint64_t(incrementalLimitBytes_)));
}

/* static */
double GCHeapThreshold::computeZoneHeapGrowthFactorForHeapSize(
    size_t lastBytes, const GCSchedulingTunables& tunables,
    const GCSchedulingState& state) {
  // For small zones, our collection heuristics do not matter much: favor
  // something simple in this case.
  if (lastBytes < 1 * 1024 * 1024) {
    return tunables.lowFrequencyHeapGrowth();
  }

  // The heap growth factor depends on the heap size after a GC and the GC
  // frequency. If GC's are not triggering in rapid succession, use a lower
  // threshold so that we will collect garbage sooner.
  if (!state.inHighFrequencyGCMode()) {
    return tunables.lowFrequencyHeapGrowth();
  }

  // For high frequency GCs we let the heap grow depending on whether we
  // classify the heap as small, medium or large. There are parameters for small
  // and large heap sizes and linear interpolation is used between them for
  // medium sized heaps.

  MOZ_ASSERT(tunables.smallHeapSizeMaxBytes() <=
             tunables.largeHeapSizeMinBytes());
  MOZ_ASSERT(tunables.highFrequencyLargeHeapGrowth() <=
             tunables.highFrequencySmallHeapGrowth());

  return LinearInterpolate(lastBytes, tunables.smallHeapSizeMaxBytes(),
                           tunables.highFrequencySmallHeapGrowth(),
                           tunables.largeHeapSizeMinBytes(),
                           tunables.highFrequencyLargeHeapGrowth());
}

/* static */
size_t GCHeapThreshold::computeZoneTriggerBytes(
    double growthFactor, size_t lastBytes, JS::GCOptions options,
    const GCSchedulingTunables& tunables, const AutoLockGC& lock) {
  size_t baseMin = options == JS::GCOptions::Shrink
                       ? tunables.minEmptyChunkCount(lock) * ChunkSize
                       : tunables.gcZoneAllocThresholdBase();
  size_t base = std::max(lastBytes, baseMin);
  double trigger = double(base) * growthFactor;
  double triggerMax =
      double(tunables.gcMaxBytes()) / tunables.largeHeapIncrementalLimit();
  return ToClampedSize(std::min(triggerMax, trigger));
}

void GCHeapThreshold::updateStartThreshold(size_t lastBytes,
                                           JS::GCOptions options,
                                           const GCSchedulingTunables& tunables,
                                           const GCSchedulingState& state,
                                           bool isAtomsZone,
                                           const AutoLockGC& lock) {
  double growthFactor =
      computeZoneHeapGrowthFactorForHeapSize(lastBytes, tunables, state);

  // Discourage collection of the atoms zone during page load as this can block
  // off-thread parsing.
  if (isAtomsZone && state.inPageLoad) {
    growthFactor *= 1.5;
  }

  startBytes_ =
      computeZoneTriggerBytes(growthFactor, lastBytes, options, tunables, lock);

  setIncrementalLimitFromStartBytes(lastBytes, tunables);
}

/* static */
size_t MallocHeapThreshold::computeZoneTriggerBytes(double growthFactor,
                                                    size_t lastBytes,
                                                    size_t baseBytes,
                                                    const AutoLockGC& lock) {
  return ToClampedSize(double(std::max(lastBytes, baseBytes)) * growthFactor);
}

void MallocHeapThreshold::updateStartThreshold(
    size_t lastBytes, const GCSchedulingTunables& tunables,
    const AutoLockGC& lock) {
  startBytes_ =
      computeZoneTriggerBytes(tunables.mallocGrowthFactor(), lastBytes,
                              tunables.mallocThresholdBase(), lock);

  setIncrementalLimitFromStartBytes(lastBytes, tunables);
}

#ifdef DEBUG

void MemoryTracker::adopt(MemoryTracker& other) {
  LockGuard<Mutex> lock(mutex);

  AutoEnterOOMUnsafeRegion oomUnsafe;

  for (auto r = other.gcMap.all(); !r.empty(); r.popFront()) {
    if (!gcMap.put(r.front().key(), r.front().value())) {
      oomUnsafe.crash("MemoryTracker::adopt");
    }
  }
  other.gcMap.clear();

  // There may still be ZoneAllocPolicies associated with the old zone since
  // some are not destroyed until the zone itself dies. Instead check there is
  // no memory associated with them and clear their zone pointer in debug builds
  // to catch further memory association.
  for (auto r = other.nonGCMap.all(); !r.empty(); r.popFront()) {
    MOZ_ASSERT(r.front().value() == 0);
    if (r.front().key().use() == MemoryUse::ZoneAllocPolicy) {
      auto policy = static_cast<ZoneAllocPolicy*>(r.front().key().ptr());
      policy->zone_ = nullptr;
    }
  }
  other.nonGCMap.clear();
}

static const char* MemoryUseName(MemoryUse use) {
  switch (use) {
#  define DEFINE_CASE(Name) \
    case MemoryUse::Name:   \
      return #Name;
    JS_FOR_EACH_MEMORY_USE(DEFINE_CASE)
#  undef DEFINE_CASE
  }

  MOZ_CRASH("Unknown memory use");
}

MemoryTracker::MemoryTracker() : mutex(mutexid::MemoryTracker) {}

void MemoryTracker::checkEmptyOnDestroy() {
  bool ok = true;

  if (!gcMap.empty()) {
    ok = false;
    fprintf(stderr, "Missing calls to JS::RemoveAssociatedMemory:\n");
    for (auto r = gcMap.all(); !r.empty(); r.popFront()) {
      fprintf(stderr, "  %p 0x%zx %s\n", r.front().key().ptr(),
              r.front().value(), MemoryUseName(r.front().key().use()));
    }
  }

  if (!nonGCMap.empty()) {
    ok = false;
    fprintf(stderr, "Missing calls to Zone::decNonGCMemory:\n");
    for (auto r = nonGCMap.all(); !r.empty(); r.popFront()) {
      fprintf(stderr, "  %p 0x%zx\n", r.front().key().ptr(), r.front().value());
    }
  }

  MOZ_ASSERT(ok);
}

/* static */
inline bool MemoryTracker::isGCMemoryUse(MemoryUse use) {
  // Most memory uses are for memory associated with GC things but some are for
  // memory associated with non-GC thing pointers.
  return !isNonGCMemoryUse(use);
}

/* static */
inline bool MemoryTracker::isNonGCMemoryUse(MemoryUse use) {
  return use == MemoryUse::ZoneAllocPolicy;
}

/* static */
inline bool MemoryTracker::allowMultipleAssociations(MemoryUse use) {
  // For most uses only one association is possible for each GC thing. Allow a
  // one-to-many relationship only where necessary.
  return isNonGCMemoryUse(use) || use == MemoryUse::RegExpSharedBytecode ||
         use == MemoryUse::BreakpointSite || use == MemoryUse::Breakpoint ||
         use == MemoryUse::ForOfPICStub || use == MemoryUse::ICUObject;
}

void MemoryTracker::trackGCMemory(Cell* cell, size_t nbytes, MemoryUse use) {
  MOZ_ASSERT(cell->isTenured());
  MOZ_ASSERT(isGCMemoryUse(use));

  LockGuard<Mutex> lock(mutex);

  Key<Cell> key{cell, use};
  AutoEnterOOMUnsafeRegion oomUnsafe;
  auto ptr = gcMap.lookupForAdd(key);
  if (ptr) {
    if (!allowMultipleAssociations(use)) {
      MOZ_CRASH_UNSAFE_PRINTF("Association already present: %p 0x%zx %s", cell,
                              nbytes, MemoryUseName(use));
    }
    ptr->value() += nbytes;
    return;
  }

  if (!gcMap.add(ptr, key, nbytes)) {
    oomUnsafe.crash("MemoryTracker::trackGCMemory");
  }
}

void MemoryTracker::untrackGCMemory(Cell* cell, size_t nbytes, MemoryUse use) {
  MOZ_ASSERT(cell->isTenured());

  LockGuard<Mutex> lock(mutex);

  Key<Cell> key{cell, use};
  auto ptr = gcMap.lookup(key);
  if (!ptr) {
    MOZ_CRASH_UNSAFE_PRINTF("Association not found: %p 0x%zx %s", cell, nbytes,
                            MemoryUseName(use));
  }

  if (!allowMultipleAssociations(use) && ptr->value() != nbytes) {
    MOZ_CRASH_UNSAFE_PRINTF(
        "Association for %p %s has different size: "
        "expected 0x%zx but got 0x%zx",
        cell, MemoryUseName(use), ptr->value(), nbytes);
  }

  if (nbytes > ptr->value()) {
    MOZ_CRASH_UNSAFE_PRINTF(
        "Association for %p %s size is too large: "
        "expected at most 0x%zx but got 0x%zx",
        cell, MemoryUseName(use), ptr->value(), nbytes);
  }

  ptr->value() -= nbytes;

  if (ptr->value() == 0) {
    gcMap.remove(ptr);
  }
}

void MemoryTracker::swapGCMemory(Cell* a, Cell* b, MemoryUse use) {
  MOZ_ASSERT(a->isTenured());
  MOZ_ASSERT(b->isTenured());

  Key<Cell> ka{a, use};
  Key<Cell> kb{b, use};

  LockGuard<Mutex> lock(mutex);

  size_t sa = getAndRemoveEntry(ka, lock);
  size_t sb = getAndRemoveEntry(kb, lock);

  AutoEnterOOMUnsafeRegion oomUnsafe;

  if ((sa && !gcMap.put(kb, sa)) || (sb && !gcMap.put(ka, sb))) {
    oomUnsafe.crash("MemoryTracker::swapGCMemory");
  }
}

size_t MemoryTracker::getAndRemoveEntry(const Key<Cell>& key,
                                        LockGuard<Mutex>& lock) {
  auto ptr = gcMap.lookup(key);
  if (!ptr) {
    return 0;
  }

  size_t size = ptr->value();
  gcMap.remove(ptr);
  return size;
}

void MemoryTracker::registerNonGCMemory(void* mem, MemoryUse use) {
  LockGuard<Mutex> lock(mutex);

  Key<void> key{mem, use};
  auto ptr = nonGCMap.lookupForAdd(key);
  if (ptr) {
    MOZ_CRASH_UNSAFE_PRINTF("%s assocaition %p already registered",
                            MemoryUseName(use), mem);
  }

  AutoEnterOOMUnsafeRegion oomUnsafe;
  if (!nonGCMap.add(ptr, key, 0)) {
    oomUnsafe.crash("MemoryTracker::registerNonGCMemory");
  }
}

void MemoryTracker::unregisterNonGCMemory(void* mem, MemoryUse use) {
  LockGuard<Mutex> lock(mutex);

  Key<void> key{mem, use};
  auto ptr = nonGCMap.lookup(key);
  if (!ptr) {
    MOZ_CRASH_UNSAFE_PRINTF("%s association %p not found", MemoryUseName(use),
                            mem);
  }

  if (ptr->value() != 0) {
    MOZ_CRASH_UNSAFE_PRINTF(
        "%s association %p still has 0x%zx bytes associated",
        MemoryUseName(use), mem, ptr->value());
  }

  nonGCMap.remove(ptr);
}

void MemoryTracker::moveNonGCMemory(void* dst, void* src, MemoryUse use) {
  LockGuard<Mutex> lock(mutex);

  Key<void> srcKey{src, use};
  auto srcPtr = nonGCMap.lookup(srcKey);
  if (!srcPtr) {
    MOZ_CRASH_UNSAFE_PRINTF("%s association %p not found", MemoryUseName(use),
                            src);
  }

  size_t nbytes = srcPtr->value();
  nonGCMap.remove(srcPtr);

  Key<void> dstKey{dst, use};
  auto dstPtr = nonGCMap.lookupForAdd(dstKey);
  if (dstPtr) {
    MOZ_CRASH_UNSAFE_PRINTF("%s %p already registered", MemoryUseName(use),
                            dst);
  }

  AutoEnterOOMUnsafeRegion oomUnsafe;
  if (!nonGCMap.add(dstPtr, dstKey, nbytes)) {
    oomUnsafe.crash("MemoryTracker::moveNonGCMemory");
  }
}

void MemoryTracker::incNonGCMemory(void* mem, size_t nbytes, MemoryUse use) {
  MOZ_ASSERT(isNonGCMemoryUse(use));

  LockGuard<Mutex> lock(mutex);

  Key<void> key{mem, use};
  auto ptr = nonGCMap.lookup(key);
  if (!ptr) {
    MOZ_CRASH_UNSAFE_PRINTF("%s allocation %p not found", MemoryUseName(use),
                            mem);
  }

  ptr->value() += nbytes;
}

void MemoryTracker::decNonGCMemory(void* mem, size_t nbytes, MemoryUse use) {
  MOZ_ASSERT(isNonGCMemoryUse(use));

  LockGuard<Mutex> lock(mutex);

  Key<void> key{mem, use};
  auto ptr = nonGCMap.lookup(key);
  if (!ptr) {
    MOZ_CRASH_UNSAFE_PRINTF("%s allocation %p not found", MemoryUseName(use),
                            mem);
  }

  size_t& value = ptr->value();
  if (nbytes > value) {
    MOZ_CRASH_UNSAFE_PRINTF(
        "%s allocation %p is too large: "
        "expected at most 0x%zx but got 0x%zx bytes",
        MemoryUseName(use), mem, value, nbytes);
  }

  value -= nbytes;
}

void MemoryTracker::fixupAfterMovingGC() {
  // Update the table after we move GC things. We don't use MovableCellHasher
  // because that would create a difference between debug and release builds.
  for (GCMap::Enum e(gcMap); !e.empty(); e.popFront()) {
    const auto& key = e.front().key();
    Cell* cell = key.ptr();
    if (cell->isForwarded()) {
      cell = gc::RelocationOverlay::fromCell(cell)->forwardingAddress();
      e.rekeyFront(Key<Cell>{cell, key.use()});
    }
  }
}

template <typename Ptr>
inline MemoryTracker::Key<Ptr>::Key(Ptr* ptr, MemoryUse use)
    : ptr_(uint64_t(ptr)), use_(uint64_t(use)) {
#  ifdef JS_64BIT
  static_assert(sizeof(Key) == 8,
                "MemoryTracker::Key should be packed into 8 bytes");
#  endif
  MOZ_ASSERT(this->ptr() == ptr);
  MOZ_ASSERT(this->use() == use);
}

template <typename Ptr>
inline Ptr* MemoryTracker::Key<Ptr>::ptr() const {
  return reinterpret_cast<Ptr*>(ptr_);
}
template <typename Ptr>
inline MemoryUse MemoryTracker::Key<Ptr>::use() const {
  return static_cast<MemoryUse>(use_);
}

template <typename Ptr>
inline HashNumber MemoryTracker::Hasher<Ptr>::hash(const Lookup& l) {
  return mozilla::HashGeneric(DefaultHasher<Ptr*>::hash(l.ptr()),
                              DefaultHasher<unsigned>::hash(unsigned(l.use())));
}

template <typename Ptr>
inline bool MemoryTracker::Hasher<Ptr>::match(const KeyT& k, const Lookup& l) {
  return k.ptr() == l.ptr() && k.use() == l.use();
}

template <typename Ptr>
inline void MemoryTracker::Hasher<Ptr>::rekey(KeyT& k, const KeyT& newKey) {
  k = newKey;
}

#endif  // DEBUG
