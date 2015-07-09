/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/ScopeObject-inl.h"

#include "mozilla/PodOperations.h"

#include "jscompartment.h"
#include "jsiter.h"

#include "vm/ArgumentsObject.h"
#include "vm/GlobalObject.h"
#include "vm/ProxyObject.h"
#include "vm/Shape.h"
#include "vm/Xdr.h"

#include "jsatominlines.h"
#include "jsobjinlines.h"
#include "jsscriptinlines.h"

#include "vm/Stack-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::PodZero;

typedef Rooted<ArgumentsObject*> RootedArgumentsObject;
typedef MutableHandle<ArgumentsObject*> MutableHandleArgumentsObject;

/*****************************************************************************/

Shape*
js::ScopeCoordinateToStaticScopeShape(JSScript* script, jsbytecode* pc)
{
    MOZ_ASSERT(JOF_OPTYPE(JSOp(*pc)) == JOF_SCOPECOORD);
    StaticScopeIter<NoGC> ssi(script->innermostStaticScopeInScript(pc));
    uint32_t hops = ScopeCoordinate(pc).hops();
    while (true) {
        MOZ_ASSERT(!ssi.done());
        if (ssi.hasDynamicScopeObject()) {
            if (!hops)
                break;
            hops--;
        }
        ssi++;
    }
    return ssi.scopeShape();
}

static const uint32_t SCOPE_COORDINATE_NAME_THRESHOLD = 20;

void
ScopeCoordinateNameCache::purge()
{
    shape = nullptr;
    if (map.initialized())
        map.finish();
}

PropertyName*
js::ScopeCoordinateName(ScopeCoordinateNameCache& cache, JSScript* script, jsbytecode* pc)
{
    Shape* shape = ScopeCoordinateToStaticScopeShape(script, pc);
    if (shape != cache.shape && shape->slot() >= SCOPE_COORDINATE_NAME_THRESHOLD) {
        cache.purge();
        if (cache.map.init(shape->slot())) {
            cache.shape = shape;
            Shape::Range<NoGC> r(shape);
            while (!r.empty()) {
                if (!cache.map.putNew(r.front().slot(), r.front().propid())) {
                    cache.purge();
                    break;
                }
                r.popFront();
            }
        }
    }

    jsid id;
    ScopeCoordinate sc(pc);
    if (shape == cache.shape) {
        ScopeCoordinateNameCache::Map::Ptr p = cache.map.lookup(sc.slot());
        id = p->value();
    } else {
        Shape::Range<NoGC> r(shape);
        while (r.front().slot() != sc.slot())
            r.popFront();
        id = r.front().propidRaw();
    }

    /* Beware nameless destructuring formal. */
    if (!JSID_IS_ATOM(id))
        return script->runtimeFromAnyThread()->commonNames->empty;
    return JSID_TO_ATOM(id)->asPropertyName();
}

JSScript*
js::ScopeCoordinateFunctionScript(JSScript* script, jsbytecode* pc)
{
    MOZ_ASSERT(JOF_OPTYPE(JSOp(*pc)) == JOF_SCOPECOORD);
    StaticScopeIter<NoGC> ssi(script->innermostStaticScopeInScript(pc));
    uint32_t hops = ScopeCoordinate(pc).hops();
    while (true) {
        if (ssi.hasDynamicScopeObject()) {
            if (!hops)
                break;
            hops--;
        }
        ssi++;
    }
    if (ssi.type() != StaticScopeIter<NoGC>::Function)
        return nullptr;
    return ssi.funScript();
}

/*****************************************************************************/

void
ScopeObject::setEnclosingScope(HandleObject obj)
{
    MOZ_ASSERT_IF(obj->is<CallObject>() || obj->is<DeclEnvObject>() || obj->is<BlockObject>(),
                  obj->isDelegate());
    setFixedSlot(SCOPE_CHAIN_SLOT, ObjectValue(*obj));
}

CallObject*
CallObject::create(JSContext* cx, HandleShape shape, HandleObjectGroup group, uint32_t lexicalBegin)
{
    MOZ_ASSERT(!group->singleton(),
               "passed a singleton group to create() (use createSingleton() "
               "instead)");
    gc::AllocKind kind = gc::GetGCObjectKind(shape->numFixedSlots());
    MOZ_ASSERT(CanBeFinalizedInBackground(kind, &CallObject::class_));
    kind = gc::GetBackgroundAllocKind(kind);

    JSObject* obj = JSObject::create(cx, kind, gc::DefaultHeap, shape, group);
    if (!obj)
        return nullptr;

    obj->as<CallObject>().initRemainingSlotsToUninitializedLexicals(lexicalBegin);
    return &obj->as<CallObject>();
}

CallObject*
CallObject::createSingleton(JSContext* cx, HandleShape shape, uint32_t lexicalBegin)
{
    gc::AllocKind kind = gc::GetGCObjectKind(shape->numFixedSlots());
    MOZ_ASSERT(CanBeFinalizedInBackground(kind, &CallObject::class_));
    kind = gc::GetBackgroundAllocKind(kind);

    RootedObjectGroup group(cx, ObjectGroup::lazySingletonGroup(cx, &class_, TaggedProto(nullptr)));
    if (!group)
        return nullptr;
    RootedObject obj(cx, JSObject::create(cx, kind, gc::TenuredHeap, shape, group));
    if (!obj)
        return nullptr;

    MOZ_ASSERT(obj->isSingleton(),
               "group created inline above must be a singleton");

    obj->as<CallObject>().initRemainingSlotsToUninitializedLexicals(lexicalBegin);
    return &obj->as<CallObject>();
}

/*
 * Create a CallObject for a JSScript that is not initialized to any particular
 * callsite. This object can either be initialized (with an enclosing scope and
 * callee) or used as a template for jit compilation.
 */
CallObject*
CallObject::createTemplateObject(JSContext* cx, HandleScript script, gc::InitialHeap heap)
{
    RootedShape shape(cx, script->bindings.callObjShape());
    MOZ_ASSERT(shape->getObjectClass() == &class_);

    RootedObjectGroup group(cx, ObjectGroup::defaultNewGroup(cx, &class_, TaggedProto(nullptr)));
    if (!group)
        return nullptr;

    gc::AllocKind kind = gc::GetGCObjectKind(shape->numFixedSlots());
    MOZ_ASSERT(CanBeFinalizedInBackground(kind, &class_));
    kind = gc::GetBackgroundAllocKind(kind);

    JSObject* obj = JSObject::create(cx, kind, heap, shape, group);
    if (!obj)
        return nullptr;

    // Set uninitialized lexicals even on template objects, as Ion will use
    // copy over the template object's slot values in the fast path.
    obj->as<CallObject>().initAliasedLexicalsToThrowOnTouch(script);

    return &obj->as<CallObject>();
}

/*
 * Construct a call object for the given bindings.  If this is a call object
 * for a function invocation, callee should be the function being called.
 * Otherwise it must be a call object for eval of strict mode code, and callee
 * must be null.
 */
CallObject*
CallObject::create(JSContext* cx, HandleScript script, HandleObject enclosing, HandleFunction callee)
{
    gc::InitialHeap heap = script->treatAsRunOnce() ? gc::TenuredHeap : gc::DefaultHeap;
    CallObject* callobj = CallObject::createTemplateObject(cx, script, heap);
    if (!callobj)
        return nullptr;

    callobj->as<ScopeObject>().setEnclosingScope(enclosing);
    callobj->initFixedSlot(CALLEE_SLOT, ObjectOrNullValue(callee));

    if (script->treatAsRunOnce()) {
        Rooted<CallObject*> ncallobj(cx, callobj);
        if (!JSObject::setSingleton(cx, ncallobj))
            return nullptr;
        return ncallobj;
    }

    return callobj;
}

CallObject*
CallObject::createForFunction(JSContext* cx, HandleObject enclosing, HandleFunction callee)
{
    RootedObject scopeChain(cx, enclosing);
    MOZ_ASSERT(scopeChain);

    /*
     * For a named function expression Call's parent points to an environment
     * object holding function's name.
     */
    if (callee->isNamedLambda()) {
        scopeChain = DeclEnvObject::create(cx, scopeChain, callee);
        if (!scopeChain)
            return nullptr;
    }

    RootedScript script(cx, callee->nonLazyScript());
    return create(cx, script, scopeChain, callee);
}

CallObject*
CallObject::createForFunction(JSContext* cx, AbstractFramePtr frame)
{
    MOZ_ASSERT(frame.isNonEvalFunctionFrame());
    assertSameCompartment(cx, frame);

    RootedObject scopeChain(cx, frame.scopeChain());
    RootedFunction callee(cx, frame.callee());

    CallObject* callobj = createForFunction(cx, scopeChain, callee);
    if (!callobj)
        return nullptr;

    /* Copy in the closed-over formal arguments. */
    for (AliasedFormalIter i(frame.script()); i; i++) {
        callobj->setAliasedVar(cx, i, i->name(),
                               frame.unaliasedFormal(i.frameIndex(), DONT_CHECK_ALIASING));
    }

    return callobj;
}

CallObject*
CallObject::createForStrictEval(JSContext* cx, AbstractFramePtr frame)
{
    MOZ_ASSERT(frame.isStrictEvalFrame());
    MOZ_ASSERT_IF(frame.isInterpreterFrame(), cx->interpreterFrame() == frame.asInterpreterFrame());
    MOZ_ASSERT_IF(frame.isInterpreterFrame(), cx->interpreterRegs().pc == frame.script()->code());

    RootedFunction callee(cx);
    RootedScript script(cx, frame.script());
    RootedObject scopeChain(cx, frame.scopeChain());
    return create(cx, script, scopeChain, callee);
}

CallObject*
CallObject::createHollowForDebug(JSContext* cx, HandleFunction callee)
{
    MOZ_ASSERT(!callee->isHeavyweight());

    // This scope's parent link is never used: the DebugScopeObject that
    // refers to this scope carries its own parent link, which is what
    // Debugger uses to construct the tree of Debugger.Environment objects. So
    // just parent this scope directly to the global.
    Rooted<GlobalObject*> global(cx, &callee->global());
    Rooted<CallObject*> callobj(cx, createForFunction(cx, global, callee));
    if (!callobj)
        return nullptr;

    RootedValue optimizedOut(cx, MagicValue(JS_OPTIMIZED_OUT));
    RootedId id(cx);
    RootedScript script(cx, callee->nonLazyScript());
    for (BindingIter bi(script); !bi.done(); bi++) {
        id = NameToId(bi->name());
        if (!SetProperty(cx, callobj, callobj, id, &optimizedOut, true))
            return nullptr;
    }

    return callobj;
}

const Class CallObject::class_ = {
    "Call",
    JSCLASS_IS_ANONYMOUS | JSCLASS_HAS_RESERVED_SLOTS(CallObject::RESERVED_SLOTS)
};

const Class DeclEnvObject::class_ = {
    js_Object_str,
    JSCLASS_HAS_RESERVED_SLOTS(DeclEnvObject::RESERVED_SLOTS) |
    JSCLASS_HAS_CACHED_PROTO(JSProto_Object)
};

/*
 * Create a DeclEnvObject for a JSScript that is not initialized to any
 * particular callsite. This object can either be initialized (with an enclosing
 * scope and callee) or used as a template for jit compilation.
 */
