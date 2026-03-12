/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/PlainMonthDay.h"

#include "mozilla/Assertions.h"
#include "mozilla/EnumSet.h"

#include <utility>

#include "jspubtd.h"
#include "NamespaceImports.h"

#include "builtin/intl/DateTimeFormat.h"
#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/CalendarFields.h"
#include "builtin/temporal/PlainDate.h"
#include "builtin/temporal/PlainDateTime.h"
#include "builtin/temporal/PlainYearMonth.h"
#include "builtin/temporal/Temporal.h"
#include "builtin/temporal/TemporalParser.h"
#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/ToString.h"
#include "gc/AllocKind.h"
#include "gc/Barrier.h"
#include "gc/GCEnum.h"
#include "js/CallArgs.h"
#include "js/CallNonGenericMethod.h"
#include "js/Class.h"
#include "js/ErrorReport.h"
#include "js/friend/ErrorMessages.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "vm/BytecodeUtil.h"
#include "vm/GlobalObject.h"
#include "vm/JSAtomState.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"
#include "vm/StringType.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;
using namespace js::temporal;

static inline bool IsPlainMonthDay(Handle<Value> v) {
  return v.isObject() && v.toObject().is<PlainMonthDayObject>();
}

/**
 * CreateTemporalMonthDay ( isoDate, calendar [ , newTarget ] )
 */
static PlainMonthDayObject* CreateTemporalMonthDay(
    JSContext* cx, const CallArgs& args, const ISODate& isoDate,
    Handle<CalendarValue> calendar) {
  // Step 1.
  if (!ISODateWithinLimits(isoDate)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_MONTH_DAY_INVALID);
    return nullptr;
  }

  // Steps 2-3.
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_PlainMonthDay,
                                          &proto)) {
    return nullptr;
  }

  auto* object = NewObjectWithClassProto<PlainMonthDayObject>(cx, proto);
  if (!object) {
    return nullptr;
  }

  // Step 4.
  auto packedDate = PackedDate::pack(isoDate);
  object->setFixedSlot(PlainMonthDayObject::PACKED_DATE_SLOT,
                       PrivateUint32Value(packedDate.value));

  // Step 5.
  object->setFixedSlot(PlainMonthDayObject::CALENDAR_SLOT,
                       calendar.toSlotValue());

  // Step 6.
  return object;
}

/**
 * CreateTemporalMonthDay ( isoDate, calendar [ , newTarget ] )
 */
PlainMonthDayObject* js::temporal::CreateTemporalMonthDay(
    JSContext* cx, Handle<PlainMonthDay> monthDay) {
  MOZ_ASSERT(IsValidISODate(monthDay));

  // Step 1.
  MOZ_ASSERT(ISODateWithinLimits(monthDay));

  // Steps 2-3.
  auto* object = NewBuiltinClassInstance<PlainMonthDayObject>(cx);
  if (!object) {
    return nullptr;
  }

  // Step 4.
  auto packedDate = PackedDate::pack(monthDay);
  object->setFixedSlot(PlainMonthDayObject::PACKED_DATE_SLOT,
                       PrivateUint32Value(packedDate.value));

  // Step 5.
  object->setFixedSlot(PlainMonthDayObject::CALENDAR_SLOT,
                       monthDay.calendar().toSlotValue());

  // Step 6.
  return object;
}

/**
 * CreateTemporalMonthDay ( isoDate, calendar [ , newTarget ] )
 */
bool js::temporal::CreateTemporalMonthDay(JSContext* cx, const ISODate& isoDate,
                                          Handle<CalendarValue> calendar,
                                          MutableHandle<PlainMonthDay> result) {
  MOZ_ASSERT(IsValidISODate(isoDate));

  // Step 1.
  if (!ISODateWithinLimits(isoDate)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_MONTH_DAY_INVALID);
    return false;
  }

  // Steps 2-6.
  result.set(PlainMonthDay{isoDate, calendar});
  return true;
}

struct MonthDayOptions {
  TemporalOverflow overflow = TemporalOverflow::Constrain;
};

/**
 * ToTemporalMonthDay ( item [ , options ] )
 */
static bool ToTemporalMonthDayOptions(JSContext* cx, Handle<Value> options,
                                      MonthDayOptions* result) {
  if (options.isUndefined()) {
    *result = {};
    return true;
  }

  // NOTE: |options| are only passed from `Temporal.PlainMonthDay.from`.

  Rooted<JSObject*> resolvedOptions(
      cx, RequireObjectArg(cx, "options", "from", options));
  if (!resolvedOptions) {
    return false;
  }

  auto overflow = TemporalOverflow::Constrain;
  if (!GetTemporalOverflowOption(cx, resolvedOptions, &overflow)) {
    return false;
  }

  *result = {overflow};
  return true;
}

