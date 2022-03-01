/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/PIC.h"

#include "gc/FreeOp.h"
#include "gc/Marking.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/Realm.h"
#include "vm/SelfHosting.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

template <typename Category>
void PICChain<Category>::addStub(JSObject* obj, CatStub* stub) {
  MOZ_ASSERT(stub);
  MOZ_ASSERT(!stub->next());

  AddCellMemory(obj, sizeof(CatStub), MemoryUse::ForOfPICStub);

  if (!stubs_) {
    stubs_ = stub;
    return;
  }

  CatStub* cur = stubs_;
  while (cur->next()) {
    cur = cur->next();
  }
  cur->append(stub);
}

bool js::ForOfPIC::Chain::initialize(JSContext* cx) {
  MOZ_ASSERT(!initialized_);

  // Get the canonical Array.prototype
  RootedNativeObject arrayProto(
      cx, GlobalObject::getOrCreateArrayPrototype(cx, cx->global()));
  if (!arrayProto) {
    return false;
  }

  // Get the canonical ArrayIterator.prototype
  RootedNativeObject arrayIteratorProto(
      cx, GlobalObject::getOrCreateArrayIteratorPrototype(cx, cx->global()));
  if (!arrayIteratorProto) {
    return false;
  }

  // From this point on, we can't fail.  Set initialized and fill the fields
  // for the canonical Array.prototype and ArrayIterator.prototype objects.
  initialized_ = true;
  arrayProto_ = arrayProto;
  arrayIteratorProto_ = arrayIteratorProto;

  // Shortcut returns below means Array for-of will never be optimizable,
  // do set disabled_ now, and clear it later when we succeed.
  disabled_ = true;

  // Look up Array.prototype[@@iterator], ensure it's a slotful shape.
  mozilla::Maybe<PropertyInfo> iterProp =
      arrayProto->lookup(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().iterator));
  if (iterProp.isNothing() || !iterProp->isDataProperty()) {
    return true;
  }

  // Get the referred value, and ensure it holds the canonical ArrayValues
  // function.
  Value iterator = arrayProto->getSlot(iterProp->slot());
  JSFunction* iterFun;
  if (!IsFunctionObject(iterator, &iterFun)) {
    return true;
  }
  if (!IsSelfHostedFunctionWithName(iterFun, cx->names().ArrayValues)) {
    return true;
  }

  // Look up the 'next' value on ArrayIterator.prototype
  mozilla::Maybe<PropertyInfo> nextProp =
      arrayIteratorProto->lookup(cx, cx->names().next);
  if (nextProp.isNothing() || !nextProp->isDataProperty()) {
    return true;
  }

  // Get the referred value, ensure it holds the canonical ArrayIteratorNext
  // function.
  Value next = arrayIteratorProto->getSlot(nextProp->slot());
  JSFunction* nextFun;
  if (!IsFunctionObject(next, &nextFun)) {
    return true;
  }
  if (!IsSelfHostedFunctionWithName(nextFun, cx->names().ArrayIteratorNext)) {
    return true;
  }

  disabled_ = false;
  arrayProtoShape_ = arrayProto->shape();
  arrayProtoIteratorSlot_ = iterProp->slot();
  canonicalIteratorFunc_ = iterator;
  arrayIteratorProtoShape_ = arrayIteratorProto->shape();
  arrayIteratorProtoNextSlot_ = nextProp->slot();
  canonicalNextFunc_ = next;
  return true;
}

