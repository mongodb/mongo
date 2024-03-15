/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/intl/ListFormat.h"

#include "mozilla/Assertions.h"
#include "mozilla/intl/ListFormat.h"

#include <stddef.h>

#include "builtin/Array.h"
#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/FormatBuffer.h"
#include "gc/GCContext.h"
#include "js/Utility.h"
#include "js/Vector.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/StringType.h"
#include "vm/WellKnownAtom.h"  // js_*_str

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/ObjectOperations-inl.h"

using namespace js;

const JSClassOps ListFormatObject::classOps_ = {
    nullptr,                     // addProperty
    nullptr,                     // delProperty
    nullptr,                     // enumerate
    nullptr,                     // newEnumerate
    nullptr,                     // resolve
    nullptr,                     // mayResolve
    ListFormatObject::finalize,  // finalize
    nullptr,                     // call
    nullptr,                     // construct
    nullptr,                     // trace
};
const JSClass ListFormatObject::class_ = {
    "Intl.ListFormat",
    JSCLASS_HAS_RESERVED_SLOTS(ListFormatObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_ListFormat) |
        JSCLASS_FOREGROUND_FINALIZE,
    &ListFormatObject::classOps_, &ListFormatObject::classSpec_};

const JSClass& ListFormatObject::protoClass_ = PlainObject::class_;

static bool listFormat_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setString(cx->names().ListFormat);
  return true;
}

static const JSFunctionSpec listFormat_static_methods[] = {
    JS_SELF_HOSTED_FN("supportedLocalesOf",
                      "Intl_ListFormat_supportedLocalesOf", 1, 0),
    JS_FS_END};

static const JSFunctionSpec listFormat_methods[] = {
    JS_SELF_HOSTED_FN("resolvedOptions", "Intl_ListFormat_resolvedOptions", 0,
                      0),
    JS_SELF_HOSTED_FN("format", "Intl_ListFormat_format", 1, 0),
    JS_SELF_HOSTED_FN("formatToParts", "Intl_ListFormat_formatToParts", 1, 0),
    JS_FN(js_toSource_str, listFormat_toSource, 0, 0), JS_FS_END};

static const JSPropertySpec listFormat_properties[] = {
    JS_STRING_SYM_PS(toStringTag, "Intl.ListFormat", JSPROP_READONLY),
    JS_PS_END};

static bool ListFormat(JSContext* cx, unsigned argc, Value* vp);

const ClassSpec ListFormatObject::classSpec_ = {
    GenericCreateConstructor<ListFormat, 0, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<ListFormatObject>,
    listFormat_static_methods,
    nullptr,
    listFormat_methods,
    listFormat_properties,
    nullptr,
    ClassSpec::DontDefineConstructor};

/**
 * Intl.ListFormat([ locales [, options]])
 */
static bool ListFormat(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Intl.ListFormat")) {
    return false;
  }

  // Step 2 (Inlined 9.1.14, OrdinaryCreateFromConstructor).
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_ListFormat,
                                          &proto)) {
    return false;
  }

  Rooted<ListFormatObject*> listFormat(
      cx, NewObjectWithClassProto<ListFormatObject>(cx, proto));
  if (!listFormat) {
    return false;
  }

  HandleValue locales = args.get(0);
  HandleValue options = args.get(1);

  // Step 3.
  if (!intl::InitializeObject(cx, listFormat, cx->names().InitializeListFormat,
                              locales, options)) {
    return false;
  }

  args.rval().setObject(*listFormat);
  return true;
}

void js::ListFormatObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  MOZ_ASSERT(gcx->onMainThread());

  mozilla::intl::ListFormat* lf =
      obj->as<ListFormatObject>().getListFormatSlot();
  if (lf) {
    intl::RemoveICUCellMemory(gcx, obj, ListFormatObject::EstimatedMemoryUse);
    delete lf;
  }
}

/**
 * Returns a new ListFormat with the locale and list formatting options
 * of the given ListFormat.
 */
static mozilla::intl::ListFormat* NewListFormat(
    JSContext* cx, Handle<ListFormatObject*> listFormat) {
  RootedObject internals(cx, intl::GetInternalsObject(cx, listFormat));
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

  mozilla::intl::ListFormat::Options options;

  using ListFormatType = mozilla::intl::ListFormat::Type;
  if (!GetProperty(cx, internals, internals, cx->names().type, &value)) {
    return nullptr;
  }
  {
    JSLinearString* strType = value.toString()->ensureLinear(cx);
    if (!strType) {
      return nullptr;
    }

    if (StringEqualsLiteral(strType, "conjunction")) {
      options.mType = ListFormatType::Conjunction;
    } else if (StringEqualsLiteral(strType, "disjunction")) {
      options.mType = ListFormatType::Disjunction;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(strType, "unit"));
      options.mType = ListFormatType::Unit;
    }
  }

  using ListFormatStyle = mozilla::intl::ListFormat::Style;
  if (!GetProperty(cx, internals, internals, cx->names().style, &value)) {
    return nullptr;
  }
  {
    JSLinearString* strStyle = value.toString()->ensureLinear(cx);
    if (!strStyle) {
      return nullptr;
    }

    if (StringEqualsLiteral(strStyle, "long")) {
      options.mStyle = ListFormatStyle::Long;
    } else if (StringEqualsLiteral(strStyle, "short")) {
      options.mStyle = ListFormatStyle::Short;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(strStyle, "narrow"));
      options.mStyle = ListFormatStyle::Narrow;
    }
  }

  auto result = mozilla::intl::ListFormat::TryCreate(
      mozilla::MakeStringSpan(locale.get()), options);

  if (result.isOk()) {
    return result.unwrap().release();
  }

  js::intl::ReportInternalError(cx, result.unwrapErr());
  return nullptr;
}