DeclEnvObject*
DeclEnvObject::createTemplateObject(JSContext* cx, HandleFunction fun, gc::InitialHeap heap)
{
    MOZ_ASSERT(IsNurseryAllocable(FINALIZE_KIND));

    RootedObjectGroup group(cx, ObjectGroup::defaultNewGroup(cx, &class_, TaggedProto(nullptr)));
    if (!group)
        return nullptr;

    RootedShape emptyDeclEnvShape(cx);
    emptyDeclEnvShape = EmptyShape::getInitialShape(cx, &class_, TaggedProto(nullptr),
                                                    cx->global(), nullptr, FINALIZE_KIND,
                                                    BaseShape::DELEGATE);
    if (!emptyDeclEnvShape)
        return nullptr;

    RootedNativeObject obj(cx, MaybeNativeObject(JSObject::create(cx, FINALIZE_KIND, heap,
                                                                  emptyDeclEnvShape, group)));
    if (!obj)
        return nullptr;

    // Assign a fixed slot to a property with the same name as the lambda.
    Rooted<jsid> id(cx, AtomToId(fun->atom()));
    const Class* clasp = obj->getClass();
    unsigned attrs = JSPROP_ENUMERATE | JSPROP_PERMANENT | JSPROP_READONLY;

    JSPropertyOp getter = clasp->getProperty;
    JSStrictPropertyOp setter = clasp->setProperty;
    MOZ_ASSERT(getter != JS_PropertyStub);
    MOZ_ASSERT(setter != JS_StrictPropertyStub);

    if (!NativeObject::putProperty(cx, obj, id, getter, setter, lambdaSlot(), attrs, 0))
        return nullptr;

    MOZ_ASSERT(!obj->hasDynamicSlots());
    return &obj->as<DeclEnvObject>();
}

DeclEnvObject*
DeclEnvObject::create(JSContext* cx, HandleObject enclosing, HandleFunction callee)
{
    Rooted<DeclEnvObject*> obj(cx, createTemplateObject(cx, callee, gc::DefaultHeap));
    if (!obj)
        return nullptr;

    obj->setEnclosingScope(enclosing);
    obj->setFixedSlot(lambdaSlot(), ObjectValue(*callee));
    return obj;
}

template<XDRMode mode>
bool
js::XDRStaticWithObject(XDRState<mode>* xdr, HandleObject enclosingScope,
                        MutableHandle<StaticWithObject*> objp)
{
    if (mode == XDR_DECODE) {
        JSContext* cx = xdr->cx();
        Rooted<StaticWithObject*> obj(cx, StaticWithObject::create(cx));
        if (!obj)
            return false;
        obj->initEnclosingNestedScope(enclosingScope);
        objp.set(obj);
    }
    // For encoding, there is nothing to do.  The only information that is
    // encoded by a StaticWithObject is its presence on the scope chain, and the
    // script XDR handler already takes care of that.

    return true;
}

template bool
js::XDRStaticWithObject(XDRState<XDR_ENCODE>*, HandleObject, MutableHandle<StaticWithObject*>);

template bool
js::XDRStaticWithObject(XDRState<XDR_DECODE>*, HandleObject, MutableHandle<StaticWithObject*>);

StaticWithObject*
StaticWithObject::create(ExclusiveContext* cx)
{
    RootedObjectGroup group(cx, ObjectGroup::defaultNewGroup(cx, &class_, TaggedProto(nullptr)));
    if (!group)
        return nullptr;

    RootedShape shape(cx, EmptyShape::getInitialShape(cx, &class_, TaggedProto(nullptr),
                                                      nullptr, nullptr, FINALIZE_KIND));
    if (!shape)
        return nullptr;

    RootedObject obj(cx, JSObject::create(cx, FINALIZE_KIND, gc::TenuredHeap, shape, group));
    if (!obj)
        return nullptr;

    return &obj->as<StaticWithObject>();
}

static JSObject*
CloneStaticWithObject(JSContext* cx, HandleObject enclosingScope, Handle<StaticWithObject*> srcWith)
{
    Rooted<StaticWithObject*> clone(cx, StaticWithObject::create(cx));
    if (!clone)
        return nullptr;

    clone->initEnclosingNestedScope(enclosingScope);

    return clone;
}

DynamicWithObject*
DynamicWithObject::create(JSContext* cx, HandleObject object, HandleObject enclosing,
                          HandleObject staticWith, WithKind kind)
{
    MOZ_ASSERT(staticWith->is<StaticWithObject>());
    RootedObjectGroup group(cx, ObjectGroup::defaultNewGroup(cx, &class_,
                                                             TaggedProto(staticWith.get())));
    if (!group)
        return nullptr;

    RootedShape shape(cx, EmptyShape::getInitialShape(cx, &class_, TaggedProto(staticWith),
                                                      &enclosing->global(), nullptr,
                                                      FINALIZE_KIND));
    if (!shape)
        return nullptr;

    RootedNativeObject obj(cx, MaybeNativeObject(JSObject::create(cx, FINALIZE_KIND,
                                                                  gc::DefaultHeap, shape, group)));
    if (!obj)
        return nullptr;

    JSObject* thisp = GetThisObject(cx, object);
    if (!thisp)
        return nullptr;

    obj->as<ScopeObject>().setEnclosingScope(enclosing);
    obj->setFixedSlot(OBJECT_SLOT, ObjectValue(*object));
    obj->setFixedSlot(THIS_SLOT, ObjectValue(*thisp));
    obj->setFixedSlot(KIND_SLOT, Int32Value(kind));

    return &obj->as<DynamicWithObject>();
}

static bool
with_LookupProperty(JSContext* cx, HandleObject obj, HandleId id,
                    MutableHandleObject objp, MutableHandleShape propp)
{
    RootedObject actual(cx, &obj->as<DynamicWithObject>().object());
    return LookupProperty(cx, actual, id, objp, propp);
}

static bool
with_DefineProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue value,
                    JSPropertyOp getter, JSStrictPropertyOp setter, unsigned attrs)
{
    RootedObject actual(cx, &obj->as<DynamicWithObject>().object());
    return DefineProperty(cx, actual, id, value, getter, setter, attrs);
}

static bool
with_HasProperty(JSContext* cx, HandleObject obj, HandleId id, bool* foundp)
{
    RootedObject actual(cx, &obj->as<DynamicWithObject>().object());
    return HasProperty(cx, actual, id, foundp);
}

static bool
with_GetProperty(JSContext* cx, HandleObject obj, HandleObject receiver, HandleId id,
                 MutableHandleValue vp)
{
    RootedObject actual(cx, &obj->as<DynamicWithObject>().object());
    return GetProperty(cx, actual, actual, id, vp);
}

static bool
with_SetProperty(JSContext* cx, HandleObject obj, HandleObject receiver, HandleId id,
                 MutableHandleValue vp, bool strict)
{
    RootedObject actual(cx, &obj->as<DynamicWithObject>().object());
    RootedObject actualReceiver(cx, receiver);
    if (receiver == obj)
        actualReceiver = actual;
    return SetProperty(cx, actual, actualReceiver, id, vp, strict);
}

static bool
with_GetOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                              MutableHandle<JSPropertyDescriptor> desc)
{
    RootedObject actual(cx, &obj->as<DynamicWithObject>().object());
    return GetOwnPropertyDescriptor(cx, actual, id, desc);
}

static bool
with_DeleteProperty(JSContext* cx, HandleObject obj, HandleId id, bool* succeeded)
{
    RootedObject actual(cx, &obj->as<DynamicWithObject>().object());
    return DeleteProperty(cx, actual, id, succeeded);
}

static JSObject*
with_ThisObject(JSContext* cx, HandleObject obj)
{
    return &obj->as<DynamicWithObject>().withThis();
}

const Class StaticWithObject::class_ = {
    "WithTemplate",
    JSCLASS_IMPLEMENTS_BARRIERS |
    JSCLASS_HAS_RESERVED_SLOTS(StaticWithObject::RESERVED_SLOTS) |
    JSCLASS_IS_ANONYMOUS
};

const Class DynamicWithObject::class_ = {
    "With",
    JSCLASS_HAS_RESERVED_SLOTS(DynamicWithObject::RESERVED_SLOTS) |
    JSCLASS_IS_ANONYMOUS,
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* convert */
    nullptr, /* finalize */
    nullptr, /* call */
    nullptr, /* hasInstance */
    nullptr, /* construct */
    nullptr, /* trace */
    JS_NULL_CLASS_SPEC,
    JS_NULL_CLASS_EXT,
    {
        with_LookupProperty,
        with_DefineProperty,
        with_HasProperty,
        with_GetProperty,
        with_SetProperty,
        with_GetOwnPropertyDescriptor,
        with_DeleteProperty,
        nullptr, nullptr,    /* watch/unwatch */
        nullptr,             /* getElements */
        nullptr,             /* enumerate (native enumeration of target doesn't work) */
        with_ThisObject,
    }
};

/* static */ StaticEvalObject*
StaticEvalObject::create(JSContext* cx, HandleObject enclosing)
{
    RootedObjectGroup group(cx, ObjectGroup::defaultNewGroup(cx, &class_, TaggedProto(nullptr)));
    if (!group)
        return nullptr;

    RootedShape shape(cx, EmptyShape::getInitialShape(cx, &class_, TaggedProto(nullptr),
                                                      cx->global(), nullptr, FINALIZE_KIND,
                                                      BaseShape::DELEGATE));
    if (!shape)
        return nullptr;

    RootedNativeObject obj(cx, MaybeNativeObject(JSObject::create(cx, FINALIZE_KIND,
                                                                  gc::TenuredHeap, shape, group)));
    if (!obj)
        return nullptr;

    obj->setReservedSlot(SCOPE_CHAIN_SLOT, ObjectOrNullValue(enclosing));
    obj->setReservedSlot(STRICT_SLOT, BooleanValue(false));
    return &obj->as<StaticEvalObject>();
}

const Class StaticEvalObject::class_ = {
    "StaticEval",
    JSCLASS_HAS_RESERVED_SLOTS(StaticEvalObject::RESERVED_SLOTS) |
    JSCLASS_IS_ANONYMOUS
};

/*****************************************************************************/

/* static */ ClonedBlockObject*
ClonedBlockObject::create(JSContext* cx, Handle<StaticBlockObject*> block, HandleObject enclosing)
{
    MOZ_ASSERT(block->getClass() == &BlockObject::class_);

    RootedObjectGroup group(cx, ObjectGroup::defaultNewGroup(cx, &BlockObject::class_,
                                                             TaggedProto(block.get())));
    if (!group)
        return nullptr;

    RootedShape shape(cx, block->lastProperty());

    RootedNativeObject obj(cx, MaybeNativeObject(JSObject::create(cx, FINALIZE_KIND,
                                                                  gc::TenuredHeap, shape, group)));
    if (!obj)
        return nullptr;

    /* Set the parent if necessary, as for call objects. */
    if (&enclosing->global() != obj->getParent()) {
        MOZ_ASSERT(obj->getParent() == nullptr);
        Rooted<GlobalObject*> global(cx, &enclosing->global());
        if (!JSObject::setParent(cx, obj, global))
            return nullptr;
    }

    MOZ_ASSERT(!obj->inDictionaryMode());
    MOZ_ASSERT(obj->slotSpan() >= block->numVariables() + RESERVED_SLOTS);

    obj->setReservedSlot(SCOPE_CHAIN_SLOT, ObjectValue(*enclosing));

    MOZ_ASSERT(obj->isDelegate());

    return &obj->as<ClonedBlockObject>();
}

/* static */ ClonedBlockObject*
ClonedBlockObject::create(JSContext* cx, Handle<StaticBlockObject*> block, AbstractFramePtr frame)
{
    assertSameCompartment(cx, frame);
    RootedObject enclosing(cx, frame.scopeChain());
    return create(cx, block, enclosing);
}

/* static */ ClonedBlockObject*
ClonedBlockObject::createHollowForDebug(JSContext* cx, Handle<StaticBlockObject*> block)
{
    MOZ_ASSERT(!block->needsClone());

    // This scope's parent link is never used: the DebugScopeObject that
    // refers to this scope carries its own parent link, which is what
    // Debugger uses to construct the tree of Debugger.Environment objects. So
    // just parent this scope directly to the global.
    Rooted<GlobalObject*> global(cx, &block->global());
    Rooted<ClonedBlockObject*> obj(cx, create(cx, block, global));
    if (!obj)
        return nullptr;

    for (unsigned i = 0; i < block->numVariables(); i++)
        obj->setVar(i, MagicValue(JS_OPTIMIZED_OUT), DONT_CHECK_ALIASING);

    return obj;
}