/**
 * ToTemporalMonthDay ( item [ , options ] )
 */
static bool ToTemporalMonthDay(JSContext* cx, Handle<JSObject*> item,
                               Handle<Value> options,
                               MutableHandle<PlainMonthDay> result) {
  // Step 2.a.
  if (auto* plainMonthDay = item->maybeUnwrapIf<PlainMonthDayObject>()) {
    auto date = plainMonthDay->date();
    Rooted<CalendarValue> calendar(cx, plainMonthDay->calendar());
    if (!calendar.wrap(cx)) {
      return false;
    }

    // Steps 2.a.i-ii.
    MonthDayOptions ignoredOptions;
    if (!ToTemporalMonthDayOptions(cx, options, &ignoredOptions)) {
      return false;
    }

    // Step 2.a.iii.
    result.set(PlainMonthDay{date, calendar});
    return true;
  }

  // Step 2.b.
  Rooted<CalendarValue> calendar(cx);
  if (!GetTemporalCalendarWithISODefault(cx, item, &calendar)) {
    return false;
  }

  // Step 2.c.
  Rooted<CalendarFields> fields(cx);
  if (!PrepareCalendarFields(cx, calendar, item,
                             {
                                 CalendarField::Year,
                                 CalendarField::Month,
                                 CalendarField::MonthCode,
                                 CalendarField::Day,
                             },
                             &fields)) {
    return false;
  }

  // Steps 2.d-e.
  MonthDayOptions resolvedOptions;
  if (!ToTemporalMonthDayOptions(cx, options, &resolvedOptions)) {
    return false;
  }
  auto [overflow] = resolvedOptions;

  // Step 2.f.
  return CalendarMonthDayFromFields(cx, calendar, fields, overflow, result);
}

/**
 * ToTemporalMonthDay ( item [ , options ] )
 */
static bool ToTemporalMonthDay(JSContext* cx, Handle<Value> item,
                               Handle<Value> options,
                               MutableHandle<PlainMonthDay> result) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  if (item.isObject()) {
    Rooted<JSObject*> itemObj(cx, &item.toObject());
    return ToTemporalMonthDay(cx, itemObj, options, result);
  }

  // Step 3.
  if (!item.isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, item,
                     nullptr, "not a string");
    return false;
  }
  Rooted<JSString*> string(cx, item.toString());

  // Step 4.
  ISODate date;
  bool hasYear;
  Rooted<JSString*> calendarString(cx);
  if (!ParseTemporalMonthDayString(cx, string, &date, &hasYear,
                                   &calendarString)) {
    return false;
  }

  // Steps 5-7.
  Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));
  if (calendarString) {
    if (!CanonicalizeCalendar(cx, calendarString, &calendar)) {
      return false;
    }
  }

  // Steps 8-9.
  MonthDayOptions ignoredOptions;
  if (!ToTemporalMonthDayOptions(cx, options, &ignoredOptions)) {
    return false;
  }

  // Step 10.
  if (calendar.identifier() == CalendarId::ISO8601) {
    // Step 10.a.
    constexpr int32_t referenceISOYear = 1972;

    // Step 10.b.
    auto isoDate = ISODate{referenceISOYear, date.month, date.day};

    // Step 10.c.
    return CreateTemporalMonthDay(cx, isoDate, calendar, result);
  }

  // Steps 11-12.
  Rooted<PlainMonthDay> monthDay(cx);
  if (!CreateTemporalMonthDay(cx, date, calendar, &monthDay)) {
    return false;
  }

  // Step 13.
  Rooted<CalendarFields> fields(cx);
  if (!ISODateToFields(cx, monthDay, &fields)) {
    return false;
  }

  // Steps 14-15.
  return CalendarMonthDayFromFields(cx, calendar, fields,
                                    TemporalOverflow::Constrain, result);
}

/**
 * ToTemporalMonthDay ( item [ , options ] )
 */
static bool ToTemporalMonthDay(JSContext* cx, Handle<Value> item,
                               MutableHandle<PlainMonthDay> result) {
  return ToTemporalMonthDay(cx, item, UndefinedHandleValue, result);
}

