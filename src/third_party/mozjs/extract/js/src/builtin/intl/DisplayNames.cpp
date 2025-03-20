/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Intl.DisplayNames implementation. */

#include "builtin/intl/DisplayNames.h"

#include "mozilla/Assertions.h"
#include "mozilla/intl/DisplayNames.h"
#include "mozilla/PodOperations.h"
#include "mozilla/Span.h"

#include <algorithm>

#include "jsnum.h"
#include "jspubtd.h"

#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/FormatBuffer.h"
#include "gc/AllocKind.h"
#include "gc/GCContext.h"
#include "js/CallArgs.h"
#include "js/Class.h"
#include "js/experimental/Intl.h"     // JS::AddMozDisplayNamesConstructor
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Printer.h"
#include "js/PropertyAndElement.h"  // JS_DefineFunctions, JS_DefineProperties
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Utility.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/Runtime.h"
#include "vm/SelfHosting.h"
#include "vm/Stack.h"
#include "vm/StringType.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

const JSClassOps DisplayNamesObject::classOps_ = {nullptr, /* addProperty */
                                                  nullptr, /* delProperty */
                                                  nullptr, /* enumerate */
                                                  nullptr, /* newEnumerate */
                                                  nullptr, /* resolve */
                                                  nullptr, /* mayResolve */
                                                  DisplayNamesObject::finalize};

const JSClass DisplayNamesObject::class_ = {
    "Intl.DisplayNames",
    JSCLASS_HAS_RESERVED_SLOTS(DisplayNamesObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_DisplayNames) |
        JSCLASS_FOREGROUND_FINALIZE,
    &DisplayNamesObject::classOps_, &DisplayNamesObject::classSpec_};

const JSClass& DisplayNamesObject::protoClass_ = PlainObject::class_;

static bool displayNames_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setString(cx->names().DisplayNames);
  return true;
}

static const JSFunctionSpec displayNames_static_methods[] = {
    JS_SELF_HOSTED_FN("supportedLocalesOf",
                      "Intl_DisplayNames_supportedLocalesOf", 1, 0),
    JS_FS_END};

static const JSFunctionSpec displayNames_methods[] = {
    JS_SELF_HOSTED_FN("of", "Intl_DisplayNames_of", 1, 0),
    JS_SELF_HOSTED_FN("resolvedOptions", "Intl_DisplayNames_resolvedOptions", 0,
                      0),
    JS_FN("toSource", displayNames_toSource, 0, 0), JS_FS_END};

static const JSPropertySpec displayNames_properties[] = {
    JS_STRING_SYM_PS(toStringTag, "Intl.DisplayNames", JSPROP_READONLY),
    JS_PS_END};

static bool DisplayNames(JSContext* cx, unsigned argc, Value* vp);

const ClassSpec DisplayNamesObject::classSpec_ = {
    GenericCreateConstructor<DisplayNames, 2, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<DisplayNamesObject>,
    displayNames_static_methods,
    nullptr,
    displayNames_methods,
    displayNames_properties,
    nullptr,
    ClassSpec::DontDefineConstructor};

enum class DisplayNamesOptions {
  Standard,

  // Calendar display names are no longer available with the current spec
  // proposal text, but may be re-enabled in the future. For our internal use
  // we still need to have them present, so use a feature guard for now.
  EnableMozExtensions,
};

/**
 * Initialize a new Intl.DisplayNames object using the named self-hosted
 * function.
 */
static bool InitializeDisplayNamesObject(JSContext* cx, HandleObject obj,
                                         Handle<PropertyName*> initializer,
                                         HandleValue locales,
                                         HandleValue options,
                                         DisplayNamesOptions dnoptions) {
  FixedInvokeArgs<4> args(cx);

  args[0].setObject(*obj);
  args[1].set(locales);
  args[2].set(options);
  args[3].setBoolean(dnoptions == DisplayNamesOptions::EnableMozExtensions);

  RootedValue ignored(cx);
  if (!CallSelfHostedFunction(cx, initializer, NullHandleValue, args,
                              &ignored)) {
    return false;
  }

  MOZ_ASSERT(ignored.isUndefined(),
             "Unexpected return value from non-legacy Intl object initializer");
  return true;
}

/**
 * Intl.DisplayNames ([ locales [ , options ]])
 */
