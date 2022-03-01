/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Intl.Collator implementation. */

#include "builtin/intl/Collator.h"

#include "mozilla/Assertions.h"
#include "mozilla/Span.h"

#include "jsapi.h"

#include "builtin/Array.h"
#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/LanguageTag.h"
#include "builtin/intl/ScopedICUObject.h"
#include "builtin/intl/SharedIntlData.h"
#include "gc/FreeOp.h"
#include "js/CharacterEncoding.h"
#include "js/PropertySpec.h"
#include "js/StableStringChars.h"
#include "js/TypeDecls.h"
#include "unicode/ucol.h"
#include "unicode/uenum.h"
#include "unicode/uloc.h"
#include "unicode/utypes.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/Runtime.h"
#include "vm/StringType.h"
#include "vm/WellKnownAtom.h"  // js_*_str

#include "vm/JSObject-inl.h"

using namespace js;

using JS::AutoStableStringChars;

using js::intl::IcuLocale;
using js::intl::ReportInternalError;
using js::intl::SharedIntlData;
using js::intl::StringsAreEqual;

const JSClassOps CollatorObject::classOps_ = {
    nullptr,                   // addProperty
    nullptr,                   // delProperty
    nullptr,                   // enumerate
    nullptr,                   // newEnumerate
    nullptr,                   // resolve
    nullptr,                   // mayResolve
    CollatorObject::finalize,  // finalize
    nullptr,                   // call
    nullptr,                   // hasInstance
    nullptr,                   // construct
    nullptr,                   // trace
};

const JSClass CollatorObject::class_ = {
    "Intl.Collator",
    JSCLASS_HAS_RESERVED_SLOTS(CollatorObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_Collator) |
        JSCLASS_FOREGROUND_FINALIZE,
    &CollatorObject::classOps_, &CollatorObject::classSpec_};

const JSClass& CollatorObject::protoClass_ = PlainObject::class_;

static bool collator_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setString(cx->names().Collator);
  return true;
}

static const JSFunctionSpec collator_static_methods[] = {
    JS_SELF_HOSTED_FN("supportedLocalesOf", "Intl_Collator_supportedLocalesOf",
                      1, 0),
    JS_FS_END};

static const JSFunctionSpec collator_methods[] = {
    JS_SELF_HOSTED_FN("resolvedOptions", "Intl_Collator_resolvedOptions", 0, 0),
    JS_FN(js_toSource_str, collator_toSource, 0, 0), JS_FS_END};

static const JSPropertySpec collator_properties[] = {
    JS_SELF_HOSTED_GET("compare", "$Intl_Collator_compare_get", 0),
    JS_STRING_SYM_PS(toStringTag, "Intl.Collator", JSPROP_READONLY), JS_PS_END};

static bool Collator(JSContext* cx, unsigned argc, Value* vp);

const ClassSpec CollatorObject::classSpec_ = {
    GenericCreateConstructor<Collator, 0, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<CollatorObject>,
    collator_static_methods,
    nullptr,
    collator_methods,
    collator_properties,
    nullptr,
    ClassSpec::DontDefineConstructor};

/**
 * 10.1.2 Intl.Collator([ locales [, options]])
 *
 * ES2017 Intl draft rev 94045d234762ad107a3d09bb6f7381a65f1a2f9b
 */
static bool Collator(JSContext* cx, const CallArgs& args) {
  // Step 1 (Handled by OrdinaryCreateFromConstructor fallback code).

  // Steps 2-5 (Inlined 9.1.14, OrdinaryCreateFromConstructor).
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_Collator, &proto)) {
    return false;
  }

  Rooted<CollatorObject*> collator(
      cx, NewObjectWithClassProto<CollatorObject>(cx, proto));
  if (!collator) {
    return false;
  }

  HandleValue locales = args.get(0);
  HandleValue options = args.get(1);

  // Step 6.
  if (!intl::InitializeObject(cx, collator, cx->names().InitializeCollator,
                              locales, options)) {
    return false;
  }

  args.rval().setObject(*collator);
  return true;
}

static bool Collator(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return Collator(cx, args);
}

bool js::intl_Collator(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  MOZ_ASSERT(!args.isConstructing());

  return Collator(cx, args);
}

void js::CollatorObject::finalize(JSFreeOp* fop, JSObject* obj) {
  MOZ_ASSERT(fop->onMainThread());

  if (UCollator* coll = obj->as<CollatorObject>().getCollator()) {
    intl::RemoveICUCellMemory(fop, obj, CollatorObject::EstimatedMemoryUse);

    ucol_close(coll);
  }
}

