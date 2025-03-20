/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/TupleType.h"

#include "mozilla/FloatingPoint.h"
#include "mozilla/HashFunctions.h"

#include "jsapi.h"

#include "builtin/TupleObject.h"
#include "gc/AllocKind.h"

#include "js/TypeDecls.h"
#include "js/Value.h"
#include "util/StringBuffer.h"
#include "vm/EqualityOperations.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/RecordTupleShared.h"
#include "vm/RecordType.h"
#include "vm/SelfHosting.h"
#include "vm/ToSource.h"

#include "vm/GeckoProfiler-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

static bool TupleConstructor(JSContext* cx, unsigned argc, Value* vp);

static const JSFunctionSpec tuple_static_methods[] = {
    JS_FN("isTuple", tuple_is_tuple, 1, 0),
    JS_SELF_HOSTED_FN("from", "TupleFrom", 1, 0), JS_FN("of", tuple_of, 0, 0),
    JS_FS_END};

static const JSFunctionSpec tuple_methods[] = {
    JS_SELF_HOSTED_FN("toSorted", "TupleToSorted", 1, 0),
    JS_SELF_HOSTED_FN("toSpliced", "TupleToSpliced", 2, 0),
    JS_SELF_HOSTED_FN("concat", "TupleConcat", 0, 0),
    JS_SELF_HOSTED_FN("includes", "TupleIncludes", 1, 0),
    JS_SELF_HOSTED_FN("indexOf", "TupleIndexOf", 1, 0),
    JS_SELF_HOSTED_FN("join", "TupleJoin", 1, 0),
    JS_SELF_HOSTED_FN("lastIndexOf", "TupleLastIndexOf", 1, 0),
    JS_SELF_HOSTED_FN("toLocaleString", "TupleToLocaleString", 2, 0),
    JS_SELF_HOSTED_FN("toString", "TupleToString", 0, 0),
    JS_SELF_HOSTED_FN("entries", "TupleEntries", 0, 0),
    JS_SELF_HOSTED_FN("every", "TupleEvery", 1, 0),
    JS_SELF_HOSTED_FN("filter", "TupleFilter", 1, 0),
    JS_SELF_HOSTED_FN("find", "TupleFind", 1, 0),
    JS_SELF_HOSTED_FN("findIndex", "TupleFindIndex", 1, 0),
    JS_SELF_HOSTED_FN("forEach", "TupleForEach", 1, 0),
    JS_SELF_HOSTED_FN("keys", "TupleKeys", 0, 0),
    JS_SELF_HOSTED_FN("map", "TupleMap", 1, 0),
    JS_SELF_HOSTED_FN("reduce", "TupleReduce", 1, 0),
    JS_SELF_HOSTED_FN("reduceRight", "TupleReduceRight", 1, 0),
    JS_SELF_HOSTED_FN("some", "TupleSome", 1, 0),
    JS_SELF_HOSTED_FN("values", "$TupleValues", 0, 0),
    JS_SELF_HOSTED_SYM_FN(iterator, "$TupleValues", 0, 0),
    JS_SELF_HOSTED_FN("flat", "TupleFlat", 0, 0),
    JS_SELF_HOSTED_FN("flatMap", "TupleFlatMap", 1, 0),
    JS_SELF_HOSTED_FN("toReversed", "TupleToReversed", 0, 0),
    JS_FN("with", tuple_with, 2, 0),
    JS_FN("slice", tuple_slice, 2, 0),
    JS_FN("valueOf", tuple_value_of, 0, 0),
    JS_FS_END};

Shape* TupleType::getInitialShape(JSContext* cx) {
  return SharedShape::getInitialShape(cx, &TupleType::class_, cx->realm(),
                                      TaggedProto(nullptr), 0);
  // tuples don't have slots, but only integer-indexed elements.
}

// Prototype methods

