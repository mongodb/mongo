/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Scheduling.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/TimeStamp.h"

#include <algorithm>
#include <cmath>

#include "gc/Memory.h"
#include "gc/Nursery.h"
#include "gc/RelocationOverlay.h"
#include "gc/ZoneAllocator.h"
#include "util/DifferentialTesting.h"
#include "vm/MutexIDs.h"

using namespace js;
using namespace js::gc;

using mozilla::CheckedInt;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;
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

// Limit various parameters to reasonable levels to catch errors.
static constexpr double MaxHeapGrowthFactor = 100;
static constexpr size_t MaxNurseryBytesParam = 128 * 1024 * 1024;

namespace {

// Helper classes to marshal GC parameter values to/from uint32_t.

template <typename T>
struct ConvertGeneric {
  static uint32_t toUint32(T value) {
    static_assert(std::is_arithmetic_v<T>);
    if constexpr (std::is_signed_v<T>) {
      MOZ_ASSERT(value >= 0);
    }
    if constexpr (!std::is_same_v<T, bool> &&
                  std::numeric_limits<T>::max() >
                      std::numeric_limits<uint32_t>::max()) {
      MOZ_ASSERT(value <= UINT32_MAX);
    }
    return uint32_t(value);
  }
  static Maybe<T> fromUint32(uint32_t param) {
    // Currently we use explicit conversion and don't range check.
    return Some(T(param));
  }
};

using ConvertBool = ConvertGeneric<bool>;
using ConvertSize = ConvertGeneric<size_t>;
using ConvertDouble = ConvertGeneric<double>;

struct ConvertTimes100 {
  static uint32_t toUint32(double value) { return uint32_t(value * 100.0); }
  static Maybe<double> fromUint32(uint32_t param) {
    return Some(double(param) / 100.0);
  }
};

struct ConvertNurseryBytes : ConvertSize {
  static Maybe<size_t> fromUint32(uint32_t param) {
    return Some(Nursery::roundSize(param));
  }
};

struct ConvertKB {
  static uint32_t toUint32(size_t value) { return value / 1024; }
  static Maybe<size_t> fromUint32(uint32_t param) {
    // Parameters which represent heap sizes in bytes are restricted to values
    // which can be represented on 32 bit platforms.
    CheckedInt<uint32_t> size = CheckedInt<uint32_t>(param) * 1024;
    return size.isValid() ? Some(size_t(size.value())) : Nothing();
  }
};

struct ConvertMB {
  static uint32_t toUint32(size_t value) { return value / (1024 * 1024); }
  static Maybe<size_t> fromUint32(uint32_t param) {
    // Parameters which represent heap sizes in bytes are restricted to values
    // which can be represented on 32 bit platforms.
    CheckedInt<uint32_t> size = CheckedInt<uint32_t>(param) * 1024 * 1024;
    return size.isValid() ? Some(size_t(size.value())) : Nothing();
  }
};

struct ConvertMillis {
  static uint32_t toUint32(TimeDuration value) {
    return uint32_t(value.ToMilliseconds());
  }
  static Maybe<TimeDuration> fromUint32(uint32_t param) {
    return Some(TimeDuration::FromMilliseconds(param));
  }
};

struct ConvertSeconds {
  static uint32_t toUint32(TimeDuration value) {
    return uint32_t(value.ToSeconds());
  }
  static Maybe<TimeDuration> fromUint32(uint32_t param) {
    return Some(TimeDuration::FromSeconds(param));
  }
};

}  // anonymous namespace

// Helper functions to check GC parameter values

template <typename T>
static bool NoCheck(T value) {
  return true;
}

template <typename T>
static bool CheckNonZero(T value) {
  return value != 0;
}

static bool CheckNurserySize(size_t bytes) {
  return bytes >= SystemPageSize() && bytes <= MaxNurseryBytesParam;
}

static bool CheckHeapGrowth(double growth) {
  return growth >= MinHeapGrowthFactor && growth <= MaxHeapGrowthFactor;
}

static bool CheckIncrementalLimit(double factor) {
  return factor >= 1.0 && factor <= MaxHeapGrowthFactor;
}

static bool CheckNonZeroUnitRange(double value) {
  return value > 0.0 && value <= 100.0;
}