bool js::intl_availableCollations(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isString());

  UniqueChars locale = intl::EncodeLocale(cx, args[0].toString());
  if (!locale) {
    return false;
  }
  UErrorCode status = U_ZERO_ERROR;
  UEnumeration* values =
      ucol_getKeywordValuesForLocale("co", locale.get(), false, &status);
  if (U_FAILURE(status)) {
    ReportInternalError(cx);
    return false;
  }
  ScopedICUObject<UEnumeration, uenum_close> toClose(values);

  uint32_t count = uenum_count(values, &status);
  if (U_FAILURE(status)) {
    ReportInternalError(cx);
    return false;
  }

  RootedObject collations(cx, NewDenseEmptyArray(cx));
  if (!collations) {
    return false;
  }

  // The first element of the collations array must be |null| per
  // ES2017 Intl, 10.2.3 Internal Slots.
  if (!NewbornArrayPush(cx, collations, NullValue())) {
    return false;
  }

  for (uint32_t i = 0; i < count; i++) {
    const char* collation = uenum_next(values, nullptr, &status);
    if (U_FAILURE(status)) {
      ReportInternalError(cx);
      return false;
    }

    // Per ECMA-402, 10.2.3, we don't include standard and search:
    // "The values 'standard' and 'search' must not be used as elements in
    // any [[sortLocaleData]][locale].co and [[searchLocaleData]][locale].co
    // array."
    if (StringsAreEqual(collation, "standard") ||
        StringsAreEqual(collation, "search")) {
      continue;
    }

    // ICU returns old-style keyword values; map them to BCP 47 equivalents.
    collation = uloc_toUnicodeLocaleType("co", collation);
    if (!collation) {
      ReportInternalError(cx);
      return false;
    }

    JSString* jscollation = NewStringCopyZ<CanGC>(cx, collation);
    if (!jscollation) {
      return false;
    }
    if (!NewbornArrayPush(cx, collations, StringValue(jscollation))) {
      return false;
    }
  }

  args.rval().setObject(*collations);
  return true;
}

/**
 * Returns a new UCollator with the locale and collation options
 * of the given Collator.
 */
static UCollator* NewUCollator(JSContext* cx,
                               Handle<CollatorObject*> collator) {
  RootedValue value(cx);

  RootedObject internals(cx, intl::GetInternalsObject(cx, collator));
  if (!internals) {
    return nullptr;
  }

  if (!GetProperty(cx, internals, internals, cx->names().locale, &value)) {
    return nullptr;
  }
  UniqueChars locale = intl::EncodeLocale(cx, value.toString());
  if (!locale) {
    return nullptr;
  }

  // UCollator options with default values.
  UColAttributeValue uStrength = UCOL_DEFAULT;
  UColAttributeValue uCaseLevel = UCOL_OFF;
  UColAttributeValue uAlternate = UCOL_DEFAULT;
  UColAttributeValue uNumeric = UCOL_OFF;
  // Normalization is always on to meet the canonical equivalence requirement.
  UColAttributeValue uNormalization = UCOL_ON;
  UColAttributeValue uCaseFirst = UCOL_DEFAULT;

  if (!GetProperty(cx, internals, internals, cx->names().usage, &value)) {
    return nullptr;
  }

  {
    JSLinearString* usage = value.toString()->ensureLinear(cx);
    if (!usage) {
      return nullptr;
    }
    if (StringEqualsLiteral(usage, "search")) {
      // ICU expects search as a Unicode locale extension on locale.
      intl::LanguageTag tag(cx);
      if (!intl::LanguageTagParser::parse(
              cx, mozilla::MakeStringSpan(locale.get()), tag)) {
        return nullptr;
      }

      JS::RootedVector<intl::UnicodeExtensionKeyword> keywords(cx);

      if (!keywords.emplaceBack("co", cx->names().search)) {
        return nullptr;
      }

      // |ApplyUnicodeExtensionToTag| applies the new keywords to the front of
      // the Unicode extension subtag. We're then relying on ICU to follow RFC
      // 6067, which states that any trailing keywords using the same key
      // should be ignored.
      if (!intl::ApplyUnicodeExtensionToTag(cx, tag, keywords)) {
        return nullptr;
      }

      locale = tag.toStringZ(cx);
      if (!locale) {
        return nullptr;
      }
    } else {
      MOZ_ASSERT(StringEqualsLiteral(usage, "sort"));
    }
  }

  // We don't need to look at the collation property - it can only be set
  // via the Unicode locale extension and is therefore already set on
  // locale.

  if (!GetProperty(cx, internals, internals, cx->names().sensitivity, &value)) {
    return nullptr;
  }

  {
    JSLinearString* sensitivity = value.toString()->ensureLinear(cx);
    if (!sensitivity) {
      return nullptr;
    }
    if (StringEqualsLiteral(sensitivity, "base")) {
      uStrength = UCOL_PRIMARY;
    } else if (StringEqualsLiteral(sensitivity, "accent")) {
      uStrength = UCOL_SECONDARY;
    } else if (StringEqualsLiteral(sensitivity, "case")) {
      uStrength = UCOL_PRIMARY;
      uCaseLevel = UCOL_ON;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(sensitivity, "variant"));
      uStrength = UCOL_TERTIARY;
    }
  }

  if (!GetProperty(cx, internals, internals, cx->names().ignorePunctuation,
                   &value)) {
    return nullptr;
  }
  // According to the ICU team, UCOL_SHIFTED causes punctuation to be
  // ignored. Looking at Unicode Technical Report 35, Unicode Locale Data
  // Markup Language, "shifted" causes whitespace and punctuation to be
  // ignored - that's a bit more than asked for, but there's no way to get
  // less.
  if (value.toBoolean()) {
    uAlternate = UCOL_SHIFTED;
  }

  if (!GetProperty(cx, internals, internals, cx->names().numeric, &value)) {
    return nullptr;
  }
  if (!value.isUndefined() && value.toBoolean()) {
    uNumeric = UCOL_ON;
  }

  if (!GetProperty(cx, internals, internals, cx->names().caseFirst, &value)) {
    return nullptr;
  }
  if (!value.isUndefined()) {
    JSLinearString* caseFirst = value.toString()->ensureLinear(cx);
    if (!caseFirst) {
      return nullptr;
    }
    if (StringEqualsLiteral(caseFirst, "upper")) {
      uCaseFirst = UCOL_UPPER_FIRST;
    } else if (StringEqualsLiteral(caseFirst, "lower")) {
      uCaseFirst = UCOL_LOWER_FIRST;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(caseFirst, "false"));
      uCaseFirst = UCOL_OFF;
    }
  }

  UErrorCode status = U_ZERO_ERROR;
  UCollator* coll = ucol_open(IcuLocale(locale.get()), &status);
  if (U_FAILURE(status)) {
    ReportInternalError(cx);
    return nullptr;
  }

  ucol_setAttribute(coll, UCOL_STRENGTH, uStrength, &status);
  ucol_setAttribute(coll, UCOL_CASE_LEVEL, uCaseLevel, &status);
  ucol_setAttribute(coll, UCOL_ALTERNATE_HANDLING, uAlternate, &status);
  ucol_setAttribute(coll, UCOL_NUMERIC_COLLATION, uNumeric, &status);
  ucol_setAttribute(coll, UCOL_NORMALIZATION_MODE, uNormalization, &status);
  ucol_setAttribute(coll, UCOL_CASE_FIRST, uCaseFirst, &status);
  if (U_FAILURE(status)) {
    ucol_close(coll);
    ReportInternalError(cx);
    return nullptr;
  }

  return coll;
}

