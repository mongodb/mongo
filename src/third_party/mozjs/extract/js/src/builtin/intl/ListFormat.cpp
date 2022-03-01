/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/intl/ListFormat.h"

#include "mozilla/Assertions.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/PodOperations.h"

#include <stddef.h>
#include <stdint.h>

#include "builtin/Array.h"
#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/ScopedICUObject.h"
#include "gc/FreeOp.h"
#include "js/Utility.h"
#include "js/Vector.h"
#include "unicode/uformattedvalue.h"
#include "unicode/ulistformatter.h"
#include "unicode/utypes.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/Runtime.h"      // js::ReportAllocationOverflow
#include "vm/SelfHosting.h"
#include "vm/Stack.h"
#include "vm/StringType.h"
#include "vm/WellKnownAtom.h"  // js_*_str

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/ObjectOperations-inl.h"

using namespace js;

using mozilla::CheckedInt;

using js::intl::CallICU;
using js::intl::IcuLocale;

const JSClassOps ListFormatObject::classOps_ = {
    nullptr,                     // addProperty
    nullptr,                     // delProperty
    nullptr,                     // enumerate
    nullptr,                     // newEnumerate
    nullptr,                     // resolve
    nullptr,                     // mayResolve
    ListFormatObject::finalize,  // finalize
    nullptr,                     // call
    nullptr,                     // hasInstance
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

void js::ListFormatObject::finalize(JSFreeOp* fop, JSObject* obj) {
  MOZ_ASSERT(fop->onMainThread());

  if (UListFormatter* lf = obj->as<ListFormatObject>().getListFormatter()) {
    intl::RemoveICUCellMemory(fop, obj, ListFormatObject::EstimatedMemoryUse);

    ulistfmt_close(lf);
  }
}

/**
 * Returns a new UListFormatter with the locale and list formatting options
 * of the given ListFormat.
 */
static UListFormatter* NewUListFormatter(JSContext* cx,
                                         Handle<ListFormatObject*> listFormat) {
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

  enum class ListFormatType { Conjunction, Disjunction, Unit };

  ListFormatType type;
  if (!GetProperty(cx, internals, internals, cx->names().type, &value)) {
    return nullptr;
  }
  {
    JSLinearString* strType = value.toString()->ensureLinear(cx);
    if (!strType) {
      return nullptr;
    }

    if (StringEqualsLiteral(strType, "conjunction")) {
      type = ListFormatType::Conjunction;
    } else if (StringEqualsLiteral(strType, "disjunction")) {
      type = ListFormatType::Disjunction;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(strType, "unit"));
      type = ListFormatType::Unit;
    }
  }

  enum class ListFormatStyle { Long, Short, Narrow };

  ListFormatStyle style;
  if (!GetProperty(cx, internals, internals, cx->names().style, &value)) {
    return nullptr;
  }
  {
    JSLinearString* strStyle = value.toString()->ensureLinear(cx);
    if (!strStyle) {
      return nullptr;
    }

    if (StringEqualsLiteral(strStyle, "long")) {
      style = ListFormatStyle::Long;
    } else if (StringEqualsLiteral(strStyle, "short")) {
      style = ListFormatStyle::Short;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(strStyle, "narrow"));
      style = ListFormatStyle::Narrow;
    }
  }

  UListFormatterType utype;
  switch (type) {
    case ListFormatType::Conjunction:
      utype = ULISTFMT_TYPE_AND;
      break;
    case ListFormatType::Disjunction:
      utype = ULISTFMT_TYPE_OR;
      break;
    case ListFormatType::Unit:
      utype = ULISTFMT_TYPE_UNITS;
      break;
  }

  UListFormatterWidth uwidth;
  switch (style) {
    case ListFormatStyle::Long:
      uwidth = ULISTFMT_WIDTH_WIDE;
      break;
    case ListFormatStyle::Short:
      uwidth = ULISTFMT_WIDTH_SHORT;
      break;
    case ListFormatStyle::Narrow:
      uwidth = ULISTFMT_WIDTH_NARROW;
      break;
  }

  UErrorCode status = U_ZERO_ERROR;
  UListFormatter* lf =
      ulistfmt_openForType(IcuLocale(locale.get()), utype, uwidth, &status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return nullptr;
  }
  return lf;
}

