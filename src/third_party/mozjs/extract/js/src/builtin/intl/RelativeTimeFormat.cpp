/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Implementation of the Intl.RelativeTimeFormat proposal. */

#include "builtin/intl/RelativeTimeFormat.h"

#include "mozilla/Assertions.h"
#include "mozilla/FloatingPoint.h"

#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/LanguageTag.h"
#include "builtin/intl/ScopedICUObject.h"
#include "gc/FreeOp.h"
#include "js/CharacterEncoding.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "unicode/udisplaycontext.h"
#include "unicode/uloc.h"
#include "unicode/unum.h"
#include "unicode/ureldatefmt.h"
#include "unicode/utypes.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/Printer.h"
#include "vm/StringType.h"
#include "vm/WellKnownAtom.h"  // js_*_str

#include "vm/NativeObject-inl.h"

using namespace js;

using js::intl::CallICU;
using js::intl::IcuLocale;

/**************** RelativeTimeFormat *****************/

const JSClassOps RelativeTimeFormatObject::classOps_ = {
    nullptr,                             // addProperty
    nullptr,                             // delProperty
    nullptr,                             // enumerate
    nullptr,                             // newEnumerate
    nullptr,                             // resolve
    nullptr,                             // mayResolve
    RelativeTimeFormatObject::finalize,  // finalize
    nullptr,                             // call
    nullptr,                             // hasInstance
    nullptr,                             // construct
    nullptr,                             // trace
};

const JSClass RelativeTimeFormatObject::class_ = {
    "Intl.RelativeTimeFormat",
    JSCLASS_HAS_RESERVED_SLOTS(RelativeTimeFormatObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_RelativeTimeFormat) |
        JSCLASS_FOREGROUND_FINALIZE,
    &RelativeTimeFormatObject::classOps_,
    &RelativeTimeFormatObject::classSpec_};

const JSClass& RelativeTimeFormatObject::protoClass_ = PlainObject::class_;

static bool relativeTimeFormat_toSource(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setString(cx->names().RelativeTimeFormat);
  return true;
}

static const JSFunctionSpec relativeTimeFormat_static_methods[] = {
    JS_SELF_HOSTED_FN("supportedLocalesOf",
                      "Intl_RelativeTimeFormat_supportedLocalesOf", 1, 0),
    JS_FS_END};

static const JSFunctionSpec relativeTimeFormat_methods[] = {
    JS_SELF_HOSTED_FN("resolvedOptions",
                      "Intl_RelativeTimeFormat_resolvedOptions", 0, 0),
    JS_SELF_HOSTED_FN("format", "Intl_RelativeTimeFormat_format", 2, 0),
    JS_SELF_HOSTED_FN("formatToParts", "Intl_RelativeTimeFormat_formatToParts",
                      2, 0),
    JS_FN(js_toSource_str, relativeTimeFormat_toSource, 0, 0), JS_FS_END};

static const JSPropertySpec relativeTimeFormat_properties[] = {
    JS_STRING_SYM_PS(toStringTag, "Intl.RelativeTimeFormat", JSPROP_READONLY),
    JS_PS_END};

static bool RelativeTimeFormat(JSContext* cx, unsigned argc, Value* vp);

const ClassSpec RelativeTimeFormatObject::classSpec_ = {
    GenericCreateConstructor<RelativeTimeFormat, 0, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<RelativeTimeFormatObject>,
    relativeTimeFormat_static_methods,
    nullptr,
    relativeTimeFormat_methods,
    relativeTimeFormat_properties,
    nullptr,
    ClassSpec::DontDefineConstructor};

/**
 * RelativeTimeFormat constructor.
 * Spec: ECMAScript 402 API, RelativeTimeFormat, 1.1
 */
