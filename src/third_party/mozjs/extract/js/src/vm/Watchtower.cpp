/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Watchtower.h"

#include "js/CallAndConstruct.h"
#include "vm/Compartment.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/NativeObject.h"
#include "vm/PlainObject.h"
#include "vm/Realm.h"

#include "vm/Compartment-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/Realm-inl.h"
#include "vm/Shape-inl.h"

using namespace js;

static bool AddToWatchtowerLog(JSContext* cx, const char* kind,
                               HandleObject obj, HandleValue extra) {
  // Add an object storing {kind, object, extra} to the log for testing
  // purposes.

  MOZ_ASSERT(obj->useWatchtowerTestingLog());

  RootedString kindString(cx, NewStringCopyZ<CanGC>(cx, kind));
  if (!kindString) {
    return false;
  }

  Rooted<PlainObject*> logObj(cx, NewPlainObjectWithProto(cx, nullptr));
  if (!logObj) {
    return false;
  }
  if (!JS_DefineProperty(cx, logObj, "kind", kindString, JSPROP_ENUMERATE)) {
    return false;
  }
  if (!JS_DefineProperty(cx, logObj, "object", obj, JSPROP_ENUMERATE)) {
    return false;
  }
  if (!JS_DefineProperty(cx, logObj, "extra", extra, JSPROP_ENUMERATE)) {
    return false;
  }

  if (!cx->runtime()->watchtowerTestingLog->append(logObj)) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

static bool ReshapeForShadowedProp(JSContext* cx, Handle<NativeObject*> obj,
                                   HandleId id) {
  // |obj| has been used as the prototype of another object. Check if we're
  // shadowing a property on its proto chain. In this case we need to reshape
  // that object for shape teleporting to work correctly.
  //
  // See also the 'Shape Teleporting Optimization' comment in jit/CacheIR.cpp.

  MOZ_ASSERT(obj->isUsedAsPrototype());

  // Lookups on integer ids cannot be cached through prototypes.
  if (id.isInt()) {
    return true;
  }

  RootedObject proto(cx, obj->staticPrototype());
  while (proto) {
    // Lookups will not be cached through non-native protos.
    if (!proto->is<NativeObject>()) {
      break;
    }

    if (proto->as<NativeObject>().contains(cx, id)) {
      return JSObject::setInvalidatedTeleporting(cx, proto);
    }

    proto = proto->staticPrototype();
  }

  return true;
}

static void InvalidateMegamorphicCache(JSContext* cx,
                                       Handle<NativeObject*> obj) {
  // The megamorphic cache only checks the receiver object's shape. We need to
  // invalidate the cache when a prototype object changes its set of properties,
  // to account for cached properties that are deleted, turned into an accessor
  // property, or shadowed by another object on the proto chain.

  MOZ_ASSERT(obj->isUsedAsPrototype());

  cx->caches().megamorphicCache.bumpGeneration();
  cx->caches().megamorphicSetPropCache->bumpGeneration();
}

// static
bool Watchtower::watchPropertyAddSlow(JSContext* cx, Handle<NativeObject*> obj,
                                      HandleId id) {
  MOZ_ASSERT(watchesPropertyAdd(obj));

  if (obj->isUsedAsPrototype()) {
    if (!ReshapeForShadowedProp(cx, obj, id)) {
      return false;
    }
    if (!id.isInt()) {
      InvalidateMegamorphicCache(cx, obj);
    }
  }

  if (MOZ_UNLIKELY(obj->useWatchtowerTestingLog())) {
    RootedValue val(cx, IdToValue(id));
    if (!AddToWatchtowerLog(cx, "add-prop", obj, val)) {
      return false;
    }
  }

  return true;
}

static bool ReshapeForProtoMutation(JSContext* cx, HandleObject obj) {
  // To avoid the JIT guarding on each prototype in the proto chain to detect
  // prototype mutation, we can instead reshape the rest of the proto chain such
  // that a guard on any of them is sufficient. To avoid excessive reshaping and
  // invalidation, we apply heuristics to decide when to apply this and when
  // to require a guard.
  //
  // There are two cases:
  //
  // (1) The object is not marked IsUsedAsPrototype. This is the common case.
  //     Because shape implies proto, we rely on the caller changing the
  //     object's shape. The JIT guards on this object's shape or prototype so
  //     there's nothing we have to do here for objects on the proto chain.
  //
  // (2) The object is marked IsUsedAsPrototype. This implies the object may be
  //     participating in shape teleporting. To invalidate JIT ICs depending on
  //     the proto chain being unchanged, set the InvalidatedTeleporting shape
  //     flag for this object and objects on its proto chain.
  //
  //     This flag disables future shape teleporting attempts, so next time this
  //     happens the loop below will be a no-op.
  //
  // NOTE: We only handle NativeObjects and don't propagate reshapes through
  //       any non-native objects on the chain.
  //
  // See Also:
  //  - GeneratePrototypeGuards
  //  - GeneratePrototypeHoleGuards

  MOZ_ASSERT(obj->isUsedAsPrototype());

  RootedObject pobj(cx, obj);

  while (pobj && pobj->is<NativeObject>()) {
    if (!pobj->hasInvalidatedTeleporting()) {
      if (!JSObject::setInvalidatedTeleporting(cx, pobj)) {
        return false;
      }
    }
    pobj = pobj->staticPrototype();
  }

  return true;
}

static bool WatchProtoChangeImpl(JSContext* cx, HandleObject obj) {
  if (!obj->isUsedAsPrototype()) {
    return true;
  }
  if (!ReshapeForProtoMutation(cx, obj)) {
    return false;
  }
  if (obj->is<NativeObject>()) {
    InvalidateMegamorphicCache(cx, obj.as<NativeObject>());
  }
  return true;
}

// static
bool Watchtower::watchProtoChangeSlow(JSContext* cx, HandleObject obj) {
  MOZ_ASSERT(watchesProtoChange(obj));

  if (!WatchProtoChangeImpl(cx, obj)) {
    return false;
  }

  if (MOZ_UNLIKELY(obj->useWatchtowerTestingLog())) {
    if (!AddToWatchtowerLog(cx, "proto-change", obj,
                            JS::UndefinedHandleValue)) {
      return false;
    }
  }

  return true;
}

// static
bool Watchtower::watchPropertyRemoveSlow(JSContext* cx,
                                         Handle<NativeObject*> obj,
                                         HandleId id) {
  MOZ_ASSERT(watchesPropertyRemove(obj));

  if (obj->isUsedAsPrototype() && !id.isInt()) {
    InvalidateMegamorphicCache(cx, obj);
  }

  if (obj->isGenerationCountedGlobal()) {
    obj->as<GlobalObject>().bumpGenerationCount();
  }

  if (MOZ_UNLIKELY(obj->useWatchtowerTestingLog())) {
    RootedValue val(cx, IdToValue(id));
    if (!AddToWatchtowerLog(cx, "remove-prop", obj, val)) {
      return false;
    }
  }

  return true;
}

// static
bool Watchtower::watchPropertyChangeSlow(JSContext* cx,
                                         Handle<NativeObject*> obj, HandleId id,
                                         PropertyFlags flags) {
  MOZ_ASSERT(watchesPropertyChange(obj));

  if (obj->isUsedAsPrototype() && !id.isInt()) {
    InvalidateMegamorphicCache(cx, obj);
  }

  if (obj->isGenerationCountedGlobal()) {
    // The global generation counter only cares whether a property
    // changes from data property to accessor or vice-versa. Changing
    // the flags on a property doesn't matter.
    uint32_t propIndex;
    Rooted<PropMap*> map(cx, obj->shape()->lookup(cx, id, &propIndex));
    MOZ_ASSERT(map);
    PropertyInfo prop = map->getPropertyInfo(propIndex);
    bool wasAccessor = prop.isAccessorProperty();
    bool isAccessor = flags.isAccessorProperty();
    if (wasAccessor != isAccessor) {
      obj->as<GlobalObject>().bumpGenerationCount();
    }
  }

  if (MOZ_UNLIKELY(obj->useWatchtowerTestingLog())) {
    RootedValue val(cx, IdToValue(id));
    if (!AddToWatchtowerLog(cx, "change-prop", obj, val)) {
      return false;
    }
  }

  return true;
}

// static
bool Watchtower::watchFreezeOrSealSlow(JSContext* cx,
                                       Handle<NativeObject*> obj) {
  MOZ_ASSERT(watchesFreezeOrSeal(obj));

  if (MOZ_UNLIKELY(obj->useWatchtowerTestingLog())) {
    if (!AddToWatchtowerLog(cx, "freeze-or-seal", obj,
                            JS::UndefinedHandleValue)) {
      return false;
    }
  }

  return true;
}

// static
bool Watchtower::watchObjectSwapSlow(JSContext* cx, HandleObject a,
                                     HandleObject b) {
  MOZ_ASSERT(watchesObjectSwap(a, b));

  // If we're swapping an object that's used as prototype, we're mutating the
  // proto chains of other objects. Treat this as a proto change to ensure we
  // invalidate shape teleporting and megamorphic caches.
  if (!WatchProtoChangeImpl(cx, a)) {
    return false;
  }
  if (!WatchProtoChangeImpl(cx, b)) {
    return false;
  }

  // Note: we don't invoke the testing callback for swap because the objects may
  // not be safe to expose to JS at this point. See bug 1754699.

  return true;
}
