/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Intl.Collator implementation. */

#include "builtin/intl/Collator.h"

#include "mozilla/Assertions.h"
#include "mozilla/intl/Collator.h"
#include "mozilla/intl/Locale.h"
#include "mozilla/Span.h"

#include "builtin/Array.h"
#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/FormatBuffer.h"
#include "builtin/intl/LanguageTag.h"
#include "builtin/intl/SharedIntlData.h"
#include "gc/GCContext.h"
#include "js/PropertySpec.h"
#include "js/StableStringChars.h"
#include "js/TypeDecls.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/Runtime.h"
#include "vm/StringType.h"

#include "vm/GeckoProfiler-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;

using JS::AutoStableStringChars;

using js::intl::ReportInternalError;
using js::intl::SharedIntlData;

const JSClassOps CollatorObject::classOps_ = {
    nullptr,                   // addProperty
    nullptr,                   // delProperty
    nullptr,                   // enumerate
    nullptr,                   // newEnumerate
    nullptr,                   // resolve
    nullptr,                   // mayResolve
    CollatorObject::finalize,  // finalize
    nullptr,                   // call
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
    JS_FN("toSource", collator_toSource, 0, 0), JS_FS_END};

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
  AutoJSConstructorProfilerEntry pseudoFrame(cx, "Intl.Collator");

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

