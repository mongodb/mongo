/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/GlobalObject.h"

#include "jsdate.h"
#include "jsexn.h"
#include "jsfriendapi.h"

#include "builtin/AtomicsObject.h"
#include "builtin/BigInt.h"
#include "builtin/DataViewObject.h"
#include "builtin/Eval.h"
#ifdef JS_HAS_INTL_API
#  include "builtin/intl/Collator.h"
#  include "builtin/intl/DateTimeFormat.h"
#  include "builtin/intl/DisplayNames.h"
#  include "builtin/intl/ListFormat.h"
#  include "builtin/intl/Locale.h"
#  include "builtin/intl/NumberFormat.h"
#  include "builtin/intl/PluralRules.h"
#  include "builtin/intl/RelativeTimeFormat.h"
#endif
#include "builtin/FinalizationRegistryObject.h"
#include "builtin/MapObject.h"
#include "builtin/ModuleObject.h"
#include "builtin/Object.h"
#include "builtin/RegExp.h"
#include "builtin/SelfHostingDefines.h"
#include "builtin/Stream.h"
#include "builtin/streams/QueueingStrategies.h"  // js::{ByteLength,Count}QueueingStrategy
#include "builtin/streams/ReadableStream.h"  // js::ReadableStream
#include "builtin/streams/ReadableStreamController.h"  // js::Readable{StreamDefault,ByteStream}Controller
#include "builtin/streams/ReadableStreamReader.h"  // js::ReadableStreamDefaultReader
#include "builtin/streams/WritableStream.h"        // js::WritableStream
#include "builtin/streams/WritableStreamDefaultController.h"  // js::WritableStreamDefaultController
#include "builtin/streams/WritableStreamDefaultWriter.h"  // js::WritableStreamDefaultWriter
#include "builtin/Symbol.h"
#include "builtin/WeakMapObject.h"
#include "builtin/WeakRefObject.h"
#include "builtin/WeakSetObject.h"
#include "debugger/DebugAPI.h"
#include "gc/FreeOp.h"
#include "js/friend/ErrorMessages.h"        // js::GetErrorMessage, JSMSG_*
#include "js/friend/WindowProxy.h"          // js::ToWindowProxyIfWindow
#include "js/OffThreadScriptCompilation.h"  // js::UseOffThreadParseGlobal
#include "js/ProtoKey.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/BooleanObject.h"
#include "vm/DateObject.h"
#include "vm/EnvironmentObject.h"
#include "vm/ErrorObject.h"
#include "vm/GeneratorObject.h"
#include "vm/HelperThreads.h"
#include "vm/JSContext.h"
#include "vm/NumberObject.h"
#include "vm/PIC.h"
#include "vm/RegExpStatics.h"
#include "vm/RegExpStaticsObject.h"
#include "vm/StringObject.h"

#include "gc/FreeOp-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"
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

/* static */
bool GlobalObject::skipDeselectedConstructor(JSContext* cx, JSProtoKey key) {
  switch (key) {
    case JSProto_Null:
    case JSProto_Object:
    case JSProto_Function:
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
      return false;

    case JSProto_WebAssembly:
      return !wasm::HasSupport(cx);

    case JSProto_WasmModule:
    case JSProto_WasmInstance:
    case JSProto_WasmMemory:
    case JSProto_WasmTable:
    case JSProto_WasmGlobal:
    case JSProto_WasmException:
    case JSProto_WasmRuntimeException:
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
#endif

    case JSProto_ReadableStream:
    case JSProto_ReadableStreamDefaultReader:
    case JSProto_ReadableStreamDefaultController:
    case JSProto_ReadableByteStreamController:
    case JSProto_ByteLengthQueuingStrategy:
    case JSProto_CountQueuingStrategy:
      return !cx->realm()->creationOptions().getStreamsEnabled();

    case JSProto_WritableStream:
    case JSProto_WritableStreamDefaultController:
    case JSProto_WritableStreamDefaultWriter: {
      const auto& realmOptions = cx->realm()->creationOptions();
      return !realmOptions.getStreamsEnabled() ||
             !realmOptions.getWritableStreamsEnabled();
    }

    // Return true if the given constructor has been disabled at run-time.
    case JSProto_Atomics:
    case JSProto_SharedArrayBuffer:
      return !cx->realm()->creationOptions().getSharedMemoryAndAtomicsEnabled();

    case JSProto_WeakRef:
    case JSProto_FinalizationRegistry:
      return cx->realm()->creationOptions().getWeakRefsEnabled() ==
             JS::WeakRefSpecifier::Disabled;

    case JSProto_Iterator:
    case JSProto_AsyncIterator:
      return !cx->realm()->creationOptions().getIteratorHelpersEnabled();

    default:
      MOZ_CRASH("unexpected JSProtoKey");
  }
}

