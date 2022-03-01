/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JavaScript API. */

#ifndef jsapi_h
#define jsapi_h

#include "mozilla/AlreadyAddRefed.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/RangedPtr.h"
#include "mozilla/RefPtr.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/Utf8.h"
#include "mozilla/Variant.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "jspubtd.h"

#include "js/AllocPolicy.h"
#include "js/CallArgs.h"
#include "js/CharacterEncoding.h"
#include "js/Class.h"
#include "js/CompileOptions.h"
#include "js/Context.h"
#include "js/ErrorReport.h"
#include "js/Exception.h"
#include "js/GCAPI.h"
#include "js/GCVector.h"
#include "js/HashTable.h"
#include "js/Id.h"
#include "js/MapAndSet.h"
#include "js/MemoryFunctions.h"
#include "js/OffThreadScriptCompilation.h"
#include "js/Principals.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/Realm.h"
#include "js/RealmIterators.h"
#include "js/RealmOptions.h"
#include "js/RefCounted.h"
#include "js/RootingAPI.h"
#include "js/String.h"
#include "js/TracingAPI.h"
#include "js/Transcoding.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "js/Value.h"
#include "js/ValueArray.h"
#include "js/Vector.h"
#include "js/WeakMap.h"
#include "js/Zone.h"

/************************************************************************/

namespace JS {
/**
 * Tell JS engine whether to use fdlibm for Math.sin, Math.cos, and Math.tan.
 * Using fdlibm ensures that we don't expose a math fingerprint.
 */
extern JS_PUBLIC_API void SetUseFdlibmForSinCosTan(bool value);
}  // namespace JS

/************************************************************************/

struct JSFunctionSpec;
struct JSPropertySpec;

namespace JS {

template <typename UnitT>
class SourceText;

class TwoByteChars;

using ValueVector = JS::GCVector<JS::Value>;
using IdVector = JS::GCVector<jsid>;
using ScriptVector = JS::GCVector<JSScript*>;
using StringVector = JS::GCVector<JSString*>;

} /* namespace JS */

/* Callbacks and their arguments. */

/************************************************************************/

// Callback for the embedding to map from a ScriptSourceObject private-value to
// an object that is exposed as the source "element" in debugger API.  This hook
// must be infallible, but can return nullptr if no such element exists.
using JSSourceElementCallback = JSObject* (*)(JSContext*, JS::HandleValue);

using JSInterruptCallback = bool (*)(JSContext*);

/**
 * Callback used to ask the embedding for the cross compartment wrapper handler
 * that implements the desired prolicy for this kind of object in the
 * destination compartment. |obj| is the object to be wrapped. If |existing| is
 * non-nullptr, it will point to an existing wrapper object that should be
 * re-used if possible. |existing| is guaranteed to be a cross-compartment
 * wrapper with a lazily-defined prototype and the correct global. It is
 * guaranteed not to wrap a function.
 */
using JSWrapObjectCallback = JSObject* (*)(JSContext*, JS::HandleObject,
                                           JS::HandleObject);

/**
 * Callback used by the wrap hook to ask the embedding to prepare an object
 * for wrapping in a context. This might include unwrapping other wrappers
 * or even finding a more suitable object for the new compartment. If |origObj|
 * is non-null, then it is the original object we are going to swap into during
 * a transplant.
 */
using JSPreWrapCallback = void (*)(JSContext*, JS::HandleObject,
                                   JS::HandleObject, JS::HandleObject,
                                   JS::HandleObject, JS::MutableHandleObject);

struct JSWrapObjectCallbacks {
  JSWrapObjectCallback wrap;
  JSPreWrapCallback preWrap;
};

/**
 * Callback used to intercept JavaScript errors.
 */
struct JSErrorInterceptor {
  /**
   * This method is called whenever an error has been raised from JS code.
   *
   * This method MUST be infallible.
   */
  virtual void interceptError(JSContext* cx, JS::HandleValue error) = 0;
};

/************************************************************************/

static MOZ_ALWAYS_INLINE JS::Value JS_NumberValue(double d) {
  int32_t i;
  d = JS::CanonicalizeNaN(d);
  if (mozilla::NumberIsInt32(d, &i)) {
    return JS::Int32Value(i);
  }
  return JS::DoubleValue(d);
}

/************************************************************************/

JS_PUBLIC_API bool JS_StringHasBeenPinned(JSContext* cx, JSString* str);

/************************************************************************/

/** Microseconds since the epoch, midnight, January 1, 1970 UTC. */
extern JS_PUBLIC_API int64_t JS_Now(void);

extern JS_PUBLIC_API bool JS_ValueToObject(JSContext* cx, JS::HandleValue v,
                                           JS::MutableHandleObject objp);

extern JS_PUBLIC_API JSFunction* JS_ValueToFunction(JSContext* cx,
                                                    JS::HandleValue v);

extern JS_PUBLIC_API JSFunction* JS_ValueToConstructor(JSContext* cx,
                                                       JS::HandleValue v);

extern JS_PUBLIC_API JSString* JS_ValueToSource(JSContext* cx,
                                                JS::Handle<JS::Value> v);

extern JS_PUBLIC_API bool JS_DoubleIsInt32(double d, int32_t* ip);

extern JS_PUBLIC_API JSType JS_TypeOfValue(JSContext* cx,
                                           JS::Handle<JS::Value> v);

namespace JS {

extern JS_PUBLIC_API const char* InformalValueTypeName(const JS::Value& v);

/** Timing information for telemetry purposes **/
struct JSTimers {
  mozilla::TimeDuration executionTime;       // Total time spent executing
  mozilla::TimeDuration delazificationTime;  // Total time spent delazifying
  mozilla::TimeDuration xdrEncodingTime;     // Total time spent XDR encoding
  mozilla::TimeDuration gcTime;              // Total time spent in GC
  mozilla::TimeDuration
      protectTime;  // Total time spent protecting JIT executable memory
  mozilla::TimeDuration
      baselineCompileTime;  // Total time spent in baseline compiler
};

extern JS_PUBLIC_API JSTimers GetJSTimers(JSContext* cx);

} /* namespace JS */

/** True iff fun is the global eval function. */
extern JS_PUBLIC_API bool JS_IsBuiltinEvalFunction(JSFunction* fun);

/** True iff fun is the Function constructor. */
extern JS_PUBLIC_API bool JS_IsBuiltinFunctionConstructor(JSFunction* fun);

extern JS_PUBLIC_API const char* JS_GetImplementationVersion(void);

extern JS_PUBLIC_API void JS_SetWrapObjectCallbacks(
    JSContext* cx, const JSWrapObjectCallbacks* callbacks);

// Set a callback that will be called whenever an error
// is thrown in this runtime. This is designed as a mechanism
// for logging errors. Note that the VM makes no attempt to sanitize
// the contents of the error (so it may contain private data)
// or to sort out among errors (so it may not be the error you
// are interested in or for the component in which you are
// interested).
//
// If the callback sets a new error, this new error
// will replace the original error.
//
// May be `nullptr`.
// This is a no-op if built without NIGHTLY_BUILD.
extern JS_PUBLIC_API void JS_SetErrorInterceptorCallback(
    JSRuntime*, JSErrorInterceptor* callback);

// This returns nullptr if built without NIGHTLY_BUILD.
extern JS_PUBLIC_API JSErrorInterceptor* JS_GetErrorInterceptorCallback(
    JSRuntime*);

// Examine a value to determine if it is one of the built-in Error types.
// If so, return the error type.
extern JS_PUBLIC_API mozilla::Maybe<JSExnType> JS_GetErrorType(
    const JS::Value& val);

extern JS_PUBLIC_API bool JS_WrapObject(JSContext* cx,
                                        JS::MutableHandleObject objp);

extern JS_PUBLIC_API bool JS_WrapValue(JSContext* cx,
                                       JS::MutableHandleValue vp);

extern JS_PUBLIC_API JSObject* JS_TransplantObject(JSContext* cx,
                                                   JS::HandleObject origobj,
                                                   JS::HandleObject target);

/**
 * Resolve id, which must contain either a string or an int, to a standard
 * class name in obj if possible, defining the class's constructor and/or
 * prototype and storing true in *resolved.  If id does not name a standard
 * class or a top-level property induced by initializing a standard class,
 * store false in *resolved and just return true.  Return false on error,
 * as usual for bool result-typed API entry points.
 *
 * This API can be called directly from a global object class's resolve op,
 * to define standard classes lazily. The class should either have an enumerate
 * hook that calls JS_EnumerateStandardClasses, or a newEnumerate hook that
 * calls JS_NewEnumerateStandardClasses. newEnumerate is preferred because it's
 * faster (does not define all standard classes).
 */
extern JS_PUBLIC_API bool JS_ResolveStandardClass(JSContext* cx,
                                                  JS::HandleObject obj,
                                                  JS::HandleId id,
                                                  bool* resolved);

extern JS_PUBLIC_API bool JS_MayResolveStandardClass(const JSAtomState& names,
                                                     jsid id,
                                                     JSObject* maybeObj);

extern JS_PUBLIC_API bool JS_EnumerateStandardClasses(JSContext* cx,
                                                      JS::HandleObject obj);

/**
 * Fill "properties" with a list of standard class names that have not yet been
 * resolved on "obj".  This can be used as (part of) a newEnumerate class hook
 * on a global.  Already-resolved things are excluded because they might have
 * been deleted by script after being resolved and enumeration considers
 * already-defined properties anyway.
 */
extern JS_PUBLIC_API bool JS_NewEnumerateStandardClasses(
    JSContext* cx, JS::HandleObject obj, JS::MutableHandleIdVector properties,
    bool enumerableOnly);

/**
 * Fill "properties" with a list of standard class names.  This can be used for
 * proxies that want to define behavior that looks like enumerating a global
 * without touching the global itself.
 */
extern JS_PUBLIC_API bool JS_NewEnumerateStandardClassesIncludingResolved(
    JSContext* cx, JS::HandleObject obj, JS::MutableHandleIdVector properties,
    bool enumerableOnly);

extern JS_PUBLIC_API bool JS_GetClassObject(JSContext* cx, JSProtoKey key,
                                            JS::MutableHandle<JSObject*> objp);

extern JS_PUBLIC_API bool JS_GetClassPrototype(
    JSContext* cx, JSProtoKey key, JS::MutableHandle<JSObject*> objp);

