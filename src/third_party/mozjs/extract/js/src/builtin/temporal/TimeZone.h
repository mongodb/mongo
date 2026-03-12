/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_TimeZone_h
#define builtin_temporal_TimeZone_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include <array>
#include <stddef.h>
#include <stdint.h>

#include "builtin/temporal/TemporalTypes.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "vm/JSObject.h"
#include "vm/NativeObject.h"
#include "vm/StringType.h"

class JS_PUBLIC_API JSTracer;
struct JSClassOps;

namespace mozilla::intl {
class TimeZone;
}

namespace js::temporal {

class TimeZoneObject : public NativeObject {
 public:
  static const JSClass class_;

  static constexpr uint32_t IDENTIFIER_SLOT = 0;
  static constexpr uint32_t PRIMARY_IDENTIFIER_SLOT = 1;
  static constexpr uint32_t OFFSET_MINUTES_SLOT = 2;
  static constexpr uint32_t INTL_TIMEZONE_SLOT = 3;
  static constexpr uint32_t SLOT_COUNT = 4;

  // Estimated memory use for intl::TimeZone (see IcuMemoryUsage).
  static constexpr size_t EstimatedMemoryUse = 6840;

  bool isOffset() const { return getFixedSlot(OFFSET_MINUTES_SLOT).isInt32(); }

  JSLinearString* identifier() const {
    return &getFixedSlot(IDENTIFIER_SLOT).toString()->asLinear();
  }

  JSLinearString* primaryIdentifier() const {
    MOZ_ASSERT(!isOffset());
    return &getFixedSlot(PRIMARY_IDENTIFIER_SLOT).toString()->asLinear();
  }

  int32_t offsetMinutes() const {
    MOZ_ASSERT(isOffset());
    return getFixedSlot(OFFSET_MINUTES_SLOT).toInt32();
  }

  mozilla::intl::TimeZone* getTimeZone() const {
    const auto& slot = getFixedSlot(INTL_TIMEZONE_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return static_cast<mozilla::intl::TimeZone*>(slot.toPrivate());
  }

  void setTimeZone(mozilla::intl::TimeZone* timeZone) {
    setFixedSlot(INTL_TIMEZONE_SLOT, JS::PrivateValue(timeZone));
  }

 private:
  static const JSClassOps classOps_;

  static void finalize(JS::GCContext* gcx, JSObject* obj);
};

} /* namespace js::temporal */

namespace js::temporal {

/**
 * Temporal time zones are either available named time zones or offset time
 * zones.
 *
 * The identifier of an available named time zones is an available named
 * time zone identifier, which is either a primary time zone identifier or a
 * non-primary time zone identifier.
 *
 * The identifier of an offset time zone is an offset time zone identifier.
 *
 * Temporal methods always return the normalized format of a time zone
 * identifier. Available named time zone identifier are always in normalized
 * format.
 *
 * Examples of valid available time zone identifiers in normalized format:
 * - "UTC" (primary identifier)
 * - "Etc/UTC" (non-primary identifier)
 * - "America/New_York" (primary identifier)
 * - "+00:00"
 *
 * Examples of valid available time zone identifiers in non-normalized format:
 * - "+00"
 * - "-00:00"
 *
 * Examples of invalid available time zone identifiers:
 * - "utc" (wrong case)
 * - "+00:00:00" (sub-minute precision)
 * - "+00:00:01" (sub-minute precision)
 *
 * The following two implementation approaches are possible:
 *
 * 1. Represent time zones as JSStrings. Additionally keep a mapping from
 *    JSString to `mozilla::intl::TimeZone` to avoid repeatedly creating new
 *    `mozilla::intl::TimeZone` for time zone operations. Offset string time
 *    zones have to be special cased, because they don't use
 *    `mozilla::intl::TimeZone`. Either detect offset strings by checking the
 *    time zone identifier or store offset strings as the offset in minutes
 *    value to avoid reparsing the offset string again and again.
 * 2. Represent time zones as objects which hold `mozilla::intl::TimeZone` in
 *    an internal slot.
 *
 * Option 2 is a bit easier to implement, so we use this approach for now.
 */
class MOZ_STACK_CLASS TimeZoneValue final {
  TimeZoneObject* object_ = nullptr;

 public:
  /**
   * Default initialize this TimeZoneValue.
   */
  TimeZoneValue() = default;

