/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/TimeZone.h"

#include "mozilla/Array.h"
#include "mozilla/Assertions.h"
#include "mozilla/intl/TimeZone.h"
#include "mozilla/Likely.h"
#include "mozilla/Maybe.h"
#include "mozilla/Range.h"
#include "mozilla/Result.h"
#include "mozilla/Span.h"
#include "mozilla/UniquePtr.h"

#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <utility>

#include "jsnum.h"
#include "jspubtd.h"
#include "jstypes.h"
#include "NamespaceImports.h"

#include "builtin/Array.h"
#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/FormatBuffer.h"
#include "builtin/intl/SharedIntlData.h"
#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/Instant.h"
#include "builtin/temporal/PlainDate.h"
#include "builtin/temporal/PlainDateTime.h"
#include "builtin/temporal/PlainTime.h"
#include "builtin/temporal/Temporal.h"
#include "builtin/temporal/TemporalParser.h"
#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/TemporalUnit.h"
#include "builtin/temporal/Wrapped.h"
#include "builtin/temporal/ZonedDateTime.h"
#include "gc/AllocKind.h"
#include "gc/Barrier.h"
#include "gc/GCContext.h"
#include "gc/GCEnum.h"
#include "gc/Tracer.h"
#include "js/AllocPolicy.h"
#include "js/CallArgs.h"
#include "js/CallNonGenericMethod.h"
#include "js/Class.h"
#include "js/ComparisonOperators.h"
#include "js/Date.h"
#include "js/ErrorReport.h"
#include "js/ForOfIterator.h"
#include "js/friend/ErrorMessages.h"
#include "js/Printer.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/RootingAPI.h"
#include "js/StableStringChars.h"
#include "threading/ProtectedData.h"
#include "vm/ArrayObject.h"
#include "vm/BytecodeUtil.h"
#include "vm/Compartment.h"
#include "vm/DateTime.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/JSAtomState.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"
#include "vm/Runtime.h"
#include "vm/StringType.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/ObjectOperations-inl.h"

using namespace js;
using namespace js::temporal;

static inline bool IsTimeZone(Handle<Value> v) {
  return v.isObject() && v.toObject().is<TimeZoneObject>();
}

void js::temporal::TimeZoneValue::trace(JSTracer* trc) {
  TraceNullableRoot(trc, &object_, "TimeZoneValue::object");
}

void js::temporal::TimeZoneRecord::trace(JSTracer* trc) {
  receiver_.trace(trc);
  TraceNullableRoot(trc, &getOffsetNanosecondsFor_,
                    "TimeZoneMethods::getOffsetNanosecondsFor");
  TraceNullableRoot(trc, &getPossibleInstantsFor_,
                    "TimeZoneMethods::getPossibleInstantsFor");
}

static mozilla::UniquePtr<mozilla::intl::TimeZone> CreateIntlTimeZone(
    JSContext* cx, JSString* identifier) {
  JS::AutoStableStringChars stableChars(cx);
  if (!stableChars.initTwoByte(cx, identifier)) {
    return nullptr;
  }

  auto result = mozilla::intl::TimeZone::TryCreate(
      mozilla::Some(stableChars.twoByteRange()));
  if (result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return nullptr;
  }
  return result.unwrap();
}

static mozilla::intl::TimeZone* GetOrCreateIntlTimeZone(
    JSContext* cx, Handle<TimeZoneObjectMaybeBuiltin*> timeZone) {
  // Obtain a cached mozilla::intl::TimeZone object.
  if (auto* tz = timeZone->getTimeZone()) {
    return tz;
  }

  auto* tz = CreateIntlTimeZone(cx, timeZone->identifier()).release();
  if (!tz) {
    return nullptr;
  }
  timeZone->setTimeZone(tz);

  intl::AddICUCellMemory(timeZone,
                         TimeZoneObjectMaybeBuiltin::EstimatedMemoryUse);
  return tz;
}

/**
 * IsValidTimeZoneName ( timeZone )
 * IsAvailableTimeZoneName ( timeZone )
 */
bool js::temporal::IsValidTimeZoneName(
    JSContext* cx, Handle<JSString*> timeZone,
    MutableHandle<JSAtom*> validatedTimeZone) {
  intl::SharedIntlData& sharedIntlData = cx->runtime()->sharedIntlData.ref();

  if (!sharedIntlData.validateTimeZoneName(cx, timeZone, validatedTimeZone)) {
    return false;
  }

  if (validatedTimeZone) {
    cx->markAtom(validatedTimeZone);
  }
  return true;
}

/**
 * 6.5.2 CanonicalizeTimeZoneName ( timeZone )
 *
 * Canonicalizes the given IANA time zone name.
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
JSString* js::temporal::CanonicalizeTimeZoneName(
    JSContext* cx, Handle<JSLinearString*> timeZone) {
  // Step 1. (Not applicable, the input is already a valid IANA time zone.)
#ifdef DEBUG
  MOZ_ASSERT(!StringEqualsLiteral(timeZone, "Etc/Unknown"),
             "Invalid time zone");

  Rooted<JSAtom*> checkTimeZone(cx);
  if (!IsValidTimeZoneName(cx, timeZone, &checkTimeZone)) {
    return nullptr;
  }
  MOZ_ASSERT(EqualStrings(timeZone, checkTimeZone),
             "Time zone name not normalized");
#endif

  // Step 2.
  Rooted<JSLinearString*> ianaTimeZone(cx);
  do {
    intl::SharedIntlData& sharedIntlData = cx->runtime()->sharedIntlData.ref();

    // Some time zone names are canonicalized differently by ICU -- handle
    // those first:
    Rooted<JSAtom*> canonicalTimeZone(cx);
    if (!sharedIntlData.tryCanonicalizeTimeZoneConsistentWithIANA(
            cx, timeZone, &canonicalTimeZone)) {
      return nullptr;
    }

    if (canonicalTimeZone) {
      cx->markAtom(canonicalTimeZone);
      ianaTimeZone = canonicalTimeZone;
      break;
    }

    JS::AutoStableStringChars stableChars(cx);
    if (!stableChars.initTwoByte(cx, timeZone)) {
      return nullptr;
    }

    intl::FormatBuffer<char16_t, intl::INITIAL_CHAR_BUFFER_SIZE> buffer(cx);
    auto result = mozilla::intl::TimeZone::GetCanonicalTimeZoneID(
        stableChars.twoByteRange(), buffer);
    if (result.isErr()) {
      intl::ReportInternalError(cx, result.unwrapErr());
      return nullptr;
    }

    ianaTimeZone = buffer.toString(cx);
    if (!ianaTimeZone) {
      return nullptr;
    }
  } while (false);

#ifdef DEBUG
  MOZ_ASSERT(!StringEqualsLiteral(ianaTimeZone, "Etc/Unknown"),
             "Invalid canonical time zone");

  if (!IsValidTimeZoneName(cx, ianaTimeZone, &checkTimeZone)) {
    return nullptr;
  }
  MOZ_ASSERT(EqualStrings(ianaTimeZone, checkTimeZone),
             "Unsupported canonical time zone");
#endif

  // Step 3.
  if (StringEqualsLiteral(ianaTimeZone, "Etc/UTC") ||
      StringEqualsLiteral(ianaTimeZone, "Etc/GMT")) {
    return cx->names().UTC;
  }

  // We don't need to check against "GMT", because ICU uses the tzdata rearguard
  // format, where "GMT" is a link to "Etc/GMT".
  MOZ_ASSERT(!StringEqualsLiteral(ianaTimeZone, "GMT"));

  // Step 4.
  return ianaTimeZone;
}

/**
 * IsValidTimeZoneName ( timeZone )
 * IsAvailableTimeZoneName ( timeZone )
 * CanonicalizeTimeZoneName ( timeZone )
 */
JSString* js::temporal::ValidateAndCanonicalizeTimeZoneName(
    JSContext* cx, Handle<JSString*> timeZone) {
  Rooted<JSAtom*> validatedTimeZone(cx);
  if (!IsValidTimeZoneName(cx, timeZone, &validatedTimeZone)) {
    return nullptr;
  }

  if (!validatedTimeZone) {
    if (auto chars = QuoteString(cx, timeZone)) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_TEMPORAL_TIMEZONE_INVALID_IDENTIFIER,
                               chars.get());
    }
    return nullptr;
  }

  return CanonicalizeTimeZoneName(cx, validatedTimeZone);
}

class EpochInstantList final {
  // GetNamedTimeZoneEpochNanoseconds can return up-to two elements.
  static constexpr size_t MaxLength = 2;

  mozilla::Array<Instant, MaxLength> array_ = {};
  size_t length_ = 0;

 public:
  EpochInstantList() = default;

  size_t length() const { return length_; }

  void append(const Instant& instant) { array_[length_++] = instant; }

  auto& operator[](size_t i) { return array_[i]; }
  const auto& operator[](size_t i) const { return array_[i]; }

  auto begin() const { return array_.begin(); }
  auto end() const { return array_.begin() + length_; }
};

/**
 * GetNamedTimeZoneEpochNanoseconds ( timeZoneIdentifier, year, month, day,
 * hour, minute, second, millisecond, microsecond, nanosecond )
 */
static bool GetNamedTimeZoneEpochNanoseconds(
    JSContext* cx, Handle<TimeZoneObjectMaybeBuiltin*> timeZone,
    const PlainDateTime& dateTime, EpochInstantList& instants) {
  MOZ_ASSERT(timeZone->offsetMinutes().isUndefined());
  MOZ_ASSERT(IsValidISODateTime(dateTime));
  MOZ_ASSERT(ISODateTimeWithinLimits(dateTime));
  MOZ_ASSERT(instants.length() == 0);

  // FIXME: spec issue - assert ISODateTimeWithinLimits instead of
  // IsValidISODate

  int64_t ms = MakeDate(dateTime);

  auto* tz = GetOrCreateIntlTimeZone(cx, timeZone);
  if (!tz) {
    return false;
  }

  auto getOffset = [&](mozilla::intl::TimeZone::LocalOption skippedTime,
                       mozilla::intl::TimeZone::LocalOption repeatedTime,
                       int32_t* offset) {
    auto result = tz->GetUTCOffsetMs(ms, skippedTime, repeatedTime);
    if (result.isErr()) {
      intl::ReportInternalError(cx, result.unwrapErr());
      return false;
    }

    *offset = result.unwrap();
    MOZ_ASSERT(std::abs(*offset) < UnitsPerDay(TemporalUnit::Millisecond));

    return true;
  };

  constexpr auto formerTime = mozilla::intl::TimeZone::LocalOption::Former;
  constexpr auto latterTime = mozilla::intl::TimeZone::LocalOption::Latter;

  int32_t formerOffset;
  if (!getOffset(formerTime, formerTime, &formerOffset)) {
    return false;
  }

  int32_t latterOffset;
  if (!getOffset(latterTime, latterTime, &latterOffset)) {
    return false;
  }

  if (formerOffset == latterOffset) {
    auto instant = GetUTCEpochNanoseconds(
        dateTime, InstantSpan::fromMilliseconds(formerOffset));
    instants.append(instant);
    return true;
  }

  int32_t disambiguationOffset;
  if (!getOffset(formerTime, latterTime, &disambiguationOffset)) {
    return false;
  }

  // Skipped time.
  if (disambiguationOffset == formerOffset) {
    return true;
  }

  // Repeated time.
  for (auto offset : {formerOffset, latterOffset}) {
    auto instant =
        GetUTCEpochNanoseconds(dateTime, InstantSpan::fromMilliseconds(offset));
    instants.append(instant);
  }

  MOZ_ASSERT(instants.length() == 2);

  // Ensure the returned instants are sorted in numerical order.
  if (instants[0] > instants[1]) {
    std::swap(instants[0], instants[1]);
  }

  return true;
}