// Proposal
// Tuple.prototype.with()
bool js::tuple_with(JSContext* cx, unsigned argc, Value* vp) {
  AutoGeckoProfilerEntry pseudoFrame(
      cx, "Tuple.prototype.with", JS::ProfilingCategoryPair::JS,
      uint32_t(ProfilingStackFrame::Flags::RELEVANT_FOR_JS));

  CallArgs args = CallArgsFromVp(argc, vp);

  /* Step 1. */
  RootedValue v(cx, args.thisv());

  mozilla::Maybe<TupleType&> maybeTuple = js::ThisTupleValue(cx, v);
  if (!maybeTuple) {
    return false;
  }

  Rooted<TupleType*> tuple(cx, &(*maybeTuple));

  /* Step 2. */
  uint64_t length = tuple->getDenseInitializedLength();
  TupleType* list = TupleType::createUninitialized(cx, length);
  if (!list) {
    return false;
  }

  /* Step 4 */
  uint64_t index;
  if (!ToIndex(cx, args.get(0), JSMSG_BAD_TUPLE_INDEX, &index)) {
    return false;
  }
  /* Step 5 */
  if (index >= length) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BAD_TUPLE_INDEX, "Tuple.with");
    return false;
  }
  /* Step 6 */
  RootedValue value(cx, args.get(1));
  if (value.isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_RECORD_TUPLE_NO_OBJECT, "Tuple.with");
    return false;
  }
  /* Step 7 */
  uint64_t before = index;
  uint64_t after = length - index - 1;
  list->copyDenseElements(0, tuple->getDenseElements(), before);
  list->setDenseInitializedLength(index + 1);
  list->initDenseElement(index, value);
  list->copyDenseElements(
      index + 1, tuple->getDenseElements() + uint32_t(index + 1), after);
  list->setDenseInitializedLength(length);
  list->finishInitialization(cx);
  /* Step 8 */
  args.rval().setExtendedPrimitive(*list);
  return true;
}

// Proposal
// Tuple.prototype.slice()
bool js::tuple_slice(JSContext* cx, unsigned argc, Value* vp) {
  AutoGeckoProfilerEntry pseudoFrame(
      cx, "Tuple.prototype.slice", JS::ProfilingCategoryPair::JS,
      uint32_t(ProfilingStackFrame::Flags::RELEVANT_FOR_JS));

  CallArgs args = CallArgsFromVp(argc, vp);
  RootedValue v(cx, args.thisv());

  /* Steps 1-2. */
  mozilla::Maybe<TupleType&> maybeList = js::ThisTupleValue(cx, v);
  if (!maybeList) {
    return false;
  }

  Rooted<TupleType*> list(cx, &(*maybeList));
  /* Step 3. */
  uint32_t len = list->getDenseInitializedLength();

  /* Step 4. */
  double relativeStart;
  if (!ToInteger(cx, args.get(0), &relativeStart)) {
    return false;
  }

  /* Step 5. */
  uint32_t k;
  if (relativeStart < 0.0) {
    k = std::max(len + relativeStart, 0.0);
  } else {
    k = std::min(relativeStart, double(len));
  }

  /* Step 6. */
  double relativeEnd;
  if (argc > 1 && !args.get(1).isUndefined()) {
    if (!ToInteger(cx, args.get(1), &relativeEnd)) {
      return false;
    }
  } else {
    relativeEnd = len;
  }

  /* Step 7. */
  uint32_t finalIndex;
  if (relativeEnd < 0.0) {
    finalIndex = std::max(len + relativeEnd, 0.0);
  } else {
    finalIndex = std::min(relativeEnd, double(len));
  }

  /* Step 8. */

  uint32_t newLen = finalIndex >= k ? finalIndex - k : 0;
  TupleType* newList = TupleType::createUninitialized(cx, newLen);
  if (!newList) {
    return false;
  }

  /* Step 9. */
  HeapSlotArray oldElements = list->getDenseElements();
  newList->copyDenseElements(0, oldElements + k, newLen);
  newList->setDenseInitializedLength(newLen);
  newList->finishInitialization(cx);
  /* Step 10. */
  args.rval().setExtendedPrimitive(*newList);
  return true;
}