  /**
   * Initialize this TimeZoneValue with a time zone object.
   */
  explicit TimeZoneValue(TimeZoneObject* timeZone) : object_(timeZone) {
    MOZ_ASSERT(object_);
  }

  /**
   * Initialize this TimeZoneValue from a slot Value.
   */
  explicit TimeZoneValue(const JS::Value& value)
      : object_(&value.toObject().as<TimeZoneObject>()) {}

  /**
   * Return true if this TimeZoneValue is not null.
   */
  explicit operator bool() const { return !!object_; }

  /**
   * Return true if this TimeZoneValue is an offset time zone.
   */
  bool isOffset() const {
    MOZ_ASSERT(object_);
    return object_->isOffset();
  }

  /**
   * Return the offset of an offset time zone.
   */
  auto offsetMinutes() const {
    MOZ_ASSERT(object_);
    return object_->offsetMinutes();
  }

  /**
   * Return the time zone identifier.
   */
  auto* identifier() const {
    MOZ_ASSERT(object_);
    return object_->identifier();
  }

  /**
   * Return the primary time zone identifier of a named time zone.
   */
  auto* primaryIdentifier() const {
    MOZ_ASSERT(object_);
    return object_->primaryIdentifier();
  }

  /**
   * Return the time zone implementation.
   */
  auto* getTimeZone() const {
    MOZ_ASSERT(object_);
    return object_->getTimeZone();
  }

  /**
   * Return the underlying TimeZoneObject.
   */
  auto* toTimeZoneObject() const {
    MOZ_ASSERT(object_);
    return object_;
  }

  /**
   * Return the slot Value representation of this TimeZoneValue.
   */
  JS::Value toSlotValue() const {
    MOZ_ASSERT(object_);
    return JS::ObjectValue(*object_);
  }

  // Helper methods for (Mutable)WrappedPtrOperations.
  auto address() { return &object_; }
  auto address() const { return &object_; }

  // Trace implementation.
  void trace(JSTracer* trc);
};

class PossibleEpochNanoseconds final {
  // GetPossibleEpochNanoseconds can return up-to two elements.
  static constexpr size_t MaxLength = 2;

  std::array<EpochNanoseconds, MaxLength> array_ = {};
  size_t length_ = 0;

  void append(const EpochNanoseconds& epochNs) { array_[length_++] = epochNs; }

 public:
  PossibleEpochNanoseconds() = default;

  explicit PossibleEpochNanoseconds(const EpochNanoseconds& epochNs) {
    append(epochNs);
  }

  explicit PossibleEpochNanoseconds(const EpochNanoseconds& earlier,
                                    const EpochNanoseconds& later) {
    MOZ_ASSERT(earlier <= later);
    append(earlier);
    append(later);
  }

  size_t length() const { return length_; }
  bool empty() const { return length_ == 0; }

  const auto& operator[](size_t i) const { return array_[i]; }

  auto begin() const { return array_.begin(); }
  auto end() const { return array_.begin() + length_; }

