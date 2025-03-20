/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_TimeZone_h
#define builtin_temporal_TimeZone_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/EnumSet.h"

#include <stddef.h>
#include <stdint.h>

#include "builtin/temporal/Wrapped.h"
#include "js/GCVector.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "vm/JSObject.h"
#include "vm/NativeObject.h"

class JSLinearString;
class JS_PUBLIC_API JSTracer;
struct JSClassOps;

namespace js {
struct ClassSpec;
}

namespace mozilla::intl {
class TimeZone;
}

namespace js::temporal {

class TimeZoneObjectMaybeBuiltin : public NativeObject {
 public:
  static constexpr uint32_t IDENTIFIER_SLOT = 0;
  static constexpr uint32_t OFFSET_MINUTES_SLOT = 1;
  static constexpr uint32_t INTL_TIMEZONE_SLOT = 2;
  static constexpr uint32_t SLOT_COUNT = 3;

  // Estimated memory use for intl::TimeZone (see IcuMemoryUsage).
  static constexpr size_t EstimatedMemoryUse = 6840;

  JSString* identifier() const {
    return getFixedSlot(IDENTIFIER_SLOT).toString();
  }

  const auto& offsetMinutes() const {
    return getFixedSlot(OFFSET_MINUTES_SLOT);
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

 protected:
  static void finalize(JS::GCContext* gcx, JSObject* obj);
};

class TimeZoneObject : public TimeZoneObjectMaybeBuiltin {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

 private:
  static const JSClassOps classOps_;
  static const ClassSpec classSpec_;
};

class BuiltinTimeZoneObject : public TimeZoneObjectMaybeBuiltin {
 public:
  static const JSClass class_;

 private:
  static const JSClassOps classOps_;
};

} /* namespace js::temporal */

template <>
inline bool JSObject::is<js::temporal::TimeZoneObjectMaybeBuiltin>() const {
  return is<js::temporal::TimeZoneObject>() ||
         is<js::temporal::BuiltinTimeZoneObject>();
}

namespace js::temporal {

/**
 * Temporal time zones can be either objects or strings. Objects are either
 * instances of `Temporal.TimeZone` or user-defined time zones. Strings are
 * either canonical time zone identifiers or time zone offset strings.
 *
 * Examples of valid Temporal time zones:
 * - Any object
 * - "UTC"
 * - "America/New_York"
 * - "+00:00"
 *
 * Examples of invalid Temporal time zones:
 * - Number values
 * - "utc" (wrong case)
 * - "Etc/UTC" (canonical name is "UTC")
 * - "+00" (missing minutes part)
 * - "+00:00:00" (sub-minute precision)
 * - "+00:00:01" (sub-minute precision)
 * - "-00:00" (wrong sign for zero offset)
 *
 * String-valued Temporal time zones are an optimization to avoid allocating
 * `Temporal.TimeZone` objects when creating `Temporal.ZonedDateTime` objects.
 * For example `Temporal.ZonedDateTime.from("1970-01-01[UTC]")` doesn't require
 * to allocate a fresh `Temporal.TimeZone` object for the "UTC" time zone.
 *
 * The specification creates new `Temporal.TimeZone` objects whenever any
 * operation is performed on a string-valued Temporal time zone. This newly
 * created object can't be accessed by the user and implementations are expected
 * to optimize away the allocation.
 *
 * The following two implementation approaches are possible:
 *
 * 1. Represent string-valued time zones as JSStrings. Additionally keep a
 *    mapping from JSString to `mozilla::intl::TimeZone` to avoid repeatedly
 *    creating new `mozilla::intl::TimeZone` for time zone operations. Offset
 *    string time zones have to be special cased, because they don't use
 *    `mozilla::intl::TimeZone`. Either detect offset strings by checking the
 *    time zone identifier or store offset strings as the offset in minutes
 *    value to avoid reparsing the offset string again and again.
 * 2. Represent string-valued time zones as `Temporal.TimeZone`-like objects.
 *    These internal `Temporal.TimeZone`-like objects must not be exposed to
 *    user-code.
 *
 * Option 2 is a bit easier to implement, so we use this approach for now.
 */
class MOZ_STACK_CLASS TimeZoneValue final {
  JSObject* object_ = nullptr;