void js::CollatorObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  MOZ_ASSERT(gcx->onMainThread());

  if (mozilla::intl::Collator* coll = obj->as<CollatorObject>().getCollator()) {
    intl::RemoveICUCellMemory(gcx, obj, CollatorObject::EstimatedMemoryUse);
    delete coll;
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
  auto keywords =
      mozilla::intl::Collator::GetBcp47KeywordValuesForLocale(locale.get());
  if (keywords.isErr()) {
    ReportInternalError(cx, keywords.unwrapErr());
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

  for (auto result : keywords.unwrap()) {
    if (result.isErr()) {
      ReportInternalError(cx);
      return false;
    }
    mozilla::Span<const char> collation = result.unwrap();

    // Per ECMA-402, 10.2.3, we don't include standard and search:
    // "The values 'standard' and 'search' must not be used as elements in
    // any [[sortLocaleData]][locale].co and [[searchLocaleData]][locale].co
    // array."
    static constexpr auto standard = mozilla::MakeStringSpan("standard");
    static constexpr auto search = mozilla::MakeStringSpan("search");
    if (collation == standard || collation == search) {
      continue;
    }

    JSString* jscollation = NewStringCopy<CanGC>(cx, collation);
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
 * Returns a new mozilla::intl::Collator with the locale and collation options
 * of the given Collator.
 */
static mozilla::intl::Collator* NewIntlCollator(
    JSContext* cx, Handle<CollatorObject*> collator) {
  RootedValue value(cx);

  RootedObject internals(cx, intl::GetInternalsObject(cx, collator));
  if (!internals) {
    return nullptr;
  }

  if (!GetProperty(cx, internals, internals, cx->names().locale, &value)) {
    return nullptr;
  }

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

  using mozilla::intl::Collator;

  Collator::Options options{};

  if (!GetProperty(cx, internals, internals, cx->names().usage, &value)) {
    return nullptr;
  }

  enum class Usage { Search, Sort };

  Usage usage;
  {
    JSLinearString* str = value.toString()->ensureLinear(cx);
    if (!str) {
      return nullptr;
    }

    if (StringEqualsLiteral(str, "search")) {
      usage = Usage::Search;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(str, "sort"));
      usage = Usage::Sort;
    }
  }

  JS::RootedVector<intl::UnicodeExtensionKeyword> keywords(cx);

  // ICU expects collation as Unicode locale extensions on locale.
  if (usage == Usage::Search) {
    if (!keywords.emplaceBack("co", cx->names().search)) {
      return nullptr;
    }

    // Search collations can't select a different collation, so the collation
    // property is guaranteed to be "default".
#ifdef DEBUG
    if (!GetProperty(cx, internals, internals, cx->names().collation, &value)) {
      return nullptr;
    }

    JSLinearString* collation = value.toString()->ensureLinear(cx);
    if (!collation) {
      return nullptr;
    }

    MOZ_ASSERT(StringEqualsLiteral(collation, "default"));
#endif
  } else {
    if (!GetProperty(cx, internals, internals, cx->names().collation, &value)) {
      return nullptr;
    }

    JSLinearString* collation = value.toString()->ensureLinear(cx);
    if (!collation) {
      return nullptr;
    }

    // Set collation as a Unicode locale extension when it was specified.
    if (!StringEqualsLiteral(collation, "default")) {
      if (!keywords.emplaceBack("co", collation)) {
        return nullptr;
      }
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

  if (!GetProperty(cx, internals, internals, cx->names().sensitivity, &value)) {
    return nullptr;
  }

  {
    JSLinearString* sensitivity = value.toString()->ensureLinear(cx);
    if (!sensitivity) {
      return nullptr;
    }
    if (StringEqualsLiteral(sensitivity, "base")) {
      options.sensitivity = Collator::Sensitivity::Base;
    } else if (StringEqualsLiteral(sensitivity, "accent")) {
      options.sensitivity = Collator::Sensitivity::Accent;
    } else if (StringEqualsLiteral(sensitivity, "case")) {
      options.sensitivity = Collator::Sensitivity::Case;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(sensitivity, "variant"));
      options.sensitivity = Collator::Sensitivity::Variant;
    }
  }

  if (!GetProperty(cx, internals, internals, cx->names().ignorePunctuation,
                   &value)) {
    return nullptr;
  }
  options.ignorePunctuation = value.toBoolean();

  if (!GetProperty(cx, internals, internals, cx->names().numeric, &value)) {
    return nullptr;
  }
  if (!value.isUndefined()) {
    options.numeric = value.toBoolean();
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
      options.caseFirst = Collator::CaseFirst::Upper;
    } else if (StringEqualsLiteral(caseFirst, "lower")) {
      options.caseFirst = Collator::CaseFirst::Lower;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(caseFirst, "false"));
      options.caseFirst = Collator::CaseFirst::False;
    }
  }

  auto collResult = Collator::TryCreate(locale.get());
  if (collResult.isErr()) {
    ReportInternalError(cx, collResult.unwrapErr());
    return nullptr;
  }
  auto coll = collResult.unwrap();

  auto optResult = coll->SetOptions(options);
  if (optResult.isErr()) {
    ReportInternalError(cx, optResult.unwrapErr());
    return nullptr;
  }

  return coll.release();
}

static mozilla::intl::Collator* GetOrCreateCollator(
    JSContext* cx, Handle<CollatorObject*> collator) {
  // Obtain a cached mozilla::intl::Collator object.
  mozilla::intl::Collator* coll = collator->getCollator();
  if (coll) {
    return coll;
  }

  coll = NewIntlCollator(cx, collator);
  if (!coll) {
    return nullptr;
  }
  collator->setCollator(coll);

  intl::AddICUCellMemory(collator, CollatorObject::EstimatedMemoryUse);
  return coll;
}

static bool intl_CompareStrings(JSContext* cx, mozilla::intl::Collator* coll,
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

  result.setInt32(coll->CompareStrings(chars1, chars2));
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

  mozilla::intl::Collator* coll = GetOrCreateCollator(cx, collator);
  if (!coll) {
    return false;
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

bool js::intl_isIgnorePunctuation(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isString());

  SharedIntlData& sharedIntlData = cx->runtime()->sharedIntlData.ref();

  RootedString locale(cx, args[0].toString());
  bool isIgnorePunctuation;
  if (!sharedIntlData.isIgnorePunctuation(cx, locale, &isIgnorePunctuation)) {
    return false;
  }

  args.rval().setBoolean(isIgnorePunctuation);
  return true;
}