GCSchedulingTunables::GCSchedulingTunables() {
#define INIT_TUNABLE_FIELD(key, type, name, convert, check, default) \
  name##_ = default;                                                 \
  MOZ_ASSERT(check(name##_));
  FOR_EACH_GC_TUNABLE(INIT_TUNABLE_FIELD)
#undef INIT_TUNABLE_FIELD

  checkInvariants();
}

uint32_t GCSchedulingTunables::getParameter(JSGCParamKey key) {
  switch (key) {
#define GET_TUNABLE_FIELD(key, type, name, convert, check, default) \
  case key:                                                         \
    return convert::toUint32(name##_);
    FOR_EACH_GC_TUNABLE(GET_TUNABLE_FIELD)
#undef GET_TUNABLE_FIELD

    default:
      MOZ_CRASH("Unknown parameter key");
  }
}

bool GCSchedulingTunables::setParameter(JSGCParamKey key, uint32_t value) {
  auto guard = mozilla::MakeScopeExit([this] { checkInvariants(); });

  switch (key) {
#define SET_TUNABLE_FIELD(key, type, name, convert, check, default) \
  case key: {                                                       \
    Maybe<type> converted = convert::fromUint32(value);             \
    if (!converted || !check(converted.value())) {                  \
      return false;                                                 \
    }                                                               \
    name##_ = converted.value();                                    \
    break;                                                          \
  }
    FOR_EACH_GC_TUNABLE(SET_TUNABLE_FIELD)
#undef SET_TUNABLE_FIELD

    default:
      MOZ_CRASH("Unknown GC parameter.");
  }

  maintainInvariantsAfterUpdate(key);
  return true;
}

void GCSchedulingTunables::resetParameter(JSGCParamKey key) {
  auto guard = mozilla::MakeScopeExit([this] { checkInvariants(); });

  switch (key) {
#define RESET_TUNABLE_FIELD(key, type, name, convert, check, default) \
  case key:                                                           \
    name##_ = default;                                                \
    MOZ_ASSERT(check(name##_));                                       \
    break;
    FOR_EACH_GC_TUNABLE(RESET_TUNABLE_FIELD)
#undef RESET_TUNABLE_FIELD

    default:
      MOZ_CRASH("Unknown GC parameter.");
  }

  maintainInvariantsAfterUpdate(key);
}

void GCSchedulingTunables::maintainInvariantsAfterUpdate(JSGCParamKey updated) {
  // Check whether a change to parameter |updated| has broken an invariant in
  // relation to another parameter. If it has, adjust that other parameter to
  // restore the invariant.
  switch (updated) {
    case JSGC_MIN_NURSERY_BYTES:
      if (gcMaxNurseryBytes_ < gcMinNurseryBytes_) {
        gcMaxNurseryBytes_ = gcMinNurseryBytes_;
      }
      break;
    case JSGC_MAX_NURSERY_BYTES:
      if (gcMinNurseryBytes_ > gcMaxNurseryBytes_) {
        gcMinNurseryBytes_ = gcMaxNurseryBytes_;
      }
      break;
    case JSGC_SMALL_HEAP_SIZE_MAX:
      if (smallHeapSizeMaxBytes_ >= largeHeapSizeMinBytes_) {
        largeHeapSizeMinBytes_ = smallHeapSizeMaxBytes_ + 1;
      }
      break;
    case JSGC_LARGE_HEAP_SIZE_MIN:
      if (largeHeapSizeMinBytes_ <= smallHeapSizeMaxBytes_) {
        smallHeapSizeMaxBytes_ = largeHeapSizeMinBytes_ - 1;
      }
      break;
    case JSGC_HIGH_FREQUENCY_SMALL_HEAP_GROWTH:
      if (highFrequencySmallHeapGrowth_ < highFrequencyLargeHeapGrowth_) {
        highFrequencyLargeHeapGrowth_ = highFrequencySmallHeapGrowth_;
      }
      break;
    case JSGC_HIGH_FREQUENCY_LARGE_HEAP_GROWTH:
      if (highFrequencyLargeHeapGrowth_ > highFrequencySmallHeapGrowth_) {
        highFrequencySmallHeapGrowth_ = highFrequencyLargeHeapGrowth_;
      }
      break;
    case JSGC_SMALL_HEAP_INCREMENTAL_LIMIT:
      if (smallHeapIncrementalLimit_ < largeHeapIncrementalLimit_) {
        largeHeapIncrementalLimit_ = smallHeapIncrementalLimit_;
      }
      break;
    case JSGC_LARGE_HEAP_INCREMENTAL_LIMIT:
      if (largeHeapIncrementalLimit_ > smallHeapIncrementalLimit_) {
        smallHeapIncrementalLimit_ = largeHeapIncrementalLimit_;
      }
      break;
    default:
      break;
  }
}

void GCSchedulingTunables::checkInvariants() {
  MOZ_ASSERT(gcMinNurseryBytes_ == Nursery::roundSize(gcMinNurseryBytes_));
  MOZ_ASSERT(gcMaxNurseryBytes_ == Nursery::roundSize(gcMaxNurseryBytes_));
  MOZ_ASSERT(gcMinNurseryBytes_ <= gcMaxNurseryBytes_);
  MOZ_ASSERT(gcMinNurseryBytes_ >= SystemPageSize());
  MOZ_ASSERT(gcMaxNurseryBytes_ <= MaxNurseryBytesParam);

  MOZ_ASSERT(largeHeapSizeMinBytes_ > smallHeapSizeMaxBytes_);

  MOZ_ASSERT(lowFrequencyHeapGrowth_ >= MinHeapGrowthFactor);
  MOZ_ASSERT(lowFrequencyHeapGrowth_ <= MaxHeapGrowthFactor);

  MOZ_ASSERT(highFrequencySmallHeapGrowth_ >= MinHeapGrowthFactor);
  MOZ_ASSERT(highFrequencySmallHeapGrowth_ <= MaxHeapGrowthFactor);
  MOZ_ASSERT(highFrequencyLargeHeapGrowth_ <= highFrequencySmallHeapGrowth_);
  MOZ_ASSERT(highFrequencyLargeHeapGrowth_ >= MinHeapGrowthFactor);
  MOZ_ASSERT(highFrequencySmallHeapGrowth_ <= MaxHeapGrowthFactor);

  MOZ_ASSERT(smallHeapIncrementalLimit_ >= largeHeapIncrementalLimit_);
}

void GCSchedulingState::updateHighFrequencyMode(
    const mozilla::TimeStamp& lastGCTime, const mozilla::TimeStamp& currentTime,
    const GCSchedulingTunables& tunables) {
  if (js::SupportDifferentialTesting()) {
    return;
  }

  inHighFrequencyGCMode_ =
      !lastGCTime.IsNull() &&
      lastGCTime + tunables.highFrequencyThreshold() > currentTime;
}

void GCSchedulingState::updateHighFrequencyModeForReason(JS::GCReason reason) {
  // These reason indicate that the embedding isn't triggering GC slices often
  // enough and allocation rate is high.
  if (reason == JS::GCReason::ALLOC_TRIGGER ||
      reason == JS::GCReason::TOO_MUCH_MALLOC) {
    inHighFrequencyGCMode_ = true;
  }
}

static constexpr size_t BytesPerMB = 1024 * 1024;
static constexpr double CollectionRateSmoothingFactor = 0.5;
static constexpr double AllocationRateSmoothingFactor = 0.5;

static double ExponentialMovingAverage(double prevAverage, double newData,
                                       double smoothingFactor) {
  MOZ_ASSERT(smoothingFactor > 0.0 && smoothingFactor <= 1.0);
  return smoothingFactor * newData + (1.0 - smoothingFactor) * prevAverage;
}

void js::ZoneAllocator::updateCollectionRate(
    mozilla::TimeDuration mainThreadGCTime, size_t initialBytesForAllZones) {
  MOZ_ASSERT(initialBytesForAllZones != 0);
  MOZ_ASSERT(gcHeapSize.initialBytes() <= initialBytesForAllZones);

  double zoneFraction =
      double(gcHeapSize.initialBytes()) / double(initialBytesForAllZones);
  double zoneDuration = mainThreadGCTime.ToSeconds() * zoneFraction +
                        perZoneGCTime.ref().ToSeconds();
  double collectionRate =
      double(gcHeapSize.initialBytes()) / (zoneDuration * BytesPerMB);

  if (!smoothedCollectionRate.ref()) {
    smoothedCollectionRate = Some(collectionRate);
  } else {
    double prevRate = smoothedCollectionRate.ref().value();
    smoothedCollectionRate = Some(ExponentialMovingAverage(
        prevRate, collectionRate, CollectionRateSmoothingFactor));
  }
}

void js::ZoneAllocator::updateAllocationRate(TimeDuration mutatorTime) {
  // To get the total size allocated since the last collection we have to
  // take account of how much memory got freed in the meantime.
  size_t freedBytes = gcHeapSize.freedBytes();

  size_t sizeIncludingFreedBytes = gcHeapSize.bytes() + freedBytes;

  MOZ_ASSERT(prevGCHeapSize <= sizeIncludingFreedBytes);
  size_t allocatedBytes = sizeIncludingFreedBytes - prevGCHeapSize;

  double allocationRate =
      double(allocatedBytes) / (mutatorTime.ToSeconds() * BytesPerMB);

  if (!smoothedAllocationRate.ref()) {
    smoothedAllocationRate = Some(allocationRate);
  } else {
    double prevRate = smoothedAllocationRate.ref().value();
    smoothedAllocationRate = Some(ExponentialMovingAverage(
        prevRate, allocationRate, AllocationRateSmoothingFactor));
  }

  gcHeapSize.clearFreedBytes();
  prevGCHeapSize = gcHeapSize.bytes();
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

  double factor = LinearInterpolate(double(retainedBytes),
                                    double(tunables.smallHeapSizeMaxBytes()),
                                    tunables.smallHeapIncrementalLimit(),
                                    double(tunables.largeHeapSizeMinBytes()),
                                    tunables.largeHeapIncrementalLimit());

  uint64_t bytes =
      std::max(uint64_t(double(startBytes_) * factor),
               uint64_t(startBytes_) + tunables.gcMaxNurseryBytes());
  incrementalLimitBytes_ = ToClampedSize(bytes);
  MOZ_ASSERT(incrementalLimitBytes_ >= startBytes_);

  // Maintain the invariant that the slice threshold is always less than the
  // incremental limit when adjusting GC parameters.
  if (hasSliceThreshold() && sliceBytes() > incrementalLimitBytes()) {
    sliceBytes_ = incrementalLimitBytes();
  }
}

size_t HeapThreshold::eagerAllocTrigger(bool highFrequencyGC) const {
  double eagerTriggerFactor = highFrequencyGC
                                  ? HighFrequencyEagerAllocTriggerFactor
                                  : LowFrequencyEagerAllocTriggerFactor;
  return size_t(eagerTriggerFactor * double(startBytes()));
}

void HeapThreshold::setSliceThreshold(ZoneAllocator* zone,
                                      const HeapSize& heapSize,
                                      const GCSchedulingTunables& tunables,
                                      bool waitingOnBGTask) {
  // Set the allocation threshold at which to trigger the a GC slice in an
  // ongoing incremental collection. This is used to ensure progress in
  // allocation heavy code that may not return to the main event loop.
  //
  // The threshold is based on the JSGC_ZONE_ALLOC_DELAY_KB parameter, but this
  // is reduced to increase the slice frequency as we approach the incremental
  // limit, in the hope that we never reach it. If collector is waiting for a
  // background task to complete, don't trigger any slices until we reach the
  // urgent threshold.

  size_t bytesRemaining = incrementalBytesRemaining(heapSize);
  bool isUrgent = bytesRemaining < tunables.urgentThresholdBytes();

  size_t delayBeforeNextSlice = tunables.zoneAllocDelayBytes();
  if (isUrgent) {
    double fractionRemaining =
        double(bytesRemaining) / double(tunables.urgentThresholdBytes());
    delayBeforeNextSlice =
        size_t(double(delayBeforeNextSlice) * fractionRemaining);
    MOZ_ASSERT(delayBeforeNextSlice <= tunables.zoneAllocDelayBytes());
  } else if (waitingOnBGTask) {
    delayBeforeNextSlice = bytesRemaining - tunables.urgentThresholdBytes();
  }

  sliceBytes_ = ToClampedSize(
      std::min(uint64_t(heapSize.bytes()) + uint64_t(delayBeforeNextSlice),
               uint64_t(incrementalLimitBytes_)));
}

size_t HeapThreshold::incrementalBytesRemaining(
    const HeapSize& heapSize) const {
  if (heapSize.bytes() >= incrementalLimitBytes_) {
    return 0;
  }

  return incrementalLimitBytes_ - heapSize.bytes();
}

/* static */
double HeapThreshold::computeZoneHeapGrowthFactorForHeapSize(
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

  return LinearInterpolate(double(lastBytes),
                           double(tunables.smallHeapSizeMaxBytes()),
                           tunables.highFrequencySmallHeapGrowth(),
                           double(tunables.largeHeapSizeMinBytes()),
                           tunables.highFrequencyLargeHeapGrowth());
}

/* static */
size_t GCHeapThreshold::computeZoneTriggerBytes(
    double growthFactor, size_t lastBytes,
    const GCSchedulingTunables& tunables) {
  size_t base = std::max(lastBytes, tunables.gcZoneAllocThresholdBase());
  double trigger = double(base) * growthFactor;
  double triggerMax =
      double(tunables.gcMaxBytes()) / tunables.largeHeapIncrementalLimit();
  return ToClampedSize(uint64_t(std::min(triggerMax, trigger)));
}

// Parameters for balanced heap limits computation.

// The W0 parameter. How much memory can be traversed in the minimum collection
// time.
static constexpr double BalancedHeapBaseMB = 5.0;

// The minimum heap limit. Do not constrain the heap to any less than this size.
static constexpr double MinBalancedHeapLimitMB = 10.0;

// The minimum amount of additional space to allow beyond the retained size.
static constexpr double MinBalancedHeadroomMB = 3.0;

// The maximum factor by which to expand the heap beyond the retained size.
static constexpr double MaxHeapGrowth = 3.0;

// The default allocation rate in MB/s allocated by the mutator to use before we
// have an estimate. Used to set the heap limit for zones that have not yet been
// collected.
static constexpr double DefaultAllocationRate = 0.0;

// The s0 parameter. The default collection rate in MB/s to use before we have
// an estimate. Used to set the heap limit for zones that have not yet been
// collected.
static constexpr double DefaultCollectionRate = 200.0;

double GCHeapThreshold::computeBalancedHeapLimit(
    size_t lastBytes, double allocationRate, double collectionRate,
    const GCSchedulingTunables& tunables) {
  MOZ_ASSERT(tunables.balancedHeapLimitsEnabled());

  // Optimal heap limits as described in https://arxiv.org/abs/2204.10455

  double W = double(lastBytes) / BytesPerMB;  // Retained size / MB.
  double W0 = BalancedHeapBaseMB;
  double d = tunables.heapGrowthFactor();  // Rearranged constant 'c'.
  double g = allocationRate;
  double s = collectionRate;
  double f = d * sqrt((W + W0) * (g / s));
  double M = W + std::min(f, MaxHeapGrowth * W);
  M = std::max({MinBalancedHeapLimitMB, W + MinBalancedHeadroomMB, M});

  return M * double(BytesPerMB);
}

void GCHeapThreshold::updateStartThreshold(
    size_t lastBytes, mozilla::Maybe<double> allocationRate,
    mozilla::Maybe<double> collectionRate, const GCSchedulingTunables& tunables,
    const GCSchedulingState& state, bool isAtomsZone) {
  if (!tunables.balancedHeapLimitsEnabled()) {
    double growthFactor =
        computeZoneHeapGrowthFactorForHeapSize(lastBytes, tunables, state);

    startBytes_ = computeZoneTriggerBytes(growthFactor, lastBytes, tunables);
  } else {
    double threshold = computeBalancedHeapLimit(
        lastBytes, allocationRate.valueOr(DefaultAllocationRate),
        collectionRate.valueOr(DefaultCollectionRate), tunables);

    double triggerMax =
        double(tunables.gcMaxBytes()) / tunables.largeHeapIncrementalLimit();

    startBytes_ = ToClampedSize(uint64_t(std::min(triggerMax, threshold)));
  }

  setIncrementalLimitFromStartBytes(lastBytes, tunables);
}

/* static */
size_t MallocHeapThreshold::computeZoneTriggerBytes(double growthFactor,
                                                    size_t lastBytes,
                                                    size_t baseBytes) {
  return ToClampedSize(
      uint64_t(double(std::max(lastBytes, baseBytes)) * growthFactor));
}

void MallocHeapThreshold::updateStartThreshold(
    size_t lastBytes, const GCSchedulingTunables& tunables,
    const GCSchedulingState& state) {
  double growthFactor =
      computeZoneHeapGrowthFactorForHeapSize(lastBytes, tunables, state);

  startBytes_ = computeZoneTriggerBytes(growthFactor, lastBytes,
                                        tunables.mallocThresholdBase());

  setIncrementalLimitFromStartBytes(lastBytes, tunables);
}

#ifdef DEBUG

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
  return use == MemoryUse::TrackedAllocPolicy;
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
  Key<Cell> ka{a, use};
  Key<Cell> kb{b, use};

  LockGuard<Mutex> lock(mutex);

  size_t sa = getAndRemoveEntry(ka, lock);
  size_t sb = getAndRemoveEntry(kb, lock);

  AutoEnterOOMUnsafeRegion oomUnsafe;

  if ((sa && b->isTenured() && !gcMap.put(kb, sa)) ||
      (sb && a->isTenured() && !gcMap.put(ka, sb))) {
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
  // Update the table after we move GC things. We don't use StableCellHasher
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
