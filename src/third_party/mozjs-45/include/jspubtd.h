/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jspubtd_h
#define jspubtd_h

/*
 * JS public API typedefs.
 */

#include "mozilla/Assertions.h"
#include "mozilla/LinkedList.h"
#include "mozilla/PodOperations.h"

#include "jsprototypes.h"
#include "jstypes.h"

#include "js/TypeDecls.h"

#if defined(JS_GC_ZEAL) || defined(DEBUG)
# define JSGC_HASH_TABLE_CHECKS
#endif

namespace JS {

template <typename T>
class AutoVectorRooter;
typedef AutoVectorRooter<jsid> AutoIdVector;
class CallArgs;

template <typename T>
class Rooted;

class JS_FRIEND_API(CompileOptions);
class JS_FRIEND_API(ReadOnlyCompileOptions);
class JS_FRIEND_API(OwningCompileOptions);
class JS_FRIEND_API(TransitiveCompileOptions);
class JS_PUBLIC_API(CompartmentOptions);

class Value;
struct Zone;

} /* namespace JS */

namespace js {
struct ContextFriendFields;
class RootLists;
} // namespace js

/*
 * Run-time version enumeration.  For compile-time version checking, please use
 * the JS_HAS_* macros in jsversion.h, or use MOZJS_MAJOR_VERSION,
 * MOZJS_MINOR_VERSION, MOZJS_PATCH_VERSION, and MOZJS_ALPHA definitions.
 */
enum JSVersion {
    JSVERSION_ECMA_3  = 148,
    JSVERSION_1_6     = 160,
    JSVERSION_1_7     = 170,
    JSVERSION_1_8     = 180,
    JSVERSION_ECMA_5  = 185,
    JSVERSION_DEFAULT = 0,
    JSVERSION_UNKNOWN = -1,
    JSVERSION_LATEST  = JSVERSION_ECMA_5
};

/* Result of typeof operator enumeration. */
enum JSType {
    JSTYPE_VOID,                /* undefined */
    JSTYPE_OBJECT,              /* object */
    JSTYPE_FUNCTION,            /* function */
    JSTYPE_STRING,              /* string */
    JSTYPE_NUMBER,              /* number */
    JSTYPE_BOOLEAN,             /* boolean */
    JSTYPE_NULL,                /* null */
    JSTYPE_SYMBOL,              /* symbol */
    JSTYPE_LIMIT
};

/* Dense index into cached prototypes and class atoms for standard objects. */
enum JSProtoKey {
#define PROTOKEY_AND_INITIALIZER(name,code,init,clasp) JSProto_##name = code,
    JS_FOR_EACH_PROTOTYPE(PROTOKEY_AND_INITIALIZER)
#undef PROTOKEY_AND_INITIALIZER
    JSProto_LIMIT
};

/* Struct forward declarations. */
struct JSClass;
struct JSCompartment;
struct JSCrossCompartmentCall;
class JSErrorReport;
struct JSExceptionState;
struct JSFunctionSpec;
struct JSLocaleCallbacks;
struct JSObjectMap;
struct JSPrincipals;
struct JSPropertyDescriptor;
struct JSPropertyName;
struct JSPropertySpec;
struct JSRuntime;
struct JSSecurityCallbacks;
struct JSStructuredCloneCallbacks;
struct JSStructuredCloneReader;
struct JSStructuredCloneWriter;
class JS_PUBLIC_API(JSTracer);

class JSFlatString;

typedef struct PRCallOnceType   JSCallOnceType;
typedef bool                    (*JSInitCallback)(void);

template<typename T> struct JSConstScalarSpec;
typedef JSConstScalarSpec<double> JSConstDoubleSpec;
typedef JSConstScalarSpec<int32_t> JSConstIntegerSpec;

/*
 * Generic trace operation that calls JS_CallTracer on each traceable thing
 * stored in data.
 */
typedef void
(* JSTraceDataOp)(JSTracer* trc, void* data);

namespace js {

void FinishGC(JSRuntime* rt);

namespace gc {
class AutoTraceSession;
class StoreBuffer;
void MarkPersistentRootedChains(JSTracer*);
void MarkPersistentRootedChainsInLists(js::RootLists&, JSTracer*);
void FinishPersistentRootedChains(js::RootLists&);
} // namespace gc
} // namespace js