/* static*/
bool GlobalObject::resolveConstructor(JSContext* cx,
                                      Handle<GlobalObject*> global,
                                      JSProtoKey key, IfClassIsDisabled mode) {
  MOZ_ASSERT(key != JSProto_Null);
  MOZ_ASSERT(!global->isStandardClassResolved(key));
  MOZ_ASSERT(cx->compartment() == global->compartment());

  // |global| must be same-compartment but make sure we're in its realm: the
  // code below relies on this.
  AutoRealm ar(cx, global);

  if (global->zone()->createdForHelperThread()) {
    return resolveOffThreadConstructor(cx, global, key);
  }

  MOZ_ASSERT(!cx->isHelperThreadContext());

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
  if (key == JSProto_Function &&
      global->getPrototype(JSProto_Object).isUndefined()) {
    return resolveConstructor(cx, global, JSProto_Object,
                              IfClassIsDisabled::DoNothing);
  }

  // %IteratorPrototype%.map.[[Prototype]] is %Generator% and
  // %Generator%.prototype.[[Prototype]] is %IteratorPrototype%.
  // A workaround in initIteratorProto prevents runaway mutual recursion while
  // setting these up. Ensure the workaround is triggered already:
  if (key == JSProto_GeneratorFunction &&
      !global->getSlotRef(ITERATOR_PROTO).isObject()) {
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

      global->setPrototype(key, ObjectValue(*proto));
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
      if (!DefineDataProperty(cx, global, id, ctorValue, JSPROP_RESOLVING)) {
        return false;
      }
    }

    global->setConstructor(key, ObjectValue(*ctor));
  }

  // If we're operating on the self-hosting global, we don't want any
  // functions and properties on the builtins and their prototypes.
  if (!cx->runtime()->isSelfHostingGlobal(global)) {
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
        if (!DefineDataProperty(cx, global, id, ctorValue, JSPROP_RESOLVING)) {
          return false;
        }
      }
    }

    // Infallible operations that modify the global object.
    global->setConstructor(key, ObjectValue(*ctor));
    if (proto) {
      global->setPrototype(key, ObjectValue(*proto));
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
  if (global->getSlot(GLOBAL_THIS_RESOLVED).isUndefined()) {
    RootedValue v(cx, ObjectValue(*ToWindowProxyIfWindow(global)));
    if (!DefineDataProperty(cx, global, cx->names().globalThis, v,
                            JSPROP_RESOLVING)) {
      return false;
    }

    *resolved = true;
    global->setSlot(GLOBAL_THIS_RESOLVED, BooleanValue(true));
  }

  return true;
}

/* static */
JSObject* GlobalObject::createObject(JSContext* cx,
                                     Handle<GlobalObject*> global,
                                     unsigned slot, ObjectInitOp init) {
  if (global->zone()->createdForHelperThread()) {
    return createOffThreadObject(cx, global, slot);
  }

  MOZ_ASSERT(!cx->isHelperThreadContext());
  if (!init(cx, global)) {
    return nullptr;
  }

  return &global->getSlot(slot).toObject();
}

JSObject* GlobalObject::createObject(JSContext* cx,
                                     Handle<GlobalObject*> global,
                                     unsigned slot, HandleAtom tag,
                                     ObjectInitWithTagOp init) {
  if (global->zone()->createdForHelperThread()) {
    return createOffThreadObject(cx, global, slot);
  }

  MOZ_ASSERT(!cx->isHelperThreadContext());
  if (!init(cx, global, tag)) {
    return nullptr;
  }

  return &global->getSlot(slot).toObject();
}

const JSClass GlobalObject::OffThreadPlaceholderObject::class_ = {
    "off-thread-prototype-placeholder", JSCLASS_HAS_RESERVED_SLOTS(1)};

