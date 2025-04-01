/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Implementation of the Intl.PluralRules proposal. */

#include "builtin/intl/PluralRules.h"

#include "mozilla/Assertions.h"
#include "mozilla/Casting.h"
#include "mozilla/intl/PluralRules.h"

#include "builtin/Array.h"
#include "builtin/intl/CommonFunctions.h"
#include "gc/GCContext.h"
#include "js/PropertySpec.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/StringType.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::AssertedCast;

const JSClassOps PluralRulesObject::classOps_ = {
    nullptr,                      // addProperty
    nullptr,                      // delProperty
    nullptr,                      // enumerate
    nullptr,                      // newEnumerate
    nullptr,                      // resolve
    nullptr,                      // mayResolve
    PluralRulesObject::finalize,  // finalize
    nullptr,                      // call
    nullptr,                      // construct
    nullptr,                      // trace
};

const JSClass PluralRulesObject::class_ = {
    "Intl.PluralRules",
    JSCLASS_HAS_RESERVED_SLOTS(PluralRulesObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_PluralRules) |
        JSCLASS_FOREGROUND_FINALIZE,
    &PluralRulesObject::classOps_, &PluralRulesObject::classSpec_};

const JSClass& PluralRulesObject::protoClass_ = PlainObject::class_;

static bool pluralRules_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setString(cx->names().PluralRules);
  return true;
}

static const JSFunctionSpec pluralRules_static_methods[] = {
    JS_SELF_HOSTED_FN("supportedLocalesOf",
                      "Intl_PluralRules_supportedLocalesOf", 1, 0),
    JS_FS_END};

static const JSFunctionSpec pluralRules_methods[] = {
    JS_SELF_HOSTED_FN("resolvedOptions", "Intl_PluralRules_resolvedOptions", 0,
                      0),
    JS_SELF_HOSTED_FN("select", "Intl_PluralRules_select", 1, 0),
    JS_SELF_HOSTED_FN("selectRange", "Intl_PluralRules_selectRange", 2, 0),
    JS_FN("toSource", pluralRules_toSource, 0, 0), JS_FS_END};

static const JSPropertySpec pluralRules_properties[] = {
    JS_STRING_SYM_PS(toStringTag, "Intl.PluralRules", JSPROP_READONLY),
    JS_PS_END};

static bool PluralRules(JSContext* cx, unsigned argc, Value* vp);

const ClassSpec PluralRulesObject::classSpec_ = {
    GenericCreateConstructor<PluralRules, 0, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<PluralRulesObject>,
    pluralRules_static_methods,
    nullptr,
    pluralRules_methods,
    pluralRules_properties,
    nullptr,
    ClassSpec::DontDefineConstructor};

/**
 * 16.1.1 Intl.PluralRules ( [ locales [ , options ] ] )
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
static bool PluralRules(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Intl.PluralRules")) {
    return false;
  }

  // Step 2 (Inlined 9.1.14, OrdinaryCreateFromConstructor).
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_PluralRules,
                                          &proto)) {
    return false;
  }

  Rooted<PluralRulesObject*> pluralRules(cx);
  pluralRules = NewObjectWithClassProto<PluralRulesObject>(cx, proto);
  if (!pluralRules) {
    return false;
  }

  HandleValue locales = args.get(0);
  HandleValue options = args.get(1);

  // Step 3.
  if (!intl::InitializeObject(cx, pluralRules,
                              cx->names().InitializePluralRules, locales,
                              options)) {
    return false;
  }

  args.rval().setObject(*pluralRules);
  return true;
}

void js::PluralRulesObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  MOZ_ASSERT(gcx->onMainThread());

  auto* pluralRules = &obj->as<PluralRulesObject>();
  if (mozilla::intl::PluralRules* pr = pluralRules->getPluralRules()) {
    intl::RemoveICUCellMemory(
        gcx, obj, PluralRulesObject::UPluralRulesEstimatedMemoryUse);
    delete pr;
  }
}

static JSString* KeywordToString(mozilla::intl::PluralRules::Keyword keyword,
                                 JSContext* cx) {
  using Keyword = mozilla::intl::PluralRules::Keyword;
  switch (keyword) {
    case Keyword::Zero: {
      return cx->names().zero;
    }
    case Keyword::One: {
      return cx->names().one;
    }
    case Keyword::Two: {
      return cx->names().two;
    }
    case Keyword::Few: {
      return cx->names().few;
    }
    case Keyword::Many: {
      return cx->names().many;
    }
    case Keyword::Other: {
      return cx->names().other;
    }
  }
  MOZ_CRASH("Unexpected PluralRules keyword");
}

/**
 * Returns a new intl::PluralRules with the locale and type options of the given
 * PluralRules.
 */
