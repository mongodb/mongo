/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsfriendapi_h
#define jsfriendapi_h

#include "jspubtd.h"

#include "js/CallArgs.h"
#include "js/Class.h"
#include "js/ColumnNumber.h"  // JS::LimitedColumnNumberOneOrigin
#include "js/GCAPI.h"
#include "js/HeapAPI.h"
#include "js/Object.h"           // JS::GetClass
#include "js/shadow/Function.h"  // JS::shadow::Function
#include "js/shadow/Object.h"    // JS::shadow::Object
#include "js/TypeDecls.h"

class JSJitInfo;

/*
 * Set a callback used to trace gray roots.
 *
 * The callback is called after the first slice of GC so the embedding must
 * implement appropriate barriers on its gray roots to ensure correctness.
 *
 * This callback may be called multiple times for different sets of zones. Use
 * JS::ZoneIsGrayMarking() to determine whether roots from a particular zone are
 * required.
 */
extern JS_PUBLIC_API void JS_SetGrayGCRootsTracer(JSContext* cx,
                                                  JSGrayRootsTracer traceOp,
                                                  void* data);

extern JS_PUBLIC_API JSObject* JS_FindCompilationScope(JSContext* cx,
                                                       JS::HandleObject obj);

extern JS_PUBLIC_API JSFunction* JS_GetObjectFunction(JSObject* obj);

/**
 * Allocate an object in exactly the same way as JS_NewObjectWithGivenProto, but
 * without invoking the metadata callback on it.  This allows creation of
 * internal bookkeeping objects that are guaranteed to not have metadata
 * attached to them.
 */
extern JS_PUBLIC_API JSObject* JS_NewObjectWithoutMetadata(
    JSContext* cx, const JSClass* clasp, JS::Handle<JSObject*> proto);

extern JS_PUBLIC_API bool JS_NondeterministicGetWeakMapKeys(
    JSContext* cx, JS::HandleObject obj, JS::MutableHandleObject ret);

extern JS_PUBLIC_API bool JS_NondeterministicGetWeakSetKeys(
    JSContext* cx, JS::HandleObject obj, JS::MutableHandleObject ret);

// Raw JSScript* because this needs to be callable from a signal handler.
extern JS_PUBLIC_API unsigned JS_PCToLineNumber(
    JSScript* script, jsbytecode* pc,
    JS::LimitedColumnNumberOneOrigin* columnp = nullptr);

/**
 * Determine whether the given object is backed by a DeadObjectProxy.
 *
 * Such objects hold no other objects (they have no outgoing reference edges)
 * and will throw if you touch them (e.g. by reading/writing a property).
 */
extern JS_PUBLIC_API bool JS_IsDeadWrapper(JSObject* obj);

/**
 * Creates a new dead wrapper object in the given scope. To be used when
 * attempting to wrap objects from scopes which are already dead.
 *
 * If origObject is passed, it must be an proxy object, and will be
 * used to determine the characteristics of the new dead wrapper.
 */
extern JS_PUBLIC_API JSObject* JS_NewDeadWrapper(
    JSContext* cx, JSObject* origObject = nullptr);

namespace js {

/**
 * Get the script private value associated with an object, if any.
 *
 * The private value is set with SetScriptPrivate() or SetModulePrivate() and is
 * internally stored on the relevant ScriptSourceObject.
 *
 * This is used by the cycle collector to trace through
 * ScriptSourceObjects. This allows private values to contain an nsISupports
 * pointer and hence support references to cycle collected C++ objects.
 */
JS_PUBLIC_API JS::Value MaybeGetScriptPrivate(JSObject* object);

}  // namespace js

/*
 * Used by the cycle collector to trace through a shape or object group and
 * all cycle-participating data it reaches, using bounded stack space.
 */
extern JS_PUBLIC_API void JS_TraceShapeCycleCollectorChildren(
    JS::CallbackTracer* trc, JS::GCCellPtr shape);
extern JS_PUBLIC_API void JS_TraceObjectGroupCycleCollectorChildren(
    JS::CallbackTracer* trc, JS::GCCellPtr group);

extern JS_PUBLIC_API JSPrincipals* JS_GetScriptPrincipals(JSScript* script);