// Proposal
// Tuple.prototype.valueOf()
bool js::tuple_value_of(JSContext* cx, unsigned argc, Value* vp) {
  AutoGeckoProfilerEntry pseudoFrame(
      cx, "Tuple.prototype.valueOf", JS::ProfilingCategoryPair::JS,
      uint32_t(ProfilingStackFrame::Flags::RELEVANT_FOR_JS));

  CallArgs args = CallArgsFromVp(argc, vp);

  /* Step 1. */
  HandleValue thisv = args.thisv();
  mozilla::Maybe<TupleType&> tuple = js::ThisTupleValue(cx, thisv);
  if (!tuple) {
    return false;
  }

  args.rval().setExtendedPrimitive(*tuple);
  return true;
}

bool TupleType::copy(JSContext* cx, Handle<TupleType*> in,
                     MutableHandle<TupleType*> out) {
  out.set(TupleType::createUninitialized(cx, in->length()));
  if (!out) {
    return false;
  }
  RootedValue v(cx), vCopy(cx);
  for (uint32_t i = 0; i < in->length(); i++) {
    // Let v = in[i]
    v.set(in->getDenseElement(i));

    // Copy v
    if (!CopyRecordTupleElement(cx, v, &vCopy)) {
      return false;
    }

    // Set result[i] to v
    if (!out->initializeNextElement(cx, vCopy)) {
      return false;
    }
  }
  out->finishInitialization(cx);
  return true;
}

TupleType* TupleType::create(JSContext* cx, uint32_t length,
                             const Value* elements) {
  for (uint32_t index = 0; index < length; index++) {
    if (!elements[index].isPrimitive()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_RECORD_TUPLE_NO_OBJECT);
      return nullptr;
    }
  }

  TupleType* tup = TupleType::createUninitialized(cx, length);
  if (!tup) {
    return nullptr;
  }

  tup->initDenseElements(elements, length);
  tup->finishInitialization(cx);

  return tup;
}

static TupleType* allocate(JSContext* cx, gc::AllocKind allocKind) {
  Rooted<Shape*> shape(cx, TupleType::getInitialShape(cx));
  if (!shape) {
    return nullptr;
  }

  TupleType* tup =
      cx->newCell<TupleType>(allocKind, gc::Heap::Default, &TupleType::class_);
  if (!tup) {
    return nullptr;
  }

  tup->initShape(shape);
  tup->initEmptyDynamicSlots();
  tup->initFixedElements(allocKind, 0);
  return tup;
}

TupleType* TupleType::createUninitialized(JSContext* cx, uint32_t length) {
  gc::AllocKind allocKind = GuessArrayGCKind(length);

  TupleType* tup = allocate(cx, allocKind);
  if (!tup) {
    return nullptr;
  }

  if (!tup->ensureElements(cx, length)) {
    return nullptr;
  }

  return tup;
}

bool TupleType::initializeNextElement(JSContext* cx, HandleValue elt) {
  if (!elt.isPrimitive()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_RECORD_TUPLE_NO_OBJECT);
    return false;
  }

  uint32_t length = getDenseInitializedLength();

  if (!ensureElements(cx, length + 1)) {
    return false;
  }
  setDenseInitializedLength(length + 1);
  initDenseElement(length, elt);

  return true;
}

void TupleType::finishInitialization(JSContext* cx) {
  shrinkCapacityToInitializedLength(cx);

  ObjectElements* header = getElementsHeader();
  header->length = header->initializedLength;
  header->setNotExtensible();
  header->seal();
  header->freeze();
}

bool TupleType::getOwnProperty(HandleId id, MutableHandleValue vp) const {
  if (!id.isInt()) {
    return false;
  }

  int32_t index = id.toInt();
  if (index < 0 || uint32_t(index) >= length()) {
    return false;
  }

  vp.set(getDenseElement(index));
  return true;
}

js::HashNumber TupleType::hash(const TupleType::ElementHasher& hasher) const {
  MOZ_ASSERT(isAtomized());

  js::HashNumber h = mozilla::HashGeneric(length());
  for (uint32_t i = 0; i < length(); i++) {
    h = mozilla::AddToHash(h, hasher(getDenseElement(i)));
  }
  return h;
}

