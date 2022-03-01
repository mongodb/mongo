/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/EnvironmentObject-inl.h"

#include "mozilla/Maybe.h"

#include "builtin/ModuleObject.h"
#include "gc/Policy.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/StackLimits.h"    // js::AutoCheckRecursionLimit
#include "js/friend/WindowProxy.h"    // js::IsWindow, js::IsWindowProxy
#include "vm/ArgumentsObject.h"
#include "vm/AsyncFunction.h"
#include "vm/BytecodeIterator.h"
#include "vm/BytecodeLocation.h"
#include "vm/GeneratorObject.h"  // js::GetGeneratorObjectForEnvironment
#include "vm/GlobalObject.h"
#include "vm/Iteration.h"
#include "vm/JSObject.h"
#include "vm/ProxyObject.h"
#include "vm/Realm.h"
#include "vm/Scope.h"
#include "vm/Shape.h"
#include "vm/Xdr.h"
#include "wasm/WasmInstance.h"

#include "gc/Marking-inl.h"
#include "vm/BytecodeIterator-inl.h"
#include "vm/BytecodeLocation-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/Stack-inl.h"

using namespace js;

using RootedArgumentsObject = Rooted<ArgumentsObject*>;
using MutableHandleArgumentsObject = MutableHandle<ArgumentsObject*>;

/*****************************************************************************/

Shape* js::EnvironmentCoordinateToEnvironmentShape(JSScript* script,
                                                   jsbytecode* pc) {
  MOZ_ASSERT(JOF_OPTYPE(JSOp(*pc)) == JOF_ENVCOORD);
  ScopeIter si(script->innermostScope(pc));
  uint32_t hops = EnvironmentCoordinate(pc).hops();
  while (true) {
    MOZ_ASSERT(!si.done());
    if (si.hasSyntacticEnvironment()) {
      if (!hops) {
        break;
      }
      hops--;
    }
    si++;
  }
  return si.environmentShape();
}

PropertyName* js::EnvironmentCoordinateNameSlow(JSScript* script,
                                                jsbytecode* pc) {
  Shape* shape = EnvironmentCoordinateToEnvironmentShape(script, pc);
  EnvironmentCoordinate ec(pc);

  ShapePropertyIter<NoGC> iter(shape);
  while (iter->slot() != ec.slot()) {
    iter++;
  }
  jsid id = iter->key();

  /* Beware nameless destructuring formal. */
  if (!id.isAtom()) {
    return script->runtimeFromAnyThread()->commonNames->empty;
  }
  return id.toAtom()->asPropertyName();
}

/*****************************************************************************/

template <typename T>
static T* CreateEnvironmentObject(JSContext* cx, HandleShape shape,
                                  gc::InitialHeap heap) {
  static_assert(std::is_base_of_v<EnvironmentObject, T>,
                "T must be an EnvironmentObject");

  // All environment objects can be background-finalized.
  gc::AllocKind allocKind = gc::GetGCObjectKind(shape->numFixedSlots());
  MOZ_ASSERT(CanChangeToBackgroundAllocKind(allocKind, &T::class_));
  allocKind = gc::ForegroundToBackgroundAllocKind(allocKind);

  JSObject* obj;
  JS_TRY_VAR_OR_RETURN_NULL(cx, obj,
                            NativeObject::create(cx, allocKind, heap, shape));

  return &obj->as<T>();
}

// Helper function for simple environment objects that don't need the overloads
// above.
template <typename T>
static T* CreateEnvironmentObject(JSContext* cx, HandleShape shape,
                                  NewObjectKind newKind = GenericObject) {
  gc::InitialHeap heap = GetInitialHeap(newKind, &T::class_);
  return CreateEnvironmentObject<T>(cx, shape, heap);
}

CallObject* CallObject::create(JSContext* cx, HandleShape shape) {
  gc::InitialHeap heap = GetInitialHeap(GenericObject, &class_);
  return CreateEnvironmentObject<CallObject>(cx, shape, heap);
}

/*
 * Create a CallObject for a JSScript that is not initialized to any particular
 * callsite. This object can either be initialized (with an enclosing scope and
 * callee) or used as a template for jit compilation.
 */
CallObject* CallObject::createTemplateObject(JSContext* cx, HandleScript script,
                                             HandleObject enclosing,
                                             gc::InitialHeap heap) {
  Rooted<FunctionScope*> scope(cx, &script->bodyScope()->as<FunctionScope>());
  RootedShape shape(cx, scope->environmentShape());
  MOZ_ASSERT(shape->getObjectClass() == &class_);

  // The JITs assume the result is nursery allocated unless we collected the
  // nursery, so don't change |heap| here.

  auto* callObj = CreateEnvironmentObject<CallObject>(cx, shape, heap);
  if (!callObj) {
    return nullptr;
  }

  callObj->initEnclosingEnvironment(enclosing);

  if (scope->hasParameterExprs()) {
    // If there are parameter expressions, all parameters are lexical and
    // have TDZ.
    for (BindingIter bi(script->bodyScope()); bi; bi++) {
      BindingLocation loc = bi.location();
      if (loc.kind() == BindingLocation::Kind::Environment &&
          BindingKindIsLexical(bi.kind())) {
        callObj->initSlot(loc.slot(), MagicValue(JS_UNINITIALIZED_LEXICAL));
      }
    }
  }

  return callObj;
}

/*
 * Construct a call object for the given bindings.  If this is a call object
 * for a function invocation, callee should be the function being called.
 * Otherwise it must be a call object for eval of strict mode code, and callee
 * must be null.
 */
CallObject* CallObject::create(JSContext* cx, HandleFunction callee,
                               HandleObject enclosing) {
  RootedScript script(cx, callee->nonLazyScript());
  gc::InitialHeap heap = gc::DefaultHeap;
  CallObject* callobj =
      CallObject::createTemplateObject(cx, script, enclosing, heap);
  if (!callobj) {
    return nullptr;
  }

  callobj->initFixedSlot(CALLEE_SLOT, ObjectValue(*callee));

  return callobj;
}

CallObject* CallObject::create(JSContext* cx, AbstractFramePtr frame) {
  MOZ_ASSERT(frame.isFunctionFrame());
  cx->check(frame);

  RootedObject envChain(cx, frame.environmentChain());
  RootedFunction callee(cx, frame.callee());

  CallObject* callobj = create(cx, callee, envChain);
  if (!callobj) {
    return nullptr;
  }

  if (!frame.script()->bodyScope()->as<FunctionScope>().hasParameterExprs()) {
    // If there are no defaults, copy the aliased arguments into the call
    // object manually. If there are defaults, bytecode is generated to do
    // the copying.

    for (PositionalFormalParameterIter fi(frame.script()); fi; fi++) {
      if (!fi.closedOver()) {
        continue;
      }
      callobj->setAliasedBinding(
          cx, fi,
          frame.unaliasedFormal(fi.argumentSlot(), DONT_CHECK_ALIASING));
    }
  }

  return callobj;
}

CallObject* CallObject::find(JSObject* env) {
  for (;;) {
    if (env->is<CallObject>()) {
      break;
    } else if (env->is<EnvironmentObject>()) {
      env = &env->as<EnvironmentObject>().enclosingEnvironment();
    } else if (env->is<DebugEnvironmentProxy>()) {
      EnvironmentObject& unwrapped =
          env->as<DebugEnvironmentProxy>().environment();
      if (unwrapped.is<CallObject>()) {
        env = &unwrapped;
        break;
      }
      env = &env->as<DebugEnvironmentProxy>().enclosingEnvironment();
    } else {
      MOZ_ASSERT(env->is<GlobalObject>());
      return nullptr;
    }
  }
  return &env->as<CallObject>();
}

CallObject* CallObject::createHollowForDebug(JSContext* cx,
                                             HandleFunction callee) {
  MOZ_ASSERT(!callee->needsCallObject());

  RootedScript script(cx, callee->nonLazyScript());
  Rooted<FunctionScope*> scope(cx, &script->bodyScope()->as<FunctionScope>());
  RootedShape shape(cx, EmptyEnvironmentShape<CallObject>(cx));
  if (!shape) {
    return nullptr;
  }
  Rooted<CallObject*> callobj(cx, create(cx, shape));
  if (!callobj) {
    return nullptr;
  }

  // This environment's enclosing link is never used: the
  // DebugEnvironmentProxy that refers to this scope carries its own
  // enclosing link, which is what Debugger uses to construct the tree of
  // Debugger.Environment objects.
  callobj->initEnclosingEnvironment(&cx->global()->lexicalEnvironment());
  callobj->initFixedSlot(CALLEE_SLOT, ObjectValue(*callee));

  RootedValue optimizedOut(cx, MagicValue(JS_OPTIMIZED_OUT));
  RootedId id(cx);
  for (Rooted<BindingIter> bi(cx, BindingIter(script)); bi; bi++) {
    id = NameToId(bi.name()->asPropertyName());
    if (!SetProperty(cx, callobj, id, optimizedOut)) {
      return nullptr;
    }
  }

  return callobj;
}

const JSClass CallObject::class_ = {
    "Call", JSCLASS_HAS_RESERVED_SLOTS(CallObject::RESERVED_SLOTS)};

/*****************************************************************************/

/* static */
VarEnvironmentObject* VarEnvironmentObject::create(JSContext* cx,
                                                   HandleShape shape,
                                                   HandleObject enclosing,
                                                   gc::InitialHeap heap) {
  MOZ_ASSERT(shape->getObjectClass() == &class_);

  auto* env = CreateEnvironmentObject<VarEnvironmentObject>(cx, shape);
  if (!env) {
    return nullptr;
  }

  MOZ_ASSERT(!env->inDictionaryMode());

  env->initEnclosingEnvironment(enclosing);

  return env;
}

/* static */
VarEnvironmentObject* VarEnvironmentObject::create(JSContext* cx,
                                                   HandleScope scope,
                                                   AbstractFramePtr frame) {
#ifdef DEBUG
  if (frame.isEvalFrame()) {
    MOZ_ASSERT(scope->is<EvalScope>() && scope == frame.script()->bodyScope());
    MOZ_ASSERT_IF(frame.isInterpreterFrame(),
                  cx->interpreterFrame() == frame.asInterpreterFrame());
    MOZ_ASSERT_IF(frame.isInterpreterFrame(),
                  cx->interpreterRegs().pc == frame.script()->code());
  } else {
    MOZ_ASSERT(frame.environmentChain());
    MOZ_ASSERT_IF(
        frame.callee()->needsCallObject(),
        &frame.environmentChain()->as<CallObject>().callee() == frame.callee());
  }
#endif

  RootedScript script(cx, frame.script());
  RootedObject envChain(cx, frame.environmentChain());
  RootedShape shape(cx, scope->environmentShape());
  VarEnvironmentObject* env = create(cx, shape, envChain, gc::DefaultHeap);
  if (!env) {
    return nullptr;
  }
  env->initScope(scope);
  return env;
}

/* static */
VarEnvironmentObject* VarEnvironmentObject::createHollowForDebug(
    JSContext* cx, Handle<Scope*> scope) {
  MOZ_ASSERT(scope->is<VarScope>() || scope->kind() == ScopeKind::StrictEval);
  MOZ_ASSERT(!scope->hasEnvironment());

  RootedShape shape(cx, EmptyEnvironmentShape<VarEnvironmentObject>(cx));
  if (!shape) {
    return nullptr;
  }

  // This environment's enclosing link is never used: the
  // DebugEnvironmentProxy that refers to this scope carries its own
  // enclosing link, which is what Debugger uses to construct the tree of
  // Debugger.Environment objects.
  RootedObject enclosingEnv(cx, &cx->global()->lexicalEnvironment());
  Rooted<VarEnvironmentObject*> env(
      cx, create(cx, shape, enclosingEnv, gc::TenuredHeap));
  if (!env) {
    return nullptr;
  }

  RootedValue optimizedOut(cx, MagicValue(JS_OPTIMIZED_OUT));
  RootedId id(cx);
  for (Rooted<BindingIter> bi(cx, BindingIter(scope)); bi; bi++) {
    id = NameToId(bi.name()->asPropertyName());
    if (!SetProperty(cx, env, id, optimizedOut)) {
      return nullptr;
    }
  }

  env->initScope(scope);
  return env;
}

const JSClass VarEnvironmentObject::class_ = {
    "Var", JSCLASS_HAS_RESERVED_SLOTS(VarEnvironmentObject::RESERVED_SLOTS)};

/*****************************************************************************/

const ObjectOps ModuleEnvironmentObject::objectOps_ = {
    ModuleEnvironmentObject::lookupProperty,  // lookupProperty
    nullptr,                                  // defineProperty
    ModuleEnvironmentObject::hasProperty,     // hasProperty
    ModuleEnvironmentObject::getProperty,     // getProperty
    ModuleEnvironmentObject::setProperty,     // setProperty
    ModuleEnvironmentObject::
        getOwnPropertyDescriptor,             // getOwnPropertyDescriptor
    ModuleEnvironmentObject::deleteProperty,  // deleteProperty
    nullptr,                                  // getElements
    nullptr,                                  // funToString
};

const JSClassOps ModuleEnvironmentObject::classOps_ = {
    nullptr,                                // addProperty
    nullptr,                                // delProperty
    nullptr,                                // enumerate
    ModuleEnvironmentObject::newEnumerate,  // newEnumerate
    nullptr,                                // resolve
    nullptr,                                // mayResolve
    nullptr,                                // finalize
    nullptr,                                // call
    nullptr,                                // hasInstance
    nullptr,                                // construct
    nullptr,                                // trace
};

const JSClass ModuleEnvironmentObject::class_ = {
    "ModuleEnvironmentObject",
    JSCLASS_HAS_RESERVED_SLOTS(ModuleEnvironmentObject::RESERVED_SLOTS),
    &ModuleEnvironmentObject::classOps_,
    JS_NULL_CLASS_SPEC,
    JS_NULL_CLASS_EXT,
    &ModuleEnvironmentObject::objectOps_};

/* static */
ModuleEnvironmentObject* ModuleEnvironmentObject::create(
    JSContext* cx, HandleModuleObject module) {
  RootedScript script(cx, module->script());
  RootedShape shape(cx,
                    script->bodyScope()->as<ModuleScope>().environmentShape());
  MOZ_ASSERT(shape->getObjectClass() == &class_);

  RootedModuleEnvironmentObject env(
      cx, CreateEnvironmentObject<ModuleEnvironmentObject>(cx, shape,
                                                           TenuredObject));
  if (!env) {
    return nullptr;
  }

  env->initReservedSlot(MODULE_SLOT, ObjectValue(*module));

  // Initialize this early so that we can manipulate the env object without
  // causing assertions.
  env->initEnclosingEnvironment(&cx->global()->lexicalEnvironment());

  // Initialize all lexical bindings and imports as uninitialized. Imports
  // get uninitialized because they have a special TDZ for cyclic imports.
  for (BindingIter bi(script); bi; bi++) {
    BindingLocation loc = bi.location();
    if (loc.kind() == BindingLocation::Kind::Environment &&
        BindingKindIsLexical(bi.kind())) {
      env->initSlot(loc.slot(), MagicValue(JS_UNINITIALIZED_LEXICAL));
    }
  }

  // It is not be possible to add or remove bindings from a module environment
  // after this point as module code is always strict.
#ifdef DEBUG
  for (ShapePropertyIter<NoGC> iter(env->shape()); !iter.done(); iter++) {
    MOZ_ASSERT(!iter->configurable());
  }
  MOZ_ASSERT(env->hasFlag(ObjectFlag::NotExtensible));
  MOZ_ASSERT(!env->inDictionaryMode());
#endif

  return env;
}

ModuleObject& ModuleEnvironmentObject::module() const {
  return getReservedSlot(MODULE_SLOT).toObject().as<ModuleObject>();
}

IndirectBindingMap& ModuleEnvironmentObject::importBindings() const {
  return module().importBindings();
}

bool ModuleEnvironmentObject::createImportBinding(JSContext* cx,
                                                  HandleAtom importName,
                                                  HandleModuleObject module,
                                                  HandleAtom localName) {
  RootedId importNameId(cx, AtomToId(importName));
  RootedId localNameId(cx, AtomToId(localName));
  RootedModuleEnvironmentObject env(cx, &module->initialEnvironment());
  if (!importBindings().put(cx, importNameId, env, localNameId)) {
    return false;
  }

  return true;
}

bool ModuleEnvironmentObject::hasImportBinding(HandlePropertyName name) {
  return importBindings().has(NameToId(name));
}

bool ModuleEnvironmentObject::lookupImport(
    jsid name, ModuleEnvironmentObject** envOut,
    mozilla::Maybe<PropertyInfo>* propOut) {
  return importBindings().lookup(name, envOut, propOut);
}

void ModuleEnvironmentObject::fixEnclosingEnvironmentAfterRealmMerge(
    GlobalObject& global) {
  setEnclosingEnvironment(&global.lexicalEnvironment());
}

/* static */
bool ModuleEnvironmentObject::lookupProperty(JSContext* cx, HandleObject obj,
                                             HandleId id,
                                             MutableHandleObject objp,
                                             PropertyResult* propp) {
  const IndirectBindingMap& bindings =
      obj->as<ModuleEnvironmentObject>().importBindings();
  mozilla::Maybe<PropertyInfo> propInfo;
  ModuleEnvironmentObject* env;
  if (bindings.lookup(id, &env, &propInfo)) {
    objp.set(env);
    propp->setNativeProperty(*propInfo);
    return true;
  }

  RootedNativeObject target(cx, &obj->as<NativeObject>());
  if (!NativeLookupOwnProperty<CanGC>(cx, target, id, propp)) {
    return false;
  }

  objp.set(obj);
  return true;
}

/* static */
bool ModuleEnvironmentObject::hasProperty(JSContext* cx, HandleObject obj,
                                          HandleId id, bool* foundp) {
  if (obj->as<ModuleEnvironmentObject>().importBindings().has(id)) {
    *foundp = true;
    return true;
  }

  RootedNativeObject self(cx, &obj->as<NativeObject>());
  return NativeHasProperty(cx, self, id, foundp);
}

/* static */
bool ModuleEnvironmentObject::getProperty(JSContext* cx, HandleObject obj,
                                          HandleValue receiver, HandleId id,
                                          MutableHandleValue vp) {
  const IndirectBindingMap& bindings =
      obj->as<ModuleEnvironmentObject>().importBindings();
  mozilla::Maybe<PropertyInfo> prop;
  ModuleEnvironmentObject* env;
  if (bindings.lookup(id, &env, &prop)) {
    vp.set(env->getSlot(prop->slot()));
    return true;
  }

  RootedNativeObject self(cx, &obj->as<NativeObject>());
  return NativeGetProperty(cx, self, receiver, id, vp);
}

/* static */
bool ModuleEnvironmentObject::setProperty(JSContext* cx, HandleObject obj,
                                          HandleId id, HandleValue v,
                                          HandleValue receiver,
                                          JS::ObjectOpResult& result) {
  RootedModuleEnvironmentObject self(cx, &obj->as<ModuleEnvironmentObject>());
  if (self->importBindings().has(id)) {
    return result.failReadOnly();
  }

  return NativeSetProperty<Qualified>(cx, self, id, v, receiver, result);
}