extern JS_PUBLIC_API bool JS_ScriptHasMutedErrors(JSScript* script);

extern JS_PUBLIC_API JSObject* JS_CloneObject(JSContext* cx,
                                              JS::HandleObject obj,
                                              JS::HandleObject proto);

/**
 * Copy the own properties of src to dst in a fast way.  src and dst must both
 * be native and must be in the compartment of cx.  They must have the same
 * class, the same parent, and the same prototype.  Class reserved slots will
 * NOT be copied.
 *
 * dst must not have any properties on it before this function is called.
 *
 * src must have been allocated via JS_NewObjectWithoutMetadata so that we can
 * be sure it has no metadata that needs copying to dst.  This also means that
 * dst needs to have the compartment global as its parent.  This function will
 * preserve the existing metadata on dst, if any.
 */
extern JS_PUBLIC_API bool JS_InitializePropertiesFromCompatibleNativeObject(
    JSContext* cx, JS::HandleObject dst, JS::HandleObject src);

namespace js {

JS_PUBLIC_API bool IsArgumentsObject(JS::HandleObject obj);

JS_PUBLIC_API bool AddRawValueRoot(JSContext* cx, JS::Value* vp,
                                   const char* name);

JS_PUBLIC_API void RemoveRawValueRoot(JSContext* cx, JS::Value* vp);

}  // namespace js

namespace JS {

/**
 * Set all of the uninitialized lexicals on an object to undefined. Return
 * true if any lexicals were initialized and false otherwise.
 * */
extern JS_PUBLIC_API bool ForceLexicalInitialization(JSContext* cx,
                                                     HandleObject obj);

/**
 * Whether we are poisoning unused/released data for error detection. Governed
 * by the JS_GC_ALLOW_EXTRA_POISONING #ifdef as well as the
 * javascript.options.extra_gc_poisoning pref.
 */
extern JS_PUBLIC_API bool IsGCPoisoning();

extern JS_PUBLIC_API JSPrincipals* GetRealmPrincipals(JS::Realm* realm);

extern JS_PUBLIC_API void SetRealmPrincipals(JS::Realm* realm,
                                             JSPrincipals* principals);

extern JS_PUBLIC_API bool GetIsSecureContext(JS::Realm* realm);

extern JS_PUBLIC_API bool GetDebuggerObservesWasm(JS::Realm* realm);

}  // namespace JS

/**
 * Copies all own properties and private fields from |obj| to |target|. Both
 * |obj| and |target| must not be cross-compartment wrappers because we have to
 * enter their realms.
 *
 * This function immediately enters a realm, and does not impose any
 * restrictions on the realm of |cx|.
 */
extern JS_PUBLIC_API bool JS_CopyOwnPropertiesAndPrivateFields(
    JSContext* cx, JS::HandleObject target, JS::HandleObject obj);

extern JS_PUBLIC_API bool JS_WrapPropertyDescriptor(
    JSContext* cx, JS::MutableHandle<JS::PropertyDescriptor> desc);

extern JS_PUBLIC_API bool JS_WrapPropertyDescriptor(
    JSContext* cx,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc);

struct JSFunctionSpecWithHelp {
  const char* name;
  JSNative call;
  uint16_t nargs;
  uint16_t flags;
  const JSJitInfo* jitInfo;
  const char* usage;
  const char* help;
};

#define JS_FN_HELP(name, call, nargs, flags, usage, help) \
  {name, call, nargs, (flags) | JSPROP_ENUMERATE, nullptr, usage, help}
#define JS_INLINABLE_FN_HELP(name, call, nargs, flags, native, usage, help)    \
  {name,  call, nargs, (flags) | JSPROP_ENUMERATE, &js::jit::JitInfo_##native, \
   usage, help}
#define JS_FS_HELP_END {nullptr, nullptr, 0, 0, nullptr, nullptr}

extern JS_PUBLIC_API bool JS_DefineFunctionsWithHelp(
    JSContext* cx, JS::HandleObject obj, const JSFunctionSpecWithHelp* fs);