 public:
  /**
   * Default initialize this TimeZoneValue.
   */
  TimeZoneValue() = default;

  /**
   * Initialize this TimeZoneValue with a "string" time zone object.
   */
  explicit TimeZoneValue(BuiltinTimeZoneObject* timeZone) : object_(timeZone) {
    MOZ_ASSERT(isString());
  }

  /**
   * Initialize this TimeZoneValue with an "object" time zone object.
   */
  explicit TimeZoneValue(JSObject* timeZone) : object_(timeZone) {
    MOZ_ASSERT(isObject());
  }

  /**
   * Initialize this TimeZoneValue from a slot Value, which must be either a
   * "string" or "object" time zone object.
   */
  explicit TimeZoneValue(const JS::Value& value) : object_(&value.toObject()) {}

  /**
   * Return true if this TimeZoneValue is not null.
   */
  explicit operator bool() const { return !!object_; }

  /**
   * Return true if this TimeZoneValue is a "string" time zone.
   */
  bool isString() const {
    return object_ && object_->is<BuiltinTimeZoneObject>();
  }

  /**
   * Return true if this TimeZoneValue is an "object" time zone.
   */
  bool isObject() const { return object_ && !isString(); }

  /**
   * Return true if this TimeZoneValue holds a TimeZoneObjectMaybeBuiltin.
   */
  bool isTimeZoneObjectMaybeBuiltin() const {
    return object_ && object_->is<TimeZoneObjectMaybeBuiltin>();
  }

  /**
   * Return this "string" time zone.
   */
  auto* toString() const {
    MOZ_ASSERT(isString());
    return &object_->as<BuiltinTimeZoneObject>();
  }

  /**
   * Return this "object" time zone.
   */
  JSObject* toObject() const {
    MOZ_ASSERT(isObject());
    return object_;
  }

  /**
   * Return the underlying object as a TimeZoneObjectMaybeBuiltin.
   */
  auto* toTimeZoneObjectMaybeBuiltin() const {
    MOZ_ASSERT(isTimeZoneObjectMaybeBuiltin());
    return &object_->as<TimeZoneObjectMaybeBuiltin>();
  }