namespace JS {

/*
 * Determine if the given object is an instance/prototype/constructor for a
 * standard class. If so, return the associated JSProtoKey. If not, return
 * JSProto_Null.
 */

extern JS_PUBLIC_API JSProtoKey IdentifyStandardInstance(JSObject* obj);

extern JS_PUBLIC_API JSProtoKey IdentifyStandardPrototype(JSObject* obj);

extern JS_PUBLIC_API JSProtoKey
IdentifyStandardInstanceOrPrototype(JSObject* obj);

extern JS_PUBLIC_API JSProtoKey IdentifyStandardConstructor(JSObject* obj);

extern JS_PUBLIC_API void ProtoKeyToId(JSContext* cx, JSProtoKey key,
                                       JS::MutableHandleId idp);

} /* namespace JS */

extern JS_PUBLIC_API JSProtoKey JS_IdToProtoKey(JSContext* cx, JS::HandleId id);

extern JS_PUBLIC_API bool JS_IsGlobalObject(JSObject* obj);

extern JS_PUBLIC_API JSObject* JS_GlobalLexicalEnvironment(JSObject* obj);

extern JS_PUBLIC_API bool JS_HasExtensibleLexicalEnvironment(JSObject* obj);

extern JS_PUBLIC_API JSObject* JS_ExtensibleLexicalEnvironment(JSObject* obj);

namespace JS {

/**
 * Get the current realm's global. Returns nullptr if no realm has been
 * entered.
 */
extern JS_PUBLIC_API JSObject* CurrentGlobalOrNull(JSContext* cx);

/**
 * Get the global object associated with an object's realm. The object must not
 * be a cross-compartment wrapper (because CCWs are shared by all realms in the
 * compartment).
 */
extern JS_PUBLIC_API JSObject* GetNonCCWObjectGlobal(JSObject* obj);

}  // namespace JS

/**
 * Add 'Reflect.parse', a SpiderMonkey extension, to the Reflect object on the
 * given global.
 */
extern JS_PUBLIC_API bool JS_InitReflectParse(JSContext* cx,
                                              JS::HandleObject global);

/**
 * Add various profiling-related functions as properties of the given object.
 * Defined in builtin/Profilers.cpp.
 */
extern JS_PUBLIC_API bool JS_DefineProfilingFunctions(JSContext* cx,
                                                      JS::HandleObject obj);

/* Defined in vm/Debugger.cpp. */
extern JS_PUBLIC_API bool JS_DefineDebuggerObject(JSContext* cx,
                                                  JS::HandleObject obj);

namespace JS {

/**
 * Tell JS engine whether Profile Timeline Recording is enabled or not.
 * If Profile Timeline Recording is enabled, data shown there like stack won't
 * be optimized out.
 * This is global state and not associated with specific runtime or context.
 */
extern JS_PUBLIC_API void SetProfileTimelineRecordingEnabled(bool enabled);

extern JS_PUBLIC_API bool IsProfileTimelineRecordingEnabled();

}  // namespace JS

/**
 * Set the size of the native stack that should not be exceed. To disable
 * stack size checking pass 0.
 *
 * SpiderMonkey allows for a distinction between system code (such as GCs, which
 * may incidentally be triggered by script but are not strictly performed on
 * behalf of such script), trusted script (as determined by
 * JS_SetTrustedPrincipals), and untrusted script. Each kind of code may have a
 * different stack quota, allowing embedders to keep higher-priority machinery
 * running in the face of scripted stack exhaustion by something else.
 *
 * The stack quotas for each kind of code should be monotonically descending,
 * and may be specified with this function. If 0 is passed for a given kind
 * of code, it defaults to the value of the next-highest-priority kind.
 *
 * This function may only be called immediately after the runtime is initialized
 * and before any code is executed and/or interrupts requested.
 */
extern JS_PUBLIC_API void JS_SetNativeStackQuota(
    JSContext* cx, size_t systemCodeStackSize,
    size_t trustedScriptStackSize = 0, size_t untrustedScriptStackSize = 0);

/************************************************************************/

extern JS_PUBLIC_API bool JS_ValueToId(JSContext* cx, JS::HandleValue v,
                                       JS::MutableHandleId idp);

extern JS_PUBLIC_API bool JS_StringToId(JSContext* cx, JS::HandleString s,
                                        JS::MutableHandleId idp);

extern JS_PUBLIC_API bool JS_IdToValue(JSContext* cx, jsid id,
                                       JS::MutableHandle<JS::Value> vp);

namespace JS {

/**
 * Convert obj to a primitive value. On success, store the result in vp and
 * return true.
 *
 * The hint argument must be JSTYPE_STRING, JSTYPE_NUMBER, or
 * JSTYPE_UNDEFINED (no hint).
 *
 * Implements: ES6 7.1.1 ToPrimitive(input, [PreferredType]).
 */
extern JS_PUBLIC_API bool ToPrimitive(JSContext* cx, JS::HandleObject obj,
                                      JSType hint, JS::MutableHandleValue vp);

/**
 * If args.get(0) is one of the strings "string", "number", or "default", set
 * result to JSTYPE_STRING, JSTYPE_NUMBER, or JSTYPE_UNDEFINED accordingly and
 * return true. Otherwise, return false with a TypeError pending.
 *
 * This can be useful in implementing a @@toPrimitive method.
 */
extern JS_PUBLIC_API bool GetFirstArgumentAsTypeHint(JSContext* cx,
                                                     CallArgs args,
                                                     JSType* result);

} /* namespace JS */

extern JS_PUBLIC_API JSObject* JS_InitClass(
    JSContext* cx, JS::HandleObject obj, JS::HandleObject parent_proto,
    const JSClass* clasp, JSNative constructor, unsigned nargs,
    const JSPropertySpec* ps, const JSFunctionSpec* fs,
    const JSPropertySpec* static_ps, const JSFunctionSpec* static_fs);

/**
 * Set up ctor.prototype = proto and proto.constructor = ctor with the
 * right property flags.
 */
extern JS_PUBLIC_API bool JS_LinkConstructorAndPrototype(
    JSContext* cx, JS::Handle<JSObject*> ctor, JS::Handle<JSObject*> proto);

extern JS_PUBLIC_API bool JS_InstanceOf(JSContext* cx,
                                        JS::Handle<JSObject*> obj,
                                        const JSClass* clasp,
                                        JS::CallArgs* args);

extern JS_PUBLIC_API bool JS_HasInstance(JSContext* cx,
                                         JS::Handle<JSObject*> obj,
                                         JS::Handle<JS::Value> v, bool* bp);

namespace JS {

// Implementation of
// http://www.ecma-international.org/ecma-262/6.0/#sec-ordinaryhasinstance.  If
// you're looking for the equivalent of "instanceof", you want JS_HasInstance,
// not this function.
extern JS_PUBLIC_API bool OrdinaryHasInstance(JSContext* cx,
                                              HandleObject objArg,
                                              HandleValue v, bool* bp);

// Implementation of
// https://www.ecma-international.org/ecma-262/6.0/#sec-instanceofoperator
// This is almost identical to JS_HasInstance, except the latter may call a
// custom hasInstance class op instead of InstanceofOperator.
extern JS_PUBLIC_API bool InstanceofOperator(JSContext* cx, HandleObject obj,
                                             HandleValue v, bool* bp);

}  // namespace JS

extern JS_PUBLIC_API void JS_InitPrivate(JSObject* obj, void* data,
                                         size_t nbytes, JS::MemoryUse use);

extern JS_PUBLIC_API void* JS_GetInstancePrivate(JSContext* cx,
                                                 JS::Handle<JSObject*> obj,
                                                 const JSClass* clasp,
                                                 JS::CallArgs* args);

extern JS_PUBLIC_API JSObject* JS_GetConstructor(JSContext* cx,
                                                 JS::Handle<JSObject*> proto);

namespace JS {

/**
 * During global creation, we fire notifications to callbacks registered
 * via the Debugger API. These callbacks are arbitrary script, and can touch
 * the global in arbitrary ways. When that happens, the global should not be
 * in a half-baked state. But this creates a problem for consumers that need
 * to set slots on the global to put it in a consistent state.
 *
 * This API provides a way for consumers to set slots atomically (immediately
 * after the global is created), before any debugger hooks are fired. It's
 * unfortunately on the clunky side, but that's the way the cookie crumbles.
 *
 * If callers have no additional state on the global to set up, they may pass
 * |FireOnNewGlobalHook| to JS_NewGlobalObject, which causes that function to
 * fire the hook as its final act before returning. Otherwise, callers should
 * pass |DontFireOnNewGlobalHook|, which means that they are responsible for
 * invoking JS_FireOnNewGlobalObject upon successfully creating the global. If
 * an error occurs and the operation aborts, callers should skip firing the
 * hook. But otherwise, callers must take care to fire the hook exactly once
 * before compiling any script in the global's scope (we have assertions in
 * place to enforce this). This lets us be sure that debugger clients never miss
 * breakpoints.
 */
enum OnNewGlobalHookOption { FireOnNewGlobalHook, DontFireOnNewGlobalHook };

} /* namespace JS */

extern JS_PUBLIC_API JSObject* JS_NewGlobalObject(
    JSContext* cx, const JSClass* clasp, JSPrincipals* principals,
    JS::OnNewGlobalHookOption hookOption, const JS::RealmOptions& options);
/**
 * Spidermonkey does not have a good way of keeping track of what compartments
 * should be marked on their own. We can mark the roots unconditionally, but
 * marking GC things only relevant in live compartments is hard. To mitigate
 * this, we create a static trace hook, installed on each global object, from
 * which we can be sure the compartment is relevant, and mark it.
 *
 * It is still possible to specify custom trace hooks for global object classes.
 * They can be provided via the RealmOptions passed to JS_NewGlobalObject.
 */
extern JS_PUBLIC_API void JS_GlobalObjectTraceHook(JSTracer* trc,
                                                   JSObject* global);

namespace JS {

/**
 * This allows easily constructing a global object without having to deal with
 * JSClassOps, forgetting to add JS_GlobalObjectTraceHook, or forgetting to call
 * JS::InitRealmStandardClasses(). Example:
 *
 *     const JSClass globalClass = { "MyGlobal", JSCLASS_GLOBAL_FLAGS,
 *         &JS::DefaultGlobalClassOps };
 *     JS_NewGlobalObject(cx, &globalClass, ...);
 */
extern JS_PUBLIC_DATA const JSClassOps DefaultGlobalClassOps;

}  // namespace JS

