/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/GlobalObject.h"

#include "jsapi.h"
#include "jsfriendapi.h"

#include "builtin/AtomicsObject.h"
#include "builtin/BigInt.h"
#include "builtin/DataViewObject.h"
#ifdef JS_HAS_INTL_API
#  include "builtin/intl/Collator.h"
#  include "builtin/intl/DateTimeFormat.h"
#  include "builtin/intl/DisplayNames.h"
#  include "builtin/intl/ListFormat.h"
#  include "builtin/intl/Locale.h"
#  include "builtin/intl/NumberFormat.h"
#  include "builtin/intl/PluralRules.h"
#  include "builtin/intl/RelativeTimeFormat.h"
#  include "builtin/intl/Segmenter.h"
#endif
#include "builtin/FinalizationRegistryObject.h"
#include "builtin/MapObject.h"
#include "builtin/ShadowRealm.h"
#include "builtin/Symbol.h"
#ifdef JS_HAS_TEMPORAL_API
#  include "builtin/temporal/Calendar.h"
#  include "builtin/temporal/Duration.h"
#  include "builtin/temporal/Instant.h"
#  include "builtin/temporal/PlainDate.h"
#  include "builtin/temporal/PlainDateTime.h"
#  include "builtin/temporal/PlainMonthDay.h"
#  include "builtin/temporal/PlainTime.h"
#  include "builtin/temporal/PlainYearMonth.h"
#  include "builtin/temporal/Temporal.h"
#  include "builtin/temporal/TemporalNow.h"
#  include "builtin/temporal/TimeZone.h"
#  include "builtin/temporal/ZonedDateTime.h"
#endif
#include "builtin/WeakMapObject.h"
#include "builtin/WeakRefObject.h"
#include "builtin/WeakSetObject.h"
#include "debugger/DebugAPI.h"
#include "frontend/CompilationStencil.h"
#include "gc/FinalizationObservers.h"
#include "gc/GC.h"
#include "gc/GCContext.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/WindowProxy.h"    // js::ToWindowProxyIfWindow
#include "js/Prefs.h"                 // JS::Prefs
#include "js/PropertyAndElement.h"    // JS_DefineFunctions, JS_DefineProperties
#include "js/ProtoKey.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/BooleanObject.h"
#include "vm/Compartment.h"
#include "vm/DateObject.h"
#include "vm/EnvironmentObject.h"
#include "vm/ErrorObject.h"
#include "vm/GeneratorObject.h"
#include "vm/JSContext.h"
#include "vm/NumberObject.h"
#include "vm/PIC.h"
#include "vm/PlainObject.h"
#include "vm/RegExpObject.h"
#include "vm/RegExpStatics.h"
#include "vm/SelfHosting.h"
#include "vm/StringObject.h"
#include "wasm/WasmFeatures.h"
#include "wasm/WasmJS.h"
#ifdef ENABLE_RECORD_TUPLE
#  include "vm/RecordType.h"
#  include "vm/TupleType.h"
#endif

#include "gc/GCContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/Realm-inl.h"

using namespace js;

namespace js {

extern const JSClass IntlClass;
extern const JSClass JSONClass;
extern const JSClass MathClass;
extern const JSClass ReflectClass;

}  // namespace js

static const JSClass* const protoTable[JSProto_LIMIT] = {
#define INIT_FUNC(name, clasp) clasp,
#define INIT_FUNC_DUMMY(name, clasp) nullptr,
    JS_FOR_PROTOTYPES(INIT_FUNC, INIT_FUNC_DUMMY)
#undef INIT_FUNC_DUMMY
#undef INIT_FUNC
};

JS_PUBLIC_API const JSClass* js::ProtoKeyToClass(JSProtoKey key) {
  MOZ_ASSERT(key < JSProto_LIMIT);
  return protoTable[key];
}

static bool IsIteratorHelpersEnabled() {
#ifdef NIGHTLY_BUILD
  return JS::Prefs::experimental_iterator_helpers();
#else
  return false;
#endif
}

static bool IsAsyncIteratorHelpersEnabled() {
#ifdef NIGHTLY_BUILD
  return JS::Prefs::experimental_async_iterator_helpers();
#else
  return false;
#endif
}

