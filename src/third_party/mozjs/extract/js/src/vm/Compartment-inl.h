/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Compartment_inl_h
#define vm_Compartment_inl_h

#include "vm/Compartment.h"

#include <type_traits>

#include "jsapi.h"
#include "jsfriendapi.h"
#include "jsnum.h"
#include "js/CallArgs.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Wrapper.h"
#include "vm/Iteration.h"
#include "vm/JSObject.h"

#include "vm/JSContext-inl.h"

struct JSClass;

inline js::StringWrapperMap::Ptr JS::Compartment::lookupWrapper(
    JSString* str) const {
  return zone()->crossZoneStringWrappers().lookup(str);
}

inline bool JS::Compartment::wrap(JSContext* cx, JS::MutableHandleValue vp) {
  /* Only GC things have to be wrapped or copied. */
  if (!vp.isGCThing()) {
    return true;
  }

  /*
   * Symbols are GC things, but never need to be wrapped or copied because
   * they are always allocated in the atoms zone. They still need to be
   * marked in the new compartment's zone, however.
   */
  if (vp.isSymbol()) {
    cx->markAtomValue(vp);
    return true;
  }

  /* Handle strings. */
  if (vp.isString()) {
    JS::RootedString str(cx, vp.toString());
    if (!wrap(cx, &str)) {
      return false;
    }
    vp.setString(str);
    return true;
  }

  if (vp.isBigInt()) {
    JS::RootedBigInt bi(cx, vp.toBigInt());
    if (!wrap(cx, &bi)) {
      return false;
    }
    vp.setBigInt(bi);
    return true;
  }

#ifdef ENABLE_RECORD_TUPLE
  if (vp.isExtendedPrimitive()) {
    JS::RootedObject extPrim(cx, &vp.toExtendedPrimitive());
    if (!wrapExtendedPrimitive(cx, &extPrim)) {
      return false;
    }
    vp.setExtendedPrimitive(*extPrim);
    return true;
  }
#endif

  MOZ_ASSERT(vp.isObject());

  /*
   * All that's left are objects.
   *
   * Object wrapping isn't the fastest thing in the world, in part because
   * we have to unwrap and invoke the prewrap hook to find the identity
   * object before we even start checking the cache. Neither of these
   * operations are needed in the common case, where we're just wrapping
   * a plain JS object from the wrappee's side of the membrane to the
   * wrapper's side.
   *
   * To optimize this, we note that the cache should only ever contain
   * identity objects - that is to say, objects that serve as the
   * canonical representation for a unique object identity observable by
   * script. Unwrap and prewrap are both steps that we take to get to the
   * identity of an incoming objects, and as such, they shuld never map
   * one identity object to another object. This means that we can safely
   * check the cache immediately, and only risk false negatives. Do this
   * in opt builds, and do both in debug builds so that we can assert
   * that we get the same answer.
   */
#ifdef DEBUG
  JS::AssertValueIsNotGray(vp);
  JS::RootedObject cacheResult(cx);
#endif
  if (js::ObjectWrapperMap::Ptr p = lookupWrapper(&vp.toObject())) {
#ifdef DEBUG
    cacheResult = p->value().get();
#else
    vp.setObject(*p->value().get());
    return true;
#endif
  }

  JS::RootedObject obj(cx, &vp.toObject());
  if (!wrap(cx, &obj)) {
    return false;
  }
  vp.setObject(*obj);
  MOZ_ASSERT_IF(cacheResult, obj == cacheResult);
  return true;
}

inline bool JS::Compartment::wrap(JSContext* cx,
                                  MutableHandle<mozilla::Maybe<Value>> vp) {
  if (vp.get().isNothing()) {
    return true;
  }

  return wrap(cx, MutableHandle<Value>::fromMarkedLocation(vp.get().ptr()));
}