static bool RelativeTimeFormat(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Intl.RelativeTimeFormat")) {
    return false;
  }

  // Step 2 (Inlined 9.1.14, OrdinaryCreateFromConstructor).
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_RelativeTimeFormat,
                                          &proto)) {
    return false;
  }

  Rooted<RelativeTimeFormatObject*> relativeTimeFormat(cx);
  relativeTimeFormat =
      NewObjectWithClassProto<RelativeTimeFormatObject>(cx, proto);
  if (!relativeTimeFormat) {
    return false;
  }

  HandleValue locales = args.get(0);
  HandleValue options = args.get(1);

  // Step 3.
  if (!intl::InitializeObject(cx, relativeTimeFormat,
                              cx->names().InitializeRelativeTimeFormat, locales,
                              options)) {
    return false;
  }

  args.rval().setObject(*relativeTimeFormat);
  return true;
}

void js::RelativeTimeFormatObject::finalize(JSFreeOp* fop, JSObject* obj) {
  MOZ_ASSERT(fop->onMainThread());

  if (URelativeDateTimeFormatter* rtf =
          obj->as<RelativeTimeFormatObject>().getRelativeDateTimeFormatter()) {
    intl::RemoveICUCellMemory(fop, obj,
                              RelativeTimeFormatObject::EstimatedMemoryUse);

    ureldatefmt_close(rtf);
  }
}

/**
 * Returns a new URelativeDateTimeFormatter with the locale and options of the
 * given RelativeTimeFormatObject.
 */
static URelativeDateTimeFormatter* NewURelativeDateTimeFormatter(
    JSContext* cx, Handle<RelativeTimeFormatObject*> relativeTimeFormat) {
  RootedObject internals(cx, intl::GetInternalsObject(cx, relativeTimeFormat));
  if (!internals) {
    return nullptr;
  }

  RootedValue value(cx);

  if (!GetProperty(cx, internals, internals, cx->names().locale, &value)) {
    return nullptr;
  }

  // ICU expects numberingSystem as a Unicode locale extensions on locale.

  intl::LanguageTag tag(cx);
  {
    JSLinearString* locale = value.toString()->ensureLinear(cx);
    if (!locale) {
      return nullptr;
    }

    if (!intl::LanguageTagParser::parse(cx, locale, tag)) {
      return nullptr;
    }
  }

  JS::RootedVector<intl::UnicodeExtensionKeyword> keywords(cx);

  if (!GetProperty(cx, internals, internals, cx->names().numberingSystem,
                   &value)) {
    return nullptr;
  }

  {
    JSLinearString* numberingSystem = value.toString()->ensureLinear(cx);
    if (!numberingSystem) {
      return nullptr;
    }

    if (!keywords.emplaceBack("nu", numberingSystem)) {
      return nullptr;
    }
  }

  // |ApplyUnicodeExtensionToTag| applies the new keywords to the front of the
  // Unicode extension subtag. We're then relying on ICU to follow RFC 6067,
  // which states that any trailing keywords using the same key should be
  // ignored.
  if (!intl::ApplyUnicodeExtensionToTag(cx, tag, keywords)) {
    return nullptr;
  }

  UniqueChars locale = tag.toStringZ(cx);
  if (!locale) {
    return nullptr;
  }

  if (!GetProperty(cx, internals, internals, cx->names().style, &value)) {
    return nullptr;
  }

  UDateRelativeDateTimeFormatterStyle relDateTimeStyle;
  {
    JSLinearString* style = value.toString()->ensureLinear(cx);
    if (!style) {
      return nullptr;
    }

    if (StringEqualsLiteral(style, "short")) {
      relDateTimeStyle = UDAT_STYLE_SHORT;
    } else if (StringEqualsLiteral(style, "narrow")) {
      relDateTimeStyle = UDAT_STYLE_NARROW;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(style, "long"));
      relDateTimeStyle = UDAT_STYLE_LONG;
    }
  }

  UErrorCode status = U_ZERO_ERROR;
  UNumberFormat* nf = unum_open(UNUM_DECIMAL, nullptr, 0,
                                IcuLocale(locale.get()), nullptr, &status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return nullptr;
  }
  ScopedICUObject<UNumberFormat, unum_close> toClose(nf);

  // Use the default values as if a new Intl.NumberFormat had been constructed.
  unum_setAttribute(nf, UNUM_MIN_INTEGER_DIGITS, 1);
  unum_setAttribute(nf, UNUM_MIN_FRACTION_DIGITS, 0);
  unum_setAttribute(nf, UNUM_MAX_FRACTION_DIGITS, 3);
  unum_setAttribute(nf, UNUM_GROUPING_USED, true);

  // The undocumented magic value -2 is needed to request locale-specific data.
  // See |icu::number::impl::Grouper::{fGrouping1, fGrouping2, fMinGrouping}|.
  //
  // Future ICU versions (> ICU 67) will expose it as a proper constant:
  // https://unicode-org.atlassian.net/browse/ICU-21109
  // https://github.com/unicode-org/icu/pull/1152
  constexpr int32_t useLocaleData = -2;

  unum_setAttribute(nf, UNUM_GROUPING_SIZE, useLocaleData);
  unum_setAttribute(nf, UNUM_SECONDARY_GROUPING_SIZE, useLocaleData);
  unum_setAttribute(nf, UNUM_MINIMUM_GROUPING_DIGITS, useLocaleData);

  URelativeDateTimeFormatter* rtf =
      ureldatefmt_open(IcuLocale(locale.get()), nf, relDateTimeStyle,
                       UDISPCTX_CAPITALIZATION_FOR_STANDALONE, &status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return nullptr;
  }

  // Ownership was transferred to the URelativeDateTimeFormatter.
  toClose.forget();
  return rtf;
}