/* static */
bool ModuleEnvironmentObject::getOwnPropertyDescriptor(
    JSContext* cx, HandleObject obj, HandleId id,
    MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) {
  const IndirectBindingMap& bindings =
      obj->as<ModuleEnvironmentObject>().importBindings();
  mozilla::Maybe<PropertyInfo> prop;
  ModuleEnvironmentObject* env;
  if (bindings.lookup(id, &env, &prop)) {
    desc.set(mozilla::Some(PropertyDescriptor::Data(
        env->getSlot(prop->slot()),
        {JS::PropertyAttribute::Enumerable, JS::PropertyAttribute::Writable})));
    return true;
  }

  RootedNativeObject self(cx, &obj->as<NativeObject>());
  return NativeGetOwnPropertyDescriptor(cx, self, id, desc);
}

/* static */
bool ModuleEnvironmentObject::deleteProperty(JSContext* cx, HandleObject obj,
                                             HandleId id,
                                             ObjectOpResult& result) {
  return result.failCantDelete();
}

/* static */
bool ModuleEnvironmentObject::newEnumerate(JSContext* cx, HandleObject obj,
                                           MutableHandleIdVector properties,
                                           bool enumerableOnly) {
  RootedModuleEnvironmentObject self(cx, &obj->as<ModuleEnvironmentObject>());
  const IndirectBindingMap& bs(self->importBindings());

  MOZ_ASSERT(properties.length() == 0);
  size_t count = bs.count() + self->slotSpan() - RESERVED_SLOTS;
  if (!properties.reserve(count)) {
    ReportOutOfMemory(cx);
    return false;
  }

  bs.forEachExportedName([&](jsid name) { properties.infallibleAppend(name); });

  for (ShapePropertyIter<NoGC> iter(self->shape()); !iter.done(); iter++) {
    properties.infallibleAppend(iter->key());
  }

  MOZ_ASSERT(properties.length() == count);
  return true;
}

/*****************************************************************************/

const JSClass WasmInstanceEnvironmentObject::class_ = {
    "WasmInstance",
    JSCLASS_HAS_RESERVED_SLOTS(WasmInstanceEnvironmentObject::RESERVED_SLOTS)};

/* static */
WasmInstanceEnvironmentObject*
WasmInstanceEnvironmentObject::createHollowForDebug(
    JSContext* cx, Handle<WasmInstanceScope*> scope) {
  RootedShape shape(cx,
                    EmptyEnvironmentShape<WasmInstanceEnvironmentObject>(cx));
  if (!shape) {
    return nullptr;
  }

  auto* env = CreateEnvironmentObject<WasmInstanceEnvironmentObject>(cx, shape);
  if (!env) {
    return nullptr;
  }

  env->initEnclosingEnvironment(&cx->global()->lexicalEnvironment());
  env->initReservedSlot(SCOPE_SLOT, PrivateGCThingValue(scope));

  return env;
}

/*****************************************************************************/

const JSClass WasmFunctionCallObject::class_ = {
    "WasmCall",
    JSCLASS_HAS_RESERVED_SLOTS(WasmFunctionCallObject::RESERVED_SLOTS)};

/* static */
WasmFunctionCallObject* WasmFunctionCallObject::createHollowForDebug(
    JSContext* cx, HandleObject enclosing, Handle<WasmFunctionScope*> scope) {
  RootedShape shape(cx, EmptyEnvironmentShape<WasmFunctionCallObject>(cx));
  if (!shape) {
    return nullptr;
  }

  auto* callobj = CreateEnvironmentObject<WasmFunctionCallObject>(cx, shape);
  if (!callobj) {
    return nullptr;
  }

  callobj->initEnclosingEnvironment(enclosing);
  callobj->initReservedSlot(SCOPE_SLOT, PrivateGCThingValue(scope));

  return callobj;
}

/*****************************************************************************/

WithEnvironmentObject* WithEnvironmentObject::create(JSContext* cx,
                                                     HandleObject object,
                                                     HandleObject enclosing,
                                                     Handle<WithScope*> scope) {
  RootedShape shape(cx, EmptyEnvironmentShape<WithEnvironmentObject>(cx));
  if (!shape) {
    return nullptr;
  }

  auto* obj = CreateEnvironmentObject<WithEnvironmentObject>(cx, shape);
  if (!obj) {
    return nullptr;
  }

  JSObject* thisObj = GetThisObject(object);

  obj->initEnclosingEnvironment(enclosing);
  obj->initReservedSlot(OBJECT_SLOT, ObjectValue(*object));
  obj->initReservedSlot(THIS_SLOT, ObjectValue(*thisObj));
  if (scope) {
    obj->initReservedSlot(SCOPE_SLOT, PrivateGCThingValue(scope));
  } else {
    obj->initReservedSlot(SCOPE_SLOT, NullValue());
  }

  return obj;
}

WithEnvironmentObject* WithEnvironmentObject::createNonSyntactic(
    JSContext* cx, HandleObject object, HandleObject enclosing) {
  return create(cx, object, enclosing, nullptr);
}

static inline bool IsUnscopableDotName(JSContext* cx, HandleId id) {
  return id.isAtom(cx->names().dotThis);
}

#ifdef DEBUG
static bool IsInternalDotName(JSContext* cx, HandleId id) {
  return id.isAtom(cx->names().dotThis) ||
         id.isAtom(cx->names().dotGenerator) ||
         id.isAtom(cx->names().dotInitializers) ||
         id.isAtom(cx->names().dotFieldKeys) ||
         id.isAtom(cx->names().dotStaticInitializers) ||
         id.isAtom(cx->names().dotStaticFieldKeys) ||
         id.isAtom(cx->names().dotArgs) ||
         id.isAtom(cx->names().starNamespaceStar);
}
#endif

/* Implements ES6 8.1.1.2.1 HasBinding steps 7-9. */
static bool CheckUnscopables(JSContext* cx, HandleObject obj, HandleId id,
                             bool* scopable) {
  RootedId unscopablesId(
      cx,
      SYMBOL_TO_JSID(cx->wellKnownSymbols().get(JS::SymbolCode::unscopables)));
  RootedValue v(cx);
  if (!GetProperty(cx, obj, obj, unscopablesId, &v)) {
    return false;
  }
  if (v.isObject()) {
    RootedObject unscopablesObj(cx, &v.toObject());
    if (!GetProperty(cx, unscopablesObj, unscopablesObj, id, &v)) {
      return false;
    }
    *scopable = !ToBoolean(v);
  } else {
    *scopable = true;
  }
  return true;
}

static bool with_LookupProperty(JSContext* cx, HandleObject obj, HandleId id,
                                MutableHandleObject objp,
                                PropertyResult* propp) {
  // SpiderMonkey-specific: consider the internal '.this' name to be unscopable.
  if (IsUnscopableDotName(cx, id)) {
    objp.set(nullptr);
    propp->setNotFound();
    return true;
  }

  // Other internal dot-names shouldn't even end up in with-environments.
  MOZ_ASSERT(!IsInternalDotName(cx, id));

  RootedObject actual(cx, &obj->as<WithEnvironmentObject>().object());
  if (!LookupProperty(cx, actual, id, objp, propp)) {
    return false;
  }

  if (propp->isFound()) {
    bool scopable;
    if (!CheckUnscopables(cx, actual, id, &scopable)) {
      return false;
    }
    if (!scopable) {
      objp.set(nullptr);
      propp->setNotFound();
    }
  }
  return true;
}

static bool with_DefineProperty(JSContext* cx, HandleObject obj, HandleId id,
                                Handle<PropertyDescriptor> desc,
                                ObjectOpResult& result) {
  MOZ_ASSERT(!IsInternalDotName(cx, id));
  RootedObject actual(cx, &obj->as<WithEnvironmentObject>().object());
  return DefineProperty(cx, actual, id, desc, result);
}

static bool with_HasProperty(JSContext* cx, HandleObject obj, HandleId id,
                             bool* foundp) {
  MOZ_ASSERT(!IsInternalDotName(cx, id));
  RootedObject actual(cx, &obj->as<WithEnvironmentObject>().object());

  // ES 8.1.1.2.1 step 3-5.
  if (!HasProperty(cx, actual, id, foundp)) {
    return false;
  }
  if (!*foundp) {
    return true;
  }

  // Steps 7-10. (Step 6 is a no-op.)
  return CheckUnscopables(cx, actual, id, foundp);
}

static bool with_GetProperty(JSContext* cx, HandleObject obj,
                             HandleValue receiver, HandleId id,
                             MutableHandleValue vp) {
  MOZ_ASSERT(!IsInternalDotName(cx, id));
  RootedObject actual(cx, &obj->as<WithEnvironmentObject>().object());
  RootedValue actualReceiver(cx, receiver);
  if (receiver.isObject() && &receiver.toObject() == obj) {
    actualReceiver.setObject(*actual);
  }
  return GetProperty(cx, actual, actualReceiver, id, vp);
}

static bool with_SetProperty(JSContext* cx, HandleObject obj, HandleId id,
                             HandleValue v, HandleValue receiver,
                             ObjectOpResult& result) {
  MOZ_ASSERT(!IsInternalDotName(cx, id));
  RootedObject actual(cx, &obj->as<WithEnvironmentObject>().object());
  RootedValue actualReceiver(cx, receiver);
  if (receiver.isObject() && &receiver.toObject() == obj) {
    actualReceiver.setObject(*actual);
  }
  return SetProperty(cx, actual, id, v, actualReceiver, result);
}

static bool with_GetOwnPropertyDescriptor(
    JSContext* cx, HandleObject obj, HandleId id,
    MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) {
  MOZ_ASSERT(!IsInternalDotName(cx, id));
  RootedObject actual(cx, &obj->as<WithEnvironmentObject>().object());
  return GetOwnPropertyDescriptor(cx, actual, id, desc);
}

static bool with_DeleteProperty(JSContext* cx, HandleObject obj, HandleId id,
                                ObjectOpResult& result) {
  MOZ_ASSERT(!IsInternalDotName(cx, id));
  RootedObject actual(cx, &obj->as<WithEnvironmentObject>().object());
  return DeleteProperty(cx, actual, id, result);
}

static const ObjectOps WithEnvironmentObjectOps = {
    with_LookupProperty,            // lookupProperty
    with_DefineProperty,            // defineProperty
    with_HasProperty,               // hasProperty
    with_GetProperty,               // getProperty
    with_SetProperty,               // setProperty
    with_GetOwnPropertyDescriptor,  // getOwnPropertyDescriptor
    with_DeleteProperty,            // deleteProperty
    nullptr,                        // getElements
    nullptr,                        // funToString
};

const JSClass WithEnvironmentObject::class_ = {
    "With",
    JSCLASS_HAS_RESERVED_SLOTS(WithEnvironmentObject::RESERVED_SLOTS),
    JS_NULL_CLASS_OPS,
    JS_NULL_CLASS_SPEC,
    JS_NULL_CLASS_EXT,
    &WithEnvironmentObjectOps};

/* static */
NonSyntacticVariablesObject* NonSyntacticVariablesObject::create(
    JSContext* cx) {
  RootedShape shape(cx, EmptyEnvironmentShape<NonSyntacticVariablesObject>(cx));
  if (!shape) {
    return nullptr;
  }

  Rooted<NonSyntacticVariablesObject*> obj(
      cx, CreateEnvironmentObject<NonSyntacticVariablesObject>(cx, shape,
                                                               TenuredObject));
  if (!obj) {
    return nullptr;
  }

  MOZ_ASSERT(obj->isUnqualifiedVarObj());
  if (!JSObject::setQualifiedVarObj(cx, obj)) {
    return nullptr;
  }

  obj->initEnclosingEnvironment(&cx->global()->lexicalEnvironment());
  return obj;
}

const JSClass NonSyntacticVariablesObject::class_ = {
    "NonSyntacticVariablesObject",
    JSCLASS_HAS_RESERVED_SLOTS(NonSyntacticVariablesObject::RESERVED_SLOTS)};

bool js::CreateNonSyntacticEnvironmentChain(JSContext* cx,
                                            HandleObjectVector envChain,
                                            MutableHandleObject env) {
  // Callers are responsible for segregating the NonSyntactic case from simple
  // compilation cases.
  MOZ_RELEASE_ASSERT(!envChain.empty());

  RootedObject globalLexical(cx, &cx->global()->lexicalEnvironment());
  if (!CreateObjectsForEnvironmentChain(cx, envChain, globalLexical, env)) {
    return false;
  }

  // The XPConnect subscript loader, which may pass in its own
  // environments to load scripts in, expects the environment chain to
  // be the holder of "var" declarations. In SpiderMonkey, such objects
  // are called "qualified varobjs", the "qualified" part meaning the
  // declaration was qualified by "var". There is only sadness.
  //
  // See JSObject::isQualifiedVarObj.
  if (!JSObject::setQualifiedVarObj(cx, env)) {
    return false;
  }

  // Also get a non-syntactic lexical environment to capture 'let' and
  // 'const' bindings. To persist lexical bindings, we have a 1-1
  // mapping with the final unwrapped environment object (the
  // environment that stores the 'var' bindings) and the lexical
  // environment.
  //
  // TODOshu: disallow the subscript loader from using non-distinguished
  // objects as dynamic scopes.
  env.set(
      ObjectRealm::get(env).getOrCreateNonSyntacticLexicalEnvironment(cx, env));
  return !!env;
}

/*****************************************************************************/

const JSClass LexicalEnvironmentObject::class_ = {
    "LexicalEnvironment",
    JSCLASS_HAS_RESERVED_SLOTS(LexicalEnvironmentObject::RESERVED_SLOTS),
    JS_NULL_CLASS_OPS,
    JS_NULL_CLASS_SPEC,
    JS_NULL_CLASS_EXT,
    JS_NULL_OBJECT_OPS};

/* static */
LexicalEnvironmentObject* LexicalEnvironmentObject::createTemplateObject(
    JSContext* cx, HandleShape shape, HandleObject enclosing,
    gc::InitialHeap heap) {
  MOZ_ASSERT(shape->getObjectClass() == &LexicalEnvironmentObject::class_);

  // The JITs assume the result is nursery allocated unless we collected the
  // nursery, so don't change |heap| here.

  auto* env =
      CreateEnvironmentObject<LexicalEnvironmentObject>(cx, shape, heap);
  if (!env) {
    return nullptr;
  }

  MOZ_ASSERT(!env->inDictionaryMode());

  if (enclosing) {
    env->initEnclosingEnvironment(enclosing);
  }

  return env;
}

bool LexicalEnvironmentObject::isExtensible() const {
  return NativeObject::isExtensible();
}

/* static */
BlockLexicalEnvironmentObject* BlockLexicalEnvironmentObject::create(
    JSContext* cx, Handle<LexicalScope*> scope, HandleObject enclosing,
    gc::InitialHeap heap) {
  cx->check(enclosing);
  MOZ_ASSERT(scope->hasEnvironment());

  RootedShape shape(cx, scope->environmentShape());
  auto* env = static_cast<BlockLexicalEnvironmentObject*>(
      createTemplateObject(cx, shape, enclosing, heap));
  if (!env) {
    return nullptr;
  }

  // All lexical bindings start off uninitialized for TDZ.
  ShapePropertyIter<NoGC> iter(shape);
  uint32_t lastSlot = iter->slot();
  MOZ_ASSERT(lastSlot == env->getLastProperty().slot());
  for (uint32_t slot = JSSLOT_FREE(&class_); slot <= lastSlot; slot++) {
    env->initSlot(slot, MagicValue(JS_UNINITIALIZED_LEXICAL));
  }

  env->initScope(scope);
  return env;
}

/* static */
BlockLexicalEnvironmentObject* BlockLexicalEnvironmentObject::createForFrame(
    JSContext* cx, Handle<LexicalScope*> scope, AbstractFramePtr frame) {
  RootedObject enclosing(cx, frame.environmentChain());
  auto* env = create(cx, scope, enclosing, gc::DefaultHeap);
  if (!env) {
    return nullptr;
  }
  return &env->as<BlockLexicalEnvironmentObject>();
}

/* static */
BlockLexicalEnvironmentObject*
BlockLexicalEnvironmentObject::createHollowForDebug(
    JSContext* cx, Handle<LexicalScope*> scope) {
  MOZ_ASSERT(!scope->hasEnvironment());

  RootedShape shape(cx, LexicalScope::getEmptyExtensibleEnvironmentShape(cx));
  if (!shape) {
    return nullptr;
  }

  // This environment's enclosing link is never used: the
  // DebugEnvironmentProxy that refers to this scope carries its own
  // enclosing link, which is what Debugger uses to construct the tree of
  // Debugger.Environment objects.
  RootedObject enclosingEnv(cx, &cx->global()->lexicalEnvironment());
  Rooted<LexicalEnvironmentObject*> env(
      cx, createTemplateObject(cx, shape, enclosingEnv, gc::TenuredHeap));
  if (!env) {
    return nullptr;
  }

  RootedValue optimizedOut(cx, MagicValue(JS_OPTIMIZED_OUT));
  RootedId id(cx);
  for (Rooted<BindingIter> bi(cx, BindingIter(scope)); bi; bi++) {
    id = NameToId(bi.name()->asPropertyName());
    if (!SetProperty(cx, env, id, optimizedOut)) {
      return nullptr;
    }
  }

  if (!JSObject::setFlag(cx, env, ObjectFlag::NotExtensible)) {
    return nullptr;
  }

  env->as<ScopedLexicalEnvironmentObject>().initScope(scope);
  return &env->as<BlockLexicalEnvironmentObject>();
}

/* static */
BlockLexicalEnvironmentObject* BlockLexicalEnvironmentObject::clone(
    JSContext* cx, Handle<BlockLexicalEnvironmentObject*> env) {
  Rooted<LexicalScope*> scope(cx, &env->scope());
  RootedObject enclosing(cx, &env->enclosingEnvironment());
  Rooted<BlockLexicalEnvironmentObject*> copy(
      cx, create(cx, scope, enclosing, gc::DefaultHeap));
  if (!copy) {
    return nullptr;
  }

  // We can't assert that the clone has the same shape, because it could
  // have been reshaped by ReshapeForShadowedProp.
  MOZ_ASSERT(env->slotSpan() == copy->slotSpan());
  for (uint32_t i = JSSLOT_FREE(&class_); i < copy->slotSpan(); i++) {
    copy->setSlot(i, env->getSlot(i));
  }

  return copy;
}

/* static */
BlockLexicalEnvironmentObject* BlockLexicalEnvironmentObject::recreate(
    JSContext* cx, Handle<BlockLexicalEnvironmentObject*> env) {
  Rooted<LexicalScope*> scope(cx, &env->scope());
  RootedObject enclosing(cx, &env->enclosingEnvironment());
  return create(cx, scope, enclosing, gc::DefaultHeap);
}

