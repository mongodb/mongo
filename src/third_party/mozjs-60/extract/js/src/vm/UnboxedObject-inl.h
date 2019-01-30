/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_UnboxedObject_inl_h
#define vm_UnboxedObject_inl_h

#include "vm/UnboxedObject.h"

#include "gc/StoreBuffer-inl.h"
#include "vm/ArrayObject-inl.h"
#include "vm/NativeObject-inl.h"

namespace js {

static inline Value
GetUnboxedValue(uint8_t* p, JSValueType type, bool maybeUninitialized)
{
    switch (type) {
      case JSVAL_TYPE_BOOLEAN:
        return BooleanValue(*p != 0);

      case JSVAL_TYPE_INT32:
        return Int32Value(*reinterpret_cast<int32_t*>(p));

      case JSVAL_TYPE_DOUBLE: {
        // During unboxed plain object creation, non-GC thing properties are
        // left uninitialized. This is normally fine, since the properties will
        // be filled in shortly, but if they are read before that happens we
        // need to make sure that doubles are canonical.
        double d = *reinterpret_cast<double*>(p);
        if (maybeUninitialized)
            return DoubleValue(JS::CanonicalizeNaN(d));
        return DoubleValue(d);
      }

      case JSVAL_TYPE_STRING:
        return StringValue(*reinterpret_cast<JSString**>(p));

      case JSVAL_TYPE_OBJECT:
        return ObjectOrNullValue(*reinterpret_cast<JSObject**>(p));

      default:
        MOZ_CRASH("Invalid type for unboxed value");
    }
}

static inline void
SetUnboxedValueNoTypeChange(JSObject* unboxedObject,
                            uint8_t* p, JSValueType type, const Value& v,
                            bool preBarrier)
{
    switch (type) {
      case JSVAL_TYPE_BOOLEAN:
        *p = v.toBoolean();
        return;

      case JSVAL_TYPE_INT32:
        *reinterpret_cast<int32_t*>(p) = v.toInt32();
        return;

      case JSVAL_TYPE_DOUBLE:
        *reinterpret_cast<double*>(p) = v.toNumber();
        return;

      case JSVAL_TYPE_STRING: {
        MOZ_ASSERT(!IsInsideNursery(v.toString()));
        JSString** np = reinterpret_cast<JSString**>(p);
        if (preBarrier)
            JSString::writeBarrierPre(*np);
        *np = v.toString();
        return;
      }

      case JSVAL_TYPE_OBJECT: {
        JSObject** np = reinterpret_cast<JSObject**>(p);

        // Manually trigger post barriers on the whole object. If we treat
        // the pointer as a HeapPtrObject we will get confused later if the
        // object is converted to its native representation.
        JSObject* obj = v.toObjectOrNull();
        if (IsInsideNursery(obj) && !IsInsideNursery(unboxedObject))
            unboxedObject->zone()->group()->storeBuffer().putWholeCell(unboxedObject);

        if (preBarrier)
            JSObject::writeBarrierPre(*np);
        *np = obj;
        return;
      }

      default:
        MOZ_CRASH("Invalid type for unboxed value");
    }
}

static inline bool
SetUnboxedValue(JSContext* cx, JSObject* unboxedObject, jsid id,
                uint8_t* p, JSValueType type, const Value& v, bool preBarrier)
{
    switch (type) {
      case JSVAL_TYPE_BOOLEAN:
        if (v.isBoolean()) {
            *p = v.toBoolean();
            return true;
        }
        return false;

      case JSVAL_TYPE_INT32:
        if (v.isInt32()) {
            *reinterpret_cast<int32_t*>(p) = v.toInt32();
            return true;
        }
        return false;

      case JSVAL_TYPE_DOUBLE:
        if (v.isNumber()) {
            *reinterpret_cast<double*>(p) = v.toNumber();
            return true;
        }
        return false;

      case JSVAL_TYPE_STRING:
        if (v.isString()) {
            JSString** np = reinterpret_cast<JSString**>(p);
            if (IsInsideNursery(v.toString()) && !IsInsideNursery(unboxedObject))
                unboxedObject->zone()->group()->storeBuffer().putWholeCell(unboxedObject);

            if (preBarrier)
                JSString::writeBarrierPre(*np);
            *np = v.toString();
            return true;
        }
        return false;

      case JSVAL_TYPE_OBJECT:
        if (v.isObjectOrNull()) {
            JSObject** np = reinterpret_cast<JSObject**>(p);

            // Update property types when writing object properties. Types for
            // other properties were captured when the unboxed layout was
            // created.
            AddTypePropertyId(cx, unboxedObject, id, v);

            // As above, trigger post barriers on the whole object.
            JSObject* obj = v.toObjectOrNull();
            if (IsInsideNursery(v.toObjectOrNull()) && !IsInsideNursery(unboxedObject))
                unboxedObject->zone()->group()->storeBuffer().putWholeCell(unboxedObject);

            if (preBarrier)
                JSObject::writeBarrierPre(*np);
            *np = obj;
            return true;
        }
        return false;

      default:
        MOZ_CRASH("Invalid type for unboxed value");
    }
}

/////////////////////////////////////////////////////////////////////
// UnboxedPlainObject
/////////////////////////////////////////////////////////////////////

inline const UnboxedLayout&
UnboxedPlainObject::layout() const
{
    return group()->unboxedLayout();
}

/////////////////////////////////////////////////////////////////////
// UnboxedLayout
/////////////////////////////////////////////////////////////////////

gc::AllocKind
js::UnboxedLayout::getAllocKind() const
{
    MOZ_ASSERT(size());
    return gc::GetGCObjectKindForBytes(UnboxedPlainObject::offsetOfData() + size());
}

} // namespace js

#endif // vm_UnboxedObject_inl_h