/* static */ GlobalObject::OffThreadPlaceholderObject*
GlobalObject::OffThreadPlaceholderObject::New(JSContext* cx, unsigned slot) {
  Rooted<OffThreadPlaceholderObject*> placeholder(cx);
  placeholder =
      NewObjectWithGivenProto<OffThreadPlaceholderObject>(cx, nullptr);
  if (!placeholder) {
    return nullptr;
  }

  placeholder->setReservedSlot(SlotIndexSlot, Int32Value(slot));
  return placeholder;
}

inline int32_t GlobalObject::OffThreadPlaceholderObject::getSlotIndex() const {
  return getReservedSlot(SlotIndexSlot).toInt32();
}

/* static */
bool GlobalObject::resolveOffThreadConstructor(JSContext* cx,
                                               Handle<GlobalObject*> global,
                                               JSProtoKey key) {
  // Don't resolve constructors for off-thread parse globals. Instead create a
  // placeholder object for the prototype which we can use to find the real
  // prototype when the off-thread compartment is merged back into the target
  // compartment.

  MOZ_ASSERT(global->zone()->createdForHelperThread());
  MOZ_ASSERT(key == JSProto_Object || key == JSProto_Function ||
             key == JSProto_Array || key == JSProto_RegExp ||
             key == JSProto_AsyncFunction || key == JSProto_GeneratorFunction ||
             key == JSProto_AsyncGeneratorFunction);

  Rooted<OffThreadPlaceholderObject*> placeholder(cx);
  placeholder = OffThreadPlaceholderObject::New(cx, prototypeSlot(key));
  if (!placeholder) {
    return false;
  }

  if (key == JSProto_Object &&
      !JSObject::setFlag(cx, placeholder, ObjectFlag::ImmutablePrototype)) {
    return false;
  }

  global->setPrototype(key, ObjectValue(*placeholder));
  global->setConstructor(key, MagicValue(JS_OFF_THREAD_CONSTRUCTOR));
  return true;
}

/* static */
JSObject* GlobalObject::createOffThreadObject(JSContext* cx,
                                              Handle<GlobalObject*> global,
                                              unsigned slot) {
  // Don't create prototype objects for off-thread parse globals. Instead
  // create a placeholder object which we can use to find the real prototype
  // when the off-thread compartment is merged back into the target
  // compartment.

  MOZ_ASSERT(global->zone()->createdForHelperThread());
  MOZ_ASSERT(slot == MODULE_PROTO || slot == IMPORT_ENTRY_PROTO ||
             slot == EXPORT_ENTRY_PROTO || slot == REQUESTED_MODULE_PROTO);

  auto placeholder = OffThreadPlaceholderObject::New(cx, slot);
  if (!placeholder) {
    return nullptr;
  }

  global->setSlot(slot, ObjectValue(*placeholder));
  return placeholder;
}

JSObject* GlobalObject::getPrototypeForOffThreadPlaceholder(JSObject* obj) {
  auto placeholder = &obj->as<OffThreadPlaceholderObject>();
  return &getSlot(placeholder->getSlotIndex()).toObject();
}

/* static */
bool GlobalObject::initBuiltinConstructor(JSContext* cx,
                                          Handle<GlobalObject*> global,
                                          JSProtoKey key, HandleObject ctor,
                                          HandleObject proto) {
  MOZ_ASSERT(!global->empty());  // reserved slots already allocated
  MOZ_ASSERT(key != JSProto_Null);
  MOZ_ASSERT(ctor);
  MOZ_ASSERT(proto);

  RootedId id(cx, NameToId(ClassName(key, cx)));
  MOZ_ASSERT(!global->lookup(cx, id));

  RootedValue ctorValue(cx, ObjectValue(*ctor));
  if (!DefineDataProperty(cx, global, id, ctorValue, JSPROP_RESOLVING)) {
    return false;
  }

  global->setConstructor(key, ObjectValue(*ctor));
  global->setPrototype(key, ObjectValue(*proto));
  return true;
}

static bool ThrowTypeError(JSContext* cx, unsigned argc, Value* vp) {
  ThrowTypeErrorBehavior(cx);
  return false;
}

