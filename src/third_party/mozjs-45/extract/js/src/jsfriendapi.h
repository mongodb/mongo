/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsfriendapi_h
#define jsfriendapi_h

#include "mozilla/Atomics.h"
#include "mozilla/Casting.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/UniquePtr.h"

#include "jsapi.h" // For JSAutoByteString.  See bug 1033916.
#include "jsbytecode.h"
#include "jspubtd.h"

#include "js/CallArgs.h"
#include "js/CallNonGenericMethod.h"
#include "js/Class.h"

#if JS_STACK_GROWTH_DIRECTION > 0
# define JS_CHECK_STACK_SIZE(limit, sp) (MOZ_LIKELY((uintptr_t)(sp) < (limit)))
#else
# define JS_CHECK_STACK_SIZE(limit, sp) (MOZ_LIKELY((uintptr_t)(sp) > (limit)))
#endif

class JSAtom;
struct JSErrorFormatString;
class JSLinearString;
struct JSJitInfo;
class JSErrorReport;

namespace JS {
template <class T>
class Heap;
} /* namespace JS */

namespace js {
class JS_FRIEND_API(BaseProxyHandler);
class InterpreterFrame;
} /* namespace js */

extern JS_FRIEND_API(void)
JS_SetGrayGCRootsTracer(JSRuntime* rt, JSTraceDataOp traceOp, void* data);

extern JS_FRIEND_API(JSObject*)
JS_FindCompilationScope(JSContext* cx, JS::HandleObject obj);

extern JS_FRIEND_API(JSFunction*)
JS_GetObjectFunction(JSObject* obj);

extern JS_FRIEND_API(bool)
JS_SplicePrototype(JSContext* cx, JS::HandleObject obj, JS::HandleObject proto);

extern JS_FRIEND_API(JSObject*)
JS_NewObjectWithUniqueType(JSContext* cx, const JSClass* clasp, JS::HandleObject proto);

/**
 * Allocate an object in exactly the same way as JS_NewObjectWithGivenProto, but
 * without invoking the metadata callback on it.  This allows creation of
 * internal bookkeeping objects that are guaranteed to not have metadata
 * attached to them.
 */
extern JS_FRIEND_API(JSObject*)
JS_NewObjectWithoutMetadata(JSContext* cx, const JSClass* clasp, JS::Handle<JSObject*> proto);

extern JS_FRIEND_API(uint32_t)
JS_ObjectCountDynamicSlots(JS::HandleObject obj);

extern JS_FRIEND_API(size_t)
JS_SetProtoCalled(JSContext* cx);

extern JS_FRIEND_API(bool)
JS_ImmutablePrototypesEnabled();

extern JS_FRIEND_API(size_t)
JS_GetCustomIteratorCount(JSContext* cx);

extern JS_FRIEND_API(bool)
JS_NondeterministicGetWeakMapKeys(JSContext* cx, JS::HandleObject obj, JS::MutableHandleObject ret);

extern JS_FRIEND_API(bool)
JS_NondeterministicGetWeakSetKeys(JSContext* cx, JS::HandleObject obj, JS::MutableHandleObject ret);

// Raw JSScript* because this needs to be callable from a signal handler.
extern JS_FRIEND_API(unsigned)
JS_PCToLineNumber(JSScript* script, jsbytecode* pc, unsigned* columnp = nullptr);

/**
 * Determine whether the given object is backed by a DeadObjectProxy.
 *
 * Such objects hold no other objects (they have no outgoing reference edges)
 * and will throw if you touch them (e.g. by reading/writing a property).
 */
extern JS_FRIEND_API(bool)
JS_IsDeadWrapper(JSObject* obj);

/*
 * Used by the cycle collector to trace through a shape or object group and
 * all cycle-participating data it reaches, using bounded stack space.
 */
extern JS_FRIEND_API(void)
JS_TraceShapeCycleCollectorChildren(JS::CallbackTracer* trc, JS::GCCellPtr shape);
extern JS_FRIEND_API(void)
JS_TraceObjectGroupCycleCollectorChildren(JS::CallbackTracer* trc, JS::GCCellPtr group);

enum {
    JS_TELEMETRY_GC_REASON,
    JS_TELEMETRY_GC_IS_COMPARTMENTAL,
    JS_TELEMETRY_GC_MS,
    JS_TELEMETRY_GC_BUDGET_MS,
    JS_TELEMETRY_GC_ANIMATION_MS,
    JS_TELEMETRY_GC_MAX_PAUSE_MS,
    JS_TELEMETRY_GC_MARK_MS,
    JS_TELEMETRY_GC_SWEEP_MS,
    JS_TELEMETRY_GC_MARK_ROOTS_MS,
    JS_TELEMETRY_GC_MARK_GRAY_MS,
    JS_TELEMETRY_GC_SLICE_MS,
    JS_TELEMETRY_GC_SLOW_PHASE,
    JS_TELEMETRY_GC_MMU_50,
    JS_TELEMETRY_GC_RESET,
    JS_TELEMETRY_GC_INCREMENTAL_DISABLED,
    JS_TELEMETRY_GC_NON_INCREMENTAL,
    JS_TELEMETRY_GC_SCC_SWEEP_TOTAL_MS,
    JS_TELEMETRY_GC_SCC_SWEEP_MAX_PAUSE_MS,
    JS_TELEMETRY_GC_MINOR_REASON,
    JS_TELEMETRY_GC_MINOR_REASON_LONG,
    JS_TELEMETRY_GC_MINOR_US,
    JS_TELEMETRY_DEPRECATED_LANGUAGE_EXTENSIONS_IN_CONTENT,
    JS_TELEMETRY_DEPRECATED_LANGUAGE_EXTENSIONS_IN_ADDONS,
    JS_TELEMETRY_ADDON_EXCEPTIONS
};

typedef void
(*JSAccumulateTelemetryDataCallback)(int id, uint32_t sample, const char* key);

extern JS_FRIEND_API(void)
JS_SetAccumulateTelemetryCallback(JSRuntime* rt, JSAccumulateTelemetryDataCallback callback);

extern JS_FRIEND_API(JSPrincipals*)
JS_GetCompartmentPrincipals(JSCompartment* compartment);

extern JS_FRIEND_API(void)
JS_SetCompartmentPrincipals(JSCompartment* compartment, JSPrincipals* principals);

extern JS_FRIEND_API(JSPrincipals*)
JS_GetScriptPrincipals(JSScript* script);

extern JS_FRIEND_API(bool)
JS_ScriptHasMutedErrors(JSScript* script);

extern JS_FRIEND_API(JSObject*)
JS_CloneObject(JSContext* cx, JS::HandleObject obj, JS::HandleObject proto);

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
extern JS_FRIEND_API(bool)
JS_InitializePropertiesFromCompatibleNativeObject(JSContext* cx,
                                                  JS::HandleObject dst,
                                                  JS::HandleObject src);

extern JS_FRIEND_API(JSString*)
JS_BasicObjectToString(JSContext* cx, JS::HandleObject obj);

namespace js {

JS_FRIEND_API(bool)
GetBuiltinClass(JSContext* cx, JS::HandleObject obj, ESClassValue* classValue);

JS_FRIEND_API(const char*)
ObjectClassName(JSContext* cx, JS::HandleObject obj);

JS_FRIEND_API(void)
ReportOverRecursed(JSContext* maybecx);

JS_FRIEND_API(bool)
AddRawValueRoot(JSContext* cx, JS::Value* vp, const char* name);

JS_FRIEND_API(void)
RemoveRawValueRoot(JSContext* cx, JS::Value* vp);

JS_FRIEND_API(JSAtom*)
GetPropertyNameFromPC(JSScript* script, jsbytecode* pc);

#ifdef JS_DEBUG

/*
 * Routines to print out values during debugging.  These are FRIEND_API to help
 * the debugger find them and to support temporarily hacking js::Dump* calls
 * into other code.
 */

extern JS_FRIEND_API(void)
DumpString(JSString* str);

extern JS_FRIEND_API(void)
DumpAtom(JSAtom* atom);

extern JS_FRIEND_API(void)
DumpObject(JSObject* obj);

extern JS_FRIEND_API(void)
DumpChars(const char16_t* s, size_t n);

extern JS_FRIEND_API(void)
DumpValue(const JS::Value& val);

extern JS_FRIEND_API(void)
DumpId(jsid id);

extern JS_FRIEND_API(void)
DumpInterpreterFrame(JSContext* cx, InterpreterFrame* start = nullptr);

extern JS_FRIEND_API(bool)
DumpPC(JSContext* cx);

extern JS_FRIEND_API(bool)
DumpScript(JSContext* cx, JSScript* scriptArg);

#endif

extern JS_FRIEND_API(void)
DumpBacktrace(JSContext* cx);

} // namespace js

namespace JS {

/** Exposed for DumpJSStack */
extern JS_FRIEND_API(char*)
FormatStackDump(JSContext* cx, char* buf, bool showArgs, bool showLocals, bool showThisProps);

} // namespace JS

/**
 * Copies all own properties from |obj| to |target|. |obj| must be a "native"
 * object (that is to say, normal-ish - not an Array or a Proxy).
 *
 * This function immediately enters a compartment, and does not impose any
 * restrictions on the compartment of |cx|.
 */
extern JS_FRIEND_API(bool)
JS_CopyPropertiesFrom(JSContext* cx, JS::HandleObject target, JS::HandleObject obj);

/*
 * Single-property version of the above. This function asserts that an |own|
 * property of the given name exists on |obj|.
 *
 * On entry, |cx| must be same-compartment with |obj|.
 *
 * The copyBehavior argument controls what happens with
 * non-configurable properties.
 */
typedef enum  {
    MakeNonConfigurableIntoConfigurable,
    CopyNonConfigurableAsIs
} PropertyCopyBehavior;

extern JS_FRIEND_API(bool)
JS_CopyPropertyFrom(JSContext* cx, JS::HandleId id, JS::HandleObject target,
                    JS::HandleObject obj,
                    PropertyCopyBehavior copyBehavior = CopyNonConfigurableAsIs);

extern JS_FRIEND_API(bool)
JS_WrapPropertyDescriptor(JSContext* cx, JS::MutableHandle<JSPropertyDescriptor> desc);

struct JSFunctionSpecWithHelp {
    const char*     name;
    JSNative        call;
    uint16_t        nargs;
    uint16_t        flags;
    const JSJitInfo* jitInfo;
    const char*     usage;
    const char*     help;
};

#define JS_FN_HELP(name,call,nargs,flags,usage,help)                          \
    {name, call, nargs, (flags) | JSPROP_ENUMERATE | JSFUN_STUB_GSOPS, nullptr, usage, help}
#define JS_INLINABLE_FN_HELP(name,call,nargs,flags,native,usage,help)         \
    {name, call, nargs, (flags) | JSPROP_ENUMERATE | JSFUN_STUB_GSOPS, &js::jit::JitInfo_##native,\
     usage, help}
#define JS_FS_HELP_END                                                        \
    {nullptr, nullptr, 0, 0, nullptr, nullptr}

extern JS_FRIEND_API(bool)
JS_DefineFunctionsWithHelp(JSContext* cx, JS::HandleObject obj, const JSFunctionSpecWithHelp* fs);