bool TupleType::ensureAtomized(JSContext* cx) {
  if (isAtomized()) {
    return true;
  }

  RootedValue child(cx);
  bool changed;

  for (uint32_t i = 0; i < length(); i++) {
    child.set(getDenseElement(i));
    if (!EnsureAtomized(cx, &child, &changed)) {
      return false;
    }
    if (changed) {
      // We cannot use setDenseElement(), because this object is frozen.
      elements_[i].set(this, HeapSlot::Element, unshiftedIndex(i), child);
    }
  }

  getElementsHeader()->setTupleIsAtomized();

  return true;
}

bool TupleType::sameValueZero(JSContext* cx, TupleType* lhs, TupleType* rhs,
                              bool* equal) {
  return sameValueWith<SameValueZero>(cx, lhs, rhs, equal);
}

bool TupleType::sameValue(JSContext* cx, TupleType* lhs, TupleType* rhs,
                          bool* equal) {
  return sameValueWith<SameValue>(cx, lhs, rhs, equal);
}
bool TupleType::sameValueZero(TupleType* lhs, TupleType* rhs) {
  MOZ_ASSERT(lhs->isAtomized());
  MOZ_ASSERT(rhs->isAtomized());

  if (lhs == rhs) {
    return true;
  }
  if (lhs->length() != rhs->length()) {
    return false;
  }

  Value v1, v2;

  for (uint32_t index = 0; index < lhs->length(); index++) {
    v1 = lhs->getDenseElement(index);
    v2 = rhs->getDenseElement(index);

    if (!js::SameValueZeroLinear(v1, v2)) {
      return false;
    }
  }

  return true;
}

template <bool Comparator(JSContext*, HandleValue, HandleValue, bool*)>
bool TupleType::sameValueWith(JSContext* cx, TupleType* lhs, TupleType* rhs,
                              bool* equal) {
  MOZ_ASSERT(lhs->getElementsHeader()->isFrozen());
  MOZ_ASSERT(rhs->getElementsHeader()->isFrozen());

  if (lhs == rhs) {
    *equal = true;
    return true;
  }

  if (lhs->length() != rhs->length()) {
    *equal = false;
    return true;
  }

  *equal = true;

  RootedValue v1(cx);
  RootedValue v2(cx);

  for (uint32_t index = 0; index < lhs->length(); index++) {
    v1.set(lhs->getDenseElement(index));
    v2.set(rhs->getDenseElement(index));

    if (!Comparator(cx, v1, v2, equal)) {
      return false;
    }

    if (!*equal) {
      return true;
    }
  }

  return true;
}

JSString* js::TupleToSource(JSContext* cx, Handle<TupleType*> tup) {
  JSStringBuilder sb(cx);

  if (!sb.append("#[")) {
    return nullptr;
  }

  uint32_t length = tup->length();

  RootedValue elt(cx);
  for (uint32_t index = 0; index < length; index++) {
    elt.set(tup->getDenseElement(index));

    /* Get element's character string. */
    JSString* str = ValueToSource(cx, elt);
    if (!str) {
      return nullptr;
    }

    /* Append element to buffer. */
    if (!sb.append(str)) {
      return nullptr;
    }
    if (index + 1 != length) {
      if (!sb.append(", ")) {
        return nullptr;
      }
    }
  }

  /* Finalize the buffer. */
  if (!sb.append(']')) {
    return nullptr;
  }

  return sb.finishString();
}

// Record and Tuple proposal section 9.2.1
bool TupleConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (args.isConstructing()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_CONSTRUCTOR, "Tuple");
    return false;
  }

  TupleType* tup = TupleType::create(cx, args.length(), args.array());
  if (!tup) {
    return false;
  }

  args.rval().setExtendedPrimitive(*tup);
  return true;
}

/*===========================================================================*\
                       BEGIN: Tuple.prototype methods
\*===========================================================================*/