extern JS_PUBLIC_API void JS_FireOnNewGlobalObject(JSContext* cx,
                                                   JS::HandleObject global);

extern JS_PUBLIC_API JSObject* JS_NewObject(JSContext* cx,
                                            const JSClass* clasp);

extern JS_PUBLIC_API bool JS_IsNative(JSObject* obj);

/**
 * Unlike JS_NewObject, JS_NewObjectWithGivenProto does not compute a default
 * proto. If proto is nullptr, the JS object will have `null` as [[Prototype]].
 */
extern JS_PUBLIC_API JSObject* JS_NewObjectWithGivenProto(
    JSContext* cx, const JSClass* clasp, JS::Handle<JSObject*> proto);

/**
 * Creates a new plain object, like `new Object()`, with Object.prototype as
 * [[Prototype]].
 */
extern JS_PUBLIC_API JSObject* JS_NewPlainObject(JSContext* cx);

/**
 * Freeze obj, and all objects it refers to, recursively. This will not recurse
 * through non-extensible objects, on the assumption that those are already
 * deep-frozen.
 */
extern JS_PUBLIC_API bool JS_DeepFreezeObject(JSContext* cx,
                                              JS::Handle<JSObject*> obj);

/**
 * Freezes an object; see ES5's Object.freeze(obj) method.
 */
extern JS_PUBLIC_API bool JS_FreezeObject(JSContext* cx,
                                          JS::Handle<JSObject*> obj);

/*** Standard internal methods **********************************************
 *
 * The functions below are the fundamental operations on objects.
 *
 * ES6 specifies 14 internal methods that define how objects behave.  The
 * standard is actually quite good on this topic, though you may have to read
 * it a few times. See ES6 sections 6.1.7.2 and 6.1.7.3.
 *
 * When 'obj' is an ordinary object, these functions have boring standard
 * behavior as specified by ES6 section 9.1; see the section about internal
 * methods in js/src/vm/NativeObject.h.
 *
 * Proxies override the behavior of internal methods. So when 'obj' is a proxy,
 * any one of the functions below could do just about anything. See
 * js/public/Proxy.h.
 */

/**
 * Get the prototype of |obj|, storing it in |proto|.
 *
 * Implements: ES6 [[GetPrototypeOf]] internal method.
 */
extern JS_PUBLIC_API bool JS_GetPrototype(JSContext* cx, JS::HandleObject obj,
                                          JS::MutableHandleObject result);

/**
 * If |obj| (underneath any functionally-transparent wrapper proxies) has as
 * its [[GetPrototypeOf]] trap the ordinary [[GetPrototypeOf]] behavior defined
 * for ordinary objects, set |*isOrdinary = true| and store |obj|'s prototype
 * in |result|.  Otherwise set |*isOrdinary = false|.  In case of error, both
 * outparams have unspecified value.
 */
extern JS_PUBLIC_API bool JS_GetPrototypeIfOrdinary(
    JSContext* cx, JS::HandleObject obj, bool* isOrdinary,
    JS::MutableHandleObject result);

/**
 * Change the prototype of obj.
 *
 * Implements: ES6 [[SetPrototypeOf]] internal method.
 *
 * In cases where ES6 [[SetPrototypeOf]] returns false without an exception,
 * JS_SetPrototype throws a TypeError and returns false.
 *
 * Performance warning: JS_SetPrototype is very bad for performance. It may
 * cause compiled jit-code to be invalidated. It also causes not only obj but
 * all other objects in the same "group" as obj to be permanently deoptimized.
 * It's better to create the object with the right prototype from the start.
 */
extern JS_PUBLIC_API bool JS_SetPrototype(JSContext* cx, JS::HandleObject obj,
                                          JS::HandleObject proto);

/**
 * Determine whether obj is extensible. Extensible objects can have new
 * properties defined on them. Inextensible objects can't, and their
 * [[Prototype]] slot is fixed as well.
 *
 * Implements: ES6 [[IsExtensible]] internal method.
 */
extern JS_PUBLIC_API bool JS_IsExtensible(JSContext* cx, JS::HandleObject obj,
                                          bool* extensible);

/**
 * Attempt to make |obj| non-extensible.
 *
 * Not all failures are treated as errors. See the comment on
 * JS::ObjectOpResult in js/public/Class.h.
 *
 * Implements: ES6 [[PreventExtensions]] internal method.
 */
extern JS_PUBLIC_API bool JS_PreventExtensions(JSContext* cx,
                                               JS::HandleObject obj,
                                               JS::ObjectOpResult& result);

/**
 * Attempt to make the [[Prototype]] of |obj| immutable, such that any attempt
 * to modify it will fail.  If an error occurs during the attempt, return false
 * (with a pending exception set, depending upon the nature of the error).  If
 * no error occurs, return true with |*succeeded| set to indicate whether the
 * attempt successfully made the [[Prototype]] immutable.
 *
 * This is a nonstandard internal method.
 */
extern JS_PUBLIC_API bool JS_SetImmutablePrototype(JSContext* cx,
                                                   JS::HandleObject obj,
                                                   bool* succeeded);

/**
 * Get a description of one of obj's own properties. If no such property exists
 * on obj, return true with desc.object() set to null.
 *
 * Implements: ES6 [[GetOwnProperty]] internal method.
 */
extern JS_PUBLIC_API bool JS_GetOwnPropertyDescriptorById(
    JSContext* cx, JS::HandleObject obj, JS::HandleId id,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc);

extern JS_PUBLIC_API bool JS_GetOwnPropertyDescriptor(
    JSContext* cx, JS::HandleObject obj, const char* name,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc);

extern JS_PUBLIC_API bool JS_GetOwnUCPropertyDescriptor(
    JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc);

/**
 * DEPRECATED
 *
 * Like JS_GetOwnPropertyDescriptorById, but also searches the prototype chain
 * if no own property is found directly on obj. The object on which the
 * property is found is returned in holder. If the property is not found
 * on the prototype chain, then desc is Nothing.
 */
extern JS_PUBLIC_API bool JS_GetPropertyDescriptorById(
    JSContext* cx, JS::HandleObject obj, JS::HandleId id,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc,
    JS::MutableHandleObject holder);

extern JS_PUBLIC_API bool JS_GetPropertyDescriptor(
    JSContext* cx, JS::HandleObject obj, const char* name,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc,
    JS::MutableHandleObject holder);

extern JS_PUBLIC_API bool JS_GetUCPropertyDescriptor(
    JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc,
    JS::MutableHandleObject holder);

/**
 * Define a property on obj.
 *
 * This function uses JS::ObjectOpResult to indicate conditions that ES6
 * specifies as non-error failures. This is inconvenient at best, so use this
 * function only if you are implementing a proxy handler's defineProperty()
 * method. For all other purposes, use one of the many DefineProperty functions
 * below that throw an exception in all failure cases.
 *
 * Implements: ES6 [[DefineOwnProperty]] internal method.
 */
extern JS_PUBLIC_API bool JS_DefinePropertyById(
    JSContext* cx, JS::HandleObject obj, JS::HandleId id,
    JS::Handle<JS::PropertyDescriptor> desc, JS::ObjectOpResult& result);

/**
 * Define a property on obj, throwing a TypeError if the attempt fails.
 * This is the C++ equivalent of `Object.defineProperty(obj, id, desc)`.
 */
extern JS_PUBLIC_API bool JS_DefinePropertyById(
    JSContext* cx, JS::HandleObject obj, JS::HandleId id,
    JS::Handle<JS::PropertyDescriptor> desc);

extern JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                                JS::HandleObject obj,
                                                JS::HandleId id,
                                                JS::HandleValue value,
                                                unsigned attrs);

extern JS_PUBLIC_API bool JS_DefinePropertyById(
    JSContext* cx, JS::HandleObject obj, JS::HandleId id, JSNative getter,
    JSNative setter, unsigned attrs);

extern JS_PUBLIC_API bool JS_DefinePropertyById(
    JSContext* cx, JS::HandleObject obj, JS::HandleId id,
    JS::HandleObject getter, JS::HandleObject setter, unsigned attrs);

extern JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                                JS::HandleObject obj,
                                                JS::HandleId id,
                                                JS::HandleObject value,
                                                unsigned attrs);

extern JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                                JS::HandleObject obj,
                                                JS::HandleId id,
                                                JS::HandleString value,
                                                unsigned attrs);

extern JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                                JS::HandleObject obj,
                                                JS::HandleId id, int32_t value,
                                                unsigned attrs);

extern JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                                JS::HandleObject obj,
                                                JS::HandleId id, uint32_t value,
                                                unsigned attrs);

extern JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx,
                                                JS::HandleObject obj,
                                                JS::HandleId id, double value,
                                                unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::HandleObject obj,
                                            const char* name,
                                            JS::HandleValue value,
                                            unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::HandleObject obj,
                                            const char* name, JSNative getter,
                                            JSNative setter, unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::HandleObject obj,
                                            const char* name,
                                            JS::HandleObject getter,
                                            JS::HandleObject setter,
                                            unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::HandleObject obj,
                                            const char* name,
                                            JS::HandleObject value,
                                            unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::HandleObject obj,
                                            const char* name,
                                            JS::HandleString value,
                                            unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::HandleObject obj,
                                            const char* name, int32_t value,
                                            unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::HandleObject obj,
                                            const char* name, uint32_t value,
                                            unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, JS::HandleObject obj,
                                            const char* name, double value,
                                            unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineUCProperty(
    JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
    JS::Handle<JS::PropertyDescriptor> desc, JS::ObjectOpResult& result);

extern JS_PUBLIC_API bool JS_DefineUCProperty(
    JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
    JS::Handle<JS::PropertyDescriptor> desc);

