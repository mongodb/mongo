/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/WeakSetObject.h"

#include "builtin/MapObject.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/PropertySpec.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/SelfHosting.h"

#include "builtin/WeakMapObject-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

/* static */ MOZ_ALWAYS_INLINE bool WeakSetObject::is(HandleValue v) {
  return v.isObject() && v.toObject().is<WeakSetObject>();
}

// ES2018 draft rev 7a2d3f053ecc2336fc19f377c55d52d78b11b296
// 23.4.3.1 WeakSet.prototype.add ( value )
/* static */ MOZ_ALWAYS_INLINE bool WeakSetObject::add_impl(
    JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(is(args.thisv()));

  // Step 4.
  if (!args.get(0).isObject()) {
    ReportNotObject(cx, JSMSG_OBJECT_REQUIRED_WEAKSET_VAL, args.get(0));
    return false;
  }

  // Steps 5-7.
  RootedObject value(cx, &args[0].toObject());
  Rooted<WeakSetObject*> map(cx, &args.thisv().toObject().as<WeakSetObject>());
  if (!WeakCollectionPutEntryInternal(cx, map, value, TrueHandleValue)) {
    return false;
  }

  // Steps 6.a.i, 8.
  args.rval().set(args.thisv());
  return true;
}

/* static */
bool WeakSetObject::add(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-3.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<WeakSetObject::is, WeakSetObject::add_impl>(cx,
                                                                          args);
}

// ES2018 draft rev 7a2d3f053ecc2336fc19f377c55d52d78b11b296
// 23.4.3.3 WeakSet.prototype.delete ( value )
/* static */ MOZ_ALWAYS_INLINE bool WeakSetObject::delete_impl(
    JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(is(args.thisv()));

  // Step 4.
  if (!args.get(0).isObject()) {
    args.rval().setBoolean(false);
    return true;
  }

  // Steps 5-6.
  if (ObjectValueWeakMap* map =
          args.thisv().toObject().as<WeakSetObject>().getMap()) {
    JSObject* value = &args[0].toObject();
    if (ObjectValueWeakMap::Ptr ptr = map->lookup(value)) {
      map->remove(ptr);
      args.rval().setBoolean(true);
      return true;
    }
  }

  // Step 7.
  args.rval().setBoolean(false);
  return true;
}

/* static */
bool WeakSetObject::delete_(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-3.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<WeakSetObject::is, WeakSetObject::delete_impl>(
      cx, args);
}

// ES2018 draft rev 7a2d3f053ecc2336fc19f377c55d52d78b11b296
// 23.4.3.4 WeakSet.prototype.has ( value )
/* static */ MOZ_ALWAYS_INLINE bool WeakSetObject::has_impl(
    JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(is(args.thisv()));

  // Step 5.
  if (!args.get(0).isObject()) {
    args.rval().setBoolean(false);
    return true;
  }

  // Steps 4, 6.
  if (ObjectValueWeakMap* map =
          args.thisv().toObject().as<WeakSetObject>().getMap()) {
    JSObject* value = &args[0].toObject();
    if (map->has(value)) {
      args.rval().setBoolean(true);
      return true;
    }
  }

  // Step 7.
  args.rval().setBoolean(false);
  return true;
}

/* static */
bool WeakSetObject::has(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-3.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<WeakSetObject::is, WeakSetObject::has_impl>(cx,
                                                                          args);
}

const ClassSpec WeakSetObject::classSpec_ = {
    GenericCreateConstructor<WeakSetObject::construct, 0,
                             gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<WeakSetObject>,
    nullptr,
    nullptr,
    WeakSetObject::methods,
    WeakSetObject::properties,
};

const JSClass WeakSetObject::class_ = {
    "WeakSet",
    JSCLASS_HAS_RESERVED_SLOTS(SlotCount) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_WeakSet) | JSCLASS_BACKGROUND_FINALIZE,
    &WeakCollectionObject::classOps_, &WeakSetObject::classSpec_};

const JSClass WeakSetObject::protoClass_ = {
    "WeakSet.prototype", JSCLASS_HAS_CACHED_PROTO(JSProto_WeakSet),
    JS_NULL_CLASS_OPS, &WeakSetObject::classSpec_};

const JSPropertySpec WeakSetObject::properties[] = {
    JS_STRING_SYM_PS(toStringTag, "WeakSet", JSPROP_READONLY), JS_PS_END};

const JSFunctionSpec WeakSetObject::methods[] = {
    JS_FN("add", add, 1, 0), JS_FN("delete", delete_, 1, 0),
    JS_FN("has", has, 1, 0), JS_FS_END};

WeakSetObject* WeakSetObject::create(JSContext* cx,
                                     HandleObject proto /* = nullptr */) {
  return NewObjectWithClassProto<WeakSetObject>(cx, proto);
}

bool WeakSetObject::isBuiltinAdd(HandleValue add) {
  return IsNativeFunction(add, WeakSetObject::add);
}

bool WeakSetObject::construct(JSContext* cx, unsigned argc, Value* vp) {
  // Based on our "Set" implementation instead of the more general ES6 steps.
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ThrowIfNotConstructing(cx, args, "WeakSet")) {
    return false;
  }

  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_WeakSet, &proto)) {
    return false;
  }

  Rooted<WeakSetObject*> obj(cx, WeakSetObject::create(cx, proto));
  if (!obj) {
    return false;
  }

  if (!args.get(0).isNullOrUndefined()) {
    RootedValue iterable(cx, args[0]);
    bool optimized = false;
    if (!IsOptimizableInitForSet<GlobalObject::getOrCreateWeakSetPrototype,
                                 isBuiltinAdd>(cx, obj, iterable, &optimized)) {
      return false;
    }

    if (optimized) {
      RootedValue keyVal(cx);
      RootedObject keyObject(cx);
      Rooted<ArrayObject*> array(cx, &iterable.toObject().as<ArrayObject>());
      for (uint32_t index = 0; index < array->getDenseInitializedLength();
           ++index) {
        keyVal.set(array->getDenseElement(index));
        MOZ_ASSERT(!keyVal.isMagic(JS_ELEMENTS_HOLE));

        if (keyVal.isPrimitive()) {
          ReportNotObject(cx, JSMSG_OBJECT_REQUIRED_WEAKSET_VAL, keyVal);
          return false;
        }

        keyObject = &keyVal.toObject();
        if (!WeakCollectionPutEntryInternal(cx, obj, keyObject,
                                            TrueHandleValue)) {
          return false;
        }
      }
    } else {
      FixedInvokeArgs<1> args2(cx);
      args2[0].set(args[0]);

      RootedValue thisv(cx, ObjectValue(*obj));
      if (!CallSelfHostedFunction(cx, cx->names().WeakSetConstructorInit, thisv,
                                  args2, args2.rval())) {
        return false;
      }
    }
  }

  args.rval().setObject(*obj);
  return true;
}

JS_PUBLIC_API bool JS_NondeterministicGetWeakSetKeys(JSContext* cx,
                                                     HandleObject objArg,
                                                     MutableHandleObject ret) {
  RootedObject obj(cx, UncheckedUnwrap(objArg));
  if (!obj || !obj->is<WeakSetObject>()) {
    ret.set(nullptr);
    return true;
  }
  return WeakCollectionObject::nondeterministicGetKeys(
      cx, obj.as<WeakCollectionObject>(), ret);
}