namespace js {
namespace detail {

/**
 * Return the name of class T as a static null-terminated ASCII string constant
 * (for error messages).
 */
template <class T>
const char* ClassName() {
  return T::class_.name;
}

template <class T, class ErrorCallback>
[[nodiscard]] T* UnwrapAndTypeCheckValueSlowPath(JSContext* cx,
                                                 HandleValue value,
                                                 ErrorCallback throwTypeError) {
  JSObject* obj = nullptr;
  if (value.isObject()) {
    obj = &value.toObject();
    if (IsWrapper(obj)) {
      obj = CheckedUnwrapStatic(obj);
      if (!obj) {
        ReportAccessDenied(cx);
        return nullptr;
      }
    }
  }

  if (!obj || !obj->is<T>()) {
    throwTypeError();
    return nullptr;
  }

  return &obj->as<T>();
}

template <class ErrorCallback>
[[nodiscard]] JSObject* UnwrapAndTypeCheckValueSlowPath(
    JSContext* cx, HandleValue value, const JSClass* clasp,
    ErrorCallback throwTypeError) {
  JSObject* obj = nullptr;
  if (value.isObject()) {
    obj = &value.toObject();
    if (IsWrapper(obj)) {
      obj = CheckedUnwrapStatic(obj);
      if (!obj) {
        ReportAccessDenied(cx);
        return nullptr;
      }
    }
  }

  if (!obj || !obj->hasClass(clasp)) {
    throwTypeError();
    return nullptr;
  }

  return obj;
}

}  // namespace detail

/**
 * Remove all wrappers from `val` and try to downcast the result to class `T`.
 *
 * DANGER: The result may not be same-compartment with `cx`.
 *
 * This calls `throwTypeError` if the value isn't an object, cannot be
 * unwrapped, or isn't an instance of the expected type. `throwTypeError` must
 * in fact throw a TypeError (or OOM trying).
 */
template <class T, class ErrorCallback>
[[nodiscard]] inline T* UnwrapAndTypeCheckValue(JSContext* cx,
                                                HandleValue value,
                                                ErrorCallback throwTypeError) {
  cx->check(value);

  static_assert(!std::is_convertible_v<T*, Wrapper*>,
                "T can't be a Wrapper type; this function discards wrappers");

  if (value.isObject() && value.toObject().is<T>()) {
    return &value.toObject().as<T>();
  }

  return detail::UnwrapAndTypeCheckValueSlowPath<T>(cx, value, throwTypeError);
}

/**
 * Remove all wrappers from |val| and try to downcast the result to an object of
 * the class |clasp|.
 *
 * DANGER: The result may not be same-compartment with |cx|.
 *
 * This calls |throwTypeError| if the value isn't an object, cannot be
 * unwrapped, or isn't an instance of the expected type.  |throwTypeError| must
 * in fact throw a TypeError (or OOM trying).
 */
template <class ErrorCallback>
[[nodiscard]] inline JSObject* UnwrapAndTypeCheckValue(
    JSContext* cx, HandleValue value, const JSClass* clasp,
    ErrorCallback throwTypeError) {
  cx->check(value);

  if (value.isObject() && value.toObject().hasClass(clasp)) {
    return &value.toObject();
  }

  return detail::UnwrapAndTypeCheckValueSlowPath(cx, value, clasp,
                                                 throwTypeError);
}

/**
 * Remove all wrappers from `args.thisv()` and try to downcast the result to
 * class `T`.
 *
 * DANGER: The result may not be same-compartment with `cx`.
 *
 * This throws a TypeError if the value isn't an object, cannot be unwrapped,
 * or isn't an instance of the expected type.
 */
template <class T>
[[nodiscard]] inline T* UnwrapAndTypeCheckThis(JSContext* cx,
                                               const CallArgs& args,
                                               const char* methodName) {
  HandleValue thisv = args.thisv();
  return UnwrapAndTypeCheckValue<T>(cx, thisv, [cx, methodName, thisv] {
    JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr,
                               JSMSG_INCOMPATIBLE_PROTO, detail::ClassName<T>(),
                               methodName, InformalValueTypeName(thisv));
  });
}

/**
 * Remove all wrappers from `args[argIndex]` and try to downcast the result to
 * class `T`.
 *
 * DANGER: The result may not be same-compartment with `cx`.
 *
 * This throws a TypeError if the specified argument is missing, isn't an
 * object, cannot be unwrapped, or isn't an instance of the expected type.
 */
template <class T>
[[nodiscard]] inline T* UnwrapAndTypeCheckArgument(JSContext* cx,
                                                   CallArgs& args,
                                                   const char* methodName,
                                                   int argIndex) {
  HandleValue val = args.get(argIndex);
  return UnwrapAndTypeCheckValue<T>(cx, val, [cx, val, methodName, argIndex] {
    Int32ToCStringBuf cbuf;
    char* numStr = Int32ToCString(&cbuf, argIndex + 1);
    MOZ_ASSERT(numStr);
    JS_ReportErrorNumberLatin1(
        cx, GetErrorMessage, nullptr, JSMSG_WRONG_TYPE_ARG, numStr, methodName,
        detail::ClassName<T>(), InformalValueTypeName(val));
  });
}

/**
 * Unwrap an object of a known type.
 *
 * If `obj` is an object of class T, this returns a pointer to that object. If
 * `obj` is a wrapper for such an object, this tries to unwrap the object and
 * return a pointer to it. If access is denied, or `obj` was a wrapper but has
 * been nuked, this reports an error and returns null.
 *
 * In all other cases, the behavior is undefined, so call this only if `obj` is
 * known to have been an object of class T, or a wrapper to a T, at some point.
 */
