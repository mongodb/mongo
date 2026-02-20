/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Barrier.h"

#include "gc/Marking.h"
#include "jit/JitContext.h"
#include "js/HashTable.h"
#include "js/shadow/Zone.h"  // JS::shadow::Zone
#include "js/Value.h"
#include "vm/BigIntType.h"  // JS::BigInt
#include "vm/EnvironmentObject.h"
#include "vm/GeneratorObject.h"
#include "vm/JSObject.h"
#include "vm/PropMap.h"
#include "wasm/WasmJS.h"

#include "gc/StableCellHasher-inl.h"

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

  if (!obj->zone()->isGCPreparing()) {
    AssertTargetIsNotGray(obj);
  }
}

bool CurrentThreadIsBaselineCompiling() {
  jit::JitContext* jcx = jit::MaybeGetJitContext();
  return jcx && jcx->inBaselineBackend();
}

bool CurrentThreadIsIonCompiling() {
  jit::JitContext* jcx = jit::MaybeGetJitContext();
  return jcx && jcx->inIonBackend();
}

bool CurrentThreadIsOffThreadCompiling() {
  return CurrentThreadIsBaselineCompiling() || CurrentThreadIsIonCompiling();
}

#endif  // DEBUG

#if !MOZ_IS_GCC
template struct JS_PUBLIC_API StableCellHasher<JSObject*>;
template struct JS_PUBLIC_API StableCellHasher<JSScript*>;
#endif

}  // namespace js

JS_PUBLIC_API void JS::HeapObjectPostWriteBarrier(JSObject** objp,
                                                  JSObject* prev,
                                                  JSObject* next) {
  MOZ_ASSERT(objp);
  js::InternalBarrierMethods<JSObject*>::postBarrier(objp, prev, next);
}

// Combined pre- and post-write barriers, used by the C++ Heap<T>
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