namespace js {

/**
 * Use the runtime's internal handling of job queues for Promise jobs.
 *
 * Most embeddings, notably web browsers, will have their own task scheduling
 * systems and need to integrate handling of Promise jobs into that, so they
 * will want to manage job queues themselves. For basic embeddings such as the
 * JS shell that don't have an event loop of their own, it's easier to have
 * SpiderMonkey handle job queues internally.
 *
 * Note that the embedding still has to trigger processing of job queues at
 * right time(s), such as after evaluation of a script has run to completion.
 */
extern JS_PUBLIC_API bool UseInternalJobQueues(JSContext* cx);

#ifdef DEBUG
/**
 * Given internal job queues are used, return currently queued jobs as an
 * array of job objects.
 */
extern JS_PUBLIC_API JSObject* GetJobsInInternalJobQueue(JSContext* cx);
#endif

/**
 * Enqueue |job| on the internal job queue.
 *
 * This is useful in tests for creating situations where a call occurs with no
 * other JavaScript on the stack.
 */
extern JS_PUBLIC_API bool EnqueueJob(JSContext* cx, JS::HandleObject job);

/**
 * Instruct the runtime to stop draining the internal job queue.
 *
 * Useful if the embedding is in the process of quitting in reaction to a
 * builtin being called, or if it wants to resume executing jobs later on.
 */
extern JS_PUBLIC_API void StopDrainingJobQueue(JSContext* cx);

/**
 * Instruct the runtime to restart draining the internal job queue after
 * stopping it with StopDrainingJobQueue.
 */
extern JS_PUBLIC_API void RestartDrainingJobQueue(JSContext* cx);

extern JS_PUBLIC_API void RunJobs(JSContext* cx);

extern JS_PUBLIC_API JS::Zone* GetRealmZone(JS::Realm* realm);

using PreserveWrapperCallback = bool (*)(JSContext*, JS::HandleObject);
using HasReleasedWrapperCallback = bool (*)(JS::HandleObject);

extern JS_PUBLIC_API bool IsSystemRealm(JS::Realm* realm);

extern JS_PUBLIC_API bool IsSystemCompartment(JS::Compartment* comp);

extern JS_PUBLIC_API bool IsSystemZone(JS::Zone* zone);

struct WeakMapTracer {
  JSRuntime* runtime;

  explicit WeakMapTracer(JSRuntime* rt) : runtime(rt) {}