/**
 * GetNamedTimeZoneOffsetNanoseconds ( timeZoneIdentifier, epochNanoseconds )
 */
static bool GetNamedTimeZoneOffsetNanoseconds(
    JSContext* cx, Handle<TimeZoneObjectMaybeBuiltin*> timeZone,
    const Instant& epochInstant, int64_t* offset) {
  MOZ_ASSERT(timeZone->offsetMinutes().isUndefined());

  // Round down (floor) to the previous full milliseconds.
  int64_t millis = epochInstant.floorToMilliseconds();

  auto* tz = GetOrCreateIntlTimeZone(cx, timeZone);
  if (!tz) {
    return false;
  }

  auto result = tz->GetOffsetMs(millis);
  if (result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return false;
  }

  // FIXME: spec issue - should constrain the range to not exceed 24-hours.
  // https://github.com/tc39/ecma262/issues/3101

  int64_t nanoPerMs = 1'000'000;
  *offset = result.unwrap() * nanoPerMs;
  return true;
}

/**
 * GetNamedTimeZoneNextTransition ( timeZoneIdentifier, epochNanoseconds )
 */
static bool GetNamedTimeZoneNextTransition(JSContext* cx,
                                           Handle<TimeZoneObject*> timeZone,
                                           const Instant& epochInstant,
                                           mozilla::Maybe<Instant>* result) {
  MOZ_ASSERT(timeZone->offsetMinutes().isUndefined());

  // Round down (floor) to the previous full millisecond.
  //
  // IANA has experimental support for transitions at sub-second precision, but
  // the default configuration doesn't enable it, therefore it's safe to round
  // to milliseconds here. In addition to that, ICU also only supports
  // transitions at millisecond precision.
  int64_t millis = epochInstant.floorToMilliseconds();

  auto* tz = GetOrCreateIntlTimeZone(cx, timeZone);
  if (!tz) {
    return false;
  }

  auto next = tz->GetNextTransition(millis);
  if (next.isErr()) {
    intl::ReportInternalError(cx, next.unwrapErr());
    return false;
  }

  auto transition = next.unwrap();
  if (!transition) {
    *result = mozilla::Nothing();
    return true;
  }

  auto transitionInstant = Instant::fromMilliseconds(*transition);
  if (!IsValidEpochInstant(transitionInstant)) {
    *result = mozilla::Nothing();
    return true;
  }

  *result = mozilla::Some(transitionInstant);
  return true;
}

/**
 * GetNamedTimeZonePreviousTransition ( timeZoneIdentifier, epochNanoseconds )
 */
static bool GetNamedTimeZonePreviousTransition(
    JSContext* cx, Handle<TimeZoneObject*> timeZone,
    const Instant& epochInstant, mozilla::Maybe<Instant>* result) {
  MOZ_ASSERT(timeZone->offsetMinutes().isUndefined());

  // Round up (ceil) to the next full millisecond.
  //
  // IANA has experimental support for transitions at sub-second precision, but
  // the default configuration doesn't enable it, therefore it's safe to round
  // to milliseconds here. In addition to that, ICU also only supports
  // transitions at millisecond precision.
  int64_t millis = epochInstant.ceilToMilliseconds();

  auto* tz = GetOrCreateIntlTimeZone(cx, timeZone);
  if (!tz) {
    return false;
  }

  auto previous = tz->GetPreviousTransition(millis);
  if (previous.isErr()) {
    intl::ReportInternalError(cx, previous.unwrapErr());
    return false;
  }

  auto transition = previous.unwrap();
  if (!transition) {
    *result = mozilla::Nothing();
    return true;
  }

  auto transitionInstant = Instant::fromMilliseconds(*transition);
  if (!IsValidEpochInstant(transitionInstant)) {
    *result = mozilla::Nothing();
    return true;
  }

  *result = mozilla::Some(transitionInstant);
  return true;
}

/**
 * FormatOffsetTimeZoneIdentifier ( offsetMinutes [ , style ] )
 */
static JSString* FormatOffsetTimeZoneIdentifier(JSContext* cx,
                                                int32_t offsetMinutes) {
  MOZ_ASSERT(std::abs(offsetMinutes) < UnitsPerDay(TemporalUnit::Minute));

  // Step 1.
  char sign = offsetMinutes >= 0 ? '+' : '-';

  // Step 2.
  int32_t absoluteMinutes = std::abs(offsetMinutes);

  // Step 3.
  int32_t hour = absoluteMinutes / 60;

  // Step 4.
  int32_t minute = absoluteMinutes % 60;

  // Step 5. (Inlined FormatTimeString).
  //
  // Format: "sign hour{2} : minute{2}"
  char result[] = {
      sign, char('0' + (hour / 10)),   char('0' + (hour % 10)),
      ':',  char('0' + (minute / 10)), char('0' + (minute % 10)),
  };

  // Step 6.
  return NewStringCopyN<CanGC>(cx, result, std::size(result));
}

/**
 * CreateTemporalTimeZone ( identifier [ , newTarget ] )
 */
static TimeZoneObject* CreateTemporalTimeZone(JSContext* cx,
                                              const CallArgs& args,
                                              Handle<JSString*> identifier,
                                              Handle<Value> offsetMinutes) {
  MOZ_ASSERT(offsetMinutes.isUndefined() || offsetMinutes.isInt32());
  MOZ_ASSERT_IF(offsetMinutes.isInt32(), std::abs(offsetMinutes.toInt32()) <
                                             UnitsPerDay(TemporalUnit::Minute));

  // Steps 1-2.
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_TimeZone, &proto)) {
    return nullptr;
  }

  auto* timeZone = NewObjectWithClassProto<TimeZoneObject>(cx, proto);
  if (!timeZone) {
    return nullptr;
  }

  // Step 4.a. (Not applicable in our implementation.)

  // Steps 3.a or 4.b.
  timeZone->setFixedSlot(TimeZoneObject::IDENTIFIER_SLOT,
                         StringValue(identifier));

  // Step 3.b or 4.c.
  timeZone->setFixedSlot(TimeZoneObject::OFFSET_MINUTES_SLOT, offsetMinutes);

  // Step 5.
  return timeZone;
}

static BuiltinTimeZoneObject* CreateBuiltinTimeZone(
    JSContext* cx, Handle<JSString*> identifier) {
  // TODO: Implement a built-in time zone object cache.

  auto* object = NewObjectWithGivenProto<BuiltinTimeZoneObject>(cx, nullptr);
  if (!object) {
    return nullptr;
  }

  object->setFixedSlot(BuiltinTimeZoneObject::IDENTIFIER_SLOT,
                       StringValue(identifier));

  object->setFixedSlot(BuiltinTimeZoneObject::OFFSET_MINUTES_SLOT,
                       UndefinedValue());

  return object;
}

static BuiltinTimeZoneObject* CreateBuiltinTimeZone(JSContext* cx,
                                                    int32_t offsetMinutes) {
  // TODO: It's unclear if offset time zones should also be cached. Real world
  // experience will tell if a cache should be added.

  MOZ_ASSERT(std::abs(offsetMinutes) < UnitsPerDay(TemporalUnit::Minute));

  Rooted<JSString*> identifier(
      cx, FormatOffsetTimeZoneIdentifier(cx, offsetMinutes));
  if (!identifier) {
    return nullptr;
  }

  auto* object = NewObjectWithGivenProto<BuiltinTimeZoneObject>(cx, nullptr);
  if (!object) {
    return nullptr;
  }

  object->setFixedSlot(BuiltinTimeZoneObject::IDENTIFIER_SLOT,
                       StringValue(identifier));

  object->setFixedSlot(BuiltinTimeZoneObject::OFFSET_MINUTES_SLOT,
                       Int32Value(offsetMinutes));

  return object;
}

/**
 * CreateTemporalTimeZone ( identifier [ , newTarget ] )
 */
static TimeZoneObject* CreateTemporalTimeZone(
    JSContext* cx, Handle<BuiltinTimeZoneObject*> timeZone) {
  // Steps 1-2.
  auto* object = NewBuiltinClassInstance<TimeZoneObject>(cx);
  if (!object) {
    return nullptr;
  }

  // Step 4.a. (Not applicable in our implementation.)

  // Steps 3.a or 4.b.
  object->setFixedSlot(
      TimeZoneObject::IDENTIFIER_SLOT,
      timeZone->getFixedSlot(BuiltinTimeZoneObject::IDENTIFIER_SLOT));

  // Step 3.b or 4.c.
  object->setFixedSlot(
      TimeZoneObject::OFFSET_MINUTES_SLOT,
      timeZone->getFixedSlot(BuiltinTimeZoneObject::OFFSET_MINUTES_SLOT));

  // Step 5.
  return object;
}

/**
 * CreateTemporalTimeZone ( identifier [ , newTarget ] )
 */
BuiltinTimeZoneObject* js::temporal::CreateTemporalTimeZone(
    JSContext* cx, Handle<JSString*> identifier) {
  return ::CreateBuiltinTimeZone(cx, identifier);
}

/**
 * ToTemporalTimeZoneSlotValue ( temporalTimeZoneLike )
 */
bool js::temporal::ToTemporalTimeZone(JSContext* cx,
                                      Handle<ParsedTimeZone> string,
                                      MutableHandle<TimeZoneValue> result) {
  // Steps 1-3. (Not applicable)

  // Steps 4-5.
  if (string.name()) {
    // Steps 4.a-c. (Not applicable in our implementation.)

    // Steps 4.d-e.
    Rooted<JSString*> timeZoneName(
        cx, ValidateAndCanonicalizeTimeZoneName(cx, string.name()));
    if (!timeZoneName) {
      return false;
    }

    // Steps 4.f and 5.
    auto* obj = ::CreateBuiltinTimeZone(cx, timeZoneName);
    if (!obj) {
      return false;
    }

    result.set(TimeZoneValue(obj));
    return true;
  }

  // Steps 4.b-c and 8.
  auto* obj = ::CreateBuiltinTimeZone(cx, string.offset());
  if (!obj) {
    return false;
  }

  result.set(TimeZoneValue(obj));
  return true;
}

/**
 * ObjectImplementsTemporalTimeZoneProtocol ( object )
 */