/* static */
JSObject* GlobalObject::getOrCreateThrowTypeError(
    JSContext* cx, Handle<GlobalObject*> global) {
  Value v = global->getReservedSlot(THROWTYPEERROR);
  if (v.isObject()) {
    return &v.toObject();
  }
  MOZ_ASSERT(v.isUndefined());

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

  global->setReservedSlot(THROWTYPEERROR, ObjectValue(*throwTypeError));
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

  // Initialize the private slot to null if present, as GC can call class
  // hooks before the caller gets to set this to a non-garbage value.
  if (clasp->flags & JSCLASS_HAS_PRIVATE) {
    global->setPrivate(nullptr);
  }

  Rooted<GlobalLexicalEnvironmentObject*> lexical(
      cx, GlobalLexicalEnvironmentObject::create(cx, global));
  if (!lexical) {
    return nullptr;
  }

  Rooted<GlobalScope*> emptyGlobalScope(
      cx, GlobalScope::createEmpty(cx, ScopeKind::Global));
  if (!emptyGlobalScope) {
    return nullptr;
  }
  global->setReservedSlot(EMPTY_GLOBAL_SCOPE,
                          PrivateGCThingValue(emptyGlobalScope));

  cx->realm()->initGlobal(*global, *lexical);

  if (!JSObject::setQualifiedVarObj(cx, global)) {
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

    if (hookOption == JS::FireOnNewGlobalHook) {
      JS_FireOnNewGlobalObject(cx, global);
    }
  }

  return global;
}

GlobalLexicalEnvironmentObject& GlobalObject::lexicalEnvironment() const {
  // The lexical environment is marked when marking the global, so we don't need
  // a read barrier here because we know the global is live.
  return *realm()->unbarrieredLexicalEnvironment();
}

GlobalScope& GlobalObject::emptyGlobalScope() const {
  const Value& v = getReservedSlot(EMPTY_GLOBAL_SCOPE);
  MOZ_ASSERT(v.isPrivateGCThing() && v.traceKind() == JS::TraceKind::Scope);
  return static_cast<Scope*>(v.toGCThing())->as<GlobalScope>();
}

/* static */
bool GlobalObject::getOrCreateEval(JSContext* cx, Handle<GlobalObject*> global,
                                   MutableHandleObject eval) {
  if (!getOrCreateObjectPrototype(cx, global)) {
    return false;
  }
  eval.set(&global->getSlot(EVAL).toObject());
  return true;
}

bool GlobalObject::valueIsEval(const Value& val) {
  Value eval = getSlot(EVAL);
  return eval.isObject() && eval == val;
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
    if (key != JSProto_Null && !global->isStandardClassResolved(key)) {
      if (!resolveConstructor(cx, global, static_cast<JSProtoKey>(k),
                              IfClassIsDisabled::DoNothing)) {
        return false;
      }
    }
  }
  return true;
}

/* static */
bool GlobalObject::initSelfHostingBuiltins(JSContext* cx,
                                           Handle<GlobalObject*> global,
                                           const JSFunctionSpec* builtins) {
  return DefineFunctions(cx, global, builtins, AsIntrinsic);
}

/* static */
bool GlobalObject::isRuntimeCodeGenEnabled(JSContext* cx, HandleString code,
                                           Handle<GlobalObject*> global) {
  HeapSlot& v = global->getSlotRef(RUNTIME_CODEGEN_ENABLED);
  if (v.isUndefined()) {
    /*
     * If there are callbacks, make sure that the CSP callback is installed
     * and that it permits runtime code generation.
     */
    JSCSPEvalChecker allows =
        cx->runtime()->securityCallbacks->contentSecurityPolicyAllows;
    if (allows) {
      return allows(cx, code);
    }

    // Let's cache the result only if the contentSecurityPolicyAllows callback
    // is not set. In this way, contentSecurityPolicyAllows callback is executed
    // each time, with the current HandleValue code.
    v.set(global, HeapSlot::Slot, RUNTIME_CODEGEN_ENABLED, JS::TrueValue());
  }
  return !v.isFalse();
}

/* static */
JSFunction* GlobalObject::createConstructor(JSContext* cx, Native ctor,
                                            JSAtom* nameArg, unsigned length,
                                            gc::AllocKind kind,
                                            const JSJitInfo* jitInfo) {
  RootedAtom name(cx, nameArg);
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
                                      HandleObject proto) {
  MOZ_ASSERT(clasp != &JSFunction::class_);

  RootedObject blankProto(cx, NewTenuredObjectWithGivenProto(cx, clasp, proto));
  if (!blankProto) {
    return nullptr;
  }

  return &blankProto->as<NativeObject>();
}

