/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/PIC.h"
#include "jscntxt.h"
#include "jscompartment.h"
#include "jsobj.h"
#include "gc/Marking.h"

#include "vm/GlobalObject.h"
#include "vm/SelfHosting.h"

#include "jsobjinlines.h"
#include "vm/NativeObject-inl.h"

using namespace js;
using namespace js::gc;

bool
js::ForOfPIC::Chain::initialize(JSContext* cx)
{
    MOZ_ASSERT(!initialized_);

    // Get the canonical Array.prototype
    RootedNativeObject arrayProto(cx, GlobalObject::getOrCreateArrayPrototype(cx, cx->global()));
    if (!arrayProto)
        return false;

    // Get the canonical ArrayIterator.prototype
    RootedNativeObject arrayIteratorProto(cx,
        GlobalObject::getOrCreateArrayIteratorPrototype(cx, cx->global()));
    if (!arrayIteratorProto)
        return false;

    // From this point on, we can't fail.  Set initialized and fill the fields
    // for the canonical Array.prototype and ArrayIterator.prototype objects.
    initialized_ = true;
    arrayProto_ = arrayProto;
    arrayIteratorProto_ = arrayIteratorProto;

    // Shortcut returns below means Array for-of will never be optimizable,
    // do set disabled_ now, and clear it later when we succeed.
    disabled_ = true;

    // Look up Array.prototype[@@iterator], ensure it's a slotful shape.
    Shape* iterShape = arrayProto->lookup(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().iterator));
    if (!iterShape || !iterShape->hasSlot() || !iterShape->hasDefaultGetter())
        return true;

    // Get the referred value, and ensure it holds the canonical ArrayValues function.
    Value iterator = arrayProto->getSlot(iterShape->slot());
    JSFunction* iterFun;
    if (!IsFunctionObject(iterator, &iterFun))
        return true;
    if (!IsSelfHostedFunctionWithName(iterFun, cx->names().ArrayValues))
        return true;

    // Look up the 'next' value on ArrayIterator.prototype
    Shape* nextShape = arrayIteratorProto->lookup(cx, cx->names().next);
    if (!nextShape || !nextShape->hasSlot())
        return true;

    // Get the referred value, ensure it holds the canonical ArrayIteratorNext function.
    Value next = arrayIteratorProto->getSlot(nextShape->slot());
    JSFunction* nextFun;
    if (!IsFunctionObject(next, &nextFun))
        return true;
    if (!IsSelfHostedFunctionWithName(nextFun, cx->names().ArrayIteratorNext))
        return true;

    disabled_ = false;
    arrayProtoShape_ = arrayProto->lastProperty();
    arrayProtoIteratorSlot_ = iterShape->slot();
    canonicalIteratorFunc_ = iterator;
    arrayIteratorProtoShape_ = arrayIteratorProto->lastProperty();
    arrayIteratorProtoNextSlot_ = nextShape->slot();
    canonicalNextFunc_ = next;
    return true;
}

js::ForOfPIC::Stub*
js::ForOfPIC::Chain::isArrayOptimized(ArrayObject* obj)
{
    Stub* stub = getMatchingStub(obj);
    if (!stub)
        return nullptr;

    // Ensure that this is an otherwise optimizable array.
    if (!isOptimizableArray(obj))
        return nullptr;

    // Not yet enough!  Ensure that the world as we know it remains sane.
    if (!isArrayStateStillSane())
        return nullptr;

    return stub;
}

bool
js::ForOfPIC::Chain::tryOptimizeArray(JSContext* cx, HandleArrayObject array, bool* optimized)
{
    MOZ_ASSERT(optimized);

    *optimized = false;

    if (!initialized_) {
        // If PIC is not initialized, initialize it.
        if (!initialize(cx))
            return false;

    } else if (!disabled_ && !isArrayStateStillSane()) {
        // Otherwise, if array state is no longer sane, reinitialize.
        reset(cx);

        if (!initialize(cx))
            return false;
    }
    MOZ_ASSERT(initialized_);

    // If PIC is disabled, don't bother trying to optimize.
    if (disabled_)
        return true;

    // By the time we get here, we should have a sane array state to work with.
    MOZ_ASSERT(isArrayStateStillSane());

    // Check if stub already exists.
    ForOfPIC::Stub* stub = isArrayOptimized(&array->as<ArrayObject>());
    if (stub) {
        *optimized = true;
        return true;
    }

    // If the number of stubs is about to exceed the limit, throw away entire
    // existing cache before adding new stubs.  We shouldn't really have heavy
    // churn on these.
    if (numStubs() >= MAX_STUBS)
        eraseChain();

    // Ensure array's prototype is the actual Array.prototype
    if (!isOptimizableArray(array))
        return true;

    // Ensure array doesn't define @@iterator directly.
    if (array->lookup(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().iterator)))
        return true;

    // Good to optimize now, create stub to add.
    RootedShape shape(cx, array->lastProperty());
    stub = cx->new_<Stub>(shape);
    if (!stub)
        return false;

    // Add the stub.
    addStub(stub);

    *optimized = true;
    return true;
}