static bool ObjectImplementsTemporalTimeZoneProtocol(JSContext* cx,
                                                     Handle<JSObject*> object,
                                                     bool* result) {
  // Step 1. (Not applicable in our implementation.)
  MOZ_ASSERT(!object->canUnwrapAs<TimeZoneObject>(),
             "TimeZone objects handled in the caller");

  // Step 2.
  for (auto key : {
           &JSAtomState::getOffsetNanosecondsFor,
           &JSAtomState::getPossibleInstantsFor,
           &JSAtomState::id,
       }) {
    // Step 2.a.
    bool has;
    if (!HasProperty(cx, object, cx->names().*key, &has)) {
      return false;
    }
    if (!has) {
      *result = false;
      return true;
    }
  }

  // Step 3.
  *result = true;
  return true;
}

/**
 * ToTemporalTimeZoneSlotValue ( temporalTimeZoneLike )
 */
bool js::temporal::ToTemporalTimeZone(JSContext* cx,
                                      Handle<Value> temporalTimeZoneLike,
                                      MutableHandle<TimeZoneValue> result) {
  // Step 1.
  Rooted<Value> timeZoneLike(cx, temporalTimeZoneLike);
  if (timeZoneLike.isObject()) {
    Rooted<JSObject*> obj(cx, &timeZoneLike.toObject());

    // Step 1.b. (Partial)
    if (obj->canUnwrapAs<TimeZoneObject>()) {
      result.set(TimeZoneValue(obj));
      return true;
    }

    // Step 1.a.
    if (auto* zonedDateTime = obj->maybeUnwrapIf<ZonedDateTimeObject>()) {
      result.set(zonedDateTime->timeZone());
      return result.wrap(cx);
    }

    // Step 1.b.
    bool implementsTimeZoneProtocol;
    if (!ObjectImplementsTemporalTimeZoneProtocol(
            cx, obj, &implementsTimeZoneProtocol)) {
      return false;
    }
    if (!implementsTimeZoneProtocol) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_TEMPORAL_INVALID_OBJECT,
                               "Temporal.TimeZone", obj->getClass()->name);
      return false;
    }

    // Step 1.c.
    result.set(TimeZoneValue(obj));
    return true;
  }

  // Step 2.
  if (!timeZoneLike.isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK,
                     timeZoneLike, nullptr, "not a string");
    return false;
  }
  Rooted<JSString*> identifier(cx, timeZoneLike.toString());

  // Step 3.
  Rooted<ParsedTimeZone> timeZoneName(cx);
  if (!ParseTemporalTimeZoneString(cx, identifier, &timeZoneName)) {
    return false;
  }

  // Steps 4-8.
  return ToTemporalTimeZone(cx, timeZoneName, result);
}

/**
 * ToTemporalTimeZoneObject ( timeZoneSlotValue )
 */
JSObject* js::temporal::ToTemporalTimeZoneObject(
    JSContext* cx, Handle<TimeZoneValue> timeZone) {
  // Step 1.
  if (timeZone.isObject()) {
    return timeZone.toObject();
  }

  // Step 2.
  return CreateTemporalTimeZone(cx, timeZone.toString());
}

/**
 * ToTemporalTimeZoneIdentifier ( timeZoneSlotValue )
 */
JSString* js::temporal::ToTemporalTimeZoneIdentifier(
    JSContext* cx, Handle<TimeZoneValue> timeZone) {
  // Step 1.
  if (timeZone.isString()) {
    // Step 1.a. (Not applicable in our implementation.)

    // Step 1.b.
    return timeZone.toString()->identifier();
  }

  // Step 2.
  Rooted<JSObject*> timeZoneObj(cx, timeZone.toObject());
  Rooted<Value> identifier(cx);
  if (!GetProperty(cx, timeZoneObj, timeZoneObj, cx->names().id, &identifier)) {
    return nullptr;
  }

  // Step 3.
  if (!identifier.isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, identifier,
                     nullptr, "not a string");
    return nullptr;
  }

  // Step 4.
  return identifier.toString();
}

static bool TimeZone_getOffsetNanosecondsFor(JSContext* cx, unsigned argc,
                                             Value* vp);

static bool TimeZone_getPossibleInstantsFor(JSContext* cx, unsigned argc,
                                            Value* vp);

/**
 * TimeZoneMethodsRecordLookup ( timeZoneRec, methodName )
 */
static bool TimeZoneMethodsRecordLookup(JSContext* cx,
                                        MutableHandle<TimeZoneRecord> timeZone,
                                        TimeZoneMethod methodName) {
  // Step 1. (Not applicable in our implementation.)

  // Steps 2-4.
  auto object = timeZone.receiver().toObject();

  auto lookup = [&](Handle<PropertyName*> name, JSNative native,
                    MutableHandle<JSObject*> result) {
    auto* method = GetMethod(cx, object, name);
    if (!method) {
      return false;
    }

    // As an optimization we only store the method if the receiver is either
    // a custom time zone object or if the method isn't the default, built-in
    // time zone method.
    if (!object->is<TimeZoneObject>() || !IsNativeFunction(method, native)) {
      result.set(method);
    }
    return true;
  };

  switch (methodName) {
    // Steps 2 and 4.
    case TimeZoneMethod::GetOffsetNanosecondsFor:
      return lookup(cx->names().getOffsetNanosecondsFor,
                    TimeZone_getOffsetNanosecondsFor,
                    timeZone.getOffsetNanosecondsFor());

      // Steps 3 and 4.
    case TimeZoneMethod::GetPossibleInstantsFor:
      return lookup(cx->names().getPossibleInstantsFor,
                    TimeZone_getPossibleInstantsFor,
                    timeZone.getPossibleInstantsFor());
  }

  MOZ_CRASH("invalid time zone method");
}

/**
 * CreateTimeZoneMethodsRecord ( timeZone, methods )
 */
bool js::temporal::CreateTimeZoneMethodsRecord(
    JSContext* cx, Handle<TimeZoneValue> timeZone,
    mozilla::EnumSet<TimeZoneMethod> methods,
    MutableHandle<TimeZoneRecord> result) {
  MOZ_ASSERT(!methods.isEmpty());

  // Step 1.
  result.set(TimeZoneRecord{timeZone});

#ifdef DEBUG
  // Remember the set of looked-up methods for assertions.
  result.get().lookedUp() += methods;
#endif

  // Built-in time zones don't perform observable lookups.
  if (timeZone.isString()) {
    return true;
  }

  // Step 2.
  for (auto method : methods) {
    if (!TimeZoneMethodsRecordLookup(cx, result, method)) {
      return false;
    }
  }

  // Step 3.
  return true;
}

bool js::temporal::WrapTimeZoneValueObject(JSContext* cx,
                                           MutableHandle<JSObject*> timeZone) {
  // First handle the common case when |timeZone| is TimeZoneObjectMaybeBuiltin
  // from the current compartment.
  if (MOZ_LIKELY(timeZone->is<TimeZoneObjectMaybeBuiltin>() &&
                 timeZone->compartment() == cx->compartment())) {
    return true;
  }

  // If it's not a built-in time zone, simply wrap the object into the current
  // compartment.
  auto* unwrappedTimeZone = timeZone->maybeUnwrapIf<BuiltinTimeZoneObject>();
  if (!unwrappedTimeZone) {
    return cx->compartment()->wrap(cx, timeZone);
  }

  // If this is a built-in time zone from a different compartment, create a
  // fresh copy using the current compartment.
  //
  // We create a fresh copy, so we don't have to support the cross-compartment
  // case, which makes detection of "string" time zones easier.

  const auto& offsetMinutes = unwrappedTimeZone->offsetMinutes();
  if (offsetMinutes.isInt32()) {
    auto* obj = CreateBuiltinTimeZone(cx, offsetMinutes.toInt32());
    if (!obj) {
      return false;
    }

    timeZone.set(obj);
    return true;
  }
  MOZ_ASSERT(offsetMinutes.isUndefined());

  Rooted<JSString*> identifier(cx, unwrappedTimeZone->identifier());
  if (!cx->compartment()->wrap(cx, &identifier)) {
    return false;
  }

  auto* obj = ::CreateBuiltinTimeZone(cx, identifier);
  if (!obj) {
    return false;
  }

  timeZone.set(obj);
  return true;
}

/**
 * Temporal.TimeZone.prototype.getOffsetNanosecondsFor ( instant )
 */
static bool BuiltinGetOffsetNanosecondsFor(
    JSContext* cx, Handle<TimeZoneObjectMaybeBuiltin*> timeZone,
    const Instant& instant, int64_t* offsetNanoseconds) {
  // Steps 1-3. (Not applicable.)

  // Step 4.
  if (timeZone->offsetMinutes().isInt32()) {
    int32_t offset = timeZone->offsetMinutes().toInt32();
    MOZ_ASSERT(std::abs(offset) < UnitsPerDay(TemporalUnit::Minute));

    *offsetNanoseconds = int64_t(offset) * ToNanoseconds(TemporalUnit::Minute);
    return true;
  }
  MOZ_ASSERT(timeZone->offsetMinutes().isUndefined());

  // Step 5.
  int64_t offset;
  if (!GetNamedTimeZoneOffsetNanoseconds(cx, timeZone, instant, &offset)) {
    return false;
  }
  MOZ_ASSERT(std::abs(offset) < ToNanoseconds(TemporalUnit::Day));

  *offsetNanoseconds = offset;
  return true;
}

/**
 * GetOffsetNanosecondsFor ( timeZoneRec, instant )
 */
static bool GetOffsetNanosecondsForSlow(JSContext* cx,
                                        Handle<TimeZoneRecord> timeZone,
                                        Handle<Wrapped<InstantObject*>> instant,
                                        int64_t* offsetNanoseconds) {
  // Step 1. (Inlined call to TimeZoneMethodsRecordCall)
  Rooted<Value> fval(cx, ObjectValue(*timeZone.getOffsetNanosecondsFor()));
  auto thisv = timeZone.receiver().toObject();
  Rooted<Value> instantVal(cx, ObjectValue(*instant));
  Rooted<Value> rval(cx);
  if (!Call(cx, fval, thisv, instantVal, &rval)) {
    return false;
  }

  // Step 2. (Not applicable)

  // Step 3.
  if (!rval.isNumber()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, rval,
                     nullptr, "not a number");
    return false;
  }

  // Steps 4-6.
  double num = rval.toNumber();
  if (!IsInteger(num) || std::abs(num) >= ToNanoseconds(TemporalUnit::Day)) {
    ToCStringBuf cbuf;
    const char* numStr = NumberToCString(&cbuf, num);

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_TIMEZONE_NANOS_RANGE, numStr);
    return false;
  }

  // Step 7.
  *offsetNanoseconds = int64_t(num);
  return true;
}

/**
 * GetOffsetNanosecondsFor ( timeZoneRec, instant )
 */