/* static */
NativeObject* GlobalObject::createBlankPrototype(JSContext* cx,
                                                 Handle<GlobalObject*> global,
                                                 const JSClass* clasp) {
  RootedObject objectProto(cx, getOrCreateObjectPrototype(cx, global));
  if (!objectProto) {
    return nullptr;
  }

  return CreateBlankProto(cx, clasp, objectProto);
}

/* static */
NativeObject* GlobalObject::createBlankPrototypeInheriting(JSContext* cx,
                                                           const JSClass* clasp,
                                                           HandleObject proto) {
  return CreateBlankProto(cx, clasp, proto);
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
  RootedId toStringTagId(cx,
                         SYMBOL_TO_JSID(cx->wellKnownSymbols().toStringTag));
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
  global->setReservedSlot(FOR_OF_PIC_CHAIN, ObjectValue(*forOfPIC));
  return forOfPIC;
}

/* static */
JSObject* GlobalObject::getOrCreateRealmKeyObject(
    JSContext* cx, Handle<GlobalObject*> global) {
  cx->check(global);
  Value v = global->getReservedSlot(REALM_KEY_OBJECT);
  if (v.isObject()) {
    return &v.toObject();
  }

  PlainObject* key = NewBuiltinClassInstance<PlainObject>(cx);
  if (!key) {
    return nullptr;
  }

  global->setReservedSlot(REALM_KEY_OBJECT, ObjectValue(*key));
  return key;
}

/* static */
RegExpStatics* GlobalObject::getRegExpStatics(JSContext* cx,
                                              Handle<GlobalObject*> global) {
  MOZ_ASSERT(cx);
  RegExpStaticsObject* resObj = nullptr;
  const Value& val = global->getSlot(REGEXP_STATICS);
  if (!val.isObject()) {
    MOZ_ASSERT(val.isUndefined());
    resObj = RegExpStatics::create(cx);
    if (!resObj) {
      return nullptr;
    }

    global->initSlot(REGEXP_STATICS, ObjectValue(*resObj));
  } else {
    resObj = &val.toObject().as<RegExpStaticsObject>();
  }
  return static_cast<RegExpStatics*>(resObj->getPrivate(/* nfixed = */ 1));
}

/* static */
NativeObject* GlobalObject::getIntrinsicsHolder(JSContext* cx,
                                                Handle<GlobalObject*> global) {
  Value slot = global->getReservedSlot(INTRINSICS);
  MOZ_ASSERT(slot.isUndefined() || slot.isObject());

  if (slot.isObject()) {
    return &slot.toObject().as<NativeObject>();
  }

  Rooted<NativeObject*> intrinsicsHolder(cx);
  bool isSelfHostingGlobal = cx->runtime()->isSelfHostingGlobal(global);
  if (isSelfHostingGlobal) {
    intrinsicsHolder = global;
  } else {
    intrinsicsHolder = NewTenuredObjectWithGivenProto<PlainObject>(cx, nullptr);
    if (!intrinsicsHolder) {
      return nullptr;
    }
  }

  // Define a top-level property 'undefined' with the undefined value.
  if (!DefineDataProperty(cx, intrinsicsHolder, cx->names().undefined,
                          UndefinedHandleValue,
                          JSPROP_PERMANENT | JSPROP_READONLY)) {
    return nullptr;
  }

  // Install the intrinsics holder in the intrinsics.
  global->setReservedSlot(INTRINSICS, ObjectValue(*intrinsicsHolder));
  return intrinsicsHolder;
}

