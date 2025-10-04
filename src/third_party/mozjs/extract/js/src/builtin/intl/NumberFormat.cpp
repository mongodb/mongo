/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Intl.NumberFormat implementation. */

#include "builtin/intl/NumberFormat.h"

#include "mozilla/Assertions.h"
#include "mozilla/Casting.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/intl/Locale.h"
#include "mozilla/intl/MeasureUnit.h"
#include "mozilla/intl/MeasureUnitGenerated.h"
#include "mozilla/intl/NumberFormat.h"
#include "mozilla/intl/NumberingSystem.h"
#include "mozilla/intl/NumberRangeFormat.h"
#include "mozilla/Span.h"
#include "mozilla/TextUtils.h"
#include "mozilla/UniquePtr.h"

#include <algorithm>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <string_view>
#include <type_traits>

#include "builtin/Array.h"
#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/FormatBuffer.h"
#include "builtin/intl/LanguageTag.h"
#include "builtin/intl/RelativeTimeFormat.h"
#include "gc/GCContext.h"
#include "js/CharacterEncoding.h"
#include "js/PropertySpec.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "util/Text.h"
#include "vm/BigIntType.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/StringType.h"

#include "vm/GeckoProfiler-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::AssertedCast;

using js::intl::DateTimeFormatOptions;
using js::intl::FieldType;

const JSClassOps NumberFormatObject::classOps_ = {
    nullptr,                       // addProperty
    nullptr,                       // delProperty
    nullptr,                       // enumerate
    nullptr,                       // newEnumerate
    nullptr,                       // resolve
    nullptr,                       // mayResolve
    NumberFormatObject::finalize,  // finalize
    nullptr,                       // call
    nullptr,                       // construct
    nullptr,                       // trace
};

const JSClass NumberFormatObject::class_ = {
    "Intl.NumberFormat",
    JSCLASS_HAS_RESERVED_SLOTS(NumberFormatObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_NumberFormat) |
        JSCLASS_FOREGROUND_FINALIZE,
    &NumberFormatObject::classOps_, &NumberFormatObject::classSpec_};

const JSClass& NumberFormatObject::protoClass_ = PlainObject::class_;

static bool numberFormat_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setString(cx->names().NumberFormat);
  return true;
}

static const JSFunctionSpec numberFormat_static_methods[] = {
    JS_SELF_HOSTED_FN("supportedLocalesOf",
                      "Intl_NumberFormat_supportedLocalesOf", 1, 0),
    JS_FS_END,
};

static const JSFunctionSpec numberFormat_methods[] = {
    JS_SELF_HOSTED_FN("resolvedOptions", "Intl_NumberFormat_resolvedOptions", 0,
                      0),
    JS_SELF_HOSTED_FN("formatToParts", "Intl_NumberFormat_formatToParts", 1, 0),
    JS_SELF_HOSTED_FN("formatRange", "Intl_NumberFormat_formatRange", 2, 0),
    JS_SELF_HOSTED_FN("formatRangeToParts",
                      "Intl_NumberFormat_formatRangeToParts", 2, 0),
    JS_FN("toSource", numberFormat_toSource, 0, 0),
    JS_FS_END,
};

static const JSPropertySpec numberFormat_properties[] = {
    JS_SELF_HOSTED_GET("format", "$Intl_NumberFormat_format_get", 0),
    JS_STRING_SYM_PS(toStringTag, "Intl.NumberFormat", JSPROP_READONLY),
    JS_PS_END,
};

static bool NumberFormat(JSContext* cx, unsigned argc, Value* vp);

const ClassSpec NumberFormatObject::classSpec_ = {
    GenericCreateConstructor<NumberFormat, 0, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<NumberFormatObject>,
    numberFormat_static_methods,
    nullptr,
    numberFormat_methods,
    numberFormat_properties,
    nullptr,
    ClassSpec::DontDefineConstructor};

