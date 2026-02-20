/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_TemporalParser_h
#define builtin_temporal_TemporalParser_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

#include <cstdlib>
#include <stdint.h>

#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/TemporalUnit.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

class JSLinearString;
class JS_PUBLIC_API JSTracer;

namespace js::temporal {

struct MOZ_STACK_CLASS ParsedTimeZone final {
  JSLinearString* name = nullptr;
  int32_t offset = INT32_MIN;

  void trace(JSTracer* trc);

  static ParsedTimeZone fromName(JSLinearString* name) {
    MOZ_ASSERT(name);
    return {name, 0};
  }

  static ParsedTimeZone fromOffset(int32_t offset) {
    MOZ_ASSERT(std::abs(offset) < UnitsPerDay(TemporalUnit::Minute));
    return {nullptr, offset};
  }

  explicit operator bool() const {
    return name != nullptr || offset != INT32_MIN;
  }
};

struct MOZ_STACK_CLASS ParsedZonedDateTime final {
  ISODateTime dateTime = {};
  JSLinearString* calendar = nullptr;
  ParsedTimeZone timeZoneAnnotation{};
  int64_t timeZoneOffset = 0;
  bool isUTC = false;
  bool hasOffset = false;
  bool isStartOfDay = false;

  void trace(JSTracer* trc);
};

/**
 * ParseTemporalInstantString ( isoString )
 */
bool ParseTemporalInstantString(JSContext* cx, JS::Handle<JSString*> str,
                                ISODateTime* result, int64_t* offset);

/**
 * ParseTemporalTimeZoneString ( timeZoneString )
 */
bool ParseTemporalTimeZoneString(JSContext* cx, JS::Handle<JSString*> str,
                                 JS::MutableHandle<ParsedTimeZone> result);

/**
 * ParseTimeZoneIdentifier ( identifier )
 */
bool ParseTimeZoneIdentifier(JSContext* cx, JS::Handle<JSString*> str,
                             JS::MutableHandle<ParsedTimeZone> result);

/**
 * ParseDateTimeUTCOffset ( offsetString )
 */
bool ParseDateTimeUTCOffset(JSContext* cx, JS::Handle<JSString*> str,
                            int64_t* result);

/**
 * ParseTemporalDurationString ( isoString )
 */
bool ParseTemporalDurationString(JSContext* cx, JS::Handle<JSString*> str,
                                 Duration* result);

/**
 * ParseTemporalCalendarString ( isoString )
 */
JSLinearString* ParseTemporalCalendarString(JSContext* cx,
                                            JS::Handle<JSString*> str);

/**
 * ParseTemporalTimeString ( isoString )
 */
bool ParseTemporalTimeString(JSContext* cx, JS::Handle<JSString*> str,
                             Time* result);

/**
 * ParseTemporalMonthDayString ( isoString )
 */
bool ParseTemporalMonthDayString(JSContext* cx, JS::Handle<JSString*> str,
                                 ISODate* result, bool* hasYear,
                                 JS::MutableHandle<JSString*> calendar);

/**
 * ParseTemporalYearMonthString ( isoString )
 */
bool ParseTemporalYearMonthString(JSContext* cx, JS::Handle<JSString*> str,
                                  ISODate* result,
                                  JS::MutableHandle<JSString*> calendar);

/**
 * ParseTemporalDateTimeString ( isoString )
 */
bool ParseTemporalDateTimeString(JSContext* cx, JS::Handle<JSString*> str,
                                 ISODateTime* result,
                                 JS::MutableHandle<JSString*> calendar);

/**
 * ParseTemporalZonedDateTimeString ( isoString )
 */
bool ParseTemporalZonedDateTimeString(
    JSContext* cx, JS::Handle<JSString*> str,
    JS::MutableHandle<ParsedZonedDateTime> result);

/**
 * ParseTemporalRelativeToString ( isoString )
 */
bool ParseTemporalRelativeToString(
    JSContext* cx, JS::Handle<JSString*> str,
    JS::MutableHandle<ParsedZonedDateTime> result);

} /* namespace js::temporal */

namespace js {

template <typename Wrapper>
class WrappedPtrOperations<temporal::ParsedTimeZone, Wrapper> {
  const auto& object() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  JS::Handle<JSLinearString*> name() const {
    return JS::Handle<JSLinearString*>::fromMarkedLocation(&object().name);
  }

  int32_t offset() const { return object().offset; }

  explicit operator bool() const { return bool(object()); }
};

template <typename Wrapper>
class WrappedPtrOperations<temporal::ParsedZonedDateTime, Wrapper> {
  const auto& object() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  const temporal::ISODateTime& dateTime() const { return object().dateTime; }

  JS::Handle<JSLinearString*> calendar() const {
    return JS::Handle<JSLinearString*>::fromMarkedLocation(&object().calendar);
  }

  JS::Handle<temporal::ParsedTimeZone> timeZoneAnnotation() const {
    return JS::Handle<temporal::ParsedTimeZone>::fromMarkedLocation(
        &object().timeZoneAnnotation);
  }

  int64_t timeZoneOffset() const { return object().timeZoneOffset; }

  bool isUTC() const { return object().isUTC; }

  bool hasOffset() const { return object().hasOffset; }

  bool isStartOfDay() const { return object().isStartOfDay; }
};

} /* namespace js */

#endif /* builtin_temporal_TemporalParser_h */
