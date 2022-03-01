/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Barrier.h"

#include "gc/Policy.h"
#include "jit/Ion.h"
#include "js/HashTable.h"
#include "js/shadow/Zone.h"  // JS::shadow::Zone
#include "js/Value.h"
#include "vm/BigIntType.h"  // JS::BigInt
#include "vm/EnvironmentObject.h"
#include "vm/GeneratorObject.h"
#include "vm/GetterSetter.h"
#include "vm/JSObject.h"
#include "vm/PropMap.h"
#include "vm/Realm.h"
#include "vm/SharedArrayObject.h"
#include "vm/SymbolType.h"
#include "wasm/WasmJS.h"

#include "gc/Zone-inl.h"

namespace js {

bool RuntimeFromMainThreadIsHeapMajorCollecting(JS::shadow::Zone* shadowZone) {
  MOZ_ASSERT(
      CurrentThreadCanAccessRuntime(shadowZone->runtimeFromMainThread()));
  return JS::RuntimeHeapIsMajorCollecting();
}

#ifdef DEBUG

bool IsMarkedBlack(JSObject* obj) { return obj->isMarkedBlack(); }

bool HeapSlot::preconditionForSet(NativeObject* owner, Kind kind,
                                  uint32_t slot) const {
  if (kind == Slot) {
    return &owner->getSlotRef(slot) == this;
  }

  uint32_t numShifted = owner->getElementsHeader()->numShiftedElements();
  MOZ_ASSERT(slot >= numShifted);
  return &owner->getDenseElement(slot - numShifted) == (const Value*)this;
}

void HeapSlot::assertPreconditionForPostWriteBarrier(
    NativeObject* obj, Kind kind, uint32_t slot, const Value& target) const {
  if (kind == Slot) {
    MOZ_ASSERT(obj->getSlotAddressUnchecked(slot)->get() == target);
  } else {
    uint32_t numShifted = obj->getElementsHeader()->numShiftedElements();
    MOZ_ASSERT(slot >= numShifted);
    MOZ_ASSERT(
        static_cast<HeapSlot*>(obj->getDenseElements() + (slot - numShifted))
            ->get() == target);
  }

  AssertTargetIsNotGray(obj);
}

bool CurrentThreadIsIonCompiling() {
  jit::JitContext* jcx = jit::MaybeGetJitContext();
  return jcx && jcx->inIonBackend();
}

bool CurrentThreadIsGCMarking() {
  JSContext* cx = MaybeGetJSContext();
  return cx && cx->gcUse == JSContext::GCUse::Marking;
}

bool CurrentThreadIsGCSweeping() {
  JSContext* cx = MaybeGetJSContext();
  return cx && cx->gcUse == JSContext::GCUse::Sweeping;
}

bool CurrentThreadIsGCFinalizing() {
  JSContext* cx = MaybeGetJSContext();
  return cx && cx->gcUse == JSContext::GCUse::Finalizing;
}

bool CurrentThreadIsTouchingGrayThings() {
  JSContext* cx = MaybeGetJSContext();
  return cx && cx->isTouchingGrayThings;
}

AutoTouchingGrayThings::AutoTouchingGrayThings() {
  TlsContext.get()->isTouchingGrayThings++;
}

AutoTouchingGrayThings::~AutoTouchingGrayThings() {
  JSContext* cx = TlsContext.get();
  MOZ_ASSERT(cx->isTouchingGrayThings);
  cx->isTouchingGrayThings--;
}

#endif  // DEBUG

// Tagged pointer barriers
//
// It's tempting to use ApplyGCThingTyped to dispatch to the typed barrier
// functions (e.g. gc::ReadBarrier(JSObject*)) but this does not compile well
// (clang generates 1580 bytes on x64 versus 296 bytes for this implementation
// of ValueReadBarrier).
//
// Instead, check known special cases and call the generic barrier functions.

static MOZ_ALWAYS_INLINE bool ValueIsPermanent(const Value& value) {
  gc::Cell* cell = value.toGCThing();

  if (value.isString()) {
    return cell->as<JSString>()->isPermanentAndMayBeShared();
  }

  if (value.isSymbol()) {
    return cell->as<JS::Symbol>()->isPermanentAndMayBeShared();
  }

#ifdef DEBUG
  // Using mozilla::DebugOnly here still generated code in opt builds.
  bool isPermanent = MapGCThingTyped(value, [](auto t) {
                       return t->isPermanentAndMayBeShared();
                     }).value();
  MOZ_ASSERT(!isPermanent);
#endif

  return false;
}

void gc::ValueReadBarrier(const Value& v) {
  MOZ_ASSERT(v.isGCThing());

  if (!ValueIsPermanent(v)) {
    ReadBarrierImpl(v.toGCThing());
  }
}

void gc::ValuePreWriteBarrier(const Value& v) {
  MOZ_ASSERT(v.isGCThing());

  if (!ValueIsPermanent(v)) {
    PreWriteBarrierImpl(v.toGCThing());
  }
}

static MOZ_ALWAYS_INLINE bool IdIsPermanent(jsid id) {
  gc::Cell* cell = id.toGCThing();

  if (id.isString()) {
    return cell->as<JSString>()->isPermanentAndMayBeShared();
  }

  if (id.isSymbol()) {
    return cell->as<JS::Symbol>()->isPermanentAndMayBeShared();
  }

#ifdef DEBUG
  bool isPermanent = MapGCThingTyped(id, [](auto t) {
                       return t->isPermanentAndMayBeShared();
                     }).value();
  MOZ_ASSERT(!isPermanent);
#endif

  return false;
}

void gc::IdPreWriteBarrier(jsid id) {
  MOZ_ASSERT(id.isGCThing());

  if (!IdIsPermanent(id)) {
    PreWriteBarrierImpl(&id.toGCThing()->asTenured());
  }
}

static MOZ_ALWAYS_INLINE bool CellPtrIsPermanent(JS::GCCellPtr thing) {
  if (thing.mayBeOwnedByOtherRuntime()) {
    return true;
  }

#ifdef DEBUG
  bool isPermanent = MapGCThingTyped(
      thing, [](auto t) { return t->isPermanentAndMayBeShared(); });
  MOZ_ASSERT(!isPermanent);
#endif

  return false;
}

void gc::CellPtrPreWriteBarrier(JS::GCCellPtr thing) {
  MOZ_ASSERT(thing);

  if (!CellPtrIsPermanent(thing)) {
    PreWriteBarrierImpl(thing.asCell());
  }
}

template <typename T>
/* static */ bool MovableCellHasher<T>::hasHash(const Lookup& l) {
  if (!l) {
    return true;
  }

  return l->zoneFromAnyThread()->hasUniqueId(l);
}

template <typename T>
/* static */ bool MovableCellHasher<T>::ensureHash(const Lookup& l) {
  if (!l) {
    return true;
  }

  uint64_t unusedId;
  return l->zoneFromAnyThread()->getOrCreateUniqueId(l, &unusedId);
}

template <typename T>
/* static */ HashNumber MovableCellHasher<T>::hash(const Lookup& l) {
  if (!l) {
    return 0;
  }

  // We have to access the zone from-any-thread here: a worker thread may be
  // cloning a self-hosted object from the main runtime's self- hosting zone
  // into another runtime. The zone's uid lock will protect against multiple
  // workers doing this simultaneously.
  MOZ_ASSERT(CurrentThreadCanAccessZone(l->zoneFromAnyThread()) ||
             l->zoneFromAnyThread()->isSelfHostingZone() ||
             CurrentThreadIsPerformingGC());

  return l->zoneFromAnyThread()->getHashCodeInfallible(l);
}

template <typename T>
/* static */ bool MovableCellHasher<T>::match(const Key& k, const Lookup& l) {
  // Return true if both are null or false if only one is null.
  if (!k) {
    return !l;
  }
  if (!l) {
    return false;
  }

  MOZ_ASSERT(k);
  MOZ_ASSERT(l);
  MOZ_ASSERT(CurrentThreadCanAccessZone(l->zoneFromAnyThread()) ||
             l->zoneFromAnyThread()->isSelfHostingZone());

  Zone* zone = k->zoneFromAnyThread();
  if (zone != l->zoneFromAnyThread()) {
    return false;
  }

#ifdef DEBUG
  // Incremental table sweeping means that existing table entries may no
  // longer have unique IDs. We fail the match in that case and the entry is
  // removed from the table later on.
  if (!zone->hasUniqueId(k)) {
    Key key = k;
    MOZ_ASSERT(IsAboutToBeFinalizedUnbarriered(&key));
  }
  MOZ_ASSERT(zone->hasUniqueId(l));
#endif

  uint64_t keyId;
  if (!zone->maybeGetUniqueId(k, &keyId)) {
    // Key is dead and cannot match lookup which must be live.
    return false;
  }

  return keyId == zone->getUniqueIdInfallible(l);
}

#if !MOZ_IS_GCC
template struct JS_PUBLIC_API MovableCellHasher<JSObject*>;
#endif

template struct JS_PUBLIC_API MovableCellHasher<AbstractGeneratorObject*>;
template struct JS_PUBLIC_API MovableCellHasher<EnvironmentObject*>;
template struct JS_PUBLIC_API MovableCellHasher<GlobalObject*>;
template struct JS_PUBLIC_API MovableCellHasher<JSScript*>;
template struct JS_PUBLIC_API MovableCellHasher<BaseScript*>;
template struct JS_PUBLIC_API MovableCellHasher<PropMap*>;
template struct JS_PUBLIC_API MovableCellHasher<ScriptSourceObject*>;
template struct JS_PUBLIC_API MovableCellHasher<SavedFrame*>;
template struct JS_PUBLIC_API MovableCellHasher<WasmInstanceObject*>;

}  // namespace js

