/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/GlobalObject.h"

#include "jsdate.h"
#include "jsexn.h"
#include "jsfriendapi.h"
#include "jsmath.h"

#include "builtin/AtomicsObject.h"
#include "builtin/DataViewObject.h"
#include "builtin/Eval.h"
#include "builtin/JSON.h"
#include "builtin/MapObject.h"
#include "builtin/ModuleObject.h"
#include "builtin/Object.h"
#include "builtin/Promise.h"
#include "builtin/RegExp.h"
#include "builtin/SelfHostingDefines.h"
#include "builtin/Stream.h"
#include "builtin/Symbol.h"
#include "builtin/TypedObject.h"
#include "builtin/WeakMapObject.h"
#include "builtin/WeakSetObject.h"
#include "gc/FreeOp.h"
#include "js/ProtoKey.h"
#include "vm/Debugger.h"
#include "vm/EnvironmentObject.h"
#include "vm/HelperThreads.h"
#include "vm/JSContext.h"
#include "vm/PIC.h"
#include "vm/RegExpStatics.h"
#include "vm/RegExpStaticsObject.h"
#include "wasm/WasmJS.h"

#include "vm/JSCompartment-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

struct ProtoTableEntry {
    const Class* clasp;
    ClassInitializerOp init;
};

namespace js {

#define DECLARE_PROTOTYPE_CLASS_INIT(name,init,clasp) \
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
#define INIT_FUNC(name,init,clasp) { clasp, init },
#define INIT_FUNC_DUMMY(name,init,clasp) { nullptr, nullptr },
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
GlobalObject::skipDeselectedConstructor(JSContext* cx, JSProtoKey key)
{
    switch (key) {
      case JSProto_WebAssembly:
        return !wasm::HasSupport(cx);

      case JSProto_ReadableStream:
      case JSProto_ReadableStreamDefaultReader:
      case JSProto_ReadableStreamBYOBReader:
      case JSProto_ReadableStreamDefaultController:
      case JSProto_ReadableByteStreamController:
      case JSProto_ReadableStreamBYOBRequest:
      case JSProto_ByteLengthQueuingStrategy:
      case JSProto_CountQueuingStrategy:
        return !cx->options().streams();

      // Return true if the given constructor has been disabled at run-time.
      case JSProto_Atomics:
      case JSProto_SharedArrayBuffer:
        return !cx->compartment()->creationOptions().getSharedMemoryAndAtomicsEnabled();
      default:
        return false;
    }
}

/* static*/ bool
GlobalObject::resolveConstructor(JSContext* cx, Handle<GlobalObject*> global, JSProtoKey key)
{
    MOZ_ASSERT(!global->isStandardClassResolved(key));

    if (global->zone()->group()->createdForHelperThread())
        return resolveOffThreadConstructor(cx, global, key);

    MOZ_ASSERT(!cx->helperThread());

    // Prohibit collection of allocation metadata. Metadata builders shouldn't
    // need to observe lazily-constructed prototype objects coming into
    // existence. And assertions start to fail when the builder itself attempts
    // an allocation that re-entrantly tries to create the same prototype.
    AutoSuppressAllocationMetadataBuilder suppressMetadata(cx);

    // Constructor resolution may execute self-hosted scripts. These
    // self-hosted scripts do not call out to user code by construction. Allow
    // all scripts to execute, even in debuggee compartments that are paused.
    AutoSuppressDebuggeeNoExecuteChecks suppressNX(cx);

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

    if (skipDeselectedConstructor(cx, key))
        return true;

    // Some classes have no init routine, which means that they're disabled at
    // compile-time. We could try to enforce that callers never pass such keys
    // to resolveConstructor, but that would cramp the style of consumers like
    // GlobalObject::initStandardClasses that want to just carpet-bomb-call
    // ensureConstructor with every JSProtoKey. So it's easier to just handle
    // it here.
    bool haveSpec = clasp && clasp->specDefined();
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

    bool isObjectOrFunction = key == JSProto_Function || key == JSProto_Object;

    // We need to create the prototype first, and immediately stash it in the
    // slot. This is so the following bootstrap ordering is possible:
    // * Object.prototype
    // * Function.prototype
    // * Function
    // * Object
    //
    // We get the above when Object is resolved before Function. If Function
    // is resolved before Object, we'll end up re-entering resolveConstructor
    // for Function, which is a problem. So if Function is being resolved
    // before Object.prototype exists, we just resolve Object instead, since we
    // know that Function will also be resolved before we return.
    if (key == JSProto_Function && global->getPrototype(JSProto_Object).isUndefined())
        return resolveConstructor(cx, global, JSProto_Object);

    // We don't always have a prototype (i.e. Math and JSON). If we don't,
    // |createPrototype|, |prototypeFunctions|, and |prototypeProperties|
    // should all be null.
    RootedObject proto(cx);
    if (ClassObjectCreationOp createPrototype = clasp->specCreatePrototypeHook()) {
        proto = createPrototype(cx, key);
        if (!proto)
            return false;

        if (isObjectOrFunction) {
            // Make sure that creating the prototype didn't recursively resolve
            // our own constructor. We can't just assert that there's no
            // prototype; OOMs can result in incomplete resolutions in which
            // the prototype is saved but not the constructor. So use the same
            // criteria that protects entry into this function.
            MOZ_ASSERT(!global->isStandardClassResolved(key));

            global->setPrototype(key, ObjectValue(*proto));
        }
    }

    // Create the constructor.
    RootedObject ctor(cx, clasp->specCreateConstructorHook()(cx, key));
    if (!ctor)
        return false;

    RootedId id(cx, NameToId(ClassName(key, cx)));
    if (isObjectOrFunction) {
        if (clasp->specShouldDefineConstructor()) {
            RootedValue ctorValue(cx, ObjectValue(*ctor));
            if (!DefineDataProperty(cx, global, id, ctorValue, JSPROP_RESOLVING))
                return false;
        }

        global->setConstructor(key, ObjectValue(*ctor));
    }

    // If we're operating on the self-hosting global, we don't want any
    // functions and properties on the builtins and their prototypes.
    if (!cx->runtime()->isSelfHostingGlobal(global)) {
        if (const JSFunctionSpec* funs = clasp->specPrototypeFunctions()) {
            if (!JS_DefineFunctions(cx, proto, funs))
                return false;
        }
        if (const JSPropertySpec* props = clasp->specPrototypeProperties()) {
            if (!JS_DefineProperties(cx, proto, props))
                return false;
        }
        if (const JSFunctionSpec* funs = clasp->specConstructorFunctions()) {
            if (!JS_DefineFunctions(cx, ctor, funs))
                return false;
        }
        if (const JSPropertySpec* props = clasp->specConstructorProperties()) {
            if (!JS_DefineProperties(cx, ctor, props))
                return false;
        }
    }

    // If the prototype exists, link it with the constructor.
    if (proto && !LinkConstructorAndPrototype(cx, ctor, proto))
        return false;

    // Call the post-initialization hook, if provided.
    if (FinishClassInitOp finishInit = clasp->specFinishInitHook()) {
        if (!finishInit(cx, ctor, proto))
            return false;
    }

    if (!isObjectOrFunction) {
        // Any operations that modifies the global object should be placed
        // after any other fallible operations.

        // Fallible operation that modifies the global object.
        if (clasp->specShouldDefineConstructor()) {
            RootedValue ctorValue(cx, ObjectValue(*ctor));
            if (!DefineDataProperty(cx, global, id, ctorValue, JSPROP_RESOLVING))
                return false;
        }

        // Infallible operations that modify the global object.
        global->setConstructor(key, ObjectValue(*ctor));
        if (proto)
            global->setPrototype(key, ObjectValue(*proto));
    }

    return true;
}

/* static */ JSObject*
GlobalObject::createObject(JSContext* cx, Handle<GlobalObject*> global, unsigned slot, ObjectInitOp init)
{
    if (global->zone()->group()->createdForHelperThread())
        return createOffThreadObject(cx, global, slot);

    MOZ_ASSERT(!cx->helperThread());
    if (!init(cx, global))
        return nullptr;

    return &global->getSlot(slot).toObject();
}

const Class GlobalObject::OffThreadPlaceholderObject::class_ = {
    "off-thread-prototype-placeholder",
    JSCLASS_IS_ANONYMOUS | JSCLASS_HAS_RESERVED_SLOTS(1)
};

/* static */ GlobalObject::OffThreadPlaceholderObject*
GlobalObject::OffThreadPlaceholderObject::New(JSContext* cx, unsigned slot)
{
    Rooted<OffThreadPlaceholderObject*> placeholder(cx);
    placeholder =
        NewObjectWithGivenTaggedProto<OffThreadPlaceholderObject>(cx, AsTaggedProto(nullptr));
    if (!placeholder)
        return nullptr;

    placeholder->setReservedSlot(SlotIndexSlot, Int32Value(slot));
    return placeholder;
}

inline int32_t
GlobalObject::OffThreadPlaceholderObject::getSlotIndex() const
{
    return getReservedSlot(SlotIndexSlot).toInt32();
}

/* static */ bool
GlobalObject::resolveOffThreadConstructor(JSContext* cx,
                                          Handle<GlobalObject*> global,
                                          JSProtoKey key)
{
    // Don't resolve constructors for off-thread parse globals. Instead create a
    // placeholder object for the prototype which we can use to find the real
    // prototype when the off-thread compartment is merged back into the target
    // compartment.

    MOZ_ASSERT(global->zone()->group()->createdForHelperThread());
    MOZ_ASSERT(key == JSProto_Object ||
               key == JSProto_Function ||
               key == JSProto_Array ||
               key == JSProto_RegExp);

    Rooted<OffThreadPlaceholderObject*> placeholder(cx);
    placeholder = OffThreadPlaceholderObject::New(cx, prototypeSlot(key));
    if (!placeholder)
        return false;

    if (key == JSProto_Object &&
        !JSObject::setFlags(cx, placeholder, BaseShape::IMMUTABLE_PROTOTYPE))
    {
        return false;
    }

    if ((key == JSProto_Object || key == JSProto_Function || key == JSProto_Array) &&
        !JSObject::setNewGroupUnknown(cx, placeholder->getClass(), placeholder))
    {
        return false;
    }

    global->setPrototype(key, ObjectValue(*placeholder));
    global->setConstructor(key, MagicValue(JS_OFF_THREAD_CONSTRUCTOR));
    return true;
}

/* static */ JSObject*
GlobalObject::createOffThreadObject(JSContext* cx, Handle<GlobalObject*> global, unsigned slot)
{
    // Don't create prototype objects for off-thread parse globals. Instead
    // create a placeholder object which we can use to find the real prototype
    // when the off-thread compartment is merged back into the target
    // compartment.

    MOZ_ASSERT(global->zone()->group()->createdForHelperThread());
    MOZ_ASSERT(slot == GENERATOR_FUNCTION_PROTO ||
               slot == MODULE_PROTO ||
               slot == IMPORT_ENTRY_PROTO ||
               slot == EXPORT_ENTRY_PROTO ||
               slot == REQUESTED_MODULE_PROTO);

    auto placeholder = OffThreadPlaceholderObject::New(cx, slot);
    if (!placeholder)
        return nullptr;

    global->setSlot(slot, ObjectValue(*placeholder));
    return placeholder;
}

JSObject*
GlobalObject::getPrototypeForOffThreadPlaceholder(JSObject* obj)
{
    auto placeholder = &obj->as<OffThreadPlaceholderObject>();
    return &getSlot(placeholder->getSlotIndex()).toObject();
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

    RootedValue ctorValue(cx, ObjectValue(*ctor));
    if (!DefineDataProperty(cx, global, id, ctorValue, JSPROP_RESOLVING))
        return false;

    global->setConstructor(key, ObjectValue(*ctor));
    global->setPrototype(key, ObjectValue(*proto));
    return true;
}

static bool
ThrowTypeError(JSContext* cx, unsigned argc, Value* vp)
{
    ThrowTypeErrorBehavior(cx);
    return false;
}

/* static */ JSObject*
GlobalObject::getOrCreateThrowTypeError(JSContext* cx, Handle<GlobalObject*> global)
{
    Value v = global->getReservedSlot(THROWTYPEERROR);
    if (v.isObject())
        return &v.toObject();
    MOZ_ASSERT(v.isUndefined());

    // Construct the unique [[%ThrowTypeError%]] function object, used only for
    // "callee" and "caller" accessors on strict mode arguments objects.  (The
    // spec also uses this for "arguments" and "caller" on various functions,
    // but we're experimenting with implementing them using accessors on
    // |Function.prototype| right now.)

    RootedFunction throwTypeError(cx, NewNativeFunction(cx, ThrowTypeError, 0, nullptr));
    if (!throwTypeError || !PreventExtensions(cx, throwTypeError))
        return nullptr;

    // The "length" property of %ThrowTypeError% is non-configurable, adjust
    // the default property attributes accordingly.
    Rooted<PropertyDescriptor> nonConfigurableDesc(cx);
    nonConfigurableDesc.setAttributes(JSPROP_PERMANENT | JSPROP_IGNORE_READONLY |
                                      JSPROP_IGNORE_ENUMERATE | JSPROP_IGNORE_VALUE);

    RootedId lengthId(cx, NameToId(cx->names().length));
    ObjectOpResult lengthResult;
    if (!NativeDefineProperty(cx, throwTypeError, lengthId, nonConfigurableDesc, lengthResult))
        return nullptr;
    MOZ_ASSERT(lengthResult);

    // Non-standard: Also change "name" to non-configurable. ECMAScript defines
    // %ThrowTypeError% as an anonymous function, i.e. it shouldn't actually
    // get an own "name" property. To be consistent with other built-in,
    // anonymous functions, we don't delete %ThrowTypeError%'s "name" property.
    RootedId nameId(cx, NameToId(cx->names().name));
    ObjectOpResult nameResult;
    if (!NativeDefineProperty(cx, throwTypeError, nameId, nonConfigurableDesc, nameResult))
        return nullptr;
    MOZ_ASSERT(nameResult);

    global->setReservedSlot(THROWTYPEERROR, ObjectValue(*throwTypeError));
    return throwTypeError;
}

GlobalObject*
GlobalObject::createInternal(JSContext* cx, const Class* clasp)
{
    MOZ_ASSERT(clasp->flags & JSCLASS_IS_GLOBAL);
    MOZ_ASSERT(clasp->isTrace(JS_GlobalObjectTraceHook));

    JSObject* obj = NewObjectWithGivenProto(cx, clasp, nullptr, SingletonObject);
    if (!obj)
        return nullptr;

    Rooted<GlobalObject*> global(cx, &obj->as<GlobalObject>());
    MOZ_ASSERT(global->isUnqualifiedVarObj());

    // Initialize the private slot to null if present, as GC can call class
    // hooks before the caller gets to set this to a non-garbage value.
    if (clasp->flags & JSCLASS_HAS_PRIVATE)
        global->setPrivate(nullptr);

    Rooted<LexicalEnvironmentObject*> lexical(cx,
        LexicalEnvironmentObject::createGlobal(cx, global));
    if (!lexical)
        return nullptr;
    global->setReservedSlot(LEXICAL_ENVIRONMENT, ObjectValue(*lexical));

    Rooted<GlobalScope*> emptyGlobalScope(cx, GlobalScope::createEmpty(cx, ScopeKind::Global));
    if (!emptyGlobalScope)
        return nullptr;
    global->setReservedSlot(EMPTY_GLOBAL_SCOPE, PrivateGCThingValue(emptyGlobalScope));

    cx->compartment()->initGlobal(*global);

    if (!JSObject::setQualifiedVarObj(cx, global))
        return nullptr;
    if (!JSObject::setDelegate(cx, global))
        return nullptr;

    return global;
}

/* static */ GlobalObject*
GlobalObject::new_(JSContext* cx, const Class* clasp, JSPrincipals* principals,
                   JS::OnNewGlobalHookOption hookOption,
                   const JS::CompartmentOptions& options)
{
    MOZ_ASSERT(!cx->isExceptionPending());
    MOZ_ASSERT(!cx->runtime()->isAtomsCompartment(cx->compartment()));

    JSCompartment* compartment = NewCompartment(cx, principals, options);
    if (!compartment)
        return nullptr;

    Rooted<GlobalObject*> global(cx);
    {
        AutoCompartmentUnchecked ac(cx, compartment);
        global = GlobalObject::createInternal(cx, clasp);
        if (!global)
            return nullptr;

        if (hookOption == JS::FireOnNewGlobalHook)
            JS_FireOnNewGlobalObject(cx, global);
    }

    return global;
}

LexicalEnvironmentObject&
GlobalObject::lexicalEnvironment() const
{
    return getReservedSlot(LEXICAL_ENVIRONMENT).toObject().as<LexicalEnvironmentObject>();
}

GlobalScope&
GlobalObject::emptyGlobalScope() const
{
    const Value& v = getReservedSlot(EMPTY_GLOBAL_SCOPE);
    MOZ_ASSERT(v.isPrivateGCThing() && v.traceKind() == JS::TraceKind::Scope);
    return static_cast<Scope*>(v.toGCThing())->as<GlobalScope>();
}

/* static */ bool
GlobalObject::getOrCreateEval(JSContext* cx, Handle<GlobalObject*> global,
                              MutableHandleObject eval)
{
    if (!getOrCreateObjectPrototype(cx, global))
        return false;
    eval.set(&global->getSlot(EVAL).toObject());
    return true;
}

bool
GlobalObject::valueIsEval(const Value& val)
{
    Value eval = getSlot(EVAL);
    return eval.isObject() && eval == val;
}

/* static */ bool
GlobalObject::initStandardClasses(JSContext* cx, Handle<GlobalObject*> global)
{
    /* Define a top-level property 'undefined' with the undefined value. */
    if (!DefineDataProperty(cx, global, cx->names().undefined, UndefinedHandleValue,
                            JSPROP_PERMANENT | JSPROP_READONLY | JSPROP_RESOLVING))
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
    proto = clasp->specCreatePrototypeHook()(cx, protoKey);
    if (!proto)
        return false;

    RootedObject ctor(cx, clasp->specCreateConstructorHook()(cx, protoKey));
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
    if (!DefineDataProperty(cx, global, cx->names().undefined, UndefinedHandleValue,
                            JSPROP_PERMANENT | JSPROP_READONLY))
    {
        return false;
    }

    RootedValue std_isConcatSpreadable(cx);
    std_isConcatSpreadable.setSymbol(cx->wellKnownSymbols().get(JS::SymbolCode::isConcatSpreadable));
    if (!JS_DefineProperty(cx, global, "std_isConcatSpreadable", std_isConcatSpreadable,
                           JSPROP_PERMANENT | JSPROP_READONLY))
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

    RootedValue std_match(cx);
    std_match.setSymbol(cx->wellKnownSymbols().get(JS::SymbolCode::match));
    if (!JS_DefineProperty(cx, global, "std_match", std_match,
                           JSPROP_PERMANENT | JSPROP_READONLY))
    {
        return false;
    }

    RootedValue std_replace(cx);
    std_replace.setSymbol(cx->wellKnownSymbols().get(JS::SymbolCode::replace));
    if (!JS_DefineProperty(cx, global, "std_replace", std_replace,
                           JSPROP_PERMANENT | JSPROP_READONLY))
    {
        return false;
    }

    RootedValue std_search(cx);
    std_search.setSymbol(cx->wellKnownSymbols().get(JS::SymbolCode::search));
    if (!JS_DefineProperty(cx, global, "std_search", std_search,
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

    RootedValue std_split(cx);
    std_split.setSymbol(cx->wellKnownSymbols().get(JS::SymbolCode::split));
    if (!JS_DefineProperty(cx, global, "std_split", std_split,
                           JSPROP_PERMANENT | JSPROP_READONLY))
    {
        return false;
    }

    return InitBareBuiltinCtor(cx, global, JSProto_Array) &&
           InitBareBuiltinCtor(cx, global, JSProto_TypedArray) &&
           InitBareBuiltinCtor(cx, global, JSProto_Uint8Array) &&
           InitBareBuiltinCtor(cx, global, JSProto_Int32Array) &&
           InitBareSymbolCtor(cx, global) &&
           DefineFunctions(cx, global, builtins, AsIntrinsic);
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

/* static */ JSFunction*
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
    if (!blankProto || !JSObject::setDelegate(cx, blankProto))
        return nullptr;

    return blankProto;
}

/* static */ NativeObject*
GlobalObject::createBlankPrototype(JSContext* cx, Handle<GlobalObject*> global, const Class* clasp)
{
    RootedObject objectProto(cx, getOrCreateObjectPrototype(cx, global));
    if (!objectProto)
        return nullptr;

    return CreateBlankProto(cx, clasp, objectProto, global);
}

/* static */ NativeObject*
GlobalObject::createBlankPrototypeInheriting(JSContext* cx, Handle<GlobalObject*> global,
                                             const Class* clasp, HandleObject proto)
{
    return CreateBlankProto(cx, clasp, proto, global);
}

bool
js::LinkConstructorAndPrototype(JSContext* cx, JSObject* ctor_, JSObject* proto_,
                                unsigned prototypeAttrs, unsigned constructorAttrs)
{
    RootedObject ctor(cx, ctor_), proto(cx, proto_);

    RootedValue protoVal(cx, ObjectValue(*proto));
    RootedValue ctorVal(cx, ObjectValue(*ctor));

    return DefineDataProperty(cx, ctor, cx->names().prototype, protoVal, prototypeAttrs) &&
           DefineDataProperty(cx, proto, cx->names().constructor, ctorVal, constructorAttrs);
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

bool
js::DefineToStringTag(JSContext* cx, HandleObject obj, JSAtom* tag)
{
    RootedId toStringTagId(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().toStringTag));
    RootedValue tagString(cx, StringValue(tag));
    return DefineDataProperty(cx, obj, toStringTagId, tagString, JSPROP_READONLY);
}

static void
GlobalDebuggees_finalize(FreeOp* fop, JSObject* obj)
{
    MOZ_ASSERT(fop->maybeOnHelperThread());
    fop->delete_((GlobalObject::DebuggerVector*) obj->as<NativeObject>().getPrivate());
}

static const ClassOps
GlobalDebuggees_classOps = {
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    GlobalDebuggees_finalize
};

static const Class
GlobalDebuggees_class = {
    "GlobalDebuggee",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_BACKGROUND_FINALIZE,
    &GlobalDebuggees_classOps
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

/* static */ RegExpStatics*
GlobalObject::getRegExpStatics(JSContext* cx, Handle<GlobalObject*> global)
{
    MOZ_ASSERT(cx);
    RegExpStaticsObject* resObj = nullptr;
    const Value& val = global->getSlot(REGEXP_STATICS);
    if (!val.isObject()) {
        MOZ_ASSERT(val.isUndefined());
        resObj = RegExpStatics::create(cx);
        if (!resObj)
            return nullptr;

        global->initSlot(REGEXP_STATICS, ObjectValue(*resObj));
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
    if (!DefineDataProperty(cx, intrinsicsHolder, cx->names().global, globalValue,
                            JSPROP_PERMANENT | JSPROP_READONLY))
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
    bool exists = false;
    if (!GlobalObject::maybeGetIntrinsicValue(cx, global, selfHostedName, funVal, &exists))
        return false;
    if (exists) {
        RootedFunction fun(cx, &funVal.toObject().as<JSFunction>());
        if (fun->explicitName() == name)
            return true;

        if (fun->explicitName() == selfHostedName) {
            // This function was initially cloned because it was called by
            // other self-hosted code, so the clone kept its self-hosted name,
            // instead of getting the name it's intended to have in content
            // compartments. This can happen when a lazy builtin is initialized
            // after self-hosted code for another builtin used the same
            // function. In that case, we need to change the function's name,
            // which is ok because it can't have been exposed to content
            // before.
            fun->initAtom(name);
            return true;
        }


        // The function might be installed multiple times on the same or
        // different builtins, under different property names, so its name
        // might be neither "selfHostedName" nor "name". In that case, its
        // canonical name must've been set using the `_SetCanonicalName`
        // intrinsic.
        cx->runtime()->assertSelfHostedFunctionHasCanonicalName(cx, selfHostedName);
        return true;
    }

    RootedFunction fun(cx);
    if (!cx->runtime()->createLazySelfHostedFunctionClone(cx, selfHostedName, name, nargs,
                                                          /* proto = */ nullptr,
                                                          SingletonObject, &fun))
    {
        return false;
    }
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
    Rooted<StackShape> child(cx, StackShape(base, id, slot, 0));
    Shape* shape = cx->zone()->propertyTree().getChild(cx, last, child);
    if (!shape)
        return false;

    if (!holder->setLastProperty(cx, shape))
        return false;

    holder->setSlot(shape->slot(), value);
    return true;
}

/* static */ bool
GlobalObject::ensureModulePrototypesCreated(JSContext *cx, Handle<GlobalObject*> global)
{
    return getOrCreateModulePrototype(cx, global) &&
           getOrCreateImportEntryPrototype(cx, global) &&
           getOrCreateExportEntryPrototype(cx, global) &&
           getOrCreateRequestedModulePrototype(cx, global);
}