extern JS_PUBLIC_API bool JS_DefineUCProperty(
    JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
    JS::HandleValue value, unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineUCProperty(
    JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
    JS::HandleObject getter, JS::HandleObject setter, unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineUCProperty(
    JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
    JS::HandleObject value, unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineUCProperty(
    JSContext* cx, JS::HandleObject obj, const char16_t* name, size_t namelen,
    JS::HandleString value, unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx,
                                              JS::HandleObject obj,
                                              const char16_t* name,
                                              size_t namelen, int32_t value,
                                              unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx,
                                              JS::HandleObject obj,
                                              const char16_t* name,
                                              size_t namelen, uint32_t value,
                                              unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx,
                                              JS::HandleObject obj,
                                              const char16_t* name,
                                              size_t namelen, double value,
                                              unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::HandleObject obj,
                                           uint32_t index,
                                           JS::HandleValue value,
                                           unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::HandleObject obj,
                                           uint32_t index,
                                           JS::HandleObject getter,
                                           JS::HandleObject setter,
                                           unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::HandleObject obj,
                                           uint32_t index,
                                           JS::HandleObject value,
                                           unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::HandleObject obj,
                                           uint32_t index,
                                           JS::HandleString value,
                                           unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::HandleObject obj,
                                           uint32_t index, int32_t value,
                                           unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::HandleObject obj,
                                           uint32_t index, uint32_t value,
                                           unsigned attrs);

extern JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, JS::HandleObject obj,
                                           uint32_t index, double value,
                                           unsigned attrs);

/**
 * Compute the expression `id in obj`.
 *
 * If obj has an own or inherited property obj[id], set *foundp = true and
 * return true. If not, set *foundp = false and return true. On error, return
 * false with an exception pending.
 *
 * Implements: ES6 [[Has]] internal method.
 */
extern JS_PUBLIC_API bool JS_HasPropertyById(JSContext* cx,
                                             JS::HandleObject obj,
                                             JS::HandleId id, bool* foundp);

extern JS_PUBLIC_API bool JS_HasProperty(JSContext* cx, JS::HandleObject obj,
                                         const char* name, bool* foundp);

extern JS_PUBLIC_API bool JS_HasUCProperty(JSContext* cx, JS::HandleObject obj,
                                           const char16_t* name, size_t namelen,
                                           bool* vp);

extern JS_PUBLIC_API bool JS_HasElement(JSContext* cx, JS::HandleObject obj,
                                        uint32_t index, bool* foundp);

/**
 * Determine whether obj has an own property with the key `id`.
 *
 * Implements: ES6 7.3.11 HasOwnProperty(O, P).
 */
extern JS_PUBLIC_API bool JS_HasOwnPropertyById(JSContext* cx,
                                                JS::HandleObject obj,
                                                JS::HandleId id, bool* foundp);

extern JS_PUBLIC_API bool JS_HasOwnProperty(JSContext* cx, JS::HandleObject obj,
                                            const char* name, bool* foundp);

/**
 * Get the value of the property `obj[id]`, or undefined if no such property
 * exists. This is the C++ equivalent of `vp = Reflect.get(obj, id, receiver)`.
 *
 * Most callers don't need the `receiver` argument. Consider using
 * JS_GetProperty instead. (But if you're implementing a proxy handler's set()
 * method, it's often correct to call this function and pass the receiver
 * through.)
 *
 * Implements: ES6 [[Get]] internal method.
 */
extern JS_PUBLIC_API bool JS_ForwardGetPropertyTo(JSContext* cx,
                                                  JS::HandleObject obj,
                                                  JS::HandleId id,
                                                  JS::HandleValue receiver,
                                                  JS::MutableHandleValue vp);

extern JS_PUBLIC_API bool JS_ForwardGetElementTo(JSContext* cx,
                                                 JS::HandleObject obj,
                                                 uint32_t index,
                                                 JS::HandleObject receiver,
                                                 JS::MutableHandleValue vp);

/**
 * Get the value of the property `obj[id]`, or undefined if no such property
 * exists. The result is stored in vp.
 *
 * Implements: ES6 7.3.1 Get(O, P).
 */
extern JS_PUBLIC_API bool JS_GetPropertyById(JSContext* cx,
                                             JS::HandleObject obj,
                                             JS::HandleId id,
                                             JS::MutableHandleValue vp);

extern JS_PUBLIC_API bool JS_GetProperty(JSContext* cx, JS::HandleObject obj,
                                         const char* name,
                                         JS::MutableHandleValue vp);

extern JS_PUBLIC_API bool JS_GetUCProperty(JSContext* cx, JS::HandleObject obj,
                                           const char16_t* name, size_t namelen,
                                           JS::MutableHandleValue vp);

extern JS_PUBLIC_API bool JS_GetElement(JSContext* cx, JS::HandleObject obj,
                                        uint32_t index,
                                        JS::MutableHandleValue vp);

/**
 * Perform the same property assignment as `Reflect.set(obj, id, v, receiver)`.
 *
 * This function has a `receiver` argument that most callers don't need.
 * Consider using JS_SetProperty instead.
 *
 * Implements: ES6 [[Set]] internal method.
 */
extern JS_PUBLIC_API bool JS_ForwardSetPropertyTo(
    JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleValue v,
    JS::HandleValue receiver, JS::ObjectOpResult& result);

/**
 * Perform the assignment `obj[id] = v`.
 *
 * This function performs non-strict assignment, so if the property is
 * read-only, nothing happens and no error is thrown.
 */
extern JS_PUBLIC_API bool JS_SetPropertyById(JSContext* cx,
                                             JS::HandleObject obj,
                                             JS::HandleId id,
                                             JS::HandleValue v);

extern JS_PUBLIC_API bool JS_SetProperty(JSContext* cx, JS::HandleObject obj,
                                         const char* name, JS::HandleValue v);

extern JS_PUBLIC_API bool JS_SetUCProperty(JSContext* cx, JS::HandleObject obj,
                                           const char16_t* name, size_t namelen,
                                           JS::HandleValue v);

extern JS_PUBLIC_API bool JS_SetElement(JSContext* cx, JS::HandleObject obj,
                                        uint32_t index, JS::HandleValue v);

extern JS_PUBLIC_API bool JS_SetElement(JSContext* cx, JS::HandleObject obj,
                                        uint32_t index, JS::HandleObject v);

extern JS_PUBLIC_API bool JS_SetElement(JSContext* cx, JS::HandleObject obj,
                                        uint32_t index, JS::HandleString v);

extern JS_PUBLIC_API bool JS_SetElement(JSContext* cx, JS::HandleObject obj,
                                        uint32_t index, int32_t v);

extern JS_PUBLIC_API bool JS_SetElement(JSContext* cx, JS::HandleObject obj,
                                        uint32_t index, uint32_t v);

extern JS_PUBLIC_API bool JS_SetElement(JSContext* cx, JS::HandleObject obj,
                                        uint32_t index, double v);

/**
 * Delete a property. This is the C++ equivalent of
 * `result = Reflect.deleteProperty(obj, id)`.
 *
 * This function has a `result` out parameter that most callers don't need.
 * Unless you can pass through an ObjectOpResult provided by your caller, it's
 * probably best to use the JS_DeletePropertyById signature with just 3
 * arguments.
 *
 * Implements: ES6 [[Delete]] internal method.
 */
extern JS_PUBLIC_API bool JS_DeletePropertyById(JSContext* cx,
                                                JS::HandleObject obj,
                                                JS::HandleId id,
                                                JS::ObjectOpResult& result);

extern JS_PUBLIC_API bool JS_DeleteProperty(JSContext* cx, JS::HandleObject obj,
                                            const char* name,
                                            JS::ObjectOpResult& result);

extern JS_PUBLIC_API bool JS_DeleteUCProperty(JSContext* cx,
                                              JS::HandleObject obj,
                                              const char16_t* name,
                                              size_t namelen,
                                              JS::ObjectOpResult& result);

extern JS_PUBLIC_API bool JS_DeleteElement(JSContext* cx, JS::HandleObject obj,
                                           uint32_t index,
                                           JS::ObjectOpResult& result);

/**
 * Delete a property, ignoring strict failures. This is the C++ equivalent of
 * the JS `delete obj[id]` in non-strict mode code.
 */
extern JS_PUBLIC_API bool JS_DeletePropertyById(JSContext* cx,
                                                JS::HandleObject obj, jsid id);

extern JS_PUBLIC_API bool JS_DeleteProperty(JSContext* cx, JS::HandleObject obj,
                                            const char* name);

extern JS_PUBLIC_API bool JS_DeleteElement(JSContext* cx, JS::HandleObject obj,
                                           uint32_t index);

/**
 * Get an array of the non-symbol enumerable properties of obj.
 * This function is roughly equivalent to:
 *
 *     var result = [];
 *     for (key in obj) {
 *         result.push(key);
 *     }
 *     return result;
 *
 * This is the closest thing we currently have to the ES6 [[Enumerate]]
 * internal method.
 *
 * The array of ids returned by JS_Enumerate must be rooted to protect its
 * contents from garbage collection. Use JS::Rooted<JS::IdVector>.
 */
extern JS_PUBLIC_API bool JS_Enumerate(JSContext* cx, JS::HandleObject obj,
                                       JS::MutableHandle<JS::IdVector> props);

/**
 * Equivalent to `Object.assign(target, src)`: Copies the properties from the
 * `src` object (which must not be null) to `target` (which also must not be
 * null).
 */
extern JS_PUBLIC_API bool JS_AssignObject(JSContext* cx,
                                          JS::HandleObject target,
                                          JS::HandleObject src);

/*
 * API for determining callability and constructability. [[Call]] and
 * [[Construct]] are internal methods that aren't present on all objects, so it
 * is useful to ask if they are there or not. The standard itself asks these
 * questions routinely.
 */
namespace JS {

/**
 * Return true if the given object is callable. In ES6 terms, an object is
 * callable if it has a [[Call]] internal method.
 *
 * Implements: ES6 7.2.3 IsCallable(argument).
 *
 * Functions are callable. A scripted proxy or wrapper is callable if its
 * target is callable. Most other objects aren't callable.
 */
extern JS_PUBLIC_API bool IsCallable(JSObject* obj);

/**
 * Return true if the given object is a constructor. In ES6 terms, an object is
 * a constructor if it has a [[Construct]] internal method. The expression
 * `new obj()` throws a TypeError if obj is not a constructor.
 *
 * Implements: ES6 7.2.4 IsConstructor(argument).
 *
 * JS functions and classes are constructors. Arrow functions and most builtin
 * functions are not. A scripted proxy or wrapper is a constructor if its
 * target is a constructor.
 */
extern JS_PUBLIC_API bool IsConstructor(JSObject* obj);

} /* namespace JS */

/**
 * Call a function, passing a this-value and arguments. This is the C++
 * equivalent of `rval = Reflect.apply(fun, obj, args)`.
 *
 * Implements: ES6 7.3.12 Call(F, V, [argumentsList]).
 * Use this function to invoke the [[Call]] internal method.
 */
extern JS_PUBLIC_API bool JS_CallFunctionValue(JSContext* cx,
                                               JS::HandleObject obj,
                                               JS::HandleValue fval,
                                               const JS::HandleValueArray& args,
                                               JS::MutableHandleValue rval);

extern JS_PUBLIC_API bool JS_CallFunction(JSContext* cx, JS::HandleObject obj,
                                          JS::HandleFunction fun,
                                          const JS::HandleValueArray& args,
                                          JS::MutableHandleValue rval);

/**
 * Perform the method call `rval = obj[name](args)`.
 */
extern JS_PUBLIC_API bool JS_CallFunctionName(JSContext* cx,
                                              JS::HandleObject obj,
                                              const char* name,
                                              const JS::HandleValueArray& args,
                                              JS::MutableHandleValue rval);

namespace JS {

static inline bool Call(JSContext* cx, JS::HandleObject thisObj,
                        JS::HandleFunction fun,
                        const JS::HandleValueArray& args,
                        MutableHandleValue rval) {
  return !!JS_CallFunction(cx, thisObj, fun, args, rval);
}

static inline bool Call(JSContext* cx, JS::HandleObject thisObj,
                        JS::HandleValue fun, const JS::HandleValueArray& args,
                        MutableHandleValue rval) {
  return !!JS_CallFunctionValue(cx, thisObj, fun, args, rval);
}

static inline bool Call(JSContext* cx, JS::HandleObject thisObj,
                        const char* name, const JS::HandleValueArray& args,
                        MutableHandleValue rval) {
  return !!JS_CallFunctionName(cx, thisObj, name, args, rval);
}

extern JS_PUBLIC_API bool Call(JSContext* cx, JS::HandleValue thisv,
                               JS::HandleValue fun,
                               const JS::HandleValueArray& args,
                               MutableHandleValue rval);

static inline bool Call(JSContext* cx, JS::HandleValue thisv,
                        JS::HandleObject funObj,
                        const JS::HandleValueArray& args,
                        MutableHandleValue rval) {
  MOZ_ASSERT(funObj);
  JS::RootedValue fun(cx, JS::ObjectValue(*funObj));
  return Call(cx, thisv, fun, args, rval);
}

/**
 * Invoke a constructor. This is the C++ equivalent of
 * `rval = Reflect.construct(fun, args, newTarget)`.
 *
 * JS::Construct() takes a `newTarget` argument that most callers don't need.
 * Consider using the four-argument Construct signature instead. (But if you're
 * implementing a subclass or a proxy handler's construct() method, this is the
 * right function to call.)
 *
 * Implements: ES6 7.3.13 Construct(F, [argumentsList], [newTarget]).
 * Use this function to invoke the [[Construct]] internal method.
 */
extern JS_PUBLIC_API bool Construct(JSContext* cx, JS::HandleValue fun,
                                    HandleObject newTarget,
                                    const JS::HandleValueArray& args,
                                    MutableHandleObject objp);

/**
 * Invoke a constructor. This is the C++ equivalent of
 * `rval = new fun(...args)`.
 *
 * Implements: ES6 7.3.13 Construct(F, [argumentsList], [newTarget]), when
 * newTarget is omitted.
 */
extern JS_PUBLIC_API bool Construct(JSContext* cx, JS::HandleValue fun,
                                    const JS::HandleValueArray& args,
                                    MutableHandleObject objp);

} /* namespace JS */

/*** Other property-defining functions **************************************/

extern JS_PUBLIC_API JSObject* JS_DefineObject(JSContext* cx,
                                               JS::HandleObject obj,
                                               const char* name,
                                               const JSClass* clasp = nullptr,
                                               unsigned attrs = 0);

extern JS_PUBLIC_API bool JS_DefineProperties(JSContext* cx,
                                              JS::HandleObject obj,
                                              const JSPropertySpec* ps);

/* * */

extern JS_PUBLIC_API bool JS_AlreadyHasOwnPropertyById(JSContext* cx,
                                                       JS::HandleObject obj,
                                                       JS::HandleId id,
                                                       bool* foundp);

extern JS_PUBLIC_API bool JS_AlreadyHasOwnProperty(JSContext* cx,
                                                   JS::HandleObject obj,
                                                   const char* name,
                                                   bool* foundp);

extern JS_PUBLIC_API bool JS_AlreadyHasOwnUCProperty(JSContext* cx,
                                                     JS::HandleObject obj,
                                                     const char16_t* name,
                                                     size_t namelen,
                                                     bool* foundp);

extern JS_PUBLIC_API bool JS_AlreadyHasOwnElement(JSContext* cx,
                                                  JS::HandleObject obj,
                                                  uint32_t index, bool* foundp);

namespace JS {

/**
 * On success, returns true, setting |*isMap| to true if |obj| is a Map object
 * or a wrapper around one, or to false if not.  Returns false on failure.
 *
 * This method returns true with |*isMap == false| when passed an ES6 proxy
 * whose target is a Map, or when passed a revoked proxy.
 */
extern JS_PUBLIC_API bool IsMapObject(JSContext* cx, JS::HandleObject obj,
                                      bool* isMap);

/**
 * On success, returns true, setting |*isSet| to true if |obj| is a Set object
 * or a wrapper around one, or to false if not.  Returns false on failure.
 *
 * This method returns true with |*isSet == false| when passed an ES6 proxy
 * whose target is a Set, or when passed a revoked proxy.
 */
extern JS_PUBLIC_API bool IsSetObject(JSContext* cx, JS::HandleObject obj,
                                      bool* isSet);

} /* namespace JS */

/**
 * Assign 'undefined' to all of the object's non-reserved slots. Note: this is
 * done for all slots, regardless of the associated property descriptor.
 */
JS_PUBLIC_API void JS_SetAllNonReservedSlotsToUndefined(JS::HandleObject obj);

extern JS_PUBLIC_API void JS_SetReservedSlot(JSObject* obj, uint32_t index,
                                             const JS::Value& v);

extern JS_PUBLIC_API void JS_InitReservedSlot(JSObject* obj, uint32_t index,
                                              void* ptr, size_t nbytes,
                                              JS::MemoryUse use);

template <typename T>
void JS_InitReservedSlot(JSObject* obj, uint32_t index, T* ptr,
                         JS::MemoryUse use) {
  JS_InitReservedSlot(obj, index, ptr, sizeof(T), use);
}

extern JS_PUBLIC_API void JS_InitPrivate(JSObject* obj, void* data,
                                         size_t nbytes, JS::MemoryUse use);

/************************************************************************/

/* native that can be called as a ctor */
static constexpr unsigned JSFUN_CONSTRUCTOR = 0x400;

/* | of all the JSFUN_* flags */
static constexpr unsigned JSFUN_FLAGS_MASK = 0x400;

static_assert((JSPROP_FLAGS_MASK & JSFUN_FLAGS_MASK) == 0,
              "JSFUN_* flags do not overlap JSPROP_* flags, because bits from "
              "the two flag-sets appear in the same flag in some APIs");

/*
 * Functions and scripts.
 */
extern JS_PUBLIC_API JSFunction* JS_NewFunction(JSContext* cx, JSNative call,
                                                unsigned nargs, unsigned flags,
                                                const char* name);

namespace JS {

extern JS_PUBLIC_API JSFunction* GetSelfHostedFunction(
    JSContext* cx, const char* selfHostedName, HandleId id, unsigned nargs);

/**
 * Create a new function based on the given JSFunctionSpec, *fs.
 * id is the result of a successful call to
 * `PropertySpecNameToId(cx, fs->name, &id)` or
   `PropertySpecNameToPermanentId(cx, fs->name, &id)`.
 *
 * Unlike JS_DefineFunctions, this does not treat fs as an array.
 * *fs must not be JS_FS_END.
 */
extern JS_PUBLIC_API JSFunction* NewFunctionFromSpec(JSContext* cx,
                                                     const JSFunctionSpec* fs,
                                                     HandleId id);

/**
 * Same as above, but without an id arg, for callers who don't have
 * the id already.
 */
extern JS_PUBLIC_API JSFunction* NewFunctionFromSpec(JSContext* cx,
                                                     const JSFunctionSpec* fs);

} /* namespace JS */

extern JS_PUBLIC_API JSObject* JS_GetFunctionObject(JSFunction* fun);

/**
 * Return the function's identifier as a JSString, or null if fun is unnamed.
 * The returned string lives as long as fun, so you don't need to root a saved
 * reference to it if fun is well-connected or rooted, and provided you bound
 * the use of the saved reference by fun's lifetime.
 */
extern JS_PUBLIC_API JSString* JS_GetFunctionId(JSFunction* fun);

/**
 * Return a function's display name. This is the defined name if one was given
 * where the function was defined, or it could be an inferred name by the JS
 * engine in the case that the function was defined to be anonymous. This can
 * still return nullptr if a useful display name could not be inferred. The
 * same restrictions on rooting as those in JS_GetFunctionId apply.
 */
extern JS_PUBLIC_API JSString* JS_GetFunctionDisplayId(JSFunction* fun);

/*
 * Return the arity of fun, which includes default parameters and rest
 * parameter.  This can be used as `nargs` parameter for other functions.
 */
extern JS_PUBLIC_API uint16_t JS_GetFunctionArity(JSFunction* fun);

/*
 * Return the length of fun, which is the original value of .length property.
 */
JS_PUBLIC_API bool JS_GetFunctionLength(JSContext* cx, JS::HandleFunction fun,
                                        uint16_t* length);

/**
 * Infallible predicate to test whether obj is a function object (faster than
 * comparing obj's class name to "Function", but equivalent unless someone has
 * overwritten the "Function" identifier with a different constructor and then
 * created instances using that constructor that might be passed in as obj).
 */
extern JS_PUBLIC_API bool JS_ObjectIsFunction(JSObject* obj);

extern JS_PUBLIC_API bool JS_IsNativeFunction(JSObject* funobj, JSNative call);

/** Return whether the given function is a valid constructor. */
extern JS_PUBLIC_API bool JS_IsConstructor(JSFunction* fun);

extern JS_PUBLIC_API bool JS_DefineFunctions(JSContext* cx,
                                             JS::Handle<JSObject*> obj,
                                             const JSFunctionSpec* fs);

extern JS_PUBLIC_API JSFunction* JS_DefineFunction(
    JSContext* cx, JS::Handle<JSObject*> obj, const char* name, JSNative call,
    unsigned nargs, unsigned attrs);

extern JS_PUBLIC_API JSFunction* JS_DefineUCFunction(
    JSContext* cx, JS::Handle<JSObject*> obj, const char16_t* name,
    size_t namelen, JSNative call, unsigned nargs, unsigned attrs);

extern JS_PUBLIC_API JSFunction* JS_DefineFunctionById(
    JSContext* cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
    JSNative call, unsigned nargs, unsigned attrs);

extern JS_PUBLIC_API bool JS_IsFunctionBound(JSFunction* fun);

extern JS_PUBLIC_API JSObject* JS_GetBoundFunctionTarget(JSFunction* fun);

extern JS_PUBLIC_API JSObject* JS_GetGlobalFromScript(JSScript* script);

extern JS_PUBLIC_API const char* JS_GetScriptFilename(JSScript* script);

extern JS_PUBLIC_API unsigned JS_GetScriptBaseLineNumber(JSContext* cx,
                                                         JSScript* script);

extern JS_PUBLIC_API JSScript* JS_GetFunctionScript(JSContext* cx,
                                                    JS::HandleFunction fun);

extern JS_PUBLIC_API JSString* JS_DecompileScript(JSContext* cx,
                                                  JS::Handle<JSScript*> script);

extern JS_PUBLIC_API JSString* JS_DecompileFunction(
    JSContext* cx, JS::Handle<JSFunction*> fun);

namespace JS {

/**
 * Set a private value associated with a script. Note that this value is shared
 * by all nested scripts compiled from a single source file.
 */
extern JS_PUBLIC_API void SetScriptPrivate(JSScript* script,
                                           const JS::Value& value);

/**
 * Get the private value associated with a script. Note that this value is
 * shared by all nested scripts compiled from a single source file.
 */
extern JS_PUBLIC_API JS::Value GetScriptPrivate(JSScript* script);

/*
 * Return the private value associated with currently executing script or
 * module, or undefined if there is no such script.
 */
extern JS_PUBLIC_API JS::Value GetScriptedCallerPrivate(JSContext* cx);

/**
 * Hooks called when references to a script private value are created or
 * destroyed. This allows use of a reference counted object as the
 * script private.
 */
using ScriptPrivateReferenceHook = void (*)(const JS::Value&);

/**
 * Set the script private finalize hook for the runtime to the given function.
 */
extern JS_PUBLIC_API void SetScriptPrivateReferenceHooks(
    JSRuntime* rt, ScriptPrivateReferenceHook addRefHook,
    ScriptPrivateReferenceHook releaseHook);

} /* namespace JS */

extern JS_PUBLIC_API bool JS_CheckForInterrupt(JSContext* cx);

/*
 * These functions allow setting an interrupt callback that will be called
 * from the JS thread some time after any thread triggered the callback using
 * JS_RequestInterruptCallback(cx).
 *
 * To schedule the GC and for other activities the engine internally triggers
 * interrupt callbacks. The embedding should thus not rely on callbacks being
 * triggered through the external API only.
 *
 * Important note: Additional callbacks can occur inside the callback handler
 * if it re-enters the JS engine. The embedding must ensure that the callback
 * is disconnected before attempting such re-entry.
 */
extern JS_PUBLIC_API bool JS_AddInterruptCallback(JSContext* cx,
                                                  JSInterruptCallback callback);

extern JS_PUBLIC_API bool JS_DisableInterruptCallback(JSContext* cx);

extern JS_PUBLIC_API void JS_ResetInterruptCallback(JSContext* cx, bool enable);

extern JS_PUBLIC_API void JS_RequestInterruptCallback(JSContext* cx);

extern JS_PUBLIC_API void JS_RequestInterruptCallbackCanWait(JSContext* cx);

namespace JS {

/**
 * The ConsumeStreamCallback is called from an active JSContext, passing a
 * StreamConsumer that wishes to consume the given host object as a stream of
 * bytes with the given MIME type. On failure, the embedding must report the
 * appropriate error on 'cx'. On success, the embedding must call
 * consumer->consumeChunk() repeatedly on any thread until exactly one of:
 *  - consumeChunk() returns false
 *  - the embedding calls consumer->streamEnd()
 *  - the embedding calls consumer->streamError()
 * before JS_DestroyContext(cx) or JS::ShutdownAsyncTasks(cx) is called.
 *
 * Note: consumeChunk(), streamEnd() and streamError() may be called
 * synchronously by ConsumeStreamCallback.
 *
 * When streamEnd() is called, the embedding may optionally pass an
 * OptimizedEncodingListener*, indicating that there is a cache entry associated
 * with this stream that can store an optimized encoding of the bytes that were
 * just streamed at some point in the future by having SpiderMonkey call
 * storeOptimizedEncoding(). Until the optimized encoding is ready, SpiderMonkey
 * will hold an outstanding refcount to keep the listener alive.
 *
 * After storeOptimizedEncoding() is called, on cache hit, the embedding
 * may call consumeOptimizedEncoding() instead of consumeChunk()/streamEnd().
 * The embedding must ensure that the GetOptimizedEncodingBuildId() (see
 * js/BuildId.h) at the time when an optimized encoding is created is the same
 * as when it is later consumed.
 */

using OptimizedEncodingBytes = js::Vector<uint8_t, 0, js::SystemAllocPolicy>;
using UniqueOptimizedEncodingBytes = js::UniquePtr<OptimizedEncodingBytes>;

class OptimizedEncodingListener {
 protected:
  virtual ~OptimizedEncodingListener() = default;

 public:
  // SpiderMonkey will hold an outstanding reference count as long as it holds
  // a pointer to OptimizedEncodingListener.
  virtual MozExternalRefCountType MOZ_XPCOM_ABI AddRef() = 0;
  virtual MozExternalRefCountType MOZ_XPCOM_ABI Release() = 0;

  // SpiderMonkey may optionally call storeOptimizedEncoding() after it has
  // finished processing a streamed resource.
  virtual void storeOptimizedEncoding(UniqueOptimizedEncodingBytes bytes) = 0;
};

class JS_PUBLIC_API StreamConsumer {
 protected:
  // AsyncStreamConsumers are created and destroyed by SpiderMonkey.
  StreamConsumer() = default;
  virtual ~StreamConsumer() = default;

 public:
  // Called by the embedding as each chunk of bytes becomes available.
  // If this function returns 'false', the stream must drop all pointers to
  // this StreamConsumer.
  virtual bool consumeChunk(const uint8_t* begin, size_t length) = 0;

  // Called by the embedding when the stream reaches end-of-file, passing the
  // listener described above.
  virtual void streamEnd(OptimizedEncodingListener* listener = nullptr) = 0;

  // Called by the embedding when there is an error during streaming. The
  // given error code should be passed to the ReportStreamErrorCallback on the
  // main thread to produce the semantically-correct rejection value.
  virtual void streamError(size_t errorCode) = 0;

  // Called by the embedding *instead of* consumeChunk()/streamEnd() if an
  // optimized encoding is available from a previous streaming of the same
  // contents with the same optimized build id.
  virtual void consumeOptimizedEncoding(const uint8_t* begin,
                                        size_t length) = 0;

  // Provides optional stream attributes such as base or source mapping URLs.
  // Necessarily called before consumeChunk(), streamEnd(), streamError() or
  // consumeOptimizedEncoding(). The caller retains ownership of the strings.
  virtual void noteResponseURLs(const char* maybeUrl,
                                const char* maybeSourceMapUrl) = 0;
};

enum class MimeType { Wasm };

using ConsumeStreamCallback = bool (*)(JSContext*, JS::HandleObject, MimeType,
                                       StreamConsumer*);

using ReportStreamErrorCallback = void (*)(JSContext*, size_t);

extern JS_PUBLIC_API void InitConsumeStreamCallback(
    JSContext* cx, ConsumeStreamCallback consume,
    ReportStreamErrorCallback report);

/**
 * Supply an alternative stack to incorporate into captured SavedFrame
 * backtraces as the imputed caller of asynchronous JavaScript calls, like async
 * function resumptions and DOM callbacks.
 *
 * When one async function awaits the result of another, it's natural to think
 * of that as a sort of function call: just as execution resumes from an
 * ordinary call expression when the callee returns, with the return value
 * providing the value of the call expression, execution resumes from an 'await'
 * expression after the awaited asynchronous function call returns, passing the
 * return value along.
 *
 * Call the two async functions in such a situation the 'awaiter' and the
 * 'awaitee'.
 *
 * As an async function, the awaitee contains 'await' expressions of its own.
 * Whenever it executes after its first 'await', there are never any actual
 * frames on the JavaScript stack under it; its awaiter is certainly not there.
 * An await expression's continuation is invoked as a promise callback, and
 * those are always called directly from the event loop in their own microtick.
 * (Ignore unusual cases like nested event loops.)
 *
 * But because await expressions bear such a strong resemblance to calls (and
 * deliberately so!), it would be unhelpful for stacks captured within the
 * awaitee to be empty; instead, they should present the awaiter as the caller.
 *
 * The AutoSetAsyncStackForNewCalls RAII class supplies a SavedFrame stack to
 * treat as the caller of any JavaScript invocations that occur within its
 * lifetime. Any SavedFrame stack captured during such an invocation uses the
 * SavedFrame passed to the constructor's 'stack' parameter as the 'asyncParent'
 * property of the SavedFrame for the invocation's oldest frame. Its 'parent'
 * property will be null, so stack-walking code can distinguish this
 * awaiter/awaitee transition from an ordinary caller/callee transition.
 *
 * The constructor's 'asyncCause' parameter supplies a string explaining what
 * sort of asynchronous call caused 'stack' to be spliced into the backtrace;
 * for example, async function resumptions use the string "async". This appears
 * as the 'asyncCause' property of the 'asyncParent' SavedFrame.
 *
 * Async callers are distinguished in the string form of a SavedFrame chain by
 * including the 'asyncCause' string in the frame. It appears before the
 * function name, with the two separated by a '*'.
 *
 * Note that, as each compartment has its own set of SavedFrames, the
 * 'asyncParent' may actually point to a copy of 'stack', rather than the exact
 * SavedFrame object passed.
 *
 * The youngest frame of 'stack' is not mutated to take the asyncCause string as
 * its 'asyncCause' property; SavedFrame objects are immutable. Rather, a fresh
 * clone of the frame is created with the needed 'asyncCause' property.
 *
 * The 'kind' argument specifies how aggressively 'stack' supplants any
 * JavaScript frames older than this AutoSetAsyncStackForNewCalls object. If
 * 'kind' is 'EXPLICIT', then all captured SavedFrame chains take on 'stack' as
 * their 'asyncParent' where the chain crosses this object's scope. If 'kind' is
 * 'IMPLICIT', then 'stack' is only included in captured chains if there are no
 * other JavaScript frames on the stack --- that is, only if the stack would
 * otherwise end at that point.
 *
 * AutoSetAsyncStackForNewCalls affects only SavedFrame chains; it does not
 * affect Debugger.Frame or js::FrameIter. SavedFrame chains are used for
 * Error.stack, allocation profiling, Promise debugging, and so on.
 *
 * See also `js/src/doc/SavedFrame/SavedFrame.md` for documentation on async
 * stack frames.
 */
class MOZ_STACK_CLASS JS_PUBLIC_API AutoSetAsyncStackForNewCalls {
  JSContext* cx;
  RootedObject oldAsyncStack;
  const char* oldAsyncCause;
  bool oldAsyncCallIsExplicit;

 public:
  enum class AsyncCallKind {
    // The ordinary kind of call, where we may apply an async
    // parent if there is no ordinary parent.
    IMPLICIT,
    // An explicit async parent, e.g., callFunctionWithAsyncStack,
    // where we always want to override any ordinary parent.
    EXPLICIT
  };

  // The stack parameter cannot be null by design, because it would be
  // ambiguous whether that would clear any scheduled async stack and make the
  // normal stack reappear in the new call, or just keep the async stack
  // already scheduled for the new call, if any.
  //
  // asyncCause is owned by the caller and its lifetime must outlive the
  // lifetime of the AutoSetAsyncStackForNewCalls object. It is strongly
  // encouraged that asyncCause be a string constant or similar statically
  // allocated string.
  AutoSetAsyncStackForNewCalls(JSContext* cx, HandleObject stack,
                               const char* asyncCause,
                               AsyncCallKind kind = AsyncCallKind::IMPLICIT);
  ~AutoSetAsyncStackForNewCalls();
};

}  // namespace JS

/************************************************************************/

namespace JS {

JS_PUBLIC_API bool PropertySpecNameEqualsId(JSPropertySpec::Name name,
                                            HandleId id);

/**
 * Create a jsid that does not need to be marked for GC.
 *
 * 'name' is a JSPropertySpec::name or JSFunctionSpec::name value. The
 * resulting jsid, on success, is either an interned string or a well-known
 * symbol; either way it is immune to GC so there is no need to visit *idp
 * during GC marking.
 */
JS_PUBLIC_API bool PropertySpecNameToPermanentId(JSContext* cx,
                                                 JSPropertySpec::Name name,
                                                 jsid* idp);

} /* namespace JS */

/************************************************************************/

/**
 * A JS context always has an "owner thread". The owner thread is set when the
 * context is created (to the current thread) and practically all entry points
 * into the JS engine check that a context (or anything contained in the
 * context: runtime, compartment, object, etc) is only touched by its owner
 * thread. Embeddings may check this invariant outside the JS engine by calling
 * JS_AbortIfWrongThread (which will abort if not on the owner thread, even for
 * non-debug builds).
 */

extern JS_PUBLIC_API void JS_AbortIfWrongThread(JSContext* cx);

/************************************************************************/

/**
 * A constructor can request that the JS engine create a default new 'this'
 * object of the given class, using the callee to determine parentage and
 * [[Prototype]].
 */
extern JS_PUBLIC_API JSObject* JS_NewObjectForConstructor(
    JSContext* cx, const JSClass* clasp, const JS::CallArgs& args);

/************************************************************************/

extern JS_PUBLIC_API void JS_SetParallelParsingEnabled(JSContext* cx,
                                                       bool enabled);

extern JS_PUBLIC_API void JS_SetOffthreadIonCompilationEnabled(JSContext* cx,
                                                               bool enabled);

// clang-format off
#define JIT_COMPILER_OPTIONS(Register) \
  Register(BASELINE_INTERPRETER_WARMUP_TRIGGER, "blinterp.warmup.trigger") \
  Register(BASELINE_WARMUP_TRIGGER, "baseline.warmup.trigger") \
  Register(IC_FORCE_MEGAMORPHIC, "ic.force-megamorphic") \
  Register(ION_NORMAL_WARMUP_TRIGGER, "ion.warmup.trigger") \
  Register(ION_GVN_ENABLE, "ion.gvn.enable") \
  Register(ION_FORCE_IC, "ion.forceinlineCaches") \
  Register(ION_ENABLE, "ion.enable") \
  Register(JIT_TRUSTEDPRINCIPALS_ENABLE, "jit_trustedprincipals.enable") \
  Register(ION_CHECK_RANGE_ANALYSIS, "ion.check-range-analysis") \
  Register(ION_FREQUENT_BAILOUT_THRESHOLD, "ion.frequent-bailout-threshold") \
  Register(INLINING_BYTECODE_MAX_LENGTH, "inlining.bytecode-max-length") \
  Register(BASELINE_INTERPRETER_ENABLE, "blinterp.enable") \
  Register(BASELINE_ENABLE, "baseline.enable") \
  Register(OFFTHREAD_COMPILATION_ENABLE, "offthread-compilation.enable")  \
  Register(FULL_DEBUG_CHECKS, "jit.full-debug-checks") \
  Register(JUMP_THRESHOLD, "jump-threshold") \
  Register(NATIVE_REGEXP_ENABLE, "native_regexp.enable") \
  Register(SIMULATOR_ALWAYS_INTERRUPT, "simulator.always-interrupt")      \
  Register(SPECTRE_INDEX_MASKING, "spectre.index-masking") \
  Register(SPECTRE_OBJECT_MITIGATIONS, "spectre.object-mitigations") \
  Register(SPECTRE_STRING_MITIGATIONS, "spectre.string-mitigations") \
  Register(SPECTRE_VALUE_MASKING, "spectre.value-masking") \
  Register(SPECTRE_JIT_TO_CXX_CALLS, "spectre.jit-to-cxx-calls") \
  Register(WASM_FOLD_OFFSETS, "wasm.fold-offsets") \
  Register(WASM_DELAY_TIER2, "wasm.delay-tier2") \
  Register(WASM_JIT_BASELINE, "wasm.baseline") \
  Register(WASM_JIT_OPTIMIZING, "wasm.optimizing") \
// clang-format on

typedef enum JSJitCompilerOption {
#define JIT_COMPILER_DECLARE(key, str) JSJITCOMPILER_##key,

  JIT_COMPILER_OPTIONS(JIT_COMPILER_DECLARE)
#undef JIT_COMPILER_DECLARE

      JSJITCOMPILER_NOT_AN_OPTION
} JSJitCompilerOption;

extern JS_PUBLIC_API void JS_SetGlobalJitCompilerOption(JSContext* cx,
                                                        JSJitCompilerOption opt,
                                                        uint32_t value);
extern JS_PUBLIC_API bool JS_GetGlobalJitCompilerOption(JSContext* cx,
                                                        JSJitCompilerOption opt,
                                                        uint32_t* valueOut);

/**
 * Convert a uint32_t index into a jsid.
 */
extern JS_PUBLIC_API bool JS_IndexToId(JSContext* cx, uint32_t index,
                                       JS::MutableHandleId);

/**
 * Convert chars into a jsid.
 *
 * |chars| may not be an index.
 */
extern JS_PUBLIC_API bool JS_CharsToId(JSContext* cx, JS::TwoByteChars chars,
                                       JS::MutableHandleId);

/**
 *  Test if the given string is a valid ECMAScript identifier
 */
extern JS_PUBLIC_API bool JS_IsIdentifier(JSContext* cx, JS::HandleString str,
                                          bool* isIdentifier);

/**
 * Test whether the given chars + length are a valid ECMAScript identifier.
 * This version is infallible, so just returns whether the chars are an
 * identifier.
 */
extern JS_PUBLIC_API bool JS_IsIdentifier(const char16_t* chars, size_t length);

namespace js {
class ScriptSource;
}  // namespace js

namespace JS {

class MOZ_RAII JS_PUBLIC_API AutoFilename {
 private:
  js::ScriptSource* ss_;
  mozilla::Variant<const char*, UniqueChars> filename_;

  AutoFilename(const AutoFilename&) = delete;
  AutoFilename& operator=(const AutoFilename&) = delete;

 public:
  AutoFilename()
      : ss_(nullptr), filename_(mozilla::AsVariant<const char*>(nullptr)) {}

  ~AutoFilename() { reset(); }

  void reset();

  void setOwned(UniqueChars&& filename);
  void setUnowned(const char* filename);
  void setScriptSource(js::ScriptSource* ss);

  const char* get() const;
};

/**
 * Return the current filename, line number and column number of the most
 * currently running frame. Returns true if a scripted frame was found, false
 * otherwise.
 *
 * If a the embedding has hidden the scripted caller for the topmost activation
 * record, this will also return false.
 */
extern JS_PUBLIC_API bool DescribeScriptedCaller(
    JSContext* cx, AutoFilename* filename = nullptr, unsigned* lineno = nullptr,
    unsigned* column = nullptr);

extern JS_PUBLIC_API JSObject* GetScriptedCallerGlobal(JSContext* cx);

/**
 * Informs the JS engine that the scripted caller should be hidden. This can be
 * used by the embedding to maintain an override of the scripted caller in its
 * calculations, by hiding the scripted caller in the JS engine and pushing data
 * onto a separate stack, which it inspects when DescribeScriptedCaller returns
 * null.
 *
 * We maintain a counter on each activation record. Add() increments the counter
 * of the topmost activation, and Remove() decrements it. The count may never
 * drop below zero, and must always be exactly zero when the activation is
 * popped from the stack.
 */
extern JS_PUBLIC_API void HideScriptedCaller(JSContext* cx);

extern JS_PUBLIC_API void UnhideScriptedCaller(JSContext* cx);

class MOZ_RAII AutoHideScriptedCaller {
 public:
  explicit AutoHideScriptedCaller(JSContext* cx) : mContext(cx) {
    HideScriptedCaller(mContext);
  }
  ~AutoHideScriptedCaller() { UnhideScriptedCaller(mContext); }

 protected:
  JSContext* mContext;
};

} /* namespace JS */

namespace js {

enum class StackFormat { SpiderMonkey, V8, Default };

/*
 * Sets the format used for stringifying Error stacks.
 *
 * The default format is StackFormat::SpiderMonkey.  Use StackFormat::V8
 * in order to emulate V8's stack formatting.  StackFormat::Default can't be
 * used here.
 */
extern JS_PUBLIC_API void SetStackFormat(JSContext* cx, StackFormat format);

extern JS_PUBLIC_API StackFormat GetStackFormat(JSContext* cx);

}  // namespace js

namespace JS {

/**
 * Attempt to disable Wasm's usage of reserving a large virtual memory
 * allocation to avoid bounds checking overhead. This must be called before any
 * Wasm module or memory is created in this process, or else this function will
 * fail.
 */
[[nodiscard]] extern JS_PUBLIC_API bool DisableWasmHugeMemory();

/**
 * If a large allocation fails when calling pod_{calloc,realloc}CanGC, the JS
 * engine may call the large-allocation-failure callback, if set, to allow the
 * embedding to flush caches, possibly perform shrinking GCs, etc. to make some
 * room. The allocation will then be retried (and may still fail.) This callback
 * can be called on any thread and must be set at most once in a process.
 */

using LargeAllocationFailureCallback = void (*)();

extern JS_PUBLIC_API void SetProcessLargeAllocationFailureCallback(
    LargeAllocationFailureCallback afc);

/**
 * Unlike the error reporter, which is only called if the exception for an OOM
 * bubbles up and is not caught, the OutOfMemoryCallback is called immediately
 * at the OOM site to allow the embedding to capture the current state of heap
 * allocation before anything is freed. If the large-allocation-failure callback
 * is called at all (not all allocation sites call the large-allocation-failure
 * callback on failure), it is called before the out-of-memory callback; the
 * out-of-memory callback is only called if the allocation still fails after the
 * large-allocation-failure callback has returned.
 */

using OutOfMemoryCallback = void (*)(JSContext*, void*);

extern JS_PUBLIC_API void SetOutOfMemoryCallback(JSContext* cx,
                                                 OutOfMemoryCallback cb,
                                                 void* data);

/**
 * When the JSRuntime is about to block in an Atomics.wait() JS call or in a
 * `wait` instruction in WebAssembly, it can notify the host by means of a call
 * to BeforeWaitCallback.  After the wait, it can notify the host by means of a
 * call to AfterWaitCallback.  Both callbacks must be null, or neither.
 *
 * (If you change the callbacks from null to not-null or vice versa while some
 * thread on the runtime is in a wait, you will be sorry.)
 *
 * The argument to the BeforeWaitCallback is a pointer to uninitialized
 * stack-allocated working memory of size WAIT_CALLBACK_CLIENT_MAXMEM bytes.
 * The caller of SetWaitCallback() must pass the amount of memory it will need,
 * and this amount will be checked against that limit and the process will crash
 * reliably if the check fails.
 *
 * The value returned by the BeforeWaitCallback will be passed to the
 * AfterWaitCallback.
 *
 * The AfterWaitCallback will be called even if the wakeup is spurious and the
 * thread goes right back to waiting again.  Of course the thread will call the
 * BeforeWaitCallback once more before it goes to sleep in this situation.
 */

static constexpr size_t WAIT_CALLBACK_CLIENT_MAXMEM = 32;

using BeforeWaitCallback = void* (*)(uint8_t* memory);
using AfterWaitCallback = void (*)(void* cookie);

extern JS_PUBLIC_API void SetWaitCallback(JSRuntime* rt,
                                          BeforeWaitCallback beforeWait,
                                          AfterWaitCallback afterWait,
                                          size_t requiredMemory);

/**
 * Capture all frames.
 */
struct AllFrames {};

/**
 * Capture at most this many frames.
 */
struct MaxFrames {
  uint32_t maxFrames;

  explicit MaxFrames(uint32_t max) : maxFrames(max) { MOZ_ASSERT(max > 0); }
};

/**
 * Capture the first frame with the given principals. By default, do not
 * consider self-hosted frames with the given principals as satisfying the stack
 * capture.
 */
struct JS_PUBLIC_API FirstSubsumedFrame {
  JSContext* cx;
  JSPrincipals* principals;
  bool ignoreSelfHosted;

  /**
   * Use the cx's current compartment's principals.
   */
  explicit FirstSubsumedFrame(JSContext* cx,
                              bool ignoreSelfHostedFrames = true);

  explicit FirstSubsumedFrame(JSContext* ctx, JSPrincipals* p,
                              bool ignoreSelfHostedFrames = true)
      : cx(ctx), principals(p), ignoreSelfHosted(ignoreSelfHostedFrames) {
    if (principals) {
      JS_HoldPrincipals(principals);
    }
  }

  // No copying because we want to avoid holding and dropping principals
  // unnecessarily.
  FirstSubsumedFrame(const FirstSubsumedFrame&) = delete;
  FirstSubsumedFrame& operator=(const FirstSubsumedFrame&) = delete;

  FirstSubsumedFrame(FirstSubsumedFrame&& rhs)
      : principals(rhs.principals), ignoreSelfHosted(rhs.ignoreSelfHosted) {
    MOZ_ASSERT(this != &rhs, "self move disallowed");
    rhs.principals = nullptr;
  }

  FirstSubsumedFrame& operator=(FirstSubsumedFrame&& rhs) {
    new (this) FirstSubsumedFrame(std::move(rhs));
    return *this;
  }

  ~FirstSubsumedFrame() {
    if (principals) {
      JS_DropPrincipals(cx, principals);
    }
  }
};

using StackCapture = mozilla::Variant<AllFrames, MaxFrames, FirstSubsumedFrame>;

/**
 * Capture the current call stack as a chain of SavedFrame JSObjects, and set
 * |stackp| to the SavedFrame for the youngest stack frame, or nullptr if there
 * are no JS frames on the stack.
 *
 * The |capture| parameter describes the portion of the JS stack to capture:
 *
 *   * |JS::AllFrames|: Capture all frames on the stack.
 *
 *   * |JS::MaxFrames|: Capture no more than |JS::MaxFrames::maxFrames| from the
 *      stack.
 *
 *   * |JS::FirstSubsumedFrame|: Capture the first frame whose principals are
 *     subsumed by |JS::FirstSubsumedFrame::principals|. By default, do not
 *     consider self-hosted frames; this can be controlled via the
 *     |JS::FirstSubsumedFrame::ignoreSelfHosted| flag. Do not capture any async
 *     stack.
 */
extern JS_PUBLIC_API bool CaptureCurrentStack(
    JSContext* cx, MutableHandleObject stackp,
    StackCapture&& capture = StackCapture(AllFrames()));

/**
 * Returns true if capturing stack trace data to associate with an asynchronous
 * operation is currently enabled for the current context realm.
 *
 * Users should check this state before capturing a stack that will be passed
 * back to AutoSetAsyncStackForNewCalls later, in order to avoid capturing a
 * stack for async use when we don't actually want to capture it.
 */
extern JS_PUBLIC_API bool IsAsyncStackCaptureEnabledForRealm(JSContext* cx);

/*
 * This is a utility function for preparing an async stack to be used
 * by some other object.  This may be used when you need to treat a
 * given stack trace as an async parent.  If you just need to capture
 * the current stack, async parents and all, use CaptureCurrentStack
 * instead.
 *
 * Here |asyncStack| is the async stack to prepare.  It is copied into
 * |cx|'s current compartment, and the newest frame is given
 * |asyncCause| as its asynchronous cause.  If |maxFrameCount| is
 * |Some(n)|, capture at most the youngest |n| frames.  The
 * new stack object is written to |stackp|.  Returns true on success,
 * or sets an exception and returns |false| on error.
 */
extern JS_PUBLIC_API bool CopyAsyncStack(
    JSContext* cx, HandleObject asyncStack, HandleString asyncCause,
    MutableHandleObject stackp, const mozilla::Maybe<size_t>& maxFrameCount);

/**
 * Given a SavedFrame JSObject stack, stringify it in the same format as
 * Error.prototype.stack. The stringified stack out parameter is placed in the
 * cx's compartment. Defaults to the empty string.
 *
 * The same notes above about SavedFrame accessors applies here as well: cx
 * doesn't need to be in stack's compartment, and stack can be null, a
 * SavedFrame object, or a wrapper (CCW or Xray) around a SavedFrame object.
 * SavedFrames not subsumed by |principals| are skipped.
 *
 * Optional indent parameter specifies the number of white spaces to indent
 * each line.
 */
extern JS_PUBLIC_API bool BuildStackString(
    JSContext* cx, JSPrincipals* principals, HandleObject stack,
    MutableHandleString stringp, size_t indent = 0,
    js::StackFormat stackFormat = js::StackFormat::Default);

/**
 * Return true iff the given object is either a SavedFrame object or wrapper
 * around a SavedFrame object, and it is not the SavedFrame.prototype object.
 */
extern JS_PUBLIC_API bool IsMaybeWrappedSavedFrame(JSObject* obj);

/**
 * Return true iff the given object is a SavedFrame object and not the
 * SavedFrame.prototype object.
 */
extern JS_PUBLIC_API bool IsUnwrappedSavedFrame(JSObject* obj);

} /* namespace JS */

namespace js {

/**
 * Hint that we expect a crash. Currently, the only thing that cares is the
 * breakpad injector, which (if loaded) will suppress minidump generation.
 */
extern JS_PUBLIC_API void NoteIntentionalCrash();

} /* namespace js */

namespace js {

enum class CompletionKind { Normal, Return, Throw };

} /* namespace js */

#ifdef DEBUG
namespace JS {

extern JS_PUBLIC_API void SetSupportDifferentialTesting(bool value);

}
#endif /* DEBUG */

#endif /* jsapi_h */
