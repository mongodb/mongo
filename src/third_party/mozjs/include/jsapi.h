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
#include "js/CallAndConstruct.h"  // JS::Call, JS_CallFunction, JS_CallFunctionName, JS_CallFunctionValue
#include "js/CallArgs.h"
#include "js/CharacterEncoding.h"
#include "js/Class.h"
#include "js/CompileOptions.h"
#include "js/Context.h"
#include "js/Debug.h"
#include "js/ErrorInterceptor.h"
#include "js/ErrorReport.h"
#include "js/Exception.h"
#include "js/GCAPI.h"
#include "js/GCVector.h"
#include "js/GlobalObject.h"
#include "js/HashTable.h"
#include "js/Id.h"
#include "js/Interrupt.h"
#include "js/MapAndSet.h"
#include "js/MemoryCallbacks.h"
#include "js/MemoryFunctions.h"
#include "js/Principals.h"
#include "js/PropertyAndElement.h"  // JS_Enumerate
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/Realm.h"
#include "js/RealmIterators.h"
#include "js/RealmOptions.h"
#include "js/RefCounted.h"
#include "js/RootingAPI.h"
#include "js/ScriptPrivate.h"
#include "js/Stack.h"
#include "js/StreamConsumer.h"
#include "js/String.h"
#include "js/TelemetryTimers.h"
#include "js/TracingAPI.h"
#include "js/Transcoding.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "js/Value.h"
#include "js/ValueArray.h"
#include "js/Vector.h"
#include "js/WaitCallbacks.h"
#include "js/WeakMap.h"
#include "js/WrapperCallbacks.h"
#include "js/Zone.h"

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

} /* namespace JS */

/** True iff fun is the global eval function. */
extern JS_PUBLIC_API bool JS_IsBuiltinEvalFunction(JSFunction* fun);

/** True iff fun is the Function constructor. */
extern JS_PUBLIC_API bool JS_IsBuiltinFunctionConstructor(JSFunction* fun);

extern JS_PUBLIC_API const char* JS_GetImplementationVersion(void);

extern JS_PUBLIC_API void JS_SetWrapObjectCallbacks(
    JSContext* cx, const JSWrapObjectCallbacks* callbacks);

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

extern JS_PUBLIC_API JSObject* JS_GlobalLexicalEnvironment(JSObject* obj);

extern JS_PUBLIC_API bool JS_HasExtensibleLexicalEnvironment(JSObject* obj);

extern JS_PUBLIC_API JSObject* JS_ExtensibleLexicalEnvironment(JSObject* obj);

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
                                                     const CallArgs& args,
                                                     JSType* result);

} /* namespace JS */

/**
 * Defines a builtin constructor and prototype. Returns the prototype object.
 *
 * - Defines a property named `name` on `obj`, with its value set to a
 *   newly-created JS function that invokes the `constructor` JSNative. The
 *   `length` of the function is `nargs`.
 *
 * - Creates a prototype object with proto `protoProto` and class `protoClass`.
 *   If `protoProto` is `nullptr`, `Object.prototype` will be used instead.
 *   If `protoClass` is `nullptr`, the prototype object will be a plain JS
 *   object.
 *
 * - The `ps` and `fs` properties/functions will be defined on the prototype
 *   object.
 *
 * - The `static_ps` and `static_fs` properties/functions will be defined on the
 *   constructor.
 */
extern JS_PUBLIC_API JSObject* JS_InitClass(
    JSContext* cx, JS::HandleObject obj, const JSClass* protoClass,
    JS::HandleObject protoProto, const char* name, JSNative constructor,
    unsigned nargs, const JSPropertySpec* ps, const JSFunctionSpec* fs,
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

}  // namespace JS

extern JS_PUBLIC_API JSObject* JS_GetConstructor(JSContext* cx,
                                                 JS::Handle<JSObject*> proto);

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
 * Equivalent to `Object.assign(target, src)`: Copies the properties from the
 * `src` object (which must not be null) to `target` (which also must not be
 * null).
 */
extern JS_PUBLIC_API bool JS_AssignObject(JSContext* cx,
                                          JS::HandleObject target,
                                          JS::HandleObject src);

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
 *
 * The returned string lives as long as fun, so you don't need to root a saved
 * reference to it if fun is well-connected or rooted, and provided you bound
 * the use of the saved reference by fun's lifetime.
 *
 * This function returns false if any error happens while generating the
 * function name string for a function with lazy name.
 */