/* static */
NamedLambdaObject* NamedLambdaObject::create(JSContext* cx,
                                             HandleFunction callee,
                                             HandleFunction func,
                                             HandleObject enclosing,
                                             gc::InitialHeap heap) {
  MOZ_ASSERT(callee->isNamedLambda());
  RootedScope scope(cx, callee->nonLazyScript()->maybeNamedLambdaScope());
  MOZ_ASSERT(scope && scope->environmentShape());

#ifdef DEBUG
  {
    // Named lambda objects have one (non-writable) property.
    ShapePropertyIter<NoGC> iter(scope->environmentShape());
    MOZ_ASSERT(iter->slot() == lambdaSlot());
    MOZ_ASSERT(!iter->writable());
    iter++;
    MOZ_ASSERT(iter.done());

    // There should be exactly one binding in the named lambda scope.
    BindingIter bi(scope);
    bi++;
    MOZ_ASSERT(bi.done());
  }
#endif

  BlockLexicalEnvironmentObject* obj = BlockLexicalEnvironmentObject::create(
      cx, scope.as<LexicalScope>(), enclosing, heap);
  if (!obj) {
    return nullptr;
  }

  obj->initFixedSlot(lambdaSlot(), ObjectValue(*func));
  return static_cast<NamedLambdaObject*>(obj);
}

/* static */
NamedLambdaObject* NamedLambdaObject::createTemplateObject(
    JSContext* cx, HandleFunction callee, gc::InitialHeap heap) {
  return create(cx, callee, callee, nullptr, heap);
}

/* static */
NamedLambdaObject* NamedLambdaObject::create(JSContext* cx,
                                             AbstractFramePtr frame) {
  RootedFunction fun(cx, frame.callee());
  RootedObject enclosing(cx, frame.environmentChain());
  return create(cx, fun, fun, enclosing, gc::DefaultHeap);
}

/* static */
size_t NamedLambdaObject::lambdaSlot() {
  // Named lambda environments have exactly one name.
  return JSSLOT_FREE(&LexicalEnvironmentObject::class_);
}

/* static */
ClassBodyLexicalEnvironmentObject* ClassBodyLexicalEnvironmentObject::create(
    JSContext* cx, Handle<ClassBodyScope*> scope, HandleObject enclosing,
    gc::InitialHeap heap) {
  cx->check(enclosing);
  MOZ_ASSERT(scope->hasEnvironment());

  RootedShape shape(cx, scope->environmentShape());
  auto* env = static_cast<ClassBodyLexicalEnvironmentObject*>(
      createTemplateObject(cx, shape, enclosing, heap));
  if (!env) {
    return nullptr;
  }

  env->initScope(scope);
  return env;
}

/* static */
ClassBodyLexicalEnvironmentObject*
ClassBodyLexicalEnvironmentObject::createForFrame(JSContext* cx,
                                                  Handle<ClassBodyScope*> scope,
                                                  AbstractFramePtr frame) {
  RootedObject enclosing(cx, frame.environmentChain());
  return create(cx, scope, enclosing, gc::DefaultHeap);
}

JSObject* ExtensibleLexicalEnvironmentObject::thisObject() const {
  JSObject* obj = &getReservedSlot(THIS_VALUE_OR_SCOPE_SLOT).toObject();

  // Windows must never be exposed to script. setWindowProxyThisValue should
  // have set this to the WindowProxy.
  MOZ_ASSERT(!IsWindow(obj));

  // WarpBuilder relies on the return value not being nursery-allocated for the
  // global lexical environment.
  MOZ_ASSERT_IF(isGlobal(), obj->isTenured());

  return obj;
}

/* static */
ExtensibleLexicalEnvironmentObject*
ExtensibleLexicalEnvironmentObject::forVarEnvironment(JSObject* obj) {
  ExtensibleLexicalEnvironmentObject* lexical = nullptr;
  if (obj->is<GlobalObject>()) {
    lexical = &obj->as<GlobalObject>().lexicalEnvironment();
  } else {
    lexical = ObjectRealm::get(obj).getNonSyntacticLexicalEnvironment(obj);
  }
  MOZ_ASSERT(lexical);
  return lexical;
}

/* static */
GlobalLexicalEnvironmentObject* GlobalLexicalEnvironmentObject::create(
    JSContext* cx, Handle<GlobalObject*> global) {
  MOZ_ASSERT(global);

  RootedShape shape(cx, LexicalScope::getEmptyExtensibleEnvironmentShape(cx));
  if (!shape) {
    return nullptr;
  }

  auto* env = static_cast<GlobalLexicalEnvironmentObject*>(
      createTemplateObject(cx, shape, global, gc::TenuredHeap));
  if (!env) {
    return nullptr;
  }

  env->initThisObject(global);
  return env;
}

void GlobalLexicalEnvironmentObject::setWindowProxyThisObject(JSObject* obj) {
  MOZ_ASSERT(IsWindowProxy(obj));
  setReservedSlot(THIS_VALUE_OR_SCOPE_SLOT, ObjectValue(*obj));
}

/* static */
NonSyntacticLexicalEnvironmentObject*
NonSyntacticLexicalEnvironmentObject::create(JSContext* cx,
                                             HandleObject enclosing,
                                             HandleObject thisv) {
  MOZ_ASSERT(enclosing);
  MOZ_ASSERT(!IsSyntacticEnvironment(enclosing));

  RootedShape shape(cx, LexicalScope::getEmptyExtensibleEnvironmentShape(cx));
  if (!shape) {
    return nullptr;
  }

  auto* env = static_cast<NonSyntacticLexicalEnvironmentObject*>(
      createTemplateObject(cx, shape, enclosing, gc::TenuredHeap));
  if (!env) {
    return nullptr;
  }

  env->initThisObject(thisv);

  return env;
}

/* static */
RuntimeLexicalErrorObject* RuntimeLexicalErrorObject::create(
    JSContext* cx, HandleObject enclosing, unsigned errorNumber) {
  RootedShape shape(cx, EmptyEnvironmentShape(cx, &class_, JSSLOT_FREE(&class_),
                                              ObjectFlags()));
  if (!shape) {
    return nullptr;
  }

  auto* obj = CreateEnvironmentObject<RuntimeLexicalErrorObject>(cx, shape);
  if (!obj) {
    return nullptr;
  }
  obj->initEnclosingEnvironment(enclosing);
  obj->initReservedSlot(ERROR_SLOT, Int32Value(int32_t(errorNumber)));

  return obj;
}

static void ReportRuntimeLexicalErrorId(JSContext* cx, unsigned errorNumber,
                                        HandleId id) {
  if (id.isAtom()) {
    RootedPropertyName name(cx, id.toAtom()->asPropertyName());
    ReportRuntimeLexicalError(cx, errorNumber, name);
    return;
  }
  MOZ_CRASH(
      "RuntimeLexicalErrorObject should only be used with property names");
}

static bool lexicalError_LookupProperty(JSContext* cx, HandleObject obj,
                                        HandleId id, MutableHandleObject objp,
                                        PropertyResult* propp) {
  ReportRuntimeLexicalErrorId(
      cx, obj->as<RuntimeLexicalErrorObject>().errorNumber(), id);
  return false;
}

static bool lexicalError_HasProperty(JSContext* cx, HandleObject obj,
                                     HandleId id, bool* foundp) {
  ReportRuntimeLexicalErrorId(
      cx, obj->as<RuntimeLexicalErrorObject>().errorNumber(), id);
  return false;
}

static bool lexicalError_GetProperty(JSContext* cx, HandleObject obj,
                                     HandleValue receiver, HandleId id,
                                     MutableHandleValue vp) {
  ReportRuntimeLexicalErrorId(
      cx, obj->as<RuntimeLexicalErrorObject>().errorNumber(), id);
  return false;
}

static bool lexicalError_SetProperty(JSContext* cx, HandleObject obj,
                                     HandleId id, HandleValue v,
                                     HandleValue receiver,
                                     ObjectOpResult& result) {
  ReportRuntimeLexicalErrorId(
      cx, obj->as<RuntimeLexicalErrorObject>().errorNumber(), id);
  return false;
}

static bool lexicalError_GetOwnPropertyDescriptor(
    JSContext* cx, HandleObject obj, HandleId id,
    MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) {
  ReportRuntimeLexicalErrorId(
      cx, obj->as<RuntimeLexicalErrorObject>().errorNumber(), id);
  return false;
}

static bool lexicalError_DeleteProperty(JSContext* cx, HandleObject obj,
                                        HandleId id, ObjectOpResult& result) {
  ReportRuntimeLexicalErrorId(
      cx, obj->as<RuntimeLexicalErrorObject>().errorNumber(), id);
  return false;
}

static const ObjectOps RuntimeLexicalErrorObjectObjectOps = {
    lexicalError_LookupProperty,            // lookupProperty
    nullptr,                                // defineProperty
    lexicalError_HasProperty,               // hasProperty
    lexicalError_GetProperty,               // getProperty
    lexicalError_SetProperty,               // setProperty
    lexicalError_GetOwnPropertyDescriptor,  // getOwnPropertyDescriptor
    lexicalError_DeleteProperty,            // deleteProperty
    nullptr,                                // getElements
    nullptr,                                // funToString
};

const JSClass RuntimeLexicalErrorObject::class_ = {
    "RuntimeLexicalError",
    JSCLASS_HAS_RESERVED_SLOTS(RuntimeLexicalErrorObject::RESERVED_SLOTS),
    JS_NULL_CLASS_OPS,
    JS_NULL_CLASS_SPEC,
    JS_NULL_CLASS_EXT,
    &RuntimeLexicalErrorObjectObjectOps};

/*****************************************************************************/

EnvironmentIter::EnvironmentIter(JSContext* cx, const EnvironmentIter& ei)
    : si_(cx, ei.si_.get()), env_(cx, ei.env_), frame_(ei.frame_) {}

EnvironmentIter::EnvironmentIter(JSContext* cx, JSObject* env, Scope* scope)
    : si_(cx, ScopeIter(scope)), env_(cx, env), frame_(NullFramePtr()) {
  settle();
}

EnvironmentIter::EnvironmentIter(JSContext* cx, AbstractFramePtr frame,
                                 jsbytecode* pc)
    : si_(cx, frame.script()->innermostScope(pc)),
      env_(cx, frame.environmentChain()),
      frame_(frame) {
  cx->check(frame);
  settle();
}

EnvironmentIter::EnvironmentIter(JSContext* cx, JSObject* env, Scope* scope,
                                 AbstractFramePtr frame)
    : si_(cx, ScopeIter(scope)), env_(cx, env), frame_(frame) {
  cx->check(frame);
  settle();
}

void EnvironmentIter::incrementScopeIter() {
  if (si_.scope()->is<GlobalScope>()) {
    // GlobalScopes may be syntactic or non-syntactic. Non-syntactic
    // GlobalScopes correspond to zero or more non-syntactic
    // EnvironmentsObjects followed by the global lexical scope, then the
    // GlobalObject or another non-EnvironmentObject object.
    if (!env_->is<EnvironmentObject>()) {
      si_++;
    }
  } else {
    si_++;
  }
}

void EnvironmentIter::settle() {
  // Check for trying to iterate a function or eval frame before the prologue
  // has created the CallObject, in which case we have to skip.
  if (frame_ && frame_.hasScript() &&
      frame_.script()->initialEnvironmentShape() &&
      !frame_.hasInitialEnvironment()) {
    // Skip until we're at the enclosing scope of the script.
    while (si_.scope() != frame_.script()->enclosingScope()) {
      if (env_->is<BlockLexicalEnvironmentObject>() &&
          &env_->as<BlockLexicalEnvironmentObject>().scope() == si_.scope()) {
        MOZ_ASSERT(si_.kind() == ScopeKind::NamedLambda ||
                   si_.kind() == ScopeKind::StrictNamedLambda);
        env_ =
            &env_->as<BlockLexicalEnvironmentObject>().enclosingEnvironment();
      }
      incrementScopeIter();
    }
  }

  // Check if we have left the extent of the initial frame after we've
  // settled on a static scope.
  if (frame_ &&
      (!si_ ||
       (frame_.hasScript() &&
        si_.scope() == frame_.script()->enclosingScope()) ||
       (frame_.isWasmDebugFrame() && !si_.scope()->is<WasmFunctionScope>()))) {
    frame_ = NullFramePtr();
  }

#ifdef DEBUG
  if (si_) {
    if (hasSyntacticEnvironment()) {
      Scope* scope = si_.scope();
      if (scope->is<LexicalScope>()) {
        MOZ_ASSERT(scope == &env_->as<BlockLexicalEnvironmentObject>().scope());
      } else if (scope->is<FunctionScope>()) {
        MOZ_ASSERT(scope->as<FunctionScope>().script() ==
                   env_->as<CallObject>()
                       .callee()
                       .maybeCanonicalFunction()
                       ->baseScript());
      } else if (scope->is<VarScope>()) {
        MOZ_ASSERT(scope == &env_->as<VarEnvironmentObject>().scope());
      } else if (scope->is<WithScope>()) {
        MOZ_ASSERT(scope == &env_->as<WithEnvironmentObject>().scope());
      } else if (scope->is<EvalScope>()) {
        MOZ_ASSERT(scope == &env_->as<VarEnvironmentObject>().scope());
      } else if (scope->is<GlobalScope>()) {
        MOZ_ASSERT(env_->is<GlobalObject>() ||
                   IsGlobalLexicalEnvironment(env_));
      }
    } else if (hasNonSyntacticEnvironmentObject()) {
      if (env_->is<LexicalEnvironmentObject>()) {
        // The global lexical environment still encloses non-syntactic
        // environment objects.
        MOZ_ASSERT(env_->is<NonSyntacticLexicalEnvironmentObject>() ||
                   env_->is<GlobalLexicalEnvironmentObject>());
      } else if (env_->is<WithEnvironmentObject>()) {
        MOZ_ASSERT(!env_->as<WithEnvironmentObject>().isSyntactic());
      } else {
        MOZ_ASSERT(env_->is<NonSyntacticVariablesObject>());
      }
    }
  }
#endif
}

JSObject& EnvironmentIter::enclosingEnvironment() const {
  // As an engine invariant (maintained internally and asserted by Execute),
  // EnvironmentObjects and non-EnvironmentObjects cannot be interleaved on
  // the scope chain; every scope chain must start with zero or more
  // EnvironmentObjects and terminate with one or more
  // non-EnvironmentObjects (viz., GlobalObject).
  MOZ_ASSERT(done());
  MOZ_ASSERT(!env_->is<EnvironmentObject>());
  return *env_;
}

bool EnvironmentIter::hasNonSyntacticEnvironmentObject() const {
  // The case we're worrying about here is a NonSyntactic static scope which
  // has 0+ corresponding non-syntactic WithEnvironmentObject scopes, a
  // NonSyntacticVariablesObject, or a NonSyntacticLexicalEnvironmentObject.
  if (si_.kind() == ScopeKind::NonSyntactic) {
    MOZ_ASSERT_IF(env_->is<WithEnvironmentObject>(),
                  !env_->as<WithEnvironmentObject>().isSyntactic());
    return env_->is<EnvironmentObject>();
  }
  return false;
}

/* static */
HashNumber MissingEnvironmentKey::hash(MissingEnvironmentKey ek) {
  return size_t(ek.frame_.raw()) ^ size_t(ek.scope_);
}

/* static */
bool MissingEnvironmentKey::match(MissingEnvironmentKey ek1,
                                  MissingEnvironmentKey ek2) {
  return ek1.frame_ == ek2.frame_ && ek1.scope_ == ek2.scope_;
}

bool LiveEnvironmentVal::needsSweep() {
  if (scope_) {
    MOZ_ALWAYS_FALSE(IsAboutToBeFinalized(&scope_));
  }
  return false;
}

// Live EnvironmentIter values may be added to DebugEnvironments::liveEnvs, as
// LiveEnvironmentVal instances.  They need to have write barriers when they are
// added to the hash table, but no barriers when rehashing inside GC.  It's a
// nasty hack, but the important thing is that LiveEnvironmentVal and
// MissingEnvironmentKey need to alias each other.
void LiveEnvironmentVal::staticAsserts() {
  static_assert(
      sizeof(LiveEnvironmentVal) == sizeof(MissingEnvironmentKey),
      "LiveEnvironmentVal must be same size of MissingEnvironmentKey");
  static_assert(
      offsetof(LiveEnvironmentVal, scope_) ==
          offsetof(MissingEnvironmentKey, scope_),
      "LiveEnvironmentVal.scope_ must alias MissingEnvironmentKey.scope_");
}

/*****************************************************************************/

namespace {

/*
 * DebugEnvironmentProxy is the handler for DebugEnvironmentProxy proxy
 * objects. Having a custom handler (rather than trying to reuse js::Wrapper)
 * gives us several important abilities:
 *  - We want to pass the EnvironmentObject as the receiver to forwarded scope
 *    property ops on aliased variables so that Call/Block/With ops do not all
 *    require a 'normalization' step.
 *  - The debug scope proxy can directly manipulate the stack frame to allow
 *    the debugger to read/write args/locals that were otherwise unaliased.
 *  - The debug scope proxy can store unaliased variables after the stack frame
 *    is popped so that they may still be read/written by the debugger.
 *  - The engine has made certain assumptions about the possible reads/writes
 *    in a scope. DebugEnvironmentProxy allows us to prevent the debugger from
 *    breaking those assumptions.
 *  - The engine makes optimizations that are observable to the debugger. The
 *    proxy can either hide these optimizations or make the situation more
 *    clear to the debugger. An example is 'arguments'.
 */
class DebugEnvironmentProxyHandler : public BaseProxyHandler {
  enum Action { SET, GET };

  enum AccessResult { ACCESS_UNALIASED, ACCESS_GENERIC, ACCESS_LOST };