/**
 * 15.1.1 Intl.NumberFormat ( [ locales [ , options ] ] )
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
static bool NumberFormat(JSContext* cx, const CallArgs& args, bool construct) {
  AutoJSConstructorProfilerEntry pseudoFrame(cx, "Intl.NumberFormat");

  // Step 1 (Handled by OrdinaryCreateFromConstructor fallback code).

  // Step 2 (Inlined 9.1.14, OrdinaryCreateFromConstructor).
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_NumberFormat,
                                          &proto)) {
    return false;
  }

  Rooted<NumberFormatObject*> numberFormat(cx);
  numberFormat = NewObjectWithClassProto<NumberFormatObject>(cx, proto);
  if (!numberFormat) {
    return false;
  }

  RootedValue thisValue(cx,
                        construct ? ObjectValue(*numberFormat) : args.thisv());
  HandleValue locales = args.get(0);
  HandleValue options = args.get(1);

  // Step 3.
  return intl::InitializeNumberFormatObject(cx, numberFormat, thisValue,
                                            locales, options, args.rval());
}

static bool NumberFormat(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return NumberFormat(cx, args, args.isConstructing());
}

bool js::intl_NumberFormat(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  MOZ_ASSERT(!args.isConstructing());
  // intl_NumberFormat is an intrinsic for self-hosted JavaScript, so it
  // cannot be used with "new", but it still has to be treated as a
  // constructor.
  return NumberFormat(cx, args, true);
}

void js::NumberFormatObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  MOZ_ASSERT(gcx->onMainThread());

  auto* numberFormat = &obj->as<NumberFormatObject>();
  mozilla::intl::NumberFormat* nf = numberFormat->getNumberFormatter();
  mozilla::intl::NumberRangeFormat* nrf =
      numberFormat->getNumberRangeFormatter();

  if (nf) {
    intl::RemoveICUCellMemory(gcx, obj, NumberFormatObject::EstimatedMemoryUse);
    // This was allocated using `new` in mozilla::intl::NumberFormat, so we
    // delete here.
    delete nf;
  }

  if (nrf) {
    intl::RemoveICUCellMemory(gcx, obj, EstimatedRangeFormatterMemoryUse);
    // This was allocated using `new` in mozilla::intl::NumberRangeFormat, so we
    // delete here.
    delete nrf;
  }
}

bool js::intl_numberingSystem(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isString());

  UniqueChars locale = intl::EncodeLocale(cx, args[0].toString());
  if (!locale) {
    return false;
  }

  auto numberingSystem =
      mozilla::intl::NumberingSystem::TryCreate(locale.get());
  if (numberingSystem.isErr()) {
    intl::ReportInternalError(cx, numberingSystem.unwrapErr());
    return false;
  }

  auto name = numberingSystem.inspect()->GetName();
  if (name.isErr()) {
    intl::ReportInternalError(cx, name.unwrapErr());
    return false;
  }

  JSString* jsname = NewStringCopy<CanGC>(cx, name.unwrap());
  if (!jsname) {
    return false;
  }

  args.rval().setString(jsname);
  return true;
}

#if DEBUG || MOZ_SYSTEM_ICU
bool js::intl_availableMeasurementUnits(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 0);

  RootedObject measurementUnits(cx, NewPlainObjectWithProto(cx, nullptr));
  if (!measurementUnits) {
    return false;
  }

  auto units = mozilla::intl::MeasureUnit::GetAvailable();
  if (units.isErr()) {
    intl::ReportInternalError(cx, units.unwrapErr());
    return false;
  }

  Rooted<JSAtom*> unitAtom(cx);
  for (auto unit : units.unwrap()) {
    if (unit.isErr()) {
      intl::ReportInternalError(cx);
      return false;
    }
    auto unitIdentifier = unit.unwrap();

    unitAtom = Atomize(cx, unitIdentifier.data(), unitIdentifier.size());
    if (!unitAtom) {
      return false;
    }

    if (!DefineDataProperty(cx, measurementUnits, unitAtom->asPropertyName(),
                            TrueHandleValue)) {
      return false;
    }
  }

  args.rval().setObject(*measurementUnits);
  return true;
}
#endif

static constexpr size_t MaxUnitLength() {
  size_t length = 0;
  for (const auto& unit : mozilla::intl::simpleMeasureUnits) {
    length = std::max(length, std::char_traits<char>::length(unit.name));
  }
  return length * 2 + std::char_traits<char>::length("-per-");
}

static UniqueChars NumberFormatLocale(JSContext* cx, HandleObject internals) {
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

  // |ApplyUnicodeExtensionToTag| applies the new keywords to the front of
  // the Unicode extension subtag. We're then relying on ICU to follow RFC
  // 6067, which states that any trailing keywords using the same key
  // should be ignored.
  if (!intl::ApplyUnicodeExtensionToTag(cx, tag, keywords)) {
    return nullptr;
  }

  intl::FormatBuffer<char> buffer(cx);
  if (auto result = tag.ToString(buffer); result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return nullptr;
  }
  return buffer.extractStringZ();
}

struct NumberFormatOptions : public mozilla::intl::NumberRangeFormatOptions {
  static_assert(std::is_base_of_v<mozilla::intl::NumberFormatOptions,
                                  mozilla::intl::NumberRangeFormatOptions>);

  char currencyChars[3] = {};
  char unitChars[MaxUnitLength()] = {};
};

static bool FillNumberFormatOptions(JSContext* cx, HandleObject internals,
                                    NumberFormatOptions& options) {
  RootedValue value(cx);
  if (!GetProperty(cx, internals, internals, cx->names().style, &value)) {
    return false;
  }

  bool accountingSign = false;
  {
    JSLinearString* style = value.toString()->ensureLinear(cx);
    if (!style) {
      return false;
    }

    if (StringEqualsLiteral(style, "currency")) {
      if (!GetProperty(cx, internals, internals, cx->names().currency,
                       &value)) {
        return false;
      }
      JSLinearString* currency = value.toString()->ensureLinear(cx);
      if (!currency) {
        return false;
      }

      MOZ_RELEASE_ASSERT(
          currency->length() == 3,
          "IsWellFormedCurrencyCode permits only length-3 strings");
      MOZ_ASSERT(StringIsAscii(currency),
                 "IsWellFormedCurrencyCode permits only ASCII strings");
      CopyChars(reinterpret_cast<Latin1Char*>(options.currencyChars),
                *currency);

      if (!GetProperty(cx, internals, internals, cx->names().currencyDisplay,
                       &value)) {
        return false;
      }
      JSLinearString* currencyDisplay = value.toString()->ensureLinear(cx);
      if (!currencyDisplay) {
        return false;
      }

      using CurrencyDisplay =
          mozilla::intl::NumberFormatOptions::CurrencyDisplay;

      CurrencyDisplay display;
      if (StringEqualsLiteral(currencyDisplay, "code")) {
        display = CurrencyDisplay::Code;
      } else if (StringEqualsLiteral(currencyDisplay, "symbol")) {
        display = CurrencyDisplay::Symbol;
      } else if (StringEqualsLiteral(currencyDisplay, "narrowSymbol")) {
        display = CurrencyDisplay::NarrowSymbol;
      } else {
        MOZ_ASSERT(StringEqualsLiteral(currencyDisplay, "name"));
        display = CurrencyDisplay::Name;
      }

      if (!GetProperty(cx, internals, internals, cx->names().currencySign,
                       &value)) {
        return false;
      }
      JSLinearString* currencySign = value.toString()->ensureLinear(cx);
      if (!currencySign) {
        return false;
      }

      if (StringEqualsLiteral(currencySign, "accounting")) {
        accountingSign = true;
      } else {
        MOZ_ASSERT(StringEqualsLiteral(currencySign, "standard"));
      }

      options.mCurrency = mozilla::Some(
          std::make_pair(std::string_view(options.currencyChars, 3), display));
    } else if (StringEqualsLiteral(style, "percent")) {
      options.mPercent = true;
    } else if (StringEqualsLiteral(style, "unit")) {
      if (!GetProperty(cx, internals, internals, cx->names().unit, &value)) {
        return false;
      }
      JSLinearString* unit = value.toString()->ensureLinear(cx);
      if (!unit) {
        return false;
      }

      size_t unit_str_length = unit->length();

      MOZ_ASSERT(StringIsAscii(unit));
      MOZ_RELEASE_ASSERT(unit_str_length <= MaxUnitLength());
      CopyChars(reinterpret_cast<Latin1Char*>(options.unitChars), *unit);

      if (!GetProperty(cx, internals, internals, cx->names().unitDisplay,
                       &value)) {
        return false;
      }
      JSLinearString* unitDisplay = value.toString()->ensureLinear(cx);
      if (!unitDisplay) {
        return false;
      }

      using UnitDisplay = mozilla::intl::NumberFormatOptions::UnitDisplay;

      UnitDisplay display;
      if (StringEqualsLiteral(unitDisplay, "short")) {
        display = UnitDisplay::Short;
      } else if (StringEqualsLiteral(unitDisplay, "narrow")) {
        display = UnitDisplay::Narrow;
      } else {
        MOZ_ASSERT(StringEqualsLiteral(unitDisplay, "long"));
        display = UnitDisplay::Long;
      }

      options.mUnit = mozilla::Some(std::make_pair(
          std::string_view(options.unitChars, unit_str_length), display));
    } else {
      MOZ_ASSERT(StringEqualsLiteral(style, "decimal"));
    }
  }

  bool hasMinimumSignificantDigits;
  if (!HasProperty(cx, internals, cx->names().minimumSignificantDigits,
                   &hasMinimumSignificantDigits)) {
    return false;
  }

  if (hasMinimumSignificantDigits) {
    if (!GetProperty(cx, internals, internals,
                     cx->names().minimumSignificantDigits, &value)) {
      return false;
    }
    uint32_t minimumSignificantDigits = AssertedCast<uint32_t>(value.toInt32());

    if (!GetProperty(cx, internals, internals,
                     cx->names().maximumSignificantDigits, &value)) {
      return false;
    }
    uint32_t maximumSignificantDigits = AssertedCast<uint32_t>(value.toInt32());

    options.mSignificantDigits = mozilla::Some(
        std::make_pair(minimumSignificantDigits, maximumSignificantDigits));
  }

  bool hasMinimumFractionDigits;
  if (!HasProperty(cx, internals, cx->names().minimumFractionDigits,
                   &hasMinimumFractionDigits)) {
    return false;
  }

  if (hasMinimumFractionDigits) {
    if (!GetProperty(cx, internals, internals,
                     cx->names().minimumFractionDigits, &value)) {
      return false;
    }
    uint32_t minimumFractionDigits = AssertedCast<uint32_t>(value.toInt32());

    if (!GetProperty(cx, internals, internals,
                     cx->names().maximumFractionDigits, &value)) {
      return false;
    }
    uint32_t maximumFractionDigits = AssertedCast<uint32_t>(value.toInt32());

    options.mFractionDigits = mozilla::Some(
        std::make_pair(minimumFractionDigits, maximumFractionDigits));
  }

  if (!GetProperty(cx, internals, internals, cx->names().roundingPriority,
                   &value)) {
    return false;
  }

  {
    JSLinearString* roundingPriority = value.toString()->ensureLinear(cx);
    if (!roundingPriority) {
      return false;
    }

    using RoundingPriority =
        mozilla::intl::NumberFormatOptions::RoundingPriority;

    RoundingPriority priority;
    if (StringEqualsLiteral(roundingPriority, "auto")) {
      priority = RoundingPriority::Auto;
    } else if (StringEqualsLiteral(roundingPriority, "morePrecision")) {
      priority = RoundingPriority::MorePrecision;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(roundingPriority, "lessPrecision"));
      priority = RoundingPriority::LessPrecision;
    }

    options.mRoundingPriority = priority;
  }

  if (!GetProperty(cx, internals, internals, cx->names().minimumIntegerDigits,
                   &value)) {
    return false;
  }
  options.mMinIntegerDigits =
      mozilla::Some(AssertedCast<uint32_t>(value.toInt32()));

  if (!GetProperty(cx, internals, internals, cx->names().useGrouping, &value)) {
    return false;
  }

  if (value.isString()) {
    JSLinearString* useGrouping = value.toString()->ensureLinear(cx);
    if (!useGrouping) {
      return false;
    }

    using Grouping = mozilla::intl::NumberFormatOptions::Grouping;

    Grouping grouping;
    if (StringEqualsLiteral(useGrouping, "auto")) {
      grouping = Grouping::Auto;
    } else if (StringEqualsLiteral(useGrouping, "always")) {
      grouping = Grouping::Always;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(useGrouping, "min2"));
      grouping = Grouping::Min2;
    }

    options.mGrouping = grouping;
  } else {
    MOZ_ASSERT(value.isBoolean());
    MOZ_ASSERT(value.toBoolean() == false);

    using Grouping = mozilla::intl::NumberFormatOptions::Grouping;

    options.mGrouping = Grouping::Never;
  }

  if (!GetProperty(cx, internals, internals, cx->names().notation, &value)) {
    return false;
  }

  {
    JSLinearString* notation = value.toString()->ensureLinear(cx);
    if (!notation) {
      return false;
    }

    using Notation = mozilla::intl::NumberFormatOptions::Notation;

    Notation style;
    if (StringEqualsLiteral(notation, "standard")) {
      style = Notation::Standard;
    } else if (StringEqualsLiteral(notation, "scientific")) {
      style = Notation::Scientific;
    } else if (StringEqualsLiteral(notation, "engineering")) {
      style = Notation::Engineering;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(notation, "compact"));

      if (!GetProperty(cx, internals, internals, cx->names().compactDisplay,
                       &value)) {
        return false;
      }

      JSLinearString* compactDisplay = value.toString()->ensureLinear(cx);
      if (!compactDisplay) {
        return false;
      }

      if (StringEqualsLiteral(compactDisplay, "short")) {
        style = Notation::CompactShort;
      } else {
        MOZ_ASSERT(StringEqualsLiteral(compactDisplay, "long"));
        style = Notation::CompactLong;
      }
    }

    options.mNotation = style;
  }

  if (!GetProperty(cx, internals, internals, cx->names().signDisplay, &value)) {
    return false;
  }

  {
    JSLinearString* signDisplay = value.toString()->ensureLinear(cx);
    if (!signDisplay) {
      return false;
    }

    using SignDisplay = mozilla::intl::NumberFormatOptions::SignDisplay;

    SignDisplay display;
    if (StringEqualsLiteral(signDisplay, "auto")) {
      if (accountingSign) {
        display = SignDisplay::Accounting;
      } else {
        display = SignDisplay::Auto;
      }
    } else if (StringEqualsLiteral(signDisplay, "never")) {
      display = SignDisplay::Never;
    } else if (StringEqualsLiteral(signDisplay, "always")) {
      if (accountingSign) {
        display = SignDisplay::AccountingAlways;
      } else {
        display = SignDisplay::Always;
      }
    } else if (StringEqualsLiteral(signDisplay, "exceptZero")) {
      if (accountingSign) {
        display = SignDisplay::AccountingExceptZero;
      } else {
        display = SignDisplay::ExceptZero;
      }
    } else {
      MOZ_ASSERT(StringEqualsLiteral(signDisplay, "negative"));
      if (accountingSign) {
        display = SignDisplay::AccountingNegative;
      } else {
        display = SignDisplay::Negative;
      }
    }

    options.mSignDisplay = display;
  }

  if (!GetProperty(cx, internals, internals, cx->names().roundingIncrement,
                   &value)) {
    return false;
  }
  options.mRoundingIncrement = AssertedCast<uint32_t>(value.toInt32());

  if (!GetProperty(cx, internals, internals, cx->names().roundingMode,
                   &value)) {
    return false;
  }

  {
    JSLinearString* roundingMode = value.toString()->ensureLinear(cx);
    if (!roundingMode) {
      return false;
    }

    using RoundingMode = mozilla::intl::NumberFormatOptions::RoundingMode;

    RoundingMode rounding;
    if (StringEqualsLiteral(roundingMode, "halfExpand")) {
      // "halfExpand" is the default mode, so we handle it first.
      rounding = RoundingMode::HalfExpand;
    } else if (StringEqualsLiteral(roundingMode, "ceil")) {
      rounding = RoundingMode::Ceil;
    } else if (StringEqualsLiteral(roundingMode, "floor")) {
      rounding = RoundingMode::Floor;
    } else if (StringEqualsLiteral(roundingMode, "expand")) {
      rounding = RoundingMode::Expand;
    } else if (StringEqualsLiteral(roundingMode, "trunc")) {
      rounding = RoundingMode::Trunc;
    } else if (StringEqualsLiteral(roundingMode, "halfCeil")) {
      rounding = RoundingMode::HalfCeil;
    } else if (StringEqualsLiteral(roundingMode, "halfFloor")) {
      rounding = RoundingMode::HalfFloor;
    } else if (StringEqualsLiteral(roundingMode, "halfTrunc")) {
      rounding = RoundingMode::HalfTrunc;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(roundingMode, "halfEven"));
      rounding = RoundingMode::HalfEven;
    }

    options.mRoundingMode = rounding;
  }

  if (!GetProperty(cx, internals, internals, cx->names().trailingZeroDisplay,
                   &value)) {
    return false;
  }

  {
    JSLinearString* trailingZeroDisplay = value.toString()->ensureLinear(cx);
    if (!trailingZeroDisplay) {
      return false;
    }

    if (StringEqualsLiteral(trailingZeroDisplay, "auto")) {
      options.mStripTrailingZero = false;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(trailingZeroDisplay, "stripIfInteger"));
      options.mStripTrailingZero = true;
    }
  }

  return true;
}

/**
 * Returns a new mozilla::intl::Number[Range]Format with the locale and number
 * formatting options of the given NumberFormat, or a nullptr if
 * initialization failed.
 */