namespace JS {

typedef void (*OffThreadCompileCallback)(void* token, void* callbackData);

enum class HeapState {
    Idle,             // doing nothing with the GC heap
    Tracing,          // tracing the GC heap without collecting, e.g. IterateCompartments()
    MajorCollecting,  // doing a GC of the major heap
    MinorCollecting   // doing a GC of the minor heap (nursery)
};

namespace shadow {

struct Runtime
{
  protected:
    // Allow inlining of heapState checks.
    friend class js::gc::AutoTraceSession;
    JS::HeapState heapState_;

    js::gc::StoreBuffer* gcStoreBufferPtr_;

  public:
    Runtime()
      : heapState_(JS::HeapState::Idle)
      , gcStoreBufferPtr_(nullptr)
    {}

    bool isHeapBusy() const { return heapState_ != JS::HeapState::Idle; }
    bool isHeapMajorCollecting() const { return heapState_ == JS::HeapState::MajorCollecting; }
    bool isHeapMinorCollecting() const { return heapState_ == JS::HeapState::MinorCollecting; }
    bool isHeapCollecting() const { return isHeapMinorCollecting() || isHeapMajorCollecting(); }

    js::gc::StoreBuffer* gcStoreBufferPtr() { return gcStoreBufferPtr_; }

    static JS::shadow::Runtime* asShadowRuntime(JSRuntime* rt) {
        return reinterpret_cast<JS::shadow::Runtime*>(rt);
    }

  protected:
    void setGCStoreBufferPtr(js::gc::StoreBuffer* storeBuffer) {
        gcStoreBufferPtr_ = storeBuffer;
    }
};

} /* namespace shadow */

class JS_PUBLIC_API(AutoGCRooter)
{
  public:
    AutoGCRooter(JSContext* cx, ptrdiff_t tag);
    AutoGCRooter(js::ContextFriendFields* cx, ptrdiff_t tag);

    ~AutoGCRooter() {
        MOZ_ASSERT(this == *stackTop);
        *stackTop = down;
    }

    /* Implemented in gc/RootMarking.cpp. */
    inline void trace(JSTracer* trc);
    static void traceAll(JSTracer* trc);
    static void traceAllWrappers(JSTracer* trc);

    /* T must be a context type */
    template<typename T>
    static void traceAllInContext(T* cx, JSTracer* trc) {
        for (AutoGCRooter* gcr = cx->roots.autoGCRooters_; gcr; gcr = gcr->down)
            gcr->trace(trc);
    }

  protected:
    AutoGCRooter * const down;

    /*
     * Discriminates actual subclass of this being used.  If non-negative, the
     * subclass roots an array of values of the length stored in this field.
     * If negative, meaning is indicated by the corresponding value in the enum
     * below.  Any other negative value indicates some deeper problem such as
     * memory corruption.
     */
    ptrdiff_t tag_;

    enum {
        VALARRAY =     -2, /* js::AutoValueArray */
        PARSER =       -3, /* js::frontend::Parser */
        VALVECTOR =   -10, /* js::AutoValueVector */
        IDVECTOR =    -11, /* js::AutoIdVector */
        OBJVECTOR =   -14, /* js::AutoObjectVector */
        IONMASM =     -19, /* js::jit::MacroAssembler */
        WRAPVECTOR =  -20, /* js::AutoWrapperVector */
        WRAPPER =     -21, /* js::AutoWrapperRooter */
        CUSTOM =      -26  /* js::CustomAutoRooter */
    };

    static ptrdiff_t GetTag(const Value& value) { return VALVECTOR; }
    static ptrdiff_t GetTag(const jsid& id) { return IDVECTOR; }
    static ptrdiff_t GetTag(JSObject* obj) { return OBJVECTOR; }

  private:
    AutoGCRooter ** const stackTop;

    /* No copy or assignment semantics. */
    AutoGCRooter(AutoGCRooter& ida) = delete;
    void operator=(AutoGCRooter& ida) = delete;
};

} /* namespace JS */