void
ClonedBlockObject::copyUnaliasedValues(AbstractFramePtr frame)
{
    StaticBlockObject& block = staticBlock();
    for (unsigned i = 0; i < numVariables(); ++i) {
        if (!block.isAliased(i)) {
            Value& val = frame.unaliasedLocal(block.blockIndexToLocalIndex(i));
            setVar(i, val, DONT_CHECK_ALIASING);
        }
    }
}

StaticBlockObject*
StaticBlockObject::create(ExclusiveContext* cx)
{
    RootedObjectGroup group(cx, ObjectGroup::defaultNewGroup(cx, &BlockObject::class_,
                                                             TaggedProto(nullptr)));
    if (!group)
        return nullptr;

    RootedShape emptyBlockShape(cx);
    emptyBlockShape = EmptyShape::getInitialShape(cx, &BlockObject::class_, TaggedProto(nullptr), nullptr,
                                                  nullptr, FINALIZE_KIND, BaseShape::DELEGATE);
    if (!emptyBlockShape)
        return nullptr;

    JSObject* obj = JSObject::create(cx, FINALIZE_KIND, gc::TenuredHeap, emptyBlockShape, group);
    if (!obj)
        return nullptr;

    return &obj->as<StaticBlockObject>();
}

/* static */ Shape*
StaticBlockObject::addVar(ExclusiveContext* cx, Handle<StaticBlockObject*> block, HandleId id,
                          bool constant, unsigned index, bool* redeclared)
{
    MOZ_ASSERT(JSID_IS_ATOM(id));
    MOZ_ASSERT(index < LOCAL_INDEX_LIMIT);

    *redeclared = false;

    /* Inline NativeObject::addProperty in order to trap the redefinition case. */
    ShapeTable::Entry* entry;
    if (Shape::search(cx, block->lastProperty(), id, &entry, true)) {
        *redeclared = true;
        return nullptr;
    }

    /*
     * Don't convert this object to dictionary mode so that we can clone the
     * block's shape later.
     */
    uint32_t slot = JSSLOT_FREE(&BlockObject::class_) + index;
    uint32_t readonly = constant ? JSPROP_READONLY : 0;
    uint32_t propFlags = readonly | JSPROP_ENUMERATE | JSPROP_PERMANENT;
    return NativeObject::addPropertyInternal(cx, block, id,
                                             /* getter = */ nullptr,
                                             /* setter = */ nullptr,
                                             slot,
                                             propFlags,
                                             /* attrs = */ 0,
                                             entry,
                                             /* allowDictionary = */ false);
}

const Class BlockObject::class_ = {
    "Block",
    JSCLASS_IMPLEMENTS_BARRIERS |
    JSCLASS_HAS_RESERVED_SLOTS(BlockObject::RESERVED_SLOTS) |
    JSCLASS_IS_ANONYMOUS
};

template<XDRMode mode>
bool
js::XDRStaticBlockObject(XDRState<mode>* xdr, HandleObject enclosingScope,
                         MutableHandle<StaticBlockObject*> objp)
{
    /* NB: Keep this in sync with CloneStaticBlockObject. */

    JSContext* cx = xdr->cx();

    Rooted<StaticBlockObject*> obj(cx);
    uint32_t count = 0, offset = 0;

    if (mode == XDR_ENCODE) {
        obj = objp;
        count = obj->numVariables();
        offset = obj->localOffset();
    }

    if (mode == XDR_DECODE) {
        obj = StaticBlockObject::create(cx);
        if (!obj)
            return false;
        obj->initEnclosingNestedScope(enclosingScope);
        objp.set(obj);
    }

    if (!xdr->codeUint32(&count))
        return false;
    if (!xdr->codeUint32(&offset))
        return false;

    /*
     * XDR the block object's properties. We know that there are 'count'
     * properties to XDR, stored as id/aliased pairs.  (The empty string as
     * id indicates an int id.)
     */
    if (mode == XDR_DECODE) {
        obj->setLocalOffset(offset);

        for (unsigned i = 0; i < count; i++) {
            RootedAtom atom(cx);
            if (!XDRAtom(xdr, &atom))
                return false;

            RootedId id(cx, atom != cx->runtime()->emptyString
                            ? AtomToId(atom)
                            : INT_TO_JSID(i));

            uint32_t propFlags;
            if (!xdr->codeUint32(&propFlags))
                return false;

            bool readonly = !!(propFlags & 1);

            bool redeclared;
            if (!StaticBlockObject::addVar(cx, obj, id, readonly, i, &redeclared)) {
                MOZ_ASSERT(!redeclared);
                return false;
            }

            bool aliased = !!(propFlags >> 1);
            obj->setAliased(i, aliased);
        }
    } else {
        AutoShapeVector shapes(cx);
        if (!shapes.growBy(count))
            return false;

        for (Shape::Range<NoGC> r(obj->lastProperty()); !r.empty(); r.popFront())
            shapes[obj->shapeToIndex(r.front())].set(&r.front());

        RootedShape shape(cx);
        RootedId propid(cx);
        RootedAtom atom(cx);
        for (unsigned i = 0; i < count; i++) {
            shape = shapes[i];
            MOZ_ASSERT(shape->hasDefaultGetter());
            MOZ_ASSERT(obj->shapeToIndex(*shape) == i);

            propid = shape->propid();
            MOZ_ASSERT(JSID_IS_ATOM(propid) || JSID_IS_INT(propid));

            atom = JSID_IS_ATOM(propid)
                   ? JSID_TO_ATOM(propid)
                   : cx->runtime()->emptyString;
            if (!XDRAtom(xdr, &atom))
                return false;

            bool aliased = obj->isAliased(i);
            bool readonly = !shape->writable();
            uint32_t propFlags = (aliased << 1) | readonly;
            if (!xdr->codeUint32(&propFlags))
                return false;
        }
    }
    return true;
}

template bool
js::XDRStaticBlockObject(XDRState<XDR_ENCODE>*, HandleObject, MutableHandle<StaticBlockObject*>);

template bool
js::XDRStaticBlockObject(XDRState<XDR_DECODE>*, HandleObject, MutableHandle<StaticBlockObject*>);

static JSObject*
CloneStaticBlockObject(JSContext* cx, HandleObject enclosingScope, Handle<StaticBlockObject*> srcBlock)
{
    /* NB: Keep this in sync with XDRStaticBlockObject. */

    Rooted<StaticBlockObject*> clone(cx, StaticBlockObject::create(cx));
    if (!clone)
        return nullptr;

    clone->initEnclosingNestedScope(enclosingScope);
    clone->setLocalOffset(srcBlock->localOffset());

    /* Shape::Range is reverse order, so build a list in forward order. */
    AutoShapeVector shapes(cx);
    if (!shapes.growBy(srcBlock->numVariables()))
        return nullptr;

    for (Shape::Range<NoGC> r(srcBlock->lastProperty()); !r.empty(); r.popFront())
        shapes[srcBlock->shapeToIndex(r.front())].set(&r.front());

    for (Shape** p = shapes.begin(); p != shapes.end(); ++p) {
        RootedId id(cx, (*p)->propid());
        unsigned i = srcBlock->shapeToIndex(**p);

        bool redeclared;
        if (!StaticBlockObject::addVar(cx, clone, id, !(*p)->writable(), i, &redeclared)) {
            MOZ_ASSERT(!redeclared);
            return nullptr;
        }

        clone->setAliased(i, srcBlock->isAliased(i));
    }

    return clone;
}

JSObject*
js::CloneNestedScopeObject(JSContext* cx, HandleObject enclosingScope, Handle<NestedScopeObject*> srcBlock)
{
    if (srcBlock->is<StaticBlockObject>()) {
        Rooted<StaticBlockObject*> blockObj(cx, &srcBlock->as<StaticBlockObject>());
        return CloneStaticBlockObject(cx, enclosingScope, blockObj);
    } else {
        Rooted<StaticWithObject*> withObj(cx, &srcBlock->as<StaticWithObject>());
        return CloneStaticWithObject(cx, enclosingScope, withObj);
    }
}

/* static */ UninitializedLexicalObject*
UninitializedLexicalObject::create(JSContext* cx, HandleObject enclosing)
{
    RootedObjectGroup group(cx, ObjectGroup::defaultNewGroup(cx, &class_, TaggedProto(nullptr)));
    if (!group)
        return nullptr;

    RootedShape shape(cx, EmptyShape::getInitialShape(cx, &class_, TaggedProto(nullptr),
                                                      nullptr, nullptr, FINALIZE_KIND));
    if (!shape)
        return nullptr;

    RootedObject obj(cx, JSObject::create(cx, FINALIZE_KIND, gc::DefaultHeap, shape, group));
    if (!obj)
        return nullptr;

    obj->as<ScopeObject>().setEnclosingScope(enclosing);

    return &obj->as<UninitializedLexicalObject>();
}

static void
ReportUninitializedLexicalId(JSContext* cx, HandleId id)
{
    if (JSID_IS_ATOM(id)) {
        RootedPropertyName name(cx, JSID_TO_ATOM(id)->asPropertyName());
        ReportUninitializedLexical(cx, name);
        return;
    }
    MOZ_CRASH("UninitializedLexicalObject should only be used with property names");
}

static bool
uninitialized_LookupProperty(JSContext* cx, HandleObject obj, HandleId id,
                             MutableHandleObject objp, MutableHandleShape propp)
{
    ReportUninitializedLexicalId(cx, id);
    return false;
}

static bool
uninitialized_HasProperty(JSContext* cx, HandleObject obj, HandleId id, bool* foundp)
{
    ReportUninitializedLexicalId(cx, id);
    return false;
}

static bool
uninitialized_GetProperty(JSContext* cx, HandleObject obj, HandleObject receiver, HandleId id,
                          MutableHandleValue vp)
{
    ReportUninitializedLexicalId(cx, id);
    return false;
}

static bool
uninitialized_SetProperty(JSContext* cx, HandleObject obj, HandleObject receiver, HandleId id,
                          MutableHandleValue vp, bool strict)
{
    ReportUninitializedLexicalId(cx, id);
    return false;
}

static bool
uninitialized_GetOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                                       MutableHandle<JSPropertyDescriptor> desc)
{
    ReportUninitializedLexicalId(cx, id);
    return false;
}

static bool
uninitialized_DeleteProperty(JSContext* cx, HandleObject obj, HandleId id, bool* succeeded)
{
    ReportUninitializedLexicalId(cx, id);
    return false;
}

const Class UninitializedLexicalObject::class_ = {
    "UninitializedLexical",
    JSCLASS_HAS_RESERVED_SLOTS(UninitializedLexicalObject::RESERVED_SLOTS) |
    JSCLASS_IS_ANONYMOUS,
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* convert */
    nullptr, /* finalize */
    nullptr, /* call */
    nullptr, /* hasInstance */
    nullptr, /* construct */
    nullptr, /* trace */
    JS_NULL_CLASS_SPEC,
    JS_NULL_CLASS_EXT,
    {
        uninitialized_LookupProperty,
        nullptr,             /* defineProperty */
        uninitialized_HasProperty,
        uninitialized_GetProperty,
        uninitialized_SetProperty,
        uninitialized_GetOwnPropertyDescriptor,
        uninitialized_DeleteProperty,
        nullptr, nullptr,    /* watch/unwatch */
        nullptr,             /* getElements */
        nullptr,             /* enumerate (native enumeration of target doesn't work) */
        nullptr,             /* this */
    }
};