template <class Formatter>
static Formatter* NewNumberFormat(JSContext* cx,
                                  Handle<NumberFormatObject*> numberFormat) {
  RootedObject internals(cx, intl::GetInternalsObject(cx, numberFormat));
  if (!internals) {
    return nullptr;
  }

  UniqueChars locale = NumberFormatLocale(cx, internals);
  if (!locale) {
    return nullptr;
  }

  NumberFormatOptions options;
  if (!FillNumberFormatOptions(cx, internals, options)) {
    return nullptr;
  }

  options.mRangeCollapse = NumberFormatOptions::RangeCollapse::Auto;
  options.mRangeIdentityFallback =
      NumberFormatOptions::RangeIdentityFallback::Approximately;

  mozilla::Result<mozilla::UniquePtr<Formatter>, mozilla::intl::ICUError>
      result = Formatter::TryCreate(locale.get(), options);

  if (result.isOk()) {
    return result.unwrap().release();
  }

  intl::ReportInternalError(cx, result.unwrapErr());
  return nullptr;
}

static mozilla::intl::NumberFormat* GetOrCreateNumberFormat(
    JSContext* cx, Handle<NumberFormatObject*> numberFormat) {
  // Obtain a cached mozilla::intl::NumberFormat object.
  mozilla::intl::NumberFormat* nf = numberFormat->getNumberFormatter();
  if (nf) {
    return nf;
  }

  nf = NewNumberFormat<mozilla::intl::NumberFormat>(cx, numberFormat);
  if (!nf) {
    return nullptr;
  }
  numberFormat->setNumberFormatter(nf);

  intl::AddICUCellMemory(numberFormat, NumberFormatObject::EstimatedMemoryUse);
  return nf;
}