static mozilla::intl::PluralRules* NewPluralRules(
    JSContext* cx, Handle<PluralRulesObject*> pluralRules) {
  RootedObject internals(cx, intl::GetInternalsObject(cx, pluralRules));
  if (!internals) {
    return nullptr;
  }

  RootedValue value(cx);

  if (!GetProperty(cx, internals, internals, cx->names().locale, &value)) {
    return nullptr;
  }
  UniqueChars locale = intl::EncodeLocale(cx, value.toString());
  if (!locale) {
    return nullptr;
  }

  using PluralRules = mozilla::intl::PluralRules;
  mozilla::intl::PluralRulesOptions options;

  if (!GetProperty(cx, internals, internals, cx->names().type, &value)) {
    return nullptr;
  }

  {
    JSLinearString* type = value.toString()->ensureLinear(cx);
    if (!type) {
      return nullptr;
    }

    if (StringEqualsLiteral(type, "ordinal")) {
      options.mPluralType = PluralRules::Type::Ordinal;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(type, "cardinal"));
      options.mPluralType = PluralRules::Type::Cardinal;
    }
  }

  bool hasMinimumSignificantDigits;
  if (!HasProperty(cx, internals, cx->names().minimumSignificantDigits,
                   &hasMinimumSignificantDigits)) {
    return nullptr;
  }

  if (hasMinimumSignificantDigits) {
    if (!GetProperty(cx, internals, internals,
                     cx->names().minimumSignificantDigits, &value)) {
      return nullptr;
    }
    uint32_t minimumSignificantDigits = AssertedCast<uint32_t>(value.toInt32());

    if (!GetProperty(cx, internals, internals,
                     cx->names().maximumSignificantDigits, &value)) {
      return nullptr;
    }
    uint32_t maximumSignificantDigits = AssertedCast<uint32_t>(value.toInt32());

    options.mSignificantDigits = mozilla::Some(
        std::make_pair(minimumSignificantDigits, maximumSignificantDigits));
  }

  bool hasMinimumFractionDigits;
  if (!HasProperty(cx, internals, cx->names().minimumFractionDigits,
                   &hasMinimumFractionDigits)) {
    return nullptr;
  }

  if (hasMinimumFractionDigits) {
    if (!GetProperty(cx, internals, internals,
                     cx->names().minimumFractionDigits, &value)) {
      return nullptr;
    }
    uint32_t minimumFractionDigits = AssertedCast<uint32_t>(value.toInt32());

    if (!GetProperty(cx, internals, internals,
                     cx->names().maximumFractionDigits, &value)) {
      return nullptr;
    }
    uint32_t maximumFractionDigits = AssertedCast<uint32_t>(value.toInt32());

    options.mFractionDigits = mozilla::Some(
        std::make_pair(minimumFractionDigits, maximumFractionDigits));
  }

  if (!GetProperty(cx, internals, internals, cx->names().roundingPriority,
                   &value)) {
    return nullptr;
  }

  {
    JSLinearString* roundingPriority = value.toString()->ensureLinear(cx);
    if (!roundingPriority) {
      return nullptr;
    }

    using RoundingPriority =
        mozilla::intl::PluralRulesOptions::RoundingPriority;

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
    return nullptr;
  }
  options.mMinIntegerDigits =
      mozilla::Some(AssertedCast<uint32_t>(value.toInt32()));

  if (!GetProperty(cx, internals, internals, cx->names().roundingIncrement,
                   &value)) {
    return nullptr;
  }
  options.mRoundingIncrement = AssertedCast<uint32_t>(value.toInt32());

  if (!GetProperty(cx, internals, internals, cx->names().roundingMode,
                   &value)) {
    return nullptr;
  }

  {
    JSLinearString* roundingMode = value.toString()->ensureLinear(cx);
    if (!roundingMode) {
      return nullptr;
    }

    using RoundingMode = mozilla::intl::PluralRulesOptions::RoundingMode;

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
    return nullptr;
  }

  {
    JSLinearString* trailingZeroDisplay = value.toString()->ensureLinear(cx);
    if (!trailingZeroDisplay) {
      return nullptr;
    }

    if (StringEqualsLiteral(trailingZeroDisplay, "auto")) {
      options.mStripTrailingZero = false;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(trailingZeroDisplay, "stripIfInteger"));
      options.mStripTrailingZero = true;
    }
  }

  auto result = PluralRules::TryCreate(locale.get(), options);
  if (result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return nullptr;
  }

  return result.unwrap().release();
}

