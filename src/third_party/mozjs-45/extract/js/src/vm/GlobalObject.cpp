/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/GlobalObject.h"

#include "jscntxt.h"
#include "jsdate.h"
#include "jsexn.h"
#include "jsfriendapi.h"
#include "jsmath.h"
#include "json.h"
#include "jsprototypes.h"
#include "jsweakmap.h"

#include "builtin/AtomicsObject.h"
#include "builtin/Eval.h"
#if EXPOSE_INTL_API
# include "builtin/Intl.h"
#endif
#include "builtin/MapObject.h"
#include "builtin/ModuleObject.h"
#include "builtin/Object.h"
#include "builtin/RegExp.h"
#include "builtin/SIMD.h"
#include "builtin/SymbolObject.h"
#include "builtin/TypedObject.h"
#include "builtin/WeakMapObject.h"
#include "builtin/WeakSetObject.h"
#include "vm/HelperThreads.h"
#include "vm/PIC.h"
#include "vm/RegExpStatics.h"
#include "vm/RegExpStaticsObject.h"
#include "vm/ScopeObject.h"
#include "vm/StopIterationObject.h"

#include "jscompartmentinlines.h"
#include "jsobjinlines.h"
#include "jsscriptinlines.h"

#include "vm/NativeObject-inl.h"

using namespace js;

struct ProtoTableEntry {
    const Class* clasp;
    ClassInitializerOp init;
};

namespace js {

#define DECLARE_PROTOTYPE_CLASS_INIT(name,code,init,clasp) \
    extern JSObject* init(JSContext* cx, Handle<JSObject*> obj);
JS_FOR_EACH_PROTOTYPE(DECLARE_PROTOTYPE_CLASS_INIT)
#undef DECLARE_PROTOTYPE_CLASS_INIT

} // namespace js

JSObject*
js::InitViaClassSpec(JSContext* cx, Handle<JSObject*> obj)
{
    MOZ_CRASH("InitViaClassSpec() should not be called.");
}

static const ProtoTableEntry protoTable[JSProto_LIMIT] = {
#define INIT_FUNC(name,code,init,clasp) { clasp, init },
#define INIT_FUNC_DUMMY(name,code,init,clasp) { nullptr, nullptr },
    JS_FOR_PROTOTYPES(INIT_FUNC, INIT_FUNC_DUMMY)
#undef INIT_FUNC_DUMMY
#undef INIT_FUNC
};

JS_FRIEND_API(const js::Class*)
js::ProtoKeyToClass(JSProtoKey key)
{
    MOZ_ASSERT(key < JSProto_LIMIT);
    return protoTable[key].clasp;
}

// This method is not in the header file to avoid having to include
// TypedObject.h from GlobalObject.h. It is not generally perf
// sensitive.
TypedObjectModuleObject&
js::GlobalObject::getTypedObjectModule() const {
    Value v = getConstructor(JSProto_TypedObject);
    // only gets called from contexts where TypedObject must be initialized
    MOZ_ASSERT(v.isObject());
    return v.toObject().as<TypedObjectModuleObject>();
}

/* static */ bool
GlobalObject::ensureConstructor(JSContext* cx, Handle<GlobalObject*> global, JSProtoKey key)
{
    if (global->isStandardClassResolved(key))
        return true;
    return resolveConstructor(cx, global, key);
}