bool js::ForOfPIC::Chain::tryOptimizeArray(JSContext* cx,
                                           HandleArrayObject array,
                                           bool* optimized) {
  MOZ_ASSERT(optimized);

  *optimized = false;

  if (!initialized_) {
    // If PIC is not initialized, initialize it.
    if (!initialize(cx)) {
      return false;
    }

  } else if (!disabled_ && !isArrayStateStillSane()) {
    // Otherwise, if array state is no longer sane, reinitialize.
    reset(cx);

    if (!initialize(cx)) {
      return false;
    }
  }
  MOZ_ASSERT(initialized_);

  // If PIC is disabled, don't bother trying to optimize.
  if (disabled_) {
    return true;
  }

  // By the time we get here, we should have a sane array state to work with.
  MOZ_ASSERT(isArrayStateStillSane());

  // Ensure array's prototype is the actual Array.prototype
  if (array->staticPrototype() != arrayProto_) {
    return true;
  }

  // Check if stub already exists.
  if (hasMatchingStub(array)) {
    *optimized = true;
    return true;
  }

  // Ensure array doesn't define @@iterator directly.
  if (array->lookup(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().iterator))) {
    return true;
  }

  // If the number of stubs is about to exceed the limit, throw away entire
  // existing cache before adding new stubs.  We shouldn't really have heavy
  // churn on these.
  if (numStubs() >= MAX_STUBS) {
    eraseChain(cx);
  }

  // Good to optimize now, create stub to add.
  RootedShape shape(cx, array->shape());
  Stub* stub = cx->new_<Stub>(shape);
  if (!stub) {
    return false;
  }

  // Add the stub.
  addStub(picObject_, stub);

  *optimized = true;
  return true;
}

bool js::ForOfPIC::Chain::tryOptimizeArrayIteratorNext(JSContext* cx,
                                                       bool* optimized) {
  MOZ_ASSERT(optimized);

  *optimized = false;

  if (!initialized_) {
    // If PIC is not initialized, initialize it.
    if (!initialize(cx)) {
      return false;
    }
  } else if (!disabled_ && !isArrayNextStillSane()) {
    // Otherwise, if array iterator state is no longer sane, reinitialize.
    reset(cx);

    if (!initialize(cx)) {
      return false;
    }
  }
  MOZ_ASSERT(initialized_);

  // If PIC is disabled, don't bother trying to optimize.
  if (disabled_) {
    return true;
  }

  // By the time we get here, we should have a sane iterator state to work with.
  MOZ_ASSERT(isArrayNextStillSane());

  *optimized = true;
  return true;
}

bool js::ForOfPIC::Chain::hasMatchingStub(ArrayObject* obj) {
  // Ensure PIC is initialized and not disabled.
  MOZ_ASSERT(initialized_ && !disabled_);

  // Check if there is a matching stub.
  for (Stub* stub = stubs(); stub != nullptr; stub = stub->next()) {
    if (stub->shape() == obj->shape()) {
      return true;
    }
  }

  return false;
}

bool js::ForOfPIC::Chain::isArrayStateStillSane() {
  // Ensure that canonical Array.prototype has matching shape.
  if (arrayProto_->shape() != arrayProtoShape_) {
    return false;
  }

  // Ensure that Array.prototype[@@iterator] contains the
  // canonical iterator function.
  if (arrayProto_->getSlot(arrayProtoIteratorSlot_) != canonicalIteratorFunc_) {
    return false;
  }

  // Chain to isArrayNextStillSane.
  return isArrayNextStillSane();
}

void js::ForOfPIC::Chain::reset(JSContext* cx) {
  // Should never reset a disabled_ stub.
  MOZ_ASSERT(!disabled_);

  // Erase the chain.
  eraseChain(cx);

  arrayProto_ = nullptr;
  arrayIteratorProto_ = nullptr;

  arrayProtoShape_ = nullptr;
  arrayProtoIteratorSlot_ = -1;
  canonicalIteratorFunc_ = UndefinedValue();

  arrayIteratorProtoShape_ = nullptr;
  arrayIteratorProtoNextSlot_ = -1;
  canonicalNextFunc_ = UndefinedValue();

  initialized_ = false;
}

void js::ForOfPIC::Chain::eraseChain(JSContext* cx) {
  // Should never need to clear the chain of a disabled stub.
  MOZ_ASSERT(!disabled_);
  freeAllStubs(cx->defaultFreeOp());
}

