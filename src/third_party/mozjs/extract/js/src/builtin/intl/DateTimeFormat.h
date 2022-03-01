/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_DateTimeFormat_h
#define builtin_intl_DateTimeFormat_h

#include "builtin/intl/CommonFunctions.h"
#include "builtin/SelfHostingDefines.h"
#include "js/Class.h"
#include "js/RootingAPI.h"
#include "vm/NativeObject.h"

struct UDateIntervalFormat;

namespace mozilla::intl {
class DateTimeFormat;
}

namespace js {

class DateTimeFormatObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  static constexpr uint32_t INTERNALS_SLOT = 0;
  static constexpr uint32_t UDATE_FORMAT_SLOT = 1;
  static constexpr uint32_t UDATE_INTERVAL_FORMAT_SLOT = 2;
  static constexpr uint32_t SLOT_COUNT = 3;

  static_assert(INTERNALS_SLOT == INTL_INTERNALS_OBJECT_SLOT,
                "INTERNALS_SLOT must match self-hosting define for internals "
                "object slot");

  // Estimated memory use for UDateFormat (see IcuMemoryUsage).
  static constexpr size_t UDateFormatEstimatedMemoryUse = 91922;

  // Estimated memory use for UDateIntervalFormat (see IcuMemoryUsage).
  static constexpr size_t UDateIntervalFormatEstimatedMemoryUse = 119856;