namespace js {

/*
 * Helper Macros for creating JSClasses that function as proxies.
 *
 * NB: The macro invocation must be surrounded by braces, so as to
 *     allow for potential JSClass extensions.
 */
#define PROXY_MAKE_EXT(isWrappedNative, objectMoved)                    \
    {                                                                   \
        isWrappedNative,                                                \
        js::proxy_WeakmapKeyDelegate,                                   \
        objectMoved                                                     \
    }

#define PROXY_CLASS_WITH_EXT(name, flags, ext)                                          \
    {                                                                                   \
        name,                                                                           \
        js::Class::NON_NATIVE |                                                         \
            JSCLASS_IS_PROXY |                                                          \
            JSCLASS_DELAY_METADATA_CALLBACK |                                           \
            flags,                                                                      \
        nullptr,                 /* addProperty */                                      \
        nullptr,                 /* delProperty */                                      \
        nullptr,                 /* getProperty */                                      \
        nullptr,                 /* setProperty */                                      \
        nullptr,                 /* enumerate */                                        \
        nullptr,                 /* resolve */                                          \
        nullptr,                 /* mayResolve */                                       \
        js::proxy_Finalize,      /* finalize    */                                      \
        nullptr,                 /* call        */                                      \
        js::proxy_HasInstance,   /* hasInstance */                                      \
        nullptr,                 /* construct   */                                      \
        js::proxy_Trace,         /* trace       */                                      \
        JS_NULL_CLASS_SPEC,                                                             \
        ext,                                                                            \
        {                                                                               \
            js::proxy_LookupProperty,                                                   \
            js::proxy_DefineProperty,                                                   \
            js::proxy_HasProperty,                                                      \
            js::proxy_GetProperty,                                                      \
            js::proxy_SetProperty,                                                      \
            js::proxy_GetOwnPropertyDescriptor,                                         \
            js::proxy_DeleteProperty,                                                   \
            js::proxy_Watch, js::proxy_Unwatch,                                         \
            js::proxy_GetElements,                                                      \
            nullptr,             /* enumerate       */                                  \
            js::proxy_FunToString,                                                      \
        }                                                                               \
    }

#define PROXY_CLASS_DEF(name, flags)                                    \
  PROXY_CLASS_WITH_EXT(name, flags,                                     \
                       PROXY_MAKE_EXT(                                  \
                         false,   /* isWrappedNative */                 \
                         js::proxy_ObjectMoved                          \
                       ))

/*
 * Proxy stubs, similar to JS_*Stub, for embedder proxy class definitions.
 *
 * NB: Should not be called directly.
 */

extern JS_FRIEND_API(bool)
proxy_LookupProperty(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::MutableHandleObject objp,
                    JS::MutableHandle<Shape*> propp);
extern JS_FRIEND_API(bool)
proxy_DefineProperty(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                     JS::Handle<JSPropertyDescriptor> desc,
                     JS::ObjectOpResult& result);
extern JS_FRIEND_API(bool)
proxy_HasProperty(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* foundp);
extern JS_FRIEND_API(bool)
proxy_GetProperty(JSContext* cx, JS::HandleObject obj, JS::HandleValue receiver, JS::HandleId id,
                  JS::MutableHandleValue vp);
extern JS_FRIEND_API(bool)
proxy_SetProperty(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleValue bp,
                  JS::HandleValue receiver, JS::ObjectOpResult& result);
extern JS_FRIEND_API(bool)
proxy_GetOwnPropertyDescriptor(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                               JS::MutableHandle<JSPropertyDescriptor> desc);
extern JS_FRIEND_API(bool)
proxy_DeleteProperty(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                     JS::ObjectOpResult& result);

extern JS_FRIEND_API(void)
proxy_Trace(JSTracer* trc, JSObject* obj);
extern JS_FRIEND_API(JSObject*)
proxy_WeakmapKeyDelegate(JSObject* obj);
extern JS_FRIEND_API(bool)
proxy_Convert(JSContext* cx, JS::HandleObject proxy, JSType hint, JS::MutableHandleValue vp);
extern JS_FRIEND_API(void)
proxy_Finalize(FreeOp* fop, JSObject* obj);
extern JS_FRIEND_API(void)
proxy_ObjectMoved(JSObject* obj, const JSObject* old);
extern JS_FRIEND_API(bool)
proxy_HasInstance(JSContext* cx, JS::HandleObject proxy, JS::MutableHandleValue v, bool* bp);
extern JS_FRIEND_API(bool)
proxy_Call(JSContext* cx, unsigned argc, JS::Value* vp);
extern JS_FRIEND_API(bool)
proxy_Construct(JSContext* cx, unsigned argc, JS::Value* vp);
extern JS_FRIEND_API(JSObject*)
proxy_innerObject(JSObject* obj);
extern JS_FRIEND_API(bool)
proxy_Watch(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleObject callable);
extern JS_FRIEND_API(bool)
proxy_Unwatch(JSContext* cx, JS::HandleObject obj, JS::HandleId id);
extern JS_FRIEND_API(bool)
proxy_GetElements(JSContext* cx, JS::HandleObject proxy, uint32_t begin, uint32_t end,
                  ElementAdder* adder);
extern JS_FRIEND_API(JSString*)
proxy_FunToString(JSContext* cx, JS::HandleObject proxy, unsigned indent);

/**
 * A class of objects that return source code on demand.
 *
 * When code is compiled with setSourceIsLazy(true), SpiderMonkey doesn't
 * retain the source code (and doesn't do lazy bytecode generation). If we ever
 * need the source code, say, in response to a call to Function.prototype.
 * toSource or Debugger.Source.prototype.text, then we call the 'load' member
 * function of the instance of this class that has hopefully been registered
 * with the runtime, passing the code's URL, and hope that it will be able to
 * find the source.
 */
class SourceHook {
  public:
    virtual ~SourceHook() { }

    /**
     * Set |*src| and |*length| to refer to the source code for |filename|.
     * On success, the caller owns the buffer to which |*src| points, and
     * should use JS_free to free it.
     */
    virtual bool load(JSContext* cx, const char* filename, char16_t** src, size_t* length) = 0;
};

/**
 * Have |rt| use |hook| to retrieve lazily-retrieved source code. See the
 * comments for SourceHook. The runtime takes ownership of the hook, and
 * will delete it when the runtime itself is deleted, or when a new hook is
 * set.
 */
extern JS_FRIEND_API(void)
SetSourceHook(JSRuntime* rt, mozilla::UniquePtr<SourceHook> hook);

/** Remove |rt|'s source hook, and return it. The caller now owns the hook. */
extern JS_FRIEND_API(mozilla::UniquePtr<SourceHook>)
ForgetSourceHook(JSRuntime* rt);

extern JS_FRIEND_API(JS::Zone*)
GetCompartmentZone(JSCompartment* comp);

typedef bool
(* PreserveWrapperCallback)(JSContext* cx, JSObject* obj);

typedef enum  {
    CollectNurseryBeforeDump,
    IgnoreNurseryObjects
} DumpHeapNurseryBehaviour;

 /**
  * Dump the complete object graph of heap-allocated things.
  * fp is the file for the dump output.
  */
extern JS_FRIEND_API(void)
DumpHeap(JSRuntime* rt, FILE* fp, DumpHeapNurseryBehaviour nurseryBehaviour);

#ifdef JS_OLD_GETTER_SETTER_METHODS
JS_FRIEND_API(bool) obj_defineGetter(JSContext* cx, unsigned argc, JS::Value* vp);
JS_FRIEND_API(bool) obj_defineSetter(JSContext* cx, unsigned argc, JS::Value* vp);
#endif

extern JS_FRIEND_API(bool)
IsSystemCompartment(JSCompartment* comp);

extern JS_FRIEND_API(bool)
IsSystemZone(JS::Zone* zone);

extern JS_FRIEND_API(bool)
IsAtomsCompartment(JSCompartment* comp);

extern JS_FRIEND_API(bool)
IsAtomsZone(JS::Zone* zone);

struct WeakMapTracer
{
    JSRuntime* runtime;

    explicit WeakMapTracer(JSRuntime* rt) : runtime(rt) {}

    // Weak map tracer callback, called once for every binding of every
    // weak map that was live at the time of the last garbage collection.
    //
    // m will be nullptr if the weak map is not contained in a JS Object.
    //
    // The callback should not GC (and will assert in a debug build if it does so.)
    virtual void trace(JSObject* m, JS::GCCellPtr key, JS::GCCellPtr value) = 0;
};

extern JS_FRIEND_API(void)
TraceWeakMaps(WeakMapTracer* trc);

extern JS_FRIEND_API(bool)
AreGCGrayBitsValid(JSRuntime* rt);

extern JS_FRIEND_API(bool)
ZoneGlobalsAreAllGray(JS::Zone* zone);

typedef void
(*GCThingCallback)(void* closure, JS::GCCellPtr thing);

extern JS_FRIEND_API(void)
VisitGrayWrapperTargets(JS::Zone* zone, GCThingCallback callback, void* closure);

extern JS_FRIEND_API(JSObject*)
GetWeakmapKeyDelegate(JSObject* key);

JS_FRIEND_API(JS::TraceKind)
GCThingTraceKind(void* thing);

/**
 * Invoke cellCallback on every gray JS_OBJECT in the given zone.
 */
extern JS_FRIEND_API(void)
IterateGrayObjects(JS::Zone* zone, GCThingCallback cellCallback, void* data);

#ifdef JS_HAS_CTYPES
extern JS_FRIEND_API(size_t)
SizeOfDataIfCDataObject(mozilla::MallocSizeOf mallocSizeOf, JSObject* obj);
#endif

extern JS_FRIEND_API(JSCompartment*)
GetAnyCompartmentInZone(JS::Zone* zone);

/*
 * Shadow declarations of JS internal structures, for access by inline access
 * functions below. Do not use these structures in any other way. When adding
 * new fields for access by inline methods, make sure to add static asserts to
 * the original header file to ensure that offsets are consistent.
 */
namespace shadow {

struct ObjectGroup {
    const Class* clasp;
    JSObject*   proto;
    JSCompartment* compartment;
};

struct BaseShape {
    const js::Class* clasp_;
    JSObject* parent;
};

class Shape {
public:
    shadow::BaseShape* base;
    jsid              _1;
    uint32_t          slotInfo;

    static const uint32_t FIXED_SLOTS_SHIFT = 27;
};

/**
 * This layout is shared by all native objects. For non-native objects, the
 * group may always be accessed safely, and other members may be as well,
 * depending on the object's specific layout.
 */
struct Object {
    shadow::ObjectGroup* group;
    shadow::Shape*      shape;
    JS::Value*          slots;
    void*               _1;

    size_t numFixedSlots() const { return shape->slotInfo >> Shape::FIXED_SLOTS_SHIFT; }
    JS::Value* fixedSlots() const {
        return (JS::Value*)(uintptr_t(this) + sizeof(shadow::Object));
    }

