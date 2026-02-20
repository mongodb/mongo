/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_Instant_h
#define builtin_temporal_Instant_h

#include "mozilla/Assertions.h"

#include <stdint.h>

#include "builtin/temporal/TemporalTypes.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "vm/NativeObject.h"

namespace js {
struct ClassSpec;
}

namespace js::temporal {

class InstantObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  static constexpr uint32_t SECONDS_SLOT = 0;
  static constexpr uint32_t NANOSECONDS_SLOT = 1;
  static constexpr uint32_t SLOT_COUNT = 2;

  /**
   * Extract the epoch nanoseconds fields from this ZonedDateTime object.
   */
  EpochNanoseconds epochNanoseconds() const {
    double seconds = getFixedSlot(SECONDS_SLOT).toNumber();
    MOZ_ASSERT(-8'640'000'000'000 <= seconds && seconds <= 8'640'000'000'000);

    int32_t nanoseconds = getFixedSlot(NANOSECONDS_SLOT).toInt32();
    MOZ_ASSERT(0 <= nanoseconds && nanoseconds <= 999'999'999);

    return {{int64_t(seconds), nanoseconds}};
  }

 private:
  static const ClassSpec classSpec_;
};

class Increment;
enum class TemporalUnit;
enum class TemporalRoundingMode;

/**
 * IsValidEpochNanoseconds ( epochNanoseconds )
 */
bool IsValidEpochNanoseconds(const JS::BigInt* epochNanoseconds);

/**
 * IsValidEpochNanoseconds ( epochNanoseconds )
 */
bool IsValidEpochNanoseconds(const EpochNanoseconds& epochNanoseconds);

#ifdef DEBUG
/**
 * Return true if the input is within the valid epoch duration limits.
 */
bool IsValidEpochDuration(const EpochDuration& duration);
#endif

/**
 * Convert a BigInt to epoch nanoseconds. The input must be a valid epoch
 * nanoseconds value.
 */
EpochNanoseconds ToEpochNanoseconds(const JS::BigInt* epochNanoseconds);

/**
 * Convert epoch nanoseconds to a BigInt. The input must be valid epoch
 * nanoseconds.
 */
JS::BigInt* ToBigInt(JSContext* cx, const EpochNanoseconds& epochNanoseconds);

/**
 * CreateTemporalInstant ( epochNanoseconds [ , newTarget ] )
 */
InstantObject* CreateTemporalInstant(JSContext* cx,
                                     const EpochNanoseconds& epochNanoseconds);

/**
 * GetUTCEpochNanoseconds ( isoDateTime )
 */
EpochNanoseconds GetUTCEpochNanoseconds(const ISODateTime& isoDateTime);

/**
 * RoundTemporalInstant ( ns, increment, unit, roundingMode )
 */
EpochNanoseconds RoundTemporalInstant(const EpochNanoseconds& ns,
                                      Increment increment, TemporalUnit unit,
                                      TemporalRoundingMode roundingMode);

/**
 * AddInstant ( epochNanoseconds, norm )
 */
bool AddInstant(JSContext* cx, const EpochNanoseconds& epochNanoseconds,
                const TimeDuration& duration, EpochNanoseconds* result);

/**
 * DifferenceInstant ( ns1, ns2, roundingIncrement, smallestUnit, roundingMode )
 */
TimeDuration DifferenceInstant(const EpochNanoseconds& ns1,
                               const EpochNanoseconds& ns2,
                               Increment roundingIncrement,
                               TemporalUnit smallestUnit,
                               TemporalRoundingMode roundingMode);

} /* namespace js::temporal */

#endif /* builtin_temporal_Instant_h */