static mozilla::intl::NumberRangeFormat* GetOrCreateNumberRangeFormat(
    JSContext* cx, Handle<NumberFormatObject*> numberFormat) {
  // Obtain a cached mozilla::intl::NumberRangeFormat object.
  mozilla::intl::NumberRangeFormat* nrf =
      numberFormat->getNumberRangeFormatter();
  if (nrf) {
    return nrf;
  }

  nrf = NewNumberFormat<mozilla::intl::NumberRangeFormat>(cx, numberFormat);
  if (!nrf) {
    return nullptr;
  }
  numberFormat->setNumberRangeFormatter(nrf);

  intl::AddICUCellMemory(numberFormat,
                         NumberFormatObject::EstimatedRangeFormatterMemoryUse);
  return nrf;
}

static FieldType GetFieldTypeForNumberPartType(
    mozilla::intl::NumberPartType type) {
  switch (type) {
    case mozilla::intl::NumberPartType::ApproximatelySign:
      return &JSAtomState::approximatelySign;
    case mozilla::intl::NumberPartType::Compact:
      return &JSAtomState::compact;
    case mozilla::intl::NumberPartType::Currency:
      return &JSAtomState::currency;
    case mozilla::intl::NumberPartType::Decimal:
      return &JSAtomState::decimal;
    case mozilla::intl::NumberPartType::ExponentInteger:
      return &JSAtomState::exponentInteger;
    case mozilla::intl::NumberPartType::ExponentMinusSign:
      return &JSAtomState::exponentMinusSign;
    case mozilla::intl::NumberPartType::ExponentSeparator:
      return &JSAtomState::exponentSeparator;
    case mozilla::intl::NumberPartType::Fraction:
      return &JSAtomState::fraction;
    case mozilla::intl::NumberPartType::Group:
      return &JSAtomState::group;
    case mozilla::intl::NumberPartType::Infinity:
      return &JSAtomState::infinity;
    case mozilla::intl::NumberPartType::Integer:
      return &JSAtomState::integer;
    case mozilla::intl::NumberPartType::Literal:
      return &JSAtomState::literal;
    case mozilla::intl::NumberPartType::MinusSign:
      return &JSAtomState::minusSign;
    case mozilla::intl::NumberPartType::Nan:
      return &JSAtomState::nan;
    case mozilla::intl::NumberPartType::Percent:
      return &JSAtomState::percentSign;
    case mozilla::intl::NumberPartType::PlusSign:
      return &JSAtomState::plusSign;
    case mozilla::intl::NumberPartType::Unit:
      return &JSAtomState::unit;
  }

  MOZ_ASSERT_UNREACHABLE(
      "unenumerated, undocumented format field returned by iterator");
  return nullptr;
}