/* static */
bool GlobalObject::skipDeselectedConstructor(JSContext* cx, JSProtoKey key) {
  switch (key) {
    case JSProto_Null:
    case JSProto_Object:
    case JSProto_Function:
    case JSProto_BoundFunction:
    case JSProto_Array:
    case JSProto_Boolean:
    case JSProto_JSON:
    case JSProto_Date:
    case JSProto_Math:
    case JSProto_Number:
    case JSProto_String:
    case JSProto_RegExp:
    case JSProto_Error:
    case JSProto_InternalError:
    case JSProto_AggregateError:
    case JSProto_EvalError:
    case JSProto_RangeError:
    case JSProto_ReferenceError:
    case JSProto_SyntaxError:
    case JSProto_TypeError:
    case JSProto_URIError:
    case JSProto_DebuggeeWouldRun:
    case JSProto_CompileError:
    case JSProto_LinkError:
    case JSProto_RuntimeError:
    case JSProto_ArrayBuffer:
    case JSProto_Int8Array:
    case JSProto_Uint8Array:
    case JSProto_Int16Array:
    case JSProto_Uint16Array:
    case JSProto_Int32Array:
    case JSProto_Uint32Array:
    case JSProto_Float32Array:
    case JSProto_Float64Array:
    case JSProto_Uint8ClampedArray:
    case JSProto_BigInt64Array:
    case JSProto_BigUint64Array:
    case JSProto_BigInt:
    case JSProto_Proxy:
    case JSProto_WeakMap:
    case JSProto_Map:
    case JSProto_Set:
    case JSProto_DataView:
    case JSProto_Symbol:
    case JSProto_Reflect:
    case JSProto_WeakSet:
    case JSProto_TypedArray:
    case JSProto_SavedFrame:
    case JSProto_Promise:
    case JSProto_AsyncFunction:
    case JSProto_GeneratorFunction:
    case JSProto_AsyncGeneratorFunction:
#ifdef ENABLE_RECORD_TUPLE
    case JSProto_Record:
    case JSProto_Tuple:
#endif
      return false;

    case JSProto_WebAssembly:
      return !wasm::HasSupport(cx);

    case JSProto_WasmModule:
    case JSProto_WasmInstance:
    case JSProto_WasmMemory:
    case JSProto_WasmTable:
    case JSProto_WasmGlobal:
    case JSProto_WasmTag:
#ifdef ENABLE_WASM_TYPE_REFLECTIONS
    case JSProto_WasmFunction:
#endif
#ifdef ENABLE_WASM_JSPI
    case JSProto_WasmSuspending:
#endif
    case JSProto_WasmException:
      return false;

#ifdef JS_HAS_INTL_API
    case JSProto_Intl:
    case JSProto_Collator:
    case JSProto_DateTimeFormat:
    case JSProto_DisplayNames:
    case JSProto_Locale:
    case JSProto_ListFormat:
    case JSProto_NumberFormat:
    case JSProto_PluralRules:
    case JSProto_RelativeTimeFormat:
      return false;

    case JSProto_Segmenter:
#  if defined(MOZ_ICU4X)
      return false;
#  else
      return true;
#  endif
#endif

#ifdef JS_HAS_TEMPORAL_API
    case JSProto_Temporal:
    case JSProto_Calendar:
    case JSProto_Duration:
    case JSProto_Instant:
    case JSProto_PlainDate:
    case JSProto_PlainDateTime:
    case JSProto_PlainMonthDay:
    case JSProto_PlainTime:
    case JSProto_PlainYearMonth:
    case JSProto_TemporalNow:
    case JSProto_TimeZone:
    case JSProto_ZonedDateTime:
      return false;
#endif

    // Return true if the given constructor has been disabled at run-time.
    case JSProto_Atomics:
    case JSProto_SharedArrayBuffer:
      return !cx->realm()->creationOptions().getSharedMemoryAndAtomicsEnabled();

    case JSProto_WeakRef:
    case JSProto_FinalizationRegistry:
      return JS::GetWeakRefsEnabled() == JS::WeakRefSpecifier::Disabled;

    case JSProto_Iterator:
      return !IsIteratorHelpersEnabled();

    case JSProto_AsyncIterator:
      return !IsAsyncIteratorHelpersEnabled();

    case JSProto_ShadowRealm:
      return !JS::Prefs::experimental_shadow_realms();

#ifdef NIGHTLY_BUILD
    case JSProto_Float16Array:
      return !JS::Prefs::experimental_float16array();
#endif

    default:
      MOZ_CRASH("unexpected JSProtoKey");
  }
}

static bool ShouldFreezeBuiltin(JSProtoKey key) {
  // We can't freeze Reflect because JS_InitReflectParse defines Reflect.parse.
  if (key == JSProto_Reflect) {
    return false;
  }
  // We can't freeze Date because some browser tests use the Sinon library which
  // redefines Date.now.
  if (key == JSProto_Date) {
    return false;
  }
  return true;
}

static unsigned GetAttrsForResolvedGlobal(GlobalObject* global,
                                          JSProtoKey key) {
  unsigned attrs = JSPROP_RESOLVING;
  if (global->realm()->creationOptions().freezeBuiltins() &&
      ShouldFreezeBuiltin(key)) {
    attrs |= JSPROP_PERMANENT | JSPROP_READONLY;
  }
  return attrs;
}