static mozilla::intl::ListFormat* GetOrCreateListFormat(
    JSContext* cx, Handle<ListFormatObject*> listFormat) {
  // Obtain a cached mozilla::intl::ListFormat object.
  mozilla::intl::ListFormat* lf = listFormat->getListFormatSlot();
  if (lf) {
    return lf;
  }

  lf = NewListFormat(cx, listFormat);
  if (!lf) {
    return nullptr;
  }
  listFormat->setListFormatSlot(lf);

  intl::AddICUCellMemory(listFormat, ListFormatObject::EstimatedMemoryUse);
  return lf;
}

/**
 * FormatList ( listFormat, list )
 */
static bool FormatList(JSContext* cx, mozilla::intl::ListFormat* lf,
                       const mozilla::intl::ListFormat::StringList& list,
                       MutableHandleValue result) {
  intl::FormatBuffer<char16_t, intl::INITIAL_CHAR_BUFFER_SIZE> formatBuffer(cx);
  auto formatResult = lf->Format(list, formatBuffer);
  if (formatResult.isErr()) {
    js::intl::ReportInternalError(cx, formatResult.unwrapErr());
    return false;
  }

  JSString* str = formatBuffer.toString(cx);
  if (!str) {
    return false;
  }
  result.setString(str);
  return true;
}

/**
 * FormatListToParts ( listFormat, list )
 */
static bool FormatListToParts(JSContext* cx, mozilla::intl::ListFormat* lf,
                              const mozilla::intl::ListFormat::StringList& list,
                              MutableHandleValue result) {
  intl::FormatBuffer<char16_t, intl::INITIAL_CHAR_BUFFER_SIZE> buffer(cx);
  mozilla::intl::ListFormat::PartVector parts;
  auto formatResult = lf->FormatToParts(list, buffer, parts);
  if (formatResult.isErr()) {
    intl::ReportInternalError(cx, formatResult.unwrapErr());
    return false;
  }

  RootedString overallResult(cx, buffer.toString(cx));
  if (!overallResult) {
    return false;
  }

  Rooted<ArrayObject*> partsArray(
      cx, NewDenseFullyAllocatedArray(cx, parts.length()));
  if (!partsArray) {
    return false;
  }
  partsArray->ensureDenseInitializedLength(0, parts.length());

  RootedObject singlePart(cx);
  RootedValue val(cx);

  size_t index = 0;
  size_t beginIndex = 0;
  for (const mozilla::intl::ListFormat::Part& part : parts) {
    singlePart = NewPlainObject(cx);
    if (!singlePart) {
      return false;
    }

    if (part.first == mozilla::intl::ListFormat::PartType::Element) {
      val = StringValue(cx->names().element);
    } else {
      val = StringValue(cx->names().literal);
    }

    if (!DefineDataProperty(cx, singlePart, cx->names().type, val)) {
      return false;
    }

    // There could be an empty string so the endIndex coule be equal to
    // beginIndex.
    MOZ_ASSERT(part.second >= beginIndex);
    JSLinearString* partStr = NewDependentString(cx, overallResult, beginIndex,
                                                 part.second - beginIndex);
    if (!partStr) {
      return false;
    }
    val = StringValue(partStr);
    if (!DefineDataProperty(cx, singlePart, cx->names().value, val)) {
      return false;
    }

    beginIndex = part.second;
    partsArray->initDenseElement(index++, ObjectValue(*singlePart));
  }

  MOZ_ASSERT(index == parts.length());
  MOZ_ASSERT(beginIndex == buffer.length());
  result.setObject(*partsArray);

  return true;
}

bool js::intl_FormatList(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);

  Rooted<ListFormatObject*> listFormat(
      cx, &args[0].toObject().as<ListFormatObject>());

  bool formatToParts = args[2].toBoolean();

  mozilla::intl::ListFormat* lf = GetOrCreateListFormat(cx, listFormat);
  if (!lf) {
    return false;
  }

  // Collect all strings and their lengths.
  //
  // 'strings' takes the ownership of those strings, and 'list' will be passed
  // to mozilla::intl::ListFormat as a Span.
  Vector<UniqueTwoByteChars, mozilla::intl::DEFAULT_LIST_LENGTH> strings(cx);
  mozilla::intl::ListFormat::StringList list;

  Rooted<ArrayObject*> listObj(cx, &args[1].toObject().as<ArrayObject>());
  RootedValue value(cx);
  uint32_t listLen = listObj->length();
  for (uint32_t i = 0; i < listLen; i++) {
    if (!GetElement(cx, listObj, listObj, i, &value)) {
      return false;
    }

    JSLinearString* linear = value.toString()->ensureLinear(cx);
    if (!linear) {
      return false;
    }

    size_t linearLength = linear->length();

    UniqueTwoByteChars chars = cx->make_pod_array<char16_t>(linearLength);
    if (!chars) {
      return false;
    }
    CopyChars(chars.get(), *linear);

    if (!strings.append(std::move(chars))) {
      return false;
    }

    if (!list.emplaceBack(strings[i].get(), linearLength)) {
      return false;
    }
  }

  if (formatToParts) {
    return FormatListToParts(cx, lf, list, args.rval());
  }
  return FormatList(cx, lf, list, args.rval());
}