static FieldType GetFieldTypeForNumberPartSource(
    mozilla::intl::NumberPartSource source) {
  switch (source) {
    case mozilla::intl::NumberPartSource::Shared:
      return &JSAtomState::shared;
    case mozilla::intl::NumberPartSource::Start:
      return &JSAtomState::startRange;
    case mozilla::intl::NumberPartSource::End:
      return &JSAtomState::endRange;
  }

  MOZ_CRASH("unexpected number part source");
}

enum class DisplayNumberPartSource : bool { No, Yes };

static bool FormattedNumberToParts(JSContext* cx, HandleString str,
                                   const mozilla::intl::NumberPartVector& parts,
                                   DisplayNumberPartSource displaySource,
                                   FieldType unitType,
                                   MutableHandleValue result) {
  size_t lastEndIndex = 0;

  RootedObject singlePart(cx);
  RootedValue propVal(cx);

  Rooted<ArrayObject*> partsArray(
      cx, NewDenseFullyAllocatedArray(cx, parts.length()));
  if (!partsArray) {
    return false;
  }
  partsArray->ensureDenseInitializedLength(0, parts.length());

  size_t index = 0;
  for (const auto& part : parts) {
    FieldType type = GetFieldTypeForNumberPartType(part.type);
    size_t endIndex = part.endIndex;

    MOZ_ASSERT(lastEndIndex < endIndex);

    singlePart = NewPlainObject(cx);
    if (!singlePart) {
      return false;
    }

    propVal.setString(cx->names().*type);
    if (!DefineDataProperty(cx, singlePart, cx->names().type, propVal)) {
      return false;
    }

    JSLinearString* partSubstr =
        NewDependentString(cx, str, lastEndIndex, endIndex - lastEndIndex);
    if (!partSubstr) {
      return false;
    }

    propVal.setString(partSubstr);
    if (!DefineDataProperty(cx, singlePart, cx->names().value, propVal)) {
      return false;
    }

    if (displaySource == DisplayNumberPartSource::Yes) {
      FieldType source = GetFieldTypeForNumberPartSource(part.source);

      propVal.setString(cx->names().*source);
      if (!DefineDataProperty(cx, singlePart, cx->names().source, propVal)) {
        return false;
      }
    }

    if (unitType != nullptr && type != &JSAtomState::literal) {
      propVal.setString(cx->names().*unitType);
      if (!DefineDataProperty(cx, singlePart, cx->names().unit, propVal)) {
        return false;
      }
    }

    partsArray->initDenseElement(index++, ObjectValue(*singlePart));

    lastEndIndex = endIndex;
  }

  MOZ_ASSERT(index == parts.length());
  MOZ_ASSERT(lastEndIndex == str->length(),
             "result array must partition the entire string");

  result.setObject(*partsArray);
  return true;
}