/*****************************************************************************/

// Any name atom for a function which will be added as a DeclEnv object to the
// scope chain above call objects for fun.
static inline JSAtom*
CallObjectLambdaName(JSFunction& fun)
{
    return fun.isNamedLambda() ? fun.atom() : nullptr;
}

ScopeIter::ScopeIter(JSContext* cx, const ScopeIter& si
                     MOZ_GUARD_OBJECT_NOTIFIER_PARAM_IN_IMPL)
  : ssi_(cx, si.ssi_),
    scope_(cx, si.scope_),
    frame_(si.frame_)
{
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;
}

ScopeIter::ScopeIter(JSContext* cx, JSObject* scope, JSObject* staticScope
                     MOZ_GUARD_OBJECT_NOTIFIER_PARAM_IN_IMPL)
  : ssi_(cx, staticScope),
    scope_(cx, scope),
    frame_(NullFramePtr())
{
    settle();
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;
}

ScopeIter::ScopeIter(JSContext* cx, AbstractFramePtr frame, jsbytecode* pc
                     MOZ_GUARD_OBJECT_NOTIFIER_PARAM_IN_IMPL)
  : ssi_(cx, frame.script()->innermostStaticScope(pc)),
    scope_(cx, frame.scopeChain()),
    frame_(frame)
{
    assertSameCompartment(cx, frame);
    settle();
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;
}

void
ScopeIter::incrementStaticScopeIter()
{
    ssi_++;

    // For named lambdas, DeclEnvObject scopes are always attached to their
    // CallObjects. Skip it here, as they are special cased in users of
    // ScopeIter.
    if (!ssi_.done() && ssi_.type() == StaticScopeIter<CanGC>::NamedLambda)
        ssi_++;
}

void
ScopeIter::settle()
{
    // Check for trying to iterate a heavyweight function frame before
    // the prologue has created the CallObject, in which case we have to skip.
    if (frame_ && frame_.isNonEvalFunctionFrame() &&
        frame_.fun()->isHeavyweight() && !frame_.hasCallObj())
    {
        MOZ_ASSERT(ssi_.type() == StaticScopeIter<CanGC>::Function);
        incrementStaticScopeIter();
    }

    // Check if we have left the extent of the initial frame after we've
    // settled on a static scope.
    if (frame_ && (ssi_.done() || maybeStaticScope() == frame_.script()->enclosingStaticScope()))
        frame_ = NullFramePtr();

#ifdef DEBUG
    if (!ssi_.done() && hasScopeObject()) {
        switch (ssi_.type()) {
          case StaticScopeIter<CanGC>::Function:
            MOZ_ASSERT(scope_->as<CallObject>().callee().nonLazyScript() == ssi_.funScript());
            break;
          case StaticScopeIter<CanGC>::Block:
            MOZ_ASSERT(scope_->as<ClonedBlockObject>().staticBlock() == staticBlock());
            break;
          case StaticScopeIter<CanGC>::With:
            MOZ_ASSERT(scope_->as<DynamicWithObject>().staticScope() == &staticWith());
            break;
          case StaticScopeIter<CanGC>::Eval:
            MOZ_ASSERT(scope_->as<CallObject>().isForEval());
            break;
          case StaticScopeIter<CanGC>::NamedLambda:
            MOZ_CRASH("named lambda static scopes should have been skipped");
        }
    }
#endif
}

ScopeIter&
ScopeIter::operator++()
{
    if (hasScopeObject()) {
        scope_ = &scope_->as<ScopeObject>().enclosingScope();
        if (scope_->is<DeclEnvObject>())
            scope_ = &scope_->as<DeclEnvObject>().enclosingScope();
    }

    incrementStaticScopeIter();
    settle();

    return *this;
}

ScopeIter::Type
ScopeIter::type() const
{
    MOZ_ASSERT(!done());

    switch (ssi_.type()) {
      case StaticScopeIter<CanGC>::Function:
        return Call;
      case StaticScopeIter<CanGC>::Block:
        return Block;
      case StaticScopeIter<CanGC>::With:
        return With;
      case StaticScopeIter<CanGC>::Eval:
        return Eval;
      case StaticScopeIter<CanGC>::NamedLambda:
        MOZ_CRASH("named lambda static scopes should have been skipped");
      default:
        MOZ_CRASH("bad SSI type");
    }
}

ScopeObject&
ScopeIter::scope() const
{
    MOZ_ASSERT(hasScopeObject());
    return scope_->as<ScopeObject>();
}

JSObject*
ScopeIter::maybeStaticScope() const
{
    if (ssi_.done())
        return nullptr;

    switch (ssi_.type()) {
      case StaticScopeIter<CanGC>::Function:
        return &fun();
      case StaticScopeIter<CanGC>::Block:
        return &staticBlock();
      case StaticScopeIter<CanGC>::With:
        return &staticWith();
      case StaticScopeIter<CanGC>::Eval:
        return &staticEval();
      case StaticScopeIter<CanGC>::NamedLambda:
        MOZ_CRASH("named lambda static scopes should have been skipped");
      default:
        MOZ_CRASH("bad SSI type");
    }
}

/* static */ HashNumber
MissingScopeKey::hash(MissingScopeKey sk)
{
    return size_t(sk.frame_.raw()) ^ size_t(sk.staticScope_);
}

/* static */ bool
MissingScopeKey::match(MissingScopeKey sk1, MissingScopeKey sk2)
{
    return sk1.frame_ == sk2.frame_ && sk1.staticScope_ == sk2.staticScope_;
}

void
LiveScopeVal::sweep()
{
    if (staticScope_)
        MOZ_ALWAYS_FALSE(IsObjectAboutToBeFinalizedFromAnyThread(staticScope_.unsafeGet()));
}

// Live ScopeIter values may be added to DebugScopes::liveScopes, as
// LiveScopeVal instances.  They need to have write barriers when they are added
// to the hash table, but no barriers when rehashing inside GC.  It's a nasty
// hack, but the important thing is that LiveScopeVal and MissingScopeKey need to
// alias each other.
void
LiveScopeVal::staticAsserts()
{
    static_assert(sizeof(LiveScopeVal) == sizeof(MissingScopeKey),
                  "LiveScopeVal must be same size of MissingScopeKey");
    static_assert(offsetof(LiveScopeVal, staticScope_) == offsetof(MissingScopeKey, staticScope_),
                  "LiveScopeVal.staticScope_ must alias MissingScopeKey.staticScope_");
}

/*****************************************************************************/

namespace {

/*
 * DebugScopeProxy is the handler for DebugScopeObject proxy objects. Having a
 * custom handler (rather than trying to reuse js::Wrapper) gives us several
 * important abilities:
 *  - We want to pass the ScopeObject as the receiver to forwarded scope
 *    property ops on aliased variables so that Call/Block/With ops do not all
 *    require a 'normalization' step.
 *  - The debug scope proxy can directly manipulate the stack frame to allow
 *    the debugger to read/write args/locals that were otherwise unaliased.
 *  - The debug scope proxy can store unaliased variables after the stack frame
 *    is popped so that they may still be read/written by the debugger.
 *  - The engine has made certain assumptions about the possible reads/writes
 *    in a scope. DebugScopeProxy allows us to prevent the debugger from
 *    breaking those assumptions.
 *  - The engine makes optimizations that are observable to the debugger. The
 *    proxy can either hide these optimizations or make the situation more
 *    clear to the debugger. An example is 'arguments'.
 */
class DebugScopeProxy : public BaseProxyHandler
{
    enum Action { SET, GET };

    enum AccessResult {
        ACCESS_UNALIASED,
        ACCESS_GENERIC,
        ACCESS_LOST
    };

    /*
     * This function handles access to unaliased locals/formals. Since they are
     * unaliased, the values of these variables are not stored in the slots of
     * the normal Call/BlockObject scope objects and thus must be recovered
     * from somewhere else:
     *  + if the invocation for which the scope was created is still executing,
     *    there is a JS frame live on the stack holding the values;
     *  + if the invocation for which the scope was created finished executing:
     *     - and there was a DebugScopeObject associated with scope, then the
     *       DebugScopes::onPop(Call|Block) handler copied out the unaliased
     *       variables:
     *        . for block scopes, the unaliased values were copied directly
     *          into the block object, since there is a slot allocated for every
     *          block binding, regardless of whether it is aliased;
     *        . for function scopes, a dense array is created in onPopCall to hold
     *          the unaliased values and attached to the DebugScopeObject;
     *     - and there was not a DebugScopeObject yet associated with the
     *       scope, then the unaliased values are lost and not recoverable.
     *
     * Callers should check accessResult for non-failure results:
     *  - ACCESS_UNALIASED if the access was unaliased and completed
     *  - ACCESS_GENERIC   if the access was aliased or the property not found
     *  - ACCESS_LOST      if the value has been lost to the debugger
     */
    bool handleUnaliasedAccess(JSContext* cx, Handle<DebugScopeObject*> debugScope,
                               Handle<ScopeObject*> scope, jsid id, Action action,
                               MutableHandleValue vp, AccessResult* accessResult) const
    {
        MOZ_ASSERT(&debugScope->scope() == scope);
        MOZ_ASSERT_IF(action == SET, !debugScope->isOptimizedOut());
        *accessResult = ACCESS_GENERIC;
        LiveScopeVal* maybeLiveScope = DebugScopes::hasLiveScope(*scope);

        /* Handle unaliased formals, vars, lets, and consts at function scope. */
        if (scope->is<CallObject>() && !scope->as<CallObject>().isForEval()) {
            CallObject& callobj = scope->as<CallObject>();
            RootedScript script(cx, callobj.callee().nonLazyScript());
            if (!script->ensureHasTypes(cx) || !script->ensureHasAnalyzedArgsUsage(cx))
                return false;

            Bindings& bindings = script->bindings;
            BindingIter bi(script);
            while (bi && NameToId(bi->name()) != id)
                bi++;
            if (!bi)
                return true;

            if (bi->kind() == Binding::VARIABLE || bi->kind() == Binding::CONSTANT) {
                if (script->bindingIsAliased(bi))
                    return true;

                uint32_t i = bi.frameIndex();
                if (maybeLiveScope) {
                    AbstractFramePtr frame = maybeLiveScope->frame();
                    if (action == GET)
                        vp.set(frame.unaliasedLocal(i));
                    else
                        frame.unaliasedLocal(i) = vp;
                } else if (NativeObject* snapshot = debugScope->maybeSnapshot()) {
                    if (action == GET)
                        vp.set(snapshot->getDenseElement(bindings.numArgs() + i));
                    else
                        snapshot->setDenseElement(bindings.numArgs() + i, vp);
                } else {
                    /* The unaliased value has been lost to the debugger. */
                    if (action == GET) {
                        *accessResult = ACCESS_LOST;
                        return true;
                    }
                }
            } else {
                MOZ_ASSERT(bi->kind() == Binding::ARGUMENT);
                unsigned i = bi.argIndex();
                if (script->formalIsAliased(i))
                    return true;

                if (maybeLiveScope) {
                    AbstractFramePtr frame = maybeLiveScope->frame();
                    if (script->argsObjAliasesFormals() && frame.hasArgsObj()) {
                        if (action == GET)
                            vp.set(frame.argsObj().arg(i));
                        else
                            frame.argsObj().setArg(i, vp);
                    } else {
                        if (action == GET)
                            vp.set(frame.unaliasedFormal(i, DONT_CHECK_ALIASING));
                        else
                            frame.unaliasedFormal(i, DONT_CHECK_ALIASING) = vp;
                    }
                } else if (NativeObject* snapshot = debugScope->maybeSnapshot()) {
                    if (action == GET)
                        vp.set(snapshot->getDenseElement(i));
                    else
                        snapshot->setDenseElement(i, vp);
                } else {
                    /* The unaliased value has been lost to the debugger. */
                    if (action == GET) {
                        *accessResult = ACCESS_LOST;
                        return true;
                    }
                }

                if (action == SET)
                    TypeScript::SetArgument(cx, script, i, vp);
            }

            *accessResult = ACCESS_UNALIASED;
            return true;
        }

        /* Handle unaliased let and catch bindings at block scope. */
        if (scope->is<ClonedBlockObject>()) {
            Rooted<ClonedBlockObject*> block(cx, &scope->as<ClonedBlockObject>());
            Shape* shape = block->lastProperty()->search(cx, id);
            if (!shape)
                return true;

            unsigned i = block->staticBlock().shapeToIndex(*shape);
            if (block->staticBlock().isAliased(i))
                return true;

            if (maybeLiveScope) {
                AbstractFramePtr frame = maybeLiveScope->frame();
                uint32_t local = block->staticBlock().blockIndexToLocalIndex(i);
                MOZ_ASSERT(local < frame.script()->nfixed());
                if (action == GET)
                    vp.set(frame.unaliasedLocal(local));
                else
                    frame.unaliasedLocal(local) = vp;
            } else {
                if (action == GET)
                    vp.set(block->var(i, DONT_CHECK_ALIASING));
                else
                    block->setVar(i, vp, DONT_CHECK_ALIASING);
            }

            *accessResult = ACCESS_UNALIASED;
            return true;
        }

        /* The rest of the internal scopes do not have unaliased vars. */
        MOZ_ASSERT(scope->is<DeclEnvObject>() || scope->is<DynamicWithObject>() ||
                   scope->as<CallObject>().isForEval());
        return true;
    }