    JS::Value& slotRef(size_t slot) const {
        size_t nfixed = numFixedSlots();
        if (slot < nfixed)
            return fixedSlots()[slot];
        return slots[slot - nfixed];
    }
};

struct Function {
    Object base;
    uint16_t nargs;
    uint16_t flags;
    /* Used only for natives */
    JSNative native;
    const JSJitInfo* jitinfo;
    void* _1;
};

struct String
{
    static const uint32_t INLINE_CHARS_BIT = JS_BIT(2);
    static const uint32_t LATIN1_CHARS_BIT = JS_BIT(6);
    static const uint32_t ROPE_FLAGS       = 0;
    static const uint32_t TYPE_FLAGS_MASK  = JS_BIT(6) - 1;
    uint32_t flags;
    uint32_t length;
    union {
        const JS::Latin1Char* nonInlineCharsLatin1;
        const char16_t* nonInlineCharsTwoByte;
        JS::Latin1Char inlineStorageLatin1[1];
        char16_t inlineStorageTwoByte[1];
    };
};

} /* namespace shadow */

// This is equal to |&JSObject::class_|.  Use it in places where you don't want
// to #include jsobj.h.
extern JS_FRIEND_DATA(const js::Class* const) ObjectClassPtr;

inline const js::Class*
GetObjectClass(const JSObject* obj)
{
    return reinterpret_cast<const shadow::Object*>(obj)->group->clasp;
}

inline const JSClass*
GetObjectJSClass(JSObject* obj)
{
    return js::Jsvalify(GetObjectClass(obj));
}

JS_FRIEND_API(const Class*)
ProtoKeyToClass(JSProtoKey key);

// Returns true if the standard class identified by |key| inherits from
// another standard class (in addition to Object) along its proto chain.
//
// In practice, this only returns true for Error subtypes.
inline bool
StandardClassIsDependent(JSProtoKey key)
{
    const Class* clasp = ProtoKeyToClass(key);
    return clasp && clasp->spec.defined() && clasp->spec.dependent();
}

// Returns the key for the class inherited by a given standard class (that
// is to say, the prototype of this standard class's prototype).
//
// You must be sure that this corresponds to a standard class with a cached
// JSProtoKey before calling this function. In general |key| will match the
// cached proto key, except in cases where multiple JSProtoKeys share a
// JSClass.
inline JSProtoKey
ParentKeyForStandardClass(JSProtoKey key)
{
    // [Object] has nothing to inherit from.
    if (key == JSProto_Object)
        return JSProto_Null;

    // If we're dependent, return the key of the class we depend on.
    if (StandardClassIsDependent(key))
        return ProtoKeyToClass(key)->spec.parentKey();

    // Otherwise, we inherit [Object].
    return JSProto_Object;
}

JS_FRIEND_API(bool)
IsFunctionObject(JSObject* obj);

static MOZ_ALWAYS_INLINE JSCompartment*
GetObjectCompartment(JSObject* obj)
{
    return reinterpret_cast<shadow::Object*>(obj)->group->compartment;
}

JS_FRIEND_API(JSObject*)
GetGlobalForObjectCrossCompartment(JSObject* obj);

JS_FRIEND_API(JSObject*)
GetPrototypeNoProxy(JSObject* obj);

JS_FRIEND_API(void)
AssertSameCompartment(JSContext* cx, JSObject* obj);

#ifdef JS_DEBUG
JS_FRIEND_API(void)
AssertSameCompartment(JSObject* objA, JSObject* objB);
#else
inline void AssertSameCompartment(JSObject* objA, JSObject* objB) {}
#endif

JS_FRIEND_API(void)
NotifyAnimationActivity(JSObject* obj);

/**
 * Return the outermost enclosing function (script) of the scripted caller.
 * This function returns nullptr in several cases:
 *  - no script is running on the context
 *  - the caller is in global or eval code
 * In particular, this function will "stop" its outermost search at eval() and
 * thus it will really return the outermost enclosing function *since the
 * innermost eval*.
 */
JS_FRIEND_API(JSFunction*)
GetOutermostEnclosingFunctionOfScriptedCaller(JSContext* cx);

JS_FRIEND_API(JSFunction*)
DefineFunctionWithReserved(JSContext* cx, JSObject* obj, const char* name, JSNative call,
                           unsigned nargs, unsigned attrs);

JS_FRIEND_API(JSFunction*)
NewFunctionWithReserved(JSContext* cx, JSNative call, unsigned nargs, unsigned flags,
                        const char* name);

JS_FRIEND_API(JSFunction*)
NewFunctionByIdWithReserved(JSContext* cx, JSNative native, unsigned nargs, unsigned flags,
                            jsid id);

JS_FRIEND_API(const JS::Value&)
GetFunctionNativeReserved(JSObject* fun, size_t which);

JS_FRIEND_API(void)
SetFunctionNativeReserved(JSObject* fun, size_t which, const JS::Value& val);

JS_FRIEND_API(bool)
FunctionHasNativeReserved(JSObject* fun);

JS_FRIEND_API(bool)
GetObjectProto(JSContext* cx, JS::HandleObject obj, JS::MutableHandleObject proto);

JS_FRIEND_API(bool)
GetOriginalEval(JSContext* cx, JS::HandleObject scope,
                JS::MutableHandleObject eval);

inline void*
GetObjectPrivate(JSObject* obj)
{
    MOZ_ASSERT(GetObjectClass(obj)->flags & JSCLASS_HAS_PRIVATE);
    const shadow::Object* nobj = reinterpret_cast<const shadow::Object*>(obj);
    void** addr = reinterpret_cast<void**>(&nobj->fixedSlots()[nobj->numFixedSlots()]);
    return *addr;
}

inline const JS::Value&
GetReservedSlot(JSObject* obj, size_t slot)
{
    MOZ_ASSERT(slot < JSCLASS_RESERVED_SLOTS(GetObjectClass(obj)));
    return reinterpret_cast<const shadow::Object*>(obj)->slotRef(slot);
}

JS_FRIEND_API(void)
SetReservedOrProxyPrivateSlotWithBarrier(JSObject* obj, size_t slot, const JS::Value& value);

inline void
SetReservedSlot(JSObject* obj, size_t slot, const JS::Value& value)
{
    MOZ_ASSERT(slot < JSCLASS_RESERVED_SLOTS(GetObjectClass(obj)));
    shadow::Object* sobj = reinterpret_cast<shadow::Object*>(obj);
    if (sobj->slotRef(slot).isMarkable() || value.isMarkable())
        SetReservedOrProxyPrivateSlotWithBarrier(obj, slot, value);
    else
        sobj->slotRef(slot) = value;
}

JS_FRIEND_API(uint32_t)
GetObjectSlotSpan(JSObject* obj);

inline const JS::Value&
GetObjectSlot(JSObject* obj, size_t slot)
{
    MOZ_ASSERT(slot < GetObjectSlotSpan(obj));
    return reinterpret_cast<const shadow::Object*>(obj)->slotRef(slot);
}

MOZ_ALWAYS_INLINE size_t
GetAtomLength(JSAtom* atom)
{
    return reinterpret_cast<shadow::String*>(atom)->length;
}

static const uint32_t MaxStringLength = (1 << 28) - 1;

MOZ_ALWAYS_INLINE size_t
GetStringLength(JSString* s)
{
    return reinterpret_cast<shadow::String*>(s)->length;
}

MOZ_ALWAYS_INLINE size_t
GetFlatStringLength(JSFlatString* s)
{
    return reinterpret_cast<shadow::String*>(s)->length;
}

MOZ_ALWAYS_INLINE size_t
GetLinearStringLength(JSLinearString* s)
{
    return reinterpret_cast<shadow::String*>(s)->length;
}

MOZ_ALWAYS_INLINE bool
LinearStringHasLatin1Chars(JSLinearString* s)
{
    return reinterpret_cast<shadow::String*>(s)->flags & shadow::String::LATIN1_CHARS_BIT;
}

MOZ_ALWAYS_INLINE bool
AtomHasLatin1Chars(JSAtom* atom)
{
    return reinterpret_cast<shadow::String*>(atom)->flags & shadow::String::LATIN1_CHARS_BIT;
}

MOZ_ALWAYS_INLINE bool
StringHasLatin1Chars(JSString* s)
{
    return reinterpret_cast<shadow::String*>(s)->flags & shadow::String::LATIN1_CHARS_BIT;
}

MOZ_ALWAYS_INLINE const JS::Latin1Char*
GetLatin1LinearStringChars(const JS::AutoCheckCannotGC& nogc, JSLinearString* linear)
{
    MOZ_ASSERT(LinearStringHasLatin1Chars(linear));

    using shadow::String;
    String* s = reinterpret_cast<String*>(linear);
    if (s->flags & String::INLINE_CHARS_BIT)
        return s->inlineStorageLatin1;
    return s->nonInlineCharsLatin1;
}

MOZ_ALWAYS_INLINE const char16_t*
GetTwoByteLinearStringChars(const JS::AutoCheckCannotGC& nogc, JSLinearString* linear)
{
    MOZ_ASSERT(!LinearStringHasLatin1Chars(linear));

    using shadow::String;
    String* s = reinterpret_cast<String*>(linear);
    if (s->flags & String::INLINE_CHARS_BIT)
        return s->inlineStorageTwoByte;
    return s->nonInlineCharsTwoByte;
}

MOZ_ALWAYS_INLINE JSLinearString*
AtomToLinearString(JSAtom* atom)
{
    return reinterpret_cast<JSLinearString*>(atom);
}

MOZ_ALWAYS_INLINE JSFlatString*
AtomToFlatString(JSAtom* atom)
{
    return reinterpret_cast<JSFlatString*>(atom);
}

MOZ_ALWAYS_INLINE JSLinearString*
FlatStringToLinearString(JSFlatString* s)
{
    return reinterpret_cast<JSLinearString*>(s);
}

MOZ_ALWAYS_INLINE const JS::Latin1Char*
GetLatin1AtomChars(const JS::AutoCheckCannotGC& nogc, JSAtom* atom)
{
    return GetLatin1LinearStringChars(nogc, AtomToLinearString(atom));
}

MOZ_ALWAYS_INLINE const char16_t*
GetTwoByteAtomChars(const JS::AutoCheckCannotGC& nogc, JSAtom* atom)
{
    return GetTwoByteLinearStringChars(nogc, AtomToLinearString(atom));
}

JS_FRIEND_API(JSLinearString*)
StringToLinearStringSlow(JSContext* cx, JSString* str);

MOZ_ALWAYS_INLINE JSLinearString*
StringToLinearString(JSContext* cx, JSString* str)
{
    using shadow::String;
    String* s = reinterpret_cast<String*>(str);
    if (MOZ_UNLIKELY((s->flags & String::TYPE_FLAGS_MASK) == String::ROPE_FLAGS))
        return StringToLinearStringSlow(cx, str);
    return reinterpret_cast<JSLinearString*>(str);
}

MOZ_ALWAYS_INLINE void
CopyLinearStringChars(char16_t* dest, JSLinearString* s, size_t len)
{
    JS::AutoCheckCannotGC nogc;
    if (LinearStringHasLatin1Chars(s)) {
        const JS::Latin1Char* src = GetLatin1LinearStringChars(nogc, s);
        for (size_t i = 0; i < len; i++)
            dest[i] = src[i];
    } else {
        const char16_t* src = GetTwoByteLinearStringChars(nogc, s);
        mozilla::PodCopy(dest, src, len);
    }
}

inline bool
CopyStringChars(JSContext* cx, char16_t* dest, JSString* s, size_t len)
{
    JSLinearString* linear = StringToLinearString(cx, s);
    if (!linear)
        return false;

    CopyLinearStringChars(dest, linear, len);
    return true;
}

inline void
CopyFlatStringChars(char16_t* dest, JSFlatString* s, size_t len)
{
    CopyLinearStringChars(dest, FlatStringToLinearString(s), len);
}

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
JS_FRIEND_API(bool)
GetPropertyKeys(JSContext* cx, JS::HandleObject obj, unsigned flags, JS::AutoIdVector* props);

JS_FRIEND_API(bool)
AppendUnique(JSContext* cx, JS::AutoIdVector& base, JS::AutoIdVector& others);

JS_FRIEND_API(bool)
StringIsArrayIndex(JSLinearString* str, uint32_t* indexp);

JS_FRIEND_API(void)
SetPreserveWrapperCallback(JSRuntime* rt, PreserveWrapperCallback callback);

JS_FRIEND_API(bool)
IsObjectInContextCompartment(JSObject* obj, const JSContext* cx);

/*
 * NB: these flag bits are encoded into the bytecode stream in the immediate
 * operand of JSOP_ITER, so don't change them without advancing vm/Xdr.h's
 * XDR_BYTECODE_VERSION.
 * NB: keep these in sync with the copy in builtin/SelfHostingDefines.h.
 * The first three are omitted because they shouldn't be used in new code.
 */
#define JSITER_ENUMERATE  0x1   /* for-in compatible hidden default iterator */
#define JSITER_FOREACH    0x2   /* get obj[key] for each property */
#define JSITER_KEYVALUE   0x4   /* obsolete destructuring for-in wants [key, value] */
#define JSITER_OWNONLY    0x8   /* iterate over obj's own properties only */
#define JSITER_HIDDEN     0x10  /* also enumerate non-enumerable properties */
#define JSITER_SYMBOLS    0x20  /* also include symbol property keys */
#define JSITER_SYMBOLSONLY 0x40 /* exclude string property keys */

JS_FRIEND_API(bool)
RunningWithTrustedPrincipals(JSContext* cx);

inline uintptr_t
GetNativeStackLimit(JSContext* cx, StackKind kind, int extraAllowance = 0)
{
    PerThreadDataFriendFields* mainThread =
      PerThreadDataFriendFields::getMainThread(GetRuntime(cx));
    uintptr_t limit = mainThread->nativeStackLimit[kind];
#if JS_STACK_GROWTH_DIRECTION > 0
    limit += extraAllowance;
#else
    limit -= extraAllowance;
#endif
    return limit;
}

inline uintptr_t
GetNativeStackLimit(JSContext* cx, int extraAllowance = 0)
{
    StackKind kind = RunningWithTrustedPrincipals(cx) ? StackForTrustedScript
                                                      : StackForUntrustedScript;
    return GetNativeStackLimit(cx, kind, extraAllowance);
}

/*
 * These macros report a stack overflow and run |onerror| if we are close to
 * using up the C stack. The JS_CHECK_CHROME_RECURSION variant gives us a
 * little extra space so that we can ensure that crucial code is able to run.
 * JS_CHECK_RECURSION_CONSERVATIVE allows less space than any other check,
 * including a safety buffer (as in, it uses the untrusted limit and subtracts
 * a little more from it).
 */

#define JS_CHECK_RECURSION_LIMIT(cx, limit, onerror)                            \
    JS_BEGIN_MACRO                                                              \
        int stackDummy_;                                                        \
        if (!JS_CHECK_STACK_SIZE(limit, &stackDummy_)) {                        \
            js::ReportOverRecursed(cx);                                         \
            onerror;                                                            \
        }                                                                       \
    JS_END_MACRO

#define JS_CHECK_RECURSION(cx, onerror)                                         \
    JS_CHECK_RECURSION_LIMIT(cx, js::GetNativeStackLimit(cx), onerror)

#define JS_CHECK_RECURSION_LIMIT_DONT_REPORT(cx, limit, onerror)                \
    JS_BEGIN_MACRO                                                              \
        int stackDummy_;                                                        \
        if (!JS_CHECK_STACK_SIZE(limit, &stackDummy_)) {                        \
            onerror;                                                            \
        }                                                                       \
    JS_END_MACRO

#define JS_CHECK_RECURSION_DONT_REPORT(cx, onerror)                             \
    JS_CHECK_RECURSION_LIMIT_DONT_REPORT(cx, js::GetNativeStackLimit(cx), onerror)

#define JS_CHECK_RECURSION_WITH_SP_DONT_REPORT(cx, sp, onerror)                 \
    JS_BEGIN_MACRO                                                              \
        if (!JS_CHECK_STACK_SIZE(js::GetNativeStackLimit(cx), sp)) {            \
            onerror;                                                            \
        }                                                                       \
    JS_END_MACRO

#define JS_CHECK_RECURSION_WITH_SP(cx, sp, onerror)                             \
    JS_BEGIN_MACRO                                                              \
        if (!JS_CHECK_STACK_SIZE(js::GetNativeStackLimit(cx), sp)) {            \
            js::ReportOverRecursed(cx);                                         \
            onerror;                                                            \
        }                                                                       \
    JS_END_MACRO

#define JS_CHECK_SYSTEM_RECURSION(cx, onerror)                                  \
    JS_CHECK_RECURSION_LIMIT(cx, js::GetNativeStackLimit(cx, js::StackForSystemCode), onerror)

#define JS_CHECK_RECURSION_CONSERVATIVE(cx, onerror)                            \
    JS_CHECK_RECURSION_LIMIT(cx,                                                \
                             js::GetNativeStackLimit(cx, js::StackForUntrustedScript, -1024 * int(sizeof(size_t))), \
                             onerror)

#define JS_CHECK_RECURSION_CONSERVATIVE_DONT_REPORT(cx, onerror)                \
    JS_CHECK_RECURSION_LIMIT_DONT_REPORT(cx,                                    \
                                         js::GetNativeStackLimit(cx, js::StackForUntrustedScript, -1024 * int(sizeof(size_t))), \
                                         onerror)

JS_FRIEND_API(void)
StartPCCountProfiling(JSContext* cx);

JS_FRIEND_API(void)
StopPCCountProfiling(JSContext* cx);

JS_FRIEND_API(void)
PurgePCCounts(JSContext* cx);

JS_FRIEND_API(size_t)
GetPCCountScriptCount(JSContext* cx);

JS_FRIEND_API(JSString*)
GetPCCountScriptSummary(JSContext* cx, size_t script);

JS_FRIEND_API(JSString*)
GetPCCountScriptContents(JSContext* cx, size_t script);

/**
 * Generate lcov trace file content for the current compartment, and allocate a
 * new buffer and return the content in it, the size of the newly allocated
 * content within the buffer would be set to the length out-param.
 *
 * In case of out-of-memory, this function returns nullptr and does not set any
 * value to the length out-param.
 */
JS_FRIEND_API(char*)
GetCodeCoverageSummary(JSContext* cx, size_t* length);

JS_FRIEND_API(bool)
ContextHasOutstandingRequests(const JSContext* cx);

typedef void
(* ActivityCallback)(void* arg, bool active);

/**
 * Sets a callback that is run whenever the runtime goes idle - the
 * last active request ceases - and begins activity - when it was
 * idle and a request begins.
 */
JS_FRIEND_API(void)
SetActivityCallback(JSRuntime* rt, ActivityCallback cb, void* arg);

typedef bool
(* DOMInstanceClassHasProtoAtDepth)(const Class* instanceClass,
                                    uint32_t protoID, uint32_t depth);
struct JSDOMCallbacks {
    DOMInstanceClassHasProtoAtDepth instanceClassMatchesProto;
};
typedef struct JSDOMCallbacks DOMCallbacks;

extern JS_FRIEND_API(void)
SetDOMCallbacks(JSRuntime* rt, const DOMCallbacks* callbacks);

extern JS_FRIEND_API(const DOMCallbacks*)
GetDOMCallbacks(JSRuntime* rt);

extern JS_FRIEND_API(JSObject*)
GetTestingFunctions(JSContext* cx);

/**
 * Helper to convert FreeOp to JSFreeOp when the definition of FreeOp is not
 * available and the compiler does not know that FreeOp inherits from
 * JSFreeOp.
 */
inline JSFreeOp*
CastToJSFreeOp(FreeOp* fop)
{
    return reinterpret_cast<JSFreeOp*>(fop);
}

/* Implemented in jsexn.cpp. */

/**
 * Get an error type name from a JSExnType constant.
 * Returns nullptr for invalid arguments and JSEXN_INTERNALERR
 */
extern JS_FRIEND_API(JSFlatString*)
GetErrorTypeName(JSRuntime* rt, int16_t exnType);

#ifdef JS_DEBUG
extern JS_FRIEND_API(unsigned)
GetEnterCompartmentDepth(JSContext* cx);
#endif

class RegExpGuard;
extern JS_FRIEND_API(bool)
RegExpToSharedNonInline(JSContext* cx, JS::HandleObject regexp, RegExpGuard* shared);

/* Implemented in jswrapper.cpp. */
typedef enum NukeReferencesToWindow {
    NukeWindowReferences,
    DontNukeWindowReferences
} NukeReferencesToWindow;

/*
 * These filters are designed to be ephemeral stack classes, and thus don't
 * do any rooting or holding of their members.
 */
struct CompartmentFilter {
    virtual bool match(JSCompartment* c) const = 0;
};

struct AllCompartments : public CompartmentFilter {
    virtual bool match(JSCompartment* c) const override { return true; }
};

struct ContentCompartmentsOnly : public CompartmentFilter {
    virtual bool match(JSCompartment* c) const override {
        return !IsSystemCompartment(c);
    }
};

struct ChromeCompartmentsOnly : public CompartmentFilter {
    virtual bool match(JSCompartment* c) const override {
        return IsSystemCompartment(c);
    }
};

struct SingleCompartment : public CompartmentFilter {
    JSCompartment* ours;
    explicit SingleCompartment(JSCompartment* c) : ours(c) {}
    virtual bool match(JSCompartment* c) const override { return c == ours; }
};

struct CompartmentsWithPrincipals : public CompartmentFilter {
    JSPrincipals* principals;
    explicit CompartmentsWithPrincipals(JSPrincipals* p) : principals(p) {}
    virtual bool match(JSCompartment* c) const override {
        return JS_GetCompartmentPrincipals(c) == principals;
    }
};

extern JS_FRIEND_API(bool)
NukeCrossCompartmentWrappers(JSContext* cx,
                             const CompartmentFilter& sourceFilter,
                             const CompartmentFilter& targetFilter,
                             NukeReferencesToWindow nukeReferencesToWindow);

/* Specify information about DOMProxy proxies in the DOM, for use by ICs. */

/*
 * The DOMProxyShadowsCheck function will be called to check if the property for
 * id should be gotten from the prototype, or if there is an own property that
 * shadows it.
 * * If ShadowsViaDirectExpando is returned, then the slot at
 *   listBaseExpandoSlot contains an expando object which has the property in
 *   question.
 * * If ShadowsViaIndirectExpando is returned, then the slot at
 *   listBaseExpandoSlot contains a private pointer to an ExpandoAndGeneration
 *   and the expando object in the ExpandoAndGeneration has the property in
 *   question.
 * * If DoesntShadow is returned then the slot at listBaseExpandoSlot should
 *   either be undefined or point to an expando object that would contain the
 *   own property.
 * * If DoesntShadowUnique is returned then the slot at listBaseExpandoSlot
 *   should contain a private pointer to a ExpandoAndGeneration, which contains
 *   a JS::Value that should either be undefined or point to an expando object,
 *   and a uint32 value. If that value changes then the IC for getting a
 *   property will be invalidated.
 * * If Shadows is returned, that means the property is an own property of the
 *   proxy but doesn't live on the expando object.
 */

struct ExpandoAndGeneration {
  ExpandoAndGeneration()
    : expando(JS::UndefinedValue()),
      generation(0)
  {}