static bool DisplayNames(JSContext* cx, const CallArgs& args,
                         DisplayNamesOptions dnoptions) {
  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Intl.DisplayNames")) {
    return false;
  }

  // Step 2 (Inlined 9.1.14, OrdinaryCreateFromConstructor).
  RootedObject proto(cx);
  if (dnoptions == DisplayNamesOptions::Standard) {
    if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_DisplayNames,
                                            &proto)) {
      return false;
    }
  } else {
    RootedObject newTarget(cx, &args.newTarget().toObject());
    if (!GetPrototypeFromConstructor(cx, newTarget, JSProto_Null, &proto)) {
      return false;
    }
  }

  Rooted<DisplayNamesObject*> displayNames(cx);
  displayNames = NewObjectWithClassProto<DisplayNamesObject>(cx, proto);
  if (!displayNames) {
    return false;
  }

  HandleValue locales = args.get(0);
  HandleValue options = args.get(1);

  // Steps 3-26.
  if (!InitializeDisplayNamesObject(cx, displayNames,
                                    cx->names().InitializeDisplayNames, locales,
                                    options, dnoptions)) {
    return false;
  }

  // Step 27.
  args.rval().setObject(*displayNames);
  return true;
}

static bool DisplayNames(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return DisplayNames(cx, args, DisplayNamesOptions::Standard);
}

static bool MozDisplayNames(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return DisplayNames(cx, args, DisplayNamesOptions::EnableMozExtensions);
}

void js::DisplayNamesObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  MOZ_ASSERT(gcx->onMainThread());

  if (mozilla::intl::DisplayNames* displayNames =
          obj->as<DisplayNamesObject>().getDisplayNames()) {
    intl::RemoveICUCellMemory(gcx, obj, DisplayNamesObject::EstimatedMemoryUse);
    delete displayNames;
  }
}

bool JS::AddMozDisplayNamesConstructor(JSContext* cx, HandleObject intl) {
  RootedObject ctor(cx, GlobalObject::createConstructor(
                            cx, MozDisplayNames, cx->names().DisplayNames, 2));
  if (!ctor) {
    return false;
  }

  RootedObject proto(
      cx, GlobalObject::createBlankPrototype<PlainObject>(cx, cx->global()));
  if (!proto) {
    return false;
  }

  if (!LinkConstructorAndPrototype(cx, ctor, proto)) {
    return false;
  }

  if (!JS_DefineFunctions(cx, ctor, displayNames_static_methods)) {
    return false;
  }

  if (!JS_DefineFunctions(cx, proto, displayNames_methods)) {
    return false;
  }

  if (!JS_DefineProperties(cx, proto, displayNames_properties)) {
    return false;
  }

  RootedValue ctorValue(cx, ObjectValue(*ctor));
  return DefineDataProperty(cx, intl, cx->names().DisplayNames, ctorValue, 0);
}

static mozilla::intl::DisplayNames* NewDisplayNames(
    JSContext* cx, const char* locale,
    mozilla::intl::DisplayNames::Options& options) {
  auto result = mozilla::intl::DisplayNames::TryCreate(locale, options);
  if (result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return nullptr;
  }
  return result.unwrap().release();
}

static mozilla::intl::DisplayNames* GetOrCreateDisplayNames(
    JSContext* cx, Handle<DisplayNamesObject*> displayNames, const char* locale,
    mozilla::intl::DisplayNames::Options& options) {
  // Obtain a cached mozilla::intl::DisplayNames object.
  mozilla::intl::DisplayNames* dn = displayNames->getDisplayNames();
  if (!dn) {
    dn = NewDisplayNames(cx, locale, options);
    if (!dn) {
      return nullptr;
    }
    displayNames->setDisplayNames(dn);

    intl::AddICUCellMemory(displayNames,
                           DisplayNamesObject::EstimatedMemoryUse);
  }
  return dn;
}

static void ReportInvalidOptionError(JSContext* cx, HandleString type,
                                     HandleString option) {
  if (UniqueChars optionStr = QuoteString(cx, option, '"')) {
    if (UniqueChars typeStr = QuoteString(cx, type)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_INVALID_OPTION_VALUE, typeStr.get(),
                                optionStr.get());
    }
  }
}

static void ReportInvalidOptionError(JSContext* cx, const char* type,
                                     HandleString option) {
  if (UniqueChars str = QuoteString(cx, option, '"')) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INVALID_OPTION_VALUE, type, str.get());
  }
}

static void ReportInvalidOptionError(JSContext* cx, const char* type,
                                     double option) {
  ToCStringBuf cbuf;
  const char* str = NumberToCString(&cbuf, option);
  MOZ_ASSERT(str);
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_INVALID_DIGITS_VALUE, str);
}

/**
 * intl_ComputeDisplayName(displayNames, locale, calendar, style,
 *                         languageDisplay, fallback, type, code)
 */
