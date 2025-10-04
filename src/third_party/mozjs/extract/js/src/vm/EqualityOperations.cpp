/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/EqualityOperations.h"  // js::LooselyEqual, js::StrictlyEqual, js::SameValue

#include "mozilla/Assertions.h"  // MOZ_ASSERT, MOZ_ASSERT_IF

#include "jsnum.h"    // js::StringToNumber
#include "jstypes.h"  // JS_PUBLIC_API

#include "js/Context.h"   // js::AssertHeapIsIdle
#include "js/Equality.h"  // JS::LooselyEqual, JS::StrictlyEqual, JS::SameValue
#include "js/Result.h"    // JS_TRY_VAR_OR_RETURN_FALSE
#include "js/RootingAPI.h"  // JS::Rooted
#include "js/Value.h"       // JS::Int32Value, JS::SameType, JS::Value
#include "vm/BigIntType.h"  // JS::BigInt
#include "vm/JSContext.h"   // CHECK_THREAD
#include "vm/JSObject.h"    // js::ToPrimitive
#include "vm/StringType.h"  // js::EqualStrings
#ifdef ENABLE_RECORD_TUPLE
#  include "vm/RecordType.h"
#  include "vm/TupleType.h"
#endif

#include "builtin/Boolean-inl.h"  // js::EmulatesUndefined
#include "vm/JSContext-inl.h"     // JSContext::check

static bool EqualGivenSameType(JSContext* cx, JS::Handle<JS::Value> lval,
                               JS::Handle<JS::Value> rval, bool* equal) {
  MOZ_ASSERT(JS::SameType(lval, rval));

  if (lval.isString()) {
    return js::EqualStrings(cx, lval.toString(), rval.toString(), equal);
  }

  if (lval.isDouble()) {
    *equal = (lval.toDouble() == rval.toDouble());
    return true;
  }

  if (lval.isBigInt()) {
    *equal = JS::BigInt::equal(lval.toBigInt(), rval.toBigInt());
    return true;
  }

#ifdef ENABLE_RECORD_TUPLE
  // Record & Tuple proposal, section 3.2.6 (Strict Equality Comparison), step 3
  //  - https://tc39.es/proposal-record-tuple/#sec-strict-equality-comparison
  //
  // When computing equality, records and tuples are compared using the
  // SameValueZero algorithm.
  //
  // NOTE: Since Records and Tuples are impemented as ExtendedPrimitives,
  // "SameType" refers to the fact that both lval and rval are
  // ExtendedPrimitives. They can still be different types (for example, a
  // Record and a Tuple).
  if (lval.isExtendedPrimitive()) {
    JSObject* lobj = &lval.toExtendedPrimitive();
    JSObject* robj = &rval.toExtendedPrimitive();

    if (lobj->getClass() != robj->getClass()) {
      *equal = false;
      return true;
    }

    if (lobj->is<js::RecordType>()) {
      return js::RecordType::sameValueZero(cx, &lobj->as<js::RecordType>(),
                                           &robj->as<js::RecordType>(), equal);
    }
    if (lobj->is<js::TupleType>()) {
      return js::TupleType::sameValueZero(cx, &lobj->as<js::TupleType>(),
                                          &robj->as<js::TupleType>(), equal);
    }
    MOZ_CRASH("Unknown ExtendedPrimitive type");
  }
#endif

  // Note: we can do a bitwise comparison even for Int32Value because both
  // Values have the same type.
  MOZ_ASSERT(CanUseBitwiseCompareForStrictlyEqual(lval) || lval.isInt32());

  *equal = (lval.asRawBits() == rval.asRawBits());
  MOZ_ASSERT_IF(lval.isUndefined() || lval.isNull(), *equal);
  return true;
}

static bool LooselyEqualBooleanAndOther(JSContext* cx,
                                        JS::Handle<JS::Value> lval,
                                        JS::Handle<JS::Value> rval,
                                        bool* result) {
  MOZ_ASSERT(!rval.isBoolean());

  JS::Rooted<JS::Value> lvalue(cx, JS::Int32Value(lval.toBoolean() ? 1 : 0));

  // The tail-call would end up in Step 3.
  if (rval.isNumber()) {
    *result = (lvalue.toNumber() == rval.toNumber());
    return true;
  }
  // The tail-call would end up in Step 6.
  if (rval.isString()) {
    double num;
    if (!StringToNumber(cx, rval.toString(), &num)) {
      return false;
    }
    *result = (lvalue.toNumber() == num);
    return true;
  }

  return js::LooselyEqual(cx, lvalue, rval, result);
}