  /**
   * Return the Value representation of this TimeZoneValue.
   */
  JS::Value toValue() const {
    if (isString()) {
      return JS::StringValue(toString()->identifier());
    }

    MOZ_ASSERT(object_);
    return JS::ObjectValue(*object_);
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

enum class TimeZoneMethod {
  GetOffsetNanosecondsFor,
  GetPossibleInstantsFor,
};

class MOZ_STACK_CLASS TimeZoneRecord final {
  TimeZoneValue receiver_;

  // Null unless non-builtin time zone methods are used.
  JSObject* getOffsetNanosecondsFor_ = nullptr;
  JSObject* getPossibleInstantsFor_ = nullptr;

#ifdef DEBUG
  mozilla::EnumSet<TimeZoneMethod> lookedUp_{};
#endif

 public:
  /**
   * Default initialize this TimeZoneRecord.
   */
  TimeZoneRecord() = default;

  explicit TimeZoneRecord(const TimeZoneValue& receiver)
      : receiver_(receiver) {}

  const auto& receiver() const { return receiver_; }
  auto* getOffsetNanosecondsFor() const { return getOffsetNanosecondsFor_; }
  auto* getPossibleInstantsFor() const { return getPossibleInstantsFor_; }

#ifdef DEBUG
  auto& lookedUp() const { return lookedUp_; }
  auto& lookedUp() { return lookedUp_; }
#endif

  // Helper methods for (Mutable)WrappedPtrOperations.
  auto* receiverDoNotUse() const { return &receiver_; }
  auto* getOffsetNanosecondsForDoNotUse() const {
    return &getOffsetNanosecondsFor_;
  }
  auto* getOffsetNanosecondsForDoNotUse() { return &getOffsetNanosecondsFor_; }
  auto* getPossibleInstantsForDoNotUse() const {
    return &getPossibleInstantsFor_;
  }
  auto* getPossibleInstantsForDoNotUse() { return &getPossibleInstantsFor_; }

  // Trace implementation.
  void trace(JSTracer* trc);
};

struct Instant;
struct ParsedTimeZone;
struct PlainDateTime;
class CalendarValue;
class InstantObject;
class PlainDateTimeObject;
class PlainDateTimeWithCalendar;
enum class TemporalDisambiguation;

/**
 * IsValidTimeZoneName ( timeZone )
 * IsAvailableTimeZoneName ( timeZone )
 */
bool IsValidTimeZoneName(JSContext* cx, JS::Handle<JSString*> timeZone,
                         JS::MutableHandle<JSAtom*> validatedTimeZone);

/**
 * CanonicalizeTimeZoneName ( timeZone )
 */
JSString* CanonicalizeTimeZoneName(JSContext* cx,
                                   JS::Handle<JSLinearString*> timeZone);

/**
 * IsValidTimeZoneName ( timeZone )
 * IsAvailableTimeZoneName ( timeZone )
 * CanonicalizeTimeZoneName ( timeZone )
 */
JSString* ValidateAndCanonicalizeTimeZoneName(JSContext* cx,
                                              JS::Handle<JSString*> timeZone);

/**
 * CreateTemporalTimeZone ( identifier [ , newTarget ] )
 */
BuiltinTimeZoneObject* CreateTemporalTimeZone(JSContext* cx,
                                              JS::Handle<JSString*> identifier);

/**
 * ToTemporalTimeZoneSlotValue ( temporalTimeZoneLike )
 */
bool ToTemporalTimeZone(JSContext* cx,
                        JS::Handle<JS::Value> temporalTimeZoneLike,
                        JS::MutableHandle<TimeZoneValue> result);

/**
 * ToTemporalTimeZoneSlotValue ( temporalTimeZoneLike )
 */
bool ToTemporalTimeZone(JSContext* cx, JS::Handle<ParsedTimeZone> string,
                        JS::MutableHandle<TimeZoneValue> result);

/**
 * ToTemporalTimeZoneObject ( timeZoneSlotValue )
 */
JSObject* ToTemporalTimeZoneObject(JSContext* cx,
                                   JS::Handle<TimeZoneValue> timeZone);

/**
 * ToTemporalTimeZoneIdentifier ( timeZoneSlotValue )
 */
JSString* ToTemporalTimeZoneIdentifier(JSContext* cx,
                                       JS::Handle<TimeZoneValue> timeZone);

/**
 * TimeZoneEquals ( one, two )
 */
bool TimeZoneEquals(JSContext* cx, JS::Handle<JSString*> one,
                    JS::Handle<JSString*> two, bool* equals);

/**
 * TimeZoneEquals ( one, two )
 */
bool TimeZoneEquals(JSContext* cx, JS::Handle<TimeZoneValue> one,
                    JS::Handle<TimeZoneValue> two, bool* equals);

/**
 * GetPlainDateTimeFor ( timeZoneRec, instant, calendar [ ,
 * precalculatedOffsetNanoseconds ] )
 */
PlainDateTimeObject* GetPlainDateTimeFor(JSContext* cx,
                                         JS::Handle<TimeZoneValue> timeZone,
                                         const Instant& instant,
                                         JS::Handle<CalendarValue> calendar);

/**
 * GetPlainDateTimeFor ( timeZoneRec, instant, calendar [ ,
 * precalculatedOffsetNanoseconds ] )
 */
PlainDateTimeObject* GetPlainDateTimeFor(JSContext* cx, const Instant& instant,
                                         JS::Handle<CalendarValue> calendar,
                                         int64_t offsetNanoseconds);

/**
 * GetPlainDateTimeFor ( timeZoneRec, instant, calendar [ ,
 * precalculatedOffsetNanoseconds ] )
 */
PlainDateTime GetPlainDateTimeFor(const Instant& instant,
                                  int64_t offsetNanoseconds);

/**
 * GetPlainDateTimeFor ( timeZoneRec, instant, calendar [ ,
 * precalculatedOffsetNanoseconds ] )
 */
bool GetPlainDateTimeFor(JSContext* cx, JS::Handle<TimeZoneRecord> timeZone,
                         const Instant& instant, PlainDateTime* result);

/**
 * GetPlainDateTimeFor ( timeZoneRec, instant, calendar [ ,
 * precalculatedOffsetNanoseconds ] )
 */
bool GetPlainDateTimeFor(JSContext* cx, JS::Handle<TimeZoneValue> timeZone,
                         const Instant& instant, PlainDateTime* result);

/**
 * GetInstantFor ( timeZoneRec, dateTime, disambiguation )
 */
bool GetInstantFor(JSContext* cx, JS::Handle<TimeZoneValue> timeZone,
                   JS::Handle<PlainDateTimeObject*> dateTime,
                   TemporalDisambiguation disambiguation, Instant* result);

/**
 * GetInstantFor ( timeZoneRec, dateTime, disambiguation )
 */
bool GetInstantFor(JSContext* cx, JS::Handle<TimeZoneRecord> timeZone,
                   JS::Handle<PlainDateTimeWithCalendar> dateTime,
                   TemporalDisambiguation disambiguation, Instant* result);

/**
 * GetInstantFor ( timeZoneRec, dateTime, disambiguation )
 */
bool GetInstantFor(JSContext* cx, JS::Handle<TimeZoneValue> timeZone,
                   JS::Handle<PlainDateTimeWithCalendar> dateTime,
                   TemporalDisambiguation disambiguation, Instant* result);

/**
 * FormatUTCOffsetNanoseconds ( offsetNanoseconds )
 */
JSString* FormatUTCOffsetNanoseconds(JSContext* cx, int64_t offsetNanoseconds);

/**
 * GetOffsetStringFor ( timeZoneRec, instant )
 */
JSString* GetOffsetStringFor(JSContext* cx, JS::Handle<TimeZoneValue> timeZone,
                             const Instant& instant);

/**
 * GetOffsetStringFor ( timeZoneRec, instant )
 */
JSString* GetOffsetStringFor(JSContext* cx, JS::Handle<TimeZoneRecord> timeZone,
                             JS::Handle<Wrapped<InstantObject*>> instant);

/**
 * GetOffsetNanosecondsFor ( timeZoneRec, instant )
 */
bool GetOffsetNanosecondsFor(JSContext* cx, JS::Handle<TimeZoneRecord> timeZone,
                             JS::Handle<Wrapped<InstantObject*>> instant,
                             int64_t* offsetNanoseconds);

/**
 * GetOffsetNanosecondsFor ( timeZoneRec, instant )
 */
bool GetOffsetNanosecondsFor(JSContext* cx, JS::Handle<TimeZoneValue> timeZone,
                             JS::Handle<Wrapped<InstantObject*>> instant,
                             int64_t* offsetNanoseconds);

/**
 * GetOffsetNanosecondsFor ( timeZoneRec, instant )
 */
bool GetOffsetNanosecondsFor(JSContext* cx, JS::Handle<TimeZoneRecord> timeZone,
                             const Instant& instant,
                             int64_t* offsetNanoseconds);

/**
 * GetOffsetNanosecondsFor ( timeZoneRec, instant )
 */
bool GetOffsetNanosecondsFor(JSContext* cx, JS::Handle<TimeZoneValue> timeZone,
                             const Instant& instant,
                             int64_t* offsetNanoseconds);

using InstantVector = JS::StackGCVector<Wrapped<InstantObject*>>;

/**
 * GetPossibleInstantsFor ( timeZoneRec, dateTime )
 */
bool GetPossibleInstantsFor(JSContext* cx, JS::Handle<TimeZoneRecord> timeZone,
                            JS::Handle<PlainDateTimeWithCalendar> dateTime,
                            JS::MutableHandle<InstantVector> list);

/**
 * DisambiguatePossibleInstants ( possibleInstants, timeZoneRec, dateTime,
 * disambiguation )
 */
bool DisambiguatePossibleInstants(
    JSContext* cx, JS::Handle<InstantVector> possibleInstants,
    JS::Handle<TimeZoneRecord> timeZone, const PlainDateTime& dateTime,
    TemporalDisambiguation disambiguation,
    JS::MutableHandle<Wrapped<InstantObject*>> result);

/**
 * CreateTimeZoneMethodsRecord ( timeZone, methods )
 */
bool CreateTimeZoneMethodsRecord(JSContext* cx,
                                 JS::Handle<TimeZoneValue> timeZone,
                                 mozilla::EnumSet<TimeZoneMethod> methods,
                                 JS::MutableHandle<TimeZoneRecord> result);

#ifdef DEBUG
/**
 * TimeZoneMethodsRecordHasLookedUp ( timeZoneRec, methodName )
 */
inline bool TimeZoneMethodsRecordHasLookedUp(const TimeZoneRecord& timeZone,
                                             TimeZoneMethod methodName) {
  // Steps 1-4.
  return timeZone.lookedUp().contains(methodName);
}
#endif

/**
 * TimeZoneMethodsRecordIsBuiltin ( timeZoneRec )
 */
inline bool TimeZoneMethodsRecordIsBuiltin(const TimeZoneRecord& timeZone) {
  // Steps 1-2.
  return timeZone.receiver().isString();
}

// Helper for MutableWrappedPtrOperations.
bool WrapTimeZoneValueObject(JSContext* cx,
                             JS::MutableHandle<JSObject*> timeZone);

} /* namespace js::temporal */

namespace js {

template <typename Wrapper>
class WrappedPtrOperations<temporal::TimeZoneValue, Wrapper> {
  const auto& container() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  explicit operator bool() const { return !!container(); }