  const auto& front() const {
    MOZ_ASSERT(length_ > 0);
    return array_[0];
  }
  const auto& back() const {
    MOZ_ASSERT(length_ > 0);
    return array_[length_ - 1];
  }
};

struct ParsedTimeZone;
enum class TemporalDisambiguation;

/**
 * SystemTimeZoneIdentifier ( )
 */
JSLinearString* SystemTimeZoneIdentifier(JSContext* cx);

/**
 * SystemTimeZoneIdentifier ( )
 */
bool SystemTimeZone(JSContext* cx, JS::MutableHandle<TimeZoneValue> result);

/**
 * ToTemporalTimeZoneIdentifier ( temporalTimeZoneLike )
 */
bool ToTemporalTimeZone(JSContext* cx,
                        JS::Handle<JS::Value> temporalTimeZoneLike,
                        JS::MutableHandle<TimeZoneValue> result);

/**
 * ToTemporalTimeZoneIdentifier ( temporalTimeZoneLike )
 */
bool ToTemporalTimeZone(JSContext* cx, JS::Handle<ParsedTimeZone> string,
                        JS::MutableHandle<TimeZoneValue> result);

/**
 * TimeZoneEquals ( one, two )
 */
bool TimeZoneEquals(const TimeZoneValue& one, const TimeZoneValue& two);

/**
 * GetISODateTimeFor ( timeZone, epochNs )
 */
ISODateTime GetISODateTimeFor(const EpochNanoseconds& epochNs,
                              int64_t offsetNanoseconds);

/**
 * GetISODateTimeFor ( timeZone, epochNs )
 */
bool GetISODateTimeFor(JSContext* cx, JS::Handle<TimeZoneValue> timeZone,
                       const EpochNanoseconds& epochNs, ISODateTime* result);

/**
 * GetEpochNanosecondsFor ( timeZone, isoDateTime, disambiguation )
 */
bool GetEpochNanosecondsFor(JSContext* cx, JS::Handle<TimeZoneValue> timeZone,
                            const ISODateTime& isoDateTime,
                            TemporalDisambiguation disambiguation,
                            EpochNanoseconds* result);

/**
 * GetOffsetNanosecondsFor ( timeZone, epochNs )
 */
bool GetOffsetNanosecondsFor(JSContext* cx, JS::Handle<TimeZoneValue> timeZone,
                             const EpochNanoseconds& epochNs,
                             int64_t* offsetNanoseconds);

/**
 * GetPossibleEpochNanoseconds ( timeZone, isoDateTime )
 */
bool GetPossibleEpochNanoseconds(JSContext* cx,
                                 JS::Handle<TimeZoneValue> timeZone,
                                 const ISODateTime& isoDateTime,
                                 PossibleEpochNanoseconds* result);

/**
 * DisambiguatePossibleEpochNanoseconds ( possibleEpochNs, timeZone,
 * isoDateTime, disambiguation )
 */
bool DisambiguatePossibleEpochNanoseconds(
    JSContext* cx, const PossibleEpochNanoseconds& possibleEpochNs,
    JS::Handle<TimeZoneValue> timeZone, const ISODateTime& isoDateTime,
    TemporalDisambiguation disambiguation, EpochNanoseconds* result);

/**
 * GetNamedTimeZoneNextTransition ( timeZoneIdentifier, epochNanoseconds )
 */
bool GetNamedTimeZoneNextTransition(JSContext* cx,
                                    JS::Handle<TimeZoneValue> timeZone,
                                    const EpochNanoseconds& epochNanoseconds,
                                    mozilla::Maybe<EpochNanoseconds>* result);

/**
 * GetNamedTimeZonePreviousTransition ( timeZoneIdentifier, epochNanoseconds )
 */
bool GetNamedTimeZonePreviousTransition(
    JSContext* cx, JS::Handle<TimeZoneValue> timeZone,
    const EpochNanoseconds& epochNanoseconds,
    mozilla::Maybe<EpochNanoseconds>* result);

/**
 * GetStartOfDay ( timeZone, isoDate )
 */
bool GetStartOfDay(JSContext* cx, JS::Handle<TimeZoneValue> timeZone,
                   const ISODate& isoDate, EpochNanoseconds* result);

// Helper for MutableWrappedPtrOperations.
bool WrapTimeZoneValueObject(JSContext* cx,
                             JS::MutableHandle<TimeZoneObject*> timeZone);

} /* namespace js::temporal */

namespace js {

template <typename Wrapper>
class WrappedPtrOperations<temporal::TimeZoneValue, Wrapper> {
  const auto& container() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  explicit operator bool() const { return !!container(); }

  bool isOffset() const { return container().isOffset(); }

  auto offsetMinutes() const { return container().offsetMinutes(); }

  auto* identifier() const { return container().identifier(); }

  auto* primaryIdentifier() const { return container().primaryIdentifier(); }

  auto* getTimeZone() const { return container().getTimeZone(); }

  JS::Value toSlotValue() const { return container().toSlotValue(); }
};

template <typename Wrapper>
class MutableWrappedPtrOperations<temporal::TimeZoneValue, Wrapper>
    : public WrappedPtrOperations<temporal::TimeZoneValue, Wrapper> {
  auto& container() { return static_cast<Wrapper*>(this)->get(); }

 public:
  /**
   * Wrap the time zone value into the current compartment.
   */
  bool wrap(JSContext* cx) {
    MOZ_ASSERT(container());
    auto mh = JS::MutableHandle<temporal::TimeZoneObject*>::fromMarkedLocation(
        container().address());
    return temporal::WrapTimeZoneValueObject(cx, mh);
  }
};

} /* namespace js */

#endif /* builtin_temporal_TimeZone_h */