/**
 * Temporal.PlainMonthDay ( isoMonth, isoDay [ , calendar [ , referenceISOYear ]
 * ] )
 */
static bool PlainMonthDayConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Temporal.PlainMonthDay")) {
    return false;
  }

  // Step 3.
  double isoMonth;
  if (!ToIntegerWithTruncation(cx, args.get(0), "month", &isoMonth)) {
    return false;
  }

  // Step 4.
  double isoDay;
  if (!ToIntegerWithTruncation(cx, args.get(1), "day", &isoDay)) {
    return false;
  }

  // Steps 5-7.
  Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));
  if (args.hasDefined(2)) {
    // Step 6.
    if (!args[2].isString()) {
      ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, args[2],
                       nullptr, "not a string");
      return false;
    }

    // Step 7.
    Rooted<JSString*> calendarString(cx, args[2].toString());
    if (!CanonicalizeCalendar(cx, calendarString, &calendar)) {
      return false;
    }
  }

  // Steps 2 and 8.
  double isoYear = 1972;
  if (args.hasDefined(3)) {
    if (!ToIntegerWithTruncation(cx, args[3], "year", &isoYear)) {
      return false;
    }
  }

  // Step 9.
  if (!ThrowIfInvalidISODate(cx, isoYear, isoMonth, isoDay)) {
    return false;
  }

  // Step 10.
  auto isoDate = ISODate{int32_t(isoYear), int32_t(isoMonth), int32_t(isoDay)};

  // Step 9.
  auto* monthDay = CreateTemporalMonthDay(cx, args, isoDate, calendar);
  if (!monthDay) {
    return false;
  }

  args.rval().setObject(*monthDay);
  return true;
}

/**
 * Temporal.PlainMonthDay.from ( item [ , options ] )
 */