    static bool isArguments(JSContext* cx, jsid id)
    {
        return id == NameToId(cx->names().arguments);
    }

    static bool isFunctionScope(ScopeObject& scope)
    {
        return scope.is<CallObject>() && !scope.as<CallObject>().isForEval();
    }

    /*
     * In theory, every function scope contains an 'arguments' bindings.
     * However, the engine only adds a binding if 'arguments' is used in the
     * function body. Thus, from the debugger's perspective, 'arguments' may be
     * missing from the list of bindings.
     */
    static bool isMissingArgumentsBinding(ScopeObject& scope)
    {
        return isFunctionScope(scope) &&
               !scope.as<CallObject>().callee().nonLazyScript()->argumentsHasVarBinding();
    }

    /*
     * This function checks if an arguments object needs to be created when
     * the debugger requests 'arguments' for a function scope where the
     * arguments object has been optimized away (either because the binding is
     * missing altogether or because !ScriptAnalysis::needsArgsObj).
     */
    static bool isMissingArguments(JSContext* cx, jsid id, ScopeObject& scope)
    {
        return isArguments(cx, id) && isFunctionScope(scope) &&
               !scope.as<CallObject>().callee().nonLazyScript()->needsArgsObj();
    }

    /*
     * Check if the value is the magic value JS_OPTIMIZED_ARGUMENTS. The
     * arguments analysis may have optimized out the 'arguments', and this
     * magic value could have propagated to other local slots. e.g.,
     *
     *   function f() { var a = arguments; h(); }
     *   function h() { evalInFrame(1, "a.push(0)"); }
     *
     * where evalInFrame(N, str) means to evaluate str N frames up.
     *
     * In this case we don't know we need to recover a missing arguments
     * object until after we've performed the property get.
     */
    static bool isMagicMissingArgumentsValue(JSContext* cx, ScopeObject& scope, HandleValue v)
    {
        bool isMagic = v.isMagic() && v.whyMagic() == JS_OPTIMIZED_ARGUMENTS;
        MOZ_ASSERT_IF(isMagic, isFunctionScope(scope) &&
                               !scope.as<CallObject>().callee().nonLazyScript()->needsArgsObj());
        return isMagic;
    }

    /*
     * Create a missing arguments object. If the function returns true but
     * argsObj is null, it means the scope is dead.
     */
    static bool createMissingArguments(JSContext* cx, ScopeObject& scope,
                                       MutableHandleArgumentsObject argsObj)
    {
        argsObj.set(nullptr);

        LiveScopeVal* maybeScope = DebugScopes::hasLiveScope(scope);
        if (!maybeScope)
            return true;

        argsObj.set(ArgumentsObject::createUnexpected(cx, maybeScope->frame()));
        return !!argsObj;
    }

  public:
    static const char family;
    static const DebugScopeProxy singleton;

    MOZ_CONSTEXPR DebugScopeProxy() : BaseProxyHandler(&family) {}

    bool preventExtensions(JSContext* cx, HandleObject proxy, bool* succeeded) const override
    {
        // always [[Extensible]], can't be made non-[[Extensible]], like most
        // proxies
        *succeeded = false;
        return true;
    }

    bool isExtensible(JSContext* cx, HandleObject proxy, bool* extensible) const override
    {
        // See above.
        *extensible = true;
        return true;
    }

    bool getPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                               MutableHandle<PropertyDescriptor> desc) const override
    {
        return getOwnPropertyDescriptor(cx, proxy, id, desc);
    }

    bool getMissingArgumentsPropertyDescriptor(JSContext* cx,
                                               Handle<DebugScopeObject*> debugScope,
                                               ScopeObject& scope,
                                               MutableHandle<PropertyDescriptor> desc) const
    {
        RootedArgumentsObject argsObj(cx);
        if (!createMissingArguments(cx, scope, &argsObj))
            return false;

        if (!argsObj) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEBUG_NOT_LIVE,
                                 "Debugger scope");
            return false;
        }

        desc.object().set(debugScope);
        desc.setAttributes(JSPROP_READONLY | JSPROP_ENUMERATE | JSPROP_PERMANENT);
        desc.value().setObject(*argsObj);
        desc.setGetter(nullptr);
        desc.setSetter(nullptr);
        return true;
    }

    bool getOwnPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                  MutableHandle<PropertyDescriptor> desc) const override
    {
        Rooted<DebugScopeObject*> debugScope(cx, &proxy->as<DebugScopeObject>());
        Rooted<ScopeObject*> scope(cx, &debugScope->scope());

        if (isMissingArguments(cx, id, *scope))
            return getMissingArgumentsPropertyDescriptor(cx, debugScope, *scope, desc);

        RootedValue v(cx);
        AccessResult access;
        if (!handleUnaliasedAccess(cx, debugScope, scope, id, GET, &v, &access))
            return false;

        switch (access) {
          case ACCESS_UNALIASED:
            if (isMagicMissingArgumentsValue(cx, *scope, v))
                return getMissingArgumentsPropertyDescriptor(cx, debugScope, *scope, desc);
            desc.object().set(debugScope);
            desc.setAttributes(JSPROP_READONLY | JSPROP_ENUMERATE | JSPROP_PERMANENT);
            desc.value().set(v);
            desc.setGetter(nullptr);
            desc.setSetter(nullptr);
            return true;
          case ACCESS_GENERIC:
            return JS_GetOwnPropertyDescriptorById(cx, scope, id, desc);
          case ACCESS_LOST:
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEBUG_OPTIMIZED_OUT);
            return false;
          default:
            MOZ_CRASH("bad AccessResult");
        }
    }

    bool getMissingArguments(JSContext* cx, ScopeObject& scope, MutableHandleValue vp) const
    {
        RootedArgumentsObject argsObj(cx);
        if (!createMissingArguments(cx, scope, &argsObj))
            return false;

        if (!argsObj) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEBUG_NOT_LIVE,
                                 "Debugger scope");
            return false;
        }

        vp.setObject(*argsObj);
        return true;
    }

    bool get(JSContext* cx, HandleObject proxy, HandleObject receiver, HandleId id,
             MutableHandleValue vp) const override
    {
        Rooted<DebugScopeObject*> debugScope(cx, &proxy->as<DebugScopeObject>());
        Rooted<ScopeObject*> scope(cx, &proxy->as<DebugScopeObject>().scope());

        if (isMissingArguments(cx, id, *scope))
            return getMissingArguments(cx, *scope, vp);

        AccessResult access;
        if (!handleUnaliasedAccess(cx, debugScope, scope, id, GET, vp, &access))
            return false;

        switch (access) {
          case ACCESS_UNALIASED:
            if (isMagicMissingArgumentsValue(cx, *scope, vp))
                return getMissingArguments(cx, *scope, vp);
            return true;
          case ACCESS_GENERIC:
            return GetProperty(cx, scope, scope, id, vp);
          case ACCESS_LOST:
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEBUG_OPTIMIZED_OUT);
            return false;
          default:
            MOZ_CRASH("bad AccessResult");
        }
    }

    bool getMissingArgumentsMaybeSentinelValue(JSContext* cx, ScopeObject& scope,
                                               MutableHandleValue vp) const
    {
        RootedArgumentsObject argsObj(cx);
        if (!createMissingArguments(cx, scope, &argsObj))
            return false;
        vp.set(argsObj ? ObjectValue(*argsObj) : MagicValue(JS_OPTIMIZED_ARGUMENTS));
        return true;
    }

    /*
     * Like 'get', but returns sentinel values instead of throwing on
     * exceptional cases.
     */
    bool getMaybeSentinelValue(JSContext* cx, Handle<DebugScopeObject*> debugScope, HandleId id,
                               MutableHandleValue vp) const
    {
        Rooted<ScopeObject*> scope(cx, &debugScope->scope());

        if (isMissingArguments(cx, id, *scope))
            return getMissingArgumentsMaybeSentinelValue(cx, *scope, vp);

        AccessResult access;
        if (!handleUnaliasedAccess(cx, debugScope, scope, id, GET, vp, &access))
            return false;

        switch (access) {
          case ACCESS_UNALIASED:
            if (isMagicMissingArgumentsValue(cx, *scope, vp))
                return getMissingArgumentsMaybeSentinelValue(cx, *scope, vp);
            return true;
          case ACCESS_GENERIC:
            return GetProperty(cx, scope, scope, id, vp);
          case ACCESS_LOST:
            vp.setMagic(JS_OPTIMIZED_OUT);
            return true;
          default:
            MOZ_CRASH("bad AccessResult");
        }
    }

    bool set(JSContext* cx, HandleObject proxy, HandleObject receiver, HandleId id, bool strict,
             MutableHandleValue vp) const override
    {
        Rooted<DebugScopeObject*> debugScope(cx, &proxy->as<DebugScopeObject>());
        Rooted<ScopeObject*> scope(cx, &proxy->as<DebugScopeObject>().scope());

        if (debugScope->isOptimizedOut())
            return Throw(cx, id, JSMSG_DEBUG_CANT_SET_OPT_ENV);

        AccessResult access;
        if (!handleUnaliasedAccess(cx, debugScope, scope, id, SET, vp, &access))
            return false;

        switch (access) {
          case ACCESS_UNALIASED:
            return true;
          case ACCESS_GENERIC:
            return SetProperty(cx, scope, scope, id, vp, strict);
          default:
            MOZ_CRASH("bad AccessResult");
        }
    }

    bool defineProperty(JSContext* cx, HandleObject proxy, HandleId id,
                        MutableHandle<PropertyDescriptor> desc) const override
    {
        Rooted<ScopeObject*> scope(cx, &proxy->as<DebugScopeObject>().scope());

        bool found;
        if (!has(cx, proxy, id, &found))
            return false;
        if (found)
            return Throw(cx, id, JSMSG_CANT_REDEFINE_PROP);

        return JS_DefinePropertyById(cx, scope, id, desc.value(),
                                     // Descriptors never store JSNatives for
                                     // accessors: they have either JSFunctions
                                     // or JSPropertyOps.
                                     desc.attributes() | JSPROP_PROPOP_ACCESSORS,
                                     JS_PROPERTYOP_GETTER(desc.getter()),
                                     JS_PROPERTYOP_SETTER(desc.setter()));
    }

    bool ownPropertyKeys(JSContext* cx, HandleObject proxy, AutoIdVector& props) const override
    {
        Rooted<ScopeObject*> scope(cx, &proxy->as<DebugScopeObject>().scope());

        if (isMissingArgumentsBinding(*scope)) {
            if (!props.append(NameToId(cx->names().arguments)))
                return false;
        }

        // DynamicWithObject isn't a very good proxy.  It doesn't have a
        // JSNewEnumerateOp implementation, because if it just delegated to the
        // target object, the object would indicate that native enumeration is
        // the thing to do, but native enumeration over the DynamicWithObject
        // wrapper yields no properties.  So instead here we hack around the
        // issue, and punch a hole through to the with object target.
        Rooted<JSObject*> target(cx, (scope->is<DynamicWithObject>()
                                      ? &scope->as<DynamicWithObject>().object() : scope));
        if (!GetPropertyKeys(cx, target, JSITER_OWNONLY, &props))
            return false;

        /*
         * Function scopes are optimized to not contain unaliased variables so
         * they must be manually appended here.
         */
        if (scope->is<CallObject>() && !scope->as<CallObject>().isForEval()) {
            RootedScript script(cx, scope->as<CallObject>().callee().nonLazyScript());
            for (BindingIter bi(script); bi; bi++) {
                if (!bi->aliased() && !props.append(NameToId(bi->name())))
                    return false;
            }
        }

        return true;
    }

    bool enumerate(JSContext* cx, HandleObject proxy, MutableHandleObject objp) const override
    {
        return BaseProxyHandler::enumerate(cx, proxy, objp);
    }

    bool has(JSContext* cx, HandleObject proxy, HandleId id_, bool* bp) const override
    {
        RootedId id(cx, id_);
        ScopeObject& scopeObj = proxy->as<DebugScopeObject>().scope();

        if (isArguments(cx, id) && isFunctionScope(scopeObj)) {
            *bp = true;
            return true;
        }

        bool found;
        RootedObject scope(cx, &scopeObj);
        if (!JS_HasPropertyById(cx, scope, id, &found))
            return false;

        /*
         * Function scopes are optimized to not contain unaliased variables so
         * a manual search is necessary.
         */
        if (!found && scope->is<CallObject>() && !scope->as<CallObject>().isForEval()) {
            RootedScript script(cx, scope->as<CallObject>().callee().nonLazyScript());
            for (BindingIter bi(script); bi; bi++) {
                if (!bi->aliased() && NameToId(bi->name()) == id) {
                    found = true;
                    break;
                }
            }
        }

        *bp = found;
        return true;
    }

    bool delete_(JSContext* cx, HandleObject proxy, HandleId id, bool* bp) const override
    {
        RootedValue idval(cx, IdToValue(id));
        return js_ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_CANT_DELETE,
                                        JSDVG_IGNORE_STACK, idval, NullPtr(), nullptr, nullptr);
    }
};

} /* anonymous namespace */