enum class RelativeTimeNumeric {
  /**
   * Only strings with numeric components like `1 day ago`.
   */
  Always,
  /**
   * Natural-language strings like `yesterday` when possible,
   * otherwise strings with numeric components as in `7 months ago`.
   */
  Auto,
};

static bool intl_FormatRelativeTime(JSContext* cx,
                                    URelativeDateTimeFormatter* rtf, double t,
                                    URelativeDateTimeUnit unit,
                                    RelativeTimeNumeric numeric,
                                    MutableHandleValue result) {
  JSString* str = CallICU(
      cx,
      [rtf, t, unit, numeric](UChar* chars, int32_t size, UErrorCode* status) {
        auto fmt = numeric == RelativeTimeNumeric::Auto
                       ? ureldatefmt_format
                       : ureldatefmt_formatNumeric;
        return fmt(rtf, t, unit, chars, size, status);
      });
  if (!str) {
    return false;
  }

  result.setString(str);
  return true;
}

static bool intl_FormatToPartsRelativeTime(JSContext* cx,
                                           URelativeDateTimeFormatter* rtf,
                                           double t, URelativeDateTimeUnit unit,
                                           RelativeTimeNumeric numeric,
                                           MutableHandleValue result) {
  UErrorCode status = U_ZERO_ERROR;
  UFormattedRelativeDateTime* formatted = ureldatefmt_openResult(&status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return false;
  }
  ScopedICUObject<UFormattedRelativeDateTime, ureldatefmt_closeResult> toClose(
      formatted);

  if (numeric == RelativeTimeNumeric::Auto) {
    ureldatefmt_formatToResult(rtf, t, unit, formatted, &status);
  } else {
    ureldatefmt_formatNumericToResult(rtf, t, unit, formatted, &status);
  }
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return false;
  }

  const UFormattedValue* formattedValue =
      ureldatefmt_resultAsValue(formatted, &status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return false;
  }

  intl::FieldType unitType;
  switch (unit) {
    case UDAT_REL_UNIT_SECOND:
      unitType = &JSAtomState::second;
      break;
    case UDAT_REL_UNIT_MINUTE:
      unitType = &JSAtomState::minute;
      break;
    case UDAT_REL_UNIT_HOUR:
      unitType = &JSAtomState::hour;
      break;
    case UDAT_REL_UNIT_DAY:
      unitType = &JSAtomState::day;
      break;
    case UDAT_REL_UNIT_WEEK:
      unitType = &JSAtomState::week;
      break;
    case UDAT_REL_UNIT_MONTH:
      unitType = &JSAtomState::month;
      break;
    case UDAT_REL_UNIT_QUARTER:
      unitType = &JSAtomState::quarter;
      break;
    case UDAT_REL_UNIT_YEAR:
      unitType = &JSAtomState::year;
      break;
    default:
      MOZ_CRASH("unexpected relative time unit");
  }

  return intl::FormattedRelativeTimeToParts(cx, formattedValue, t, unitType,
                                            result);
}