/* static */
bool GlobalObject::getSelfHostedFunction(JSContext* cx,
                                         Handle<GlobalObject*> global,
                                         HandlePropertyName selfHostedName,
                                         HandleAtom name, unsigned nargs,
                                         MutableHandleValue funVal) {
  bool exists = false;
  if (!GlobalObject::maybeGetIntrinsicValue(cx, global, selfHostedName, funVal,
                                            &exists)) {
    return false;
  }
  if (exists) {
    RootedFunction fun(cx, &funVal.toObject().as<JSFunction>());
    if (fun->explicitName() == name) {
      return true;
    }

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
  if (!cx->runtime()->createLazySelfHostedFunctionClone(
          cx, selfHostedName, name, nargs, TenuredObject, &fun)) {
    return false;
  }
  funVal.setObject(*fun);

  return GlobalObject::addIntrinsicValue(cx, global, selfHostedName, funVal);
}

/* static */
bool GlobalObject::getIntrinsicValueSlow(JSContext* cx,
                                         Handle<GlobalObject*> global,
                                         HandlePropertyName name,
                                         MutableHandleValue value) {
  if (!cx->runtime()->cloneSelfHostedValue(cx, name, value)) {
    return false;
  }

  // It's possible in certain edge cases that cloning the value ended up
  // defining the intrinsic. For instance, cloning can call NewArray, which
  // resolves Array.prototype, which defines some self-hosted functions. If this
  // happens we use the value already defined on the intrinsics holder.
  bool exists = false;
  if (!GlobalObject::maybeGetIntrinsicValue(cx, global, name, value, &exists)) {
    return false;
  }
  if (exists) {
    return true;
  }

  return GlobalObject::addIntrinsicValue(cx, global, name, value);
}

/* static */
bool GlobalObject::addIntrinsicValue(JSContext* cx,
                                     Handle<GlobalObject*> global,
                                     HandlePropertyName name,
                                     HandleValue value) {
  RootedNativeObject holder(cx, GlobalObject::getIntrinsicsHolder(cx, global));
  if (!holder) {
    return false;
  }

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
bool GlobalObject::ensureModulePrototypesCreated(JSContext* cx,
                                                 Handle<GlobalObject*> global,
                                                 bool setUsedAsPrototype) {
  // Note: if you arrived here because you're removing UseOffThreadParseGlobal,
  // please also remove the setUsedAsPrototype argument and the lambda below.
  MOZ_ASSERT_IF(!UseOffThreadParseGlobal(), !setUsedAsPrototype);

  auto maybeSetUsedAsPrototype = [cx, setUsedAsPrototype](HandleObject proto) {
    if (!setUsedAsPrototype) {
      return true;
    }
    return JSObject::setIsUsedAsPrototype(cx, proto);
  };

  RootedObject proto(cx);
  proto = getOrCreateModulePrototype(cx, global);
  if (!proto || !maybeSetUsedAsPrototype(proto)) {
    return false;
  }

  proto = getOrCreateImportEntryPrototype(cx, global);
  if (!proto || !maybeSetUsedAsPrototype(proto)) {
    return false;
  }

  proto = getOrCreateExportEntryPrototype(cx, global);
  if (!proto || !maybeSetUsedAsPrototype(proto)) {
    return false;
  }

  proto = getOrCreateRequestedModulePrototype(cx, global);
  if (!proto || !maybeSetUsedAsPrototype(proto)) {
    return false;
  }

  return true;
}

/* static */
JSObject* GlobalObject::createIteratorPrototype(JSContext* cx,
                                                Handle<GlobalObject*> global) {
  if (!cx->realm()->creationOptions().getIteratorHelpersEnabled()) {
    return getOrCreateObject(cx, global, ITERATOR_PROTO, initIteratorProto);
  }

  if (!ensureConstructor(cx, global, JSProto_Iterator)) {
    return nullptr;
  }
  JSObject* proto = &global->getPrototype(JSProto_Iterator).toObject();
  global->setReservedSlot(ITERATOR_PROTO, ObjectValue(*proto));
  return proto;
}

/* static */
JSObject* GlobalObject::createAsyncIteratorPrototype(
    JSContext* cx, Handle<GlobalObject*> global) {
  if (!cx->realm()->creationOptions().getIteratorHelpersEnabled()) {
    return getOrCreateObject(cx, global, ASYNC_ITERATOR_PROTO,
                             initAsyncIteratorProto);
  }

  if (!ensureConstructor(cx, global, JSProto_AsyncIterator)) {
    return nullptr;
  }
  JSObject* proto = &global->getPrototype(JSProto_AsyncIterator).toObject();
  global->setReservedSlot(ASYNC_ITERATOR_PROTO, ObjectValue(*proto));
  return proto;
}