static constexpr size_t DEFAULT_LIST_LENGTH = 8;

using ListFormatStringVector = Vector<UniqueTwoByteChars, DEFAULT_LIST_LENGTH>;
using ListFormatStringLengthVector = Vector<int32_t, DEFAULT_LIST_LENGTH>;

static_assert(sizeof(UniqueTwoByteChars) == sizeof(char16_t*),
              "UniqueTwoByteChars are stored efficiently and are held in "
              "continuous memory");

/**
 * FormatList ( listFormat, list )
 */
static bool FormatList(JSContext* cx, UListFormatter* lf,
                       const ListFormatStringVector& strings,
                       const ListFormatStringLengthVector& stringLengths,
                       MutableHandleValue result) {
  MOZ_ASSERT(strings.length() == stringLengths.length());
  MOZ_ASSERT(strings.length() <= INT32_MAX);

  JSString* str = intl::CallICU(cx, [lf, &strings, &stringLengths](
                                        UChar* chars, int32_t size,
                                        UErrorCode* status) {
    return ulistfmt_format(
        lf, reinterpret_cast<char16_t* const*>(strings.begin()),
        stringLengths.begin(), int32_t(strings.length()), chars, size, status);
  });
  if (!str) {
    return false;
  }

  result.setString(str);
  return true;
}

/**
 * FormatListToParts ( listFormat, list )
 */
static bool FormatListToParts(JSContext* cx, UListFormatter* lf,
                              const ListFormatStringVector& strings,
                              const ListFormatStringLengthVector& stringLengths,
                              MutableHandleValue result) {
  MOZ_ASSERT(strings.length() == stringLengths.length());
  MOZ_ASSERT(strings.length() <= INT32_MAX);

  UErrorCode status = U_ZERO_ERROR;
  UFormattedList* formatted = ulistfmt_openResult(&status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return false;
  }
  ScopedICUObject<UFormattedList, ulistfmt_closeResult> toClose(formatted);

  ulistfmt_formatStringsToResult(
      lf, reinterpret_cast<char16_t* const*>(strings.begin()),
      stringLengths.begin(), int32_t(strings.length()), formatted, &status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return false;
  }

  const UFormattedValue* formattedValue =
      ulistfmt_resultAsValue(formatted, &status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return false;
  }

  RootedString overallResult(cx,
                             intl::FormattedValueToString(cx, formattedValue));
  if (!overallResult) {
    return false;
  }

  RootedArrayObject partsArray(cx, NewDenseEmptyArray(cx));
  if (!partsArray) {
    return false;
  }

  using FieldType = js::ImmutablePropertyNamePtr JSAtomState::*;

  size_t lastEndIndex = 0;
  RootedObject singlePart(cx);
  RootedValue val(cx);

  auto AppendPart = [&](FieldType type, size_t beginIndex, size_t endIndex) {
    singlePart = NewBuiltinClassInstance<PlainObject>(cx);
    if (!singlePart) {
      return false;
    }

    val = StringValue(cx->names().*type);
    if (!DefineDataProperty(cx, singlePart, cx->names().type, val)) {
      return false;
    }

    JSLinearString* partSubstr = NewDependentString(
        cx, overallResult, beginIndex, endIndex - beginIndex);
    if (!partSubstr) {
      return false;
    }

    val = StringValue(partSubstr);
    if (!DefineDataProperty(cx, singlePart, cx->names().value, val)) {
      return false;
    }

    if (!NewbornArrayPush(cx, partsArray, ObjectValue(*singlePart))) {
      return false;
    }

    lastEndIndex = endIndex;
    return true;
  };

  UConstrainedFieldPosition* fpos = ucfpos_open(&status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return false;
  }
  ScopedICUObject<UConstrainedFieldPosition, ucfpos_close> toCloseFpos(fpos);

  // We're only interested in ULISTFMT_ELEMENT_FIELD fields.
  ucfpos_constrainField(fpos, UFIELD_CATEGORY_LIST, ULISTFMT_ELEMENT_FIELD,
                        &status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return false;
  }

  while (true) {
    bool hasMore = ufmtval_nextPosition(formattedValue, fpos, &status);
    if (U_FAILURE(status)) {
      intl::ReportInternalError(cx);
      return false;
    }
    if (!hasMore) {
      break;
    }

    int32_t beginIndexInt, endIndexInt;
    ucfpos_getIndexes(fpos, &beginIndexInt, &endIndexInt, &status);
    if (U_FAILURE(status)) {
      intl::ReportInternalError(cx);
      return false;
    }

    MOZ_ASSERT(beginIndexInt >= 0);
    MOZ_ASSERT(endIndexInt >= 0);
    MOZ_ASSERT(beginIndexInt <= endIndexInt,
               "field iterator returning invalid range");

    size_t beginIndex = size_t(beginIndexInt);
    size_t endIndex = size_t(endIndexInt);

    // Indices are guaranteed to be returned in order (from left to right).
    MOZ_ASSERT(lastEndIndex <= beginIndex,
               "field iteration didn't return fields in order start to "
               "finish as expected");

    if (lastEndIndex < beginIndex) {
      if (!AppendPart(&JSAtomState::literal, lastEndIndex, beginIndex)) {
        return false;
      }
    }

    if (!AppendPart(&JSAtomState::element, beginIndex, endIndex)) {
      return false;
    }
  }

  // Append any final literal.
  if (lastEndIndex < overallResult->length()) {
    if (!AppendPart(&JSAtomState::literal, lastEndIndex,
                    overallResult->length())) {
      return false;
    }
  }

  result.setObject(*partsArray);
  return true;
}