template <class T>
[[nodiscard]] inline T* UnwrapAndDowncastObject(JSContext* cx, JSObject* obj) {
  static_assert(!std::is_convertible_v<T*, Wrapper*>,
                "T can't be a Wrapper type; this function discards wrappers");

  if (IsProxy(obj)) {
    if (JS_IsDeadWrapper(obj)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEAD_OBJECT);
      return nullptr;
    }

    // It would probably be OK to do an unchecked unwrap here, but we allow
    // arbitrary security policies, so check anyway.
    obj = obj->maybeUnwrapAs<T>();
    if (!obj) {
      ReportAccessDenied(cx);
      return nullptr;
    }
  }

  return &obj->as<T>();
}

/**
 * Unwrap an object of a known (but not compile-time-known) class.
 *
 * If |obj| is an object with class |clasp|, this returns |obj|.  If |obj| is a
 * wrapper for such an object, this tries to unwrap the object and return a
 * pointer to it.  If access is denied, or |obj| was a wrapper but has been
 * nuked, this reports an error and returns null.
 *
 * In all other cases, the behavior is undefined, so call this only if |obj| is
 * known to have had class |clasp|, or been a wrapper to such an object, at some
 * point.
 */
[[nodiscard]] inline JSObject* UnwrapAndDowncastObject(JSContext* cx,
                                                       JSObject* obj,
                                                       const JSClass* clasp) {
  if (IsProxy(obj)) {
    if (JS_IsDeadWrapper(obj)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEAD_OBJECT);
      return nullptr;
    }

    // It would probably be OK to do an unchecked unwrap here, but we allow
    // arbitrary security policies, so check anyway.
    obj = obj->maybeUnwrapAs(clasp);
    if (!obj) {
      ReportAccessDenied(cx);
      return nullptr;
    }
  }

  MOZ_ASSERT(obj->hasClass(clasp));
  return obj;
}

/**
 * Unwrap a value of a known type. See UnwrapAndDowncastObject.
 */
template <class T>
[[nodiscard]] inline T* UnwrapAndDowncastValue(JSContext* cx,
                                               const Value& value) {
  return UnwrapAndDowncastObject<T>(cx, &value.toObject());
}

/**
 * Unwrap an object of a known (but not compile-time-known) class.  See
 * UnwrapAndDowncastObject.
 */
[[nodiscard]] inline JSObject* UnwrapAndDowncastValue(JSContext* cx,
                                                      const Value& value,
                                                      const JSClass* clasp) {
  return UnwrapAndDowncastObject(cx, &value.toObject(), clasp);
}

/**
 * Read a private slot that is known to point to a particular type of object.
 *
 * Some internal slots specified in various standards effectively have static
 * types. For example, the [[ownerReadableStream]] slot of a stream reader is
 * guaranteed to be a ReadableStream. However, because of compartments, we
 * sometimes store a cross-compartment wrapper in that slot. And since wrappers
 * can be nuked, that wrapper may become a dead object proxy.
 *
 * UnwrapInternalSlot() copes with the cross-compartment and dead object cases,
 * but not plain bugs where the slot hasn't been initialized or doesn't contain
 * the expected type of object. Call this only if the slot is certain to
 * contain either an instance of T, a wrapper for a T, or a dead object.
 *
 * `cx` and `unwrappedObj` are not required to be same-compartment.
 *
 * DANGER: The result may not be same-compartment with either `cx` or `obj`.
 */
template <class T>
[[nodiscard]] inline T* UnwrapInternalSlot(JSContext* cx,
                                           Handle<NativeObject*> unwrappedObj,
                                           uint32_t slot) {
  static_assert(!std::is_convertible_v<T*, Wrapper*>,
                "T can't be a Wrapper type; this function discards wrappers");

  return UnwrapAndDowncastValue<T>(cx, unwrappedObj->getFixedSlot(slot));
}

/**
 * Read a function slot that is known to point to a particular type of object.
 *
 * This is like UnwrapInternalSlot, but for extended function slots. Call this
 * only if the specified slot is known to have been initialized with an object
 * of class T or a wrapper for such an object.
 *
 * DANGER: The result may not be same-compartment with `cx`.
 */
template <class T>
[[nodiscard]] T* UnwrapCalleeSlot(JSContext* cx, CallArgs& args,
                                  size_t extendedSlot) {
  JSFunction& func = args.callee().as<JSFunction>();
  return UnwrapAndDowncastValue<T>(cx, func.getExtendedSlot(extendedSlot));
}

}  // namespace js

MOZ_ALWAYS_INLINE bool JS::Compartment::objectMaybeInIteration(JSObject* obj) {
  MOZ_ASSERT(obj->compartment() == this);

  js::NativeIteratorListIter iter(&enumerators_);

  // If the list is empty, we're not iterating any objects.
  if (iter.done()) {
    return false;
  }

  // If the list contains a single object, check if it's |obj|.
  js::NativeIterator* next = iter.next();
  if (iter.done()) {
    return next->objectBeingIterated() == obj;
  }

  return true;
}

#endif /* vm_Compartment_inl_h */