  void Unlink()
  {
      ++generation;
      expando.setUndefined();
  }

  static size_t offsetOfExpando()
  {
      return offsetof(ExpandoAndGeneration, expando);
  }

  static size_t offsetOfGeneration()
  {
      return offsetof(ExpandoAndGeneration, generation);
  }

  JS::Heap<JS::Value> expando;
  uint32_t generation;
};

typedef enum DOMProxyShadowsResult {
  ShadowCheckFailed,
  Shadows,
  DoesntShadow,
  DoesntShadowUnique,
  ShadowsViaDirectExpando,
  ShadowsViaIndirectExpando
} DOMProxyShadowsResult;
typedef DOMProxyShadowsResult
(* DOMProxyShadowsCheck)(JSContext* cx, JS::HandleObject object, JS::HandleId id);
JS_FRIEND_API(void)
SetDOMProxyInformation(const void* domProxyHandlerFamily, uint32_t domProxyExpandoSlot,
                       DOMProxyShadowsCheck domProxyShadowsCheck);

const void* GetDOMProxyHandlerFamily();
uint32_t GetDOMProxyExpandoSlot();
DOMProxyShadowsCheck GetDOMProxyShadowsCheck();
inline bool DOMProxyIsShadowing(DOMProxyShadowsResult result) {
    return result == Shadows ||
           result == ShadowsViaDirectExpando ||
           result == ShadowsViaIndirectExpando;
}

/* Implemented in jsdate.cpp. */

/** Detect whether the internal date value is NaN. */
extern JS_FRIEND_API(bool)
DateIsValid(JSContext* cx, JS::HandleObject obj, bool* isValid);

extern JS_FRIEND_API(bool)
DateGetMsecSinceEpoch(JSContext* cx, JS::HandleObject obj, double* msecSinceEpoch);

} /* namespace js */

/* Implemented in jscntxt.cpp. */

/**
 * Report an exception, which is currently realized as a printf-style format
 * string and its arguments.
 */
typedef enum JSErrNum {
#define MSG_DEF(name, count, exception, format) \
    name,
#include "js.msg"
#undef MSG_DEF
    JSErr_Limit
} JSErrNum;

namespace js {

extern JS_FRIEND_API(const JSErrorFormatString*)
GetErrorMessage(void* userRef, const unsigned errorNumber);

// AutoStableStringChars is here so we can use it in ErrorReport.  It
// should get moved out of here if we can manage it.  See bug 1040316.

/**
 * This class provides safe access to a string's chars across a GC. Once
 * we allocate strings and chars in the nursery (bug 903519), this class
 * will have to make a copy of the string's chars if they are allocated
 * in the nursery, so it's best to avoid using this class unless you really
 * need it. It's usually more efficient to use the latin1Chars/twoByteChars
 * JSString methods and often the code can be rewritten so that only indexes
 * instead of char pointers are used in parts of the code that can GC.
 */
class MOZ_STACK_CLASS AutoStableStringChars
{
    /* Ensure the string is kept alive while we're using its chars. */
    JS::RootedString s_;
    union {
        const char16_t* twoByteChars_;
        const JS::Latin1Char* latin1Chars_;
    };
    enum State { Uninitialized, Latin1, TwoByte };
    State state_;
    bool ownsChars_;

  public:
    explicit AutoStableStringChars(JSContext* cx)
      : s_(cx), state_(Uninitialized), ownsChars_(false)
    {}
    ~AutoStableStringChars();

    MOZ_WARN_UNUSED_RESULT
    bool init(JSContext* cx, JSString* s);

    /* Like init(), but Latin1 chars are inflated to TwoByte. */
    MOZ_WARN_UNUSED_RESULT
    bool initTwoByte(JSContext* cx, JSString* s);

    bool isLatin1() const { return state_ == Latin1; }
    bool isTwoByte() const { return state_ == TwoByte; }

    const char16_t* twoByteChars() const {
        MOZ_ASSERT(state_ == TwoByte);
        return twoByteChars_;
    }

    mozilla::Range<const JS::Latin1Char> latin1Range() const {
        MOZ_ASSERT(state_ == Latin1);
        return mozilla::Range<const JS::Latin1Char>(latin1Chars_,
                                                    GetStringLength(s_));
    }

    mozilla::Range<const char16_t> twoByteRange() const {
        MOZ_ASSERT(state_ == TwoByte);
        return mozilla::Range<const char16_t>(twoByteChars_,
                                            GetStringLength(s_));
    }

    /* If we own the chars, transfer ownership to the caller. */
    bool maybeGiveOwnershipToCaller() {
        MOZ_ASSERT(state_ != Uninitialized);
        if (!ownsChars_)
            return false;
        state_ = Uninitialized;
        ownsChars_ = false;
        return true;
    }

  private:
    AutoStableStringChars(const AutoStableStringChars& other) = delete;
    void operator=(const AutoStableStringChars& other) = delete;
};

/**
 * Creates a string of the form |ErrorType: ErrorMessage| for a JSErrorReport,
 * which generally matches the toString() behavior of an ErrorObject.
 */
extern JS_FRIEND_API(JSString*)
ErrorReportToString(JSContext* cx, JSErrorReport* reportp);

struct MOZ_STACK_CLASS JS_FRIEND_API(ErrorReport)
{
    explicit ErrorReport(JSContext* cx);
    ~ErrorReport();

    bool init(JSContext* cx, JS::HandleValue exn);

    JSErrorReport* report()
    {
        return reportp;
    }

    const char* message()
    {
        return message_;
    }

  private:
    // More or less an equivalent of JS_ReportErrorNumber/js::ReportErrorNumberVA
    // but fills in an ErrorReport instead of reporting it.  Uses varargs to
    // make it simpler to call js::ExpandErrorArgumentsVA.
    //
    // Returns false if we fail to actually populate the ErrorReport
    // for some reason (probably out of memory).
    bool populateUncaughtExceptionReport(JSContext* cx, ...);
    bool populateUncaughtExceptionReportVA(JSContext* cx, va_list ap);

    // Reports exceptions from add-on scopes to telementry.
    void ReportAddonExceptionToTelementry(JSContext* cx);

    // We may have a provided JSErrorReport, so need a way to represent that.
    JSErrorReport* reportp;

    // And we may have a message.
    const char* message_;

    // Or we may need to synthesize a JSErrorReport one of our own.
    JSErrorReport ownedReport;

    // Or a message of our own.  If this is non-null, we need to clean up both
    // it and ownedReport.
    char* ownedMessage;

    // And we have a string to maybe keep alive that has pointers into
    // it from ownedReport.
    JS::RootedString str;

    // And keep its chars alive too.
    AutoStableStringChars strChars;

    // And we need to root our exception value.
    JS::RootedObject exnObject;

    // And possibly some byte storage for our message_.
    JSAutoByteString bytesStorage;

    // And for our filename.
    JSAutoByteString filename;

    // True if we need to free message_ and the stuff in ownedReport
    bool ownsMessageAndReport;
};

/* Implemented in vm/StructuredClone.cpp. */
extern JS_FRIEND_API(uint64_t)
GetSCOffset(JSStructuredCloneWriter* writer);

namespace Scalar {

/**
 * Scalar types that can appear in typed arrays and typed objects.  The enum
 * values must to be kept in sync with the JS_SCALARTYPEREPR_ constants, as
 * well as the TypedArrayObject::classes and TypedArrayObject::protoClasses
 * definitions.
 */
enum Type {
    Int8 = 0,
    Uint8,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Float32,
    Float64,

    /**
     * Special type that is a uint8_t, but assignments are clamped to [0, 256).
     * Treat the raw data type as a uint8_t.
     */
    Uint8Clamped,

    /**
     * SIMD types don't have their own TypedArray equivalent, for now.
     */
    MaxTypedArrayViewType,