  // Weak map tracer callback, called once for every binding of every
  // weak map that was live at the time of the last garbage collection.
  //
  // m will be nullptr if the weak map is not contained in a JS Object.
  //
  // The callback should not GC (and will assert in a debug build if it does
  // so.)
  virtual void trace(JSObject* m, JS::GCCellPtr key, JS::GCCellPtr value) = 0;
};

extern JS_PUBLIC_API void TraceWeakMaps(WeakMapTracer* trc);

extern JS_PUBLIC_API bool AreGCGrayBitsValid(JSRuntime* rt);

extern JS_PUBLIC_API bool ZoneGlobalsAreAllGray(JS::Zone* zone);

extern JS_PUBLIC_API bool IsCompartmentZoneSweepingOrCompacting(
    JS::Compartment* comp);

using IterateGCThingCallback = void (*)(void*, JS::GCCellPtr,
                                        const JS::AutoRequireNoGC&);

extern JS_PUBLIC_API void TraceGrayWrapperTargets(JSTracer* trc,
                                                  JS::Zone* zone);

/**
 * Invoke cellCallback on every gray JSObject in the given zone.
 */
extern JS_PUBLIC_API void IterateGrayObjects(
    JS::Zone* zone, IterateGCThingCallback cellCallback, void* data);

#if defined(JS_GC_ZEAL) || defined(DEBUG)
// Trace the heap and check there are no black to gray edges. These are
// not allowed since the cycle collector could throw away the gray thing and
// leave a dangling pointer.
//
// This doesn't trace weak maps as these are handled separately.
extern JS_PUBLIC_API bool CheckGrayMarkingState(JSRuntime* rt);
#endif

// Note: this returns nullptr iff |zone| is the atoms zone.
extern JS_PUBLIC_API JS::Realm* GetAnyRealmInZone(JS::Zone* zone);

// Returns the first realm's global in a compartment. Note: this is not
// guaranteed to always be the same realm because individual realms can be
// collected by the GC.
extern JS_PUBLIC_API JSObject* GetFirstGlobalInCompartment(
    JS::Compartment* comp);

// Returns true if the compartment contains a global object and this global is
// not being collected.
extern JS_PUBLIC_API bool CompartmentHasLiveGlobal(JS::Compartment* comp);

// Returns true if this compartment can be shared across multiple Realms.  Used
// when we're looking for an existing compartment to place a new Realm in.
extern JS_PUBLIC_API bool IsSharableCompartment(JS::Compartment* comp);

// This is equal to |&JSObject::class_|.  Use it in places where you don't want
// to #include vm/JSObject.h.
extern JS_PUBLIC_DATA const JSClass* const ObjectClassPtr;

JS_PUBLIC_API const JSClass* ProtoKeyToClass(JSProtoKey key);

// Returns the key for the class inherited by a given standard class (that
// is to say, the prototype of this standard class's prototype).
//
// You must be sure that this corresponds to a standard class with a cached
// JSProtoKey before calling this function. In general |key| will match the
// cached proto key, except in cases where multiple JSProtoKeys share a
// JSClass.
inline JSProtoKey InheritanceProtoKeyForStandardClass(JSProtoKey key) {
  // [Object] has nothing to inherit from.
  if (key == JSProto_Object) {
    return JSProto_Null;
  }

  // If we're ClassSpec defined return the proto key from that
  if (ProtoKeyToClass(key)->specDefined()) {
    return ProtoKeyToClass(key)->specInheritanceProtoKey();
  }

  // Otherwise, we inherit [Object].
  return JSProto_Object;
}

JS_PUBLIC_API bool ShouldIgnorePropertyDefinition(JSContext* cx, JSProtoKey key,
                                                  jsid id);

JS_PUBLIC_API bool IsFunctionObject(JSObject* obj);

JS_PUBLIC_API bool UninlinedIsCrossCompartmentWrapper(const JSObject* obj);

// CrossCompartmentWrappers are shared by all realms within the compartment, so
// getting a wrapper's realm usually doesn't make sense.
static MOZ_ALWAYS_INLINE JS::Realm* GetNonCCWObjectRealm(JSObject* obj) {
  MOZ_ASSERT(!js::UninlinedIsCrossCompartmentWrapper(obj));
  return reinterpret_cast<JS::shadow::Object*>(obj)->shape->base->realm;
}

JS_PUBLIC_API void AssertSameCompartment(JSContext* cx, JSObject* obj);

JS_PUBLIC_API void AssertSameCompartment(JSContext* cx, JS::HandleValue v);

#ifdef JS_DEBUG
JS_PUBLIC_API void AssertSameCompartment(JSObject* objA, JSObject* objB);
#else
inline void AssertSameCompartment(JSObject* objA, JSObject* objB) {}
#endif

JS_PUBLIC_API void NotifyAnimationActivity(JSObject* obj);

JS_PUBLIC_API JSFunction* DefineFunctionWithReserved(
    JSContext* cx, JSObject* obj, const char* name, JSNative call,
    unsigned nargs, unsigned attrs);

JS_PUBLIC_API JSFunction* NewFunctionWithReserved(JSContext* cx, JSNative call,
                                                  unsigned nargs,
                                                  unsigned flags,
                                                  const char* name);

JS_PUBLIC_API JSFunction* NewFunctionByIdWithReserved(JSContext* cx,
                                                      JSNative native,
                                                      unsigned nargs,
                                                      unsigned flags, jsid id);

JS_PUBLIC_API JSFunction* NewFunctionByIdWithReservedAndProto(
    JSContext* cx, JSNative native, JS::Handle<JSObject*> proto, unsigned nargs,
    unsigned flags, jsid id);

/**
 * Get or set function's reserved slot value.
 * `fun` should be a function created with `*WithReserved` API above.
 * Such functions have 2 reserved slots, and `which` can be either 0 or 1.
 */
JS_PUBLIC_API const JS::Value& GetFunctionNativeReserved(JSObject* fun,
                                                         size_t which);

JS_PUBLIC_API void SetFunctionNativeReserved(JSObject* fun, size_t which,
                                             const JS::Value& val);

JS_PUBLIC_API bool FunctionHasNativeReserved(JSObject* fun);

JS_PUBLIC_API bool GetObjectProto(JSContext* cx, JS::HandleObject obj,
                                  JS::MutableHandleObject proto);

extern JS_PUBLIC_API JSObject* GetStaticPrototype(JSObject* obj);

JS_PUBLIC_API bool GetRealmOriginalEval(JSContext* cx,
                                        JS::MutableHandleObject eval);

/**
 * Add some or all property keys of obj to the id vector *props.
 *
 * The flags parameter controls which property keys are added. Pass a
 * combination of the following bits:
 *
 *     JSITER_OWNONLY - Don't also search the prototype chain; only consider
 *       obj's own properties.
 *
 *     JSITER_HIDDEN - Include nonenumerable properties.
 *
 *     JSITER_SYMBOLS - Include property keys that are symbols. The default
 *       behavior is to filter out symbols.
 *
 *     JSITER_SYMBOLSONLY - Exclude non-symbol property keys.
 *
 * This is the closest C++ API we have to `Reflect.ownKeys(obj)`, or
 * equivalently, the ES6 [[OwnPropertyKeys]] internal method. Pass
 * `JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS` as flags to get
 * results that match the output of Reflect.ownKeys.
 */
JS_PUBLIC_API bool GetPropertyKeys(JSContext* cx, JS::HandleObject obj,
                                   unsigned flags,
                                   JS::MutableHandleIdVector props);

JS_PUBLIC_API bool AppendUnique(JSContext* cx, JS::MutableHandleIdVector base,
                                JS::HandleIdVector others);

/**
 * Determine whether the given string is an array index in the sense of
 * <https://tc39.github.io/ecma262/#array-index>.
 *
 * If it isn't, returns false.
 *
 * If it is, returns true and outputs the index in *indexp.
 */
JS_PUBLIC_API bool StringIsArrayIndex(const JSLinearString* str,
                                      uint32_t* indexp);

/**
 * Overload of StringIsArrayIndex taking a (char16_t*,length) pair. Behaves
 * the same as the JSLinearString version.
 */
JS_PUBLIC_API bool StringIsArrayIndex(const char16_t* str, uint32_t length,
                                      uint32_t* indexp);

JS_PUBLIC_API void SetPreserveWrapperCallbacks(
    JSContext* cx, PreserveWrapperCallback preserveWrapper,
    HasReleasedWrapperCallback hasReleasedWrapper);

JS_PUBLIC_API bool IsObjectInContextCompartment(JSObject* obj,
                                                const JSContext* cx);

/*
 * NB: keep these in sync with the copy in builtin/SelfHostingDefines.h.
 */
/* 0x1 is no longer used */
/* 0x2 is no longer used */
#define JSITER_PRIVATE 0x4      /* Include private names in iteration */
#define JSITER_OWNONLY 0x8      /* iterate over obj's own properties only */
#define JSITER_HIDDEN 0x10      /* also enumerate non-enumerable properties */
#define JSITER_SYMBOLS 0x20     /* also include symbol property keys */
#define JSITER_SYMBOLSONLY 0x40 /* exclude string property keys */
#define JSITER_FORAWAITOF 0x80  /* for-await-of */

using DOMInstanceClassHasProtoAtDepth = bool (*)(const JSClass*, uint32_t,
                                                 uint32_t);
using DOMInstanceClassIsError = bool (*)(const JSClass*);

struct JSDOMCallbacks {
  DOMInstanceClassHasProtoAtDepth instanceClassMatchesProto;
  DOMInstanceClassIsError instanceClassIsError;
};
using DOMCallbacks = struct JSDOMCallbacks;

extern JS_PUBLIC_API void SetDOMCallbacks(JSContext* cx,
                                          const DOMCallbacks* callbacks);

extern JS_PUBLIC_API const DOMCallbacks* GetDOMCallbacks(JSContext* cx);

extern JS_PUBLIC_API JSObject* GetTestingFunctions(JSContext* cx);

/* Implemented in jsexn.cpp. */

/**
 * Get an error type name from a JSExnType constant.
 * Returns nullptr for invalid arguments and JSEXN_INTERNALERR
 */
extern JS_PUBLIC_API JSLinearString* GetErrorTypeName(JSContext* cx,
                                                      int16_t exnType);

/* Implemented in CrossCompartmentWrapper.cpp. */
enum NukeReferencesToWindow { NukeWindowReferences, DontNukeWindowReferences };

enum NukeReferencesFromTarget {
  NukeAllReferences,
  NukeIncomingReferences,
};

/*
 * These filters are designed to be ephemeral stack classes, and thus don't
 * do any rooting or holding of their members.
 */
struct CompartmentFilter {
  virtual bool match(JS::Compartment* c) const = 0;
};

struct AllCompartments : public CompartmentFilter {
  virtual bool match(JS::Compartment* c) const override { return true; }
};

struct SingleCompartment : public CompartmentFilter {
  JS::Compartment* ours;
  explicit SingleCompartment(JS::Compartment* c) : ours(c) {}
  virtual bool match(JS::Compartment* c) const override { return c == ours; }
};

extern JS_PUBLIC_API bool NukeCrossCompartmentWrappers(
    JSContext* cx, const CompartmentFilter& sourceFilter, JS::Realm* target,
    NukeReferencesToWindow nukeReferencesToWindow,
    NukeReferencesFromTarget nukeReferencesFromTarget);

extern JS_PUBLIC_API bool AllowNewWrapper(JS::Compartment* target,
                                          JSObject* obj);

extern JS_PUBLIC_API bool NukedObjectRealm(JSObject* obj);

/* Implemented in jsdate.cpp. */

/** Detect whether the internal date value is NaN. */
extern JS_PUBLIC_API bool DateIsValid(JSContext* cx, JS::HandleObject obj,
                                      bool* isValid);

extern JS_PUBLIC_API bool DateGetMsecSinceEpoch(JSContext* cx,
                                                JS::HandleObject obj,
                                                double* msecSinceEpoch);

} /* namespace js */