  /*
   * This function handles access to unaliased locals/formals. Since they
   * are unaliased, the values of these variables are not stored in the
   * slots of the normal CallObject and BlockLexicalEnvironmentObject
   * environments and thus must be recovered from somewhere else:
   *  + if the invocation for which the env was created is still executing,
   *    there is a JS frame live on the stack holding the values;
   *  + if the invocation for which the env was created finished executing:
   *     - and there was a DebugEnvironmentProxy associated with env, then
   *       the DebugEnvironments::onPop(Call|Lexical) handler copied out the
   *       unaliased variables. In both cases, a dense array is created in
   *       onPop(Call|Lexical) to hold the unaliased values and attached to
   *       the DebugEnvironmentProxy;
   *     - and there was not a DebugEnvironmentProxy yet associated with the
   *       scope, then the unaliased values are lost and not recoverable.
   *
   * Callers should check accessResult for non-failure results:
   *  - ACCESS_UNALIASED if the access was unaliased and completed
   *  - ACCESS_GENERIC   if the access was aliased or the property not found
   *  - ACCESS_LOST      if the value has been lost to the debugger and the
   *                     action is GET; if the action is SET, we assign to the
   *                     name of the variable on the environment object
   */
  bool handleUnaliasedAccess(JSContext* cx,
                             Handle<DebugEnvironmentProxy*> debugEnv,
                             Handle<EnvironmentObject*> env, HandleId id,
                             Action action, MutableHandleValue vp,
                             AccessResult* accessResult) const {
    MOZ_ASSERT(&debugEnv->environment() == env);
    MOZ_ASSERT_IF(action == SET, !debugEnv->isOptimizedOut());
    *accessResult = ACCESS_GENERIC;
    LiveEnvironmentVal* maybeLiveEnv =
        DebugEnvironments::hasLiveEnvironment(*env);

    // Handle unaliased formals, vars, lets, and consts at function or module
    // scope.
    if (env->is<CallObject>() || env->is<ModuleEnvironmentObject>()) {
      RootedScript script(cx);
      if (env->is<CallObject>()) {
        CallObject& callobj = env->as<CallObject>();
        RootedFunction fun(cx, &callobj.callee());
        script = JSFunction::getOrCreateScript(cx, fun);
      } else {
        script = env->as<ModuleEnvironmentObject>().module().maybeScript();
        if (!script) {
          return true;
        }
      }

      BindingIter bi(script);
      while (bi && NameToId(bi.name()->asPropertyName()) != id) {
        bi++;
      }
      if (!bi) {
        return true;
      }

      if (bi.location().kind() == BindingLocation::Kind::Import) {
        return true;
      }

      if (!bi.hasArgumentSlot()) {
        if (bi.closedOver()) {
          return true;
        }

        uint32_t i = bi.location().slot();
        if (maybeLiveEnv) {
          AbstractFramePtr frame = maybeLiveEnv->frame();
          if (action == GET) {
            vp.set(frame.unaliasedLocal(i));
          } else {
            frame.unaliasedLocal(i) = vp;
          }
        } else if (AbstractGeneratorObject* genObj =
                       GetGeneratorObjectForEnvironment(cx, env);
                   genObj && genObj->isSuspended() &&
                   genObj->hasStackStorage()) {
          if (action == GET) {
            vp.set(genObj->getUnaliasedLocal(i));
          } else {
            genObj->setUnaliasedLocal(i, vp);
          }
        } else if (NativeObject* snapshot = debugEnv->maybeSnapshot()) {
          if (action == GET) {
            vp.set(snapshot->getDenseElement(script->numArgs() + i));
          } else {
            snapshot->setDenseElement(script->numArgs() + i, vp);
          }
        } else {
          /* The unaliased value has been lost to the debugger. */
          if (action == GET) {
            *accessResult = ACCESS_LOST;
            return true;
          }
        }
      } else {
        unsigned i = bi.argumentSlot();
        if (bi.closedOver()) {
          return true;
        }

        if (maybeLiveEnv) {
          AbstractFramePtr frame = maybeLiveEnv->frame();
          if (script->argsObjAliasesFormals() && frame.hasArgsObj()) {
            if (action == GET) {
              vp.set(frame.argsObj().arg(i));
            } else {
              frame.argsObj().setArg(i, vp);
            }
          } else {
            if (action == GET) {
              vp.set(frame.unaliasedFormal(i, DONT_CHECK_ALIASING));
            } else {
              frame.unaliasedFormal(i, DONT_CHECK_ALIASING) = vp;
            }
          }
        } else if (NativeObject* snapshot = debugEnv->maybeSnapshot()) {
          if (action == GET) {
            vp.set(snapshot->getDenseElement(i));
          } else {
            snapshot->setDenseElement(i, vp);
          }
        } else {
          /* The unaliased value has been lost to the debugger. */
          if (action == GET) {
            *accessResult = ACCESS_LOST;
            return true;
          }
        }
      }

      // It is possible that an optimized out value flows to this
      // location due to Debugger.Frame.prototype.eval operating on a
      // live bailed-out Baseline frame. In that case, treat the access
      // as lost.
      if (vp.isMagic() && vp.whyMagic() == JS_OPTIMIZED_OUT) {
        *accessResult = ACCESS_LOST;
      } else {
        *accessResult = ACCESS_UNALIASED;
      }

      return true;
    }

    /*
     * Handle unaliased vars in functions with parameter expressions and
     * lexical bindings at block scope.
     */
    if (env->is<LexicalEnvironmentObject>() ||
        env->is<VarEnvironmentObject>()) {
      // Currently consider all global and non-syntactic top-level lexical
      // bindings to be aliased.
      if (env->is<LexicalEnvironmentObject>() &&
          env->as<LexicalEnvironmentObject>().isExtensible()) {
        MOZ_ASSERT(IsGlobalLexicalEnvironment(env) ||
                   !IsSyntacticEnvironment(env));
        return true;
      }

      // Currently all vars inside non-strict eval var environments are aliased.
      if (env->is<VarEnvironmentObject>() &&
          env->as<VarEnvironmentObject>().isForNonStrictEval()) {
        return true;
      }

      RootedScope scope(cx, getEnvironmentScope(*env));
      uint32_t firstFrameSlot = scope->firstFrameSlot();

      BindingIter bi(scope);
      while (bi && NameToId(bi.name()->asPropertyName()) != id) {
        bi++;
      }
      if (!bi) {
        return true;
      }

      BindingLocation loc = bi.location();
      if (loc.kind() == BindingLocation::Kind::Environment) {
        return true;
      }

      // Named lambdas that are not closed over are lost.
      if (loc.kind() == BindingLocation::Kind::NamedLambdaCallee) {
        if (action == GET) {
          *accessResult = ACCESS_LOST;
        }
        return true;
      }

      MOZ_ASSERT(loc.kind() == BindingLocation::Kind::Frame);

      if (maybeLiveEnv) {
        AbstractFramePtr frame = maybeLiveEnv->frame();
        uint32_t local = loc.slot();
        MOZ_ASSERT(local < frame.script()->nfixed());
        if (action == GET) {
          vp.set(frame.unaliasedLocal(local));
        } else {
          frame.unaliasedLocal(local) = vp;
        }
      } else if (AbstractGeneratorObject* genObj =
                     GetGeneratorObjectForEnvironment(cx, debugEnv);
                 genObj && genObj->isSuspended() && genObj->hasStackStorage()) {
        if (action == GET) {
          vp.set(genObj->getUnaliasedLocal(loc.slot()));
        } else {
          genObj->setUnaliasedLocal(loc.slot(), vp);
        }
      } else if (NativeObject* snapshot = debugEnv->maybeSnapshot()) {
        // Indices in the frame snapshot are offset by the first frame
        // slot. See DebugEnvironments::takeFrameSnapshot.
        MOZ_ASSERT(loc.slot() >= firstFrameSlot);
        uint32_t snapshotIndex = loc.slot() - firstFrameSlot;
        if (action == GET) {
          vp.set(snapshot->getDenseElement(snapshotIndex));
        } else {
          snapshot->setDenseElement(snapshotIndex, vp);
        }
      } else {
        if (action == GET) {
          // A {Lexical,Var}EnvironmentObject whose static scope
          // does not have an environment shape at all is a "hollow"
          // block object reflected for missing block scopes. Their
          // slot values are lost.
          if (!scope->hasEnvironment()) {
            *accessResult = ACCESS_LOST;
            return true;
          }

          if (!GetProperty(cx, env, env, id, vp)) {
            return false;
          }
        } else {
          if (!SetProperty(cx, env, id, vp)) {
            return false;
          }
        }
      }

      // See comment above in analogous CallObject case.
      if (vp.isMagic() && vp.whyMagic() == JS_OPTIMIZED_OUT) {
        *accessResult = ACCESS_LOST;
      } else {
        *accessResult = ACCESS_UNALIASED;
      }

      return true;
    }

    if (env->is<WasmFunctionCallObject>()) {
      if (maybeLiveEnv) {
        RootedScope scope(cx, getEnvironmentScope(*env));
        uint32_t index = 0;
        for (BindingIter bi(scope); bi; bi++) {
          if (id.isAtom(bi.name())) {
            break;
          }
          MOZ_ASSERT(!bi.isLast());
          index++;
        }

        AbstractFramePtr frame = maybeLiveEnv->frame();
        MOZ_ASSERT(frame.isWasmDebugFrame());
        wasm::DebugFrame* wasmFrame = frame.asWasmDebugFrame();
        if (action == GET) {
          if (!wasmFrame->getLocal(index, vp)) {
            ReportOutOfMemory(cx);
            return false;
          }
          *accessResult = ACCESS_UNALIASED;
        } else {  // if (action == SET)
                  // TODO
        }
      } else {
        *accessResult = ACCESS_LOST;
      }
      return true;
    }

    if (env->is<WasmInstanceEnvironmentObject>()) {
      RootedScope scope(cx, getEnvironmentScope(*env));
      MOZ_ASSERT(scope->is<WasmInstanceScope>());
      uint32_t index = 0;
      for (BindingIter bi(scope); bi; bi++) {
        if (id.isAtom(bi.name())) {
          break;
        }
        MOZ_ASSERT(!bi.isLast());
        index++;
      }
      Rooted<WasmInstanceScope*> instanceScope(cx,
                                               &scope->as<WasmInstanceScope>());
      wasm::Instance& instance = instanceScope->instance()->instance();

      if (action == GET) {
        if (instanceScope->memoriesStart() <= index &&
            index < instanceScope->globalsStart()) {
          MOZ_ASSERT(instanceScope->memoriesStart() + 1 ==
                     instanceScope->globalsStart());
          vp.set(ObjectValue(*instance.memory()));
        }
        if (instanceScope->globalsStart() <= index) {
          MOZ_ASSERT(index < instanceScope->namesCount());
          if (!instance.debug().getGlobal(
                  instance, index - instanceScope->globalsStart(), vp)) {
            ReportOutOfMemory(cx);
            return false;
          }
        }
        *accessResult = ACCESS_UNALIASED;
      } else {  // if (action == SET)
                // TODO
      }
      return true;
    }

    /* The rest of the internal scopes do not have unaliased vars. */
    MOZ_ASSERT(!IsSyntacticEnvironment(env) ||
               env->is<WithEnvironmentObject>());
    return true;
  }

  static bool isArguments(JSContext* cx, jsid id) {
    return id == NameToId(cx->names().arguments);
  }
  static bool isThis(JSContext* cx, jsid id) {
    return id == NameToId(cx->names().dotThis);
  }

  static bool isFunctionEnvironment(const JSObject& env) {
    return env.is<CallObject>();
  }

  static bool isNonExtensibleLexicalEnvironment(const JSObject& env) {
    return env.is<ScopedLexicalEnvironmentObject>();
  }

  static Scope* getEnvironmentScope(const JSObject& env) {
    if (isFunctionEnvironment(env)) {
      return env.as<CallObject>().callee().nonLazyScript()->bodyScope();
    }
    if (env.is<ModuleEnvironmentObject>()) {
      JSScript* script =
          env.as<ModuleEnvironmentObject>().module().maybeScript();
      return script ? script->bodyScope() : nullptr;
    }
    if (isNonExtensibleLexicalEnvironment(env)) {
      return &env.as<ScopedLexicalEnvironmentObject>().scope();
    }
    if (env.is<VarEnvironmentObject>()) {
      return &env.as<VarEnvironmentObject>().scope();
    }
    if (env.is<WasmInstanceEnvironmentObject>()) {
      return &env.as<WasmInstanceEnvironmentObject>().scope();
    }
    if (env.is<WasmFunctionCallObject>()) {
      return &env.as<WasmFunctionCallObject>().scope();
    }
    return nullptr;
  }

  friend Scope* js::GetEnvironmentScope(const JSObject& env);

  /*
   * In theory, every non-arrow function scope contains an 'arguments'
   * bindings.  However, the engine only adds a binding if 'arguments' is
   * used in the function body. Thus, from the debugger's perspective,
   * 'arguments' may be missing from the list of bindings.
   */
  static bool isMissingArgumentsBinding(EnvironmentObject& env) {
    return isFunctionEnvironment(env) &&
           !env.as<CallObject>().callee().baseScript()->needsArgsObj();
  }

  /*
   * Similar to 'arguments' above, we don't add a 'this' binding to
   * non-arrow functions if it's not used.
   */
  static bool isMissingThisBinding(EnvironmentObject& env) {
    return isFunctionEnvironmentWithThis(env) &&
           !env.as<CallObject>()
                .callee()
                .baseScript()
                ->functionHasThisBinding();
  }

  /*
   * This function checks if an arguments object needs to be created when
   * the debugger requests 'arguments' for a function scope where the
   * arguments object was not otherwise needed.
   */
  static bool isMissingArguments(JSContext* cx, jsid id,
                                 EnvironmentObject& env) {
    return isArguments(cx, id) && isMissingArgumentsBinding(env);
  }
  static bool isMissingThis(JSContext* cx, jsid id, EnvironmentObject& env) {
    return isThis(cx, id) && isMissingThisBinding(env);
  }

  /*
   * If the value of |this| is requested before the this-binding has been
   * initialized by JSOp::FunctionThis, the this-binding will be |undefined|.
   * In that case, we have to call createMissingThis to initialize the
   * this-binding.
   *
   * Note that an |undefined| this-binding is perfectly valid in strict-mode
   * code, but that's fine: createMissingThis will do the right thing in that
   * case.
   */
  static bool isMaybeUninitializedThisValue(JSContext* cx, jsid id,
                                            const Value& v) {
    return isThis(cx, id) && v.isUndefined();
  }

  /*
   * Create a missing arguments object. If the function returns true but
   * argsObj is null, it means the env is dead.
   */
  static bool createMissingArguments(JSContext* cx, EnvironmentObject& env,
                                     MutableHandleArgumentsObject argsObj) {
    argsObj.set(nullptr);

    LiveEnvironmentVal* maybeEnv = DebugEnvironments::hasLiveEnvironment(env);
    if (!maybeEnv) {
      return true;
    }

    argsObj.set(ArgumentsObject::createUnexpected(cx, maybeEnv->frame()));
    return !!argsObj;
  }

  /*
   * Create a missing this Value. If the function returns true but
   * *success is false, it means the scope is dead.
   */
  static bool createMissingThis(JSContext* cx, EnvironmentObject& env,
                                MutableHandleValue thisv, bool* success) {
    *success = false;

    LiveEnvironmentVal* maybeEnv = DebugEnvironments::hasLiveEnvironment(env);
    if (!maybeEnv) {
      return true;
    }

    if (!GetFunctionThis(cx, maybeEnv->frame(), thisv)) {
      return false;
    }

    // Update the this-argument to avoid boxing primitive |this| more
    // than once.
    maybeEnv->frame().thisArgument() = thisv;
    *success = true;
    return true;
  }

  static void reportOptimizedOut(JSContext* cx, HandleId id) {
    if (isThis(cx, id)) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_DEBUG_OPTIMIZED_OUT, "this");
      return;
    }