js::ForOfPIC::Stub*
js::ForOfPIC::Chain::getMatchingStub(JSObject* obj)
{
    // Ensure PIC is initialized and not disabled.
    if (!initialized_ || disabled_)
        return nullptr;

    // Check if there is a matching stub.
    for (Stub* stub = stubs(); stub != nullptr; stub = stub->next()) {
        if (stub->shape() == obj->maybeShape())
            return stub;
    }

    return nullptr;
}

bool
js::ForOfPIC::Chain::isOptimizableArray(JSObject* obj)
{
    MOZ_ASSERT(obj->is<ArrayObject>());

    // Ensure object's prototype is the actual Array.prototype
    if (!obj->getTaggedProto().isObject())
        return false;
    if (obj->getTaggedProto().toObject() != arrayProto_)
        return false;

    return true;
}

bool
js::ForOfPIC::Chain::isArrayStateStillSane()
{
    // Ensure that canonical Array.prototype has matching shape.
    if (arrayProto_->lastProperty() != arrayProtoShape_)
        return false;

    // Ensure that Array.prototype[@@iterator] contains the
    // canonical iterator function.
    if (arrayProto_->getSlot(arrayProtoIteratorSlot_) != canonicalIteratorFunc_)
        return false;

    // Chain to isArrayNextStillSane.
    return isArrayNextStillSane();
}

void
js::ForOfPIC::Chain::reset(JSContext* cx)
{
    // Should never reset a disabled_ stub.
    MOZ_ASSERT(!disabled_);

    // Erase the chain.
    eraseChain();

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

void
js::ForOfPIC::Chain::eraseChain()
{
    // Should never need to clear the chain of a disabled stub.
    MOZ_ASSERT(!disabled_);

    // Free all stubs.
    Stub* stub = stubs_;
    while (stub) {
        Stub* next = stub->next();
        js_delete(stub);
        stub = next;
    }
    stubs_ = nullptr;
}


// Trace the pointers stored directly on the stub.
void
js::ForOfPIC::Chain::mark(JSTracer* trc)
{
    if (!initialized_ || disabled_)
        return;

    TraceEdge(trc, &arrayProto_, "ForOfPIC Array.prototype.");
    TraceEdge(trc, &arrayIteratorProto_, "ForOfPIC ArrayIterator.prototype.");

    TraceEdge(trc, &arrayProtoShape_, "ForOfPIC Array.prototype shape.");
    TraceEdge(trc, &arrayIteratorProtoShape_, "ForOfPIC ArrayIterator.prototype shape.");

    TraceEdge(trc, &canonicalIteratorFunc_, "ForOfPIC ArrayValues builtin.");
    TraceEdge(trc, &canonicalNextFunc_, "ForOfPIC ArrayIterator.prototype.next builtin.");

    // Free all the stubs in the chain.
    while (stubs_)
        removeStub(stubs_, nullptr);
}

void
js::ForOfPIC::Chain::sweep(FreeOp* fop)
{
    // Free all the stubs in the chain.
    while (stubs_) {
        Stub* next = stubs_->next();
        fop->delete_(stubs_);
        stubs_ = next;
    }
    fop->delete_(this);
}

static void
ForOfPIC_finalize(FreeOp* fop, JSObject* obj)
{
    if (ForOfPIC::Chain* chain = ForOfPIC::fromJSObject(&obj->as<NativeObject>()))
        chain->sweep(fop);
}

static void
ForOfPIC_traceObject(JSTracer* trc, JSObject* obj)
{
    if (ForOfPIC::Chain* chain = ForOfPIC::fromJSObject(&obj->as<NativeObject>()))
        chain->mark(trc);
}

const Class ForOfPIC::jsclass = {
    "ForOfPIC", JSCLASS_HAS_PRIVATE,
    nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, ForOfPIC_finalize,
    nullptr,              /* call        */
    nullptr,              /* hasInstance */
    nullptr,              /* construct   */
    ForOfPIC_traceObject
};

/* static */ NativeObject*
js::ForOfPIC::createForOfPICObject(JSContext* cx, Handle<GlobalObject*> global)
{
    assertSameCompartment(cx, global);
    NativeObject* obj = NewNativeObjectWithGivenProto(cx, &ForOfPIC::jsclass, nullptr);
    if (!obj)
        return nullptr;
    ForOfPIC::Chain* chain = cx->new_<ForOfPIC::Chain>();
    if (!chain)
        return nullptr;
    obj->setPrivate(chain);
    return obj;
}

/* static */ js::ForOfPIC::Chain*
js::ForOfPIC::create(JSContext* cx)
{
    MOZ_ASSERT(!cx->global()->getForOfPICObject());
    Rooted<GlobalObject*> global(cx, cx->global());
    NativeObject* obj = GlobalObject::getOrCreateForOfPICObject(cx, global);
    if (!obj)
        return nullptr;
    return fromJSObject(obj);
}