namespace js {

class ExclusiveContext;

/*
 * This list enumerates the different types of conceptual stacks we have in
 * SpiderMonkey. In reality, they all share the C stack, but we allow different
 * stack limits depending on the type of code running.
 */
enum StackKind
{
    StackForSystemCode,      // C++, such as the GC, running on behalf of the VM.
    StackForTrustedScript,   // Script running with trusted principals.
    StackForUntrustedScript, // Script running with untrusted principals.
    StackKindCount
};

enum ThingRootKind
{
    THING_ROOT_OBJECT,
    THING_ROOT_SHAPE,
    THING_ROOT_BASE_SHAPE,
    THING_ROOT_OBJECT_GROUP,
    THING_ROOT_STRING,
    THING_ROOT_SYMBOL,
    THING_ROOT_JIT_CODE,
    THING_ROOT_SCRIPT,
    THING_ROOT_LAZY_SCRIPT,
    THING_ROOT_ID,
    THING_ROOT_VALUE,
    THING_ROOT_TRACEABLE,
    THING_ROOT_LIMIT
};

template <typename T>
struct RootKind;

/*
 * Specifically mark the ThingRootKind of externally visible types, so that
 * JSAPI users may use JSRooted... types without having the class definition
 * available.
 */
template<typename T, ThingRootKind Kind>
struct SpecificRootKind
{
    static ThingRootKind rootKind() { return Kind; }
};

template <> struct RootKind<JSObject*> : SpecificRootKind<JSObject*, THING_ROOT_OBJECT> {};
template <> struct RootKind<JSFlatString*> : SpecificRootKind<JSFlatString*, THING_ROOT_STRING> {};
template <> struct RootKind<JSFunction*> : SpecificRootKind<JSFunction*, THING_ROOT_OBJECT> {};
template <> struct RootKind<JSString*> : SpecificRootKind<JSString*, THING_ROOT_STRING> {};
template <> struct RootKind<JS::Symbol*> : SpecificRootKind<JS::Symbol*, THING_ROOT_SYMBOL> {};
template <> struct RootKind<JSScript*> : SpecificRootKind<JSScript*, THING_ROOT_SCRIPT> {};
template <> struct RootKind<jsid> : SpecificRootKind<jsid, THING_ROOT_ID> {};
template <> struct RootKind<JS::Value> : SpecificRootKind<JS::Value, THING_ROOT_VALUE> {};

// Abstracts JS rooting mechanisms so they can be shared between the JSContext
// and JSRuntime.
class RootLists
{
    // Stack GC roots for stack-allocated GC heap pointers.
    JS::Rooted<void*>* stackRoots_[THING_ROOT_LIMIT];
    template <typename T> friend class JS::Rooted;

    // Stack GC roots for stack-allocated AutoFooRooter classes.
    JS::AutoGCRooter* autoGCRooters_;
    friend class JS::AutoGCRooter;

  public:
    RootLists() : autoGCRooters_(nullptr) {
        mozilla::PodArrayZero(stackRoots_);
    }

    template <class T>
    inline JS::Rooted<T>* gcRooters() {
        js::ThingRootKind kind = RootKind<T>::rootKind();
        return reinterpret_cast<JS::Rooted<T>*>(stackRoots_[kind]);
    }

    void checkNoGCRooters();

    /* Allow inlining of PersistentRooted constructors and destructors. */
  private:
    template <typename Referent> friend class JS::PersistentRooted;
    friend void js::gc::MarkPersistentRootedChains(JSTracer*);
    friend void js::gc::MarkPersistentRootedChainsInLists(RootLists&, JSTracer*);
    friend void js::gc::FinishPersistentRootedChains(RootLists&);

    mozilla::LinkedList<JS::PersistentRooted<void*>> heapRoots_[THING_ROOT_LIMIT];

    /* Specializations of this return references to the appropriate list. */
    template<typename Referent>
    inline mozilla::LinkedList<JS::PersistentRooted<Referent>>& getPersistentRootedList();
};

template<>
inline mozilla::LinkedList<JS::PersistentRootedFunction>&
RootLists::getPersistentRootedList<JSFunction*>() {
    return reinterpret_cast<mozilla::LinkedList<JS::PersistentRooted<JSFunction*>>&>(
        heapRoots_[THING_ROOT_OBJECT]);
}

template<>
inline mozilla::LinkedList<JS::PersistentRootedObject>&
RootLists::getPersistentRootedList<JSObject*>() {
    return reinterpret_cast<mozilla::LinkedList<JS::PersistentRooted<JSObject*>>&>(
        heapRoots_[THING_ROOT_OBJECT]);
}

template<>
inline mozilla::LinkedList<JS::PersistentRootedId>&
RootLists::getPersistentRootedList<jsid>() {
    return reinterpret_cast<mozilla::LinkedList<JS::PersistentRooted<jsid>>&>(
        heapRoots_[THING_ROOT_ID]);
}

template<>
inline mozilla::LinkedList<JS::PersistentRootedScript>&
RootLists::getPersistentRootedList<JSScript*>() {
    return reinterpret_cast<mozilla::LinkedList<JS::PersistentRooted<JSScript*>>&>(
        heapRoots_[THING_ROOT_SCRIPT]);
}

template<>
inline mozilla::LinkedList<JS::PersistentRootedString>&
RootLists::getPersistentRootedList<JSString*>() {
    return reinterpret_cast<mozilla::LinkedList<JS::PersistentRooted<JSString*>>&>(
        heapRoots_[THING_ROOT_STRING]);
}

template<>
inline mozilla::LinkedList<JS::PersistentRootedValue>&
RootLists::getPersistentRootedList<JS::Value>() {
    return reinterpret_cast<mozilla::LinkedList<JS::PersistentRooted<JS::Value>>&>(
        heapRoots_[THING_ROOT_VALUE]);
}

struct ContextFriendFields
{
  protected:
    JSRuntime* const     runtime_;