bool js::intl_FormatRelativeTime(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 5);

  Rooted<RelativeTimeFormatObject*> relativeTimeFormat(cx);
  relativeTimeFormat = &args[0].toObject().as<RelativeTimeFormatObject>();

  bool formatToParts = args[4].toBoolean();

  // PartitionRelativeTimePattern, step 4.
  double t = args[1].toNumber();
  if (!mozilla::IsFinite(t)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DATE_NOT_FINITE, "RelativeTimeFormat",
                              formatToParts ? "formatToParts" : "format");
    return false;
  }

  // Obtain a cached URelativeDateTimeFormatter object.
  URelativeDateTimeFormatter* rtf =
      relativeTimeFormat->getRelativeDateTimeFormatter();
  if (!rtf) {
    rtf = NewURelativeDateTimeFormatter(cx, relativeTimeFormat);
    if (!rtf) {
      return false;
    }
    relativeTimeFormat->setRelativeDateTimeFormatter(rtf);

    intl::AddICUCellMemory(relativeTimeFormat,
                           RelativeTimeFormatObject::EstimatedMemoryUse);
  }

  URelativeDateTimeUnit relDateTimeUnit;
  {
    JSLinearString* unit = args[2].toString()->ensureLinear(cx);
    if (!unit) {
      return false;
    }

    // PartitionRelativeTimePattern, step 5.
    if (StringEqualsLiteral(unit, "second") ||
        StringEqualsLiteral(unit, "seconds")) {
      relDateTimeUnit = UDAT_REL_UNIT_SECOND;
    } else if (StringEqualsLiteral(unit, "minute") ||
               StringEqualsLiteral(unit, "minutes")) {
      relDateTimeUnit = UDAT_REL_UNIT_MINUTE;
    } else if (StringEqualsLiteral(unit, "hour") ||
               StringEqualsLiteral(unit, "hours")) {
      relDateTimeUnit = UDAT_REL_UNIT_HOUR;
    } else if (StringEqualsLiteral(unit, "day") ||
               StringEqualsLiteral(unit, "days")) {
      relDateTimeUnit = UDAT_REL_UNIT_DAY;
    } else if (StringEqualsLiteral(unit, "week") ||
               StringEqualsLiteral(unit, "weeks")) {
      relDateTimeUnit = UDAT_REL_UNIT_WEEK;
    } else if (StringEqualsLiteral(unit, "month") ||
               StringEqualsLiteral(unit, "months")) {
      relDateTimeUnit = UDAT_REL_UNIT_MONTH;
    } else if (StringEqualsLiteral(unit, "quarter") ||
               StringEqualsLiteral(unit, "quarters")) {
      relDateTimeUnit = UDAT_REL_UNIT_QUARTER;
    } else if (StringEqualsLiteral(unit, "year") ||
               StringEqualsLiteral(unit, "years")) {
      relDateTimeUnit = UDAT_REL_UNIT_YEAR;
    } else {
      if (auto unitChars = QuoteString(cx, unit, '"')) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_INVALID_OPTION_VALUE, "unit",
                                  unitChars.get());
      }
      return false;
    }
  }

  RelativeTimeNumeric relDateTimeNumeric;
  {
    JSLinearString* numeric = args[3].toString()->ensureLinear(cx);
    if (!numeric) {
      return false;
    }

    if (StringEqualsLiteral(numeric, "auto")) {
      relDateTimeNumeric = RelativeTimeNumeric::Auto;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(numeric, "always"));
      relDateTimeNumeric = RelativeTimeNumeric::Always;
    }
  }

  return formatToParts
             ? intl_FormatToPartsRelativeTime(cx, rtf, t, relDateTimeUnit,
                                              relDateTimeNumeric, args.rval())
             : intl_FormatRelativeTime(cx, rtf, t, relDateTimeUnit,
                                       relDateTimeNumeric, args.rval());
}