/* static*/
bool GlobalObject::resolveConstructor(JSContext* cx,
                                      Handle<GlobalObject*> global,
                                      JSProtoKey key, IfClassIsDisabled mode) {
  MOZ_ASSERT(key != JSProto_Null);
  MOZ_ASSERT(key != JSProto_BoundFunction,
             "bound functions don't have their own proto object");
  MOZ_ASSERT(!global->isStandardClassResolved(key));
  MOZ_ASSERT(cx->compartment() == global->compartment());

  // |global| must be same-compartment but make sure we're in its realm: the
  // code below relies on this.
  AutoRealm ar(cx, global);

  // Prohibit collection of allocation metadata. Metadata builders shouldn't
  // need to observe lazily-constructed prototype objects coming into
  // existence. And assertions start to fail when the builder itself attempts
  // an allocation that re-entrantly tries to create the same prototype.
  AutoSuppressAllocationMetadataBuilder suppressMetadata(cx);

  // Constructor resolution may execute self-hosted scripts. These
  // self-hosted scripts do not call out to user code by construction. Allow
  // all scripts to execute, even in debuggee compartments that are paused.
  AutoSuppressDebuggeeNoExecuteChecks suppressNX(cx);

  // Some classes can be disabled at compile time, others at run time;
  // if a feature is compile-time disabled, clasp is null.
  const JSClass* clasp = ProtoKeyToClass(key);
  if (!clasp || skipDeselectedConstructor(cx, key)) {
    if (mode == IfClassIsDisabled::Throw) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_CONSTRUCTOR_DISABLED,
                                clasp ? clasp->name : "constructor");
      return false;
    }
    return true;
  }

  // Class spec must have a constructor defined.
  if (!clasp->specDefined()) {
    return true;
  }

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
  if (key == JSProto_Function && !global->hasPrototype(JSProto_Object)) {
    return resolveConstructor(cx, global, JSProto_Object,
                              IfClassIsDisabled::DoNothing);
  }

  // %IteratorPrototype%.map.[[Prototype]] is %Generator% and
  // %Generator%.prototype.[[Prototype]] is %IteratorPrototype%.
  // A workaround in initIteratorProto prevents runaway mutual recursion while
  // setting these up. Ensure the workaround is triggered already:
  if (key == JSProto_GeneratorFunction &&
      !global->hasBuiltinProto(ProtoKind::IteratorProto)) {
    if (!getOrCreateIteratorPrototype(cx, global)) {
      return false;
    }

    // If iterator helpers are enabled, populating %IteratorPrototype% will
    // have recursively gone through here.
    if (global->isStandardClassResolved(key)) {
      return true;
    }
  }

  // We don't always have a prototype (i.e. Math and JSON). If we don't,
  // |createPrototype|, |prototypeFunctions|, and |prototypeProperties|
  // should all be null.
  RootedObject proto(cx);
  if (ClassObjectCreationOp createPrototype =
          clasp->specCreatePrototypeHook()) {
    proto = createPrototype(cx, key);
    if (!proto) {
      return false;
    }

    if (isObjectOrFunction) {
      // Make sure that creating the prototype didn't recursively resolve
      // our own constructor. We can't just assert that there's no
      // prototype; OOMs can result in incomplete resolutions in which
      // the prototype is saved but not the constructor. So use the same
      // criteria that protects entry into this function.
      MOZ_ASSERT(!global->isStandardClassResolved(key));

      global->setPrototype(key, proto);
    }
  }

  // Create the constructor.
  RootedObject ctor(cx, clasp->specCreateConstructorHook()(cx, key));
  if (!ctor) {
    return false;
  }

  RootedId id(cx, NameToId(ClassName(key, cx)));
  if (isObjectOrFunction) {
    if (clasp->specShouldDefineConstructor()) {
      RootedValue ctorValue(cx, ObjectValue(*ctor));
      unsigned attrs = GetAttrsForResolvedGlobal(global, key);
      if (!DefineDataProperty(cx, global, id, ctorValue, attrs)) {
        return false;
      }
    }

    global->setConstructor(key, ctor);
  }

  if (const JSFunctionSpec* funs = clasp->specPrototypeFunctions()) {
    if (!JS_DefineFunctions(cx, proto, funs)) {
      return false;
    }
  }
  if (const JSPropertySpec* props = clasp->specPrototypeProperties()) {
    if (!JS_DefineProperties(cx, proto, props)) {
      return false;
    }
  }
  if (const JSFunctionSpec* funs = clasp->specConstructorFunctions()) {
    if (!JS_DefineFunctions(cx, ctor, funs)) {
      return false;
    }
  }
  if (const JSPropertySpec* props = clasp->specConstructorProperties()) {
    if (!JS_DefineProperties(cx, ctor, props)) {
      return false;
    }
  }

  // If the prototype exists, link it with the constructor.
  if (proto && !LinkConstructorAndPrototype(cx, ctor, proto)) {
    return false;
  }

  // Call the post-initialization hook, if provided.
  if (FinishClassInitOp finishInit = clasp->specFinishInitHook()) {
    if (!finishInit(cx, ctor, proto)) {
      return false;
    }
  }

  if (ShouldFreezeBuiltin(key)) {
    if (!JS::MaybeFreezeCtorAndPrototype(cx, ctor, proto)) {
      return false;
    }
  }

  if (!isObjectOrFunction) {
    // Any operations that modifies the global object should be placed
    // after any other fallible operations.

    // Fallible operation that modifies the global object.
    if (clasp->specShouldDefineConstructor()) {
      bool shouldReallyDefine = true;

      // On the web, it isn't presently possible to expose the global
      // "SharedArrayBuffer" property unless the page is cross-site-isolated.
      // Only define this constructor if an option on the realm indicates that
      // it should be defined.
      if (key == JSProto_SharedArrayBuffer) {
        const JS::RealmCreationOptions& options =
            global->realm()->creationOptions();

        MOZ_ASSERT(options.getSharedMemoryAndAtomicsEnabled(),
                   "shouldn't be defining SharedArrayBuffer if shared memory "
                   "is disabled");

        shouldReallyDefine = options.defineSharedArrayBufferConstructor();
      }

      if (shouldReallyDefine) {
        RootedValue ctorValue(cx, ObjectValue(*ctor));
        unsigned attrs = GetAttrsForResolvedGlobal(global, key);
        if (!DefineDataProperty(cx, global, id, ctorValue, attrs)) {
          return false;
        }
      }
    }

    // Infallible operations that modify the global object.
    global->setConstructor(key, ctor);
    if (proto) {
      global->setPrototype(key, proto);
    }
  }

  return true;
}