const char DebugScopeProxy::family = 0;
const DebugScopeProxy DebugScopeProxy::singleton;

/* static */ DebugScopeObject*
DebugScopeObject::create(JSContext* cx, ScopeObject& scope, HandleObject enclosing)
{
    MOZ_ASSERT(scope.compartment() == cx->compartment());
    RootedValue priv(cx, ObjectValue(scope));
    JSObject* obj = NewProxyObject(cx, &DebugScopeProxy::singleton, priv,
                                   nullptr /* proto */, &scope.global());
    if (!obj)
        return nullptr;

    MOZ_ASSERT(!enclosing->is<ScopeObject>());

    DebugScopeObject* debugScope = &obj->as<DebugScopeObject>();
    debugScope->setExtra(ENCLOSING_EXTRA, ObjectValue(*enclosing));
    debugScope->setExtra(SNAPSHOT_EXTRA, NullValue());

    return debugScope;
}

ScopeObject&
DebugScopeObject::scope() const
{
    return target()->as<ScopeObject>();
}

JSObject&
DebugScopeObject::enclosingScope() const
{
    return extra(ENCLOSING_EXTRA).toObject();
}

ArrayObject*
DebugScopeObject::maybeSnapshot() const
{
    MOZ_ASSERT(!scope().as<CallObject>().isForEval());
    JSObject* obj = extra(SNAPSHOT_EXTRA).toObjectOrNull();
    return obj ? &obj->as<ArrayObject>() : nullptr;
}

void
DebugScopeObject::initSnapshot(ArrayObject& o)
{
    MOZ_ASSERT(maybeSnapshot() == nullptr);
    setExtra(SNAPSHOT_EXTRA, ObjectValue(o));
}

bool
DebugScopeObject::isForDeclarative() const
{
    ScopeObject& s = scope();
    return s.is<CallObject>() || s.is<BlockObject>() || s.is<DeclEnvObject>();
}

bool
DebugScopeObject::getMaybeSentinelValue(JSContext* cx, HandleId id, MutableHandleValue vp)
{
    Rooted<DebugScopeObject*> self(cx, this);
    return DebugScopeProxy::singleton.getMaybeSentinelValue(cx, self, id, vp);
}

bool
DebugScopeObject::isOptimizedOut() const
{
    ScopeObject& s = scope();

    if (DebugScopes::hasLiveScope(s))
        return false;

    if (s.is<ClonedBlockObject>())
        return !s.as<ClonedBlockObject>().staticBlock().needsClone();

    if (s.is<CallObject>()) {
        return !s.as<CallObject>().isForEval() &&
               !s.as<CallObject>().callee().isHeavyweight() &&
               !maybeSnapshot();
    }

    return false;
}

bool
js_IsDebugScopeSlow(ProxyObject* proxy)
{
    MOZ_ASSERT(proxy->hasClass(&ProxyObject::class_));
    return proxy->handler() == &DebugScopeProxy::singleton;
}

/*****************************************************************************/

/* static */ MOZ_ALWAYS_INLINE void
DebugScopes::proxiedScopesPostWriteBarrier(JSRuntime* rt, ObjectWeakMap* map,
                                           const PreBarrieredObject& key)
{
    if (key && IsInsideNursery(key))
        rt->gc.storeBuffer.putGeneric(UnbarrieredRef(map, key.get()));
}

/* static */ MOZ_ALWAYS_INLINE void
DebugScopes::liveScopesPostWriteBarrier(JSRuntime* rt, LiveScopeMap* map, ScopeObject* key)
{
    // As above.  Otherwise, barriers could fire during GC when moving the
    // value.
    typedef HashMap<ScopeObject*,
                    MissingScopeKey,
                    DefaultHasher<ScopeObject*>,
                    RuntimeAllocPolicy> UnbarrieredLiveScopeMap;
    typedef gc::HashKeyRef<UnbarrieredLiveScopeMap, ScopeObject*> Ref;
    if (key && IsInsideNursery(key))
        rt->gc.storeBuffer.putGeneric(Ref(reinterpret_cast<UnbarrieredLiveScopeMap*>(map), key));
}

DebugScopes::DebugScopes(JSContext* cx)
 : proxiedScopes(cx),
   missingScopes(cx->runtime()),
   liveScopes(cx->runtime())
{}

DebugScopes::~DebugScopes()
{
    MOZ_ASSERT(missingScopes.empty());
    WeakMapBase::removeWeakMapFromList(&proxiedScopes);
}

bool
DebugScopes::init()
{
    if (!liveScopes.init() ||
        !proxiedScopes.init() ||
        !missingScopes.init())
    {
        return false;
    }
    return true;
}

void
DebugScopes::mark(JSTracer* trc)
{
    proxiedScopes.trace(trc);
}

void
DebugScopes::sweep(JSRuntime* rt)
{
    /*
     * missingScopes points to debug scopes weakly so that debug scopes can be
     * released more eagerly.
     */
    for (MissingScopeMap::Enum e(missingScopes); !e.empty(); e.popFront()) {
        DebugScopeObject** debugScope = e.front().value().unsafeGet();
        if (IsObjectAboutToBeFinalizedFromAnyThread(debugScope)) {
            /*
             * Note that onPopCall and onPopBlock rely on missingScopes to find
             * scope objects that we synthesized for the debugger's sake, and
             * clean up the synthetic scope objects' entries in liveScopes. So
             * if we remove an entry frcom missingScopes here, we must also
             * remove the corresponding liveScopes entry.
             *
             * Since the DebugScopeObject is the only thing using its scope
             * object, and the DSO is about to be finalized, you might assume
             * that the synthetic SO is also about to be finalized too, and thus
             * the loop below will take care of things. But complex GC behavior
             * means that marks are only conservative approximations of
             * liveness; we should assume that anything could be marked.
             *
             * Thus, we must explicitly remove the entries from both liveScopes
             * and missingScopes here.
             */
            liveScopes.remove(&(*debugScope)->scope());
            e.removeFront();
        } else {
            MissingScopeKey key = e.front().key();
            if (IsForwarded(key.staticScope())) {
                key.updateStaticScope(Forwarded(key.staticScope()));
                e.rekeyFront(key);
            }
        }
    }

    for (LiveScopeMap::Enum e(liveScopes); !e.empty(); e.popFront()) {
        ScopeObject* scope = e.front().key();

        e.front().value().sweep();

        /*
         * Scopes can be finalized when a debugger-synthesized ScopeObject is
         * no longer reachable via its DebugScopeObject.
         */
        if (IsObjectAboutToBeFinalizedFromAnyThread(&scope))
            e.removeFront();
        else if (scope != e.front().key())
            e.rekeyFront(scope);
    }
}

#ifdef JSGC_HASH_TABLE_CHECKS
void
DebugScopes::checkHashTablesAfterMovingGC(JSRuntime* runtime)
{
    /*
     * This is called at the end of StoreBuffer::mark() to check that our
     * postbarriers have worked and that no hashtable keys (or values) are left
     * pointing into the nursery.
     */
    for (ObjectWeakMap::Range r = proxiedScopes.all(); !r.empty(); r.popFront()) {
        CheckGCThingAfterMovingGC(r.front().key().get());
        CheckGCThingAfterMovingGC(r.front().value().get());
    }
    for (MissingScopeMap::Range r = missingScopes.all(); !r.empty(); r.popFront()) {
        CheckGCThingAfterMovingGC(r.front().key().staticScope());
        CheckGCThingAfterMovingGC(r.front().value().get());
    }
    for (LiveScopeMap::Range r = liveScopes.all(); !r.empty(); r.popFront()) {
        CheckGCThingAfterMovingGC(r.front().key());
        CheckGCThingAfterMovingGC(r.front().value().staticScope_.get());
    }
}
#endif

/*
 * Unfortunately, GetDebugScopeForFrame needs to work even outside debug mode
 * (in particular, JS_GetFrameScopeChain does not require debug mode). Since
 * DebugScopes::onPop* are only called in debuggee frames, this means we
 * cannot use any of the maps in DebugScopes. This will produce debug scope
 * chains that do not obey the debugger invariants but that is just fine.
 */
static bool
CanUseDebugScopeMaps(JSContext* cx)
{
    return cx->compartment()->isDebuggee();
}

DebugScopes*
DebugScopes::ensureCompartmentData(JSContext* cx)
{
    JSCompartment* c = cx->compartment();
    if (c->debugScopes)
        return c->debugScopes;

    c->debugScopes = cx->runtime()->new_<DebugScopes>(cx);
    if (c->debugScopes && c->debugScopes->init())
        return c->debugScopes;

    if (c->debugScopes)
        js_delete<DebugScopes>(c->debugScopes);
    c->debugScopes = nullptr;
    js_ReportOutOfMemory(cx);
    return nullptr;
}