namespace js {

/* Implemented in vm/StructuredClone.cpp. */
extern JS_PUBLIC_API uint64_t GetSCOffset(JSStructuredCloneWriter* writer);

}  // namespace js

namespace js {

/* Statically asserted in FunctionFlags.cpp. */
static const unsigned JS_FUNCTION_INTERPRETED_BITS = 0x0060;

}  // namespace js

static MOZ_ALWAYS_INLINE const JSJitInfo* FUNCTION_VALUE_TO_JITINFO(
    const JS::Value& v) {
  JSObject* obj = &v.toObject();
  MOZ_ASSERT(JS::GetClass(obj)->isJSFunction());

  auto* fun = reinterpret_cast<JS::shadow::Function*>(obj);
  MOZ_ASSERT(!(fun->flagsAndArgCount() & js::JS_FUNCTION_INTERPRETED_BITS),
             "Unexpected non-native function");

  return static_cast<const JSJitInfo*>(fun->jitInfoOrScript());
}

static MOZ_ALWAYS_INLINE void SET_JITINFO(JSFunction* func,
                                          const JSJitInfo* info) {
  auto* fun = reinterpret_cast<JS::shadow::Function*>(func);
  MOZ_ASSERT(!(fun->flagsAndArgCount() & js::JS_FUNCTION_INTERPRETED_BITS));

  fun->setJitInfoOrScript(const_cast<JSJitInfo*>(info));
}