// Resolve a "globalThis" self-referential property if necessary,
// per a stage-3 proposal. https://github.com/tc39/ecma262/pull/702
//
// We could also do this in |FinishObjectClassInit| to trim the global
// resolve hook.  Unfortunately, |ToWindowProxyIfWindow| doesn't work then:
// the browser's |nsGlobalWindow::SetNewDocument| invokes Object init
// *before* it sets the global's WindowProxy using |js::SetWindowProxy|.
//
// Refactoring global object creation code to support this approach is a
// challenge for another day.
/* static */
bool GlobalObject::maybeResolveGlobalThis(JSContext* cx,
                                          Handle<GlobalObject*> global,
                                          bool* resolved) {
  if (!global->data().globalThisResolved) {
    RootedValue v(cx, ObjectValue(*ToWindowProxyIfWindow(global)));
    if (!DefineDataProperty(cx, global, cx->names().globalThis, v,
                            JSPROP_RESOLVING)) {
      return false;
    }

    *resolved = true;
    global->data().globalThisResolved = true;
  }

  return true;
}

/* static */
JSObject* GlobalObject::createBuiltinProto(JSContext* cx,
                                           Handle<GlobalObject*> global,
                                           ProtoKind kind, ObjectInitOp init) {
  if (!init(cx, global)) {
    return nullptr;
  }

  return &global->getBuiltinProto(kind);
}

JSObject* GlobalObject::createBuiltinProto(JSContext* cx,
                                           Handle<GlobalObject*> global,
                                           ProtoKind kind, Handle<JSAtom*> tag,
                                           ObjectInitWithTagOp init) {
  if (!init(cx, global, tag)) {
    return nullptr;
  }

  return &global->getBuiltinProto(kind);
}

static bool ThrowTypeError(JSContext* cx, unsigned argc, Value* vp) {
  ThrowTypeErrorBehavior(cx);
  return false;
}

/* static */
JSObject* GlobalObject::getOrCreateThrowTypeError(
    JSContext* cx, Handle<GlobalObject*> global) {
  if (JSFunction* fun = global->data().throwTypeError) {
    return fun;
  }

  // Construct the unique [[%ThrowTypeError%]] function object, used only for
  // "callee" and "caller" accessors on strict mode arguments objects.  (The
  // spec also uses this for "arguments" and "caller" on various functions,
  // but we're experimenting with implementing them using accessors on
  // |Function.prototype| right now.)

  RootedFunction throwTypeError(
      cx, NewNativeFunction(cx, ThrowTypeError, 0, nullptr));
  if (!throwTypeError || !PreventExtensions(cx, throwTypeError)) {
    return nullptr;
  }

  // The "length" property of %ThrowTypeError% is non-configurable.
  Rooted<PropertyDescriptor> nonConfigurableDesc(cx,
                                                 PropertyDescriptor::Empty());
  nonConfigurableDesc.setConfigurable(false);

  RootedId lengthId(cx, NameToId(cx->names().length));
  ObjectOpResult lengthResult;
  if (!NativeDefineProperty(cx, throwTypeError, lengthId, nonConfigurableDesc,
                            lengthResult)) {
    return nullptr;
  }
  MOZ_ASSERT(lengthResult);

  // The "name" property of %ThrowTypeError% is non-configurable, adjust
  // the default property attributes accordingly.
  RootedId nameId(cx, NameToId(cx->names().name));
  ObjectOpResult nameResult;
  if (!NativeDefineProperty(cx, throwTypeError, nameId, nonConfigurableDesc,
                            nameResult)) {
    return nullptr;
  }
  MOZ_ASSERT(nameResult);

  global->data().throwTypeError.init(throwTypeError);
  return throwTypeError;
}