/* static*/ bool
GlobalObject::resolveConstructor(JSContext* cx, Handle<GlobalObject*> global, JSProtoKey key)
{
    MOZ_ASSERT(!global->isStandardClassResolved(key));

    // There are two different kinds of initialization hooks. One of them is
    // the class js::InitFoo hook, defined in a JSProtoKey-keyed table at the
    // top of this file. The other lives in the ClassSpec for classes that
    // define it. Classes may use one or the other, but not both.
    ClassInitializerOp init = protoTable[key].init;
    if (init == InitViaClassSpec)
        init = nullptr;

    const Class* clasp = ProtoKeyToClass(key);
    if (!init && !clasp)
        return true;  // JSProto_Null or a compile-time-disabled feature.

    // Some classes have no init routine, which means that they're disabled at
    // compile-time. We could try to enforce that callers never pass such keys
    // to resolveConstructor, but that would cramp the style of consumers like
    // GlobalObject::initStandardClasses that want to just carpet-bomb-call
    // ensureConstructor with every JSProtoKey. So it's easier to just handle
    // it here.
    bool haveSpec = clasp && clasp->spec.defined();
    if (!init && !haveSpec)
        return true;

    // See if there's an old-style initialization hook.
    if (init) {
        MOZ_ASSERT(!haveSpec);
        return init(cx, global);
    }

    //
    // Ok, we're doing it with a class spec.
    //

    // We need to create the prototype first, and immediately stash it in the
    // slot. This is so the following bootstrap ordering is possible:
    // * Object.prototype
    // * Function.prototype
    // * Function
    // * Object
    //
    // We get the above when Object is resolved before Function. If Function
    // is resolved before Object, we'll end up re-entering resolveConstructor
    // for Function, which is a problem. So if Function is being resolved before
    // Object.prototype exists, we just resolve Object instead, since we know that
    // Function will also be resolved before we return.
    if (key == JSProto_Function && global->getPrototype(JSProto_Object).isUndefined())
        return resolveConstructor(cx, global, JSProto_Object);

    // We don't always have a prototype (i.e. Math and JSON). If we don't,
    // |createPrototype|, |prototypeFunctions|, and |prototypeProperties|
    // should all be null.
    RootedObject proto(cx);
    if (clasp->spec.createPrototypeHook()) {
        proto = clasp->spec.createPrototypeHook()(cx, key);
        if (!proto)
            return false;

        global->setPrototype(key, ObjectValue(*proto));
    }

    // Create the constructor.
    RootedObject ctor(cx, clasp->spec.createConstructorHook()(cx, key));
    if (!ctor)
        return false;

    RootedId id(cx, NameToId(ClassName(key, cx)));
    if (clasp->spec.shouldDefineConstructor()) {
        if (!global->addDataProperty(cx, id, constructorPropertySlot(key), 0))
            return false;
    }

    global->setConstructor(key, ObjectValue(*ctor));
    global->setConstructorPropertySlot(key, ObjectValue(*ctor));

    // Define any specified functions and properties, unless we're a dependent
    // standard class (in which case they live on the prototype).
    if (!StandardClassIsDependent(key)) {
        if (const JSFunctionSpec* funs = clasp->spec.prototypeFunctions()) {
            if (!JS_DefineFunctions(cx, proto, funs, DontDefineLateProperties))
                return false;
        }
        if (const JSPropertySpec* props = clasp->spec.prototypeProperties()) {
            if (!JS_DefineProperties(cx, proto, props))
                return false;
        }
        if (const JSFunctionSpec* funs = clasp->spec.constructorFunctions()) {
            if (!JS_DefineFunctions(cx, ctor, funs, DontDefineLateProperties))
                return false;
        }
        if (const JSPropertySpec* props = clasp->spec.constructorProperties()) {
            if (!JS_DefineProperties(cx, ctor, props))
                return false;
        }
    }

    // If the prototype exists, link it with the constructor.
    if (proto && !LinkConstructorAndPrototype(cx, ctor, proto))
        return false;

    // Call the post-initialization hook, if provided.
    if (clasp->spec.finishInitHook() && !clasp->spec.finishInitHook()(cx, ctor, proto))
        return false;

    if (clasp->spec.shouldDefineConstructor()) {
        // Stash type information, so that what we do here is equivalent to
        // initBuiltinConstructor.
        AddTypePropertyId(cx, global, id, ObjectValue(*ctor));
    }

    return true;
}

/* static */ bool
GlobalObject::initBuiltinConstructor(JSContext* cx, Handle<GlobalObject*> global,
                                     JSProtoKey key, HandleObject ctor, HandleObject proto)
{
    MOZ_ASSERT(!global->empty()); // reserved slots already allocated
    MOZ_ASSERT(key != JSProto_Null);
    MOZ_ASSERT(ctor);
    MOZ_ASSERT(proto);

    RootedId id(cx, NameToId(ClassName(key, cx)));
    MOZ_ASSERT(!global->lookup(cx, id));

    if (!global->addDataProperty(cx, id, constructorPropertySlot(key), 0))
        return false;

    global->setConstructor(key, ObjectValue(*ctor));
    global->setPrototype(key, ObjectValue(*proto));
    global->setConstructorPropertySlot(key, ObjectValue(*ctor));

    AddTypePropertyId(cx, global, id, ObjectValue(*ctor));
    return true;
}