// Post-write barrier, used by the C++ Heap<T> implementation.

JS_PUBLIC_API void JS::HeapObjectPostWriteBarrier(JSObject** objp,
                                                  JSObject* prev,
                                                  JSObject* next) {
  MOZ_ASSERT(objp);
  js::InternalBarrierMethods<JSObject*>::postBarrier(objp, prev, next);
}

JS_PUBLIC_API void JS::HeapStringPostWriteBarrier(JSString** strp,
                                                  JSString* prev,
                                                  JSString* next) {
  MOZ_ASSERT(strp);
  js::InternalBarrierMethods<JSString*>::postBarrier(strp, prev, next);
}

JS_PUBLIC_API void JS::HeapBigIntPostWriteBarrier(JS::BigInt** bip,
                                                  JS::BigInt* prev,
                                                  JS::BigInt* next) {
  MOZ_ASSERT(bip);
  js::InternalBarrierMethods<JS::BigInt*>::postBarrier(bip, prev, next);
}

JS_PUBLIC_API void JS::HeapValuePostWriteBarrier(JS::Value* valuep,
                                                 const Value& prev,
                                                 const Value& next) {
  MOZ_ASSERT(valuep);
  js::InternalBarrierMethods<JS::Value>::postBarrier(valuep, prev, next);
}

