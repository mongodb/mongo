/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Implementation of the Intl.PluralRules proposal. */

#include "builtin/intl/PluralRules.h"

#include "mozilla/Assertions.h"
#include "mozilla/Casting.h"
#include "mozilla/intl/NumberFormat.h"
#include "mozilla/intl/PluralRules.h"

#include "builtin/Array.h"
#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/ScopedICUObject.h"
#include "gc/FreeOp.h"
#include "js/CharacterEncoding.h"
#include "js/PropertySpec.h"
#include "unicode/uenum.h"
#include "unicode/uloc.h"
#include "unicode/unumberformatter.h"
#include "unicode/upluralrules.h"
#include "unicode/utypes.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/StringType.h"
#include "vm/WellKnownAtom.h"  // js_*_str

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::AssertedCast;

using js::intl::CallICU;
using js::intl::IcuLocale;

const JSClassOps PluralRulesObject::classOps_ = {
    nullptr,                      // addProperty
    nullptr,                      // delProperty
    nullptr,                      // enumerate
    nullptr,                      // newEnumerate
    nullptr,                      // resolve
    nullptr,                      // mayResolve
    PluralRulesObject::finalize,  // finalize
    nullptr,                      // call
    nullptr,                      // hasInstance
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
    JS_FN(js_toSource_str, pluralRules_toSource, 0, 0), JS_FS_END};

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
 * PluralRules constructor.
 * Spec: ECMAScript 402 API, PluralRules, 13.2.1
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

void js::PluralRulesObject::finalize(JSFreeOp* fop, JSObject* obj) {
  MOZ_ASSERT(fop->onMainThread());

  auto* pluralRules = &obj->as<PluralRulesObject>();
  if (mozilla::intl::PluralRules* pr = pluralRules->getPluralRules()) {
    intl::RemoveICUCellMemory(
        fop, obj, PluralRulesObject::UPluralRulesEstimatedMemoryUse);
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
  } else {
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

  if (!GetProperty(cx, internals, internals, cx->names().minimumIntegerDigits,
                   &value)) {
    return nullptr;
  }
  options.mMinIntegerDigits =
      mozilla::Some(AssertedCast<uint32_t>(value.toInt32()));

  mozilla::Result<mozilla::UniquePtr<PluralRules>, PluralRules::Error> result =
      PluralRules::TryCreate(locale.get(), options);
  if (result.isErr()) {
    intl::ReportInternalError(cx);
    return nullptr;
  }

  return result.unwrap().release();
}

bool js::intl_SelectPluralRule(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);

  Rooted<PluralRulesObject*> pluralRules(
      cx, &args[0].toObject().as<PluralRulesObject>());

  double x = args[1].toNumber();

  // Obtain a cached PluralRules object.
  using PluralRules = mozilla::intl::PluralRules;
  PluralRules* pr = pluralRules->getPluralRules();
  if (!pr) {
    pr = NewPluralRules(cx, pluralRules);
    if (!pr) {
      return false;
    }
    pluralRules->setPluralRules(pr);

    intl::AddICUCellMemory(pluralRules,
                           PluralRulesObject::UPluralRulesEstimatedMemoryUse);
  }

  Result<PluralRules::Keyword, PluralRules::Error> keywordResult =
      pr->Select(x);
  if (keywordResult.isErr()) {
    intl::ReportInternalError(cx);
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

  // Obtain a cached PluralRules object.
  using PluralRules = mozilla::intl::PluralRules;
  PluralRules* pr = pluralRules->getPluralRules();
  if (!pr) {
    pr = NewPluralRules(cx, pluralRules);
    if (!pr) {
      return false;
    }
    pluralRules->setPluralRules(pr);

    intl::AddICUCellMemory(pluralRules,
                           PluralRulesObject::UPluralRulesEstimatedMemoryUse);
  }

  auto categoriesResult = pr->Categories();
  if (categoriesResult.isErr()) {
    intl::ReportInternalError(cx);
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