    Float32x4,
    Int32x4
};

static inline size_t
byteSize(Type atype)
{
    switch (atype) {
      case Int8:
      case Uint8:
      case Uint8Clamped:
        return 1;
      case Int16:
      case Uint16:
        return 2;
      case Int32:
      case Uint32:
      case Float32:
        return 4;
      case Float64:
        return 8;
      case Int32x4:
      case Float32x4:
        return 16;
      default:
        MOZ_CRASH("invalid scalar type");
    }
}

static inline bool
isSignedIntType(Type atype) {
    switch (atype) {
      case Int8:
      case Int16:
      case Int32:
      case Int32x4:
        return true;
      case Uint8:
      case Uint8Clamped:
      case Uint16:
      case Uint32:
      case Float32:
      case Float64:
      case Float32x4:
        return false;
      default:
        MOZ_CRASH("invalid scalar type");
    }
}

static inline bool
isSimdType(Type atype) {
    switch (atype) {
      case Int8:
      case Uint8:
      case Uint8Clamped:
      case Int16:
      case Uint16:
      case Int32:
      case Uint32:
      case Float32:
      case Float64:
        return false;
      case Int32x4:
      case Float32x4:
        return true;
      case MaxTypedArrayViewType:
        break;
    }
    MOZ_CRASH("invalid scalar type");
}

static inline size_t
scalarByteSize(Type atype) {
    switch (atype) {
      case Int32x4:
      case Float32x4:
        return 4;
      case Int8:
      case Uint8:
      case Uint8Clamped:
      case Int16:
      case Uint16:
      case Int32:
      case Uint32:
      case Float32:
      case Float64:
      case MaxTypedArrayViewType:
        break;
    }
    MOZ_CRASH("invalid simd type");
}

} /* namespace Scalar */
} /* namespace js */

/*
 * Create a new typed array with nelements elements.
 *
 * These functions (except the WithBuffer variants) fill in the array with zeros.
 */

extern JS_FRIEND_API(JSObject*)
JS_NewInt8Array(JSContext* cx, uint32_t nelements);
extern JS_FRIEND_API(JSObject*)
JS_NewUint8Array(JSContext* cx, uint32_t nelements);
extern JS_FRIEND_API(JSObject*)
JS_NewUint8ClampedArray(JSContext* cx, uint32_t nelements);
extern JS_FRIEND_API(JSObject*)
JS_NewInt16Array(JSContext* cx, uint32_t nelements);
extern JS_FRIEND_API(JSObject*)
JS_NewUint16Array(JSContext* cx, uint32_t nelements);
extern JS_FRIEND_API(JSObject*)
JS_NewInt32Array(JSContext* cx, uint32_t nelements);
extern JS_FRIEND_API(JSObject*)
JS_NewUint32Array(JSContext* cx, uint32_t nelements);
extern JS_FRIEND_API(JSObject*)
JS_NewFloat32Array(JSContext* cx, uint32_t nelements);
extern JS_FRIEND_API(JSObject*)
JS_NewFloat64Array(JSContext* cx, uint32_t nelements);

/*
 * Create a new typed array and copy in values from the given object. The
 * object is used as if it were an array; that is, the new array (if
 * successfully created) will have length given by array.length, and its
 * elements will be those specified by array[0], array[1], and so on, after
 * conversion to the typed array element type.
 */

extern JS_FRIEND_API(JSObject*)
JS_NewInt8ArrayFromArray(JSContext* cx, JS::HandleObject array);
extern JS_FRIEND_API(JSObject*)
JS_NewUint8ArrayFromArray(JSContext* cx, JS::HandleObject array);
extern JS_FRIEND_API(JSObject*)
JS_NewUint8ClampedArrayFromArray(JSContext* cx, JS::HandleObject array);
extern JS_FRIEND_API(JSObject*)
JS_NewInt16ArrayFromArray(JSContext* cx, JS::HandleObject array);
extern JS_FRIEND_API(JSObject*)
JS_NewUint16ArrayFromArray(JSContext* cx, JS::HandleObject array);
extern JS_FRIEND_API(JSObject*)
JS_NewInt32ArrayFromArray(JSContext* cx, JS::HandleObject array);
extern JS_FRIEND_API(JSObject*)
JS_NewUint32ArrayFromArray(JSContext* cx, JS::HandleObject array);
extern JS_FRIEND_API(JSObject*)
JS_NewFloat32ArrayFromArray(JSContext* cx, JS::HandleObject array);
extern JS_FRIEND_API(JSObject*)
JS_NewFloat64ArrayFromArray(JSContext* cx, JS::HandleObject array);

/*
 * Create a new typed array using the given ArrayBuffer or
 * SharedArrayBuffer for storage.  The length value is optional; if -1
 * is passed, enough elements to use up the remainder of the byte
 * array is used as the default value.
 */

extern JS_FRIEND_API(JSObject*)
JS_NewInt8ArrayWithBuffer(JSContext* cx, JS::HandleObject arrayBuffer,
                          uint32_t byteOffset, int32_t length);
extern JS_FRIEND_API(JSObject*)
JS_NewUint8ArrayWithBuffer(JSContext* cx, JS::HandleObject arrayBuffer,
                           uint32_t byteOffset, int32_t length);
extern JS_FRIEND_API(JSObject*)
JS_NewUint8ClampedArrayWithBuffer(JSContext* cx, JS::HandleObject arrayBuffer,
                                  uint32_t byteOffset, int32_t length);
extern JS_FRIEND_API(JSObject*)
JS_NewInt16ArrayWithBuffer(JSContext* cx, JS::HandleObject arrayBuffer,
                           uint32_t byteOffset, int32_t length);
extern JS_FRIEND_API(JSObject*)
JS_NewUint16ArrayWithBuffer(JSContext* cx, JS::HandleObject arrayBuffer,
                            uint32_t byteOffset, int32_t length);
extern JS_FRIEND_API(JSObject*)
JS_NewInt32ArrayWithBuffer(JSContext* cx, JS::HandleObject arrayBuffer,
                           uint32_t byteOffset, int32_t length);
extern JS_FRIEND_API(JSObject*)
JS_NewUint32ArrayWithBuffer(JSContext* cx, JS::HandleObject arrayBuffer,
                            uint32_t byteOffset, int32_t length);
extern JS_FRIEND_API(JSObject*)
JS_NewFloat32ArrayWithBuffer(JSContext* cx, JS::HandleObject arrayBuffer,
                             uint32_t byteOffset, int32_t length);
extern JS_FRIEND_API(JSObject*)
JS_NewFloat64ArrayWithBuffer(JSContext* cx, JS::HandleObject arrayBuffer,
                             uint32_t byteOffset, int32_t length);

/**
 * Create a new SharedArrayBuffer with the given byte length.
 */
extern JS_FRIEND_API(JSObject*)
JS_NewSharedArrayBuffer(JSContext* cx, uint32_t nbytes);

/**
 * Create a new ArrayBuffer with the given byte length.
 */
extern JS_FRIEND_API(JSObject*)
JS_NewArrayBuffer(JSContext* cx, uint32_t nbytes);

/**
 * Check whether obj supports JS_GetTypedArray* APIs. Note that this may return
 * false if a security wrapper is encountered that denies the unwrapping. If
 * this test or one of the JS_Is*Array tests succeeds, then it is safe to call
 * the various accessor JSAPI calls defined below.
 */
extern JS_FRIEND_API(bool)
JS_IsTypedArrayObject(JSObject* obj);

/**
 * Check whether obj supports JS_GetArrayBufferView* APIs. Note that this may
 * return false if a security wrapper is encountered that denies the
 * unwrapping. If this test or one of the more specific tests succeeds, then it
 * is safe to call the various ArrayBufferView accessor JSAPI calls defined
 * below.
 */
extern JS_FRIEND_API(bool)
JS_IsArrayBufferViewObject(JSObject* obj);

/*
 * Test for specific typed array types (ArrayBufferView subtypes)
 */

extern JS_FRIEND_API(bool)
JS_IsInt8Array(JSObject* obj);
extern JS_FRIEND_API(bool)
JS_IsUint8Array(JSObject* obj);
extern JS_FRIEND_API(bool)
JS_IsUint8ClampedArray(JSObject* obj);
extern JS_FRIEND_API(bool)
JS_IsInt16Array(JSObject* obj);
extern JS_FRIEND_API(bool)
JS_IsUint16Array(JSObject* obj);
extern JS_FRIEND_API(bool)
JS_IsInt32Array(JSObject* obj);
extern JS_FRIEND_API(bool)
JS_IsUint32Array(JSObject* obj);
extern JS_FRIEND_API(bool)
JS_IsFloat32Array(JSObject* obj);
extern JS_FRIEND_API(bool)
JS_IsFloat64Array(JSObject* obj);

/**
 * Return the isShared flag of a typed array, which denotes whether
 * the underlying buffer is a SharedArrayBuffer.
 *
 * |obj| must have passed a JS_IsTypedArrayObject/JS_Is*Array test, or somehow
 * be known that it would pass such a test: it is a typed array or a wrapper of
 * a typed array, and the unwrapping will succeed.
 */
extern JS_FRIEND_API(bool)
JS_GetTypedArraySharedness(JSObject* obj);

/*
 * Test for specific typed array types (ArrayBufferView subtypes) and return
 * the unwrapped object if so, else nullptr.  Never throws.
 */

namespace js {

extern JS_FRIEND_API(JSObject*)
UnwrapInt8Array(JSObject* obj);
extern JS_FRIEND_API(JSObject*)
UnwrapUint8Array(JSObject* obj);
extern JS_FRIEND_API(JSObject*)
UnwrapUint8ClampedArray(JSObject* obj);
extern JS_FRIEND_API(JSObject*)
UnwrapInt16Array(JSObject* obj);
extern JS_FRIEND_API(JSObject*)
UnwrapUint16Array(JSObject* obj);
extern JS_FRIEND_API(JSObject*)
UnwrapInt32Array(JSObject* obj);
extern JS_FRIEND_API(JSObject*)
UnwrapUint32Array(JSObject* obj);
extern JS_FRIEND_API(JSObject*)
UnwrapFloat32Array(JSObject* obj);
extern JS_FRIEND_API(JSObject*)
UnwrapFloat64Array(JSObject* obj);

extern JS_FRIEND_API(JSObject*)
UnwrapArrayBuffer(JSObject* obj);

extern JS_FRIEND_API(JSObject*)
UnwrapArrayBufferView(JSObject* obj);

extern JS_FRIEND_API(JSObject*)
UnwrapSharedArrayBuffer(JSObject* obj);


namespace detail {

extern JS_FRIEND_DATA(const Class* const) Int8ArrayClassPtr;
extern JS_FRIEND_DATA(const Class* const) Uint8ArrayClassPtr;
extern JS_FRIEND_DATA(const Class* const) Uint8ClampedArrayClassPtr;
extern JS_FRIEND_DATA(const Class* const) Int16ArrayClassPtr;
extern JS_FRIEND_DATA(const Class* const) Uint16ArrayClassPtr;
extern JS_FRIEND_DATA(const Class* const) Int32ArrayClassPtr;
extern JS_FRIEND_DATA(const Class* const) Uint32ArrayClassPtr;
extern JS_FRIEND_DATA(const Class* const) Float32ArrayClassPtr;
extern JS_FRIEND_DATA(const Class* const) Float64ArrayClassPtr;

const size_t TypedArrayLengthSlot = 1;

} // namespace detail

#define JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Type, type) \
inline void \
Get ## Type ## ArrayLengthAndData(JSObject* obj, uint32_t* length, bool* isSharedMemory, type** data) \
{ \
    MOZ_ASSERT(GetObjectClass(obj) == detail::Type ## ArrayClassPtr); \
    const JS::Value& lenSlot = GetReservedSlot(obj, detail::TypedArrayLengthSlot); \
    *length = mozilla::AssertedCast<uint32_t>(lenSlot.toInt32()); \
    *isSharedMemory = JS_GetTypedArraySharedness(obj); \
    *data = static_cast<type*>(GetObjectPrivate(obj)); \
}

JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Int8, int8_t)
JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Uint8, uint8_t)
JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Uint8Clamped, uint8_t)
JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Int16, int16_t)
JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Uint16, uint16_t)
JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Int32, int32_t)
JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Uint32, uint32_t)
JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Float32, float)
JS_DEFINE_DATA_AND_LENGTH_ACCESSOR(Float64, double)

#undef JS_DEFINE_DATA_AND_LENGTH_ACCESSOR

// This one isn't inlined because it's rather tricky (by dint of having to deal
// with a dozen-plus classes and varying slot layouts.
extern JS_FRIEND_API(void)
GetArrayBufferViewLengthAndData(JSObject* obj, uint32_t* length, bool* isSharedMemory, uint8_t** data);

// This one isn't inlined because there are a bunch of different ArrayBuffer
// classes that would have to be individually handled here.
//
// There is an isShared out argument for API consistency (eases use from DOM).
// It will always be set to false.
extern JS_FRIEND_API(void)
GetArrayBufferLengthAndData(JSObject* obj, uint32_t* length, bool* isSharedMemory, uint8_t** data);

// Ditto for SharedArrayBuffer.
//
// There is an isShared out argument for API consistency (eases use from DOM).
// It will always be set to true.
extern JS_FRIEND_API(void)
GetSharedArrayBufferLengthAndData(JSObject* obj, uint32_t* length, bool* isSharedMemory, uint8_t** data);

} // namespace js

JS_FRIEND_API(uint8_t*)
JS_GetSharedArrayBufferData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&);

/*
 * Unwrap Typed arrays all at once. Return nullptr without throwing if the
 * object cannot be viewed as the correct typed array, or the typed array
 * object on success, filling both outparameters.
 */