GlobalObject*
GlobalObject::createInternal(JSContext* cx, const Class* clasp)
{
    MOZ_ASSERT(clasp->flags & JSCLASS_IS_GLOBAL);
    MOZ_ASSERT(clasp->trace == JS_GlobalObjectTraceHook);

    JSObject* obj = NewObjectWithGivenProto(cx, clasp, nullptr, SingletonObject);
    if (!obj)
        return nullptr;

    Rooted<GlobalObject*> global(cx, &obj->as<GlobalObject>());
    MOZ_ASSERT(global->isUnqualifiedVarObj());

    // Initialize the private slot to null if present, as GC can call class
    // hooks before the caller gets to set this to a non-garbage value.
    if (clasp->flags & JSCLASS_HAS_PRIVATE)
        global->setPrivate(nullptr);

    Rooted<ClonedBlockObject*> lexical(cx, ClonedBlockObject::createGlobal(cx, global));
    if (!lexical)
        return nullptr;
    global->setReservedSlot(LEXICAL_SCOPE, ObjectValue(*lexical));

    cx->compartment()->initGlobal(*global);

    if (!global->setQualifiedVarObj(cx))
        return nullptr;
    if (!global->setDelegate(cx))
        return nullptr;

    return global;
}

GlobalObject*
GlobalObject::new_(JSContext* cx, const Class* clasp, JSPrincipals* principals,
                   JS::OnNewGlobalHookOption hookOption,
                   const JS::CompartmentOptions& options)
{
    MOZ_ASSERT(!cx->isExceptionPending());
    MOZ_ASSERT(!cx->runtime()->isAtomsCompartment(cx->compartment()));

    JSRuntime* rt = cx->runtime();

    Zone* zone;
    if (options.zoneSpecifier() == JS::SystemZone)
        zone = rt->gc.systemZone;
    else if (options.zoneSpecifier() == JS::FreshZone)
        zone = nullptr;
    else
        zone = static_cast<Zone*>(options.zonePointer());

    JSCompartment* compartment = NewCompartment(cx, zone, principals, options);
    if (!compartment)
        return nullptr;

    // Lazily create the system zone.
    if (!rt->gc.systemZone && options.zoneSpecifier() == JS::SystemZone) {
        rt->gc.systemZone = compartment->zone();
        rt->gc.systemZone->isSystem = true;
    }

    Rooted<GlobalObject*> global(cx);
    {
        AutoCompartment ac(cx, compartment);
        global = GlobalObject::createInternal(cx, clasp);
        if (!global)
            return nullptr;
    }

    if (hookOption == JS::FireOnNewGlobalHook)
        JS_FireOnNewGlobalObject(cx, global);

    return global;
}

ClonedBlockObject&
GlobalObject::lexicalScope() const
{
    return getReservedSlot(LEXICAL_SCOPE).toObject().as<ClonedBlockObject>();
}

/* static */ bool
GlobalObject::getOrCreateEval(JSContext* cx, Handle<GlobalObject*> global,
                              MutableHandleObject eval)
{
    if (!global->getOrCreateObjectPrototype(cx))
        return false;
    eval.set(&global->getSlot(EVAL).toObject());
    return true;
}

bool
GlobalObject::valueIsEval(Value val)
{
    Value eval = getSlot(EVAL);
    return eval.isObject() && eval == val;
}

/* static */ bool
GlobalObject::initStandardClasses(JSContext* cx, Handle<GlobalObject*> global)
{
    /* Define a top-level property 'undefined' with the undefined value. */
    if (!DefineProperty(cx, global, cx->names().undefined, UndefinedHandleValue,
                        nullptr, nullptr, JSPROP_PERMANENT | JSPROP_READONLY | JSPROP_RESOLVING))
    {
        return false;
    }

    for (size_t k = 0; k < JSProto_LIMIT; ++k) {
        if (!ensureConstructor(cx, global, static_cast<JSProtoKey>(k)))
            return false;
    }
    return true;
}

/**
 * Initializes a builtin constructor and its prototype without defining any
 * properties or functions on it.
 *
 * Used in self-hosting to install the few builtin constructors required by
 * self-hosted builtins.
 */