bool js::temporal::GetOffsetNanosecondsFor(
    JSContext* cx, Handle<TimeZoneRecord> timeZone,
    Handle<Wrapped<InstantObject*>> instant, int64_t* offsetNanoseconds) {
  MOZ_ASSERT(TimeZoneMethodsRecordHasLookedUp(
      timeZone, TimeZoneMethod::GetOffsetNanosecondsFor));

  // Step 2. (Reordered)
  auto getOffsetNanosecondsFor = timeZone.getOffsetNanosecondsFor();
  if (!getOffsetNanosecondsFor) {
    auto* unwrapped = instant.unwrap(cx);
    if (!unwrapped) {
      return false;
    }
    auto instant = ToInstant(unwrapped);
    auto builtin = timeZone.receiver().toTimeZoneObjectMaybeBuiltin();

    return BuiltinGetOffsetNanosecondsFor(cx, builtin, instant,
                                          offsetNanoseconds);
  }

  // Steps 1 and 3-7.
  return ::GetOffsetNanosecondsForSlow(cx, timeZone, instant,
                                       offsetNanoseconds);
}

/**
 * GetOffsetNanosecondsFor ( timeZoneRec, instant )
 */
bool js::temporal::GetOffsetNanosecondsFor(
    JSContext* cx, Handle<TimeZoneValue> timeZone,
    Handle<Wrapped<InstantObject*>> instant, int64_t* offsetNanoseconds) {
  Rooted<TimeZoneRecord> timeZoneRec(cx);
  if (!CreateTimeZoneMethodsRecord(cx, timeZone,
                                   {
                                       TimeZoneMethod::GetOffsetNanosecondsFor,
                                   },
                                   &timeZoneRec)) {
    return false;
  }

  return GetOffsetNanosecondsFor(cx, timeZoneRec, instant, offsetNanoseconds);
}

/**
 * GetOffsetNanosecondsFor ( timeZoneRec, instant )
 */
bool js::temporal::GetOffsetNanosecondsFor(JSContext* cx,
                                           Handle<TimeZoneRecord> timeZone,
                                           const Instant& instant,
                                           int64_t* offsetNanoseconds) {
  MOZ_ASSERT(TimeZoneMethodsRecordHasLookedUp(
      timeZone, TimeZoneMethod::GetOffsetNanosecondsFor));

  // Step 2. (Reordered)
  auto getOffsetNanosecondsFor = timeZone.getOffsetNanosecondsFor();
  if (!getOffsetNanosecondsFor) {
    auto builtin = timeZone.receiver().toTimeZoneObjectMaybeBuiltin();
    return BuiltinGetOffsetNanosecondsFor(cx, builtin, instant,
                                          offsetNanoseconds);
  }

  // Steps 1 and 3-7.
  Rooted<InstantObject*> obj(cx, CreateTemporalInstant(cx, instant));
  if (!obj) {
    return false;
  }
  return ::GetOffsetNanosecondsForSlow(cx, timeZone, obj, offsetNanoseconds);
}

/**
 * GetOffsetNanosecondsFor ( timeZoneRec, instant )
 */
bool js::temporal::GetOffsetNanosecondsFor(JSContext* cx,
                                           Handle<TimeZoneValue> timeZone,
                                           const Instant& instant,
                                           int64_t* offsetNanoseconds) {
  Rooted<TimeZoneRecord> timeZoneRec(cx);
  if (!CreateTimeZoneMethodsRecord(cx, timeZone,
                                   {
                                       TimeZoneMethod::GetOffsetNanosecondsFor,
                                   },
                                   &timeZoneRec)) {
    return false;
  }

  return GetOffsetNanosecondsFor(cx, timeZoneRec, instant, offsetNanoseconds);
}

/**
 * FormatUTCOffsetNanoseconds ( offsetNanoseconds )
 */