// ES6 draft rev32 7.2.12 Abstract Equality Comparison
bool js::LooselyEqual(JSContext* cx, JS::Handle<JS::Value> lval,
                      JS::Handle<JS::Value> rval, bool* result) {
  // Step 3.
  if (JS::SameType(lval, rval)) {
    return EqualGivenSameType(cx, lval, rval, result);
  }

  // Handle int32 x double.
  if (lval.isNumber() && rval.isNumber()) {
    *result = (lval.toNumber() == rval.toNumber());
    return true;
  }

  // Step 4. This a bit more complex, because of the undefined emulating object.
  if (lval.isNullOrUndefined()) {
    // We can return early here, because null | undefined is only equal to the
    // same set.
    *result = rval.isNullOrUndefined() ||
              (rval.isObject() && EmulatesUndefined(&rval.toObject()));
    return true;
  }

  // Step 5.
  if (rval.isNullOrUndefined()) {
    MOZ_ASSERT(!lval.isNullOrUndefined());
    *result = lval.isObject() && EmulatesUndefined(&lval.toObject());
    return true;
  }

  // Step 6.
  if (lval.isNumber() && rval.isString()) {
    double num;
    if (!StringToNumber(cx, rval.toString(), &num)) {
      return false;
    }
    *result = (lval.toNumber() == num);
    return true;
  }

  // Step 7.
  if (lval.isString() && rval.isNumber()) {
    double num;
    if (!StringToNumber(cx, lval.toString(), &num)) {
      return false;
    }
    *result = (num == rval.toNumber());
    return true;
  }

  // Step 8.
  if (lval.isBoolean()) {
    return LooselyEqualBooleanAndOther(cx, lval, rval, result);
  }

  // Step 9.
  if (rval.isBoolean()) {
    return LooselyEqualBooleanAndOther(cx, rval, lval, result);
  }

  // Step 10.
  if ((lval.isString() || lval.isNumber() || lval.isSymbol()) &&
      rval.isObject()) {
    JS::Rooted<JS::Value> rvalue(cx, rval);
    if (!ToPrimitive(cx, &rvalue)) {
      return false;
    }
    return js::LooselyEqual(cx, lval, rvalue, result);
  }

  // Step 11.
  if (lval.isObject() &&
      (rval.isString() || rval.isNumber() || rval.isSymbol())) {
    JS::Rooted<JS::Value> lvalue(cx, lval);
    if (!ToPrimitive(cx, &lvalue)) {
      return false;
    }
    return js::LooselyEqual(cx, lvalue, rval, result);
  }

  if (lval.isBigInt()) {
    JS::Rooted<JS::BigInt*> lbi(cx, lval.toBigInt());
    bool tmpResult;
    JS_TRY_VAR_OR_RETURN_FALSE(cx, tmpResult,
                               JS::BigInt::looselyEqual(cx, lbi, rval));
    *result = tmpResult;
    return true;
  }

  if (rval.isBigInt()) {
    JS::Rooted<JS::BigInt*> rbi(cx, rval.toBigInt());
    bool tmpResult;
    JS_TRY_VAR_OR_RETURN_FALSE(cx, tmpResult,
                               JS::BigInt::looselyEqual(cx, rbi, lval));
    *result = tmpResult;
    return true;
  }

  // Step 12.
  *result = false;
  return true;
}

JS_PUBLIC_API bool JS::LooselyEqual(JSContext* cx, Handle<Value> value1,
                                    Handle<Value> value2, bool* equal) {
  js::AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(value1, value2);
  MOZ_ASSERT(equal);
  return js::LooselyEqual(cx, value1, value2, equal);
}

bool js::StrictlyEqual(JSContext* cx, JS::Handle<JS::Value> lval,
                       JS::Handle<JS::Value> rval, bool* equal) {
  if (SameType(lval, rval)) {
    return EqualGivenSameType(cx, lval, rval, equal);
  }

  if (lval.isNumber() && rval.isNumber()) {
    *equal = (lval.toNumber() == rval.toNumber());
    return true;
  }

  *equal = false;
  return true;
}