bool js::intl::FormattedRelativeTimeToParts(
    JSContext* cx, HandleString str,
    const mozilla::intl::NumberPartVector& parts, FieldType relativeTimeUnit,
    MutableHandleValue result) {
  return FormattedNumberToParts(cx, str, parts, DisplayNumberPartSource::No,
                                relativeTimeUnit, result);
}

// Return true if the string starts with "0[bBoOxX]", possibly skipping over
// leading whitespace.
template <typename CharT>
static bool IsNonDecimalNumber(mozilla::Range<const CharT> chars) {
  const CharT* end = chars.begin().get() + chars.length();
  const CharT* start = SkipSpace(chars.begin().get(), end);

  if (end - start >= 2 && start[0] == '0') {
    CharT ch = start[1];
    return ch == 'b' || ch == 'B' || ch == 'o' || ch == 'O' || ch == 'x' ||
           ch == 'X';
  }
  return false;
}

static bool IsNonDecimalNumber(JSLinearString* str) {
  JS::AutoCheckCannotGC nogc;
  return str->hasLatin1Chars() ? IsNonDecimalNumber(str->latin1Range(nogc))
                               : IsNonDecimalNumber(str->twoByteRange(nogc));
}

/**
 * 15.5.16 ToIntlMathematicalValue ( value )
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
static bool ToIntlMathematicalValue(JSContext* cx, MutableHandleValue value) {
  // Step 1.
  if (!ToPrimitive(cx, JSTYPE_NUMBER, value)) {
    return false;
  }

  // Step 2.
  if (value.isBigInt()) {
    return true;
  }

  // Step 4.
  if (!value.isString()) {
    // Step 4.a. (Steps 4.b-10 not applicable in our implementation.)
    return ToNumber(cx, value);
  }

  // Step 3.
  JSLinearString* str = value.toString()->ensureLinear(cx);
  if (!str) {
    return false;
  }

  // Steps 5-6, 8, and 9.a.
  double number = LinearStringToNumber(str);

  // Step 7.
  if (std::isnan(number)) {
    // Set to NaN if the input can't be parsed as a number.
    value.setNaN();
    return true;
  }

  // Step 9.
  if (number == 0.0 || std::isinf(number)) {
    // Step 9.a. (Reordered)

    // Steps 9.b-e.
    value.setDouble(number);
    return true;
  }

  // Step 10.
  if (IsNonDecimalNumber(str)) {
    // ICU doesn't accept non-decimal numbers, so we have to convert the input
    // into a base-10 string.

    MOZ_ASSERT(!mozilla::IsNegative(number),
               "non-decimal numbers can't be negative");

    if (number < DOUBLE_INTEGRAL_PRECISION_LIMIT) {
      // Fast-path if we can guarantee there was no loss of precision.
      value.setDouble(number);
    } else {
      // For the slow-path convert the string into a BigInt.

      // StringToBigInt can't fail (other than OOM) when StringToNumber already
      // succeeded.
      RootedString rooted(cx, str);
      BigInt* bi;
      JS_TRY_VAR_OR_RETURN_FALSE(cx, bi, StringToBigInt(cx, rooted));
      MOZ_ASSERT(bi);

      value.setBigInt(bi);
    }
  }
  return true;
}

// Return the number part of the input by removing leading and trailing
// whitespace.
template <typename CharT>
static mozilla::Span<const CharT> NumberPart(const CharT* chars,
                                             size_t length) {
  const CharT* start = chars;
  const CharT* end = chars + length;

  start = SkipSpace(start, end);

  // |SkipSpace| only supports forward iteration, so inline the backwards
  // iteration here.
  MOZ_ASSERT(start <= end);
  while (end > start && unicode::IsSpace(end[-1])) {
    end--;
  }

  // The number part is a non-empty, ASCII-only substring.
  MOZ_ASSERT(start < end);
  MOZ_ASSERT(mozilla::IsAscii(mozilla::Span(start, end)));

  return {start, end};
}

static bool NumberPart(JSContext* cx, JSLinearString* str,
                       const JS::AutoCheckCannotGC& nogc,
                       JS::UniqueChars& latin1, std::string_view& result) {
  if (str->hasLatin1Chars()) {
    auto span = NumberPart(
        reinterpret_cast<const char*>(str->latin1Chars(nogc)), str->length());

    result = {span.data(), span.size()};
    return true;
  }

  auto span = NumberPart(str->twoByteChars(nogc), str->length());

  latin1.reset(JS::LossyTwoByteCharsToNewLatin1CharsZ(cx, span).c_str());
  if (!latin1) {
    return false;
  }

  result = {latin1.get(), span.size()};
  return true;
}

bool js::intl_FormatNumber(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);
  MOZ_ASSERT(args[0].isObject());
  MOZ_ASSERT(args[2].isBoolean());

  Rooted<NumberFormatObject*> numberFormat(
      cx, &args[0].toObject().as<NumberFormatObject>());

  RootedValue value(cx, args[1]);
  if (!ToIntlMathematicalValue(cx, &value)) {
    return false;
  }

  mozilla::intl::NumberFormat* nf = GetOrCreateNumberFormat(cx, numberFormat);
  if (!nf) {
    return false;
  }

  // Actually format the number
  using ICUError = mozilla::intl::ICUError;

  bool formatToParts = args[2].toBoolean();
  mozilla::Result<std::u16string_view, ICUError> result =
      mozilla::Err(ICUError::InternalError);
  mozilla::intl::NumberPartVector parts;
  if (value.isNumber()) {
    double num = value.toNumber();
    if (formatToParts) {
      result = nf->formatToParts(num, parts);
    } else {
      result = nf->format(num);
    }
  } else if (value.isBigInt()) {
    RootedBigInt bi(cx, value.toBigInt());

    int64_t num;
    if (BigInt::isInt64(bi, &num)) {
      if (formatToParts) {
        result = nf->formatToParts(num, parts);
      } else {
        result = nf->format(num);
      }
    } else {
      JSLinearString* str = BigInt::toString<CanGC>(cx, bi, 10);
      if (!str) {
        return false;
      }
      MOZ_RELEASE_ASSERT(str->hasLatin1Chars());

      JS::AutoCheckCannotGC nogc;

      const char* chars = reinterpret_cast<const char*>(str->latin1Chars(nogc));
      if (formatToParts) {
        result =
            nf->formatToParts(std::string_view(chars, str->length()), parts);
      } else {
        result = nf->format(std::string_view(chars, str->length()));
      }
    }
  } else {
    JSLinearString* str = value.toString()->ensureLinear(cx);
    if (!str) {
      return false;
    }

    JS::AutoCheckCannotGC nogc;

    // Two-byte strings have to be copied into a separate |char| buffer.
    JS::UniqueChars latin1;

    std::string_view sv;
    if (!NumberPart(cx, str, nogc, latin1, sv)) {
      return false;
    }

    if (formatToParts) {
      result = nf->formatToParts(sv, parts);
    } else {
      result = nf->format(sv);
    }
  }

  if (result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return false;
  }

  RootedString str(cx, NewStringCopy<CanGC>(cx, result.unwrap()));
  if (!str) {
    return false;
  }

  if (formatToParts) {
    return FormattedNumberToParts(cx, str, parts, DisplayNumberPartSource::No,
                                  nullptr, args.rval());
  }

  args.rval().setString(str);
  return true;
}

static JSLinearString* ToLinearString(JSContext* cx, HandleValue val) {
  // Special case to preserve negative zero.
  if (val.isDouble() && mozilla::IsNegativeZero(val.toDouble())) {
    constexpr std::string_view negativeZero = "-0";
    return NewStringCopy<CanGC>(cx, negativeZero);
  }

  JSString* str = ToString(cx, val);
  return str ? str->ensureLinear(cx) : nullptr;
};

bool js::intl_FormatNumberRange(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 4);
  MOZ_ASSERT(args[0].isObject());
  MOZ_ASSERT(!args[1].isUndefined());
  MOZ_ASSERT(!args[2].isUndefined());
  MOZ_ASSERT(args[3].isBoolean());

  Rooted<NumberFormatObject*> numberFormat(
      cx, &args[0].toObject().as<NumberFormatObject>());
  bool formatToParts = args[3].toBoolean();

  RootedValue start(cx, args[1]);
  if (!ToIntlMathematicalValue(cx, &start)) {
    return false;
  }

  RootedValue end(cx, args[2]);
  if (!ToIntlMathematicalValue(cx, &end)) {
    return false;
  }

  // PartitionNumberRangePattern, step 1.
  if (start.isDouble() && std::isnan(start.toDouble())) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr, JSMSG_NAN_NUMBER_RANGE, "start",
        "NumberFormat", formatToParts ? "formatRangeToParts" : "formatRange");
    return false;
  }
  if (end.isDouble() && std::isnan(end.toDouble())) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr, JSMSG_NAN_NUMBER_RANGE, "end",
        "NumberFormat", formatToParts ? "formatRangeToParts" : "formatRange");
    return false;
  }

  using NumberRangeFormat = mozilla::intl::NumberRangeFormat;
  NumberRangeFormat* nf = GetOrCreateNumberRangeFormat(cx, numberFormat);
  if (!nf) {
    return false;
  }

  auto valueRepresentableAsDouble = [](const Value& val, double* num) {
    if (val.isNumber()) {
      *num = val.toNumber();
      return true;
    }
    if (val.isBigInt()) {
      int64_t i64;
      if (BigInt::isInt64(val.toBigInt(), &i64) &&
          i64 < int64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT) &&
          i64 > -int64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT)) {
        *num = double(i64);
        return true;
      }
    }
    return false;
  };

  // Actually format the number range.
  using ICUError = mozilla::intl::ICUError;

  mozilla::Result<std::u16string_view, ICUError> result =
      mozilla::Err(ICUError::InternalError);
  mozilla::intl::NumberPartVector parts;

  double numStart, numEnd;
  if (valueRepresentableAsDouble(start, &numStart) &&
      valueRepresentableAsDouble(end, &numEnd)) {
    if (formatToParts) {
      result = nf->formatToParts(numStart, numEnd, parts);
    } else {
      result = nf->format(numStart, numEnd);
    }
  } else {
    Rooted<JSLinearString*> strStart(cx, ToLinearString(cx, start));
    if (!strStart) {
      return false;
    }

    Rooted<JSLinearString*> strEnd(cx, ToLinearString(cx, end));
    if (!strEnd) {
      return false;
    }

    JS::AutoCheckCannotGC nogc;

    // Two-byte strings have to be copied into a separate |char| buffer.
    JS::UniqueChars latin1Start;
    JS::UniqueChars latin1End;

    std::string_view svStart;
    if (!NumberPart(cx, strStart, nogc, latin1Start, svStart)) {
      return false;
    }

    std::string_view svEnd;
    if (!NumberPart(cx, strEnd, nogc, latin1End, svEnd)) {
      return false;
    }

    if (formatToParts) {
      result = nf->formatToParts(svStart, svEnd, parts);
    } else {
      result = nf->format(svStart, svEnd);
    }
  }

  if (result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return false;
  }

  RootedString str(cx, NewStringCopy<CanGC>(cx, result.unwrap()));
  if (!str) {
    return false;
  }

  if (formatToParts) {
    return FormattedNumberToParts(cx, str, parts, DisplayNumberPartSource::Yes,
                                  nullptr, args.rval());
  }

  args.rval().setString(str);
  return true;
}