GlobalObject* GlobalObject::createInternal(JSContext* cx,
                                           const JSClass* clasp) {
  MOZ_ASSERT(clasp->flags & JSCLASS_IS_GLOBAL);
  MOZ_ASSERT(clasp->isTrace(JS_GlobalObjectTraceHook));

  JSObject* obj = NewTenuredObjectWithGivenProto(cx, clasp, nullptr);
  if (!obj) {
    return nullptr;
  }

  Rooted<GlobalObject*> global(cx, &obj->as<GlobalObject>());
  MOZ_ASSERT(global->isUnqualifiedVarObj());

  {
    auto data = cx->make_unique<GlobalObjectData>(cx->zone());
    if (!data) {
      return nullptr;
    }
    // Note: it's important for the realm's global to be initialized at the
    // same time as the global's GlobalObjectData, because we free the global's
    // data when Realm::global_ is cleared.
    cx->realm()->initGlobal(*global);
    InitReservedSlot(global, GLOBAL_DATA_SLOT, data.release(),
                     MemoryUse::GlobalObjectData);
  }

  Rooted<GlobalLexicalEnvironmentObject*> lexical(
      cx, GlobalLexicalEnvironmentObject::create(cx, global));
  if (!lexical) {
    return nullptr;
  }
  global->data().lexicalEnvironment.init(lexical);

  Rooted<GlobalScope*> emptyGlobalScope(
      cx, GlobalScope::createEmpty(cx, ScopeKind::Global));
  if (!emptyGlobalScope) {
    return nullptr;
  }
  global->data().emptyGlobalScope.init(emptyGlobalScope);

  if (!GlobalObject::createIntrinsicsHolder(cx, global)) {
    return nullptr;
  }

  if (!JSObject::setQualifiedVarObj(cx, global)) {
    return nullptr;
  }
  if (!JSObject::setGenerationCountedGlobal(cx, global)) {
    return nullptr;
  }

  return global;
}

/* static */
GlobalObject* GlobalObject::new_(JSContext* cx, const JSClass* clasp,
                                 JSPrincipals* principals,
                                 JS::OnNewGlobalHookOption hookOption,
                                 const JS::RealmOptions& options) {
  MOZ_ASSERT(!cx->isExceptionPending());
  MOZ_ASSERT_IF(cx->zone(), !cx->zone()->isAtomsZone());

  // If we are creating a new global in an existing compartment, make sure the
  // compartment has a live global at all times (by rooting it here).
  // See bug 1530364.
  Rooted<GlobalObject*> existingGlobal(cx);
  const JS::RealmCreationOptions& creationOptions = options.creationOptions();
  if (creationOptions.compartmentSpecifier() ==
      JS::CompartmentSpecifier::ExistingCompartment) {
    Compartment* comp = creationOptions.compartment();
    existingGlobal = &comp->firstGlobal();
  }

  Realm* realm = NewRealm(cx, principals, options);
  if (!realm) {
    return nullptr;
  }

  Rooted<GlobalObject*> global(cx);
  {
    AutoRealmUnchecked ar(cx, realm);
    global = GlobalObject::createInternal(cx, clasp);
    if (!global) {
      return nullptr;
    }

    // Make transactional initialization of these constructors by discarding the
    // incompletely initialized global if an error occur. This also ensures the
    // global's prototype chain is initialized (in FinishObjectClassInit).
    if (!ensureConstructor(cx, global, JSProto_Object) ||
        !ensureConstructor(cx, global, JSProto_Function)) {
      return nullptr;
    }

    // Create a shape for plain objects with zero slots. This is required to be
    // present in case allocating dynamic slots for objects fails, so we can
    // leave a valid object in the heap.
    if (!createPlainObjectShapeWithDefaultProto(cx, gc::AllocKind::OBJECT0)) {
      return nullptr;
    }

    realm->clearInitializingGlobal();
    if (hookOption == JS::FireOnNewGlobalHook) {
      JS_FireOnNewGlobalObject(cx, global);
    }
  }

  return global;
}

GlobalScope& GlobalObject::emptyGlobalScope() const {
  return *data().emptyGlobalScope;
}

bool GlobalObject::valueIsEval(const Value& val) {
  return val.isObject() && data().eval == &val.toObject();
}

/* static */
bool GlobalObject::initStandardClasses(JSContext* cx,
                                       Handle<GlobalObject*> global) {
  /* Define a top-level property 'undefined' with the undefined value. */
  if (!DefineDataProperty(
          cx, global, cx->names().undefined, UndefinedHandleValue,
          JSPROP_PERMANENT | JSPROP_READONLY | JSPROP_RESOLVING)) {
    return false;
  }

  // Resolve a "globalThis" self-referential property if necessary.
  bool resolved;
  if (!GlobalObject::maybeResolveGlobalThis(cx, global, &resolved)) {
    return false;
  }

  for (size_t k = 0; k < JSProto_LIMIT; ++k) {
    JSProtoKey key = static_cast<JSProtoKey>(k);
    if (key != JSProto_Null && key != JSProto_BoundFunction &&
        !global->isStandardClassResolved(key)) {
      if (!resolveConstructor(cx, global, static_cast<JSProtoKey>(k),
                              IfClassIsDisabled::DoNothing)) {
        return false;
      }
    }
  }
  return true;
}