static_assert(sizeof(jsid) == sizeof(void*));

namespace js {

static MOZ_ALWAYS_INLINE JS::Value IdToValue(jsid id) {
  if (id.isString()) {
    return JS::StringValue(id.toString());
  }
  if (id.isInt()) {
    return JS::Int32Value(id.toInt());
  }
  if (id.isSymbol()) {
    return JS::SymbolValue(id.toSymbol());
  }
  MOZ_ASSERT(id.isVoid());
  return JS::UndefinedValue();
}

/**
 * PrepareScriptEnvironmentAndInvoke asserts the embedder has registered a
 * ScriptEnvironmentPreparer and then it calls the preparer's 'invoke' method
 * with the given |closure|, with the assumption that the preparer will set up
 * any state necessary to run script in |global|, invoke |closure| with a valid
 * JSContext*, report any exceptions thrown from the closure, and return.
 *
 * PrepareScriptEnvironmentAndInvoke will report any exceptions that are thrown
 * by the closure.  Consumers who want to propagate back whether the closure
 * succeeded should do so via members of the closure itself.
 */

struct ScriptEnvironmentPreparer {
  struct Closure {
    virtual bool operator()(JSContext* cx) = 0;
  };

  virtual void invoke(JS::HandleObject global, Closure& closure) = 0;
};

extern JS_PUBLIC_API void PrepareScriptEnvironmentAndInvoke(
    JSContext* cx, JS::HandleObject global,
    ScriptEnvironmentPreparer::Closure& closure);

JS_PUBLIC_API void SetScriptEnvironmentPreparer(
    JSContext* cx, ScriptEnvironmentPreparer* preparer);

// Abstract base class for objects that build allocation metadata for JavaScript
// values.
struct AllocationMetadataBuilder {
  AllocationMetadataBuilder() = default;

