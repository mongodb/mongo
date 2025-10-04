/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Implementation of the Intl.RelativeTimeFormat proposal. */

#include "builtin/intl/RelativeTimeFormat.h"

#include "mozilla/Assertions.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/intl/RelativeTimeFormat.h"

#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/FormatBuffer.h"
#include "builtin/intl/LanguageTag.h"
#include "gc/GCContext.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Printer.h"
#include "js/PropertySpec.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/StringType.h"

#include "vm/NativeObject-inl.h"

using namespace js;

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
    JS_FN("toSource", relativeTimeFormat_toSource, 0, 0), JS_FS_END};

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

void js::RelativeTimeFormatObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  MOZ_ASSERT(gcx->onMainThread());

  if (mozilla::intl::RelativeTimeFormat* rtf =
          obj->as<RelativeTimeFormatObject>().getRelativeTimeFormatter()) {
    intl::RemoveICUCellMemory(gcx, obj,
                              RelativeTimeFormatObject::EstimatedMemoryUse);

    // This was allocated using `new` in mozilla::intl::RelativeTimeFormat,
    // so we delete here.
    delete rtf;
  }
}

/**
 * Returns a new URelativeDateTimeFormatter with the locale and options of the
 * given RelativeTimeFormatObject.
 */
static mozilla::intl::RelativeTimeFormat* NewRelativeTimeFormatter(
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

  mozilla::intl::Locale tag;
  {
    Rooted<JSLinearString*> locale(cx, value.toString()->ensureLinear(cx));
    if (!locale) {
      return nullptr;
    }

    if (!intl::ParseLocale(cx, locale, tag)) {
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

  intl::FormatBuffer<char> buffer(cx);
  if (auto result = tag.ToString(buffer); result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return nullptr;
  }

  UniqueChars locale = buffer.extractStringZ();
  if (!locale) {
    return nullptr;
  }

  if (!GetProperty(cx, internals, internals, cx->names().style, &value)) {
    return nullptr;
  }

  using RelativeTimeFormatOptions = mozilla::intl::RelativeTimeFormatOptions;
  RelativeTimeFormatOptions options;
  {
    JSLinearString* style = value.toString()->ensureLinear(cx);
    if (!style) {
      return nullptr;
    }

    if (StringEqualsLiteral(style, "short")) {
      options.style = RelativeTimeFormatOptions::Style::Short;
    } else if (StringEqualsLiteral(style, "narrow")) {
      options.style = RelativeTimeFormatOptions::Style::Narrow;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(style, "long"));
      options.style = RelativeTimeFormatOptions::Style::Long;
    }
  }

  if (!GetProperty(cx, internals, internals, cx->names().numeric, &value)) {
    return nullptr;
  }

  {
    JSLinearString* numeric = value.toString()->ensureLinear(cx);
    if (!numeric) {
      return nullptr;
    }

    if (StringEqualsLiteral(numeric, "auto")) {
      options.numeric = RelativeTimeFormatOptions::Numeric::Auto;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(numeric, "always"));
      options.numeric = RelativeTimeFormatOptions::Numeric::Always;
    }
  }

  using RelativeTimeFormat = mozilla::intl::RelativeTimeFormat;
  mozilla::Result<mozilla::UniquePtr<RelativeTimeFormat>,
                  mozilla::intl::ICUError>
      result = RelativeTimeFormat::TryCreate(locale.get(), options);

  if (result.isOk()) {
    return result.unwrap().release();
  }

  intl::ReportInternalError(cx, result.unwrapErr());
  return nullptr;
}

static mozilla::intl::RelativeTimeFormat* GetOrCreateRelativeTimeFormat(
    JSContext* cx, Handle<RelativeTimeFormatObject*> relativeTimeFormat) {
  // Obtain a cached RelativeDateTimeFormatter object.
  mozilla::intl::RelativeTimeFormat* rtf =
      relativeTimeFormat->getRelativeTimeFormatter();
  if (rtf) {
    return rtf;
  }

  rtf = NewRelativeTimeFormatter(cx, relativeTimeFormat);
  if (!rtf) {
    return nullptr;
  }
  relativeTimeFormat->setRelativeTimeFormatter(rtf);

  intl::AddICUCellMemory(relativeTimeFormat,
                         RelativeTimeFormatObject::EstimatedMemoryUse);
  return rtf;
}