  mozilla::intl::DateTimeFormat* getDateFormat() const {
    const auto& slot = getFixedSlot(UDATE_FORMAT_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return static_cast<mozilla::intl::DateTimeFormat*>(slot.toPrivate());
  }

  void setDateFormat(mozilla::intl::DateTimeFormat* dateFormat) {
    setFixedSlot(UDATE_FORMAT_SLOT, PrivateValue(dateFormat));
  }

  UDateIntervalFormat* getDateIntervalFormat() const {
    const auto& slot = getFixedSlot(UDATE_INTERVAL_FORMAT_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return static_cast<UDateIntervalFormat*>(slot.toPrivate());
  }

  void setDateIntervalFormat(UDateIntervalFormat* dateIntervalFormat) {
    setFixedSlot(UDATE_INTERVAL_FORMAT_SLOT, PrivateValue(dateIntervalFormat));
  }

 private:
  static const JSClassOps classOps_;
  static const ClassSpec classSpec_;

  static void finalize(JSFreeOp* fop, JSObject* obj);
};

/**
 * Returns a new instance of the standard built-in DateTimeFormat constructor.
 * Self-hosted code cannot cache this constructor (as it does for others in
 * Utilities.js) because it is initialized after self-hosted code is compiled.
 *
 * Usage: dateTimeFormat = intl_DateTimeFormat(locales, options)
 */
[[nodiscard]] extern bool intl_DateTimeFormat(JSContext* cx, unsigned argc,
                                              JS::Value* vp);

/**
 * Returns an array with the calendar type identifiers per Unicode
 * Technical Standard 35, Unicode Locale Data Markup Language, for the
 * supported calendars for the given locale. The default calendar is
 * element 0.
 *
 * Usage: calendars = intl_availableCalendars(locale)
 */
[[nodiscard]] extern bool intl_availableCalendars(JSContext* cx, unsigned argc,
                                                  JS::Value* vp);

/**
 * Returns the calendar type identifier per Unicode Technical Standard 35,
 * Unicode Locale Data Markup Language, for the default calendar for the given
 * locale.
 *
 * Usage: calendar = intl_defaultCalendar(locale)
 */
[[nodiscard]] extern bool intl_defaultCalendar(JSContext* cx, unsigned argc,
                                               JS::Value* vp);

/**
 * 6.4.1 IsValidTimeZoneName ( timeZone )
 *
 * Verifies that the given string is a valid time zone name. If it is a valid
 * time zone name, its IANA time zone name is returned. Otherwise returns null.
 *
 * ES2017 Intl draft rev 4a23f407336d382ed5e3471200c690c9b020b5f3
 *
 * Usage: ianaTimeZone = intl_IsValidTimeZoneName(timeZone)
 */
[[nodiscard]] extern bool intl_IsValidTimeZoneName(JSContext* cx, unsigned argc,
                                                   JS::Value* vp);

/**
 * Return the canonicalized time zone name. Canonicalization resolves link
 * names to their target time zones.
 *
 * Usage: ianaTimeZone = intl_canonicalizeTimeZone(timeZone)
 */
[[nodiscard]] extern bool intl_canonicalizeTimeZone(JSContext* cx,
                                                    unsigned argc,
                                                    JS::Value* vp);

/**
 * Return the default time zone name. The time zone name is not canonicalized.
 *
 * Usage: icuDefaultTimeZone = intl_defaultTimeZone()
 */
[[nodiscard]] extern bool intl_defaultTimeZone(JSContext* cx, unsigned argc,
                                               JS::Value* vp);

/**
 * Return the raw offset from GMT in milliseconds for the default time zone.
 *
 * Usage: defaultTimeZoneOffset = intl_defaultTimeZoneOffset()
 */
[[nodiscard]] extern bool intl_defaultTimeZoneOffset(JSContext* cx,
                                                     unsigned argc,
                                                     JS::Value* vp);

/**
 * Return true if the given string is the default time zone as returned by
 * intl_defaultTimeZone(). Otherwise return false.
 *
 * Usage: isIcuDefaultTimeZone = intl_isDefaultTimeZone(icuDefaultTimeZone)
 */
[[nodiscard]] extern bool intl_isDefaultTimeZone(JSContext* cx, unsigned argc,
                                                 JS::Value* vp);

/**
 * Return a pattern in the date-time format pattern language of Unicode
 * Technical Standard 35, Unicode Locale Data Markup Language, for the
 * best-fit date-time format pattern corresponding to skeleton for the
 * given locale.
 *
 * Usage: pattern = intl_patternForSkeleton(locale, skeleton, hourCycle)
 */
[[nodiscard]] extern bool intl_patternForSkeleton(JSContext* cx, unsigned argc,
                                                  JS::Value* vp);

/**
 * Return a pattern in the date-time format pattern language of Unicode
 * Technical Standard 35, Unicode Locale Data Markup Language, for the
 * best-fit date-time style for the given locale.
 * The function takes six arguments:
 *
 *   locale
 *     BCP47 compliant locale string
 *   dateStyle
 *     A string with values: full or long or medium or short, or `undefined`
 *   timeStyle
 *     A string with values: full or long or medium or short, or `undefined`
 *   timeZone
 *     IANA time zone name
 *   hour12
 *     A boolean to request hour12 representation, or `undefined`
 *   hourCycle
 *     A string with values: h11, h12, h23, or h24, or `undefined`
 *
 * Date and time style categories map to CLDR time/date standard
 * format patterns.
 *
 * For the definition of a pattern string, see LDML 4.8:
 * http://unicode.org/reports/tr35/tr35-dates.html#Date_Format_Patterns
 *
 * If `undefined` is passed to `dateStyle` or `timeStyle`, the respective
 * portions of the pattern will not be included in the result.
 *
 * Usage: pattern = intl_patternForStyle(locale, dateStyle, timeStyle, timeZone,
 *                                       hour12, hourCycle)
 */
[[nodiscard]] extern bool intl_patternForStyle(JSContext* cx, unsigned argc,
                                               JS::Value* vp);

/**
 * Return a skeleton for the pattern in the date-time format pattern language of
 * Unicode Technical Standard 35, Unicode Locale Data Markup Language.
 *
 * Usage: skeleton = intl_skeletonForPattern(pattern)
 */
[[nodiscard]] extern bool intl_skeletonForPattern(JSContext* cx, unsigned argc,
                                                  JS::Value* vp);

/**
 * Returns a String value representing x (which must be a Number value)
 * according to the effective locale and the formatting options of the
 * given DateTimeFormat.
 *
 * Spec: ECMAScript Internationalization API Specification, 12.3.2.
 *
 * Usage: formatted = intl_FormatDateTime(dateTimeFormat, x, formatToParts)
 */
[[nodiscard]] extern bool intl_FormatDateTime(JSContext* cx, unsigned argc,
                                              JS::Value* vp);

/**
 * Returns a String value representing the range between x and y (which both
 * must be Number values) according to the effective locale and the formatting
 * options of the given DateTimeFormat.
 *
 * Spec: Intl.DateTimeFormat.prototype.formatRange proposal
 *
 * Usage: formatted = intl_FormatDateTimeRange(dateTimeFmt, x, y, formatToParts)
 */
[[nodiscard]] extern bool intl_FormatDateTimeRange(JSContext* cx, unsigned argc,
                                                   JS::Value* vp);

}  // namespace js

#endif /* builtin_intl_DateTimeFormat_h */