  // Return a metadata object for the newly constructed object |obj|, or
  // nullptr if there's no metadata to attach.
  //
  // Implementations should treat all errors as fatal; there is no way to
  // report errors from this callback. In particular, the caller provides an
  // oomUnsafe for overriding implementations to use.
  virtual JSObject* build(JSContext* cx, JS::HandleObject obj,
                          AutoEnterOOMUnsafeRegion& oomUnsafe) const {
    return nullptr;
  }
};

/**
 * Specify a callback to invoke when creating each JS object in the current
 * compartment, which may return a metadata object to associate with the
 * object.
 */
JS_PUBLIC_API void SetAllocationMetadataBuilder(
    JSContext* cx, const AllocationMetadataBuilder* callback);

/** Get the metadata associated with an object. */
JS_PUBLIC_API JSObject* GetAllocationMetadata(JSObject* obj);

JS_PUBLIC_API bool GetElementsWithAdder(JSContext* cx, JS::HandleObject obj,
                                        JS::HandleObject receiver,
                                        uint32_t begin, uint32_t end,
                                        js::ElementAdder* adder);

JS_PUBLIC_API bool ForwardToNative(JSContext* cx, JSNative native,
                                   const JS::CallArgs& args);

/**
 * Helper function for HTMLDocument and HTMLFormElement.
 *
 * These are the only two interfaces that have [OverrideBuiltins], a named
 * getter, and no named setter. They're implemented as proxies with a custom
 * getOwnPropertyDescriptor() method. Unfortunately, overriding
 * getOwnPropertyDescriptor() automatically affects the behavior of set(),
 * which normally is just common sense but is *not* desired for these two
 * interfaces.
 *
 * The fix is for these two interfaces to override set() to ignore the
 * getOwnPropertyDescriptor() override.
 *
 * SetPropertyIgnoringNamedGetter is exposed to make it easier to override
 * set() in this way.  It carries out all the steps of BaseProxyHandler::set()
 * except the initial getOwnPropertyDescriptor() call.  The caller must supply
 * that descriptor as the 'ownDesc' parameter.
 *
 * Implemented in proxy/BaseProxyHandler.cpp.
 */
JS_PUBLIC_API bool SetPropertyIgnoringNamedGetter(
    JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleValue v,
    JS::HandleValue receiver,
    JS::Handle<mozilla::Maybe<JS::PropertyDescriptor>> ownDesc,
    JS::ObjectOpResult& result);

// This function is for one specific use case, please don't use this for
// anything else!
extern JS_PUBLIC_API bool ExecuteInFrameScriptEnvironment(
    JSContext* cx, JS::HandleObject obj, JS::HandleScript script,
    JS::MutableHandleObject scope);

extern JS_PUBLIC_API bool IsSavedFrame(JSObject* obj);

// Matches the condition in js/src/jit/ProcessExecutableMemory.cpp
#if defined(XP_WIN)
// Parameters use void* types to avoid #including windows.h. The return value of
// this function is returned from the exception handler.
using JitExceptionHandler = long (*)(void* exceptionRecord,  // PEXECTION_RECORD
                                     void* context);         // PCONTEXT

/**
 * Windows uses "structured exception handling" to handle faults. When a fault
 * occurs, the stack is searched for a handler (similar to C++ exception
 * handling). If the search does not find a handler, the "unhandled exception
 * filter" is called. Breakpad uses the unhandled exception filter to do crash
 * reporting. Unfortunately, on Win64, JIT code on the stack completely throws
 * off this unwinding process and prevents the unhandled exception filter from
 * being called. The reason is that Win64 requires unwind information be
 * registered for all code regions and JIT code has none. While it is possible
 * to register full unwind information for JIT code, this is a lot of work (one
 * has to be able to recover the frame pointer at any PC) so instead we register
 * a handler for all JIT code that simply calls breakpad's unhandled exception
 * filter (which will perform crash reporting and then terminate the process).
 * This would be wrong if there was an outer __try block that expected to handle
 * the fault, but this is not generally allowed.
 *
 * Gecko must call SetJitExceptionFilter before any JIT code is compiled and
 * only once per process.
 */
extern JS_PUBLIC_API void SetJitExceptionHandler(JitExceptionHandler handler);
#endif

extern JS_PUBLIC_API bool ReportIsNotFunction(JSContext* cx, JS::HandleValue v);

class MOZ_STACK_CLASS JS_PUBLIC_API AutoAssertNoContentJS {
 public:
  explicit AutoAssertNoContentJS(JSContext* cx);
  ~AutoAssertNoContentJS();