// Combined pre- and post-write barriers, used by the rust Heap<T>
// implementation.

JS_PUBLIC_API void JS::HeapObjectWriteBarriers(JSObject** objp, JSObject* prev,
                                               JSObject* next) {
  MOZ_ASSERT(objp);
  js::InternalBarrierMethods<JSObject*>::preBarrier(prev);
  js::InternalBarrierMethods<JSObject*>::postBarrier(objp, prev, next);
}

JS_PUBLIC_API void JS::HeapStringWriteBarriers(JSString** strp, JSString* prev,
                                               JSString* next) {
  MOZ_ASSERT(strp);
  js::InternalBarrierMethods<JSString*>::preBarrier(prev);
  js::InternalBarrierMethods<JSString*>::postBarrier(strp, prev, next);
}

JS_PUBLIC_API void JS::HeapBigIntWriteBarriers(JS::BigInt** bip,
                                               JS::BigInt* prev,
                                               JS::BigInt* next) {
  MOZ_ASSERT(bip);
  js::InternalBarrierMethods<JS::BigInt*>::preBarrier(prev);
  js::InternalBarrierMethods<JS::BigInt*>::postBarrier(bip, prev, next);
}

JS_PUBLIC_API void JS::HeapScriptWriteBarriers(JSScript** scriptp,
                                               JSScript* prev, JSScript* next) {
  MOZ_ASSERT(scriptp);
  js::InternalBarrierMethods<JSScript*>::preBarrier(prev);
  js::InternalBarrierMethods<JSScript*>::postBarrier(scriptp, prev, next);
}

JS_PUBLIC_API void JS::HeapValueWriteBarriers(JS::Value* valuep,
                                              const Value& prev,
                                              const Value& next) {
  MOZ_ASSERT(valuep);
  js::InternalBarrierMethods<JS::Value>::preBarrier(prev);
  js::InternalBarrierMethods<JS::Value>::postBarrier(valuep, prev, next);
}
