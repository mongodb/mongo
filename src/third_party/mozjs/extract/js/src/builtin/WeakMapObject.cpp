/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/WeakMapObject-inl.h"

#include "builtin/WeakSetObject.h"
#include "gc/GC.h"
#include "gc/GCContext.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/PropertySpec.h"
#include "js/WeakMap.h"
#include "vm/Compartment.h"
#include "vm/JSContext.h"
#include "vm/SelfHosting.h"

#include "gc/GCContext-inl.h"
#include "gc/WeakMap-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

/* static */ MOZ_ALWAYS_INLINE bool WeakMapObject::is(HandleValue v) {
  return v.isObject() && v.toObject().is<WeakMapObject>();
}

/* static */ MOZ_ALWAYS_INLINE bool WeakMapObject::has_impl(
    JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(is(args.thisv()));

  if (!args.get(0).isObject()) {
    args.rval().setBoolean(false);
    return true;
  }

  if (ObjectValueWeakMap* map =
          args.thisv().toObject().as<WeakMapObject>().getMap()) {
    JSObject* key = &args[0].toObject();
    if (map->has(key)) {
      args.rval().setBoolean(true);
      return true;
    }
  }

  args.rval().setBoolean(false);
  return true;
}

/* static */
bool WeakMapObject::has(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<WeakMapObject::is, WeakMapObject::has_impl>(cx,
                                                                          args);
}

/* static */ MOZ_ALWAYS_INLINE bool WeakMapObject::get_impl(
    JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(WeakMapObject::is(args.thisv()));

  if (!args.get(0).isObject()) {
    args.rval().setUndefined();
    return true;
  }

  if (ObjectValueWeakMap* map =
          args.thisv().toObject().as<WeakMapObject>().getMap()) {
    JSObject* key = &args[0].toObject();
    if (ObjectValueWeakMap::Ptr ptr = map->lookup(key)) {
      args.rval().set(ptr->value());
      return true;
    }
  }

  args.rval().setUndefined();
  return true;
}

/* static */
bool WeakMapObject::get(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<WeakMapObject::is, WeakMapObject::get_impl>(cx,
                                                                          args);
}

/* static */ MOZ_ALWAYS_INLINE bool WeakMapObject::delete_impl(
    JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(WeakMapObject::is(args.thisv()));

  if (!args.get(0).isObject()) {
    args.rval().setBoolean(false);
    return true;
  }

  if (ObjectValueWeakMap* map =
          args.thisv().toObject().as<WeakMapObject>().getMap()) {
    JSObject* key = &args[0].toObject();
    // The lookup here is only used for the removal, so we can skip the read
    // barrier. This is not very important for performance, but makes it easier
    // to test nonbarriered removal from internal weakmaps (eg Debugger maps.)
    if (ObjectValueWeakMap::Ptr ptr = map->lookupUnbarriered(key)) {
      map->remove(ptr);
      args.rval().setBoolean(true);
      return true;
    }
  }

  args.rval().setBoolean(false);
  return true;
}

/* static */
bool WeakMapObject::delete_(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<WeakMapObject::is, WeakMapObject::delete_impl>(
      cx, args);
}

/* static */ MOZ_ALWAYS_INLINE bool WeakMapObject::set_impl(
    JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(WeakMapObject::is(args.thisv()));

  if (!args.get(0).isObject()) {
    ReportNotObject(cx, JSMSG_OBJECT_REQUIRED_WEAKMAP_KEY, args.get(0));
    return false;
  }

  RootedObject key(cx, &args[0].toObject());
  Rooted<WeakMapObject*> map(cx, &args.thisv().toObject().as<WeakMapObject>());

  if (!WeakCollectionPutEntryInternal(cx, map, key, args.get(1))) {
    return false;
  }
  args.rval().set(args.thisv());
  return true;
}

/* static */
bool WeakMapObject::set(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<WeakMapObject::is, WeakMapObject::set_impl>(cx,
                                                                          args);
}

size_t WeakCollectionObject::sizeOfExcludingThis(
    mozilla::MallocSizeOf aMallocSizeOf) {
  ObjectValueWeakMap* map = getMap();
  return map ? map->sizeOfIncludingThis(aMallocSizeOf) : 0;
}

bool WeakCollectionObject::nondeterministicGetKeys(
    JSContext* cx, Handle<WeakCollectionObject*> obj, MutableHandleObject ret) {
  RootedObject arr(cx, NewDenseEmptyArray(cx));
  if (!arr) {
    return false;
  }
  if (ObjectValueWeakMap* map = obj->getMap()) {
    // Prevent GC from mutating the weakmap while iterating.
    gc::AutoSuppressGC suppress(cx);
    for (ObjectValueWeakMap::Base::Range r = map->all(); !r.empty();
         r.popFront()) {
      JS::ExposeObjectToActiveJS(r.front().key());
      RootedObject key(cx, r.front().key());
      if (!cx->compartment()->wrap(cx, &key)) {
        return false;
      }
      if (!NewbornArrayPush(cx, arr, ObjectValue(*key))) {
        return false;
      }
    }
  }
  ret.set(arr);
  return true;
}