// Trace the pointers stored directly on the stub.
void js::ForOfPIC::Chain::trace(JSTracer* trc) {
  TraceEdge(trc, &picObject_, "ForOfPIC object");

  if (!initialized_ || disabled_) {
    return;
  }

  TraceEdge(trc, &arrayProto_, "ForOfPIC Array.prototype.");
  TraceEdge(trc, &arrayIteratorProto_, "ForOfPIC ArrayIterator.prototype.");

  TraceEdge(trc, &arrayProtoShape_, "ForOfPIC Array.prototype shape.");
  TraceEdge(trc, &arrayIteratorProtoShape_,
            "ForOfPIC ArrayIterator.prototype shape.");

  TraceEdge(trc, &canonicalIteratorFunc_, "ForOfPIC ArrayValues builtin.");
  TraceEdge(trc, &canonicalNextFunc_,
            "ForOfPIC ArrayIterator.prototype.next builtin.");

  if (trc->isMarkingTracer()) {
    // Free all the stubs in the chain.
    freeAllStubs(trc->runtime()->defaultFreeOp());
  }
}

static void ForOfPIC_finalize(JSFreeOp* fop, JSObject* obj) {
  MOZ_ASSERT(fop->maybeOnHelperThread());
  if (ForOfPIC::Chain* chain =
          ForOfPIC::fromJSObject(&obj->as<NativeObject>())) {
    chain->finalize(fop, obj);
  }
}

void js::ForOfPIC::Chain::finalize(JSFreeOp* fop, JSObject* obj) {
  freeAllStubs(fop);
  fop->delete_(obj, this, MemoryUse::ForOfPIC);
}

void js::ForOfPIC::Chain::freeAllStubs(JSFreeOp* fop) {
  Stub* stub = stubs_;
  while (stub) {
    Stub* next = stub->next();
    fop->delete_(picObject_, stub, MemoryUse::ForOfPICStub);
    stub = next;
  }
  stubs_ = nullptr;
}

static void ForOfPIC_traceObject(JSTracer* trc, JSObject* obj) {
  if (ForOfPIC::Chain* chain =
          ForOfPIC::fromJSObject(&obj->as<NativeObject>())) {
    chain->trace(trc);
  }
}

static const JSClassOps ForOfPICClassOps = {
    nullptr,               // addProperty
    nullptr,               // delProperty
    nullptr,               // enumerate
    nullptr,               // newEnumerate
    nullptr,               // resolve
    nullptr,               // mayResolve
    ForOfPIC_finalize,     // finalize
    nullptr,               // call
    nullptr,               // hasInstance
    nullptr,               // construct
    ForOfPIC_traceObject,  // trace
};

const JSClass ForOfPICObject::class_ = {
    "ForOfPIC", JSCLASS_HAS_PRIVATE | JSCLASS_BACKGROUND_FINALIZE,
    &ForOfPICClassOps};

/* static */
NativeObject* js::ForOfPIC::createForOfPICObject(JSContext* cx,
                                                 Handle<GlobalObject*> global) {
  cx->check(global);
  ForOfPICObject* obj =
      NewTenuredObjectWithGivenProto<ForOfPICObject>(cx, nullptr);
  if (!obj) {
    return nullptr;
  }
  ForOfPIC::Chain* chain = cx->new_<ForOfPIC::Chain>(obj);
  if (!chain) {
    return nullptr;
  }
  InitObjectPrivate(obj, chain, MemoryUse::ForOfPIC);
  obj->setPrivate(chain);
  return obj;
}

/* static */ js::ForOfPIC::Chain* js::ForOfPIC::create(JSContext* cx) {
  MOZ_ASSERT(!cx->global()->getForOfPICObject());
  Rooted<GlobalObject*> global(cx, cx->global());
  NativeObject* obj = GlobalObject::getOrCreateForOfPICObject(cx, global);
  if (!obj) {
    return nullptr;
  }
  return fromJSObject(obj);
}