static bool ArrayToTuple(JSContext* cx, const CallArgs& args) {
  Rooted<ArrayObject*> aObj(cx, &args.rval().toObject().as<ArrayObject>());
  TupleType* tup = TupleType::createUnchecked(cx, aObj);

  if (!tup) {
    return false;
  }

  args.rval().setExtendedPrimitive(*tup);
  return true;
}

// Takes an array as a single argument and returns a tuple of the
// array elements. This method copies the array, because the callee
// may still hold a pointer to it and it would break garbage collection
// to change the type of the object from ArrayObject to TupleType (which
// is the only way to re-use the same object if it has fixed elements.)
// Should only be called from self-hosted tuple methods;
// assumes all elements are non-objects and the array is packed
bool js::tuple_construct(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  MOZ_ASSERT(args[0].toObject().is<ArrayObject>());

  args.rval().set(args[0]);
  return ArrayToTuple(cx, args);
}

bool js::tuple_is_tuple(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return IsTupleUnchecked(cx, args);
}

TupleType* TupleType::createUnchecked(JSContext* cx,
                                      Handle<ArrayObject*> aObj) {
  size_t len = aObj->getDenseInitializedLength();
  MOZ_ASSERT(aObj->getElementsHeader()->numShiftedElements() == 0);
  TupleType* tup = createUninitialized(cx, len);
  if (!tup) {
    return nullptr;
  }
  tup->initDenseElements(aObj, 0, len);
  tup->finishInitialization(cx);
  return tup;
}

bool js::tuple_of(JSContext* cx, unsigned argc, Value* vp) {
  /* Step 1 */
  CallArgs args = CallArgsFromVp(argc, vp);
  size_t len = args.length();
  Value* items = args.array();

  /* Step 2 */
  for (size_t i = 0; i < len; i++) {
    if (items[i].isObject()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_RECORD_TUPLE_NO_OBJECT, "Tuple.of");
      return false;
    }
  }
  /* Step 3 */
  ArrayObject* result = js::NewDenseCopiedArray(cx, len, items, GenericObject);
  if (!result) {
    return false;
  }
  args.rval().setObject(*result);
  /* Step 4 */
  return ArrayToTuple(cx, args);
}

bool js::IsTuple(const Value& v) {
  if (v.isExtendedPrimitive()) return v.toExtendedPrimitive().is<TupleType>();
  if (v.isObject()) return v.toObject().is<TupleObject>();
  return false;
}

// Caller is responsible for rooting the result
TupleType& TupleType::thisTupleValue(const Value& val) {
  MOZ_ASSERT(IsTuple(val));
  return (val.isExtendedPrimitive() ? val.toExtendedPrimitive().as<TupleType>()
                                    : val.toObject().as<TupleObject>().unbox());
}

bool HandleIsTuple(HandleValue v) { return IsTuple(v.get()); }

// 8.2.3.2 get Tuple.prototype.length
bool lengthAccessor_impl(JSContext* cx, const CallArgs& args) {
  // Step 1.
  TupleType& tuple = TupleType::thisTupleValue(args.thisv().get());
  // Step 2.
  args.rval().setInt32(tuple.length());
  return true;
}

bool TupleType::lengthAccessor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<HandleIsTuple, lengthAccessor_impl>(cx, args);
}

/*===========================================================================*\
                         END: Tuple.prototype methods
\*===========================================================================*/

const JSClass TupleType::class_ = {"tuple", 0, JS_NULL_CLASS_OPS,
                                   &TupleType::classSpec_};

const JSClass TupleType::protoClass_ = {
    "Tuple.prototype", JSCLASS_HAS_CACHED_PROTO(JSProto_Tuple),
    JS_NULL_CLASS_OPS, &TupleType::classSpec_};

/* static */ const JSPropertySpec properties_[] = {
    JS_STRING_SYM_PS(toStringTag, "Tuple", JSPROP_READONLY),
    JS_PSG("length", TupleType::lengthAccessor, 0), JS_PS_END};

const ClassSpec TupleType::classSpec_ = {
    GenericCreateConstructor<TupleConstructor, 0, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<TupleType>,
    tuple_static_methods,
    nullptr,
    tuple_methods,
    properties_,
    nullptr};
