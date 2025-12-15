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
#include "vm/NativeObject-inl.h"
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

  bool useDictionaryTeleporting =
      cx->zone()->shapeZone().useDictionaryModeTeleportation();

  RootedObject proto(cx, obj->staticPrototype());
  while (proto) {
    // Lookups will not be cached through non-native protos.
    if (!proto->is<NativeObject>()) {
      break;
    }
    if (proto->as<NativeObject>().contains(cx, id)) {
      if (useDictionaryTeleporting) {
        JS_LOG(teleporting, Debug,
               "Shadowed Prop: Dictionary Reshape for Teleporting");

        return JSObject::reshapeForTeleporting(cx, proto);
      }

      JS_LOG(teleporting, Info,
             "Shadowed Prop: Invalidating Reshape for Teleporting");
      return JSObject::setInvalidatedTeleporting(cx, proto);
    }

    proto = proto->staticPrototype();
  }

  return true;
}

static void InvalidateMegamorphicCache(JSContext* cx, Handle<NativeObject*> obj,
                                       bool invalidateGetPropCache = true) {
  // The megamorphic cache only checks the receiver object's shape. We need to
  // invalidate the cache when a prototype object changes its set of properties,
  // to account for cached properties that are deleted, turned into an accessor
  // property, or shadowed by another object on the proto chain.

  MOZ_ASSERT(obj->isUsedAsPrototype());

  if (invalidateGetPropCache) {
    cx->caches().megamorphicCache.bumpGeneration();
  }
  cx->caches().megamorphicSetPropCache->bumpGeneration();
}

void MaybePopReturnFuses(JSContext* cx, Handle<NativeObject*> nobj) {
  GlobalObject* global = &nobj->global();
  JSObject* objectProto = &global->getObjectPrototype();
  if (nobj == objectProto) {
    nobj->realm()->realmFuses.objectPrototypeHasNoReturnProperty.popFuse(
        cx, nobj->realm()->realmFuses);
    return;
  }

  JSObject* iteratorProto = global->maybeGetIteratorPrototype();
  if (nobj == iteratorProto) {
    nobj->realm()->realmFuses.iteratorPrototypeHasNoReturnProperty.popFuse(
        cx, nobj->realm()->realmFuses);
    return;
  }

  JSObject* arrayIterProto = global->maybeGetArrayIteratorPrototype();
  if (nobj == arrayIterProto) {
    nobj->realm()->realmFuses.arrayIteratorPrototypeHasNoReturnProperty.popFuse(
        cx, nobj->realm()->realmFuses);
    return;
  }
}

