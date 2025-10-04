/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_public_Object_h
#define js_public_Object_h

#include "js/shadow/Object.h"  // JS::shadow::Object

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/Class.h"       // js::ESClass, JSCLASS_RESERVED_SLOTS
#include "js/Realm.h"       // JS::GetCompartmentForRealm
#include "js/RootingAPI.h"  // JS::{,Mutable}Handle
#include "js/Value.h"       // JS::Value

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

namespace JS {

class JS_PUBLIC_API Compartment;

/**
 * Determine the ECMAScript "class" -- Date, String, RegExp, and all the other
 * builtin object types (described in ECMAScript in terms of an objecting having
 * "an [[ArrayBufferData]] internal slot" or similar language for other kinds of
 * object -- of the provided object.
 *
 * If this function is passed a wrapper that can be unwrapped, the determination
 * is performed on that object.  If the wrapper can't be unwrapped, and it's not
 * a wrapper that prefers to treat this operation as a failure, this function
 * will indicate that the object is |js::ESClass::Other|.
 */
extern JS_PUBLIC_API bool GetBuiltinClass(JSContext* cx, Handle<JSObject*> obj,
                                          js::ESClass* cls);

/** Get the |JSClass| of an object. */
inline const JSClass* GetClass(const JSObject* obj) {
  return reinterpret_cast<const shadow::Object*>(obj)->shape->base->clasp;
}

/**
 * Get the |JS::Compartment*| of an object.
 *
 * Note that the compartment of an object in this realm, that is a
 * cross-compartment wrapper around an object from another realm, is the
 * compartment of this realm.
 */
static MOZ_ALWAYS_INLINE Compartment* GetCompartment(JSObject* obj) {
  Realm* realm = reinterpret_cast<shadow::Object*>(obj)->shape->base->realm;
  return GetCompartmentForRealm(realm);
}

/**
 * Get the value stored in a reserved slot in an object.
 *
 * If |obj| is known to be a proxy and you're willing to use friend APIs,
 * |js::GetProxyReservedSlot| in "js/Proxy.h" is very slightly more efficient.
 */
inline const Value& GetReservedSlot(JSObject* obj, size_t slot) {
  MOZ_ASSERT(slot < JSCLASS_RESERVED_SLOTS(GetClass(obj)));
  return reinterpret_cast<const shadow::Object*>(obj)->slotRef(slot);
}

namespace detail {

extern JS_PUBLIC_API void SetReservedSlotWithBarrier(JSObject* obj, size_t slot,
                                                     const Value& value);

}  // namespace detail

/**
 * Store a value in an object's reserved slot.
 *
 * This can be used with both native objects and proxies.  However, if |obj| is
 * known to be a proxy, |js::SetProxyReservedSlot| in "js/Proxy.h" is very
 * slightly more efficient.
 */
inline void SetReservedSlot(JSObject* obj, size_t slot, const Value& value) {
  MOZ_ASSERT(slot < JSCLASS_RESERVED_SLOTS(GetClass(obj)));
  auto* sobj = reinterpret_cast<shadow::Object*>(obj);
  if (sobj->slotRef(slot).isGCThing() || value.isGCThing()) {
    detail::SetReservedSlotWithBarrier(obj, slot, value);
  } else {
    sobj->slotRef(slot) = value;
  }
}

/**
 * Helper function to get the pointer value (or nullptr if not set) from an
 * object's reserved slot. The slot must contain either a PrivateValue(T*) or
 * UndefinedValue.
 */
template <typename T>
inline T* GetMaybePtrFromReservedSlot(JSObject* obj, size_t slot) {
  Value v = GetReservedSlot(obj, slot);
  return v.isUndefined() ? nullptr : static_cast<T*>(v.toPrivate());
}

/**
 * Helper function to get the pointer value (or nullptr if not set) from the
 * object's first reserved slot. Must only be used for objects with a JSClass
 * that has the JSCLASS_SLOT0_IS_NSISUPPORTS flag.
 */
template <typename T>
inline T* GetObjectISupports(JSObject* obj) {
  MOZ_ASSERT(GetClass(obj)->slot0IsISupports());
  return GetMaybePtrFromReservedSlot<T>(obj, 0);
}

/**
 * Helper function to store |PrivateValue(nsISupportsValue)| in the object's
 * first reserved slot. Must only be used for objects with a JSClass that has
 * the JSCLASS_SLOT0_IS_NSISUPPORTS flag.
 *
 * Note: the pointer is opaque to the JS engine (including the GC) so it's the
 * embedding's responsibility to trace or free this value.
 */
inline void SetObjectISupports(JSObject* obj, void* nsISupportsValue) {
  MOZ_ASSERT(GetClass(obj)->slot0IsISupports());
  SetReservedSlot(obj, 0, PrivateValue(nsISupportsValue));
}

}  // namespace JS

// JSObject* is an aligned pointer, but this information isn't available in the
// public header. We specialize HasFreeLSB here so that JS::Result<JSObject*>
// compiles.

namespace mozilla {
namespace detail {
template <>
struct HasFreeLSB<JSObject*> {
  static constexpr bool value = true;
};
}  // namespace detail
}  // namespace mozilla

#endif  // js_public_Object_h