DebugScopeObject*
DebugScopes::hasDebugScope(JSContext* cx, ScopeObject& scope)
{
    DebugScopes* scopes = scope.compartment()->debugScopes;
    if (!scopes)
        return nullptr;

    if (ObjectWeakMap::Ptr p = scopes->proxiedScopes.lookup(&scope)) {
        MOZ_ASSERT(CanUseDebugScopeMaps(cx));
        return &p->value()->as<DebugScopeObject>();
    }

    return nullptr;
}

bool
DebugScopes::addDebugScope(JSContext* cx, ScopeObject& scope, DebugScopeObject& debugScope)
{
    MOZ_ASSERT(cx->compartment() == scope.compartment());
    MOZ_ASSERT(cx->compartment() == debugScope.compartment());

    if (!CanUseDebugScopeMaps(cx))
        return true;

    DebugScopes* scopes = ensureCompartmentData(cx);
    if (!scopes)
        return false;

    MOZ_ASSERT(!scopes->proxiedScopes.has(&scope));
    if (!scopes->proxiedScopes.put(&scope, &debugScope)) {
        js_ReportOutOfMemory(cx);
        return false;
    }

    proxiedScopesPostWriteBarrier(cx->runtime(), &scopes->proxiedScopes, &scope);
    return true;
}

DebugScopeObject*
DebugScopes::hasDebugScope(JSContext* cx, const ScopeIter& si)
{
    MOZ_ASSERT(!si.hasScopeObject());

    DebugScopes* scopes = cx->compartment()->debugScopes;
    if (!scopes)
        return nullptr;

    if (MissingScopeMap::Ptr p = scopes->missingScopes.lookup(MissingScopeKey(si))) {
        MOZ_ASSERT(CanUseDebugScopeMaps(cx));
        return p->value();
    }
    return nullptr;
}

bool
DebugScopes::addDebugScope(JSContext* cx, const ScopeIter& si, DebugScopeObject& debugScope)
{
    MOZ_ASSERT(!si.hasScopeObject());
    MOZ_ASSERT(cx->compartment() == debugScope.compartment());
    MOZ_ASSERT_IF(si.withinInitialFrame() && si.initialFrame().isFunctionFrame(),
                  !si.initialFrame().callee()->isGenerator());
    // Generators should always reify their scopes.
    MOZ_ASSERT_IF(si.type() == ScopeIter::Call, !si.fun().isGenerator());

    if (!CanUseDebugScopeMaps(cx))
        return true;

    DebugScopes* scopes = ensureCompartmentData(cx);
    if (!scopes)
        return false;

    MissingScopeKey key(si);
    MOZ_ASSERT(!scopes->missingScopes.has(key));
    if (!scopes->missingScopes.put(key, ReadBarriered<DebugScopeObject*>(&debugScope))) {
        js_ReportOutOfMemory(cx);
        return false;
    }

    // Only add to liveScopes if we synthesized the debug scope on a live
    // frame.
    if (si.withinInitialFrame()) {
        MOZ_ASSERT(!scopes->liveScopes.has(&debugScope.scope()));
        if (!scopes->liveScopes.put(&debugScope.scope(), LiveScopeVal(si))) {
            js_ReportOutOfMemory(cx);
            return false;
        }
        liveScopesPostWriteBarrier(cx->runtime(), &scopes->liveScopes, &debugScope.scope());
    }

    return true;
}

void
DebugScopes::onPopCall(AbstractFramePtr frame, JSContext* cx)
{
    assertSameCompartment(cx, frame);

    DebugScopes* scopes = cx->compartment()->debugScopes;
    if (!scopes)
        return;

    Rooted<DebugScopeObject*> debugScope(cx, nullptr);

    if (frame.fun()->isHeavyweight()) {
        /*
         * The frame may be observed before the prologue has created the
         * CallObject. See ScopeIter::settle.
         */
        if (!frame.hasCallObj())
            return;

        if (frame.fun()->isGenerator())
            return;

        CallObject& callobj = frame.scopeChain()->as<CallObject>();
        scopes->liveScopes.remove(&callobj);
        if (ObjectWeakMap::Ptr p = scopes->proxiedScopes.lookup(&callobj))
            debugScope = &p->value()->as<DebugScopeObject>();
    } else {
        ScopeIter si(cx, frame, frame.script()->main());
        if (MissingScopeMap::Ptr p = scopes->missingScopes.lookup(MissingScopeKey(si))) {
            debugScope = p->value();
            scopes->liveScopes.remove(&debugScope->scope().as<CallObject>());
            scopes->missingScopes.remove(p);
        }
    }

    /*
     * When the JS stack frame is popped, the values of unaliased variables
     * are lost. If there is any debug scope referring to this scope, save a
     * copy of the unaliased variables' values in an array for later debugger
     * access via DebugScopeProxy::handleUnaliasedAccess.
     *
     * Note: since it is simplest for this function to be infallible, failure
     * in this code will be silently ignored. This does not break any
     * invariants since DebugScopeObject::maybeSnapshot can already be nullptr.
     */
    if (debugScope) {
        /*
         * Copy all frame values into the snapshot, regardless of
         * aliasing. This unnecessarily includes aliased variables
         * but it simplifies later indexing logic.
         */
        AutoValueVector vec(cx);
        if (!frame.copyRawFrameSlots(&vec) || vec.length() == 0)
            return;

        /*
         * Copy in formals that are not aliased via the scope chain
         * but are aliased via the arguments object.
         */
        RootedScript script(cx, frame.script());
        if (script->analyzedArgsUsage() && script->needsArgsObj() && frame.hasArgsObj()) {
            for (unsigned i = 0; i < frame.numFormalArgs(); ++i) {
                if (script->formalLivesInArgumentsObject(i))
                    vec[i].set(frame.argsObj().arg(i));
            }
        }

        /*
         * Use a dense array as storage (since proxies do not have trace
         * hooks). This array must not escape into the wild.
         */
        RootedArrayObject snapshot(cx, NewDenseCopiedArray(cx, vec.length(), vec.begin()));
        if (!snapshot) {
            cx->clearPendingException();
            return;
        }

        debugScope->initSnapshot(*snapshot);
    }
}

void
DebugScopes::onPopBlock(JSContext* cx, AbstractFramePtr frame, jsbytecode* pc)
{
    assertSameCompartment(cx, frame);

    DebugScopes* scopes = cx->compartment()->debugScopes;
    if (!scopes)
        return;

    ScopeIter si(cx, frame, pc);
    onPopBlock(cx, si);
}

void
DebugScopes::onPopBlock(JSContext* cx, const ScopeIter& si)
{
    DebugScopes* scopes = cx->compartment()->debugScopes;
    if (!scopes)
        return;

    MOZ_ASSERT(si.withinInitialFrame());
    MOZ_ASSERT(si.type() == ScopeIter::Block);

    if (si.staticBlock().needsClone()) {
        ClonedBlockObject& clone = si.scope().as<ClonedBlockObject>();
        clone.copyUnaliasedValues(si.initialFrame());
        scopes->liveScopes.remove(&clone);
    } else {
        if (MissingScopeMap::Ptr p = scopes->missingScopes.lookup(MissingScopeKey(si))) {
            ClonedBlockObject& clone = p->value()->scope().as<ClonedBlockObject>();
            clone.copyUnaliasedValues(si.initialFrame());
            scopes->liveScopes.remove(&clone);
            scopes->missingScopes.remove(p);
        }
    }
}

void
DebugScopes::onPopWith(AbstractFramePtr frame)
{
    DebugScopes* scopes = frame.compartment()->debugScopes;
    if (scopes)
        scopes->liveScopes.remove(&frame.scopeChain()->as<DynamicWithObject>());
}

void
DebugScopes::onPopStrictEvalScope(AbstractFramePtr frame)
{
    DebugScopes* scopes = frame.compartment()->debugScopes;
    if (!scopes)
        return;

    /*
     * The stack frame may be observed before the prologue has created the
     * CallObject. See ScopeIter::settle.
     */
    if (frame.hasCallObj())
        scopes->liveScopes.remove(&frame.scopeChain()->as<CallObject>());
}

void
DebugScopes::onCompartmentUnsetIsDebuggee(JSCompartment* c)
{
    DebugScopes* scopes = c->debugScopes;
    if (scopes) {
        scopes->proxiedScopes.clear();
        scopes->missingScopes.clear();
        scopes->liveScopes.clear();
    }
}

bool
DebugScopes::updateLiveScopes(JSContext* cx)
{
    JS_CHECK_RECURSION(cx, return false);

    /*
     * Note that we must always update the top frame's scope objects' entries
     * in liveScopes because we can't be sure code hasn't run in that frame to
     * change the scope chain since we were last called. The fp->prevUpToDate()
     * flag indicates whether the scopes of frames older than fp are already
     * included in liveScopes. It might seem simpler to have fp instead carry a
     * flag indicating whether fp itself is accurately described, but then we
     * would need to clear that flag whenever fp ran code. By storing the 'up
     * to date' bit for fp->prev() in fp, simply popping fp effectively clears
     * the flag for us, at exactly the time when execution resumes fp->prev().
     */
    for (AllFramesIter i(cx); !i.done(); ++i) {
        if (!i.hasUsableAbstractFramePtr())
            continue;

        AbstractFramePtr frame = i.abstractFramePtr();
        if (frame.scopeChain()->compartment() != cx->compartment())
            continue;

        if (frame.isFunctionFrame() && frame.callee()->isGenerator())
            continue;

        if (!frame.isDebuggee())
            continue;

        for (ScopeIter si(cx, frame, i.pc()); si.withinInitialFrame(); ++si) {
            if (si.hasScopeObject()) {
                MOZ_ASSERT(si.scope().compartment() == cx->compartment());
                DebugScopes* scopes = ensureCompartmentData(cx);
                if (!scopes)
                    return false;
                if (!scopes->liveScopes.put(&si.scope(), LiveScopeVal(si)))
                    return false;
                liveScopesPostWriteBarrier(cx->runtime(), &scopes->liveScopes, &si.scope());
            }
        }

        if (frame.prevUpToDate())
            return true;
        MOZ_ASSERT(frame.scopeChain()->compartment()->isDebuggee());
        frame.setPrevUpToDate();
    }

    return true;
}

LiveScopeVal*
DebugScopes::hasLiveScope(ScopeObject& scope)
{
    DebugScopes* scopes = scope.compartment()->debugScopes;
    if (!scopes)
        return nullptr;

    if (LiveScopeMap::Ptr p = scopes->liveScopes.lookup(&scope))
        return &p->value();

    return nullptr;
}

/* static */ void
DebugScopes::unsetPrevUpToDateUntil(JSContext* cx, AbstractFramePtr until)
{
    // This is the one exception where fp->prevUpToDate() is cleared without
    // popping the frame. When a frame is rematerialized, all frames younger
    // than the rematerialized frame have their prevUpToDate set to
    // false. This is because unrematerialized Ion frames have no usable
    // AbstractFramePtr, and so are skipped by the updateLiveScopes. If in the
    // future a frame suddenly gains a usable AbstractFramePtr via
    // rematerialization, the prevUpToDate invariant will no longer hold.
    for (AllFramesIter i(cx); !i.done(); ++i) {
        if (!i.hasUsableAbstractFramePtr())
            continue;

        AbstractFramePtr frame = i.abstractFramePtr();
        if (frame == until)
            return;

        if (frame.scopeChain()->compartment() != cx->compartment())
            continue;

        frame.unsetPrevUpToDate();
    }
}

/* static */ void
DebugScopes::forwardLiveFrame(JSContext* cx, AbstractFramePtr from, AbstractFramePtr to)
{
    DebugScopes* scopes = cx->compartment()->debugScopes;
    if (!scopes)
        return;

    for (MissingScopeMap::Enum e(scopes->missingScopes); !e.empty(); e.popFront()) {
        MissingScopeKey key = e.front().key();
        if (key.frame() == from) {
            key.updateFrame(to);
            e.rekeyFront(key);
        }
    }

    for (LiveScopeMap::Enum e(scopes->liveScopes); !e.empty(); e.popFront()) {
        LiveScopeVal& val = e.front().value();
        if (val.frame() == from)
            val.updateFrame(to);
    }
}