    if (UniqueChars printable =
            IdToPrintableUTF8(cx, id, IdToPrintableBehavior::IdIsIdentifier)) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_DEBUG_OPTIMIZED_OUT, printable.get());
    }
  }

 public:
  static const char family;
  static const DebugEnvironmentProxyHandler singleton;

  constexpr DebugEnvironmentProxyHandler() : BaseProxyHandler(&family) {}

  static bool isFunctionEnvironmentWithThis(const JSObject& env) {
    // All functions except arrows should have their own this binding.
    return isFunctionEnvironment(env) &&
           !env.as<CallObject>().callee().hasLexicalThis();
  }

  bool getPrototypeIfOrdinary(JSContext* cx, HandleObject proxy,
                              bool* isOrdinary,
                              MutableHandleObject protop) const override {
    MOZ_CRASH(
        "shouldn't be possible to access the prototype chain of a "
        "DebugEnvironmentProxyHandler");
  }

  bool preventExtensions(JSContext* cx, HandleObject proxy,
                         ObjectOpResult& result) const override {
    // always [[Extensible]], can't be made non-[[Extensible]], like most
    // proxies
    return result.fail(JSMSG_CANT_CHANGE_EXTENSIBILITY);
  }

  bool isExtensible(JSContext* cx, HandleObject proxy,
                    bool* extensible) const override {
    // See above.
    *extensible = true;
    return true;
  }

  bool getMissingArgumentsPropertyDescriptor(
      JSContext* cx, Handle<DebugEnvironmentProxy*> debugEnv,
      EnvironmentObject& env,
      MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) const {
    RootedArgumentsObject argsObj(cx);
    if (!createMissingArguments(cx, env, &argsObj)) {
      return false;
    }

    if (!argsObj) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEBUG_NOT_ON_STACK, "Debugger scope");
      return false;
    }

    desc.set(mozilla::Some(PropertyDescriptor::Data(
        ObjectValue(*argsObj), {JS::PropertyAttribute::Enumerable})));
    return true;
  }
  bool getMissingThisPropertyDescriptor(
      JSContext* cx, Handle<DebugEnvironmentProxy*> debugEnv,
      EnvironmentObject& env,
      MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) const {
    RootedValue thisv(cx);
    bool success;
    if (!createMissingThis(cx, env, &thisv, &success)) {
      return false;
    }

    if (!success) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEBUG_NOT_ON_STACK, "Debugger scope");
      return false;
    }

    desc.set(mozilla::Some(
        PropertyDescriptor::Data(thisv, {JS::PropertyAttribute::Enumerable})));
    return true;
  }

  bool getOwnPropertyDescriptor(
      JSContext* cx, HandleObject proxy, HandleId id,
      MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) const override {
    Rooted<DebugEnvironmentProxy*> debugEnv(
        cx, &proxy->as<DebugEnvironmentProxy>());
    Rooted<EnvironmentObject*> env(cx, &debugEnv->environment());

    if (isMissingArguments(cx, id, *env)) {
      return getMissingArgumentsPropertyDescriptor(cx, debugEnv, *env, desc);
    }

    if (isMissingThis(cx, id, *env)) {
      return getMissingThisPropertyDescriptor(cx, debugEnv, *env, desc);
    }

    RootedValue v(cx);
    AccessResult access;
    if (!handleUnaliasedAccess(cx, debugEnv, env, id, GET, &v, &access)) {
      return false;
    }

    switch (access) {
      case ACCESS_UNALIASED: {
        desc.set(mozilla::Some(
            PropertyDescriptor::Data(v, {JS::PropertyAttribute::Enumerable})));
        return true;
      }
      case ACCESS_GENERIC:
        return GetOwnPropertyDescriptor(cx, env, id, desc);
      case ACCESS_LOST:
        reportOptimizedOut(cx, id);
        return false;
      default:
        MOZ_CRASH("bad AccessResult");
    }
  }

  bool getMissingArguments(JSContext* cx, EnvironmentObject& env,
                           MutableHandleValue vp) const {
    RootedArgumentsObject argsObj(cx);
    if (!createMissingArguments(cx, env, &argsObj)) {
      return false;
    }

    if (!argsObj) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEBUG_NOT_ON_STACK, "Debugger env");
      return false;
    }

    vp.setObject(*argsObj);
    return true;
  }

  bool getMissingThis(JSContext* cx, EnvironmentObject& env,
                      MutableHandleValue vp) const {
    RootedValue thisv(cx);
    bool success;
    if (!createMissingThis(cx, env, &thisv, &success)) {
      return false;
    }

    if (!success) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEBUG_NOT_ON_STACK, "Debugger env");
      return false;
    }

    vp.set(thisv);
    return true;
  }

  bool get(JSContext* cx, HandleObject proxy, HandleValue receiver, HandleId id,
           MutableHandleValue vp) const override {
    Rooted<DebugEnvironmentProxy*> debugEnv(
        cx, &proxy->as<DebugEnvironmentProxy>());
    Rooted<EnvironmentObject*> env(
        cx, &proxy->as<DebugEnvironmentProxy>().environment());

    if (isMissingArguments(cx, id, *env)) {
      return getMissingArguments(cx, *env, vp);
    }

    if (isMissingThis(cx, id, *env)) {
      return getMissingThis(cx, *env, vp);
    }

    AccessResult access;
    if (!handleUnaliasedAccess(cx, debugEnv, env, id, GET, vp, &access)) {
      return false;
    }

    switch (access) {
      case ACCESS_UNALIASED:
        if (isMaybeUninitializedThisValue(cx, id, vp)) {
          return getMissingThis(cx, *env, vp);
        }
        return true;
      case ACCESS_GENERIC:
        if (!GetProperty(cx, env, env, id, vp)) {
          return false;
        }
        if (isMaybeUninitializedThisValue(cx, id, vp)) {
          return getMissingThis(cx, *env, vp);
        }
        return true;
      case ACCESS_LOST:
        reportOptimizedOut(cx, id);
        return false;
      default:
        MOZ_CRASH("bad AccessResult");
    }
  }

  bool getMissingArgumentsMaybeSentinelValue(JSContext* cx,
                                             EnvironmentObject& env,
                                             MutableHandleValue vp) const {
    RootedArgumentsObject argsObj(cx);
    if (!createMissingArguments(cx, env, &argsObj)) {
      return false;
    }
    vp.set(argsObj ? ObjectValue(*argsObj) : MagicValue(JS_MISSING_ARGUMENTS));
    return true;
  }

  bool getMissingThisMaybeSentinelValue(JSContext* cx, EnvironmentObject& env,
                                        MutableHandleValue vp) const {
    RootedValue thisv(cx);
    bool success;
    if (!createMissingThis(cx, env, &thisv, &success)) {
      return false;
    }
    vp.set(success ? thisv : MagicValue(JS_OPTIMIZED_OUT));
    return true;
  }

  /*
   * Like 'get', but returns sentinel values instead of throwing on
   * exceptional cases.
   */
  bool getMaybeSentinelValue(JSContext* cx,
                             Handle<DebugEnvironmentProxy*> debugEnv,
                             HandleId id, MutableHandleValue vp) const {
    Rooted<EnvironmentObject*> env(cx, &debugEnv->environment());

    if (isMissingArguments(cx, id, *env)) {
      return getMissingArgumentsMaybeSentinelValue(cx, *env, vp);
    }
    if (isMissingThis(cx, id, *env)) {
      return getMissingThisMaybeSentinelValue(cx, *env, vp);
    }

    AccessResult access;
    if (!handleUnaliasedAccess(cx, debugEnv, env, id, GET, vp, &access)) {
      return false;
    }

    switch (access) {
      case ACCESS_UNALIASED:
        if (isMaybeUninitializedThisValue(cx, id, vp)) {
          return getMissingThisMaybeSentinelValue(cx, *env, vp);
        }
        return true;
      case ACCESS_GENERIC:
        if (!GetProperty(cx, env, env, id, vp)) {
          return false;
        }
        if (isMaybeUninitializedThisValue(cx, id, vp)) {
          return getMissingThisMaybeSentinelValue(cx, *env, vp);
        }
        return true;
      case ACCESS_LOST:
        vp.setMagic(JS_OPTIMIZED_OUT);
        return true;
      default:
        MOZ_CRASH("bad AccessResult");
    }
  }

  bool set(JSContext* cx, HandleObject proxy, HandleId id, HandleValue v,
           HandleValue receiver, ObjectOpResult& result) const override {
    Rooted<DebugEnvironmentProxy*> debugEnv(
        cx, &proxy->as<DebugEnvironmentProxy>());
    Rooted<EnvironmentObject*> env(
        cx, &proxy->as<DebugEnvironmentProxy>().environment());

    if (debugEnv->isOptimizedOut()) {
      return Throw(cx, id, JSMSG_DEBUG_CANT_SET_OPT_ENV);
    }

    AccessResult access;
    RootedValue valCopy(cx, v);
    if (!handleUnaliasedAccess(cx, debugEnv, env, id, SET, &valCopy, &access)) {
      return false;
    }

    switch (access) {
      case ACCESS_UNALIASED:
        return result.succeed();
      case ACCESS_GENERIC: {
        RootedValue envVal(cx, ObjectValue(*env));
        return SetProperty(cx, env, id, v, envVal, result);
      }
      default:
        MOZ_CRASH("bad AccessResult");
    }
  }

  bool defineProperty(JSContext* cx, HandleObject proxy, HandleId id,
                      Handle<PropertyDescriptor> desc,
                      ObjectOpResult& result) const override {
    Rooted<EnvironmentObject*> env(
        cx, &proxy->as<DebugEnvironmentProxy>().environment());

    bool found;
    if (!has(cx, proxy, id, &found)) {
      return false;
    }
    if (found) {
      return Throw(cx, id, JSMSG_CANT_REDEFINE_PROP);
    }

    return JS_DefinePropertyById(cx, env, id, desc, result);
  }

  bool ownPropertyKeys(JSContext* cx, HandleObject proxy,
                       MutableHandleIdVector props) const override {
    Rooted<EnvironmentObject*> env(
        cx, &proxy->as<DebugEnvironmentProxy>().environment());

    if (isMissingArgumentsBinding(*env)) {
      if (!props.append(NameToId(cx->names().arguments))) {
        return false;
      }
    }
    if (isMissingThisBinding(*env)) {
      if (!props.append(NameToId(cx->names().dotThis))) {
        return false;
      }
    }

    // WithEnvironmentObject isn't a very good proxy.  It doesn't have a
    // JSNewEnumerateOp implementation, because if it just delegated to the
    // target object, the object would indicate that native enumeration is
    // the thing to do, but native enumeration over the WithEnvironmentObject
    // wrapper yields no properties.  So instead here we hack around the
    // issue: punch a hole through to the with object target, then manually
    // examine @@unscopables.
    RootedObject target(cx);
    bool isWith = env->is<WithEnvironmentObject>();
    if (isWith) {
      target = &env->as<WithEnvironmentObject>().object();
    } else {
      target = env;
    }
    if (!GetPropertyKeys(cx, target, JSITER_OWNONLY, props)) {
      return false;
    }

    if (isWith) {
      size_t j = 0;
      for (size_t i = 0; i < props.length(); i++) {
        bool inScope;
        if (!CheckUnscopables(cx, env, props[i], &inScope)) {
          return false;
        }
        if (inScope) {
          props[j++].set(props[i]);
        }
      }
      if (!props.resize(j)) {
        return false;
      }
    }

    /*
     * Environments with Scopes are optimized to not contain unaliased
     * variables so they must be manually appended here.
     */
    if (Scope* scope = getEnvironmentScope(*env)) {
      for (Rooted<BindingIter> bi(cx, BindingIter(scope)); bi; bi++) {
        if (!bi.closedOver() &&
            !props.append(NameToId(bi.name()->asPropertyName()))) {
          return false;
        }
      }
    }

    return true;
  }

  bool has(JSContext* cx, HandleObject proxy, HandleId id_,
           bool* bp) const override {
    RootedId id(cx, id_);
    EnvironmentObject& envObj =
        proxy->as<DebugEnvironmentProxy>().environment();

    if (isArguments(cx, id) && isFunctionEnvironment(envObj)) {
      *bp = true;
      return true;
    }

    // Be careful not to look up '.this' as a normal binding below, it will
    // assert in with_HasProperty.
    if (isThis(cx, id)) {
      *bp = isFunctionEnvironmentWithThis(envObj);
      return true;
    }

    bool found;
    RootedObject env(cx, &envObj);
    if (!JS_HasPropertyById(cx, env, id, &found)) {
      return false;
    }

    if (!found) {
      if (Scope* scope = getEnvironmentScope(*env)) {
        for (BindingIter bi(scope); bi; bi++) {
          if (!bi.closedOver() && NameToId(bi.name()->asPropertyName()) == id) {
            found = true;
            break;
          }
        }
      }
    }

    *bp = found;
    return true;
  }

  bool delete_(JSContext* cx, HandleObject proxy, HandleId id,
               ObjectOpResult& result) const override {
    return result.fail(JSMSG_CANT_DELETE);
  }
};

} /* anonymous namespace */

Scope* js::GetEnvironmentScope(const JSObject& env) {
  return DebugEnvironmentProxyHandler::getEnvironmentScope(env);
}

template <>
bool JSObject::is<js::DebugEnvironmentProxy>() const {
  return IsDerivedProxyObject(this, &DebugEnvironmentProxyHandler::singleton);
}

const char DebugEnvironmentProxyHandler::family = 0;
const DebugEnvironmentProxyHandler DebugEnvironmentProxyHandler::singleton;

/* static */
DebugEnvironmentProxy* DebugEnvironmentProxy::create(JSContext* cx,
                                                     EnvironmentObject& env,
                                                     HandleObject enclosing) {
  MOZ_ASSERT(env.realm() == cx->realm());
  MOZ_ASSERT(!enclosing->is<EnvironmentObject>());

  RootedValue priv(cx, ObjectValue(env));
  JSObject* obj = NewProxyObject(cx, &DebugEnvironmentProxyHandler::singleton,
                                 priv, nullptr /* proto */);
  if (!obj) {
    return nullptr;
  }

  DebugEnvironmentProxy* debugEnv = &obj->as<DebugEnvironmentProxy>();
  debugEnv->setReservedSlot(ENCLOSING_SLOT, ObjectValue(*enclosing));
  debugEnv->setReservedSlot(SNAPSHOT_SLOT, NullValue());

  return debugEnv;
}

EnvironmentObject& DebugEnvironmentProxy::environment() const {
  return target()->as<EnvironmentObject>();
}

JSObject& DebugEnvironmentProxy::enclosingEnvironment() const {
  return reservedSlot(ENCLOSING_SLOT).toObject();
}

ArrayObject* DebugEnvironmentProxy::maybeSnapshot() const {
  JSObject* obj = reservedSlot(SNAPSHOT_SLOT).toObjectOrNull();
  return obj ? &obj->as<ArrayObject>() : nullptr;
}

void DebugEnvironmentProxy::initSnapshot(ArrayObject& o) {
  MOZ_ASSERT_IF(
      maybeSnapshot() != nullptr,
      CallObject::find(&environment())->callee().isGeneratorOrAsync());
  setReservedSlot(SNAPSHOT_SLOT, ObjectValue(o));
}

bool DebugEnvironmentProxy::isForDeclarative() const {
  EnvironmentObject& e = environment();
  return e.is<CallObject>() || e.is<VarEnvironmentObject>() ||
         e.is<ModuleEnvironmentObject>() ||
         e.is<WasmInstanceEnvironmentObject>() ||
         e.is<WasmFunctionCallObject>() || e.is<LexicalEnvironmentObject>();
}

/* static */
bool DebugEnvironmentProxy::getMaybeSentinelValue(
    JSContext* cx, Handle<DebugEnvironmentProxy*> env, HandleId id,
    MutableHandleValue vp) {
  return DebugEnvironmentProxyHandler::singleton.getMaybeSentinelValue(cx, env,
                                                                       id, vp);
}

bool DebugEnvironmentProxy::isFunctionEnvironmentWithThis() {
  return DebugEnvironmentProxyHandler::isFunctionEnvironmentWithThis(
      environment());
}

bool DebugEnvironmentProxy::isOptimizedOut() const {
  EnvironmentObject& e = environment();

  if (DebugEnvironments::hasLiveEnvironment(e)) {
    return false;
  }

  if (e.is<LexicalEnvironmentObject>()) {
    return e.is<BlockLexicalEnvironmentObject>() &&
           !e.as<BlockLexicalEnvironmentObject>().scope().hasEnvironment();
  }

  if (e.is<CallObject>()) {
    return !e.as<CallObject>().callee().needsCallObject() && !maybeSnapshot();
  }

  return false;
}

/*****************************************************************************/

DebugEnvironments::DebugEnvironments(JSContext* cx, Zone* zone)
    : zone_(zone),
      proxiedEnvs(cx),
      missingEnvs(cx->zone()),
      liveEnvs(cx->zone()) {}

DebugEnvironments::~DebugEnvironments() { MOZ_ASSERT(missingEnvs.empty()); }

void DebugEnvironments::trace(JSTracer* trc) { proxiedEnvs.trace(trc); }

void DebugEnvironments::sweep() {
  /*
   * missingEnvs points to debug envs weakly so that debug envs can be
   * released more eagerly.
   */
  for (MissingEnvironmentMap::Enum e(missingEnvs); !e.empty(); e.popFront()) {
    if (IsAboutToBeFinalized(&e.front().value())) {
      /*
       * Note that onPopCall, onPopVar, and onPopLexical rely on
       * missingEnvs to find environment objects that we synthesized for
       * the debugger's sake, and clean up the synthetic environment
       * objects' entries in liveEnvs. So if we remove an entry from
       * missingEnvs here, we must also remove the corresponding
       * liveEnvs entry.
       *
       * Since the DebugEnvironmentProxy is the only thing using its environment
       * object, and the DSO is about to be finalized, you might assume
       * that the synthetic SO is also about to be finalized too, and thus
       * the loop below will take care of things. But complex GC behavior
       * means that marks are only conservative approximations of
       * liveness; we should assume that anything could be marked.
       *
       * Thus, we must explicitly remove the entries from both liveEnvs
       * and missingEnvs here.
       */
      liveEnvs.remove(&e.front().value().unbarrieredGet()->environment());
      e.removeFront();
    } else {
      MissingEnvironmentKey key = e.front().key();
      if (IsForwarded(key.scope())) {
        key.updateScope(Forwarded(key.scope()));
        e.rekeyFront(key);
      }
    }
  }

  /*
   * Scopes can be finalized when a debugger-synthesized EnvironmentObject is
   * no longer reachable via its DebugEnvironmentProxy.
   */
  liveEnvs.sweep();
}

void DebugEnvironments::finish() { proxiedEnvs.clear(); }

#ifdef JSGC_HASH_TABLE_CHECKS
void DebugEnvironments::checkHashTablesAfterMovingGC() {
  /*
   * This is called at the end of StoreBuffer::mark() to check that our
   * postbarriers have worked and that no hashtable keys (or values) are left
   * pointing into the nursery.
   */
  proxiedEnvs.checkAfterMovingGC();
  for (MissingEnvironmentMap::Range r = missingEnvs.all(); !r.empty();
       r.popFront()) {
    CheckGCThingAfterMovingGC(r.front().key().scope());
    // Use unbarrieredGet() to prevent triggering read barrier while collecting.
    CheckGCThingAfterMovingGC(r.front().value().unbarrieredGet());
  }
  for (LiveEnvironmentMap::Range r = liveEnvs.all(); !r.empty(); r.popFront()) {
    CheckGCThingAfterMovingGC(r.front().key());
    CheckGCThingAfterMovingGC(r.front().value().scope_.get());
  }
}
#endif

/*
 * Unfortunately, GetDebugEnvironmentForFrame needs to work even outside debug
 * mode (in particular, JS_GetFrameScopeChain does not require debug mode).
 * Since DebugEnvironments::onPop* are only called in debuggee frames, this
 * means we cannot use any of the maps in DebugEnvironments. This will produce
 * debug scope chains that do not obey the debugger invariants but that is just
 * fine.
 */
static bool CanUseDebugEnvironmentMaps(JSContext* cx) {
  return cx->realm()->isDebuggee();
}

DebugEnvironments* DebugEnvironments::ensureRealmData(JSContext* cx) {
  Realm* realm = cx->realm();
  if (auto* debugEnvs = realm->debugEnvs()) {
    return debugEnvs;
  }

  auto debugEnvs = cx->make_unique<DebugEnvironments>(cx, cx->zone());
  if (!debugEnvs) {
    return nullptr;
  }

  realm->debugEnvsRef() = std::move(debugEnvs);
  return realm->debugEnvs();
}

/* static */
DebugEnvironmentProxy* DebugEnvironments::hasDebugEnvironment(
    JSContext* cx, EnvironmentObject& env) {
  DebugEnvironments* envs = env.realm()->debugEnvs();
  if (!envs) {
    return nullptr;
  }

  if (JSObject* obj = envs->proxiedEnvs.lookup(&env)) {
    MOZ_ASSERT(CanUseDebugEnvironmentMaps(cx));
    return &obj->as<DebugEnvironmentProxy>();
  }

  return nullptr;
}

/* static */
bool DebugEnvironments::addDebugEnvironment(
    JSContext* cx, Handle<EnvironmentObject*> env,
    Handle<DebugEnvironmentProxy*> debugEnv) {
  MOZ_ASSERT(cx->realm() == env->realm());
  MOZ_ASSERT(cx->realm() == debugEnv->nonCCWRealm());

  if (!CanUseDebugEnvironmentMaps(cx)) {
    return true;
  }

  DebugEnvironments* envs = ensureRealmData(cx);
  if (!envs) {
    return false;
  }

  return envs->proxiedEnvs.add(cx, env, debugEnv);
}

/* static */
DebugEnvironmentProxy* DebugEnvironments::hasDebugEnvironment(
    JSContext* cx, const EnvironmentIter& ei) {
  MOZ_ASSERT(!ei.hasSyntacticEnvironment());

  DebugEnvironments* envs = cx->realm()->debugEnvs();
  if (!envs) {
    return nullptr;
  }

  if (MissingEnvironmentMap::Ptr p =
          envs->missingEnvs.lookup(MissingEnvironmentKey(ei))) {
    MOZ_ASSERT(CanUseDebugEnvironmentMaps(cx));
    return p->value();
  }
  return nullptr;
}