static bool PlainMonthDay_from(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Rooted<PlainMonthDay> monthDay(cx);
  if (!ToTemporalMonthDay(cx, args.get(0), args.get(1), &monthDay)) {
    return false;
  }

  auto* result = CreateTemporalMonthDay(cx, monthDay);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * get Temporal.PlainMonthDay.prototype.calendarId
 */
static bool PlainMonthDay_calendarId(JSContext* cx, const CallArgs& args) {
  auto* monthDay = &args.thisv().toObject().as<PlainMonthDayObject>();

  // Step 3.
  auto* str =
      NewStringCopy<CanGC>(cx, CalendarIdentifier(monthDay->calendar()));
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * get Temporal.PlainMonthDay.prototype.calendarId
 */
static bool PlainMonthDay_calendarId(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainMonthDay, PlainMonthDay_calendarId>(cx,
                                                                         args);
}

/**
 * get Temporal.PlainMonthDay.prototype.monthCode
 */
static bool PlainMonthDay_monthCode(JSContext* cx, const CallArgs& args) {
  auto* monthDay = &args.thisv().toObject().as<PlainMonthDayObject>();
  Rooted<CalendarValue> calendar(cx, monthDay->calendar());

  // Step 3.
  return CalendarMonthCode(cx, calendar, monthDay->date(), args.rval());
}

/**
 * get Temporal.PlainMonthDay.prototype.monthCode
 */
static bool PlainMonthDay_monthCode(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainMonthDay, PlainMonthDay_monthCode>(cx,
                                                                        args);
}

/**
 * get Temporal.PlainMonthDay.prototype.day
 */
static bool PlainMonthDay_day(JSContext* cx, const CallArgs& args) {
  auto* monthDay = &args.thisv().toObject().as<PlainMonthDayObject>();
  Rooted<CalendarValue> calendar(cx, monthDay->calendar());

  // Step 3.
  return CalendarDay(cx, calendar, monthDay->date(), args.rval());
}

/**
 * get Temporal.PlainMonthDay.prototype.day
 */
static bool PlainMonthDay_day(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainMonthDay, PlainMonthDay_day>(cx, args);
}

/**
 * Temporal.PlainMonthDay.prototype.with ( temporalMonthDayLike [ , options ] )
 */
static bool PlainMonthDay_with(JSContext* cx, const CallArgs& args) {
  Rooted<PlainMonthDay> monthDay(
      cx, &args.thisv().toObject().as<PlainMonthDayObject>());

  // Step 3.
  Rooted<JSObject*> temporalMonthDayLike(
      cx, RequireObjectArg(cx, "temporalMonthDayLike", "with", args.get(0)));
  if (!temporalMonthDayLike) {
    return false;
  }
  if (!ThrowIfTemporalLikeObject(cx, temporalMonthDayLike)) {
    return false;
  }

  // Step 4.
  auto calendar = monthDay.calendar();

  // Step 5.
  Rooted<CalendarFields> fields(cx);
  if (!ISODateToFields(cx, monthDay, &fields)) {
    return false;
  }

  // Step 6.
  Rooted<CalendarFields> partialMonthDay(cx);
  if (!PreparePartialCalendarFields(cx, calendar, temporalMonthDayLike,
                                    {
                                        CalendarField::Year,
                                        CalendarField::Month,
                                        CalendarField::MonthCode,
                                        CalendarField::Day,
                                    },
                                    &partialMonthDay)) {
    return false;
  }
  MOZ_ASSERT(!partialMonthDay.keys().isEmpty());

  // Step 7.
  fields = CalendarMergeFields(calendar, fields, partialMonthDay);

  // Steps 8-9.
  auto overflow = TemporalOverflow::Constrain;
  if (args.hasDefined(1)) {
    // Step 8.
    Rooted<JSObject*> options(cx,
                              RequireObjectArg(cx, "options", "with", args[1]));
    if (!options) {
      return false;
    }

    // Step 9.
    if (!GetTemporalOverflowOption(cx, options, &overflow)) {
      return false;
    }
  }

  // Step 10.
  Rooted<PlainMonthDay> result(cx);
  if (!CalendarMonthDayFromFields(cx, calendar, fields, overflow, &result)) {
    return false;
  }
  MOZ_ASSERT(ISODateWithinLimits(result));

  // Step 11.
  auto* obj = CreateTemporalMonthDay(cx, result);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainMonthDay.prototype.with ( temporalMonthDayLike [ , options ] )
 */
static bool PlainMonthDay_with(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainMonthDay, PlainMonthDay_with>(cx, args);
}

/**
 * Temporal.PlainMonthDay.prototype.equals ( other )
 */
static bool PlainMonthDay_equals(JSContext* cx, const CallArgs& args) {
  auto* monthDay = &args.thisv().toObject().as<PlainMonthDayObject>();
  auto date = monthDay->date();
  Rooted<CalendarValue> calendar(cx, monthDay->calendar());

  // Step 3.
  Rooted<PlainMonthDay> other(cx);
  if (!ToTemporalMonthDay(cx, args.get(0), &other)) {
    return false;
  }

  // Steps 4-7.
  bool equals =
      date == other.date() && CalendarEquals(calendar, other.calendar());

  args.rval().setBoolean(equals);
  return true;
}

/**
 * Temporal.PlainMonthDay.prototype.equals ( other )
 */
static bool PlainMonthDay_equals(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainMonthDay, PlainMonthDay_equals>(cx, args);
}

/**
 * Temporal.PlainMonthDay.prototype.toString ( [ options ] )
 */
static bool PlainMonthDay_toString(JSContext* cx, const CallArgs& args) {
  Rooted<PlainMonthDayObject*> monthDay(
      cx, &args.thisv().toObject().as<PlainMonthDayObject>());

  auto showCalendar = ShowCalendar::Auto;
  if (args.hasDefined(0)) {
    // Step 3.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", "toString", args[0]));
    if (!options) {
      return false;
    }

    // Step 4.
    if (!GetTemporalShowCalendarNameOption(cx, options, &showCalendar)) {
      return false;
    }
  }

  // Step 5.
  JSString* str = TemporalMonthDayToString(cx, monthDay, showCalendar);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.PlainMonthDay.prototype.toString ( [ options ] )
 */
static bool PlainMonthDay_toString(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainMonthDay, PlainMonthDay_toString>(cx,
                                                                       args);
}

/**
 * Temporal.PlainMonthDay.prototype.toLocaleString ( [ locales [ , options ] ] )
 */
static bool PlainMonthDay_toLocaleString(JSContext* cx, const CallArgs& args) {
  // Steps 3-4.
  Handle<PropertyName*> required = cx->names().date;
  Handle<PropertyName*> defaults = cx->names().date;
  return TemporalObjectToLocaleString(cx, args, required, defaults);
}

/**
 * Temporal.PlainMonthDay.prototype.toLocaleString ( [ locales [ , options ] ] )
 */
static bool PlainMonthDay_toLocaleString(JSContext* cx, unsigned argc,
                                         Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainMonthDay, PlainMonthDay_toLocaleString>(
      cx, args);
}

/**
 * Temporal.PlainMonthDay.prototype.toJSON ( )
 */
static bool PlainMonthDay_toJSON(JSContext* cx, const CallArgs& args) {
  Rooted<PlainMonthDayObject*> monthDay(
      cx, &args.thisv().toObject().as<PlainMonthDayObject>());

  // Step 3.
  JSString* str = TemporalMonthDayToString(cx, monthDay, ShowCalendar::Auto);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.PlainMonthDay.prototype.toJSON ( )
 */
static bool PlainMonthDay_toJSON(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainMonthDay, PlainMonthDay_toJSON>(cx, args);
}

/**
 *  Temporal.PlainMonthDay.prototype.valueOf ( )
 */
static bool PlainMonthDay_valueOf(JSContext* cx, unsigned argc, Value* vp) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_CANT_CONVERT_TO,
                            "PlainMonthDay", "primitive type");
  return false;
}

/**
 * Temporal.PlainMonthDay.prototype.toPlainDate ( item )
 */
static bool PlainMonthDay_toPlainDate(JSContext* cx, const CallArgs& args) {
  Rooted<PlainMonthDay> monthDay(
      cx, &args.thisv().toObject().as<PlainMonthDayObject>());

  // Step 3.
  Rooted<JSObject*> item(
      cx, RequireObjectArg(cx, "item", "toPlainDate", args.get(0)));
  if (!item) {
    return false;
  }

  // Step 4.
  auto calendar = monthDay.calendar();

  // Step 5.
  Rooted<CalendarFields> fields(cx);
  if (!ISODateToFields(cx, monthDay, &fields)) {
    return false;
  }

  // Step 6.
  Rooted<CalendarFields> inputFields(cx);
  if (!PrepareCalendarFields(cx, calendar, item,
                             {
                                 CalendarField::Year,
                             },
                             &inputFields)) {
    return false;
  }

  // Step 7.
  fields = CalendarMergeFields(calendar, fields, inputFields);

  // Step 8.
  Rooted<PlainDate> date(cx);
  if (!CalendarDateFromFields(cx, calendar, fields, TemporalOverflow::Constrain,
                              &date)) {
    return false;
  }
  MOZ_ASSERT(ISODateWithinLimits(date));

  // Step 9.
  auto* obj = CreateTemporalDate(cx, date);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.PlainMonthDay.prototype.toPlainDate ( item )
 */
static bool PlainMonthDay_toPlainDate(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsPlainMonthDay, PlainMonthDay_toPlainDate>(cx,
                                                                          args);
}

const JSClass PlainMonthDayObject::class_ = {
    "Temporal.PlainMonthDay",
    JSCLASS_HAS_RESERVED_SLOTS(PlainMonthDayObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_PlainMonthDay),
    JS_NULL_CLASS_OPS,
    &PlainMonthDayObject::classSpec_,
};

const JSClass& PlainMonthDayObject::protoClass_ = PlainObject::class_;

static const JSFunctionSpec PlainMonthDay_methods[] = {
    JS_FN("from", PlainMonthDay_from, 1, 0),
    JS_FS_END,
};

static const JSFunctionSpec PlainMonthDay_prototype_methods[] = {
    JS_FN("with", PlainMonthDay_with, 1, 0),
    JS_FN("equals", PlainMonthDay_equals, 1, 0),
    JS_FN("toString", PlainMonthDay_toString, 0, 0),
    JS_FN("toLocaleString", PlainMonthDay_toLocaleString, 0, 0),
    JS_FN("toJSON", PlainMonthDay_toJSON, 0, 0),
    JS_FN("valueOf", PlainMonthDay_valueOf, 0, 0),
    JS_FN("toPlainDate", PlainMonthDay_toPlainDate, 1, 0),
    JS_FS_END,
};

static const JSPropertySpec PlainMonthDay_prototype_properties[] = {
    JS_PSG("calendarId", PlainMonthDay_calendarId, 0),
    JS_PSG("monthCode", PlainMonthDay_monthCode, 0),
    JS_PSG("day", PlainMonthDay_day, 0),
    JS_STRING_SYM_PS(toStringTag, "Temporal.PlainMonthDay", JSPROP_READONLY),
    JS_PS_END,
};

const ClassSpec PlainMonthDayObject::classSpec_ = {
    GenericCreateConstructor<PlainMonthDayConstructor, 2,
                             gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<PlainMonthDayObject>,
    PlainMonthDay_methods,
    nullptr,
    PlainMonthDay_prototype_methods,
    PlainMonthDay_prototype_properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};
