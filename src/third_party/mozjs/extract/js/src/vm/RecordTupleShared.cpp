/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_RecordTupleShared_h
#define vm_RecordTupleShared_h

#include "vm/RecordTupleShared.h"

#include "NamespaceImports.h"
#include "builtin/RecordObject.h"
#include "builtin/TupleObject.h"
#include "js/Value.h"
#include "vm/BigIntType.h"
#include "vm/Compartment.h"
#include "vm/GlobalObject.h"
#include "vm/JSObject.h"
#include "vm/NativeObject.h"

#include "gc/Marking-inl.h"

namespace js {

bool IsExtendedPrimitive(const JSObject& obj) {
  return obj.is<RecordType>() || obj.is<TupleType>();
}

template <typename T>
JSObject* CopyExtendedPrimitiveHelper(JSContext* cx, HandleObject extPrim) {
  MOZ_ASSERT(gc::MaybeForwardedObjectIs<T>(extPrim));
  Rooted<T*> in(cx, &extPrim->template as<T>());
  Rooted<T*> out(cx);
  if (!T::copy(cx, in, &out)) {
    return nullptr;
  }
  return out;
}

JSObject* CopyExtendedPrimitive(JSContext* cx, HandleObject extPrim) {
  if (gc::MaybeForwardedObjectIs<RecordType>(extPrim)) {
    return CopyExtendedPrimitiveHelper<RecordType>(cx, extPrim);
  }
  MOZ_ASSERT(gc::MaybeForwardedObjectIs<TupleType>(extPrim));
  return CopyExtendedPrimitiveHelper<TupleType>(cx, extPrim);
}

// Returns false if v is not a valid record/tuple element (e.g. it's an Object
// or Symbol
bool CopyRecordTupleElement(JSContext* cx, HandleValue v,
                            MutableHandleValue out) {
  switch (v.type()) {
    case JS::ValueType::Double:
    case JS::ValueType::Int32:
    case JS::ValueType::Undefined:
    case JS::ValueType::Boolean: {
      out.set(v);
      break;
    }
    case JS::ValueType::String: {
      RootedString vStr(cx, v.toString());
      JSString* copy = CopyStringPure(cx, vStr);
      if (!copy) {
        return false;
      }
      out.setString(copy);
      break;
    }
    case JS::ValueType::BigInt: {
      RootedBigInt bi(cx, v.toBigInt());
      BigInt* copy = BigInt::copy(cx, bi);
      if (!copy) {
        return false;
      }
      out.setBigInt(copy);
      break;
    }
    case JS::ValueType::ExtendedPrimitive: {
      RootedObject extPrim(cx, &v.toExtendedPrimitive());
      JSObject* copy = CopyExtendedPrimitive(cx, extPrim);
      if (!copy) {
        return false;
      }
      out.setExtendedPrimitive(*copy);
      break;
    }
    default:
      MOZ_CRASH("CopyRecordTupleElement(): unexpected element type");
  }
  return true;
}

bool gc::MaybeForwardedIsExtendedPrimitive(const JSObject& obj) {
  return MaybeForwardedObjectIs<RecordType>(&obj) ||
         MaybeForwardedObjectIs<TupleType>(&obj);
}

bool IsExtendedPrimitiveWrapper(const JSObject& obj) {
  return obj.is<RecordObject>() || obj.is<TupleObject>();
}

bool ExtendedPrimitiveGetProperty(JSContext* cx, HandleObject obj,
                                  HandleValue receiver, HandleId id,
                                  MutableHandleValue vp) {
  MOZ_ASSERT(IsExtendedPrimitive(*obj));

  if (obj->is<RecordType>()) {
    if (obj->as<RecordType>().getOwnProperty(cx, id, vp)) {
      return true;
    }
    // If records will not have a null prototype, this should use a mehanism
    // similar to tuples.
    vp.set(JS::UndefinedValue());
    return true;
  }

  MOZ_ASSERT(obj->is<TupleType>());
  if (obj->as<TupleType>().getOwnProperty(id, vp)) {
    return true;
  }

  JSObject* proto = GlobalObject::getOrCreateTuplePrototype(cx, cx->global());
  if (!proto) {
    return false;
  }

  Rooted<NativeObject*> rootedProto(cx, &proto->as<NativeObject>());
  return NativeGetProperty(cx, rootedProto, receiver, id, vp);
}

}  // namespace js

#endif