JSString* js::temporal::FormatUTCOffsetNanoseconds(JSContext* cx,
                                                   int64_t offsetNanoseconds) {
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  // Step 1.
  char sign = offsetNanoseconds >= 0 ? '+' : '-';

  // Step 2.
  int64_t absoluteNanoseconds = std::abs(offsetNanoseconds);

  // Step 6. (Reordered)
  int32_t subSecondNanoseconds = int32_t(absoluteNanoseconds % 1'000'000'000);

  // Step 5. (Reordered)
  int32_t quotient = int32_t(absoluteNanoseconds / 1'000'000'000);
  int32_t second = quotient % 60;

  // Step 4. (Reordered)
  quotient /= 60;
  int32_t minute = quotient % 60;

  // Step 3.
  int32_t hour = quotient / 60;
  MOZ_ASSERT(hour < 24, "time zone offset mustn't exceed 24-hours");

  // Format: "sign hour{2} : minute{2} : second{2} . fractional{9}"
  constexpr size_t maxLength = 1 + 2 + 1 + 2 + 1 + 2 + 1 + 9;
  char result[maxLength];

  size_t n = 0;

  // Steps 7-8. (Inlined FormatTimeString).
  result[n++] = sign;
  result[n++] = char('0' + (hour / 10));
  result[n++] = char('0' + (hour % 10));
  result[n++] = ':';
  result[n++] = char('0' + (minute / 10));
  result[n++] = char('0' + (minute % 10));

  if (second != 0 || subSecondNanoseconds != 0) {
    result[n++] = ':';
    result[n++] = char('0' + (second / 10));
    result[n++] = char('0' + (second % 10));

    if (uint32_t fractional = subSecondNanoseconds) {
      result[n++] = '.';

      uint32_t k = 100'000'000;
      do {
        result[n++] = char('0' + (fractional / k));
        fractional %= k;
        k /= 10;
      } while (fractional);
    }
  }

  MOZ_ASSERT(n <= maxLength);

  // Step 9.
  return NewStringCopyN<CanGC>(cx, result, n);
}

/**
 * GetOffsetStringFor ( timeZoneRec, instant )
 */
JSString* js::temporal::GetOffsetStringFor(JSContext* cx,
                                           Handle<TimeZoneValue> timeZone,
                                           const Instant& instant) {
  // Step 1.
  int64_t offsetNanoseconds;
  if (!GetOffsetNanosecondsFor(cx, timeZone, instant, &offsetNanoseconds)) {
    return nullptr;
  }
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  // Step 2.
  return FormatUTCOffsetNanoseconds(cx, offsetNanoseconds);
}

/**
 * GetOffsetStringFor ( timeZoneRec, instant )
 */
JSString* js::temporal::GetOffsetStringFor(
    JSContext* cx, Handle<TimeZoneRecord> timeZone,
    Handle<Wrapped<InstantObject*>> instant) {
  // Step 1.
  int64_t offsetNanoseconds;
  if (!GetOffsetNanosecondsFor(cx, timeZone, instant, &offsetNanoseconds)) {
    return nullptr;
  }
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  // Step 2.
  return FormatUTCOffsetNanoseconds(cx, offsetNanoseconds);
}

/**
 * TimeZoneEquals ( one, two )
 */
bool js::temporal::TimeZoneEquals(JSContext* cx, Handle<JSString*> one,
                                  Handle<JSString*> two, bool* equals) {
  // Steps 1-3. (Not applicable)

  // Step 4.
  if (!EqualStrings(cx, one, two, equals)) {
    return false;
  }
  if (*equals) {
    return true;
  }

  // Step 5.
  Rooted<ParsedTimeZone> timeZoneOne(cx);
  if (!ParseTimeZoneIdentifier(cx, one, &timeZoneOne)) {
    return false;
  }

  // Step 6.
  Rooted<ParsedTimeZone> timeZoneTwo(cx);
  if (!ParseTimeZoneIdentifier(cx, two, &timeZoneTwo)) {
    return false;
  }

  // Step 7.
  if (timeZoneOne.name() && timeZoneTwo.name()) {
    // Step 7.a.
    Rooted<JSAtom*> validTimeZoneOne(cx);
    if (!IsValidTimeZoneName(cx, timeZoneOne.name(), &validTimeZoneOne)) {
      return false;
    }
    if (!validTimeZoneOne) {
      *equals = false;
      return true;
    }

    // Step 7.b.
    Rooted<JSAtom*> validTimeZoneTwo(cx);
    if (!IsValidTimeZoneName(cx, timeZoneTwo.name(), &validTimeZoneTwo)) {
      return false;
    }
    if (!validTimeZoneTwo) {
      *equals = false;
      return true;
    }

    // Step 7.c and 9.
    Rooted<JSString*> canonicalOne(
        cx, CanonicalizeTimeZoneName(cx, validTimeZoneOne));
    if (!canonicalOne) {
      return false;
    }

    JSString* canonicalTwo = CanonicalizeTimeZoneName(cx, validTimeZoneTwo);
    if (!canonicalTwo) {
      return false;
    }

    return EqualStrings(cx, canonicalOne, canonicalTwo, equals);
  }

  // Step 8.a.
  if (!timeZoneOne.name() && !timeZoneTwo.name()) {
    *equals = (timeZoneOne.offset() == timeZoneTwo.offset());
    return true;
  }

  // Step 9.
  *equals = false;
  return true;
}

/**
 * TimeZoneEquals ( one, two )
 */
bool js::temporal::TimeZoneEquals(JSContext* cx, Handle<TimeZoneValue> one,
                                  Handle<TimeZoneValue> two, bool* equals) {
  // Step 1.
  if (one.isObject() && two.isObject() && one.toObject() == two.toObject()) {
    *equals = true;
    return true;
  }

  // Step 2.
  Rooted<JSString*> timeZoneOne(cx, ToTemporalTimeZoneIdentifier(cx, one));
  if (!timeZoneOne) {
    return false;
  }

  // Step 3.
  Rooted<JSString*> timeZoneTwo(cx, ToTemporalTimeZoneIdentifier(cx, two));
  if (!timeZoneTwo) {
    return false;
  }

  // Steps 4-9.
  return TimeZoneEquals(cx, timeZoneOne, timeZoneTwo, equals);
}

// ES2019 draft rev 0ceb728a1adbffe42b26972a6541fd7f398b1557
// 5.2.5 Mathematical Operations
static inline double PositiveModulo(double dividend, double divisor) {
  MOZ_ASSERT(divisor > 0);
  MOZ_ASSERT(std::isfinite(divisor));

  double result = std::fmod(dividend, divisor);
  if (result < 0) {
    result += divisor;
  }
  return result + (+0.0);
}

/* ES5 15.9.1.10. */
static double HourFromTime(double t) {
  return PositiveModulo(std::floor(t / msPerHour), HoursPerDay);
}

static double MinFromTime(double t) {
  return PositiveModulo(std::floor(t / msPerMinute), MinutesPerHour);
}

static double SecFromTime(double t) {
  return PositiveModulo(std::floor(t / msPerSecond), SecondsPerMinute);
}

static double msFromTime(double t) { return PositiveModulo(t, msPerSecond); }

/**
 * GetISOPartsFromEpoch ( epochNanoseconds )
 */
static PlainDateTime GetISOPartsFromEpoch(const Instant& instant) {
  // TODO: YearFromTime/MonthFromTime/DayFromTime recompute the same values
  // multiple times. Consider adding a new function avoids this.

  // Step 1.
  MOZ_ASSERT(IsValidEpochInstant(instant));

  // Step 2.
  int32_t remainderNs = instant.nanoseconds % 1'000'000;

  // Step 3.
  double epochMilliseconds = double(instant.floorToMilliseconds());

  // Step 4.
  int32_t year = int32_t(JS::YearFromTime(epochMilliseconds));

  // Step 5.
  int32_t month = int32_t(JS::MonthFromTime(epochMilliseconds)) + 1;

  // Step 6.
  int32_t day = int32_t(JS::DayFromTime(epochMilliseconds));

  // Step 7.
  int32_t hour = int32_t(HourFromTime(epochMilliseconds));

  // Step 8.
  int32_t minute = int32_t(MinFromTime(epochMilliseconds));

  // Step 9.
  int32_t second = int32_t(SecFromTime(epochMilliseconds));

  // Step 10.
  int32_t millisecond = int32_t(msFromTime(epochMilliseconds));

  // Step 11.
  int32_t microsecond = remainderNs / 1000;

  // Step 12.
  int32_t nanosecond = remainderNs % 1000;

  // Step 13.
  PlainDateTime result = {
      {year, month, day},
      {hour, minute, second, millisecond, microsecond, nanosecond}};

  // Always valid when the epoch nanoseconds are within the representable limit.
  MOZ_ASSERT(IsValidISODateTime(result));
  MOZ_ASSERT(ISODateTimeWithinLimits(result));

  return result;
}

/**
 * BalanceISODateTime ( year, month, day, hour, minute, second, millisecond,
 * microsecond, nanosecond )
 */
static PlainDateTime BalanceISODateTime(const PlainDateTime& dateTime,
                                        int64_t nanoseconds) {
  MOZ_ASSERT(IsValidISODateTime(dateTime));
  MOZ_ASSERT(ISODateTimeWithinLimits(dateTime));
  MOZ_ASSERT(std::abs(nanoseconds) < ToNanoseconds(TemporalUnit::Day));

  const auto& [date, time] = dateTime;

  // Step 1.
  auto balancedTime = BalanceTime(time, nanoseconds);
  MOZ_ASSERT(std::abs(balancedTime.days) <= 1);

  // Step 2.
  auto balancedDate =
      BalanceISODate(date.year, date.month, date.day + balancedTime.days);

  // Step 3.
  return {balancedDate, balancedTime.time};
}

/**
 * GetPlainDateTimeFor ( timeZoneRec, instant, calendar [ ,
 * precalculatedOffsetNanoseconds ] )
 */
static PlainDateTimeObject* GetPlainDateTimeFor(
    JSContext* cx, Handle<TimeZoneValue> timeZone,
    Handle<Wrapped<InstantObject*>> instant, Handle<CalendarValue> calendar) {
  // Step 1. (Not applicable in our implementation.)

  // Steps 2-3.
  int64_t offsetNanoseconds;
  if (!GetOffsetNanosecondsFor(cx, timeZone, instant, &offsetNanoseconds)) {
    return nullptr;
  }

  // Step 4.
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  auto* unwrappedInstant = instant.unwrap(cx);
  if (!unwrappedInstant) {
    return nullptr;
  }

  // Steps 5-7.
  auto dateTime =
      GetPlainDateTimeFor(ToInstant(unwrappedInstant), offsetNanoseconds);
  MOZ_ASSERT(ISODateTimeWithinLimits(dateTime));

  return CreateTemporalDateTime(cx, dateTime, calendar);
}

/**
 * GetPlainDateTimeFor ( timeZoneRec, instant, calendar [ ,
 * precalculatedOffsetNanoseconds ] )
 */
PlainDateTime js::temporal::GetPlainDateTimeFor(const Instant& instant,
                                                int64_t offsetNanoseconds) {
  // Steps 1-3. (Not applicable)

  // Step 4.
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  // TODO: Steps 5-6 can be combined into a single operation to improve perf.

  // Step 5.
  PlainDateTime dateTime = GetISOPartsFromEpoch(instant);

  // Step 6.
  auto balanced = BalanceISODateTime(dateTime, offsetNanoseconds);
  MOZ_ASSERT(ISODateTimeWithinLimits(balanced));

  // Step 7.
  return balanced;
}

/**
 * GetPlainDateTimeFor ( timeZone, instant, calendar [ ,
 * precalculatedOffsetNanoseconds ] )
 */
bool js::temporal::GetPlainDateTimeFor(JSContext* cx,
                                       Handle<TimeZoneRecord> timeZone,
                                       const Instant& instant,
                                       PlainDateTime* result) {
  MOZ_ASSERT(IsValidEpochInstant(instant));

  // Step 1.
  MOZ_ASSERT(TimeZoneMethodsRecordHasLookedUp(
      timeZone, TimeZoneMethod::GetOffsetNanosecondsFor));

  // Steps 2-3.
  int64_t offsetNanoseconds;
  if (!GetOffsetNanosecondsFor(cx, timeZone, instant, &offsetNanoseconds)) {
    return false;
  }

  // Step 4.
  MOZ_ASSERT(std::abs(offsetNanoseconds) < ToNanoseconds(TemporalUnit::Day));

  // Steps 5-7.
  *result = GetPlainDateTimeFor(instant, offsetNanoseconds);
  return true;
}

/**
 * GetPlainDateTimeFor ( timeZone, instant, calendar [ ,
 * precalculatedOffsetNanoseconds ] )
 */
bool js::temporal::GetPlainDateTimeFor(JSContext* cx,
                                       Handle<TimeZoneValue> timeZone,
                                       const Instant& instant,
                                       PlainDateTime* result) {
  Rooted<TimeZoneRecord> timeZoneRec(cx);
  if (!CreateTimeZoneMethodsRecord(cx, timeZone,
                                   {
                                       TimeZoneMethod::GetOffsetNanosecondsFor,
                                   },
                                   &timeZoneRec)) {
    return false;
  }

  return GetPlainDateTimeFor(cx, timeZoneRec, instant, result);
}

/**
 * GetPlainDateTimeFor ( timeZone, instant, calendar [ ,
 * precalculatedOffsetNanoseconds ] )
 */
PlainDateTimeObject* js::temporal::GetPlainDateTimeFor(
    JSContext* cx, Handle<TimeZoneValue> timeZone, const Instant& instant,
    Handle<CalendarValue> calendar) {
  // Steps 1-6.
  PlainDateTime dateTime;
  if (!GetPlainDateTimeFor(cx, timeZone, instant, &dateTime)) {
    return nullptr;
  }
  MOZ_ASSERT(ISODateTimeWithinLimits(dateTime));

  // Step 7.
  return CreateTemporalDateTime(cx, dateTime, calendar);
}

/**
 * GetPlainDateTimeFor ( timeZone, instant, calendar [ ,
 * precalculatedOffsetNanoseconds ] )
 */
PlainDateTimeObject* js::temporal::GetPlainDateTimeFor(
    JSContext* cx, const Instant& instant, Handle<CalendarValue> calendar,
    int64_t offsetNanoseconds) {
  MOZ_ASSERT(IsValidEpochInstant(instant));

  // Steps 1-6.
  auto dateTime = GetPlainDateTimeFor(instant, offsetNanoseconds);
  MOZ_ASSERT(ISODateTimeWithinLimits(dateTime));

  // Step 7.
  return CreateTemporalDateTime(cx, dateTime, calendar);
}

/**
 * Temporal.TimeZone.prototype.getPossibleInstantsFor ( dateTime )
 */
static bool BuiltinGetPossibleInstantsFor(
    JSContext* cx, Handle<TimeZoneObjectMaybeBuiltin*> timeZone,
    const PlainDateTime& dateTime, EpochInstantList& possibleInstants) {
  MOZ_ASSERT(possibleInstants.length() == 0);

  // Steps 1-3. (Not applicable)

  // Step 4.
  if (timeZone->offsetMinutes().isInt32()) {
    int32_t offsetMin = timeZone->offsetMinutes().toInt32();
    MOZ_ASSERT(std::abs(offsetMin) < UnitsPerDay(TemporalUnit::Minute));

    // Step 4.a.
    auto epochInstant =
        GetUTCEpochNanoseconds(dateTime, InstantSpan::fromMinutes(offsetMin));

    // Step 4.b.
    possibleInstants.append(epochInstant);
  } else {
    // Step 5.
    if (!GetNamedTimeZoneEpochNanoseconds(cx, timeZone, dateTime,
                                          possibleInstants)) {
      return false;
    }
  }

  MOZ_ASSERT(possibleInstants.length() <= 2);

  // Step 7.b.
  for (const auto& epochInstant : possibleInstants) {
    if (!IsValidEpochInstant(epochInstant)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_INSTANT_INVALID);
      return false;
    }
  }

  // Steps 6-8. (Handled in the caller).
  return true;
}

static bool BuiltinGetPossibleInstantsFor(
    JSContext* cx, Handle<TimeZoneObjectMaybeBuiltin*> timeZone,
    const PlainDateTime& dateTime, MutableHandle<InstantVector> list) {
  // Temporal.TimeZone.prototype.getInstantFor, step 4.
  EpochInstantList possibleInstants;
  if (!BuiltinGetPossibleInstantsFor(cx, timeZone, dateTime,
                                     possibleInstants)) {
    return false;
  }

  // Temporal.TimeZone.prototype.getInstantFor, step 7.
  for (const auto& possibleInstant : possibleInstants) {
    auto* instant = CreateTemporalInstant(cx, possibleInstant);
    if (!instant) {
      return false;
    }

    if (!list.append(instant)) {
      return false;
    }
  }
  return true;
}

/**
 * GetPossibleInstantsFor ( timeZoneRec, dateTime )
 */
static bool GetPossibleInstantsForSlow(
    JSContext* cx, Handle<TimeZoneRecord> timeZone,
    Handle<Wrapped<PlainDateTimeObject*>> dateTime,
    MutableHandle<InstantVector> list) {
  MOZ_ASSERT(list.empty());

  // Step 1. (Inlined call to TimeZoneMethodsRecordCall)
  Rooted<Value> fval(cx, ObjectValue(*timeZone.getPossibleInstantsFor()));
  auto thisv = timeZone.receiver().toObject();
  Rooted<Value> arg(cx, ObjectValue(*dateTime));
  Rooted<Value> rval(cx);
  if (!Call(cx, fval, thisv, arg, &rval)) {
    return false;
  }

  // Step 2. (Not applicable)

  // Step 3.
  JS::ForOfIterator iterator(cx);
  if (!iterator.init(rval)) {
    return false;
  }

  // Step 4. (Not applicable in our implementation.)

  // Step 5.
  auto min = Instant::max();
  auto max = Instant::min();
  Rooted<Value> nextValue(cx);
  while (true) {
    // Step 5.a.
    bool done;
    if (!iterator.next(&nextValue, &done)) {
      return false;
    }

    // Step 5.b.
    if (done) {
      // Steps 5.b.i-ii.
      if (list.length() > 1) {
        // Steps 5.b.ii.1-4. (Not applicable in our implementation.)

        // Step 5.b.ii.5.
        constexpr auto nsPerDay =
            InstantSpan::fromNanoseconds(ToNanoseconds(TemporalUnit::Day));
        if ((max - min).abs() > nsPerDay) {
          JS_ReportErrorNumberASCII(
              cx, GetErrorMessage, nullptr,
              JSMSG_TEMPORAL_TIMEZONE_OFFSET_SHIFT_ONE_DAY);
          return false;
        }
      }

      // Step 5.b.iii.
      return true;
    }

    // Step 5.d. (Reordered)
    if (nextValue.isObject()) {
      JSObject* obj = &nextValue.toObject();
      if (auto* unwrapped = obj->maybeUnwrapIf<InstantObject>()) {
        auto instant = ToInstant(unwrapped);
        min = std::min(min, instant);
        max = std::max(max, instant);

        if (!list.append(obj)) {
          return false;
        }
        continue;
      }
    }

    // Step 5.c.1.
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, nextValue,
                     nullptr, "not an instant");

    // Step 5.c.2.
    iterator.closeThrow();
    return false;
  }
}