static void MaybePopStringPrototypeSymbolsFuse(JSContext* cx, NativeObject* obj,
                                               PropertyKey key) {
  if (!key.isSymbol()) {
    return;
  }
  GlobalObject* global = &obj->global();
  if (obj != global->maybeGetPrototype(JSProto_String) &&
      obj != global->maybeGetPrototype(JSProto_Object)) {
    return;
  }
  if (key.toSymbol() == cx->wellKnownSymbols().match ||
      key.toSymbol() == cx->wellKnownSymbols().replace ||
      key.toSymbol() == cx->wellKnownSymbols().search ||
      key.toSymbol() == cx->wellKnownSymbols().split) {
    obj->realm()->realmFuses.optimizeStringPrototypeSymbolsFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
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

    if (id == NameToId(cx->names().return_)) {
      MaybePopReturnFuses(cx, obj);
    }

    MaybePopStringPrototypeSymbolsFuse(cx, obj, id);
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

  bool useDictionaryTeleporting =
      cx->zone()->shapeZone().useDictionaryModeTeleportation();

  while (pobj && pobj->is<NativeObject>()) {
    if (useDictionaryTeleporting) {
      MOZ_ASSERT(!pobj->hasInvalidatedTeleporting(),
                 "Once we start using invalidation shouldn't do any more "
                 "dictionary mode teleportation");
      JS_LOG(teleporting, Debug,
             "Proto Mutation: Dictionary Reshape for Teleporting");

      if (!JSObject::reshapeForTeleporting(cx, pobj)) {
        return false;
      }
    } else if (!pobj->hasInvalidatedTeleporting()) {
      JS_LOG(teleporting, Info,
             "Proto Mutation: Invalidating Reshape for Teleporting");

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

    NativeObject* nobj = &obj->as<NativeObject>();
    if (nobj == nobj->global().maybeGetArrayIteratorPrototype()) {
      nobj->realm()->realmFuses.arrayIteratorPrototypeHasIteratorProto.popFuse(
          cx, nobj->realm()->realmFuses);
    }

    if (nobj == nobj->global().maybeGetIteratorPrototype()) {
      nobj->realm()->realmFuses.iteratorPrototypeHasObjectProto.popFuse(
          cx, nobj->realm()->realmFuses);
    }

    if (nobj == nobj->global().maybeGetPrototype(JSProto_String)) {
      nobj->realm()->realmFuses.optimizeStringPrototypeSymbolsFuse.popFuse(
          cx, nobj->realm()->realmFuses);
    }
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

static void MaybePopArrayConstructorFuses(JSContext* cx, NativeObject* obj,
                                          jsid id) {
  if (obj != obj->global().maybeGetConstructor(JSProto_Array)) {
    return;
  }
  if (id.isWellKnownSymbol(JS::SymbolCode::species)) {
    obj->realm()->realmFuses.optimizeArraySpeciesFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
}

static void MaybePopArrayPrototypeFuses(JSContext* cx, NativeObject* obj,
                                        jsid id) {
  if (obj != obj->global().maybeGetArrayPrototype()) {
    return;
  }
  if (id.isWellKnownSymbol(JS::SymbolCode::iterator)) {
    obj->realm()->realmFuses.arrayPrototypeIteratorFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
  if (id.isAtom(cx->names().constructor)) {
    obj->realm()->realmFuses.optimizeArraySpeciesFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
}

static void MaybePopArrayIteratorPrototypeFuses(JSContext* cx,
                                                NativeObject* obj, jsid id) {
  if (obj != obj->global().maybeGetArrayIteratorPrototype()) {
    return;
  }
  if (id.isAtom(cx->names().next)) {
    obj->realm()->realmFuses.arrayPrototypeIteratorNextFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
}

static void MaybePopMapPrototypeFuses(JSContext* cx, NativeObject* obj,
                                      jsid id) {
  if (obj != obj->global().maybeGetPrototype(JSProto_Map)) {
    return;
  }
  if (id.isWellKnownSymbol(JS::SymbolCode::iterator)) {
    obj->realm()->realmFuses.optimizeMapObjectIteratorFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
  if (id.isAtom(cx->names().set)) {
    obj->realm()->realmFuses.optimizeMapPrototypeSetFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
}

static void MaybePopMapIteratorPrototypeFuses(JSContext* cx, NativeObject* obj,
                                              jsid id) {
  if (obj != obj->global().maybeBuiltinProto(
                 GlobalObject::ProtoKind::MapIteratorProto)) {
    return;
  }
  if (id.isAtom(cx->names().next)) {
    obj->realm()->realmFuses.optimizeMapObjectIteratorFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
}

static void MaybePopSetPrototypeFuses(JSContext* cx, NativeObject* obj,
                                      jsid id) {
  if (obj != obj->global().maybeGetPrototype(JSProto_Set)) {
    return;
  }
  if (id.isWellKnownSymbol(JS::SymbolCode::iterator)) {
    obj->realm()->realmFuses.optimizeSetObjectIteratorFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
  if (id.isAtom(cx->names().add)) {
    obj->realm()->realmFuses.optimizeSetPrototypeAddFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
}

static void MaybePopSetIteratorPrototypeFuses(JSContext* cx, NativeObject* obj,
                                              jsid id) {
  if (obj != obj->global().maybeBuiltinProto(
                 GlobalObject::ProtoKind::SetIteratorProto)) {
    return;
  }
  if (id.isAtom(cx->names().next)) {
    obj->realm()->realmFuses.optimizeSetObjectIteratorFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
}

static void MaybePopWeakMapPrototypeFuses(JSContext* cx, NativeObject* obj,
                                          jsid id) {
  if (obj != obj->global().maybeGetPrototype(JSProto_WeakMap)) {
    return;
  }
  if (id.isAtom(cx->names().set)) {
    obj->realm()->realmFuses.optimizeWeakMapPrototypeSetFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
}

static void MaybePopWeakSetPrototypeFuses(JSContext* cx, NativeObject* obj,
                                          jsid id) {
  if (obj != obj->global().maybeGetPrototype(JSProto_WeakSet)) {
    return;
  }
  if (id.isAtom(cx->names().add)) {
    obj->realm()->realmFuses.optimizeWeakSetPrototypeAddFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
}

static void MaybePopPromiseConstructorFuses(JSContext* cx, NativeObject* obj,
                                            jsid id) {
  if (obj != obj->global().maybeGetConstructor(JSProto_Promise)) {
    return;
  }
  if (id.isWellKnownSymbol(JS::SymbolCode::species) ||
      id.isAtom(cx->names().resolve)) {
    obj->realm()->realmFuses.optimizePromiseLookupFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
}

static void MaybePopPromisePrototypeFuses(JSContext* cx, NativeObject* obj,
                                          jsid id) {
  if (obj != obj->global().maybeGetPrototype(JSProto_Promise)) {
    return;
  }
  if (id.isAtom(cx->names().constructor) || id.isAtom(cx->names().then)) {
    obj->realm()->realmFuses.optimizePromiseLookupFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
}

static void MaybePopRegExpPrototypeFuses(JSContext* cx, NativeObject* obj,
                                         jsid id) {
  if (obj != obj->global().maybeGetPrototype(JSProto_RegExp)) {
    return;
  }
  if (id.isAtom(cx->names().flags) || id.isAtom(cx->names().global) ||
      id.isAtom(cx->names().hasIndices) || id.isAtom(cx->names().ignoreCase) ||
      id.isAtom(cx->names().multiline) || id.isAtom(cx->names().sticky) ||
      id.isAtom(cx->names().unicode) || id.isAtom(cx->names().unicodeSets) ||
      id.isAtom(cx->names().dotAll) || id.isAtom(cx->names().exec) ||
      id.isWellKnownSymbol(JS::SymbolCode::match) ||
      id.isWellKnownSymbol(JS::SymbolCode::matchAll) ||
      id.isWellKnownSymbol(JS::SymbolCode::replace) ||
      id.isWellKnownSymbol(JS::SymbolCode::search) ||
      id.isWellKnownSymbol(JS::SymbolCode::split)) {
    obj->realm()->realmFuses.optimizeRegExpPrototypeFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
}

static void MaybePopArrayBufferConstructorFuses(JSContext* cx,
                                                NativeObject* obj, jsid id) {
  if (obj != obj->global().maybeGetConstructor(JSProto_ArrayBuffer)) {
    return;
  }
  if (id.isWellKnownSymbol(JS::SymbolCode::species)) {
    obj->realm()->realmFuses.optimizeArrayBufferSpeciesFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
}

static void MaybePopArrayBufferPrototypeFuses(JSContext* cx, NativeObject* obj,
                                              jsid id) {
  if (obj != obj->global().maybeGetPrototype(JSProto_ArrayBuffer)) {
    return;
  }
  if (id.isAtom(cx->names().constructor)) {
    obj->realm()->realmFuses.optimizeArrayBufferSpeciesFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
}

static void MaybePopSharedArrayBufferConstructorFuses(JSContext* cx,
                                                      NativeObject* obj,
                                                      jsid id) {
  if (obj != obj->global().maybeGetConstructor(JSProto_SharedArrayBuffer)) {
    return;
  }
  if (id.isWellKnownSymbol(JS::SymbolCode::species)) {
    obj->realm()->realmFuses.optimizeSharedArrayBufferSpeciesFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
}

static void MaybePopSharedArrayBufferPrototypeFuses(JSContext* cx,
                                                    NativeObject* obj,
                                                    jsid id) {
  if (obj != obj->global().maybeGetPrototype(JSProto_SharedArrayBuffer)) {
    return;
  }
  if (id.isAtom(cx->names().constructor)) {
    obj->realm()->realmFuses.optimizeSharedArrayBufferSpeciesFuse.popFuse(
        cx, obj->realm()->realmFuses);
  }
}

static void MaybePopFuses(JSContext* cx, NativeObject* obj, jsid id) {
  // Handle writes to Array constructor fuse properties.
  MaybePopArrayConstructorFuses(cx, obj, id);

  // Handle writes to Array.prototype fuse properties.
  MaybePopArrayPrototypeFuses(cx, obj, id);

  // Handle writes to %ArrayIteratorPrototype% fuse properties.
  MaybePopArrayIteratorPrototypeFuses(cx, obj, id);

  // Handle writes to Map.prototype fuse properties.
  MaybePopMapPrototypeFuses(cx, obj, id);

  // Handle writes to %MapIteratorPrototype% fuse properties.
  MaybePopMapIteratorPrototypeFuses(cx, obj, id);

  // Handle writes to Set.prototype fuse properties.
  MaybePopSetPrototypeFuses(cx, obj, id);

  // Handle writes to %SetIteratorPrototype% fuse properties.
  MaybePopSetIteratorPrototypeFuses(cx, obj, id);

  // Handle writes to WeakMap.prototype fuse properties.
  MaybePopWeakMapPrototypeFuses(cx, obj, id);

  // Handle writes to WeakSet.prototype fuse properties.
  MaybePopWeakSetPrototypeFuses(cx, obj, id);

  // Handle writes to Promise constructor fuse properties.
  MaybePopPromiseConstructorFuses(cx, obj, id);

  // Handle writes to Promise.prototype fuse properties.
  MaybePopPromisePrototypeFuses(cx, obj, id);

  // Handle writes to RegExp.prototype fuse properties.
  MaybePopRegExpPrototypeFuses(cx, obj, id);

  // Handle writes to ArrayBuffer constructor fuse properties.
  MaybePopArrayBufferConstructorFuses(cx, obj, id);

  // Handle writes to ArrayBuffer.prototype fuse properties.
  MaybePopArrayBufferPrototypeFuses(cx, obj, id);

  // Handle writes to SharedArrayBuffer constructor fuse properties.
  MaybePopSharedArrayBufferConstructorFuses(cx, obj, id);

  // Handle writes to SharedArrayBuffer.prototype fuse properties.
  MaybePopSharedArrayBufferPrototypeFuses(cx, obj, id);
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

  if (MOZ_UNLIKELY(obj->hasFuseProperty())) {
    MaybePopFuses(cx, obj, id);
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
bool Watchtower::watchPropertyFlagsChangeSlow(JSContext* cx,
                                              Handle<NativeObject*> obj,
                                              HandleId id,
                                              PropertyInfo propInfo,
                                              PropertyFlags newFlags) {
  MOZ_ASSERT(watchesPropertyFlagsChange(obj));
  MOZ_ASSERT(obj->lookupPure(id).ref() == propInfo);
  MOZ_ASSERT(propInfo.flags() != newFlags);

  if (obj->isUsedAsPrototype() && !id.isInt()) {
    InvalidateMegamorphicCache(cx, obj);
  }

  if (obj->isGenerationCountedGlobal()) {
    // The global generation counter only cares whether a property
    // changes from data property to accessor or vice-versa. Changing
    // the flags on a property doesn't matter.
    bool wasAccessor = propInfo.isAccessorProperty();
    bool isAccessor = newFlags.isAccessorProperty();
    if (wasAccessor != isAccessor) {
      obj->as<GlobalObject>().bumpGenerationCount();
    }
  }

  if (MOZ_UNLIKELY(obj->useWatchtowerTestingLog())) {
    RootedValue val(cx, IdToValue(id));
    if (!AddToWatchtowerLog(cx, "change-prop-flags", obj, val)) {
      return false;
    }
  }

  return true;
}

// static
template <AllowGC allowGC>
void Watchtower::watchPropertyValueChangeSlow(
    JSContext* cx, typename MaybeRooted<NativeObject*, allowGC>::HandleType obj,
    typename MaybeRooted<PropertyKey, allowGC>::HandleType id,
    typename MaybeRooted<Value, allowGC>::HandleType value,
    PropertyInfo propInfo) {
  MOZ_ASSERT(watchesPropertyValueChange(obj));

  // Note: this is also called when changing the GetterSetter value of an
  // accessor property or when redefining a data property as an accessor
  // property and vice versa.

  if (propInfo.hasSlot() && obj->getSlot(propInfo.slot()) == value) {
    // We're not actually changing the property's value.
    return;
  }

  if (MOZ_UNLIKELY(obj->hasFuseProperty())) {
    MaybePopFuses(cx, obj, id);
  }

  // If we cannot GC, we can't manipulate the log, but we need to be able to
  // call this in places we cannot GC.
  if constexpr (allowGC == AllowGC::CanGC) {
    if (MOZ_UNLIKELY(obj->useWatchtowerTestingLog())) {
      RootedValue val(cx, IdToValue(id));
      if (!AddToWatchtowerLog(cx, "change-prop-value", obj, val)) {
        // Ignore OOM because this is just a testing feature and infallible
        // watchPropertyValueChange simplifies the callers.
        cx->clearPendingException();
      }
    }
  }
}

template void Watchtower::watchPropertyValueChangeSlow<AllowGC::CanGC>(
    JSContext* cx,
    typename MaybeRooted<NativeObject*, AllowGC::CanGC>::HandleType obj,
    typename MaybeRooted<PropertyKey, AllowGC::CanGC>::HandleType id,
    typename MaybeRooted<Value, AllowGC::CanGC>::HandleType value,
    PropertyInfo propInfo);
template void Watchtower::watchPropertyValueChangeSlow<AllowGC::NoGC>(
    JSContext* cx,
    typename MaybeRooted<NativeObject*, AllowGC::NoGC>::HandleType obj,
    typename MaybeRooted<PropertyKey, AllowGC::NoGC>::HandleType id,
    typename MaybeRooted<Value, AllowGC::NoGC>::HandleType value,
    PropertyInfo propInfo);

// static
bool Watchtower::watchFreezeOrSealSlow(JSContext* cx, Handle<NativeObject*> obj,
                                       IntegrityLevel level) {
  MOZ_ASSERT(watchesFreezeOrSeal(obj));

  // Invalidate the megamorphic set-property cache when freezing a prototype
  // object. Non-writable prototype properties can't be shadowed (through
  // SetProp) so this affects the behavior of add-property cache entries.
  if (level == IntegrityLevel::Frozen && obj->isUsedAsPrototype()) {
    InvalidateMegamorphicCache(cx, obj, /* invalidateGetPropCache = */ false);
  }

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