    /* The current compartment. */
    JSCompartment*      compartment_;

    /* The current zone. */
    JS::Zone*           zone_;

  public:
    /* Rooting structures. */
    RootLists           roots;

    explicit ContextFriendFields(JSRuntime* rt)
      : runtime_(rt), compartment_(nullptr), zone_(nullptr)
    {}

    static const ContextFriendFields* get(const JSContext* cx) {
        return reinterpret_cast<const ContextFriendFields*>(cx);
    }

    static ContextFriendFields* get(JSContext* cx) {
        return reinterpret_cast<ContextFriendFields*>(cx);
    }

    friend JSRuntime* GetRuntime(const JSContext* cx);
    friend JSCompartment* GetContextCompartment(const JSContext* cx);
    friend JS::Zone* GetContextZone(const JSContext* cx);
    template <typename T> friend class JS::Rooted;
};

/*
 * Inlinable accessors for JSContext.
 *
 * - These must not be available on the more restricted superclasses of
 *   JSContext, so we can't simply define them on ContextFriendFields.
 *
 * - They're perfectly ordinary JSContext functionality, so ought to be
 *   usable without resorting to jsfriendapi.h, and when JSContext is an
 *   incomplete type.
 */
inline JSRuntime*
GetRuntime(const JSContext* cx)
{
    return ContextFriendFields::get(cx)->runtime_;
}

inline JSCompartment*
GetContextCompartment(const JSContext* cx)
{
    return ContextFriendFields::get(cx)->compartment_;
}

inline JS::Zone*
GetContextZone(const JSContext* cx)
{
    return ContextFriendFields::get(cx)->zone_;
}

class PerThreadData;

struct PerThreadDataFriendFields
{
  private:
    // Note: this type only exists to permit us to derive the offset of
    // the perThread data within the real JSRuntime* type in a portable
    // way.
    struct RuntimeDummy : JS::shadow::Runtime
    {
        struct PerThreadDummy {
            void* field1;
            uintptr_t field2;
#ifdef JS_DEBUG
            uint64_t field3;
#endif
        } mainThread;
    };

  public:
    /* Rooting structures. */
    RootLists roots;

    PerThreadDataFriendFields();

    /* Limit pointer for checking native stack consumption. */
    uintptr_t nativeStackLimit[js::StackKindCount];

    static const size_t RuntimeMainThreadOffset = offsetof(RuntimeDummy, mainThread);

    static inline PerThreadDataFriendFields* get(js::PerThreadData* pt) {
        return reinterpret_cast<PerThreadDataFriendFields*>(pt);
    }

    static inline PerThreadDataFriendFields* getMainThread(JSRuntime* rt) {
        // mainThread must always appear directly after |JS::shadow::Runtime|.
        // Tested by a JS_STATIC_ASSERT in |jsfriendapi.cpp|
        return reinterpret_cast<PerThreadDataFriendFields*>(
            reinterpret_cast<char*>(rt) + RuntimeMainThreadOffset);
    }

    static inline const PerThreadDataFriendFields* getMainThread(const JSRuntime* rt) {
        // mainThread must always appear directly after |JS::shadow::Runtime|.
        // Tested by a JS_STATIC_ASSERT in |jsfriendapi.cpp|
        return reinterpret_cast<const PerThreadDataFriendFields*>(
            reinterpret_cast<const char*>(rt) + RuntimeMainThreadOffset);
    }

    template <typename T> friend class JS::Rooted;
};

} /* namespace js */

#endif /* jspubtd_h */