static bool
InitBareBuiltinCtor(JSContext* cx, Handle<GlobalObject*> global, JSProtoKey protoKey)
{
    MOZ_ASSERT(cx->runtime()->isSelfHostingGlobal(global));
    const Class* clasp = ProtoKeyToClass(protoKey);
    RootedObject proto(cx);
    proto = clasp->spec.createPrototypeHook()(cx, protoKey);
    if (!proto)
        return false;

    RootedObject ctor(cx, clasp->spec.createConstructorHook()(cx, protoKey));
    if (!ctor)
        return false;

    return GlobalObject::initBuiltinConstructor(cx, global, protoKey, ctor, proto);
}

/**
 * The self-hosting global only gets a small subset of all standard classes.
 * Even those are only created as bare constructors without any properties
 * or functions.
 */
/* static */ bool
GlobalObject::initSelfHostingBuiltins(JSContext* cx, Handle<GlobalObject*> global,
                                      const JSFunctionSpec* builtins)
{
    // Define a top-level property 'undefined' with the undefined value.
    if (!DefineProperty(cx, global, cx->names().undefined, UndefinedHandleValue,
                        nullptr, nullptr, JSPROP_PERMANENT | JSPROP_READONLY))
    {
        return false;
    }

    // Define a top-level property 'std_iterator' with the name of the method
    // used by for-of loops to create an iterator.
    RootedValue std_iterator(cx);
    std_iterator.setSymbol(cx->wellKnownSymbols().get(JS::SymbolCode::iterator));
    if (!JS_DefineProperty(cx, global, "std_iterator", std_iterator,
                           JSPROP_PERMANENT | JSPROP_READONLY))
    {
        return false;
    }

    RootedValue std_species(cx);
    std_species.setSymbol(cx->wellKnownSymbols().get(JS::SymbolCode::species));
    if (!JS_DefineProperty(cx, global, "std_species", std_species,
                           JSPROP_PERMANENT | JSPROP_READONLY))
    {
        return false;
    }

    return InitBareBuiltinCtor(cx, global, JSProto_Array) &&
           InitBareBuiltinCtor(cx, global, JSProto_TypedArray) &&
           InitBareBuiltinCtor(cx, global, JSProto_Uint8Array) &&
           InitBareBuiltinCtor(cx, global, JSProto_Uint32Array) &&
           InitBareWeakMapCtor(cx, global) &&
           InitStopIterationClass(cx, global) &&
           InitSelfHostingCollectionIteratorFunctions(cx, global) &&
           JS_DefineFunctions(cx, global, builtins);
}

/* static */ bool
GlobalObject::isRuntimeCodeGenEnabled(JSContext* cx, Handle<GlobalObject*> global)
{
    HeapSlot& v = global->getSlotRef(RUNTIME_CODEGEN_ENABLED);
    if (v.isUndefined()) {
        /*
         * If there are callbacks, make sure that the CSP callback is installed
         * and that it permits runtime code generation, then cache the result.
         */
        JSCSPEvalChecker allows = cx->runtime()->securityCallbacks->contentSecurityPolicyAllows;
        Value boolValue = BooleanValue(!allows || allows(cx));
        v.set(global, HeapSlot::Slot, RUNTIME_CODEGEN_ENABLED, boolValue);
    }
    return !v.isFalse();
}

/* static */ bool
GlobalObject::warnOnceAbout(JSContext* cx, HandleObject obj, WarnOnceFlag flag,
                            unsigned errorNumber)
{
    Rooted<GlobalObject*> global(cx, &obj->global());
    HeapSlot& v = global->getSlotRef(WARNED_ONCE_FLAGS);
    MOZ_ASSERT_IF(!v.isUndefined(), v.toInt32());
    int32_t flags = v.isUndefined() ? 0 : v.toInt32();
    if (!(flags & flag)) {
        if (!JS_ReportErrorFlagsAndNumber(cx, JSREPORT_WARNING, GetErrorMessage, nullptr,
                                          errorNumber))
        {
            return false;
        }
        if (v.isUndefined())
            v.init(global, HeapSlot::Slot, WARNED_ONCE_FLAGS, Int32Value(flags | flag));
        else
            v.set(global, HeapSlot::Slot, WARNED_ONCE_FLAGS, Int32Value(flags | flag));
    }
    return true;
}

JSFunction*
GlobalObject::createConstructor(JSContext* cx, Native ctor, JSAtom* nameArg, unsigned length,
                                gc::AllocKind kind, const JSJitInfo* jitInfo)
{
    RootedAtom name(cx, nameArg);
    JSFunction* fun = NewNativeConstructor(cx, ctor, length, name, kind);
    if (!fun)
        return nullptr;

    if (jitInfo)
        fun->setJitInfo(jitInfo);

    return fun;
}

