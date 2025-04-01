/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/TupleObject.h"

#include "mozilla/Assertions.h"
#include "mozilla/Maybe.h"  // mozilla::Maybe

#include "jsapi.h"

#include "vm/NativeObject.h"
#include "vm/ObjectOperations.h"
#include "vm/TupleType.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;
using mozilla::Maybe;

// Record and Tuple proposal section 9.2.1

TupleObject* TupleObject::create(JSContext* cx, Handle<TupleType*> tuple) {
  TupleObject* tup = NewBuiltinClassInstance<TupleObject>(cx);
  if (!tup) {
    return nullptr;
  }
  tup->setFixedSlot(PrimitiveValueSlot, ExtendedPrimitiveValue(*tuple));
  return tup;
}

// Caller is responsible for rooting the result
TupleType& TupleObject::unbox() const {
  return getFixedSlot(PrimitiveValueSlot).toExtendedPrimitive().as<TupleType>();
}

// Caller is responsible for rooting the result
mozilla::Maybe<TupleType&> TupleObject::maybeUnbox(JSObject* obj) {
  Maybe<TupleType&> result = mozilla::Nothing();
  if (obj->is<TupleType>()) {
    result.emplace(obj->as<TupleType>());
  } else if (obj->is<TupleObject>()) {
    result.emplace(obj->as<TupleObject>().unbox());
  }
  return result;
}

bool js::IsTuple(JSObject& obj) {
  return (obj.is<TupleType>() || obj.is<TupleObject>());
}

// Caller is responsible for rooting the result
mozilla::Maybe<TupleType&> js::ThisTupleValue(JSContext* cx, HandleValue val) {
  if (!js::IsTuple(val)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BAD_TUPLE_OBJECT);
    return mozilla::Nothing();
  }
  Maybe<TupleType&> result = mozilla::Nothing();
  result.emplace(TupleType::thisTupleValue(val));
  return (result);
}

bool tup_mayResolve(const JSAtomState&, jsid id, JSObject*) {
  // tup_resolve ignores non-integer ids.
  return id.isInt();
}

bool tup_resolve(JSContext* cx, HandleObject obj, HandleId id,
                 bool* resolvedp) {
  RootedValue value(cx);
  *resolvedp = obj->as<TupleObject>().unbox().getOwnProperty(id, &value);

  if (*resolvedp) {
    static const unsigned TUPLE_ELEMENT_ATTRS =
        JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT;
    return DefineDataProperty(cx, obj, id, value,
                              TUPLE_ELEMENT_ATTRS | JSPROP_RESOLVING);
  }

  return true;
}

const JSClassOps TupleObjectClassOps = {
    nullptr,         // addProperty
    nullptr,         // delProperty
    nullptr,         // enumerate
    nullptr,         // newEnumerate
    tup_resolve,     // resolve
    tup_mayResolve,  // mayResolve
    nullptr,         // finalize
    nullptr,         // call
    nullptr,         // construct
    nullptr,         // trace
};

const JSClass TupleObject::class_ = {
    "TupleObject",
    JSCLASS_HAS_RESERVED_SLOTS(SlotCount) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_Tuple),
    &TupleObjectClassOps};