extern JS_FRIEND_API(JSObject*)
JS_GetObjectAsInt8Array(JSObject* obj, uint32_t* length, bool* isSharedMemory, int8_t** data);
extern JS_FRIEND_API(JSObject*)
JS_GetObjectAsUint8Array(JSObject* obj, uint32_t* length, bool* isSharedMemory, uint8_t** data);
extern JS_FRIEND_API(JSObject*)
JS_GetObjectAsUint8ClampedArray(JSObject* obj, uint32_t* length, bool* isSharedMemory, uint8_t** data);
extern JS_FRIEND_API(JSObject*)
JS_GetObjectAsInt16Array(JSObject* obj, uint32_t* length, bool* isSharedMemory, int16_t** data);
extern JS_FRIEND_API(JSObject*)
JS_GetObjectAsUint16Array(JSObject* obj, uint32_t* length, bool* isSharedMemory, uint16_t** data);
extern JS_FRIEND_API(JSObject*)
JS_GetObjectAsInt32Array(JSObject* obj, uint32_t* length, bool* isSharedMemory, int32_t** data);
extern JS_FRIEND_API(JSObject*)
JS_GetObjectAsUint32Array(JSObject* obj, uint32_t* length, bool* isSharedMemory, uint32_t** data);
extern JS_FRIEND_API(JSObject*)
JS_GetObjectAsFloat32Array(JSObject* obj, uint32_t* length, bool* isSharedMemory, float** data);
extern JS_FRIEND_API(JSObject*)
JS_GetObjectAsFloat64Array(JSObject* obj, uint32_t* length, bool* isSharedMemory, double** data);
extern JS_FRIEND_API(JSObject*)
JS_GetObjectAsArrayBufferView(JSObject* obj, uint32_t* length, bool* isSharedMemory, uint8_t** data);

/*
 * Unwrap an ArrayBuffer, return nullptr if it's a different type.
 */
extern JS_FRIEND_API(JSObject*)
JS_GetObjectAsArrayBuffer(JSObject* obj, uint32_t* length, uint8_t** data);

/*
 * Get the type of elements in a typed array, or MaxTypedArrayViewType if a DataView.
 *
 * |obj| must have passed a JS_IsArrayBufferView/JS_Is*Array test, or somehow
 * be known that it would pass such a test: it is an ArrayBufferView or a
 * wrapper of an ArrayBufferView, and the unwrapping will succeed.
 */
extern JS_FRIEND_API(js::Scalar::Type)
JS_GetArrayBufferViewType(JSObject* obj);

extern JS_FRIEND_API(js::Scalar::Type)
JS_GetSharedArrayBufferViewType(JSObject* obj);

/*
 * Check whether obj supports the JS_GetArrayBuffer* APIs. Note that this may
 * return false if a security wrapper is encountered that denies the
 * unwrapping. If this test succeeds, then it is safe to call the various
 * accessor JSAPI calls defined below.
 */
extern JS_FRIEND_API(bool)
JS_IsArrayBufferObject(JSObject* obj);

extern JS_FRIEND_API(bool)
JS_IsSharedArrayBufferObject(JSObject* obj);

/**
 * Return the available byte length of an array buffer.
 *
 * |obj| must have passed a JS_IsArrayBufferObject test, or somehow be known
 * that it would pass such a test: it is an ArrayBuffer or a wrapper of an
 * ArrayBuffer, and the unwrapping will succeed.
 */
extern JS_FRIEND_API(uint32_t)
JS_GetArrayBufferByteLength(JSObject* obj);

extern JS_FRIEND_API(uint32_t)
JS_GetSharedArrayBufferByteLength(JSObject* obj);

/**
 * Return true if the arrayBuffer contains any data. This will return false for
 * ArrayBuffer.prototype and neutered ArrayBuffers.
 *
 * |obj| must have passed a JS_IsArrayBufferObject test, or somehow be known
 * that it would pass such a test: it is an ArrayBuffer or a wrapper of an
 * ArrayBuffer, and the unwrapping will succeed.
 */
extern JS_FRIEND_API(bool)
JS_ArrayBufferHasData(JSObject* obj);

/**
 * Return a pointer to the start of the data referenced by a typed array. The
 * data is still owned by the typed array, and should not be modified on
 * another thread. Furthermore, the pointer can become invalid on GC (if the
 * data is small and fits inside the array's GC header), so callers must take
 * care not to hold on across anything that could GC.
 *
 * |obj| must have passed a JS_IsArrayBufferObject test, or somehow be known
 * that it would pass such a test: it is an ArrayBuffer or a wrapper of an
 * ArrayBuffer, and the unwrapping will succeed.
 *
 * *isSharedMemory will be set to false, the argument is present to simplify
 * its use from code that also interacts with SharedArrayBuffer.
 */
extern JS_FRIEND_API(uint8_t*)
JS_GetArrayBufferData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&);

/**
 * Check whether the obj is ArrayBufferObject and memory mapped. Note that this
 * may return false if a security wrapper is encountered that denies the
 * unwrapping.
 */
extern JS_FRIEND_API(bool)
JS_IsMappedArrayBufferObject(JSObject* obj);

/**
 * Return the number of elements in a typed array.
 *
 * |obj| must have passed a JS_IsTypedArrayObject/JS_Is*Array test, or somehow
 * be known that it would pass such a test: it is a typed array or a wrapper of
 * a typed array, and the unwrapping will succeed.
 */
extern JS_FRIEND_API(uint32_t)
JS_GetTypedArrayLength(JSObject* obj);

/**
 * Return the byte offset from the start of an array buffer to the start of a
 * typed array view.
 *
 * |obj| must have passed a JS_IsTypedArrayObject/JS_Is*Array test, or somehow
 * be known that it would pass such a test: it is a typed array or a wrapper of
 * a typed array, and the unwrapping will succeed.
 */
extern JS_FRIEND_API(uint32_t)
JS_GetTypedArrayByteOffset(JSObject* obj);

/**
 * Return the byte length of a typed array.
 *
 * |obj| must have passed a JS_IsTypedArrayObject/JS_Is*Array test, or somehow
 * be known that it would pass such a test: it is a typed array or a wrapper of
 * a typed array, and the unwrapping will succeed.
 */
extern JS_FRIEND_API(uint32_t)
JS_GetTypedArrayByteLength(JSObject* obj);

/**
 * Check whether obj supports JS_ArrayBufferView* APIs. Note that this may
 * return false if a security wrapper is encountered that denies the
 * unwrapping.
 */
extern JS_FRIEND_API(bool)
JS_IsArrayBufferViewObject(JSObject* obj);

/**
 * More generic name for JS_GetTypedArrayByteLength to cover DataViews as well
 */
extern JS_FRIEND_API(uint32_t)
JS_GetArrayBufferViewByteLength(JSObject* obj);

/*
 * Return a pointer to the start of the data referenced by a typed array. The
 * data is still owned by the typed array, and should not be modified on
 * another thread. Furthermore, the pointer can become invalid on GC (if the
 * data is small and fits inside the array's GC header), so callers must take
 * care not to hold on across anything that could GC.
 *
 * |obj| must have passed a JS_Is*Array test, or somehow be known that it would
 * pass such a test: it is a typed array or a wrapper of a typed array, and the
 * unwrapping will succeed.
 *
 * *isSharedMemory will be set to true if the typed array maps a
 * SharedArrayBuffer, otherwise to false.
 */

extern JS_FRIEND_API(int8_t*)
JS_GetInt8ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&);
extern JS_FRIEND_API(uint8_t*)
JS_GetUint8ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&);
extern JS_FRIEND_API(uint8_t*)
JS_GetUint8ClampedArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&);
extern JS_FRIEND_API(int16_t*)
JS_GetInt16ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&);
extern JS_FRIEND_API(uint16_t*)
JS_GetUint16ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&);
extern JS_FRIEND_API(int32_t*)
JS_GetInt32ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&);
extern JS_FRIEND_API(uint32_t*)
JS_GetUint32ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&);
extern JS_FRIEND_API(float*)
JS_GetFloat32ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&);
extern JS_FRIEND_API(double*)
JS_GetFloat64ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&);

/**
 * Same as above, but for any kind of ArrayBufferView. Prefer the type-specific
 * versions when possible.
 */
extern JS_FRIEND_API(void*)
JS_GetArrayBufferViewData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&);

/**
 * Return the ArrayBuffer or SharedArrayBuffer underlying an
 * ArrayBufferView. If the buffer has been neutered, this will still
 * return the neutered buffer. |obj| must be an object that would
 * return true for JS_IsArrayBufferViewObject().
 */
extern JS_FRIEND_API(JSObject*)
JS_GetArrayBufferViewBuffer(JSContext* cx, JS::HandleObject obj, bool* isSharedMemory);

typedef enum {
    ChangeData,
    KeepData
} NeuterDataDisposition;

/**
 * Set an ArrayBuffer's length to 0 and neuter all of its views.
 *
 * The |changeData| argument is a hint to inform internal behavior with respect
 * to the internal pointer to the ArrayBuffer's data after being neutered.
 * There is no guarantee it will be respected.  But if it is respected, the
 * ArrayBuffer's internal data pointer will, or will not, have changed
 * accordingly.
 */
extern JS_FRIEND_API(bool)
JS_NeuterArrayBuffer(JSContext* cx, JS::HandleObject obj,
                     NeuterDataDisposition changeData);

/**
 * Check whether the obj is ArrayBufferObject and neutered. Note that this
 * may return false if a security wrapper is encountered that denies the
 * unwrapping.
 */
extern JS_FRIEND_API(bool)
JS_IsNeuteredArrayBufferObject(JSObject* obj);

/**
 * Check whether obj supports JS_GetDataView* APIs.
 */
JS_FRIEND_API(bool)
JS_IsDataViewObject(JSObject* obj);

/**
 * Create a new DataView using the given ArrayBuffer for storage. The given
 * buffer must be an ArrayBuffer (or a cross-compartment wrapper of an
 * ArrayBuffer), and the offset and length must fit within the bounds of the
 * arrayBuffer. Currently, nullptr will be returned and an exception will be
 * thrown if these conditions do not hold, but do not depend on that behavior.
 */
JS_FRIEND_API(JSObject*)
JS_NewDataView(JSContext* cx, JS::HandleObject arrayBuffer, uint32_t byteOffset, int32_t byteLength);

/**
 * Return the byte offset of a data view into its array buffer. |obj| must be a
 * DataView.
 *
 * |obj| must have passed a JS_IsDataViewObject test, or somehow be known that
 * it would pass such a test: it is a data view or a wrapper of a data view,
 * and the unwrapping will succeed.
 */
JS_FRIEND_API(uint32_t)
JS_GetDataViewByteOffset(JSObject* obj);

/**
 * Return the byte length of a data view.
 *
 * |obj| must have passed a JS_IsDataViewObject test, or somehow be known that
 * it would pass such a test: it is a data view or a wrapper of a data view,
 * and the unwrapping will succeed. If cx is nullptr, then DEBUG builds may be
 * unable to assert when unwrapping should be disallowed.
 */
JS_FRIEND_API(uint32_t)
JS_GetDataViewByteLength(JSObject* obj);

/**
 * Return a pointer to the beginning of the data referenced by a DataView.
 *
 * |obj| must have passed a JS_IsDataViewObject test, or somehow be known that
 * it would pass such a test: it is a data view or a wrapper of a data view,
 * and the unwrapping will succeed. If cx is nullptr, then DEBUG builds may be
 * unable to assert when unwrapping should be disallowed.
 */
JS_FRIEND_API(void*)
JS_GetDataViewData(JSObject* obj, const JS::AutoCheckCannotGC&);

namespace js {

/**
 * Add a watchpoint -- in the Object.prototype.watch sense -- to |obj| for the
 * property |id|, using the callable object |callable| as the function to be
 * called for notifications.
 *
 * This is an internal function exposed -- temporarily -- only so that DOM
 * proxies can be watchable.  Don't use it!  We'll soon kill off the
 * Object.prototype.{,un}watch functions, at which point this will go too.
 */
extern JS_FRIEND_API(bool)
WatchGuts(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleObject callable);

/**
 * Remove a watchpoint -- in the Object.prototype.watch sense -- from |obj| for
 * the property |id|.
 *
 * This is an internal function exposed -- temporarily -- only so that DOM
 * proxies can be watchable.  Don't use it!  We'll soon kill off the
 * Object.prototype.{,un}watch functions, at which point this will go too.
 */
extern JS_FRIEND_API(bool)
UnwatchGuts(JSContext* cx, JS::HandleObject obj, JS::HandleId id);

namespace jit {

enum class InlinableNative : uint16_t;

} // namespace jit

} // namespace js

/**
 * A class, expected to be passed by value, which represents the CallArgs for a
 * JSJitGetterOp.
 */
class JSJitGetterCallArgs : protected JS::MutableHandleValue
{
  public:
    explicit JSJitGetterCallArgs(const JS::CallArgs& args)
      : JS::MutableHandleValue(args.rval())
    {}

    explicit JSJitGetterCallArgs(JS::RootedValue* rooted)
      : JS::MutableHandleValue(rooted)
    {}

    JS::MutableHandleValue rval() {
        return *this;
    }
};

/**
 * A class, expected to be passed by value, which represents the CallArgs for a
 * JSJitSetterOp.
 */
class JSJitSetterCallArgs : protected JS::MutableHandleValue
{
  public:
    explicit JSJitSetterCallArgs(const JS::CallArgs& args)
      : JS::MutableHandleValue(args[0])
    {}

    JS::MutableHandleValue operator[](unsigned i) {
        MOZ_ASSERT(i == 0);
        return *this;
    }

    unsigned length() const { return 1; }

    // Add get() or maybe hasDefined() as needed
};

struct JSJitMethodCallArgsTraits;

/**
 * A class, expected to be passed by reference, which represents the CallArgs
 * for a JSJitMethodOp.
 */
class JSJitMethodCallArgs : protected JS::detail::CallArgsBase<JS::detail::NoUsedRval>
{
  private:
    typedef JS::detail::CallArgsBase<JS::detail::NoUsedRval> Base;
    friend struct JSJitMethodCallArgsTraits;