static NativeObject*
CreateBlankProto(JSContext* cx, const Class* clasp, HandleObject proto, HandleObject global)
{
    MOZ_ASSERT(clasp != &JSFunction::class_);

    RootedNativeObject blankProto(cx, NewNativeObjectWithGivenProto(cx, clasp, proto,
                                                                    SingletonObject));
    if (!blankProto || !blankProto->setDelegate(cx))
        return nullptr;

    return blankProto;
}

NativeObject*
GlobalObject::createBlankPrototype(JSContext* cx, const Class* clasp)
{
    Rooted<GlobalObject*> self(cx, this);
    RootedObject objectProto(cx, getOrCreateObjectPrototype(cx));
    if (!objectProto)
        return nullptr;

    return CreateBlankProto(cx, clasp, objectProto, self);
}

NativeObject*
GlobalObject::createBlankPrototypeInheriting(JSContext* cx, const Class* clasp, HandleObject proto)
{
    Rooted<GlobalObject*> self(cx, this);
    return CreateBlankProto(cx, clasp, proto, self);
}

bool
js::LinkConstructorAndPrototype(JSContext* cx, JSObject* ctor_, JSObject* proto_)
{
    RootedObject ctor(cx, ctor_), proto(cx, proto_);

    RootedValue protoVal(cx, ObjectValue(*proto));
    RootedValue ctorVal(cx, ObjectValue(*ctor));

    return DefineProperty(cx, ctor, cx->names().prototype, protoVal,
                          nullptr, nullptr, JSPROP_PERMANENT | JSPROP_READONLY) &&
           DefineProperty(cx, proto, cx->names().constructor, ctorVal,
                          nullptr, nullptr, 0);
}

bool
js::DefinePropertiesAndFunctions(JSContext* cx, HandleObject obj,
                                 const JSPropertySpec* ps, const JSFunctionSpec* fs)
{
    if (ps && !JS_DefineProperties(cx, obj, ps))
        return false;
    if (fs && !JS_DefineFunctions(cx, obj, fs))
        return false;
    return true;
}

static void
GlobalDebuggees_finalize(FreeOp* fop, JSObject* obj)
{
    fop->delete_((GlobalObject::DebuggerVector*) obj->as<NativeObject>().getPrivate());
}

static const Class
GlobalDebuggees_class = {
    "GlobalDebuggee", JSCLASS_HAS_PRIVATE,
    nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, GlobalDebuggees_finalize
};

GlobalObject::DebuggerVector*
GlobalObject::getDebuggers() const
{
    Value debuggers = getReservedSlot(DEBUGGERS);
    if (debuggers.isUndefined())
        return nullptr;
    MOZ_ASSERT(debuggers.toObject().getClass() == &GlobalDebuggees_class);
    return (DebuggerVector*) debuggers.toObject().as<NativeObject>().getPrivate();
}

/* static */ GlobalObject::DebuggerVector*
GlobalObject::getOrCreateDebuggers(JSContext* cx, Handle<GlobalObject*> global)
{
    assertSameCompartment(cx, global);
    DebuggerVector* debuggers = global->getDebuggers();
    if (debuggers)
        return debuggers;

    NativeObject* obj = NewNativeObjectWithGivenProto(cx, &GlobalDebuggees_class, nullptr);
    if (!obj)
        return nullptr;
    debuggers = cx->new_<DebuggerVector>();
    if (!debuggers)
        return nullptr;
    obj->setPrivate(debuggers);
    global->setReservedSlot(DEBUGGERS, ObjectValue(*obj));
    return debuggers;
}

/* static */ NativeObject*
GlobalObject::getOrCreateForOfPICObject(JSContext* cx, Handle<GlobalObject*> global)
{
    assertSameCompartment(cx, global);
    NativeObject* forOfPIC = global->getForOfPICObject();
    if (forOfPIC)
        return forOfPIC;

    forOfPIC = ForOfPIC::createForOfPICObject(cx, global);
    if (!forOfPIC)
        return nullptr;
    global->setReservedSlot(FOR_OF_PIC_CHAIN, ObjectValue(*forOfPIC));
    return forOfPIC;
}