bool js::intl_ComputeDisplayName(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 8);

  Rooted<DisplayNamesObject*> displayNames(
      cx, &args[0].toObject().as<DisplayNamesObject>());

  UniqueChars locale = intl::EncodeLocale(cx, args[1].toString());
  if (!locale) {
    return false;
  }

  Rooted<JSLinearString*> calendar(cx, args[2].toString()->ensureLinear(cx));
  if (!calendar) {
    return false;
  }

  Rooted<JSLinearString*> code(cx, args[7].toString()->ensureLinear(cx));
  if (!code) {
    return false;
  }

  mozilla::intl::DisplayNames::Style style;
  {
    JSLinearString* styleStr = args[3].toString()->ensureLinear(cx);
    if (!styleStr) {
      return false;
    }

    if (StringEqualsLiteral(styleStr, "long")) {
      style = mozilla::intl::DisplayNames::Style::Long;
    } else if (StringEqualsLiteral(styleStr, "short")) {
      style = mozilla::intl::DisplayNames::Style::Short;
    } else if (StringEqualsLiteral(styleStr, "narrow")) {
      style = mozilla::intl::DisplayNames::Style::Narrow;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(styleStr, "abbreviated"));
      style = mozilla::intl::DisplayNames::Style::Abbreviated;
    }
  }

  mozilla::intl::DisplayNames::LanguageDisplay languageDisplay;
  {
    JSLinearString* language = args[4].toString()->ensureLinear(cx);
    if (!language) {
      return false;
    }

    if (StringEqualsLiteral(language, "dialect")) {
      languageDisplay = mozilla::intl::DisplayNames::LanguageDisplay::Dialect;
    } else {
      MOZ_ASSERT(language->empty() ||
                 StringEqualsLiteral(language, "standard"));
      languageDisplay = mozilla::intl::DisplayNames::LanguageDisplay::Standard;
    }
  }

  mozilla::intl::DisplayNames::Fallback fallback;
  {
    JSLinearString* fallbackStr = args[5].toString()->ensureLinear(cx);
    if (!fallbackStr) {
      return false;
    }

    if (StringEqualsLiteral(fallbackStr, "none")) {
      fallback = mozilla::intl::DisplayNames::Fallback::None;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(fallbackStr, "code"));
      fallback = mozilla::intl::DisplayNames::Fallback::Code;
    }
  }

  Rooted<JSLinearString*> type(cx, args[6].toString()->ensureLinear(cx));
  if (!type) {
    return false;
  }

  mozilla::intl::DisplayNames::Options options{
      style,
      languageDisplay,
  };

  // If a calendar exists, set it as an option.
  JS::UniqueChars calendarChars = nullptr;
  if (!calendar->empty()) {
    calendarChars = JS_EncodeStringToUTF8(cx, calendar);
    if (!calendarChars) {
      return false;
    }
  }

  mozilla::intl::DisplayNames* dn =
      GetOrCreateDisplayNames(cx, displayNames, locale.get(), options);
  if (!dn) {
    return false;
  }

  // The "code" is usually a small ASCII string, so try to avoid an allocation
  // by copying it to the stack. Unfortunately we can't pass a string span of
  // the JSString directly to the unified DisplayNames API, as the
  // intl::FormatBuffer will be written to. This writing can trigger a GC and
  // invalidate the span, creating a nogc rooting hazard.
  JS::UniqueChars utf8 = nullptr;
  unsigned char ascii[32];
  mozilla::Span<const char> codeSpan = nullptr;
  if (code->length() < 32 && code->hasLatin1Chars() && StringIsAscii(code)) {
    JS::AutoCheckCannotGC nogc;
    mozilla::PodCopy(ascii, code->latin1Chars(nogc), code->length());
    codeSpan =
        mozilla::Span(reinterpret_cast<const char*>(ascii), code->length());
  } else {
    utf8 = JS_EncodeStringToUTF8(cx, code);
    if (!utf8) {
      return false;
    }
    codeSpan = mozilla::MakeStringSpan(utf8.get());
  }

  intl::FormatBuffer<char16_t, intl::INITIAL_CHAR_BUFFER_SIZE> buffer(cx);
  mozilla::Result<mozilla::Ok, mozilla::intl::DisplayNamesError> result =
      mozilla::Ok{};

  if (StringEqualsLiteral(type, "language")) {
    result = dn->GetLanguage(buffer, codeSpan, fallback);
  } else if (StringEqualsLiteral(type, "script")) {
    result = dn->GetScript(buffer, codeSpan, fallback);
  } else if (StringEqualsLiteral(type, "region")) {
    result = dn->GetRegion(buffer, codeSpan, fallback);
  } else if (StringEqualsLiteral(type, "currency")) {
    result = dn->GetCurrency(buffer, codeSpan, fallback);
  } else if (StringEqualsLiteral(type, "calendar")) {
    result = dn->GetCalendar(buffer, codeSpan, fallback);
  } else if (StringEqualsLiteral(type, "weekday")) {
    double d = LinearStringToNumber(code);
    if (!IsInteger(d) || d < 1 || d > 7) {
      ReportInvalidOptionError(cx, "weekday", d);
      return false;
    }
    result =
        dn->GetWeekday(buffer, static_cast<mozilla::intl::Weekday>(d),
                       mozilla::MakeStringSpan(calendarChars.get()), fallback);
  } else if (StringEqualsLiteral(type, "month")) {
    double d = LinearStringToNumber(code);
    if (!IsInteger(d) || d < 1 || d > 13) {
      ReportInvalidOptionError(cx, "month", d);
      return false;
    }

    result =
        dn->GetMonth(buffer, static_cast<mozilla::intl::Month>(d),
                     mozilla::MakeStringSpan(calendarChars.get()), fallback);

  } else if (StringEqualsLiteral(type, "quarter")) {
    double d = LinearStringToNumber(code);

    // Inlined implementation of `IsValidQuarterCode ( quarter )`.
    if (!IsInteger(d) || d < 1 || d > 4) {
      ReportInvalidOptionError(cx, "quarter", d);
      return false;
    }

    result =
        dn->GetQuarter(buffer, static_cast<mozilla::intl::Quarter>(d),
                       mozilla::MakeStringSpan(calendarChars.get()), fallback);

  } else if (StringEqualsLiteral(type, "dayPeriod")) {
    mozilla::intl::DayPeriod dayPeriod;
    if (StringEqualsLiteral(code, "am")) {
      dayPeriod = mozilla::intl::DayPeriod::AM;
    } else if (StringEqualsLiteral(code, "pm")) {
      dayPeriod = mozilla::intl::DayPeriod::PM;
    } else {
      ReportInvalidOptionError(cx, "dayPeriod", code);
      return false;
    }
    result = dn->GetDayPeriod(buffer, dayPeriod,
                              mozilla::MakeStringSpan(calendarChars.get()),
                              fallback);

  } else {
    MOZ_ASSERT(StringEqualsLiteral(type, "dateTimeField"));
    mozilla::intl::DateTimeField field;
    if (StringEqualsLiteral(code, "era")) {
      field = mozilla::intl::DateTimeField::Era;
    } else if (StringEqualsLiteral(code, "year")) {
      field = mozilla::intl::DateTimeField::Year;
    } else if (StringEqualsLiteral(code, "quarter")) {
      field = mozilla::intl::DateTimeField::Quarter;
    } else if (StringEqualsLiteral(code, "month")) {
      field = mozilla::intl::DateTimeField::Month;
    } else if (StringEqualsLiteral(code, "weekOfYear")) {
      field = mozilla::intl::DateTimeField::WeekOfYear;
    } else if (StringEqualsLiteral(code, "weekday")) {
      field = mozilla::intl::DateTimeField::Weekday;
    } else if (StringEqualsLiteral(code, "day")) {
      field = mozilla::intl::DateTimeField::Day;
    } else if (StringEqualsLiteral(code, "dayPeriod")) {
      field = mozilla::intl::DateTimeField::DayPeriod;
    } else if (StringEqualsLiteral(code, "hour")) {
      field = mozilla::intl::DateTimeField::Hour;
    } else if (StringEqualsLiteral(code, "minute")) {
      field = mozilla::intl::DateTimeField::Minute;
    } else if (StringEqualsLiteral(code, "second")) {
      field = mozilla::intl::DateTimeField::Second;
    } else if (StringEqualsLiteral(code, "timeZoneName")) {
      field = mozilla::intl::DateTimeField::TimeZoneName;
    } else {
      ReportInvalidOptionError(cx, "dateTimeField", code);
      return false;
    }

    intl::SharedIntlData& sharedIntlData = cx->runtime()->sharedIntlData.ref();
    mozilla::intl::DateTimePatternGenerator* dtpgen =
        sharedIntlData.getDateTimePatternGenerator(cx, locale.get());
    if (!dtpgen) {
      return false;
    }

    result = dn->GetDateTimeField(buffer, field, *dtpgen, fallback);
  }

  if (result.isErr()) {
    switch (result.unwrapErr()) {
      case mozilla::intl::DisplayNamesError::InternalError:
        intl::ReportInternalError(cx);
        break;
      case mozilla::intl::DisplayNamesError::OutOfMemory:
        ReportOutOfMemory(cx);
        break;
      case mozilla::intl::DisplayNamesError::InvalidOption:
        ReportInvalidOptionError(cx, type, code);
        break;
      case mozilla::intl::DisplayNamesError::DuplicateVariantSubtag:
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_DUPLICATE_VARIANT_SUBTAG);
        break;
      case mozilla::intl::DisplayNamesError::InvalidLanguageTag:
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_INVALID_LANGUAGE_TAG);
        break;
    }
    return false;
  }

  JSString* str = buffer.toString(cx);
  if (!str) {
    return false;
  }

  if (str->empty()) {
    args.rval().setUndefined();
  } else {
    args.rval().setString(str);
  }

  return true;
}