extern JS_PUBLIC_API bool JS_GetFunctionId(JSContext* cx,
                                           JS::Handle<JSFunction*> fun,
                                           JS::MutableHandle<JSString*> name);

/**
 * Almost same as JS_GetFunctionId.
 *
 * If the function has lazy name, this returns partial name, such as the
 * function name without "get " or "set " prefix.
 */
extern JS_PUBLIC_API JSString* JS_GetMaybePartialFunctionId(JSFunction* fun);

/**
 * Return a function's display name as `name` out-parameter.
 *
 * This is the defined name if one was given where the function was defined, or
 * it could be an inferred name by the JS engine in the case that the function
 * was defined to be anonymous.
 *
 * This can still return nullptr as `name` out-parameter if a useful display
 * name could not be inferred.
 *
 * This function returns false if any error happens while generating the
 * function name string for a function with lazy name.
 */
extern JS_PUBLIC_API bool JS_GetFunctionDisplayId(
    JSContext* cx, JS::Handle<JSFunction*> fun,
    JS::MutableHandle<JSString*> name);

/**
 * Almost same as JS_GetFunctionDisplayId.
 *
 * If the function has lazy name, this returns partial name, such as the
 * function name without "get " or "set " prefix.
 */
extern JS_PUBLIC_API JSString* JS_GetMaybePartialFunctionDisplayId(JSFunction*);

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

extern JS_PUBLIC_API bool JS_ObjectIsBoundFunction(JSObject* obj);

extern JS_PUBLIC_API JSObject* JS_GetBoundFunctionTarget(JSObject* obj);

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
  Register(BASE_REG_FOR_LOCALS, "base-reg-for-locals") \
  Register(INLINING_BYTECODE_MAX_LENGTH, "inlining.bytecode-max-length") \
  Register(BASELINE_INTERPRETER_ENABLE, "blinterp.enable") \
  Register(BASELINE_ENABLE, "baseline.enable") \
  Register(PORTABLE_BASELINE_ENABLE, "pbl.enable") \
  Register(PORTABLE_BASELINE_WARMUP_THRESHOLD, "pbl.warmup.threshold") \
  Register(OFFTHREAD_COMPILATION_ENABLE, "offthread-compilation.enable")  \
  Register(FULL_DEBUG_CHECKS, "jit.full-debug-checks") \
  Register(JUMP_THRESHOLD, "jump-threshold") \
  Register(NATIVE_REGEXP_ENABLE, "native_regexp.enable") \
  Register(JIT_HINTS_ENABLE, "jitHints.enable") \
  Register(SIMULATOR_ALWAYS_INTERRUPT, "simulator.always-interrupt")      \
  Register(SPECTRE_INDEX_MASKING, "spectre.index-masking") \
  Register(SPECTRE_OBJECT_MITIGATIONS, "spectre.object-mitigations") \
  Register(SPECTRE_STRING_MITIGATIONS, "spectre.string-mitigations") \
  Register(SPECTRE_VALUE_MASKING, "spectre.value-masking") \
  Register(SPECTRE_JIT_TO_CXX_CALLS, "spectre.jit-to-cxx-calls") \
  Register(WRITE_PROTECT_CODE, "write-protect-code") \
  Register(WASM_FOLD_OFFSETS, "wasm.fold-offsets") \
  Register(WASM_DELAY_TIER2, "wasm.delay-tier2") \
  Register(WASM_JIT_BASELINE, "wasm.baseline") \
  Register(WASM_JIT_OPTIMIZING, "wasm.optimizing") \
  Register(REGEXP_DUPLICATE_NAMED_GROUPS, "regexp.duplicate-named-groups")  // clang-format on

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

namespace JS {

// Disable all Spectre mitigations for this process after creating the initial
// JSContext. Must be called on this context's thread.
extern JS_PUBLIC_API void DisableSpectreMitigationsAfterInit();

};  // namespace JS

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
    JSContext* cx, AutoFilename* filename = nullptr, uint32_t* lineno = nullptr,
    JS::ColumnNumberOneOrigin* column = nullptr);

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

/**
 * Attempt to disable Wasm's usage of reserving a large virtual memory
 * allocation to avoid bounds checking overhead. This must be called before any
 * Wasm module or memory is created in this process, or else this function will
 * fail.
 */
[[nodiscard]] extern JS_PUBLIC_API bool DisableWasmHugeMemory();

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

#ifdef DEBUG
namespace JS {

extern JS_PUBLIC_API void SetSupportDifferentialTesting(bool value);

}
#endif /* DEBUG */

#endif /* jsapi_h */