/* static */
bool DebugEnvironments::addDebugEnvironment(
    JSContext* cx, const EnvironmentIter& ei,
    Handle<DebugEnvironmentProxy*> debugEnv) {
  MOZ_ASSERT(!ei.hasSyntacticEnvironment());
  MOZ_ASSERT(cx->realm() == debugEnv->nonCCWRealm());

  if (!CanUseDebugEnvironmentMaps(cx)) {
    return true;
  }

  DebugEnvironments* envs = ensureRealmData(cx);
  if (!envs) {
    return false;
  }

  MissingEnvironmentKey key(ei);
  MOZ_ASSERT(!envs->missingEnvs.has(key));
  if (!envs->missingEnvs.put(key,
                             WeakHeapPtr<DebugEnvironmentProxy*>(debugEnv))) {
    ReportOutOfMemory(cx);
    return false;
  }

  // Only add to liveEnvs if we synthesized the debug env on a live
  // frame.
  if (ei.withinInitialFrame()) {
    MOZ_ASSERT(!envs->liveEnvs.has(&debugEnv->environment()));
    if (!envs->liveEnvs.put(&debugEnv->environment(), LiveEnvironmentVal(ei))) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  return true;
}

/* static */
void DebugEnvironments::takeFrameSnapshot(
    JSContext* cx, Handle<DebugEnvironmentProxy*> debugEnv,
    AbstractFramePtr frame) {
  /*
   * When the JS stack frame is popped, the values of unaliased variables
   * are lost. If there is any debug env referring to this environment, save a
   * copy of the unaliased variables' values in an array for later debugger
   * access via DebugEnvironmentProxy::handleUnaliasedAccess.
   *
   * Note: since it is simplest for this function to be infallible, failure
   * in this code will be silently ignored. This does not break any
   * invariants since DebugEnvironmentProxy::maybeSnapshot can already be
   * nullptr.
   */

  JSScript* script = frame.script();

  // Act like no snapshot was taken if we run OOM while taking the snapshot.
  Rooted<GCVector<Value>> vec(cx, GCVector<Value>(cx));
  if (debugEnv->environment().is<CallObject>()) {
    FunctionScope* scope = &script->bodyScope()->as<FunctionScope>();
    uint32_t frameSlotCount = scope->nextFrameSlot();
    MOZ_ASSERT(frameSlotCount <= script->nfixed());

    // For simplicity, copy all frame slots from 0 to the frameSlotCount,
    // even if we don't need all of them (like in the case of a defaults
    // parameter scope having frame slots).
    uint32_t numFormals = frame.numFormalArgs();
    if (!vec.resize(numFormals + frameSlotCount)) {
      cx->recoverFromOutOfMemory();
      return;
    }
    mozilla::PodCopy(vec.begin(), frame.argv(), numFormals);
    for (uint32_t slot = 0; slot < frameSlotCount; slot++) {
      vec[slot + frame.numFormalArgs()].set(frame.unaliasedLocal(slot));
    }

    /*
     * Copy in formals that are not aliased via the scope chain
     * but are aliased via the arguments object.
     */
    if (script->needsArgsObj() && frame.hasArgsObj()) {
      for (unsigned i = 0; i < frame.numFormalArgs(); ++i) {
        if (script->formalLivesInArgumentsObject(i)) {
          vec[i].set(frame.argsObj().arg(i));
        }
      }
    }
  } else {
    uint32_t frameSlotStart;
    uint32_t frameSlotEnd;

    if (debugEnv->environment().is<BlockLexicalEnvironmentObject>()) {
      LexicalScope* scope =
          &debugEnv->environment().as<BlockLexicalEnvironmentObject>().scope();
      frameSlotStart = scope->firstFrameSlot();
      frameSlotEnd = scope->nextFrameSlot();
    } else if (debugEnv->environment()
                   .is<ClassBodyLexicalEnvironmentObject>()) {
      ClassBodyScope* scope = &debugEnv->environment()
                                   .as<ClassBodyLexicalEnvironmentObject>()
                                   .scope();
      frameSlotStart = scope->firstFrameSlot();
      frameSlotEnd = scope->nextFrameSlot();
    } else if (debugEnv->environment().is<VarEnvironmentObject>()) {
      VarEnvironmentObject* env =
          &debugEnv->environment().as<VarEnvironmentObject>();
      if (frame.isFunctionFrame()) {
        VarScope* scope = &env->scope().as<VarScope>();
        frameSlotStart = scope->firstFrameSlot();
        frameSlotEnd = scope->nextFrameSlot();
      } else {
        EvalScope* scope = &env->scope().as<EvalScope>();
        MOZ_ASSERT(scope == script->bodyScope());
        frameSlotStart = 0;
        frameSlotEnd = scope->nextFrameSlot();
      }
    } else {
      MOZ_ASSERT(&debugEnv->environment().as<ModuleEnvironmentObject>() ==
                 script->module()->environment());
      ModuleScope* scope = &script->bodyScope()->as<ModuleScope>();
      frameSlotStart = 0;
      frameSlotEnd = scope->nextFrameSlot();
    }

    uint32_t frameSlotCount = frameSlotEnd - frameSlotStart;
    MOZ_ASSERT(frameSlotCount <= script->nfixed());

    if (!vec.resize(frameSlotCount)) {
      cx->recoverFromOutOfMemory();
      return;
    }
    for (uint32_t slot = frameSlotStart; slot < frameSlotCount; slot++) {
      vec[slot - frameSlotStart].set(frame.unaliasedLocal(slot));
    }
  }

  if (vec.length() == 0) {
    return;
  }

  /*
   * Use a dense array as storage (since proxies do not have trace
   * hooks). This array must not escape into the wild.
   */
  RootedArrayObject snapshot(
      cx, NewDenseCopiedArray(cx, vec.length(), vec.begin()));
  if (!snapshot) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory() || cx->isThrowingOverRecursed());
    cx->clearPendingException();
    return;
  }

  debugEnv->initSnapshot(*snapshot);
}

/* static */
void DebugEnvironments::onPopCall(JSContext* cx, AbstractFramePtr frame) {
  cx->check(frame);

  DebugEnvironments* envs = cx->realm()->debugEnvs();
  if (!envs) {
    return;
  }

  Rooted<DebugEnvironmentProxy*> debugEnv(cx, nullptr);

  FunctionScope* funScope = &frame.script()->bodyScope()->as<FunctionScope>();
  if (funScope->hasEnvironment()) {
    MOZ_ASSERT(frame.callee()->needsCallObject());

    /*
     * The frame may be observed before the prologue has created the
     * CallObject. See EnvironmentIter::settle.
     */
    if (!frame.environmentChain()->is<CallObject>()) {
      return;
    }

    CallObject& callobj = frame.environmentChain()->as<CallObject>();
    envs->liveEnvs.remove(&callobj);
    if (JSObject* obj = envs->proxiedEnvs.lookup(&callobj)) {
      debugEnv = &obj->as<DebugEnvironmentProxy>();
    }
  } else {
    MissingEnvironmentKey key(frame, funScope);
    if (MissingEnvironmentMap::Ptr p = envs->missingEnvs.lookup(key)) {
      debugEnv = p->value();
      envs->liveEnvs.remove(&debugEnv->environment().as<CallObject>());
      envs->missingEnvs.remove(p);
    }
  }

  if (debugEnv) {
    DebugEnvironments::takeFrameSnapshot(cx, debugEnv, frame);
  }
}

void DebugEnvironments::onPopLexical(JSContext* cx, AbstractFramePtr frame,
                                     jsbytecode* pc) {
  cx->check(frame);

  DebugEnvironments* envs = cx->realm()->debugEnvs();
  if (!envs) {
    return;
  }

  EnvironmentIter ei(cx, frame, pc);
  onPopLexical(cx, ei);
}

template <typename Environment, typename Scope>
void DebugEnvironments::onPopGeneric(JSContext* cx, const EnvironmentIter& ei) {
  DebugEnvironments* envs = cx->realm()->debugEnvs();
  if (!envs) {
    return;
  }

  MOZ_ASSERT(ei.withinInitialFrame());
  MOZ_ASSERT(ei.scope().is<Scope>());

  Rooted<Environment*> env(cx);
  if (MissingEnvironmentMap::Ptr p =
          envs->missingEnvs.lookup(MissingEnvironmentKey(ei))) {
    env = &p->value()->environment().as<Environment>();
    envs->missingEnvs.remove(p);
  } else if (ei.hasSyntacticEnvironment()) {
    env = &ei.environment().as<Environment>();
  }

  if (env) {
    envs->liveEnvs.remove(env);

    if (JSObject* obj = envs->proxiedEnvs.lookup(env)) {
      Rooted<DebugEnvironmentProxy*> debugEnv(
          cx, &obj->as<DebugEnvironmentProxy>());
      DebugEnvironments::takeFrameSnapshot(cx, debugEnv, ei.initialFrame());
    }
  }
}

void DebugEnvironments::onPopLexical(JSContext* cx, const EnvironmentIter& ei) {
  if (ei.scope().is<ClassBodyScope>()) {
    onPopGeneric<ScopedLexicalEnvironmentObject, ClassBodyScope>(cx, ei);
  } else {
    onPopGeneric<ScopedLexicalEnvironmentObject, LexicalScope>(cx, ei);
  }
}

void DebugEnvironments::onPopVar(JSContext* cx, const EnvironmentIter& ei) {
  if (ei.scope().is<EvalScope>()) {
    onPopGeneric<VarEnvironmentObject, EvalScope>(cx, ei);
  } else {
    onPopGeneric<VarEnvironmentObject, VarScope>(cx, ei);
  }
}

void DebugEnvironments::onPopWith(AbstractFramePtr frame) {
  Realm* realm = frame.realm();
  if (DebugEnvironments* envs = realm->debugEnvs()) {
    envs->liveEnvs.remove(
        &frame.environmentChain()->as<WithEnvironmentObject>());
  }
}

void DebugEnvironments::onPopModule(JSContext* cx, const EnvironmentIter& ei) {
  onPopGeneric<ModuleEnvironmentObject, ModuleScope>(cx, ei);
}

void DebugEnvironments::onRealmUnsetIsDebuggee(Realm* realm) {
  if (DebugEnvironments* envs = realm->debugEnvs()) {
    envs->proxiedEnvs.clear();
    envs->missingEnvs.clear();
    envs->liveEnvs.clear();
  }
}

bool DebugEnvironments::updateLiveEnvironments(JSContext* cx) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }

  /*
   * Note that we must always update the top frame's environment objects'
   * entries in liveEnvs because we can't be sure code hasn't run in that
   * frame to change the environment chain since we were last called. The
   * fp->prevUpToDate() flag indicates whether the environments of frames
   * older than fp are already included in liveEnvs. It might seem simpler
   * to have fp instead carry a flag indicating whether fp itself is
   * accurately described, but then we would need to clear that flag
   * whenever fp ran code. By storing the 'up to date' bit for fp->prev() in
   * fp, simply popping fp effectively clears the flag for us, at exactly
   * the time when execution resumes fp->prev().
   */
  for (AllFramesIter i(cx); !i.done(); ++i) {
    if (!i.hasUsableAbstractFramePtr()) {
      continue;
    }

    AbstractFramePtr frame = i.abstractFramePtr();
    if (frame.realm() != cx->realm()) {
      continue;
    }

    if (!frame.isDebuggee()) {
      continue;
    }

    RootedObject env(cx);
    RootedScope scope(cx);
    if (!GetFrameEnvironmentAndScope(cx, frame, i.pc(), &env, &scope)) {
      return false;
    }

    for (EnvironmentIter ei(cx, env, scope, frame); ei.withinInitialFrame();
         ei++) {
      if (ei.hasSyntacticEnvironment() && !ei.scope().is<GlobalScope>()) {
        MOZ_ASSERT(ei.environment().realm() == cx->realm());
        DebugEnvironments* envs = ensureRealmData(cx);
        if (!envs) {
          return false;
        }
        if (!envs->liveEnvs.put(&ei.environment(), LiveEnvironmentVal(ei))) {
          ReportOutOfMemory(cx);
          return false;
        }
      }
    }

    if (frame.prevUpToDate()) {
      return true;
    }
    MOZ_ASSERT(frame.realm()->isDebuggee());
    frame.setPrevUpToDate();
  }

  return true;
}

LiveEnvironmentVal* DebugEnvironments::hasLiveEnvironment(
    EnvironmentObject& env) {
  DebugEnvironments* envs = env.realm()->debugEnvs();
  if (!envs) {
    return nullptr;
  }

  if (LiveEnvironmentMap::Ptr p = envs->liveEnvs.lookup(&env)) {
    return &p->value();
  }

  return nullptr;
}

/* static */
void DebugEnvironments::unsetPrevUpToDateUntil(JSContext* cx,
                                               AbstractFramePtr until) {
  // This are two exceptions where fp->prevUpToDate() is cleared without
  // popping the frame. When a frame is rematerialized or has its
  // debuggeeness toggled off->on, all frames younger than the frame must
  // have their prevUpToDate set to false. This is because unrematerialized
  // Ion frames and non-debuggee frames are skipped by updateLiveEnvironments.
  // If in the future a frame suddenly gains a usable AbstractFramePtr via
  // rematerialization or becomes a debuggee, the prevUpToDate invariant
  // will no longer hold for older frames on its stack.
  for (AllFramesIter i(cx); !i.done(); ++i) {
    if (!i.hasUsableAbstractFramePtr()) {
      continue;
    }

    AbstractFramePtr frame = i.abstractFramePtr();
    if (frame == until) {
      return;
    }

    if (frame.realm() != cx->realm()) {
      continue;
    }

    frame.unsetPrevUpToDate();
  }
}

/* static */
void DebugEnvironments::forwardLiveFrame(JSContext* cx, AbstractFramePtr from,
                                         AbstractFramePtr to) {
  DebugEnvironments* envs = cx->realm()->debugEnvs();
  if (!envs) {
    return;
  }

  for (MissingEnvironmentMap::Enum e(envs->missingEnvs); !e.empty();
       e.popFront()) {
    MissingEnvironmentKey key = e.front().key();
    if (key.frame() == from) {
      key.updateFrame(to);
      e.rekeyFront(key);
    }
  }

  for (LiveEnvironmentMap::Enum e(envs->liveEnvs); !e.empty(); e.popFront()) {
    LiveEnvironmentVal& val = e.front().value();
    if (val.frame() == from) {
      val.updateFrame(to);
    }
  }
}

/* static */
void DebugEnvironments::traceLiveFrame(JSTracer* trc, AbstractFramePtr frame) {
  for (MissingEnvironmentMap::Enum e(missingEnvs); !e.empty(); e.popFront()) {
    if (e.front().key().frame() == frame) {
      TraceEdge(trc, &e.front().value(), "debug-env-live-frame-missing-env");
    }
  }
}

/*****************************************************************************/

static JSObject* GetDebugEnvironment(JSContext* cx, const EnvironmentIter& ei);

static DebugEnvironmentProxy* GetDebugEnvironmentForEnvironmentObject(
    JSContext* cx, const EnvironmentIter& ei) {
  Rooted<EnvironmentObject*> env(cx, &ei.environment());
  if (DebugEnvironmentProxy* debugEnv =
          DebugEnvironments::hasDebugEnvironment(cx, *env)) {
    return debugEnv;
  }

  EnvironmentIter copy(cx, ei);
  RootedObject enclosingDebug(cx, GetDebugEnvironment(cx, ++copy));
  if (!enclosingDebug) {
    return nullptr;
  }

  Rooted<DebugEnvironmentProxy*> debugEnv(
      cx, DebugEnvironmentProxy::create(cx, *env, enclosingDebug));
  if (!debugEnv) {
    return nullptr;
  }

  if (!DebugEnvironments::addDebugEnvironment(cx, env, debugEnv)) {
    return nullptr;
  }

  return debugEnv;
}

static DebugEnvironmentProxy* GetDebugEnvironmentForMissing(
    JSContext* cx, const EnvironmentIter& ei) {
  MOZ_ASSERT(!ei.hasSyntacticEnvironment() &&
             (ei.scope().is<FunctionScope>() || ei.scope().is<LexicalScope>() ||
              ei.scope().is<WasmInstanceScope>() ||
              ei.scope().is<WasmFunctionScope>() || ei.scope().is<VarScope>() ||
              ei.scope().kind() == ScopeKind::StrictEval));

  if (DebugEnvironmentProxy* debugEnv =
          DebugEnvironments::hasDebugEnvironment(cx, ei)) {
    return debugEnv;
  }

  EnvironmentIter copy(cx, ei);
  RootedObject enclosingDebug(cx, GetDebugEnvironment(cx, ++copy));
  if (!enclosingDebug) {
    return nullptr;
  }

  /*
   * Create the missing environment object. For lexical environment objects,
   * this takes care of storing variable values after the stack frame has
   * been popped. For call objects, we only use the pretend call object to
   * access callee, bindings and to receive dynamically added
   * properties. Together, this provides the nice invariant that every
   * DebugEnvironmentProxy has a EnvironmentObject.
   *
   * Note: to preserve envChain depth invariants, these lazily-reified
   * envs must not be put on the frame's environment chain; instead, they are
   * maintained via DebugEnvironments hooks.
   */
  Rooted<DebugEnvironmentProxy*> debugEnv(cx);
  if (ei.scope().is<FunctionScope>()) {
    RootedFunction callee(cx,
                          ei.scope().as<FunctionScope>().canonicalFunction());

    JS::ExposeObjectToActiveJS(callee);
    Rooted<CallObject*> callobj(cx,
                                CallObject::createHollowForDebug(cx, callee));
    if (!callobj) {
      return nullptr;
    }

    debugEnv = DebugEnvironmentProxy::create(cx, *callobj, enclosingDebug);
  } else if (ei.scope().is<LexicalScope>()) {
    Rooted<LexicalScope*> lexicalScope(cx, &ei.scope().as<LexicalScope>());
    Rooted<BlockLexicalEnvironmentObject*> env(
        cx,
        BlockLexicalEnvironmentObject::createHollowForDebug(cx, lexicalScope));
    if (!env) {
      return nullptr;
    }

    debugEnv = DebugEnvironmentProxy::create(cx, *env, enclosingDebug);
  } else if (ei.scope().is<WasmInstanceScope>()) {
    Rooted<WasmInstanceScope*> wasmInstanceScope(
        cx, &ei.scope().as<WasmInstanceScope>());
    Rooted<WasmInstanceEnvironmentObject*> env(
        cx, WasmInstanceEnvironmentObject::createHollowForDebug(
                cx, wasmInstanceScope));
    if (!env) {
      return nullptr;
    }

    debugEnv = DebugEnvironmentProxy::create(cx, *env, enclosingDebug);
  } else if (ei.scope().is<WasmFunctionScope>()) {
    Rooted<WasmFunctionScope*> wasmFunctionScope(
        cx, &ei.scope().as<WasmFunctionScope>());
    RootedObject enclosing(
        cx, &enclosingDebug->as<DebugEnvironmentProxy>().environment());
    Rooted<WasmFunctionCallObject*> callobj(
        cx, WasmFunctionCallObject::createHollowForDebug(cx, enclosing,
                                                         wasmFunctionScope));
    if (!callobj) {
      return nullptr;
    }

    debugEnv = DebugEnvironmentProxy::create(cx, *callobj, enclosingDebug);
  } else {
    Rooted<Scope*> scope(cx, &ei.scope());
    MOZ_ASSERT(scope->is<VarScope>() || scope->kind() == ScopeKind::StrictEval);

    Rooted<VarEnvironmentObject*> env(
        cx, VarEnvironmentObject::createHollowForDebug(cx, scope));
    if (!env) {
      return nullptr;
    }

    debugEnv = DebugEnvironmentProxy::create(cx, *env, enclosingDebug);
  }

  if (!debugEnv) {
    return nullptr;
  }

  if (!DebugEnvironments::addDebugEnvironment(cx, ei, debugEnv)) {
    return nullptr;
  }

  return debugEnv;
}