/* static */
JSFunction* GlobalObject::createConstructor(JSContext* cx, Native ctor,
                                            JSAtom* nameArg, unsigned length,
                                            gc::AllocKind kind,
                                            const JSJitInfo* jitInfo) {
  Rooted<JSAtom*> name(cx, nameArg);
  JSFunction* fun = NewNativeConstructor(cx, ctor, length, name, kind);
  if (!fun) {
    return nullptr;
  }

  if (jitInfo) {
    fun->setJitInfo(jitInfo);
  }

  return fun;
}

static NativeObject* CreateBlankProto(JSContext* cx, const JSClass* clasp,
                                      HandleObject proto,
                                      ObjectFlags objFlags) {
  MOZ_ASSERT(!clasp->isJSFunction());

  if (clasp == &PlainObject::class_) {
    // NOTE: There should be no reason currently to support this. It could
    // however be added later if needed.
    MOZ_ASSERT(objFlags.isEmpty());
    return NewPlainObjectWithProto(cx, proto, TenuredObject);
  }

  return NewTenuredObjectWithGivenProto(cx, clasp, proto, objFlags);
}

/* static */
NativeObject* GlobalObject::createBlankPrototype(JSContext* cx,
                                                 Handle<GlobalObject*> global,
                                                 const JSClass* clasp,
                                                 ObjectFlags objFlags) {
  RootedObject objectProto(cx, &global->getObjectPrototype());
  return CreateBlankProto(cx, clasp, objectProto, objFlags);
}

/* static */
NativeObject* GlobalObject::createBlankPrototypeInheriting(JSContext* cx,
                                                           const JSClass* clasp,
                                                           HandleObject proto) {
  return CreateBlankProto(cx, clasp, proto, ObjectFlags());
}

bool js::LinkConstructorAndPrototype(JSContext* cx, JSObject* ctor_,
                                     JSObject* proto_, unsigned prototypeAttrs,
                                     unsigned constructorAttrs) {
  RootedObject ctor(cx, ctor_), proto(cx, proto_);

  RootedValue protoVal(cx, ObjectValue(*proto));
  RootedValue ctorVal(cx, ObjectValue(*ctor));

  return DefineDataProperty(cx, ctor, cx->names().prototype, protoVal,
                            prototypeAttrs) &&
         DefineDataProperty(cx, proto, cx->names().constructor, ctorVal,
                            constructorAttrs);
}

bool js::DefinePropertiesAndFunctions(JSContext* cx, HandleObject obj,
                                      const JSPropertySpec* ps,
                                      const JSFunctionSpec* fs) {
  if (ps && !JS_DefineProperties(cx, obj, ps)) {
    return false;
  }
  if (fs && !JS_DefineFunctions(cx, obj, fs)) {
    return false;
  }
  return true;
}

bool js::DefineToStringTag(JSContext* cx, HandleObject obj, JSAtom* tag) {
  RootedId toStringTagId(
      cx, PropertyKey::Symbol(cx->wellKnownSymbols().toStringTag));
  RootedValue tagString(cx, StringValue(tag));
  return DefineDataProperty(cx, obj, toStringTagId, tagString, JSPROP_READONLY);
}

/* static */
NativeObject* GlobalObject::getOrCreateForOfPICObject(
    JSContext* cx, Handle<GlobalObject*> global) {
  cx->check(global);
  NativeObject* forOfPIC = global->getForOfPICObject();
  if (forOfPIC) {
    return forOfPIC;
  }

  forOfPIC = ForOfPIC::createForOfPICObject(cx, global);
  if (!forOfPIC) {
    return nullptr;
  }
  global->data().forOfPICChain.init(forOfPIC);
  return forOfPIC;
}

/* static */
JSObject* GlobalObject::getOrCreateRealmKeyObject(
    JSContext* cx, Handle<GlobalObject*> global) {
  cx->check(global);
  if (PlainObject* key = global->data().realmKeyObject) {
    return key;
  }

  PlainObject* key = NewPlainObject(cx);
  if (!key) {
    return nullptr;
  }

  global->data().realmKeyObject.init(key);
  return key;
}

/* static */
RegExpStatics* GlobalObject::getRegExpStatics(JSContext* cx,
                                              Handle<GlobalObject*> global) {
  MOZ_ASSERT(cx);

  if (!global->regExpRealm().regExpStatics) {
    auto statics = RegExpStatics::create(cx);
    if (!statics) {
      return nullptr;
    }
    global->regExpRealm().regExpStatics = std::move(statics);
  }

  return global->regExpRealm().regExpStatics.get();
}

gc::FinalizationRegistryGlobalData*
GlobalObject::getOrCreateFinalizationRegistryData() {
  if (!data().finalizationRegistryData) {
    data().finalizationRegistryData =
        MakeUnique<gc::FinalizationRegistryGlobalData>(zone());
  }

  return maybeFinalizationRegistryData();
}