static bool intl_CompareStrings(JSContext* cx, UCollator* coll,
                                HandleString str1, HandleString str2,
                                MutableHandleValue result) {
  MOZ_ASSERT(str1);
  MOZ_ASSERT(str2);

  if (str1 == str2) {
    result.setInt32(0);
    return true;
  }

  AutoStableStringChars stableChars1(cx);
  if (!stableChars1.initTwoByte(cx, str1)) {
    return false;
  }

  AutoStableStringChars stableChars2(cx);
  if (!stableChars2.initTwoByte(cx, str2)) {
    return false;
  }

  mozilla::Range<const char16_t> chars1 = stableChars1.twoByteRange();
  mozilla::Range<const char16_t> chars2 = stableChars2.twoByteRange();

  UCollationResult uresult =
      ucol_strcoll(coll, chars1.begin().get(), chars1.length(),
                   chars2.begin().get(), chars2.length());
  int32_t res;
  switch (uresult) {
    case UCOL_LESS:
      res = -1;
      break;
    case UCOL_EQUAL:
      res = 0;
      break;
    case UCOL_GREATER:
      res = 1;
      break;
    default:
      MOZ_CRASH("ucol_strcoll returned bad UCollationResult");
  }
  result.setInt32(res);
  return true;
}

bool js::intl_CompareStrings(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);
  MOZ_ASSERT(args[0].isObject());
  MOZ_ASSERT(args[1].isString());
  MOZ_ASSERT(args[2].isString());

  Rooted<CollatorObject*> collator(cx,
                                   &args[0].toObject().as<CollatorObject>());

  // Obtain a cached UCollator object.
  UCollator* coll = collator->getCollator();
  if (!coll) {
    coll = NewUCollator(cx, collator);
    if (!coll) {
      return false;
    }
    collator->setCollator(coll);

    intl::AddICUCellMemory(collator, CollatorObject::EstimatedMemoryUse);
  }

  // Use the UCollator to actually compare the strings.
  RootedString str1(cx, args[1].toString());
  RootedString str2(cx, args[2].toString());
  return intl_CompareStrings(cx, coll, str1, str2, args.rval());
}

bool js::intl_isUpperCaseFirst(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isString());

  SharedIntlData& sharedIntlData = cx->runtime()->sharedIntlData.ref();

  RootedString locale(cx, args[0].toString());
  bool isUpperFirst;
  if (!sharedIntlData.isUpperCaseFirst(cx, locale, &isUpperFirst)) {
    return false;
  }

  args.rval().setBoolean(isUpperFirst);
  return true;
}