/**
 * GetPossibleInstantsFor ( timeZoneRec, dateTime )
 */
static bool GetPossibleInstantsFor(
    JSContext* cx, Handle<TimeZoneRecord> timeZone,
    Handle<Wrapped<PlainDateTimeObject*>> dateTimeObj,
    const PlainDateTime& dateTime, MutableHandle<InstantVector> list) {
  MOZ_ASSERT(TimeZoneMethodsRecordHasLookedUp(
      timeZone, TimeZoneMethod::GetPossibleInstantsFor));

  // Step 2. (Reordered)
  auto getPossibleInstantsFor = timeZone.getPossibleInstantsFor();
  if (!getPossibleInstantsFor) {
    bool arrayIterationSane;
    if (timeZone.receiver().isString()) {
      // "String" time zones don't perform observable array iteration.
      arrayIterationSane = true;
    } else {
      // "Object" time zones need to ensure array iteration is still sane.
      if (!IsArrayIterationSane(cx, &arrayIterationSane)) {
        return false;
      }
    }

    if (arrayIterationSane) {
      auto builtin = timeZone.receiver().toTimeZoneObjectMaybeBuiltin();
      return BuiltinGetPossibleInstantsFor(cx, builtin, dateTime, list);
    }
  }

  // Steps 1 and 3-5.
  return GetPossibleInstantsForSlow(cx, timeZone, dateTimeObj, list);
}

/**
 * GetPossibleInstantsFor ( timeZoneRec, dateTime )
 */
bool js::temporal::GetPossibleInstantsFor(
    JSContext* cx, Handle<TimeZoneRecord> timeZone,
    Handle<PlainDateTimeWithCalendar> dateTime,
    MutableHandle<InstantVector> list) {
  // Step 2. (Reordered)
  auto getPossibleInstantsFor = timeZone.getPossibleInstantsFor();
  if (!getPossibleInstantsFor) {
    bool arrayIterationSane;
    if (timeZone.receiver().isString()) {
      // "String" time zones don't perform observable array iteration.
      arrayIterationSane = true;
    } else {
      // "Object" time zones need to ensure array iteration is still sane.
      if (!IsArrayIterationSane(cx, &arrayIterationSane)) {
        return false;
      }
    }

    if (arrayIterationSane) {
      auto builtin = timeZone.receiver().toTimeZoneObjectMaybeBuiltin();
      return BuiltinGetPossibleInstantsFor(cx, builtin,
                                           ToPlainDateTime(dateTime), list);
    }
  }

  Rooted<PlainDateTimeObject*> dateTimeObj(
      cx, CreateTemporalDateTime(cx, ToPlainDateTime(dateTime),
                                 dateTime.calendar()));
  if (!dateTimeObj) {
    return false;
  }

  // Steps 1 and 3-5.
  return GetPossibleInstantsForSlow(cx, timeZone, dateTimeObj, list);
}

/**
 * AddTime ( hour, minute, second, millisecond, microsecond, nanosecond, hours,
 * minutes, seconds, milliseconds, microseconds, nanoseconds )
 */
static auto AddTime(const PlainTime& time, int64_t nanoseconds) {
  MOZ_ASSERT(IsValidTime(time));
  MOZ_ASSERT(std::abs(nanoseconds) <= ToNanoseconds(TemporalUnit::Day));

  // Steps 1-3.
  return BalanceTime(time, nanoseconds);
}

/**
 * DisambiguatePossibleInstants ( possibleInstants, timeZoneRec, dateTime,
 * disambiguation )
 */
bool js::temporal::DisambiguatePossibleInstants(
    JSContext* cx, Handle<InstantVector> possibleInstants,
    Handle<TimeZoneRecord> timeZone, const PlainDateTime& dateTime,
    TemporalDisambiguation disambiguation,
    MutableHandle<Wrapped<InstantObject*>> result) {
  // Step 1.
  MOZ_ASSERT(TimeZoneMethodsRecordHasLookedUp(
      timeZone, TimeZoneMethod::GetPossibleInstantsFor));

  // Step 2.
  MOZ_ASSERT_IF(possibleInstants.empty() &&
                    disambiguation != TemporalDisambiguation::Reject,
                TimeZoneMethodsRecordHasLookedUp(
                    timeZone, TimeZoneMethod::GetOffsetNanosecondsFor));

  // Steps 3-4.
  if (possibleInstants.length() == 1) {
    result.set(possibleInstants[0]);
    return true;
  }

  // Steps 5-6.
  if (!possibleInstants.empty()) {
    // Step 5.a.
    if (disambiguation == TemporalDisambiguation::Earlier ||
        disambiguation == TemporalDisambiguation::Compatible) {
      result.set(possibleInstants[0]);
      return true;
    }

    // Step 5.b.
    if (disambiguation == TemporalDisambiguation::Later) {
      size_t last = possibleInstants.length() - 1;
      result.set(possibleInstants[last]);
      return true;
    }

    // Step 5.c.
    MOZ_ASSERT(disambiguation == TemporalDisambiguation::Reject);

    // Step 5.d.
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_TIMEZONE_INSTANT_AMBIGUOUS);
    return false;
  }

  // Step 7.
  if (disambiguation == TemporalDisambiguation::Reject) {
    // TODO: Improve error message to say the date was skipped.
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_TIMEZONE_INSTANT_AMBIGUOUS);
    return false;
  }

  constexpr auto oneDay =
      InstantSpan::fromNanoseconds(ToNanoseconds(TemporalUnit::Day));

  // Step 8.
  auto epochNanoseconds = GetUTCEpochNanoseconds(dateTime);

  // Steps 9 and 11.
  auto dayBefore = epochNanoseconds - oneDay;

  // Step 10.
  if (!IsValidEpochInstant(dayBefore)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INSTANT_INVALID);
    return false;
  }

  // Step 12 and 14.
  auto dayAfter = epochNanoseconds + oneDay;

  // Step 13.
  if (!IsValidEpochInstant(dayAfter)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INSTANT_INVALID);
    return false;
  }

  // Step 15.
  int64_t offsetBefore;
  if (!GetOffsetNanosecondsFor(cx, timeZone, dayBefore, &offsetBefore)) {
    return false;
  }
  MOZ_ASSERT(std::abs(offsetBefore) < ToNanoseconds(TemporalUnit::Day));

  // Step 16.
  int64_t offsetAfter;
  if (!GetOffsetNanosecondsFor(cx, timeZone, dayAfter, &offsetAfter)) {
    return false;
  }
  MOZ_ASSERT(std::abs(offsetAfter) < ToNanoseconds(TemporalUnit::Day));

  // Step 17.
  int64_t nanoseconds = offsetAfter - offsetBefore;

  // Step 18.
  if (std::abs(nanoseconds) > ToNanoseconds(TemporalUnit::Day)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_TIMEZONE_OFFSET_SHIFT_ONE_DAY);
    return false;
  }

  // Step 19.
  if (disambiguation == TemporalDisambiguation::Earlier) {
    // Steps 19.a-b.
    auto earlierTime = ::AddTime(dateTime.time, -nanoseconds);
    MOZ_ASSERT(std::abs(earlierTime.days) <= 1,
               "subtracting nanoseconds is at most one day");

    // Step 19.c.
    auto earlierDate = BalanceISODate(dateTime.date.year, dateTime.date.month,
                                      dateTime.date.day + earlierTime.days);

    // Step 19.d.
    Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));
    Rooted<PlainDateTimeWithCalendar> earlierDateTime(
        cx,
        PlainDateTimeWithCalendar{{earlierDate, earlierTime.time}, calendar});

    // Step 19.e.
    Rooted<InstantVector> earlierInstants(cx, InstantVector(cx));
    if (!GetPossibleInstantsFor(cx, timeZone, earlierDateTime,
                                &earlierInstants)) {
      return false;
    }

    // Step 19.f.
    if (earlierInstants.empty()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_TIMEZONE_INSTANT_AMBIGUOUS);
      return false;
    }

    // Step 19.g.
    result.set(earlierInstants[0]);
    return true;
  }

  // Step 20.
  MOZ_ASSERT(disambiguation == TemporalDisambiguation::Compatible ||
             disambiguation == TemporalDisambiguation::Later);

  // Steps 21-22.
  auto laterTime = ::AddTime(dateTime.time, nanoseconds);
  MOZ_ASSERT(std::abs(laterTime.days) <= 1,
             "adding nanoseconds is at most one day");

  // Step 23.
  auto laterDate = BalanceISODate(dateTime.date.year, dateTime.date.month,
                                  dateTime.date.day + laterTime.days);

  // Step 24.
  Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));
  Rooted<PlainDateTimeWithCalendar> laterDateTime(
      cx, PlainDateTimeWithCalendar{{laterDate, laterTime.time}, calendar});

  // Step 25.
  Rooted<InstantVector> laterInstants(cx, InstantVector(cx));
  if (!GetPossibleInstantsFor(cx, timeZone, laterDateTime, &laterInstants)) {
    return false;
  }

  // Steps 26-27.
  if (laterInstants.empty()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_TIMEZONE_INSTANT_AMBIGUOUS);
    return false;
  }

  // Step 28.
  size_t last = laterInstants.length() - 1;
  result.set(laterInstants[last]);
  return true;
}

/**
 * GetInstantFor ( timeZoneRec, dateTime, disambiguation )
 */
static bool GetInstantFor(JSContext* cx, Handle<TimeZoneRecord> timeZone,
                          Handle<Wrapped<PlainDateTimeObject*>> dateTime,
                          TemporalDisambiguation disambiguation,
                          MutableHandle<Wrapped<InstantObject*>> result) {
  // Step 1.
  MOZ_ASSERT(TimeZoneMethodsRecordHasLookedUp(
      timeZone, TimeZoneMethod::GetOffsetNanosecondsFor));

  // Step 2.
  MOZ_ASSERT(TimeZoneMethodsRecordHasLookedUp(
      timeZone, TimeZoneMethod::GetPossibleInstantsFor));

  auto* unwrappedDateTime = dateTime.unwrap(cx);
  if (!unwrappedDateTime) {
    return false;
  }
  auto plainDateTime = ToPlainDateTime(unwrappedDateTime);

  // Step 3.
  Rooted<InstantVector> possibleInstants(cx, InstantVector(cx));
  if (!GetPossibleInstantsFor(cx, timeZone, dateTime, plainDateTime,
                              &possibleInstants)) {
    return false;
  }

  // Step 4.
  return DisambiguatePossibleInstants(cx, possibleInstants, timeZone,
                                      plainDateTime, disambiguation, result);
}