 private:
  JSContext* context_;
  bool prevAllowContentJS_;
};

/**
 * This function reports memory used by a zone in bytes, this includes:
 *  * The size of this JS GC zone.
 *  * Malloc memory referred to from this zone.
 *  * JIT memory for this zone.
 *
 * Note that malloc memory referred to from this zone can include
 * SharedArrayBuffers which may also be referred to from other zones. Adding the
 * memory usage of multiple zones may lead to an over-estimate.
 */
extern JS_PUBLIC_API uint64_t GetMemoryUsageForZone(JS::Zone* zone);

enum class MemoryUse : uint8_t;

namespace gc {

struct SharedMemoryUse {
  explicit SharedMemoryUse(MemoryUse use) : count(0), nbytes(0) {
#ifdef DEBUG
    this->use = use;
#endif
  }

  size_t count;
  size_t nbytes;
#ifdef DEBUG
  MemoryUse use;
#endif
};

// A map which tracks shared memory uses (shared in the sense that an allocation
// can be referenced by more than one GC thing in a zone). This allows us to
// only account for the memory once.
using SharedMemoryMap =
    HashMap<void*, SharedMemoryUse, DefaultHasher<void*>, SystemAllocPolicy>;

} /* namespace gc */

extern JS_PUBLIC_API const gc::SharedMemoryMap& GetSharedMemoryUsageForZone(
    JS::Zone* zone);

/**
 * This function only reports GC heap memory,
 * and not malloc allocated memory associated with GC things.
 * It reports the total of all memory for the whole Runtime.
 */
extern JS_PUBLIC_API uint64_t GetGCHeapUsage(JSContext* cx);

class JS_PUBLIC_API CompartmentTransplantCallback {
 public:
  virtual JSObject* getObjectToTransplant(JS::Compartment* compartment) = 0;
};

// Gather a set of remote window proxies by calling the callback on every
// compartment, then transform them into cross-compartment wrappers to newTarget
// via brain transplants. If there's a proxy in newTarget's compartment, it will
// get swapped with newTarget, and the value of newTarget will be updated. If
// the callback returns null for a compartment, no cross-compartment wrapper
// will be created for that compartment. Any non-null values it returns must be
// DOM remote proxies from the compartment that was passed in.
extern JS_PUBLIC_API void RemapRemoteWindowProxies(
    JSContext* cx, CompartmentTransplantCallback* callback,
    JS::MutableHandleObject newTarget);

extern JS_PUBLIC_API JS::Zone* GetObjectZoneFromAnyThread(const JSObject* obj);

} /* namespace js */

#endif /* jsfriendapi_h */