  bool isString() const { return container().isString(); }

  bool isObject() const { return container().isObject(); }

  JS::Handle<temporal::BuiltinTimeZoneObject*> toString() const {
    MOZ_ASSERT(container().isString());
    auto h = JS::Handle<JSObject*>::fromMarkedLocation(container().address());
    return h.template as<temporal::BuiltinTimeZoneObject>();
  }

  JS::Handle<JSObject*> toObject() const {
    MOZ_ASSERT(container().isObject());
    return JS::Handle<JSObject*>::fromMarkedLocation(container().address());
  }

  JS::Handle<temporal::TimeZoneObjectMaybeBuiltin*>
  toTimeZoneObjectMaybeBuiltin() const {
    MOZ_ASSERT(container().isTimeZoneObjectMaybeBuiltin());
    auto h = JS::Handle<JSObject*>::fromMarkedLocation(container().address());
    return h.template as<temporal::TimeZoneObjectMaybeBuiltin>();
  }

  JS::Value toValue() const { return container().toValue(); }

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
    MOZ_ASSERT(container().isString() || container().isObject());
    auto mh =
        JS::MutableHandle<JSObject*>::fromMarkedLocation(container().address());
    return temporal::WrapTimeZoneValueObject(cx, mh);
  }
};

template <typename Wrapper>
class WrappedPtrOperations<temporal::TimeZoneRecord, Wrapper> {
  const auto& container() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  JS::Handle<temporal::TimeZoneValue> receiver() const {
    return JS::Handle<temporal::TimeZoneValue>::fromMarkedLocation(
        container().receiverDoNotUse());
  }

  JS::Handle<JSObject*> getOffsetNanosecondsFor() const {
    return JS::Handle<JSObject*>::fromMarkedLocation(
        container().getOffsetNanosecondsForDoNotUse());
  }

  JS::Handle<JSObject*> getPossibleInstantsFor() const {
    return JS::Handle<JSObject*>::fromMarkedLocation(
        container().getPossibleInstantsForDoNotUse());
  }
};

template <typename Wrapper>
class MutableWrappedPtrOperations<temporal::TimeZoneRecord, Wrapper>
    : public WrappedPtrOperations<temporal::TimeZoneRecord, Wrapper> {
  auto& container() { return static_cast<Wrapper*>(this)->get(); }

 public:
  JS::MutableHandle<JSObject*> getOffsetNanosecondsFor() {
    return JS::MutableHandle<JSObject*>::fromMarkedLocation(
        container().getOffsetNanosecondsForDoNotUse());
  }

  JS::MutableHandle<JSObject*> getPossibleInstantsFor() {
    return JS::MutableHandle<JSObject*>::fromMarkedLocation(
        container().getPossibleInstantsForDoNotUse());
  }
};

} /* namespace js */

#endif /* builtin_temporal_TimeZone_h */