bool GlobalObject::addToVarNames(JSContext* cx, JS::Handle<JSAtom*> name) {
  MOZ_ASSERT(name);

  if (!data().varNames.put(name)) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

/* static */
bool GlobalObject::createIntrinsicsHolder(JSContext* cx,
                                          Handle<GlobalObject*> global) {
  Rooted<NativeObject*> intrinsicsHolder(
      cx, NewPlainObjectWithProto(cx, nullptr, TenuredObject));
  if (!intrinsicsHolder) {
    return false;
  }

  // Install the intrinsics holder on the global.
  global->data().intrinsicsHolder.init(intrinsicsHolder);
  return true;
}

/* static */
bool GlobalObject::getSelfHostedFunction(JSContext* cx,
                                         Handle<GlobalObject*> global,
                                         Handle<PropertyName*> selfHostedName,
                                         Handle<JSAtom*> name, unsigned nargs,
                                         MutableHandleValue funVal) {
  if (global->maybeGetIntrinsicValue(selfHostedName, funVal.address(), cx)) {
    RootedFunction fun(cx, &funVal.toObject().as<JSFunction>());
    if (fun->fullExplicitName() == name) {
      return true;
    }

    if (fun->fullExplicitName() == selfHostedName) {
      // This function was initially cloned because it was called by
      // other self-hosted code, so the clone kept its self-hosted name,
      // instead of getting the name it's intended to have in content
      // compartments. This can happen when a lazy builtin is initialized
      // after self-hosted code for another builtin used the same
      // function. In that case, we need to change the function's name,
      // which is ok because it can't have been exposed to content
      // before.
      fun->setAtom(name);
      return true;
    }

    // The function might be installed multiple times on the same or
    // different builtins, under different property names, so its name
    // might be neither "selfHostedName" nor "name". In that case, its
    // canonical name must've been set using the `_SetCanonicalName`
    // intrinsic.
    cx->runtime()->assertSelfHostedFunctionHasCanonicalName(selfHostedName);
    return true;
  }

  // Don't collect metadata for self-hosted functions or intrinsics.
  // This is similar to the suppression in GlobalObject::resolveConstructor.
  AutoSuppressAllocationMetadataBuilder suppressMetadata(cx);

  JSRuntime* runtime = cx->runtime();
  frontend::ScriptIndex index =
      runtime->getSelfHostedScriptIndexRange(selfHostedName)->start;
  JSFunction* fun =
      runtime->selfHostStencil().instantiateSelfHostedLazyFunction(
          cx, runtime->selfHostStencilInput().atomCache, index, name);
  if (!fun) {
    return false;
  }
  MOZ_ASSERT(fun->nargs() == nargs);
  funVal.setObject(*fun);

  return GlobalObject::addIntrinsicValue(cx, global, selfHostedName, funVal);
}

/* static */
bool GlobalObject::getIntrinsicValueSlow(JSContext* cx,
                                         Handle<GlobalObject*> global,
                                         Handle<PropertyName*> name,
                                         MutableHandleValue value) {
  // Don't collect metadata for self-hosted functions or intrinsics.
  // This is similar to the suppression in GlobalObject::resolveConstructor.
  AutoSuppressAllocationMetadataBuilder suppressMetadata(cx);

  // If this is a C++ intrinsic, simply define the function on the intrinsics
  // holder.
  if (const JSFunctionSpec* spec = js::FindIntrinsicSpec(name)) {
    RootedId id(cx, NameToId(name));
    RootedFunction fun(cx, JS::NewFunctionFromSpec(cx, spec, id));
    if (!fun) {
      return false;
    }
    fun->setIsIntrinsic();

    value.setObject(*fun);
    return GlobalObject::addIntrinsicValue(cx, global, name, value);
  }

  if (!cx->runtime()->getSelfHostedValue(cx, name, value)) {
    return false;
  }

  // It's possible in certain edge cases that cloning the value ended up
  // defining the intrinsic. For instance, cloning can call NewArray, which
  // resolves Array.prototype, which defines some self-hosted functions. If this
  // happens we use the value already defined on the intrinsics holder.
  if (global->maybeGetIntrinsicValue(name, value.address(), cx)) {
    return true;
  }

  return GlobalObject::addIntrinsicValue(cx, global, name, value);
}

/* static */
bool GlobalObject::addIntrinsicValue(JSContext* cx,
                                     Handle<GlobalObject*> global,
                                     Handle<PropertyName*> name,
                                     HandleValue value) {
  Rooted<NativeObject*> holder(cx, &global->getIntrinsicsHolder());

  RootedId id(cx, NameToId(name));
  MOZ_ASSERT(!holder->containsPure(id));

  constexpr PropertyFlags propFlags = {PropertyFlag::Configurable,
                                       PropertyFlag::Writable};
  uint32_t slot;
  if (!NativeObject::addProperty(cx, holder, id, propFlags, &slot)) {
    return false;
  }
  holder->initSlot(slot, value);
  return true;
}

/* static */
JSObject* GlobalObject::createIteratorPrototype(JSContext* cx,
                                                Handle<GlobalObject*> global) {
  if (!IsIteratorHelpersEnabled()) {
    return getOrCreateBuiltinProto(cx, global, ProtoKind::IteratorProto,
                                   initIteratorProto);
  }

  if (!ensureConstructor(cx, global, JSProto_Iterator)) {
    return nullptr;
  }
  JSObject* proto = &global->getPrototype(JSProto_Iterator);
  global->initBuiltinProto(ProtoKind::IteratorProto, proto);
  return proto;
}

/* static */
JSObject* GlobalObject::createAsyncIteratorPrototype(
    JSContext* cx, Handle<GlobalObject*> global) {
  if (!IsAsyncIteratorHelpersEnabled()) {
    return getOrCreateBuiltinProto(cx, global, ProtoKind::AsyncIteratorProto,
                                   initAsyncIteratorProto);
  }

  if (!ensureConstructor(cx, global, JSProto_AsyncIterator)) {
    return nullptr;
  }
  JSObject* proto = &global->getPrototype(JSProto_AsyncIterator);
  global->initBuiltinProto(ProtoKind::AsyncIteratorProto, proto);
  return proto;
}

void GlobalObject::releaseData(JS::GCContext* gcx) {
  GlobalObjectData* data = maybeData();
  setReservedSlot(GLOBAL_DATA_SLOT, PrivateValue(nullptr));
  gcx->delete_(this, data, MemoryUse::GlobalObjectData);
}

GlobalObjectData::GlobalObjectData(Zone* zone) : varNames(zone) {}

GlobalObjectData::~GlobalObjectData() = default;

void GlobalObjectData::trace(JSTracer* trc, GlobalObject* global) {
  // Atoms are always tenured so don't need to be traced during minor GC.
  if (trc->runtime()->heapState() != JS::HeapState::MinorCollecting) {
    varNames.trace(trc);
  }

  for (auto& ctorWithProto : builtinConstructors) {
    TraceNullableEdge(trc, &ctorWithProto.constructor, "global-builtin-ctor");
    TraceNullableEdge(trc, &ctorWithProto.prototype,
                      "global-builtin-ctor-proto");
  }

  for (auto& proto : builtinProtos) {
    TraceNullableEdge(trc, &proto, "global-builtin-proto");
  }

  TraceNullableEdge(trc, &emptyGlobalScope, "global-empty-scope");

  TraceNullableEdge(trc, &lexicalEnvironment, "global-lexical-env");
  TraceNullableEdge(trc, &windowProxy, "global-window-proxy");
  TraceNullableEdge(trc, &intrinsicsHolder, "global-intrinsics-holder");
  TraceNullableEdge(trc, &computedIntrinsicsHolder,
                    "global-computed-intrinsics-holder");
  TraceNullableEdge(trc, &forOfPICChain, "global-for-of-pic");
  TraceNullableEdge(trc, &sourceURLsHolder, "global-source-urls");
  TraceNullableEdge(trc, &realmKeyObject, "global-realm-key");
  TraceNullableEdge(trc, &throwTypeError, "global-throw-type-error");
  TraceNullableEdge(trc, &eval, "global-eval");
  TraceNullableEdge(trc, &emptyIterator, "global-empty-iterator");

  TraceNullableEdge(trc, &arrayShapeWithDefaultProto, "global-array-shape");

  for (auto& shape : plainObjectShapesWithDefaultProto) {
    TraceNullableEdge(trc, &shape, "global-plain-shape");
  }

  TraceNullableEdge(trc, &functionShapeWithDefaultProto,
                    "global-function-shape");
  TraceNullableEdge(trc, &extendedFunctionShapeWithDefaultProto,
                    "global-ext-function-shape");

  TraceNullableEdge(trc, &boundFunctionShapeWithDefaultProto,
                    "global-bound-function-shape");

  regExpRealm.trace(trc);

  TraceNullableEdge(trc, &mappedArgumentsTemplate, "mapped-arguments-template");
  TraceNullableEdge(trc, &unmappedArgumentsTemplate,
                    "unmapped-arguments-template");

  TraceNullableEdge(trc, &iterResultTemplate, "iter-result-template_");
  TraceNullableEdge(trc, &iterResultWithoutPrototypeTemplate,
                    "iter-result-without-prototype-template");

  TraceNullableEdge(trc, &selfHostingScriptSource,
                    "self-hosting-script-source");

  if (finalizationRegistryData) {
    finalizationRegistryData->trace(trc);
  }
}

void GlobalObjectData::addSizeOfIncludingThis(
    mozilla::MallocSizeOf mallocSizeOf, JS::ClassInfo* info) const {
  info->objectsMallocHeapGlobalData += mallocSizeOf(this);

  if (regExpRealm.regExpStatics) {
    info->objectsMallocHeapGlobalData +=
        regExpRealm.regExpStatics->sizeOfIncludingThis(mallocSizeOf);
  }

  info->objectsMallocHeapGlobalVarNamesSet +=
      varNames.shallowSizeOfExcludingThis(mallocSizeOf);
}