static mozilla::intl::PluralRules* GetOrCreatePluralRules(
    JSContext* cx, Handle<PluralRulesObject*> pluralRules) {
  // Obtain a cached PluralRules object.
  mozilla::intl::PluralRules* pr = pluralRules->getPluralRules();
  if (pr) {
    return pr;
  }

  pr = NewPluralRules(cx, pluralRules);
  if (!pr) {
    return nullptr;
  }
  pluralRules->setPluralRules(pr);

  intl::AddICUCellMemory(pluralRules,
                         PluralRulesObject::UPluralRulesEstimatedMemoryUse);
  return pr;
}

/**
 * 16.5.3 ResolvePlural ( pluralRules, n )
 * 16.5.2 PluralRuleSelect ( locale, type, n, operands )
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
bool js::intl_SelectPluralRule(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);

  // Steps 1-2.
  Rooted<PluralRulesObject*> pluralRules(
      cx, &args[0].toObject().as<PluralRulesObject>());

  // Step 3.
  double x = args[1].toNumber();

  // Steps 4-11.
  using PluralRules = mozilla::intl::PluralRules;
  PluralRules* pr = GetOrCreatePluralRules(cx, pluralRules);
  if (!pr) {
    return false;
  }

  auto keywordResult = pr->Select(x);
  if (keywordResult.isErr()) {
    intl::ReportInternalError(cx, keywordResult.unwrapErr());
    return false;
  }

  JSString* str = KeywordToString(keywordResult.unwrap(), cx);
  MOZ_ASSERT(str);

  args.rval().setString(str);
  return true;
}

/**
 * 16.5.5 ResolvePluralRange ( pluralRules, x, y )
 * 16.5.4 PluralRuleSelectRange ( locale, type, xp, yp )
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
bool js::intl_SelectPluralRuleRange(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);

  // Steps 1-2.
  Rooted<PluralRulesObject*> pluralRules(
      cx, &args[0].toObject().as<PluralRulesObject>());

  // Steps 3-4.
  double x = args[1].toNumber();
  double y = args[2].toNumber();

  // Step 5.
  if (std::isnan(x)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NAN_NUMBER_RANGE, "start", "PluralRules",
                              "selectRange");
    return false;
  }
  if (std::isnan(y)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NAN_NUMBER_RANGE, "end", "PluralRules",
                              "selectRange");
    return false;
  }

  using PluralRules = mozilla::intl::PluralRules;
  PluralRules* pr = GetOrCreatePluralRules(cx, pluralRules);
  if (!pr) {
    return false;
  }

  // Steps 6-11.
  auto keywordResult = pr->SelectRange(x, y);
  if (keywordResult.isErr()) {
    intl::ReportInternalError(cx, keywordResult.unwrapErr());
    return false;
  }

  JSString* str = KeywordToString(keywordResult.unwrap(), cx);
  MOZ_ASSERT(str);

  args.rval().setString(str);
  return true;
}

bool js::intl_GetPluralCategories(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);

  Rooted<PluralRulesObject*> pluralRules(
      cx, &args[0].toObject().as<PluralRulesObject>());

  using PluralRules = mozilla::intl::PluralRules;
  PluralRules* pr = GetOrCreatePluralRules(cx, pluralRules);
  if (!pr) {
    return false;
  }

  auto categoriesResult = pr->Categories();
  if (categoriesResult.isErr()) {
    intl::ReportInternalError(cx, categoriesResult.unwrapErr());
    return false;
  }
  auto categories = categoriesResult.unwrap();

  ArrayObject* res = NewDenseFullyAllocatedArray(cx, categories.size());
  if (!res) {
    return false;
  }
  res->setDenseInitializedLength(categories.size());

  size_t index = 0;
  for (PluralRules::Keyword keyword : categories) {
    JSString* str = KeywordToString(keyword, cx);
    MOZ_ASSERT(str);

    res->initDenseElement(index++, StringValue(str));
  }
  MOZ_ASSERT(index == categories.size());

  args.rval().setObject(*res);
  return true;
}