/**
 * GetInstantFor ( timeZoneRec, dateTime, disambiguation )
 */
static bool GetInstantFor(JSContext* cx, Handle<TimeZoneValue> timeZone,
                          Handle<Wrapped<PlainDateTimeObject*>> dateTime,
                          TemporalDisambiguation disambiguation,
                          MutableHandle<Wrapped<InstantObject*>> result) {
  Rooted<TimeZoneRecord> timeZoneRec(cx);
  if (!CreateTimeZoneMethodsRecord(cx, timeZone,
                                   {
                                       TimeZoneMethod::GetOffsetNanosecondsFor,
                                       TimeZoneMethod::GetPossibleInstantsFor,
                                   },
                                   &timeZoneRec)) {
    return false;
  }

  return GetInstantFor(cx, timeZoneRec, dateTime, disambiguation, result);
}

/**
 * GetInstantFor ( timeZoneRec, dateTime, disambiguation )
 */
bool js::temporal::GetInstantFor(JSContext* cx, Handle<TimeZoneValue> timeZone,
                                 Handle<PlainDateTimeObject*> dateTime,
                                 TemporalDisambiguation disambiguation,
                                 Instant* result) {
  Rooted<TimeZoneRecord> timeZoneRec(cx);
  if (!CreateTimeZoneMethodsRecord(cx, timeZone,
                                   {
                                       TimeZoneMethod::GetOffsetNanosecondsFor,
                                       TimeZoneMethod::GetPossibleInstantsFor,
                                   },
                                   &timeZoneRec)) {
    return false;
  }

  Rooted<Wrapped<InstantObject*>> instant(cx);
  if (!::GetInstantFor(cx, timeZoneRec, dateTime, disambiguation, &instant)) {
    return false;
  }

  auto* unwrappedInstant = instant.unwrap(cx);
  if (!unwrappedInstant) {
    return false;
  }

  *result = ToInstant(unwrappedInstant);
  return true;
}

/**
 * GetInstantFor ( timeZoneRec, dateTime, disambiguation )
 */
bool js::temporal::GetInstantFor(JSContext* cx, Handle<TimeZoneRecord> timeZone,
                                 Handle<PlainDateTimeWithCalendar> dateTime,
                                 TemporalDisambiguation disambiguation,
                                 Instant* result) {
  // Step 1.
  MOZ_ASSERT(TimeZoneMethodsRecordHasLookedUp(
      timeZone, TimeZoneMethod::GetOffsetNanosecondsFor));

  // Step 2.
  MOZ_ASSERT(TimeZoneMethodsRecordHasLookedUp(
      timeZone, TimeZoneMethod::GetPossibleInstantsFor));

  // Step 3.
  Rooted<InstantVector> possibleInstants(cx, InstantVector(cx));
  if (!GetPossibleInstantsFor(cx, timeZone, dateTime, &possibleInstants)) {
    return false;
  }

  // Step 4.
  Rooted<Wrapped<InstantObject*>> instant(cx);
  if (!DisambiguatePossibleInstants(cx, possibleInstants, timeZone,
                                    ToPlainDateTime(dateTime), disambiguation,
                                    &instant)) {
    return false;
  }

  auto* unwrappedInstant = instant.unwrap(cx);
  if (!unwrappedInstant) {
    return false;
  }

  *result = ToInstant(unwrappedInstant);
  return true;
}

/**
 * GetInstantFor ( timeZoneRec, dateTime, disambiguation )
 */
bool js::temporal::GetInstantFor(JSContext* cx, Handle<TimeZoneValue> timeZone,
                                 Handle<PlainDateTimeWithCalendar> dateTime,
                                 TemporalDisambiguation disambiguation,
                                 Instant* result) {
  Rooted<TimeZoneRecord> timeZoneRec(cx);
  if (!CreateTimeZoneMethodsRecord(cx, timeZone,
                                   {
                                       TimeZoneMethod::GetOffsetNanosecondsFor,
                                       TimeZoneMethod::GetPossibleInstantsFor,
                                   },
                                   &timeZoneRec)) {
    return false;
  }

  return GetInstantFor(cx, timeZoneRec, dateTime, disambiguation, result);
}

/**
 * IsOffsetTimeZoneIdentifier ( offsetString )
 *
 * Return true if |offsetString| is the prefix of a time zone offset string.
 * Time zone offset strings are be parsed through the |TimeZoneUTCOffsetName|
 * production.
 *
 * TimeZoneUTCOffsetName :
 *   UTCOffsetMinutePrecision
 *
 * UTCOffsetMinutePrecision :
 *   Sign Hour[+Padded]
 *   Sign Hour[+Padded] TimeSeparator[+Extended] MinuteSecond
 *   Sign Hour[+Padded] TimeSeparator[~Extended] MinuteSecond
 *
 * Sign :
 *   ASCIISign
 *   U+2212
 *
 * ASCIISign : one of + -
 *
 * NOTE: IANA time zone identifiers can't start with |Sign|.
 */
static bool IsOffsetTimeZoneIdentifierPrefix(JSLinearString* offsetString) {
  // Empty string can't be the prefix of |TimeZoneUTCOffsetName|.
  if (offsetString->empty()) {
    return false;
  }

  // Return true iff |offsetString| starts with |Sign|.
  char16_t ch = offsetString->latin1OrTwoByteChar(0);
  return ch == '+' || ch == '-' || ch == 0x2212;
}

/**
 * Temporal.TimeZone ( identifier )
 */
static bool TimeZoneConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Temporal.TimeZone")) {
    return false;
  }

  // Step 2.
  if (!args.requireAtLeast(cx, "Temporal.TimeZone", 1)) {
    return false;
  }

  if (!args[0].isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK, args[0],
                     nullptr, "not a string");
    return false;
  }

  Rooted<JSLinearString*> identifier(cx, args[0].toString()->ensureLinear(cx));
  if (!identifier) {
    return false;
  }

  Rooted<JSString*> canonical(cx);
  Rooted<Value> offsetMinutes(cx);
  if (IsOffsetTimeZoneIdentifierPrefix(identifier)) {
    // Step 3.
    int32_t minutes;
    if (!ParseTimeZoneOffsetString(cx, identifier, &minutes)) {
      return false;
    }
    MOZ_ASSERT(std::abs(minutes) < UnitsPerDay(TemporalUnit::Minute));

    canonical = FormatOffsetTimeZoneIdentifier(cx, minutes);
    if (!canonical) {
      return false;
    }

    offsetMinutes.setInt32(minutes);
  } else {
    // Step 4.
    canonical = ValidateAndCanonicalizeTimeZoneName(cx, identifier);
    if (!canonical) {
      return false;
    }

    offsetMinutes.setUndefined();
  }

  // Step 5.
  auto* timeZone = CreateTemporalTimeZone(cx, args, canonical, offsetMinutes);
  if (!timeZone) {
    return false;
  }

  args.rval().setObject(*timeZone);
  return true;
}

/**
 * Temporal.TimeZone.from ( item )
 */