JS_PUBLIC_API bool JS_NondeterministicGetWeakMapKeys(JSContext* cx,
                                                     HandleObject objArg,
                                                     MutableHandleObject ret) {
  RootedObject obj(cx, UncheckedUnwrap(objArg));
  if (!obj || !obj->is<WeakMapObject>()) {
    ret.set(nullptr);
    return true;
  }
  return WeakCollectionObject::nondeterministicGetKeys(
      cx, obj.as<WeakCollectionObject>(), ret);
}

static void WeakCollection_trace(JSTracer* trc, JSObject* obj) {
  if (ObjectValueWeakMap* map = obj->as<WeakCollectionObject>().getMap()) {
    map->trace(trc);
  }
}

static void WeakCollection_finalize(JS::GCContext* gcx, JSObject* obj) {
  if (ObjectValueWeakMap* map = obj->as<WeakCollectionObject>().getMap()) {
    gcx->delete_(obj, map, MemoryUse::WeakMapObject);
  }
}

JS_PUBLIC_API JSObject* JS::NewWeakMapObject(JSContext* cx) {
  return NewBuiltinClassInstance<WeakMapObject>(cx);
}

JS_PUBLIC_API bool JS::IsWeakMapObject(JSObject* obj) {
  return obj->is<WeakMapObject>();
}

JS_PUBLIC_API bool JS::GetWeakMapEntry(JSContext* cx, HandleObject mapObj,
                                       HandleObject key,
                                       MutableHandleValue rval) {
  CHECK_THREAD(cx);
  cx->check(key);
  rval.setUndefined();
  ObjectValueWeakMap* map = mapObj->as<WeakMapObject>().getMap();
  if (!map) {
    return true;
  }
  if (ObjectValueWeakMap::Ptr ptr = map->lookup(key)) {
    // Read barrier to prevent an incorrectly gray value from escaping the
    // weak map. See the comment before UnmarkGrayChildren in gc/Marking.cpp
    ExposeValueToActiveJS(ptr->value().get());
    rval.set(ptr->value());
  }
  return true;
}

JS_PUBLIC_API bool JS::SetWeakMapEntry(JSContext* cx, HandleObject mapObj,
                                       HandleObject key, HandleValue val) {
  CHECK_THREAD(cx);
  cx->check(key, val);
  Handle<WeakMapObject*> rootedMap = mapObj.as<WeakMapObject>();
  return WeakCollectionPutEntryInternal(cx, rootedMap, key, val);
}

/* static */
bool WeakMapObject::construct(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // ES6 draft rev 31 (15 Jan 2015) 23.3.1.1 step 1.
  if (!ThrowIfNotConstructing(cx, args, "WeakMap")) {
    return false;
  }

  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_WeakMap, &proto)) {
    return false;
  }

  RootedObject obj(cx, NewObjectWithClassProto<WeakMapObject>(cx, proto));
  if (!obj) {
    return false;
  }

  // Steps 5-6, 11.
  if (!args.get(0).isNullOrUndefined()) {
    FixedInvokeArgs<1> args2(cx);
    args2[0].set(args[0]);

    RootedValue thisv(cx, ObjectValue(*obj));
    if (!CallSelfHostedFunction(cx, cx->names().WeakMapConstructorInit, thisv,
                                args2, args2.rval())) {
      return false;
    }
  }

  args.rval().setObject(*obj);
  return true;
}

const JSClassOps WeakCollectionObject::classOps_ = {
    nullptr,                  // addProperty
    nullptr,                  // delProperty
    nullptr,                  // enumerate
    nullptr,                  // newEnumerate
    nullptr,                  // resolve
    nullptr,                  // mayResolve
    WeakCollection_finalize,  // finalize
    nullptr,                  // call
    nullptr,                  // construct
    WeakCollection_trace,     // trace
};

const ClassSpec WeakMapObject::classSpec_ = {
    GenericCreateConstructor<WeakMapObject::construct, 0,
                             gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<WeakMapObject>,
    nullptr,
    nullptr,
    WeakMapObject::methods,
    WeakMapObject::properties,
};

const JSClass WeakMapObject::class_ = {
    "WeakMap",
    JSCLASS_HAS_RESERVED_SLOTS(SlotCount) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_WeakMap) | JSCLASS_BACKGROUND_FINALIZE,
    &WeakCollectionObject::classOps_, &WeakMapObject::classSpec_};

const JSClass WeakMapObject::protoClass_ = {
    "WeakMap.prototype", JSCLASS_HAS_CACHED_PROTO(JSProto_WeakMap),
    JS_NULL_CLASS_OPS, &WeakMapObject::classSpec_};

const JSPropertySpec WeakMapObject::properties[] = {
    JS_STRING_SYM_PS(toStringTag, "WeakMap", JSPROP_READONLY), JS_PS_END};

const JSFunctionSpec WeakMapObject::methods[] = {
    JS_FN("has", has, 1, 0), JS_FN("get", get, 1, 0),
    JS_FN("delete", delete_, 1, 0), JS_FN("set", set, 2, 0), JS_FS_END};
