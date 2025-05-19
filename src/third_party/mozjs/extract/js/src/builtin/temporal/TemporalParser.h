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

#include "builtin/temporal/TemporalUnit.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

class JSLinearString;
class JS_PUBLIC_API JSTracer;

namespace js::temporal {

struct Duration;
struct PlainDate;
struct PlainDateTime;
struct PlainTime;

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

/**
 * ParseTemporalInstantString ( isoString )
 */
bool ParseTemporalInstantString(JSContext* cx, JS::Handle<JSString*> str,
                                PlainDateTime* result, int64_t* offset);

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
 * ParseTimeZoneOffsetString ( isoString )
 */
bool ParseTimeZoneOffsetString(JSContext* cx, JS::Handle<JSString*> str,
                               int32_t* result);

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
                             PlainTime* result);

/**
 * ParseTemporalDateString ( isoString )
 */
bool ParseTemporalDateString(JSContext* cx, JS::Handle<JSString*> str,
                             PlainDate* result,
                             JS::MutableHandle<JSString*> calendar);

/**
 * ParseTemporalMonthDayString ( isoString )
 */
bool ParseTemporalMonthDayString(JSContext* cx, JS::Handle<JSString*> str,
                                 PlainDate* result, bool* hasYear,
                                 JS::MutableHandle<JSString*> calendar);

/**
 * ParseTemporalYearMonthString ( isoString )
 */
bool ParseTemporalYearMonthString(JSContext* cx, JS::Handle<JSString*> str,
                                  PlainDate* result,
                                  JS::MutableHandle<JSString*> calendar);

/**
 * ParseTemporalDateTimeString ( isoString )
 */
bool ParseTemporalDateTimeString(JSContext* cx, JS::Handle<JSString*> str,
                                 PlainDateTime* result,
                                 JS::MutableHandle<JSString*> calendar);

/**
 * ParseTemporalZonedDateTimeString ( isoString )
 */
bool ParseTemporalZonedDateTimeString(
    JSContext* cx, JS::Handle<JSString*> str, PlainDateTime* dateTime,
    bool* isUTC, bool* hasOffset, int64_t* timeZoneOffset,
    JS::MutableHandle<ParsedTimeZone> timeZoneAnnotation,
    JS::MutableHandle<JSString*> calendar);

/**
 * ParseTemporalRelativeToString ( isoString )
 */
bool ParseTemporalRelativeToString(
    JSContext* cx, JS::Handle<JSString*> str, PlainDateTime* dateTime,
    bool* isUTC, bool* hasOffset, int64_t* timeZoneOffset,
    JS::MutableHandle<ParsedTimeZone> timeZoneAnnotation,
    JS::MutableHandle<JSString*> calendar);

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

} /* namespace js */

#endif /* builtin_temporal_TemporalParser_h */