static bool TimeZone_from(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Rooted<TimeZoneValue> timeZone(cx);
  if (!ToTemporalTimeZone(cx, args.get(0), &timeZone)) {
    return false;
  }

  // Step 2.
  auto* obj = ToTemporalTimeZoneObject(cx, timeZone);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.TimeZone.prototype.equals ( timeZoneLike )
 */
static bool TimeZone_equals(JSContext* cx, const CallArgs& args) {
  Rooted<TimeZoneValue> timeZone(cx, &args.thisv().toObject());

  // Step 3.
  Rooted<TimeZoneValue> other(cx);
  if (!ToTemporalTimeZone(cx, args.get(0), &other)) {
    return false;
  }

  // Step 4.
  bool equals;
  if (!TimeZoneEquals(cx, timeZone, other, &equals)) {
    return false;
  }

  args.rval().setBoolean(equals);
  return true;
}

/**
 * Temporal.TimeZone.prototype.equals ( timeZoneLike )
 */
static bool TimeZone_equals(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsTimeZone, TimeZone_equals>(cx, args);
}

/**
 * Temporal.TimeZone.prototype.getOffsetNanosecondsFor ( instant )
 */
static bool TimeZone_getOffsetNanosecondsFor(JSContext* cx,
                                             const CallArgs& args) {
  Rooted<TimeZoneObject*> timeZone(
      cx, &args.thisv().toObject().as<TimeZoneObject>());

  // Step 3.
  Instant instant;
  if (!ToTemporalInstant(cx, args.get(0), &instant)) {
    return false;
  }

  // Steps 4-5.
  int64_t offset;
  if (!BuiltinGetOffsetNanosecondsFor(cx, timeZone, instant, &offset)) {
    return false;
  }

  args.rval().setNumber(offset);
  return true;
}

/**
 * Temporal.TimeZone.prototype.getOffsetNanosecondsFor ( instant )
 */
static bool TimeZone_getOffsetNanosecondsFor(JSContext* cx, unsigned argc,
                                             Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsTimeZone, TimeZone_getOffsetNanosecondsFor>(
      cx, args);
}

/**
 * Temporal.TimeZone.prototype.getOffsetStringFor ( instant )
 */
static bool TimeZone_getOffsetStringFor(JSContext* cx, const CallArgs& args) {
  Rooted<TimeZoneValue> timeZone(cx, &args.thisv().toObject());

  // Step 3.
  Rooted<Wrapped<InstantObject*>> instant(cx,
                                          ToTemporalInstant(cx, args.get(0)));
  if (!instant) {
    return false;
  }

  // Step 4.
  Rooted<TimeZoneRecord> timeZoneRec(cx);
  if (!CreateTimeZoneMethodsRecord(cx, timeZone,
                                   {
                                       TimeZoneMethod::GetOffsetNanosecondsFor,
                                   },
                                   &timeZoneRec)) {
    return false;
  }

  // Step 5.
  JSString* str = GetOffsetStringFor(cx, timeZoneRec, instant);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.TimeZone.prototype.getOffsetStringFor ( instant )
 */
static bool TimeZone_getOffsetStringFor(JSContext* cx, unsigned argc,
                                        Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsTimeZone, TimeZone_getOffsetStringFor>(cx,
                                                                       args);
}

/**
 * Temporal.TimeZone.prototype.getPlainDateTimeFor ( instant [, calendarLike ] )
 */
static bool TimeZone_getPlainDateTimeFor(JSContext* cx, const CallArgs& args) {
  Rooted<TimeZoneValue> timeZone(cx, &args.thisv().toObject());

  // Step 3.
  Rooted<Wrapped<InstantObject*>> instant(cx,
                                          ToTemporalInstant(cx, args.get(0)));
  if (!instant) {
    return false;
  }

  // Step 4.
  Rooted<CalendarValue> calendar(cx);
  if (!ToTemporalCalendarWithISODefault(cx, args.get(1), &calendar)) {
    return false;
  }

  // Steps 5-6.
  auto* result = GetPlainDateTimeFor(cx, timeZone, instant, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.TimeZone.prototype.getPlainDateTimeFor ( instant [, calendarLike ] )
 */
static bool TimeZone_getPlainDateTimeFor(JSContext* cx, unsigned argc,
                                         Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsTimeZone, TimeZone_getPlainDateTimeFor>(cx,
                                                                        args);
}

/**
 * Temporal.TimeZone.prototype.getInstantFor ( dateTime [ , options ] )
 */
static bool TimeZone_getInstantFor(JSContext* cx, const CallArgs& args) {
  Rooted<TimeZoneValue> timeZone(cx, &args.thisv().toObject());

  // Step 3.
  Rooted<Wrapped<PlainDateTimeObject*>> dateTime(
      cx, ToTemporalDateTime(cx, args.get(0)));
  if (!dateTime) {
    return false;
  }

  // Steps 4-5.
  auto disambiguation = TemporalDisambiguation::Compatible;
  if (args.hasDefined(1)) {
    // Step 4.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", "getInstantFor", args[1]));
    if (!options) {
      return false;
    }

    // Step 5.
    if (!GetTemporalDisambiguationOption(cx, options, &disambiguation)) {
      return false;
    }
  }

  // Steps 6-7.
  Rooted<Wrapped<InstantObject*>> result(cx);
  if (!::GetInstantFor(cx, timeZone, dateTime, disambiguation, &result)) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.TimeZone.prototype.getInstantFor ( dateTime [ , options ] )
 */
static bool TimeZone_getInstantFor(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsTimeZone, TimeZone_getInstantFor>(cx, args);
}

/**
 * Temporal.TimeZone.prototype.getPossibleInstantsFor ( dateTime )
 */
static bool TimeZone_getPossibleInstantsFor(JSContext* cx,
                                            const CallArgs& args) {
  Rooted<TimeZoneObject*> timeZone(
      cx, &args.thisv().toObject().as<TimeZoneObject>());

  // Step 3.
  PlainDateTime dateTime;
  if (!ToTemporalDateTime(cx, args.get(0), &dateTime)) {
    return false;
  }

  // Steps 4-5.
  EpochInstantList possibleInstants;
  if (!BuiltinGetPossibleInstantsFor(cx, timeZone, dateTime,
                                     possibleInstants)) {
    return false;
  }

  // Step 6.
  size_t length = possibleInstants.length();
  Rooted<ArrayObject*> result(cx, NewDenseFullyAllocatedArray(cx, length));
  if (!result) {
    return false;
  }
  result->ensureDenseInitializedLength(0, length);

  // Step 7.
  for (size_t i = 0; i < length; i++) {
    // Step 7.a. (Already performed in step 4 in our implementation.)
    MOZ_ASSERT(IsValidEpochInstant(possibleInstants[i]));

    // Step 7.b.
    auto* instant = CreateTemporalInstant(cx, possibleInstants[i]);
    if (!instant) {
      return false;
    }

    // Step 7.c.
    result->initDenseElement(i, ObjectValue(*instant));
  }

  // Step 8.
  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.TimeZone.prototype.getPossibleInstantsFor ( dateTime )
 */
static bool TimeZone_getPossibleInstantsFor(JSContext* cx, unsigned argc,
                                            Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsTimeZone, TimeZone_getPossibleInstantsFor>(
      cx, args);
}

/**
 * Temporal.TimeZone.prototype.getNextTransition ( startingPoint )
 */
static bool TimeZone_getNextTransition(JSContext* cx, const CallArgs& args) {
  Rooted<TimeZoneObject*> timeZone(
      cx, &args.thisv().toObject().as<TimeZoneObject>());

  // Step 3.
  Instant startingPoint;
  if (!ToTemporalInstant(cx, args.get(0), &startingPoint)) {
    return false;
  }

  // Step 4.
  if (!timeZone->offsetMinutes().isUndefined()) {
    args.rval().setNull();
    return true;
  }

  // Step 5.
  mozilla::Maybe<Instant> transition;
  if (!GetNamedTimeZoneNextTransition(cx, timeZone, startingPoint,
                                      &transition)) {
    return false;
  }

  // Step 6.
  if (!transition) {
    args.rval().setNull();
    return true;
  }

  // Step 7.
  auto* instant = CreateTemporalInstant(cx, *transition);
  if (!instant) {
    return false;
  }

  args.rval().setObject(*instant);
  return true;
}

/**
 * Temporal.TimeZone.prototype.getNextTransition ( startingPoint )
 */
static bool TimeZone_getNextTransition(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsTimeZone, TimeZone_getNextTransition>(cx, args);
}

/**
 * Temporal.TimeZone.prototype.getPreviousTransition ( startingPoint )
 */
static bool TimeZone_getPreviousTransition(JSContext* cx,
                                           const CallArgs& args) {
  Rooted<TimeZoneObject*> timeZone(
      cx, &args.thisv().toObject().as<TimeZoneObject>());

  // Step 3.
  Instant startingPoint;
  if (!ToTemporalInstant(cx, args.get(0), &startingPoint)) {
    return false;
  }

  // Step 4.
  if (!timeZone->offsetMinutes().isUndefined()) {
    args.rval().setNull();
    return true;
  }

  // Step 5.
  mozilla::Maybe<Instant> transition;
  if (!GetNamedTimeZonePreviousTransition(cx, timeZone, startingPoint,
                                          &transition)) {
    return false;
  }

  // Step 6.
  if (!transition) {
    args.rval().setNull();
    return true;
  }

  // Step 7.
  auto* instant = CreateTemporalInstant(cx, *transition);
  if (!instant) {
    return false;
  }

  args.rval().setObject(*instant);
  return true;
}

/**
 * Temporal.TimeZone.prototype.getPreviousTransition ( startingPoint )
 */
static bool TimeZone_getPreviousTransition(JSContext* cx, unsigned argc,
                                           Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsTimeZone, TimeZone_getPreviousTransition>(cx,
                                                                          args);
}

/**
 * Temporal.TimeZone.prototype.toString ( )
 */
static bool TimeZone_toString(JSContext* cx, const CallArgs& args) {
  auto* timeZone = &args.thisv().toObject().as<TimeZoneObject>();

  // Steps 3-4.
  args.rval().setString(timeZone->identifier());
  return true;
}

/**
 * Temporal.TimeZone.prototype.toString ( )
 */
static bool TimeZone_toString(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsTimeZone, TimeZone_toString>(cx, args);
}

/**
 * Temporal.TimeZone.prototype.toJSON ( )
 */
static bool TimeZone_toJSON(JSContext* cx, const CallArgs& args) {
  auto* timeZone = &args.thisv().toObject().as<TimeZoneObject>();

  // Steps 3-4.
  args.rval().setString(timeZone->identifier());
  return true;
}

/**
 * Temporal.TimeZone.prototype.toJSON ( )
 */
static bool TimeZone_toJSON(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsTimeZone, TimeZone_toJSON>(cx, args);
}

/**
 * get Temporal.TimeZone.prototype.id
 */
static bool TimeZone_id(JSContext* cx, const CallArgs& args) {
  auto* timeZone = &args.thisv().toObject().as<TimeZoneObject>();

  // Steps 3-4.
  args.rval().setString(timeZone->identifier());
  return true;
}

/**
 * get Temporal.TimeZone.prototype.id
 */
static bool TimeZone_id(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsTimeZone, TimeZone_id>(cx, args);
}

void js::temporal::TimeZoneObjectMaybeBuiltin::finalize(JS::GCContext* gcx,
                                                        JSObject* obj) {
  MOZ_ASSERT(gcx->onMainThread());

  if (auto* timeZone = obj->as<TimeZoneObjectMaybeBuiltin>().getTimeZone()) {
    intl::RemoveICUCellMemory(gcx, obj, TimeZoneObject::EstimatedMemoryUse);
    delete timeZone;
  }
}

const JSClassOps TimeZoneObject::classOps_ = {
    nullptr,                   // addProperty
    nullptr,                   // delProperty
    nullptr,                   // enumerate
    nullptr,                   // newEnumerate
    nullptr,                   // resolve
    nullptr,                   // mayResolve
    TimeZoneObject::finalize,  // finalize
    nullptr,                   // call
    nullptr,                   // construct
    nullptr,                   // trace
};

const JSClass TimeZoneObject::class_ = {
    "Temporal.TimeZone",
    JSCLASS_HAS_RESERVED_SLOTS(TimeZoneObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_TimeZone) |
        JSCLASS_FOREGROUND_FINALIZE,
    &TimeZoneObject::classOps_,
    &TimeZoneObject::classSpec_,
};

const JSClass& TimeZoneObject::protoClass_ = PlainObject::class_;

static const JSFunctionSpec TimeZone_methods[] = {
    JS_FN("from", TimeZone_from, 1, 0),
    JS_FS_END,
};

static const JSFunctionSpec TimeZone_prototype_methods[] = {
    JS_FN("equals", TimeZone_equals, 1, 0),
    JS_FN("getOffsetNanosecondsFor", TimeZone_getOffsetNanosecondsFor, 1, 0),
    JS_FN("getOffsetStringFor", TimeZone_getOffsetStringFor, 1, 0),
    JS_FN("getPlainDateTimeFor", TimeZone_getPlainDateTimeFor, 1, 0),
    JS_FN("getInstantFor", TimeZone_getInstantFor, 1, 0),
    JS_FN("getPossibleInstantsFor", TimeZone_getPossibleInstantsFor, 1, 0),
    JS_FN("getNextTransition", TimeZone_getNextTransition, 1, 0),
    JS_FN("getPreviousTransition", TimeZone_getPreviousTransition, 1, 0),
    JS_FN("toString", TimeZone_toString, 0, 0),
    JS_FN("toJSON", TimeZone_toJSON, 0, 0),
    JS_FS_END,
};

static const JSPropertySpec TimeZone_prototype_properties[] = {
    JS_PSG("id", TimeZone_id, 0),
    JS_STRING_SYM_PS(toStringTag, "Temporal.TimeZone", JSPROP_READONLY),
    JS_PS_END,
};

const ClassSpec TimeZoneObject::classSpec_ = {
    GenericCreateConstructor<TimeZoneConstructor, 1, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<TimeZoneObject>,
    TimeZone_methods,
    nullptr,
    TimeZone_prototype_methods,
    TimeZone_prototype_properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};

const JSClassOps BuiltinTimeZoneObject::classOps_ = {
    nullptr,                          // addProperty
    nullptr,                          // delProperty
    nullptr,                          // enumerate
    nullptr,                          // newEnumerate
    nullptr,                          // resolve
    nullptr,                          // mayResolve
    BuiltinTimeZoneObject::finalize,  // finalize
    nullptr,                          // call
    nullptr,                          // construct
    nullptr,                          // trace
};

const JSClass BuiltinTimeZoneObject::class_ = {
    "Temporal.BuiltinTimeZone",
    JSCLASS_HAS_RESERVED_SLOTS(BuiltinTimeZoneObject::SLOT_COUNT) |
        JSCLASS_FOREGROUND_FINALIZE,
    &BuiltinTimeZoneObject::classOps_,
};