  public:
    explicit JSJitMethodCallArgs(const JS::CallArgs& args) {
        argv_ = args.array();
        argc_ = args.length();
    }

    JS::MutableHandleValue rval() const {
        return Base::rval();
    }

    unsigned length() const { return Base::length(); }

    JS::MutableHandleValue operator[](unsigned i) const {
        return Base::operator[](i);
    }

    bool hasDefined(unsigned i) const {
        return Base::hasDefined(i);
    }

    JSObject& callee() const {
        // We can't use Base::callee() because that will try to poke at
        // this->usedRval_, which we don't have.
        return argv_[-2].toObject();
    }

    JS::HandleValue get(unsigned i) const {
        return Base::get(i);
    }
};

struct JSJitMethodCallArgsTraits
{
    static const size_t offsetOfArgv = offsetof(JSJitMethodCallArgs, argv_);
    static const size_t offsetOfArgc = offsetof(JSJitMethodCallArgs, argc_);
};

typedef bool
(* JSJitGetterOp)(JSContext* cx, JS::HandleObject thisObj,
                  void* specializedThis, JSJitGetterCallArgs args);
typedef bool
(* JSJitSetterOp)(JSContext* cx, JS::HandleObject thisObj,
                  void* specializedThis, JSJitSetterCallArgs args);
typedef bool
(* JSJitMethodOp)(JSContext* cx, JS::HandleObject thisObj,
                  void* specializedThis, const JSJitMethodCallArgs& args);

/**
 * This struct contains metadata passed from the DOM to the JS Engine for JIT
 * optimizations on DOM property accessors. Eventually, this should be made
 * available to general JSAPI users, but we are not currently ready to do so.
 */
struct JSJitInfo {
    enum OpType {
        Getter,
        Setter,
        Method,
        StaticMethod,
        InlinableNative,
        // Must be last
        OpTypeCount
    };

    enum ArgType {
        // Basic types
        String = (1 << 0),
        Integer = (1 << 1), // Only 32-bit or less
        Double = (1 << 2), // Maybe we want to add Float sometime too
        Boolean = (1 << 3),
        Object = (1 << 4),
        Null = (1 << 5),

        // And derived types
        Numeric = Integer | Double,
        // Should "Primitive" use the WebIDL definition, which
        // excludes string and null, or the typical JS one that includes them?
        Primitive = Numeric | Boolean | Null | String,
        ObjectOrNull = Object | Null,
        Any = ObjectOrNull | Primitive,

        // Our sentinel value.
        ArgTypeListEnd = (1 << 31)
    };

    static_assert(Any & String, "Any must include String.");
    static_assert(Any & Integer, "Any must include Integer.");
    static_assert(Any & Double, "Any must include Double.");
    static_assert(Any & Boolean, "Any must include Boolean.");
    static_assert(Any & Object, "Any must include Object.");
    static_assert(Any & Null, "Any must include Null.");

    /**
     * An enum that describes what this getter/setter/method aliases.  This
     * determines what things can be hoisted past this call, and if this
     * call is movable what it can be hoisted past.
     */
    enum AliasSet {
        /**
         * Alias nothing: a constant value, getting it can't affect any other
         * values, nothing can affect it.
         */
        AliasNone,

        /**
         * Alias things that can modify the DOM but nothing else.  Doing the
         * call can't affect the behavior of any other function.
         */
        AliasDOMSets,

        /**
         * Alias the world.  Calling this can change arbitrary values anywhere
         * in the system.  Most things fall in this bucket.
         */
        AliasEverything,

        /** Must be last. */
        AliasSetCount
    };

    bool needsOuterizedThisObject() const
    {
        return type() != Getter && type() != Setter;
    }

    bool isTypedMethodJitInfo() const
    {
        return isTypedMethod;
    }

    OpType type() const
    {
        return OpType(type_);
    }

    AliasSet aliasSet() const
    {
        return AliasSet(aliasSet_);
    }

    JSValueType returnType() const
    {
        return JSValueType(returnType_);
    }

    union {
        JSJitGetterOp getter;
        JSJitSetterOp setter;
        JSJitMethodOp method;
        /** A DOM static method, used for Promise wrappers */
        JSNative staticMethod;
    };

    union {
        uint16_t protoID;
        js::jit::InlinableNative inlinableNative;
    };

    uint16_t depth;

    // These fields are carefully packed to take up 4 bytes.  If you need more
    // bits for whatever reason, please see if you can steal bits from existing
    // fields before adding more members to this structure.

#define JITINFO_OP_TYPE_BITS 4
#define JITINFO_ALIAS_SET_BITS 4
#define JITINFO_RETURN_TYPE_BITS 8
#define JITINFO_SLOT_INDEX_BITS 10

    /** The OpType that says what sort of function we are. */
    uint32_t type_ : JITINFO_OP_TYPE_BITS;

    /**
     * The alias set for this op.  This is a _minimal_ alias set; in
     * particular for a method it does not include whatever argument
     * conversions might do.  That's covered by argTypes and runtime
     * analysis of the actual argument types being passed in.
     */
    uint32_t aliasSet_ : JITINFO_ALIAS_SET_BITS;

    /** The return type tag.  Might be JSVAL_TYPE_UNKNOWN. */
    uint32_t returnType_ : JITINFO_RETURN_TYPE_BITS;

    static_assert(OpTypeCount <= (1 << JITINFO_OP_TYPE_BITS),
                  "Not enough space for OpType");
    static_assert(AliasSetCount <= (1 << JITINFO_ALIAS_SET_BITS),
                  "Not enough space for AliasSet");
    static_assert((sizeof(JSValueType) * 8) <= JITINFO_RETURN_TYPE_BITS,
                  "Not enough space for JSValueType");

#undef JITINFO_RETURN_TYPE_BITS
#undef JITINFO_ALIAS_SET_BITS
#undef JITINFO_OP_TYPE_BITS

    /** Is op fallible? False in setters. */
    uint32_t isInfallible : 1;

    /**
     * Is op movable?  To be movable the op must
     * not AliasEverything, but even that might
     * not be enough (e.g. in cases when it can
     * throw or is explicitly not movable).
     */
    uint32_t isMovable : 1;

    /**
     * Can op be dead-code eliminated? Again, this
     * depends on whether the op can throw, in
     * addition to the alias set.
     */
    uint32_t isEliminatable : 1;

    // XXXbz should we have a JSValueType for the type of the member?
    /**
     * True if this is a getter that can always
     * get the value from a slot of the "this" object.
     */
    uint32_t isAlwaysInSlot : 1;

    /**
     * True if this is a getter that can sometimes (if the slot doesn't contain
     * UndefinedValue()) get the value from a slot of the "this" object.
     */
    uint32_t isLazilyCachedInSlot : 1;

    /** True if this is an instance of JSTypedMethodJitInfo. */
    uint32_t isTypedMethod : 1;

    /**
     * If isAlwaysInSlot or isSometimesInSlot is true,
     * the index of the slot to get the value from.
     * Otherwise 0.
     */
    uint32_t slotIndex : JITINFO_SLOT_INDEX_BITS;

    static const size_t maxSlotIndex = (1 << JITINFO_SLOT_INDEX_BITS) - 1;

#undef JITINFO_SLOT_INDEX_BITS
};

static_assert(sizeof(JSJitInfo) == (sizeof(void*) + 2 * sizeof(uint32_t)),
              "There are several thousand instances of JSJitInfo stored in "
              "a binary. Please don't increase its space requirements without "
              "verifying that there is no other way forward (better packing, "
              "smaller datatypes for fields, subclassing, etc.).");

struct JSTypedMethodJitInfo
{
    // We use C-style inheritance here, rather than C++ style inheritance
    // because not all compilers support brace-initialization for non-aggregate
    // classes. Using C++ style inheritance and constructors instead of
    // brace-initialization would also force the creation of static
    // constructors (on some compilers) when JSJitInfo and JSTypedMethodJitInfo
    // structures are declared. Since there can be several thousand of these
    // structures present and we want to have roughly equivalent performance
    // across a range of compilers, we do things manually.
    JSJitInfo base;

    const JSJitInfo::ArgType* const argTypes; /* For a method, a list of sets of
                                                 types that the function
                                                 expects.  This can be used,
                                                 for example, to figure out
                                                 when argument coercions can
                                                 have side-effects. */
};

namespace js {

static MOZ_ALWAYS_INLINE shadow::Function*
FunctionObjectToShadowFunction(JSObject* fun)
{
    MOZ_ASSERT(GetObjectClass(fun) == FunctionClassPtr);
    return reinterpret_cast<shadow::Function*>(fun);
}

/* Statically asserted in jsfun.h. */
static const unsigned JS_FUNCTION_INTERPRETED_BITS = 0x0201;

// Return whether the given function object is native.
static MOZ_ALWAYS_INLINE bool
FunctionObjectIsNative(JSObject* fun)
{
    return !(FunctionObjectToShadowFunction(fun)->flags & JS_FUNCTION_INTERPRETED_BITS);
}

static MOZ_ALWAYS_INLINE JSNative
GetFunctionObjectNative(JSObject* fun)
{
    MOZ_ASSERT(FunctionObjectIsNative(fun));
    return FunctionObjectToShadowFunction(fun)->native;
}

} // namespace js

static MOZ_ALWAYS_INLINE const JSJitInfo*
FUNCTION_VALUE_TO_JITINFO(const JS::Value& v)
{
    MOZ_ASSERT(js::FunctionObjectIsNative(&v.toObject()));
    return js::FunctionObjectToShadowFunction(&v.toObject())->jitinfo;
}

static MOZ_ALWAYS_INLINE void
SET_JITINFO(JSFunction * func, const JSJitInfo* info)
{
    js::shadow::Function* fun = reinterpret_cast<js::shadow::Function*>(func);
    MOZ_ASSERT(!(fun->flags & js::JS_FUNCTION_INTERPRETED_BITS));
    fun->jitinfo = info;
}

/*
 * Engine-internal extensions of jsid.  This code is here only until we
 * eliminate Gecko's dependencies on it!
 */

static MOZ_ALWAYS_INLINE jsid
JSID_FROM_BITS(size_t bits)
{
    jsid id;
    JSID_BITS(id) = bits;
    return id;
}

namespace js {
namespace detail {
bool IdMatchesAtom(jsid id, JSAtom* atom);
} // namespace detail
} // namespace js

/**
 * Must not be used on atoms that are representable as integer jsids.
 * Prefer NameToId or AtomToId over this function:
 *
 * A PropertyName is an atom that does not contain an integer in the range
 * [0, UINT32_MAX]. However, jsid can only hold an integer in the range
 * [0, JSID_INT_MAX] (where JSID_INT_MAX == 2^31-1).  Thus, for the range of
 * integers (JSID_INT_MAX, UINT32_MAX], to represent as a jsid 'id', it must be
 * the case JSID_IS_ATOM(id) and !JSID_TO_ATOM(id)->isPropertyName().  In most
 * cases when creating a jsid, code does not have to care about this corner
 * case because:
 *
 * - When given an arbitrary JSAtom*, AtomToId must be used, which checks for
 *   integer atoms representable as integer jsids, and does this conversion.
 *
 * - When given a PropertyName*, NameToId can be used which which does not need
 *   to do any dynamic checks.
 *
 * Thus, it is only the rare third case which needs this function, which
 * handles any JSAtom* that is known not to be representable with an int jsid.
 */
static MOZ_ALWAYS_INLINE jsid
NON_INTEGER_ATOM_TO_JSID(JSAtom* atom)
{
    MOZ_ASSERT(((size_t)atom & 0x7) == 0);
    jsid id = JSID_FROM_BITS((size_t)atom);
    MOZ_ASSERT(js::detail::IdMatchesAtom(id, atom));
    return id;
}

/* All strings stored in jsids are atomized, but are not necessarily property names. */
static MOZ_ALWAYS_INLINE bool
JSID_IS_ATOM(jsid id)
{
    return JSID_IS_STRING(id);
}

static MOZ_ALWAYS_INLINE bool
JSID_IS_ATOM(jsid id, JSAtom* atom)
{
    return id == JSID_FROM_BITS((size_t)atom);
}

static MOZ_ALWAYS_INLINE JSAtom*
JSID_TO_ATOM(jsid id)
{
    return (JSAtom*)JSID_TO_STRING(id);
}

JS_STATIC_ASSERT(sizeof(jsid) == sizeof(void*));

namespace js {

static MOZ_ALWAYS_INLINE JS::Value
IdToValue(jsid id)
{
    if (JSID_IS_STRING(id))
        return JS::StringValue(JSID_TO_STRING(id));
    if (JSID_IS_INT(id))
        return JS::Int32Value(JSID_TO_INT(id));
    if (JSID_IS_SYMBOL(id))
        return JS::SymbolValue(JSID_TO_SYMBOL(id));
    MOZ_ASSERT(JSID_IS_VOID(id));
    return JS::UndefinedValue();
}

/**
 * If the embedder has registered a ScriptEnvironmentPreparer,
 * PrepareScriptEnvironmentAndInvoke will call the preparer's 'invoke' method
 * with the given |closure|, with the assumption that the preparer will set up
 * any state necessary to run script in |scope|, invoke |closure| with a valid
 * JSContext*, report any exceptions thrown from the closure, and return.
 *
 * If no preparer is registered, PrepareScriptEnvironmentAndInvoke will assert
 * that |rt| has exactly one JSContext associated with it, enter the compartment
 * of |scope| on that context, and invoke |closure|.
 *
 * In both cases, PrepareScriptEnvironmentAndInvoke will report any exceptions
 * that are thrown by the closure.  Consumers who want to propagate back
 * whether the closure succeeded should do so via members of the closure
 * itself.
 */

struct ScriptEnvironmentPreparer {
    struct Closure {
        virtual bool operator()(JSContext* cx) = 0;
    };