static JSObject* GetDebugEnvironmentForNonEnvironmentObject(
    const EnvironmentIter& ei) {
  JSObject& enclosing = ei.enclosingEnvironment();
#ifdef DEBUG
  JSObject* o = &enclosing;
  while ((o = o->enclosingEnvironment())) {
    MOZ_ASSERT(!o->is<EnvironmentObject>());
  }
#endif
  return &enclosing;
}

static JSObject* GetDebugEnvironment(JSContext* cx, const EnvironmentIter& ei) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return nullptr;
  }

  if (ei.done()) {
    return GetDebugEnvironmentForNonEnvironmentObject(ei);
  }

  if (ei.hasAnyEnvironmentObject()) {
    return GetDebugEnvironmentForEnvironmentObject(cx, ei);
  }

  if (ei.scope().is<FunctionScope>() || ei.scope().is<LexicalScope>() ||
      ei.scope().is<WasmInstanceScope>() ||
      ei.scope().is<WasmFunctionScope>() || ei.scope().is<VarScope>() ||
      ei.scope().kind() == ScopeKind::StrictEval) {
    return GetDebugEnvironmentForMissing(cx, ei);
  }

  EnvironmentIter copy(cx, ei);
  return GetDebugEnvironment(cx, ++copy);
}

JSObject* js::GetDebugEnvironmentForFunction(JSContext* cx,
                                             HandleFunction fun) {
  cx->check(fun);
  MOZ_ASSERT(CanUseDebugEnvironmentMaps(cx));
  if (!DebugEnvironments::updateLiveEnvironments(cx)) {
    return nullptr;
  }
  JSScript* script = JSFunction::getOrCreateScript(cx, fun);
  if (!script) {
    return nullptr;
  }
  EnvironmentIter ei(cx, fun->environment(), script->enclosingScope());
  return GetDebugEnvironment(cx, ei);
}

JSObject* js::GetDebugEnvironmentForSuspendedGenerator(
    JSContext* cx, JSScript* script, AbstractGeneratorObject& genObj) {
  RootedObject env(cx);
  RootedScope scope(cx);
  GetSuspendedGeneratorEnvironmentAndScope(genObj, script, &env, &scope);

  EnvironmentIter ei(cx, env, scope);
  return GetDebugEnvironment(cx, ei);
}

JSObject* js::GetDebugEnvironmentForFrame(JSContext* cx, AbstractFramePtr frame,
                                          jsbytecode* pc) {
  cx->check(frame);
  if (CanUseDebugEnvironmentMaps(cx) &&
      !DebugEnvironments::updateLiveEnvironments(cx)) {
    return nullptr;
  }

  RootedObject env(cx);
  RootedScope scope(cx);
  if (!GetFrameEnvironmentAndScope(cx, frame, pc, &env, &scope)) {
    return nullptr;
  }

  EnvironmentIter ei(cx, env, scope, frame);
  return GetDebugEnvironment(cx, ei);
}

JSObject* js::GetDebugEnvironmentForGlobalLexicalEnvironment(JSContext* cx) {
  EnvironmentIter ei(cx, &cx->global()->lexicalEnvironment(),
                     &cx->global()->emptyGlobalScope());
  return GetDebugEnvironment(cx, ei);
}

bool js::CreateObjectsForEnvironmentChain(JSContext* cx,
                                          HandleObjectVector chain,
                                          HandleObject terminatingEnv,
                                          MutableHandleObject envObj) {
#ifdef DEBUG
  for (size_t i = 0; i < chain.length(); ++i) {
    cx->check(chain[i]);
    MOZ_ASSERT(!chain[i]->is<GlobalObject>() &&
               !chain[i]->is<NonSyntacticVariablesObject>());
  }
#endif

  // Construct With object wrappers for the things on this environment chain
  // and use the result as the thing to scope the function to.
  Rooted<WithEnvironmentObject*> withEnv(cx);
  RootedObject enclosingEnv(cx, terminatingEnv);
  for (size_t i = chain.length(); i > 0;) {
    withEnv =
        WithEnvironmentObject::createNonSyntactic(cx, chain[--i], enclosingEnv);
    if (!withEnv) {
      return false;
    }
    enclosingEnv = withEnv;
  }

  envObj.set(enclosingEnv);
  return true;
}

JSObject& WithEnvironmentObject::object() const {
  return getReservedSlot(OBJECT_SLOT).toObject();
}

JSObject* WithEnvironmentObject::withThis() const {
  return &getReservedSlot(THIS_SLOT).toObject();
}

bool WithEnvironmentObject::isSyntactic() const {
  Value v = getReservedSlot(SCOPE_SLOT);
  MOZ_ASSERT(v.isPrivateGCThing() || v.isNull());
  return v.isPrivateGCThing();
}

WithScope& WithEnvironmentObject::scope() const {
  MOZ_ASSERT(isSyntactic());
  return *static_cast<WithScope*>(getReservedSlot(SCOPE_SLOT).toGCThing());
}

ModuleEnvironmentObject* js::GetModuleEnvironmentForScript(JSScript* script) {
  ModuleObject* module = GetModuleObjectForScript(script);
  if (!module) {
    return nullptr;
  }

  return module->environment();
}

ModuleObject* js::GetModuleObjectForScript(JSScript* script) {
  for (ScopeIter si(script); si; si++) {
    if (si.kind() == ScopeKind::Module) {
      return si.scope()->as<ModuleScope>().module();
    }
  }
  return nullptr;
}

static bool GetThisValueForDebuggerEnvironmentIterMaybeOptimizedOut(
    JSContext* cx, const EnvironmentIter& originalIter, HandleObject scopeChain,
    const jsbytecode* pc, MutableHandleValue res) {
  for (EnvironmentIter ei(cx, originalIter); ei; ei++) {
    if (ei.scope().kind() == ScopeKind::Module) {
      res.setUndefined();
      return true;
    }

    if (!ei.scope().is<FunctionScope>() ||
        ei.scope().as<FunctionScope>().canonicalFunction()->hasLexicalThis()) {
      continue;
    }

    RootedScript script(cx, ei.scope().as<FunctionScope>().script());

    if (ei.withinInitialFrame()) {
      MOZ_ASSERT(pc, "must have PC if there is an initial frame");

      // Figure out if we executed JSOp::FunctionThis and set it.
      bool executedInitThisOp = false;
      if (script->functionHasThisBinding()) {
        for (const BytecodeLocation& loc : js::AllBytecodesIterable(script)) {
          if (loc.getOp() == JSOp::FunctionThis) {
            // The next op after JSOp::FunctionThis always sets it.
            executedInitThisOp = pc > GetNextPc(loc.toRawBytecode());
            break;
          }
        }
      }

      if (!executedInitThisOp) {
        AbstractFramePtr initialFrame = ei.initialFrame();
        // Either we're yet to initialize the this-binding
        // (JSOp::FunctionThis), or the script does not have a this-binding
        // (because it doesn't use |this|).

        // If our this-argument is an object, or we're in strict mode,
        // the this-binding is always the same as our this-argument.
        if (initialFrame.thisArgument().isObject() || script->strict()) {
          res.set(initialFrame.thisArgument());
          return true;
        }

        // We didn't initialize the this-binding yet. Determine the
        // correct |this| value for this frame (box primitives if not
        // in strict mode), and assign it to the this-argument slot so
        // JSOp::FunctionThis will use it and not box a second time.
        if (!GetFunctionThis(cx, initialFrame, res)) {
          return false;
        }
        initialFrame.thisArgument() = res;
        return true;
      }
    }

    if (!script->functionHasThisBinding()) {
      res.setMagic(JS_OPTIMIZED_OUT);
      return true;
    }

    for (Rooted<BindingIter> bi(cx, BindingIter(script)); bi; bi++) {
      if (bi.name() != cx->names().dotThis) {
        continue;
      }

      BindingLocation loc = bi.location();
      if (loc.kind() == BindingLocation::Kind::Environment) {
        RootedObject callObj(cx, &ei.environment().as<CallObject>());
        return GetProperty(cx, callObj, callObj, bi.name()->asPropertyName(),
                           res);
      }

      if (loc.kind() == BindingLocation::Kind::Frame) {
        if (ei.withinInitialFrame()) {
          res.set(ei.initialFrame().unaliasedLocal(loc.slot()));
          return true;
        }

        if (ei.hasAnyEnvironmentObject()) {
          RootedObject env(cx, &ei.environment());
          AbstractGeneratorObject* genObj =
              GetGeneratorObjectForEnvironment(cx, env);
          if (genObj && genObj->isSuspended() && genObj->hasStackStorage()) {
            res.set(genObj->getUnaliasedLocal(loc.slot()));
            return true;
          }
        }
      }

      res.setMagic(JS_OPTIMIZED_OUT);
      return true;
    }

    MOZ_CRASH("'this' binding must be found");
  }

  GetNonSyntacticGlobalThis(cx, scopeChain, res);
  return true;
}

bool js::GetThisValueForDebuggerFrameMaybeOptimizedOut(JSContext* cx,
                                                       AbstractFramePtr frame,
                                                       jsbytecode* pc,
                                                       MutableHandleValue res) {
  RootedObject scopeChain(cx);
  RootedScope scope(cx);
  if (!GetFrameEnvironmentAndScope(cx, frame, pc, &scopeChain, &scope)) {
    return false;
  }

  EnvironmentIter ei(cx, scopeChain, scope, frame);
  return GetThisValueForDebuggerEnvironmentIterMaybeOptimizedOut(
      cx, ei, scopeChain, pc, res);
}

bool js::GetThisValueForDebuggerSuspendedGeneratorMaybeOptimizedOut(
    JSContext* cx, AbstractGeneratorObject& genObj, JSScript* script,
    MutableHandleValue res) {
  RootedObject scopeChain(cx);
  RootedScope scope(cx);
  GetSuspendedGeneratorEnvironmentAndScope(genObj, script, &scopeChain, &scope);

  EnvironmentIter ei(cx, scopeChain, scope);
  return GetThisValueForDebuggerEnvironmentIterMaybeOptimizedOut(
      cx, ei, scopeChain, nullptr, res);
}

bool js::CheckLexicalNameConflict(
    JSContext* cx, Handle<ExtensibleLexicalEnvironmentObject*> lexicalEnv,
    HandleObject varObj, HandlePropertyName name) {
  const char* redeclKind = nullptr;
  RootedId id(cx, NameToId(name));
  mozilla::Maybe<PropertyInfo> prop;
  if (varObj->is<GlobalObject>() &&
      varObj->as<GlobalObject>().realm()->isInVarNames(name)) {
    // ES 15.1.11 step 5.a
    redeclKind = "var";
  } else if ((prop = lexicalEnv->lookup(cx, name))) {
    // ES 15.1.11 step 5.b
    redeclKind = prop->writable() ? "let" : "const";
  } else if (varObj->is<NativeObject>() &&
             (prop = varObj->as<NativeObject>().lookup(cx, name))) {
    // Faster path for ES 15.1.11 step 5.c-d when the shape can be found
    // without going through a resolve hook.
    if (!prop->configurable()) {
      redeclKind = "non-configurable global property";
    }
  } else {
    // ES 15.1.11 step 5.c-d
    Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);
    if (!GetOwnPropertyDescriptor(cx, varObj, id, &desc)) {
      return false;
    }
    if (desc.isSome() && !desc->configurable()) {
      redeclKind = "non-configurable global property";
    }
  }

  if (redeclKind) {
    ReportRuntimeRedeclaration(cx, name, redeclKind);
    return false;
  }

  return true;
}

[[nodiscard]] static bool CheckVarNameConflict(
    JSContext* cx, Handle<LexicalEnvironmentObject*> lexicalEnv,
    HandlePropertyName name) {
  mozilla::Maybe<PropertyInfo> prop = lexicalEnv->lookup(cx, name);
  if (prop.isSome()) {
    ReportRuntimeRedeclaration(cx, name, prop->writable() ? "let" : "const");
    return false;
  }
  return true;
}

static void ReportCannotDeclareGlobalBinding(JSContext* cx,
                                             HandlePropertyName name,
                                             const char* reason) {
  if (UniqueChars printable = AtomToPrintableString(cx, name)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CANT_DECLARE_GLOBAL_BINDING,
                              printable.get(), reason);
  }
}

bool js::CheckCanDeclareGlobalBinding(JSContext* cx,
                                      Handle<GlobalObject*> global,
                                      HandlePropertyName name,
                                      bool isFunction) {
  RootedId id(cx, NameToId(name));
  Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);
  if (!GetOwnPropertyDescriptor(cx, global, id, &desc)) {
    return false;
  }

  // ES 8.1.1.4.15 CanDeclareGlobalVar
  // ES 8.1.1.4.16 CanDeclareGlobalFunction

  // Step 4.
  if (desc.isNothing()) {
    // 8.1.14.15 step 6.
    // 8.1.14.16 step 5.
    if (global->isExtensible()) {
      return true;
    }

    ReportCannotDeclareGlobalBinding(cx, name, "global is non-extensible");
    return false;
  }

  // Global functions have additional restrictions.
  if (isFunction) {
    // 8.1.14.16 step 6.
    if (desc->configurable()) {
      return true;
    }

    // 8.1.14.16 step 7.
    if (desc->isDataDescriptor() && desc->writable() && desc->enumerable()) {
      return true;
    }

    ReportCannotDeclareGlobalBinding(cx, name,
                                     "property must be configurable or "
                                     "both writable and enumerable");
    return false;
  }

  return true;
}

// Add the var/let/const bindings to the variables environment of a global or
// sloppy-eval script. The redeclaration checks should already have been
// performed.
static bool InitGlobalOrEvalDeclarations(
    JSContext* cx, HandleScript script,
    Handle<ExtensibleLexicalEnvironmentObject*> lexicalEnv,
    HandleObject varObj) {
  Rooted<BindingIter> bi(cx, BindingIter(script));
  for (; bi; bi++) {
    if (bi.isTopLevelFunction()) {
      continue;
    }

    RootedPropertyName name(cx, bi.name()->asPropertyName());
    unsigned attrs = script->isForEval() ? JSPROP_ENUMERATE
                                         : JSPROP_ENUMERATE | JSPROP_PERMANENT;

    switch (bi.kind()) {
      case BindingKind::Var: {
        PropertyResult prop;
        RootedObject obj2(cx);
        if (!LookupProperty(cx, varObj, name, &obj2, &prop)) {
          return false;
        }

        if (prop.isNotFound() ||
            (obj2 != varObj && varObj->is<GlobalObject>())) {
          if (!DefineDataProperty(cx, varObj, name, UndefinedHandleValue,
                                  attrs)) {
            return false;
          }
        }

        if (varObj->is<GlobalObject>()) {
          if (!varObj->as<GlobalObject>().realm()->addToVarNames(cx, name)) {
            return false;
          }
        }

        break;
      }

      case BindingKind::Const:
        attrs |= JSPROP_READONLY;
        [[fallthrough]];

      case BindingKind::Let: {
        RootedId id(cx, NameToId(name));
        RootedValue uninitialized(cx, MagicValue(JS_UNINITIALIZED_LEXICAL));
        if (!NativeDefineDataProperty(cx, lexicalEnv, id, uninitialized,
                                      attrs)) {
          return false;
        }

        break;
      }

      default:
        MOZ_CRASH("Expected binding kind");
        return false;
    }
  }

  return true;
}

// Define the hoisted top-level functions on the variables environment of a
// global or sloppy-eval script. Redeclaration checks must already have been
// performed.
static bool InitHoistedFunctionDeclarations(JSContext* cx, HandleScript script,
                                            HandleObject envChain,
                                            HandleObject varObj,
                                            GCThingIndex lastFun) {
  // The inner-functions up to `lastFun` are the hoisted function declarations
  // of the script. We must clone and bind them now.
  for (size_t i = 0; i <= lastFun; ++i) {
    const JS::GCCellPtr& thing = script->gcthings()[i];

    // Skip the initial scopes. In practice, there is at most one variables and
    // one lexical scope.
    if (thing.is<js::Scope>()) {
      MOZ_ASSERT(i < 2);
      continue;
    }

    RootedFunction fun(cx, &thing.as<JSObject>().as<JSFunction>());
    RootedPropertyName name(cx, fun->explicitName()->asPropertyName());

    // Clone the function before exposing to script as a binding.
    JSObject* clone = Lambda(cx, fun, envChain);
    if (!clone) {
      return false;
    }
    RootedValue rval(cx, ObjectValue(*clone));

    PropertyResult prop;
    RootedObject pobj(cx);
    if (!LookupProperty(cx, varObj, name, &pobj, &prop)) {
      return false;
    }

    // ECMA requires functions defined when entering Eval code to be
    // impermanent.
    unsigned attrs = script->isForEval() ? JSPROP_ENUMERATE
                                         : JSPROP_ENUMERATE | JSPROP_PERMANENT;

    if (prop.isNotFound() || pobj != varObj) {
      if (!DefineDataProperty(cx, varObj, name, rval, attrs)) {
        return false;
      }

      if (varObj->is<GlobalObject>()) {
        if (!varObj->as<GlobalObject>().realm()->addToVarNames(cx, name)) {
          return false;
        }
      }

      // Done processing this function.
      continue;
    }

    /*
     * A DebugEnvironmentProxy is okay here, and sometimes necessary. If
     * Debugger.Frame.prototype.eval defines a function with the same name as an
     * extant variable in the frame, the DebugEnvironmentProxy takes care of
     * storing the function in the stack frame (for non-aliased variables) or on
     * the scope object (for aliased).
     */
    MOZ_ASSERT(varObj->is<NativeObject>() ||
               varObj->is<DebugEnvironmentProxy>());
    if (varObj->is<GlobalObject>()) {
      PropertyInfo propInfo = prop.propertyInfo();
      if (propInfo.configurable()) {
        if (!DefineDataProperty(cx, varObj, name, rval, attrs)) {
          return false;
        }
      } else {
        MOZ_ASSERT(propInfo.isDataProperty());
        MOZ_ASSERT(propInfo.writable());
        MOZ_ASSERT(propInfo.enumerable());
      }

      // Careful: the presence of a shape, even one appearing to derive from
      // a variable declaration, doesn't mean it's in [[VarNames]].
      if (!varObj->as<GlobalObject>().realm()->addToVarNames(cx, name)) {
        return false;
      }
    }

    /*
     * Non-global properties, and global properties which we aren't simply
     * redefining, must be set.  First, this preserves their attributes.
     * Second, this will produce warnings and/or errors as necessary if the
     * specified Call object property is not writable (const).
     */

    RootedId id(cx, NameToId(name));
    if (!PutProperty(cx, varObj, id, rval, script->strict())) {
      return false;
    }
  }

  return true;
}