bool js::intl_FormatRelativeTime(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 4);
  MOZ_ASSERT(args[0].isObject());
  MOZ_ASSERT(args[1].isNumber());
  MOZ_ASSERT(args[2].isString());
  MOZ_ASSERT(args[3].isBoolean());

  Rooted<RelativeTimeFormatObject*> relativeTimeFormat(cx);
  relativeTimeFormat = &args[0].toObject().as<RelativeTimeFormatObject>();

  bool formatToParts = args[3].toBoolean();

  // PartitionRelativeTimePattern, step 4.
  double t = args[1].toNumber();
  if (!std::isfinite(t)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DATE_NOT_FINITE, "RelativeTimeFormat",
                              formatToParts ? "formatToParts" : "format");
    return false;
  }

  mozilla::intl::RelativeTimeFormat* rtf =
      GetOrCreateRelativeTimeFormat(cx, relativeTimeFormat);
  if (!rtf) {
    return false;
  }

  intl::FieldType jsUnitType;
  using FormatUnit = mozilla::intl::RelativeTimeFormat::FormatUnit;
  FormatUnit relTimeUnit;
  {
    JSLinearString* unit = args[2].toString()->ensureLinear(cx);
    if (!unit) {
      return false;
    }

    // PartitionRelativeTimePattern, step 5.
    if (StringEqualsLiteral(unit, "second") ||
        StringEqualsLiteral(unit, "seconds")) {
      jsUnitType = &JSAtomState::second;
      relTimeUnit = FormatUnit::Second;
    } else if (StringEqualsLiteral(unit, "minute") ||
               StringEqualsLiteral(unit, "minutes")) {
      jsUnitType = &JSAtomState::minute;
      relTimeUnit = FormatUnit::Minute;
    } else if (StringEqualsLiteral(unit, "hour") ||
               StringEqualsLiteral(unit, "hours")) {
      jsUnitType = &JSAtomState::hour;
      relTimeUnit = FormatUnit::Hour;
    } else if (StringEqualsLiteral(unit, "day") ||
               StringEqualsLiteral(unit, "days")) {
      jsUnitType = &JSAtomState::day;
      relTimeUnit = FormatUnit::Day;
    } else if (StringEqualsLiteral(unit, "week") ||
               StringEqualsLiteral(unit, "weeks")) {
      jsUnitType = &JSAtomState::week;
      relTimeUnit = FormatUnit::Week;
    } else if (StringEqualsLiteral(unit, "month") ||
               StringEqualsLiteral(unit, "months")) {
      jsUnitType = &JSAtomState::month;
      relTimeUnit = FormatUnit::Month;
    } else if (StringEqualsLiteral(unit, "quarter") ||
               StringEqualsLiteral(unit, "quarters")) {
      jsUnitType = &JSAtomState::quarter;
      relTimeUnit = FormatUnit::Quarter;
    } else if (StringEqualsLiteral(unit, "year") ||
               StringEqualsLiteral(unit, "years")) {
      jsUnitType = &JSAtomState::year;
      relTimeUnit = FormatUnit::Year;
    } else {
      if (auto unitChars = QuoteString(cx, unit, '"')) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_INVALID_OPTION_VALUE, "unit",
                                  unitChars.get());
      }
      return false;
    }
  }

  using ICUError = mozilla::intl::ICUError;
  if (formatToParts) {
    mozilla::intl::NumberPartVector parts;
    mozilla::Result<mozilla::Span<const char16_t>, ICUError> result =
        rtf->formatToParts(t, relTimeUnit, parts);

    if (result.isErr()) {
      intl::ReportInternalError(cx, result.unwrapErr());
      return false;
    }

    RootedString str(cx, NewStringCopy<CanGC>(cx, result.unwrap()));
    if (!str) {
      return false;
    }

    return js::intl::FormattedRelativeTimeToParts(cx, str, parts, jsUnitType,
                                                  args.rval());
  }

  js::intl::FormatBuffer<char16_t, intl::INITIAL_CHAR_BUFFER_SIZE> buffer(cx);
  mozilla::Result<Ok, ICUError> result = rtf->format(t, relTimeUnit, buffer);

  if (result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return false;
  }

  JSString* str = buffer.toString(cx);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}