    virtual void invoke(JS::HandleObject scope, Closure& closure) = 0;
};

extern JS_FRIEND_API(void)
PrepareScriptEnvironmentAndInvoke(JSContext* cx, JS::HandleObject scope,
                                  ScriptEnvironmentPreparer::Closure& closure);

JS_FRIEND_API(void)
SetScriptEnvironmentPreparer(JSRuntime* rt, ScriptEnvironmentPreparer*
preparer);

/**
 * To help embedders enforce their invariants, we allow them to specify in
 * advance which JSContext should be passed to JSAPI calls. If this is set
 * to a non-null value, the assertSameCompartment machinery does double-
 * duty (in debug builds) to verify that it matches the cx being used.
 */
#ifdef DEBUG
JS_FRIEND_API(void)
Debug_SetActiveJSContext(JSRuntime* rt, JSContext* cx);
#else
inline void
Debug_SetActiveJSContext(JSRuntime* rt, JSContext* cx) {}
#endif

enum CTypesActivityType {
    CTYPES_CALL_BEGIN,
    CTYPES_CALL_END,
    CTYPES_CALLBACK_BEGIN,
    CTYPES_CALLBACK_END
};

typedef void
(* CTypesActivityCallback)(JSContext* cx, CTypesActivityType type);

/**
 * Sets a callback that is run whenever js-ctypes is about to be used when
 * calling into C.
 */
JS_FRIEND_API(void)
SetCTypesActivityCallback(JSRuntime* rt, CTypesActivityCallback cb);

class MOZ_RAII JS_FRIEND_API(AutoCTypesActivityCallback) {
  private:
    JSContext* cx;
    CTypesActivityCallback callback;
    CTypesActivityType endType;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

  public:
    AutoCTypesActivityCallback(JSContext* cx, CTypesActivityType beginType,
                               CTypesActivityType endType
                               MOZ_GUARD_OBJECT_NOTIFIER_PARAM);
    ~AutoCTypesActivityCallback() {
        DoEndCallback();
    }
    void DoEndCallback() {
        if (callback) {
            callback(cx, endType);
            callback = nullptr;
        }
    }
};

typedef JSObject*
(* ObjectMetadataCallback)(JSContext* cx, JSObject* obj);

/**
 * Specify a callback to invoke when creating each JS object in the current
 * compartment, which may return a metadata object to associate with the
 * object.
 */
JS_FRIEND_API(void)
SetObjectMetadataCallback(JSContext* cx, ObjectMetadataCallback callback);

/** Get the metadata associated with an object. */
JS_FRIEND_API(JSObject*)
GetObjectMetadata(JSObject* obj);

JS_FRIEND_API(bool)
GetElementsWithAdder(JSContext* cx, JS::HandleObject obj, JS::HandleObject receiver,
                     uint32_t begin, uint32_t end, js::ElementAdder* adder);

JS_FRIEND_API(bool)
ForwardToNative(JSContext* cx, JSNative native, const JS::CallArgs& args);

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
JS_FRIEND_API(bool)
SetPropertyIgnoringNamedGetter(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                               JS::HandleValue v, JS::HandleValue receiver,
                               JS::Handle<JSPropertyDescriptor> ownDesc,
                               JS::ObjectOpResult& result);

JS_FRIEND_API(void)
ReportErrorWithId(JSContext* cx, const char* msg, JS::HandleId id);

// This function is for one specific use case, please don't use this for anything else!
extern JS_FRIEND_API(bool)
ExecuteInGlobalAndReturnScope(JSContext* cx, JS::HandleObject obj, JS::HandleScript script,
                              JS::MutableHandleObject scope);

#if defined(XP_WIN) && defined(_WIN64)
// Parameters use void* types to avoid #including windows.h. The return value of
// this function is returned from the exception handler.
typedef long
(*JitExceptionHandler)(void* exceptionRecord,  // PEXECTION_RECORD
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
extern JS_FRIEND_API(void)
SetJitExceptionHandler(JitExceptionHandler handler);
#endif

/**
 * Get the nearest enclosing with scope object for a given function. If the
 * function is not scripted or is not enclosed by a with scope, returns the
 * global.
 */
extern JS_FRIEND_API(JSObject*)
GetNearestEnclosingWithScopeObjectForFunction(JSFunction* fun);

/**
 * Get the first SavedFrame object in this SavedFrame stack whose principals are
 * subsumed by the cx's principals. If there is no such frame, return nullptr.
 *
 * Do NOT pass a non-SavedFrame object here.
 *
 * The savedFrame and cx do not need to be in the same compartment.
 */
extern JS_FRIEND_API(JSObject*)
GetFirstSubsumedSavedFrame(JSContext* cx, JS::HandleObject savedFrame, JS::SavedFrameSelfHosted selfHosted);

extern JS_FRIEND_API(bool)
ReportIsNotFunction(JSContext* cx, JS::HandleValue v);

extern JS_FRIEND_API(JSObject*)
ConvertArgsToArray(JSContext* cx, const JS::CallArgs& args);

/**
 * Window and WindowProxy
 *
 * The functions below have to do with Windows and WindowProxies. There's an
 * invariant that actual Window objects (the global objects of web pages) are
 * never directly exposed to script. Instead we often substitute a WindowProxy.
 *
 * The scope chain, on the other hand, contains the Window and never its
 * WindowProxy.
 *
 * As a result, we have calls to these "substitute-this-object-for-that-object"
 * functions sprinkled at apparently arbitrary (but actually *very* carefully
 * and nervously selected) places throughout the engine and indeed the
 * universe.
 */

/**
 * Tell the JS engine which Class is used for WindowProxy objects. Used by the
 * functions below.
 */
extern JS_FRIEND_API(void)
SetWindowProxyClass(JSRuntime* rt, const Class* clasp);

/**
 * Associates a WindowProxy with a Window (global object). `windowProxy` must
 * have the Class set by SetWindowProxyClass.
 */
extern JS_FRIEND_API(void)
SetWindowProxy(JSContext* cx, JS::HandleObject global, JS::HandleObject windowProxy);

namespace detail {

JS_FRIEND_API(bool)
IsWindowSlow(JSObject* obj);

} // namespace detail

/**
 * Returns true iff `obj` is a global object with an associated WindowProxy,
 * see SetWindowProxy.
 */
inline bool
IsWindow(JSObject* obj)
{
    if (GetObjectClass(obj)->flags & JSCLASS_IS_GLOBAL)
        return detail::IsWindowSlow(obj);
    return false;
}

/**
 * Returns true iff `obj` has the WindowProxy Class (see SetWindowProxyClass).
 */
JS_FRIEND_API(bool)
IsWindowProxy(JSObject* obj);

/**
 * If `obj` is a Window, get its associated WindowProxy (or a CCW or dead
 * wrapper if the page was navigated away from), else return `obj`. This
 * function is infallible and never returns nullptr.
 */
extern JS_FRIEND_API(JSObject*)
ToWindowProxyIfWindow(JSObject* obj);

/**
 * If `obj` is a WindowProxy, get its associated Window (the compartment's
 * global), else return `obj`. This function is infallible and never returns
 * nullptr.
 */
extern JS_FRIEND_API(JSObject*)
ToWindowIfWindowProxy(JSObject* obj);

} /* namespace js */

extern JS_FRIEND_API(void)
JS_StoreObjectPostBarrierCallback(JSContext* cx,
                                  void (*callback)(JSTracer* trc, JSObject* key, void* data),
                                  JSObject* key, void* data);

extern JS_FRIEND_API(void)
JS_StoreStringPostBarrierCallback(JSContext* cx,
                                  void (*callback)(JSTracer* trc, JSString* key, void* data),
                                  JSString* key, void* data);

/**
 * Forcibly clear postbarrier callbacks queued by the previous two methods.
 * This should be used when the object owning the postbarriered pointers is
 * being destroyed outside of a garbage collection.
 *
 * This currently works by performing a minor GC.
 */
extern JS_FRIEND_API(void)
JS_ClearAllPostBarrierCallbacks(JSRuntime *rt);

class NativeProfiler
{
  public:
    virtual ~NativeProfiler() {};
    virtual void sampleNative(void* addr, uint32_t size) = 0;
    virtual void removeNative(void* addr) = 0;
    virtual void reset() = 0;
};

class GCHeapProfiler
{
  public:
    virtual ~GCHeapProfiler() {};
    virtual void sampleTenured(void* addr, uint32_t size) = 0;
    virtual void sampleNursery(void* addr, uint32_t size) = 0;
    virtual void markTenuredStart() = 0;
    virtual void markTenured(void* addr) = 0;
    virtual void sweepTenured() = 0;
    virtual void sweepNursery() = 0;
    virtual void moveNurseryToTenured(void* addrOld, void* addrNew) = 0;
    virtual void reset() = 0;
};

class MemProfiler
{
    static mozilla::Atomic<uint32_t, mozilla::Relaxed> sActiveProfilerCount;
    static NativeProfiler* sNativeProfiler;

    static GCHeapProfiler* GetGCHeapProfiler(void* addr);
    static GCHeapProfiler* GetGCHeapProfiler(JSRuntime* runtime);

    static NativeProfiler* GetNativeProfiler() {
        return sNativeProfiler;
    }

    GCHeapProfiler* mGCHeapProfiler;
    JSRuntime* mRuntime;

  public:
    explicit MemProfiler(JSRuntime* aRuntime) : mGCHeapProfiler(nullptr), mRuntime(aRuntime) {}

    void start(GCHeapProfiler* aGCHeapProfiler);
    void stop();

    GCHeapProfiler* getGCHeapProfiler() const {
        return mGCHeapProfiler;
    }

    static MOZ_ALWAYS_INLINE bool enabled() {
        return sActiveProfilerCount > 0;
    }

    static MemProfiler* GetMemProfiler(JSRuntime* runtime);

    static void SetNativeProfiler(NativeProfiler* aProfiler) {
        sNativeProfiler = aProfiler;
    }

    static MOZ_ALWAYS_INLINE void SampleNative(void* addr, uint32_t size) {
        JS::AutoSuppressGCAnalysis nogc;

        if (MOZ_LIKELY(!enabled()))
            return;

        NativeProfiler* profiler = GetNativeProfiler();
        if (profiler)
            profiler->sampleNative(addr, size);
    }

    static MOZ_ALWAYS_INLINE void SampleTenured(void* addr, uint32_t size) {
        JS::AutoSuppressGCAnalysis nogc;

        if (MOZ_LIKELY(!enabled()))
            return;

        GCHeapProfiler* profiler = GetGCHeapProfiler(addr);
        if (profiler)
            profiler->sampleTenured(addr, size);
    }

    static MOZ_ALWAYS_INLINE void SampleNursery(void* addr, uint32_t size) {
        JS::AutoSuppressGCAnalysis nogc;

        if (MOZ_LIKELY(!enabled()))
            return;

        GCHeapProfiler* profiler = GetGCHeapProfiler(addr);
        if (profiler)
            profiler->sampleNursery(addr, size);
    }

    static MOZ_ALWAYS_INLINE void RemoveNative(void* addr) {
        JS::AutoSuppressGCAnalysis nogc;

        if (MOZ_LIKELY(!enabled()))
            return;

        NativeProfiler* profiler = GetNativeProfiler();
        if (profiler)
            profiler->removeNative(addr);
    }

    static MOZ_ALWAYS_INLINE void MarkTenuredStart(JSRuntime* runtime) {
        JS::AutoSuppressGCAnalysis nogc;

        if (MOZ_LIKELY(!enabled()))
            return;

        GCHeapProfiler* profiler = GetGCHeapProfiler(runtime);
        if (profiler)
            profiler->markTenuredStart();
    }

    static MOZ_ALWAYS_INLINE void MarkTenured(void* addr) {
        JS::AutoSuppressGCAnalysis nogc;

        if (MOZ_LIKELY(!enabled()))
            return;

        GCHeapProfiler* profiler = GetGCHeapProfiler(addr);
        if (profiler)
            profiler->markTenured(addr);
    }

    static MOZ_ALWAYS_INLINE void SweepTenured(JSRuntime* runtime) {
        JS::AutoSuppressGCAnalysis nogc;

        if (MOZ_LIKELY(!enabled()))
            return;

        GCHeapProfiler* profiler = GetGCHeapProfiler(runtime);
        if (profiler)
            profiler->sweepTenured();
    }

    static MOZ_ALWAYS_INLINE void SweepNursery(JSRuntime* runtime) {
        JS::AutoSuppressGCAnalysis nogc;

        if (MOZ_LIKELY(!enabled()))
            return;

        GCHeapProfiler* profiler = GetGCHeapProfiler(runtime);
        if (profiler)
            profiler->sweepNursery();
    }

    static MOZ_ALWAYS_INLINE void MoveNurseryToTenured(void* addrOld, void* addrNew) {
        JS::AutoSuppressGCAnalysis nogc;

        if (MOZ_LIKELY(!enabled()))
            return;

        GCHeapProfiler* profiler = GetGCHeapProfiler(addrOld);
        if (profiler)
            profiler->moveNurseryToTenured(addrOld, addrNew);
    }
};

#endif /* jsfriendapi_h */