/*****************************************************************************/

static JSObject*
GetDebugScope(JSContext* cx, const ScopeIter& si);

static DebugScopeObject*
GetDebugScopeForScope(JSContext* cx, const ScopeIter& si)
{
    Rooted<ScopeObject*> scope(cx, &si.scope());
    if (DebugScopeObject* debugScope = DebugScopes::hasDebugScope(cx, *scope))
        return debugScope;

    ScopeIter copy(cx, si);
    RootedObject enclosingDebug(cx, GetDebugScope(cx, ++copy));
    if (!enclosingDebug)
        return nullptr;

    JSObject& maybeDecl = scope->enclosingScope();
    if (maybeDecl.is<DeclEnvObject>()) {
        MOZ_ASSERT(CallObjectLambdaName(scope->as<CallObject>().callee()));
        enclosingDebug = DebugScopeObject::create(cx, maybeDecl.as<DeclEnvObject>(), enclosingDebug);
        if (!enclosingDebug)
            return nullptr;
    }

    DebugScopeObject* debugScope = DebugScopeObject::create(cx, *scope, enclosingDebug);
    if (!debugScope)
        return nullptr;

    if (!DebugScopes::addDebugScope(cx, *scope, *debugScope))
        return nullptr;

    return debugScope;
}

static DebugScopeObject*
GetDebugScopeForMissing(JSContext* cx, const ScopeIter& si)
{
    MOZ_ASSERT(!si.hasScopeObject() && si.canHaveScopeObject());

    if (DebugScopeObject* debugScope = DebugScopes::hasDebugScope(cx, si))
        return debugScope;

    ScopeIter copy(cx, si);
    RootedObject enclosingDebug(cx, GetDebugScope(cx, ++copy));
    if (!enclosingDebug)
        return nullptr;

    /*
     * Create the missing scope object. For block objects, this takes care of
     * storing variable values after the stack frame has been popped. For call
     * objects, we only use the pretend call object to access callee, bindings
     * and to receive dynamically added properties. Together, this provides the
     * nice invariant that every DebugScopeObject has a ScopeObject.
     *
     * Note: to preserve scopeChain depth invariants, these lazily-reified
     * scopes must not be put on the frame's scope chain; instead, they are
     * maintained via DebugScopes hooks.
     */
    DebugScopeObject* debugScope = nullptr;
    switch (si.type()) {
      case ScopeIter::Call: {
        RootedFunction callee(cx, &si.fun());
        // Generators should always reify their scopes.
        MOZ_ASSERT(!callee->isGenerator());

        Rooted<CallObject*> callobj(cx);
        if (si.withinInitialFrame())
            callobj = CallObject::createForFunction(cx, si.initialFrame());
        else
            callobj = CallObject::createHollowForDebug(cx, callee);
        if (!callobj)
            return nullptr;

        if (callobj->enclosingScope().is<DeclEnvObject>()) {
            MOZ_ASSERT(CallObjectLambdaName(callobj->callee()));
            DeclEnvObject& declenv = callobj->enclosingScope().as<DeclEnvObject>();
            enclosingDebug = DebugScopeObject::create(cx, declenv, enclosingDebug);
            if (!enclosingDebug)
                return nullptr;
        }

        debugScope = DebugScopeObject::create(cx, *callobj, enclosingDebug);
        break;
      }
      case ScopeIter::Block: {
        // Generators should always reify their scopes.
        MOZ_ASSERT_IF(si.withinInitialFrame() && si.initialFrame().isFunctionFrame(),
                      !si.initialFrame().callee()->isGenerator());

        Rooted<StaticBlockObject*> staticBlock(cx, &si.staticBlock());
        ClonedBlockObject* block;
        if (si.withinInitialFrame())
            block = ClonedBlockObject::create(cx, staticBlock, si.initialFrame());
        else
            block = ClonedBlockObject::createHollowForDebug(cx, staticBlock);
        if (!block)
            return nullptr;

        debugScope = DebugScopeObject::create(cx, *block, enclosingDebug);
        break;
      }
      case ScopeIter::With:
      case ScopeIter::Eval:
        MOZ_CRASH("should already have a scope");
    }
    if (!debugScope)
        return nullptr;

    if (!DebugScopes::addDebugScope(cx, si, *debugScope))
        return nullptr;

    return debugScope;
}

static JSObject*
GetDebugScopeForNonScopeObject(const ScopeIter& si)
{
    JSObject& enclosing = si.enclosingScope();
    MOZ_ASSERT(!enclosing.is<ScopeObject>());
#ifdef DEBUG
    JSObject* o = &enclosing;
    while ((o = o->enclosingScope()))
        MOZ_ASSERT(!o->is<ScopeObject>());
#endif
    return &enclosing;
}

static JSObject*
GetDebugScope(JSContext* cx, const ScopeIter& si)
{
    JS_CHECK_RECURSION(cx, return nullptr);

    if (si.done())
        return GetDebugScopeForNonScopeObject(si);

    if (si.hasScopeObject())
        return GetDebugScopeForScope(cx, si);

    if (si.canHaveScopeObject())
        return GetDebugScopeForMissing(cx, si);

    ScopeIter copy(cx, si);
    return GetDebugScope(cx, ++copy);
}

JSObject*
js::GetDebugScopeForFunction(JSContext* cx, HandleFunction fun)
{
    assertSameCompartment(cx, fun);
    MOZ_ASSERT(CanUseDebugScopeMaps(cx));
    if (!DebugScopes::updateLiveScopes(cx))
        return nullptr;
    ScopeIter si(cx, fun->environment(), fun->nonLazyScript()->enclosingStaticScope());
    return GetDebugScope(cx, si);
}

JSObject*
js::GetDebugScopeForFrame(JSContext* cx, AbstractFramePtr frame, jsbytecode* pc)
{
    assertSameCompartment(cx, frame);
    if (CanUseDebugScopeMaps(cx) && !DebugScopes::updateLiveScopes(cx))
        return nullptr;
    ScopeIter si(cx, frame, pc);
    return GetDebugScope(cx, si);
}

// See declaration and documentation in jsfriendapi.h
JS_FRIEND_API(JSObject*)
js::GetObjectEnvironmentObjectForFunction(JSFunction* fun)
{
    if (!fun->isInterpreted())
        return fun->getParent();

    JSObject* env = fun->environment();
    if (!env || !env->is<DynamicWithObject>())
        return fun->getParent();

    return &env->as<DynamicWithObject>().object();
}

#ifdef DEBUG

typedef HashSet<PropertyName*> PropertyNameSet;

static bool
RemoveReferencedNames(JSContext* cx, HandleScript script, PropertyNameSet& remainingNames)
{
    // Remove from remainingNames --- the closure variables in some outer
    // script --- any free variables in this script. This analysis isn't perfect:
    //
    // - It will not account for free variables in an inner script which are
    //   actually accessing some name in an intermediate script between the
    //   inner and outer scripts. This can cause remainingNames to be an
    //   underapproximation.
    //
    // - It will not account for new names introduced via eval. This can cause
    //   remainingNames to be an overapproximation. This would be easy to fix
    //   but is nice to have as the eval will probably not access these
    //   these names and putting eval in an inner script is bad news if you
    //   care about entraining variables unnecessarily.

    for (jsbytecode* pc = script->code(); pc != script->codeEnd(); pc += GetBytecodeLength(pc)) {
        PropertyName* name;

        switch (JSOp(*pc)) {
          case JSOP_GETNAME:
          case JSOP_SETNAME:
            name = script->getName(pc);
            break;

          case JSOP_GETALIASEDVAR:
          case JSOP_SETALIASEDVAR:
            name = ScopeCoordinateName(cx->runtime()->scopeCoordinateNameCache, script, pc);
            break;

          default:
            name = nullptr;
            break;
        }

        if (name)
            remainingNames.remove(name);
    }

    if (script->hasObjects()) {
        ObjectArray* objects = script->objects();
        for (size_t i = 0; i < objects->length; i++) {
            JSObject* obj = objects->vector[i];
            if (obj->is<JSFunction>() && obj->as<JSFunction>().isInterpreted()) {
                JSFunction* fun = &obj->as<JSFunction>();
                RootedScript innerScript(cx, fun->getOrCreateScript(cx));
                if (!innerScript)
                    return false;

                if (!RemoveReferencedNames(cx, innerScript, remainingNames))
                    return false;
            }
        }
    }

    return true;
}

static bool
AnalyzeEntrainedVariablesInScript(JSContext* cx, HandleScript script, HandleScript innerScript)
{
    PropertyNameSet remainingNames(cx);
    if (!remainingNames.init())
        return false;

    for (BindingIter bi(script); bi; bi++) {
        if (bi->aliased()) {
            PropertyNameSet::AddPtr p = remainingNames.lookupForAdd(bi->name());
            if (!p && !remainingNames.add(p, bi->name()))
                return false;
        }
    }

    if (!RemoveReferencedNames(cx, innerScript, remainingNames))
        return false;

    if (!remainingNames.empty()) {
        Sprinter buf(cx);
        if (!buf.init())
            return false;

        buf.printf("Script ");

        if (JSAtom* name = script->functionNonDelazifying()->displayAtom()) {
            buf.putString(name);
            buf.printf(" ");
        }

        buf.printf("(%s:%d) has variables entrained by ", script->filename(), script->lineno());

        if (JSAtom* name = innerScript->functionNonDelazifying()->displayAtom()) {
            buf.putString(name);
            buf.printf(" ");
        }

        buf.printf("(%s:%d) ::", innerScript->filename(), innerScript->lineno());

        for (PropertyNameSet::Range r = remainingNames.all(); !r.empty(); r.popFront()) {
            buf.printf(" ");
            buf.putString(r.front());
        }

        printf("%s\n", buf.string());
    }

    if (innerScript->hasObjects()) {
        ObjectArray* objects = innerScript->objects();
        for (size_t i = 0; i < objects->length; i++) {
            JSObject* obj = objects->vector[i];
            if (obj->is<JSFunction>() && obj->as<JSFunction>().isInterpreted()) {
                JSFunction* fun = &obj->as<JSFunction>();
                RootedScript innerInnerScript(cx, fun->getOrCreateScript(cx));
                if (!innerInnerScript ||
                    !AnalyzeEntrainedVariablesInScript(cx, script, innerInnerScript))
                {
                    return false;
                }
            }
        }
    }

    return true;
}

// Look for local variables in script or any other script inner to it, which are
// part of the script's call object and are unnecessarily entrained by their own
// inner scripts which do not refer to those variables. An example is:
//
// function foo() {
//   var a, b;
//   function bar() { return a; }
//   function baz() { return b; }
// }
//
// |bar| unnecessarily entrains |b|, and |baz| unnecessarily entrains |a|.
bool
js::AnalyzeEntrainedVariables(JSContext* cx, HandleScript script)
{
    if (!script->hasObjects())
        return true;

    ObjectArray* objects = script->objects();
    for (size_t i = 0; i < objects->length; i++) {
        JSObject* obj = objects->vector[i];
        if (obj->is<JSFunction>() && obj->as<JSFunction>().isInterpreted()) {
            JSFunction* fun = &obj->as<JSFunction>();
            RootedScript innerScript(cx, fun->getOrCreateScript(cx));
            if (!innerScript)
                return false;

            if (script->functionDelazifying() && script->functionDelazifying()->isHeavyweight()) {
                if (!AnalyzeEntrainedVariablesInScript(cx, script, innerScript))
                    return false;
            }

            if (!AnalyzeEntrainedVariables(cx, innerScript))
                return false;
        }
    }

    return true;
}

#endif