bool js::intl_FormatList(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);

  Rooted<ListFormatObject*> listFormat(
      cx, &args[0].toObject().as<ListFormatObject>());

  bool formatToParts = args[2].toBoolean();

  // Obtain a cached UListFormatter object.
  UListFormatter* lf = listFormat->getListFormatter();
  if (!lf) {
    lf = NewUListFormatter(cx, listFormat);
    if (!lf) {
      return false;
    }
    listFormat->setListFormatter(lf);

    intl::AddICUCellMemory(listFormat, ListFormatObject::EstimatedMemoryUse);
  }

  // Collect all strings and their lengths.
  ListFormatStringVector strings(cx);
  ListFormatStringLengthVector stringLengths(cx);

  // Keep a conservative running count of overall length.
  CheckedInt<int32_t> stringLengthTotal(0);

  RootedArrayObject list(cx, &args[1].toObject().as<ArrayObject>());
  RootedValue value(cx);
  uint32_t listLen = list->length();
  for (uint32_t i = 0; i < listLen; i++) {
    if (!GetElement(cx, list, list, i, &value)) {
      return false;
    }

    JSLinearString* linear = value.toString()->ensureLinear(cx);
    if (!linear) {
      return false;
    }

    size_t linearLength = linear->length();
    if (!stringLengths.append(linearLength)) {
      return false;
    }
    stringLengthTotal += linearLength;

    UniqueTwoByteChars chars = cx->make_pod_array<char16_t>(linearLength);
    if (!chars) {
      return false;
    }
    CopyChars(chars.get(), *linear);

    if (!strings.append(std::move(chars))) {
      return false;
    }
  }

  // Add space for N unrealistically large conjunctions.
  constexpr int32_t MaxConjunctionLen = 100;
  stringLengthTotal += CheckedInt<int32_t>(listLen) * MaxConjunctionLen;

  // If the overestimate exceeds ICU length limits, don't try to format.
  if (!stringLengthTotal.isValid()) {
    ReportAllocationOverflow(cx);
    return false;
  }

  // Use the UListFormatter to actually format the strings.
  if (formatToParts) {
    return FormatListToParts(cx, lf, strings, stringLengths, args.rval());
  }
  return FormatList(cx, lf, strings, stringLengths, args.rval());
}