bool js::CheckGlobalDeclarationConflicts(
    JSContext* cx, HandleScript script,
    Handle<ExtensibleLexicalEnvironmentObject*> lexicalEnv,
    HandleObject varObj) {
  // Due to the extensibility of the global lexical environment, we must
  // check for redeclaring a binding.
  //
  // In the case of non-syntactic environment chains, we are checking
  // redeclarations against the non-syntactic lexical environment and the
  // variables object that the lexical environment corresponds to.
  RootedPropertyName name(cx);
  Rooted<BindingIter> bi(cx, BindingIter(script));

  // ES 15.1.11 GlobalDeclarationInstantiation

  // Step 6.
  //
  // Check 'var' declarations do not conflict with existing bindings in the
  // global lexical environment.
  for (; bi; bi++) {
    if (bi.kind() != BindingKind::Var) {
      break;
    }
    name = bi.name()->asPropertyName();
    if (!CheckVarNameConflict(cx, lexicalEnv, name)) {
      return false;
    }

    // Step 10 and 12.
    //
    // Check that global functions and vars may be declared.
    if (varObj->is<GlobalObject>()) {
      Handle<GlobalObject*> global = varObj.as<GlobalObject>();
      if (!CheckCanDeclareGlobalBinding(cx, global, name,
                                        bi.isTopLevelFunction())) {
        return false;
      }
    }
  }

  // Step 5.
  //
  // Check that lexical bindings do not conflict.
  for (; bi; bi++) {
    name = bi.name()->asPropertyName();
    if (!CheckLexicalNameConflict(cx, lexicalEnv, varObj, name)) {
      return false;
    }
  }

  return true;
}

[[nodiscard]] static bool CheckVarNameConflictsInEnv(JSContext* cx,
                                                     HandleScript script,
                                                     HandleObject obj) {
  Rooted<LexicalEnvironmentObject*> env(cx);

  if (obj->is<LexicalEnvironmentObject>()) {
    env = &obj->as<LexicalEnvironmentObject>();
  } else if (obj->is<DebugEnvironmentProxy>() &&
             obj->as<DebugEnvironmentProxy>()
                 .environment()
                 .is<LexicalEnvironmentObject>()) {
    env = &obj->as<DebugEnvironmentProxy>()
               .environment()
               .as<LexicalEnvironmentObject>();
  } else {
    // Environment cannot contain lexical bindings.
    return true;
  }

  if (env->is<BlockLexicalEnvironmentObject>() &&
      env->as<BlockLexicalEnvironmentObject>().scope().kind() ==
          ScopeKind::SimpleCatch) {
    // Annex B.3.5 allows redeclaring simple (non-destructured) catch parameters
    // with var declarations.
    return true;
  }

  RootedPropertyName name(cx);
  for (BindingIter bi(script); bi; bi++) {
    name = bi.name()->asPropertyName();
    if (!CheckVarNameConflict(cx, env, name)) {
      return false;
    }
  }

  return true;
}

static bool CheckArgumentsRedeclaration(JSContext* cx, HandleScript script) {
  for (BindingIter bi(script); bi; bi++) {
    if (bi.name() == cx->names().arguments) {
      ReportRuntimeRedeclaration(cx, cx->names().arguments, "let");
      return false;
    }
  }

  return true;
}

static bool CheckEvalDeclarationConflicts(JSContext* cx, HandleScript script,
                                          HandleObject scopeChain,
                                          HandleObject varObj) {
  // Strict eval has its own call objects and we shouldn't end up here.
  //
  // Non-strict eval may introduce 'var' bindings that conflict with lexical
  // bindings in an enclosing lexical scope.
  MOZ_ASSERT(!script->bodyScope()->hasEnvironment());
  MOZ_ASSERT(!script->strict());

  MOZ_ASSERT(script->bodyScope()->as<EvalScope>().hasBindings());

  RootedObject obj(cx, scopeChain);

  // ES 18.2.1.3.

  // Step 5.
  //
  // Check that a direct eval will not hoist 'var' bindings over lexical
  // bindings with the same name.
  while (obj != varObj) {
    if (!CheckVarNameConflictsInEnv(cx, script, obj)) {
      return false;
    }
    obj = obj->enclosingEnvironment();
  }

  // Check for redeclared "arguments" in function parameter expressions.
  //
  // Direct eval in function parameter expressions isn't allowed to redeclare
  // the implicit "arguments" bindings:
  //   function f(a = eval("var arguments;")) {}
  //
  // |varObj| isn't a CallObject when the direct eval occurs in the function
  // body and the extra function body var scope is present. The extra var scope
  // is present iff the function has parameter expressions. So when we test
  // that |varObj| is a CallObject and function parameter expressions are
  // present, we can pinpoint the direct eval location to be in a function
  // parameter expression. Additionally we must ensure the function isn't an
  // arrow function, because arrow functions don't have an implicit "arguments"
  // binding.
  if (script->isDirectEvalInFunction() && varObj->is<CallObject>()) {
    JSFunction* fun = &varObj->as<CallObject>().callee();
    JSScript* funScript = fun->nonLazyScript();
    if (funScript->functionHasParameterExprs() && !fun->isArrow()) {
      if (!CheckArgumentsRedeclaration(cx, script)) {
        return false;
      }
    }
  }

  // Step 8.
  //
  // Check that global functions may be declared.
  if (varObj->is<GlobalObject>()) {
    Handle<GlobalObject*> global = varObj.as<GlobalObject>();
    RootedPropertyName name(cx);
    for (Rooted<BindingIter> bi(cx, BindingIter(script)); bi; bi++) {
      name = bi.name()->asPropertyName();
      if (!CheckCanDeclareGlobalBinding(cx, global, name,
                                        bi.isTopLevelFunction())) {
        return false;
      }
    }
  }

  return true;
}

bool js::GlobalOrEvalDeclInstantiation(JSContext* cx, HandleObject envChain,
                                       HandleScript script,
                                       GCThingIndex lastFun) {
  MOZ_ASSERT(script->isGlobalCode() || script->isForEval());

  RootedObject varObj(cx, &GetVariablesObject(envChain));
  Rooted<ExtensibleLexicalEnvironmentObject*> lexicalEnv(cx);

  if (script->isForEval()) {
    if (!CheckEvalDeclarationConflicts(cx, script, envChain, varObj)) {
      return false;
    }
  } else {
    lexicalEnv = &NearestEnclosingExtensibleLexicalEnvironment(envChain);
    if (!CheckGlobalDeclarationConflicts(cx, script, lexicalEnv, varObj)) {
      return false;
    }
  }

  if (!InitGlobalOrEvalDeclarations(cx, script, lexicalEnv, varObj)) {
    return false;
  }

  return InitHoistedFunctionDeclarations(cx, script, envChain, varObj, lastFun);
}

bool js::InitFunctionEnvironmentObjects(JSContext* cx, AbstractFramePtr frame) {
  MOZ_ASSERT(frame.isFunctionFrame());
  MOZ_ASSERT(frame.callee()->needsSomeEnvironmentObject());

  RootedFunction callee(cx, frame.callee());

  // Named lambdas may have an environment that holds itself for recursion.
  if (callee->needsNamedLambdaEnvironment()) {
    NamedLambdaObject* declEnv = NamedLambdaObject::create(cx, frame);
    if (!declEnv) {
      return false;
    }
    frame.pushOnEnvironmentChain(*declEnv);
  }

  // If the function has parameter default expressions, there may be an
  // extra environment to hold the parameters.
  if (callee->needsCallObject()) {
    CallObject* callObj = CallObject::create(cx, frame);
    if (!callObj) {
      return false;
    }
    frame.pushOnEnvironmentChain(*callObj);
  }

  return true;
}

bool js::PushVarEnvironmentObject(JSContext* cx, HandleScope scope,
                                  AbstractFramePtr frame) {
  VarEnvironmentObject* env = VarEnvironmentObject::create(cx, scope, frame);
  if (!env) {
    return false;
  }
  frame.pushOnEnvironmentChain(*env);
  return true;
}

bool js::GetFrameEnvironmentAndScope(JSContext* cx, AbstractFramePtr frame,
                                     jsbytecode* pc, MutableHandleObject env,
                                     MutableHandleScope scope) {
  env.set(frame.environmentChain());

  if (frame.isWasmDebugFrame()) {
    RootedWasmInstanceObject instance(cx, frame.wasmInstance()->object());
    uint32_t funcIndex = frame.asWasmDebugFrame()->funcIndex();
    scope.set(WasmInstanceObject::getFunctionScope(cx, instance, funcIndex));
    if (!scope) {
      return false;
    }
  } else {
    scope.set(frame.script()->innermostScope(pc));
  }
  return true;
}

void js::GetSuspendedGeneratorEnvironmentAndScope(
    AbstractGeneratorObject& genObj, JSScript* script, MutableHandleObject env,
    MutableHandleScope scope) {
  env.set(&genObj.environmentChain());

  jsbytecode* pc =
      script->offsetToPC(script->resumeOffsets()[genObj.resumeIndex()]);
  scope.set(script->innermostScope(pc));
}

#ifdef DEBUG

typedef HashSet<PropertyName*> PropertyNameSet;

static bool RemoveReferencedNames(JSContext* cx, HandleScript script,
                                  PropertyNameSet& remainingNames) {
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

  AllBytecodesIterable iter(script);
  for (BytecodeLocation loc : iter) {
    PropertyName* name;

    switch (loc.getOp()) {
      case JSOp::GetName:
      case JSOp::SetName:
      case JSOp::StrictSetName:
        name = script->getName(loc.toRawBytecode());
        break;

      case JSOp::GetGName:
      case JSOp::SetGName:
      case JSOp::StrictSetGName:
        if (script->hasNonSyntacticScope()) {
          name = script->getName(loc.toRawBytecode());
        } else {
          name = nullptr;
        }
        break;

      case JSOp::GetAliasedVar:
      case JSOp::SetAliasedVar:
        name = EnvironmentCoordinateNameSlow(script, loc.toRawBytecode());
        break;

      default:
        name = nullptr;
        break;
    }

    if (name) {
      remainingNames.remove(name);
    }
  }

  RootedFunction fun(cx);
  RootedScript innerScript(cx);
  for (JS::GCCellPtr gcThing : script->gcthings()) {
    if (!gcThing.is<JSObject>()) {
      continue;
    }
    JSObject* obj = &gcThing.as<JSObject>();

    if (!obj->is<JSFunction>()) {
      continue;
    }
    fun = &obj->as<JSFunction>();

    if (!fun->isInterpreted()) {
      continue;
    }

    innerScript = JSFunction::getOrCreateScript(cx, fun);
    if (!innerScript) {
      return false;
    }

    if (!RemoveReferencedNames(cx, innerScript, remainingNames)) {
      return false;
    }
  }

  return true;
}

static bool AnalyzeEntrainedVariablesInScript(JSContext* cx,
                                              HandleScript script,
                                              HandleScript innerScript) {
  PropertyNameSet remainingNames(cx);

  for (BindingIter bi(script); bi; bi++) {
    if (bi.closedOver()) {
      PropertyName* name = bi.name()->asPropertyName();
      PropertyNameSet::AddPtr p = remainingNames.lookupForAdd(name);
      if (!p && !remainingNames.add(p, name)) {
        return false;
      }
    }
  }

  if (!RemoveReferencedNames(cx, innerScript, remainingNames)) {
    return false;
  }

  if (!remainingNames.empty()) {
    Sprinter buf(cx);
    if (!buf.init()) {
      return false;
    }

    buf.printf("Script ");

    if (JSAtom* name = script->function()->displayAtom()) {
      buf.putString(name);
      buf.printf(" ");
    }

    buf.printf("(%s:%u) has variables entrained by ", script->filename(),
               script->lineno());

    if (JSAtom* name = innerScript->function()->displayAtom()) {
      buf.putString(name);
      buf.printf(" ");
    }

    buf.printf("(%s:%u) ::", innerScript->filename(), innerScript->lineno());

    for (PropertyNameSet::Range r = remainingNames.all(); !r.empty();
         r.popFront()) {
      buf.printf(" ");
      buf.putString(r.front());
    }

    printf("%s\n", buf.string());
  }

  RootedFunction fun(cx);
  RootedScript innerInnerScript(cx);
  for (JS::GCCellPtr gcThing : script->gcthings()) {
    if (!gcThing.is<JSObject>()) {
      continue;
    }
    JSObject* obj = &gcThing.as<JSObject>();

    if (!obj->is<JSFunction>()) {
      continue;
    }
    fun = &obj->as<JSFunction>();

    if (!fun->isInterpreted()) {
      continue;
    }

    innerInnerScript = JSFunction::getOrCreateScript(cx, fun);
    if (!innerInnerScript) {
      return false;
    }

    if (!AnalyzeEntrainedVariablesInScript(cx, script, innerInnerScript)) {
      return false;
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
bool js::AnalyzeEntrainedVariables(JSContext* cx, HandleScript script) {
  RootedFunction fun(cx);
  RootedScript innerScript(cx);
  for (JS::GCCellPtr gcThing : script->gcthings()) {
    if (!gcThing.is<JSObject>()) {
      continue;
    }
    JSObject* obj = &gcThing.as<JSObject>();

    if (!obj->is<JSFunction>()) {
      continue;
    }
    fun = &obj->as<JSFunction>();

    if (!fun->isInterpreted()) {
      continue;
    }

    innerScript = JSFunction::getOrCreateScript(cx, fun);
    if (!innerScript) {
      return false;
    }

    if (fun->needsCallObject()) {
      if (!AnalyzeEntrainedVariablesInScript(cx, script, innerScript)) {
        return false;
      }
    }

    if (!AnalyzeEntrainedVariables(cx, innerScript)) {
      return false;
    }
  }

  return true;
}
#endif

JSObject* js::MaybeOptimizeBindGlobalName(JSContext* cx,
                                          Handle<GlobalObject*> global,
                                          HandlePropertyName name) {
  // We can bind name to the global lexical scope if the binding already
  // exists, is initialized, and is writable (i.e., an initialized
  // 'let') at compile time.
  Rooted<GlobalLexicalEnvironmentObject*> env(cx,
                                              &global->lexicalEnvironment());
  mozilla::Maybe<PropertyInfo> prop = env->lookup(cx, name);
  if (prop.isSome()) {
    if (prop->writable() &&
        !env->getSlot(prop->slot()).isMagic(JS_UNINITIALIZED_LEXICAL)) {
      return env;
    }
    return nullptr;
  }

  prop = global->lookup(cx, name);
  if (prop.isSome()) {
    // If the property does not currently exist on the global lexical
    // scope, we can bind name to the global object if the property
    // exists on the global and is non-configurable, as then it cannot
    // be shadowed.
    if (!prop->configurable()) {
      return global;
    }
  }

  return nullptr;
}

#if defined(DEBUG) || defined(JS_JITSPEW)
static void DumpEnvironmentObject(JSObject* unrootedEnvObj) {
  JSContext* cx = TlsContext.get();
  if (!cx) {
    fprintf(stderr, "*** can't get JSContext for current thread\n");
    return;
  }

  Rooted<JSObject*> envObj(cx, unrootedEnvObj);
  while (envObj) {
    Rooted<EnvironmentObject*> env(cx);
    if (envObj->is<EnvironmentObject>()) {
      env = &envObj->as<EnvironmentObject>();
    } else if (envObj->is<DebugEnvironmentProxy>()) {
      fprintf(stderr, "[DebugProxy] ");
      env = &envObj->as<DebugEnvironmentProxy>().environment();
    } else {
      MOZ_ASSERT(envObj->is<GlobalObject>());
      fprintf(stderr, "global\n");
      break;
    }

    Rooted<Scope*> scope(cx);
    if (env->is<CallObject>()) {
      fprintf(stderr, "CallObject");
    } else if (env->is<VarEnvironmentObject>()) {
      fprintf(stderr, "VarEnvironmentObject");
      scope = &env->as<VarEnvironmentObject>().scope();
    } else if (env->is<ModuleEnvironmentObject>()) {
      fprintf(stderr, "ModuleEnvironmentObject");
    } else if (env->is<WasmInstanceEnvironmentObject>()) {
      fprintf(stderr, "WasmInstanceEnvironmentObject");
      scope = &env->as<WasmInstanceEnvironmentObject>().scope();
    } else if (env->is<WasmFunctionCallObject>()) {
      fprintf(stderr, "WasmFunctionCallObject");
      scope = &env->as<WasmFunctionCallObject>().scope();
    } else if (env->is<LexicalEnvironmentObject>()) {
      if (env->is<ScopedLexicalEnvironmentObject>()) {
        if (env->is<BlockLexicalEnvironmentObject>()) {
          if (env->is<NamedLambdaObject>()) {
            fprintf(stderr, "NamedLambdaObject");
          } else {
            fprintf(stderr, "BlockLexicalEnvironmentObject");
          }
        } else if (env->is<ClassBodyLexicalEnvironmentObject>()) {
          fprintf(stderr, "ClassBodyLexicalEnvironmentObject");
        } else {
          fprintf(stderr, "ScopedLexicalEnvironmentObject");
        }
        scope = &env->as<ScopedLexicalEnvironmentObject>().scope();
      } else if (env->is<ExtensibleLexicalEnvironmentObject>()) {
        if (env->is<GlobalLexicalEnvironmentObject>()) {
          fprintf(stderr, "GlobalLexicalEnvironmentObject");
        } else if (env->is<NonSyntacticLexicalEnvironmentObject>()) {
          fprintf(stderr, "NonSyntacticLexicalEnvironmentObject");
        } else {
          fprintf(stderr, "ExtensibleLexicalEnvironmentObject");
        }
      } else {
        fprintf(stderr, "LexicalEnvironmentObject");
      }
    } else if (env->is<NonSyntacticVariablesObject>()) {
      fprintf(stderr, "NonSyntacticVariablesObject");
    } else if (env->is<WithEnvironmentObject>()) {
      fprintf(stderr, "WithEnvironmentObject");
    } else if (env->is<RuntimeLexicalErrorObject>()) {
      fprintf(stderr, "RuntimeLexicalErrorObject");
    } else {
      fprintf(stderr, "EnvironmentObject");
    }

    if (scope) {
      fprintf(stderr, " {\n");
      for (Rooted<BindingIter> bi(cx, BindingIter(scope)); bi; bi++) {
        if (bi.location().kind() == BindingLocation::Kind::Environment) {
          UniqueChars bytes = AtomToPrintableString(cx, bi.name());
          if (!bytes) {
            fprintf(stderr, "  *** out of memory\n");
            return;
          }

          fprintf(stderr, "  %u: %s %s\n", bi.location().slot(),
                  BindingKindString(bi.kind()), bytes.get());
        }
      }
      fprintf(stderr, "}");
    }

    fprintf(stderr, "\n");

    if (envObj->is<DebugEnvironmentProxy>()) {
      envObj = &envObj->as<DebugEnvironmentProxy>().enclosingEnvironment();
    } else {
      envObj = &env->enclosingEnvironment();
    }

    if (envObj) {
      fprintf(stderr, "-> ");
    }
  }
}

void EnvironmentObject::dump() { DumpEnvironmentObject(this); }

void DebugEnvironmentProxy::dump() { DumpEnvironmentObject(this); }
#endif /* defined(DEBUG) || defined(JS_JITSPEW) */