bool
GlobalObject::hasRegExpStatics() const
{
    return !getSlot(REGEXP_STATICS).isUndefined();
}

RegExpStatics*
GlobalObject::getRegExpStatics(ExclusiveContext* cx) const
{
    MOZ_ASSERT(cx);
    Rooted<GlobalObject*> self(cx, const_cast<GlobalObject*>(this));

    RegExpStaticsObject* resObj = nullptr;
    const Value& val = this->getSlot(REGEXP_STATICS);
    if (!val.isObject()) {
        MOZ_ASSERT(val.isUndefined());
        resObj = RegExpStatics::create(cx, self);
        if (!resObj)
            return nullptr;

        self->initSlot(REGEXP_STATICS, ObjectValue(*resObj));
    } else {
        resObj = &val.toObject().as<RegExpStaticsObject>();
    }
    return static_cast<RegExpStatics*>(resObj->getPrivate(/* nfixed = */ 1));
}

RegExpStatics*
GlobalObject::getAlreadyCreatedRegExpStatics() const
{
    const Value& val = this->getSlot(REGEXP_STATICS);
    MOZ_ASSERT(val.isObject());
    return static_cast<RegExpStatics*>(val.toObject().as<RegExpStaticsObject>().getPrivate(/* nfixed = */ 1));
}

/* static */ NativeObject*
GlobalObject::getIntrinsicsHolder(JSContext* cx, Handle<GlobalObject*> global)
{
    Value slot = global->getReservedSlot(INTRINSICS);
    MOZ_ASSERT(slot.isUndefined() || slot.isObject());

    if (slot.isObject())
        return &slot.toObject().as<NativeObject>();

    Rooted<NativeObject*> intrinsicsHolder(cx);
    bool isSelfHostingGlobal = cx->runtime()->isSelfHostingGlobal(global);
    if (isSelfHostingGlobal) {
        intrinsicsHolder = global;
    } else {
        intrinsicsHolder = NewObjectWithGivenProto<PlainObject>(cx, nullptr, TenuredObject);
        if (!intrinsicsHolder)
            return nullptr;
    }

    /* Define a property 'global' with the current global as its value. */
    RootedValue globalValue(cx, ObjectValue(*global));
    if (!DefineProperty(cx, intrinsicsHolder, cx->names().global, globalValue,
                        nullptr, nullptr, JSPROP_PERMANENT | JSPROP_READONLY))
    {
        return nullptr;
    }

    // Install the intrinsics holder in the intrinsics.
    global->setReservedSlot(INTRINSICS, ObjectValue(*intrinsicsHolder));
    return intrinsicsHolder;
}

/* static */ bool
GlobalObject::getSelfHostedFunction(JSContext* cx, Handle<GlobalObject*> global,
                                    HandlePropertyName selfHostedName, HandleAtom name,
                                    unsigned nargs, MutableHandleValue funVal)
{
    if (GlobalObject::maybeGetIntrinsicValue(cx, global, selfHostedName, funVal))
        return true;

    JSFunction* fun =
        NewScriptedFunction(cx, nargs, JSFunction::INTERPRETED_LAZY,
                            name, gc::AllocKind::FUNCTION_EXTENDED, SingletonObject);
    if (!fun)
        return false;
    fun->setIsSelfHostedBuiltin();
    fun->setExtendedSlot(LAZY_FUNCTION_NAME_SLOT, StringValue(selfHostedName));
    funVal.setObject(*fun);

    return GlobalObject::addIntrinsicValue(cx, global, selfHostedName, funVal);
}

/* static */ bool
GlobalObject::addIntrinsicValue(JSContext* cx, Handle<GlobalObject*> global,
                                HandlePropertyName name, HandleValue value)
{
    RootedNativeObject holder(cx, GlobalObject::getIntrinsicsHolder(cx, global));
    if (!holder)
        return false;

    uint32_t slot = holder->slotSpan();
    RootedShape last(cx, holder->lastProperty());
    Rooted<UnownedBaseShape*> base(cx, last->base()->unowned());

    RootedId id(cx, NameToId(name));
    Rooted<StackShape> child(cx, StackShape(base, id, slot, 0, 0));
    Shape* shape = cx->compartment()->propertyTree.getChild(cx, last, child);
    if (!shape)
        return false;

    if (!holder->setLastProperty(cx, shape))
        return false;

    holder->setSlot(shape->slot(), value);
    return true;
}