JS_PUBLIC_API bool JS::StrictlyEqual(JSContext* cx, Handle<Value> value1,
                                     Handle<Value> value2, bool* equal) {
  js::AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(value1, value2);
  MOZ_ASSERT(equal);
  return js::StrictlyEqual(cx, value1, value2, equal);
}

static inline bool IsNegativeZero(const JS::Value& v) {
  return v.isDouble() && mozilla::IsNegativeZero(v.toDouble());
}

static inline bool IsNaN(const JS::Value& v) {
  return v.isDouble() && std::isnan(v.toDouble());
}

bool js::SameValue(JSContext* cx, JS::Handle<JS::Value> v1,
                   JS::Handle<JS::Value> v2, bool* same) {
  if (IsNegativeZero(v1)) {
    *same = IsNegativeZero(v2);
    return true;
  }

  if (IsNegativeZero(v2)) {
    *same = false;
    return true;
  }

#ifdef ENABLE_RECORD_TUPLE
  if (v1.isExtendedPrimitive()) {
    JSObject* lobj = &v1.toExtendedPrimitive();
    JSObject* robj = &v2.toExtendedPrimitive();

    if (lobj->getClass() != robj->getClass()) {
      *same = false;
      return true;
    }

    if (lobj->is<js::RecordType>()) {
      return js::RecordType::sameValue(cx, &lobj->as<js::RecordType>(),
                                       &robj->as<js::RecordType>(), same);
    }
    if (lobj->is<js::TupleType>()) {
      return js::TupleType::sameValue(cx, &lobj->as<js::TupleType>(),
                                      &robj->as<js::TupleType>(), same);
    }
    MOZ_CRASH("Unknown ExtendedPrimitive type");
  }
#endif

  return js::SameValueZero(cx, v1, v2, same);
}

#ifdef ENABLE_RECORD_TUPLE
bool js::SameValueZeroLinear(const JS::Value& lval, const JS::Value& rval) {
  if (lval.isNumber() && rval.isNumber()) {
    return IsNaN(lval) ? IsNaN(rval) : lval.toNumber() == rval.toNumber();
  }

  if (lval.type() != rval.type()) {
    return false;
  }

  switch (lval.type()) {
    case ValueType::Double:
      return IsNaN(lval) ? IsNaN(rval) : lval.toDouble() == rval.toDouble();

    case ValueType::BigInt:
      // BigInt values are considered equal if they represent the same
      // mathematical value.
      return BigInt::equal(lval.toBigInt(), rval.toBigInt());

    case ValueType::String:
      MOZ_ASSERT(lval.toString()->isLinear() && rval.toString()->isLinear());
      return EqualStrings(&lval.toString()->asLinear(),
                          &rval.toString()->asLinear());

    case ValueType::ExtendedPrimitive: {
      JSObject& lobj = lval.toExtendedPrimitive();
      JSObject& robj = rval.toExtendedPrimitive();
      if (lobj.getClass() != robj.getClass()) {
        return false;
      }
      if (lobj.is<RecordType>()) {
        return RecordType::sameValueZero(&lobj.as<RecordType>(),
                                         &robj.as<RecordType>());
      }
      MOZ_ASSERT(lobj.is<TupleType>());
      return TupleType::sameValueZero(&lobj.as<TupleType>(),
                                      &robj.as<TupleType>());
    }

    default:
      MOZ_ASSERT(CanUseBitwiseCompareForStrictlyEqual(lval));
      return lval.asRawBits() == rval.asRawBits();
  }
}
#endif

JS_PUBLIC_API bool JS::SameValue(JSContext* cx, Handle<Value> value1,
                                 Handle<Value> value2, bool* same) {
  js::AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(value1, value2);
  MOZ_ASSERT(same);
  return js::SameValue(cx, value1, value2, same);
}

bool js::SameValueZero(JSContext* cx, Handle<Value> v1, Handle<Value> v2,
                       bool* same) {
  if (IsNaN(v1) && IsNaN(v2)) {
    *same = true;
    return true;
  }

  return js::StrictlyEqual(cx, v1, v2, same);
}
